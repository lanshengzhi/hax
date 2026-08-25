/* SPDX-License-Identifier: MIT */
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>

#include "config.h"
#include "json_value.h"
#include "provider.h"
#include "tool.h"
#include "util.h"
#include "tools/bash_cd_strip.h"
#include "tools/bash_classify.h"
#include "tools/bash_process.h"
#include "tools/task_registry.h"

/* The model cannot observe the configured ceiling, so clamp rather than requiring a retry. */
static char *resolve_timeout_ms(const hax::json::value *arguments, long *timeout_ms_out)
{
    const hax::json::value *value = arguments->find("timeout_seconds");
    if (!value) {
        *timeout_ms_out = config_duration_ms("bash.timeout");
        return NULL;
    }
    if (!value->is_integer())
        return xstrdup("'timeout_seconds' must be an integer");

    long seconds = (long)value->integer_value();
    if (seconds < 1)
        return xstrdup("'timeout_seconds' must be >= 1");

    long timeout_ms = seconds > LONG_MAX / 1000L ? LONG_MAX : seconds * 1000L;
    long maximum_ms = config_duration_ms("bash.timeout_max");
    if (maximum_ms > 0 && timeout_ms > maximum_ms)
        timeout_ms = maximum_ms;
    *timeout_ms_out = timeout_ms;
    return NULL;
}

static char *run_bash(const char *args_json, struct tool_run_ctx *ctx)
{
    auto arguments = hax::json::parse_value(args_json ? args_json : "{}",
                                            {.source = "bash arguments", .max_input_bytes = 0});
    if (!arguments)
        return xasprintf("invalid arguments: %s",
                         hax::json::format_error(arguments.error()).c_str());

    const hax::json::value *command_value = arguments->find("command");
    const char *command =
        command_value && command_value->is_string() ? command_value->string_value().c_str() : NULL;
    if (!command || !*command)
        return xstrdup("missing 'command' argument");

    const hax::json::value *background_value = arguments->find("background");
    if (background_value && !background_value->is_boolean())
        return xstrdup("'background' must be a boolean");
    int background = background_value && background_value->boolean_value();
    if (background && config_bool("no_tasks"))
        return xstrdup("background tasks are disabled; run the command synchronously");

    /* Validate before the command runs, so a bad name is a side-effect-free retry. With tasks
     * disabled the name is inert and ignored. Models routinely send every declared field, so an
     * empty name means unnamed rather than forcing a retry. */
    const char *name = NULL;
    const hax::json::value *name_value = arguments->find("name");
    if (name_value && !name_value->is_null() && !config_bool("no_tasks")) {
        if (!name_value->is_string())
            return xstrdup("'name' must be a string");
        name = name_value->string_value().c_str();
        if (!*name) {
            name = NULL;
        } else {
            char *name_error = task_name_error(name);
            if (name_error)
                return name_error;
        }
    }

    /* Refuse before the command runs, like a bad name, so the model can kill or wait first. */
    int max_running = config_int("task.max_running");
    if (background && task_running_count() >= (size_t)max_running)
        return xasprintf("too many running tasks (max %d): wait on or kill one first", max_running);

    long timeout_ms = 0;
    char *error = resolve_timeout_ms(&*arguments, &timeout_ms);
    if (error)
        return error;

    return bash_run_command(command, timeout_ms, background, name, ctx);
}

/* Return rewritten arguments only when the leading cd is proven to be a filesystem no-op. */
static char *preprocess_args(const char *args_json)
{
    if (!args_json)
        return NULL;
    auto arguments =
        hax::json::parse_value(args_json, {.source = "bash arguments", .max_input_bytes = 0});
    if (!arguments)
        return NULL;
    const hax::json::value *command_value = arguments->find("command");
    if (!command_value || !command_value->is_string())
        return NULL;
    const char *command = command_value->string_value().c_str();
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd)))
        return NULL;
    size_t command_offset = bash_strip_cd_prefix(command, cwd, getenv("HOME"));
    if (command_offset == 0)
        return NULL;
    arguments->set("command", command + command_offset);
    auto encoded =
        hax::json::serialize_value(*arguments, {.source = "bash arguments", .max_input_bytes = 0});
    return encoded ? xstrdup(encoded->c_str()) : NULL;
}

static enum tool_preview_mode select_preview(const char *args_json)
{
    if (!args_json)
        return TOOL_PREVIEW_HEAD_TAIL;
    auto arguments =
        hax::json::parse_value(args_json, {.source = "bash arguments", .max_input_bytes = 0});
    if (!arguments)
        return TOOL_PREVIEW_HEAD_TAIL;
    const hax::json::value *command_value = arguments->find("command");
    const char *command =
        command_value && command_value->is_string() ? command_value->string_value().c_str() : NULL;
    return command && bash_command_is_exploration(command) ? TOOL_PREVIEW_COLLAPSED
                                                           : TOOL_PREVIEW_HEAD_TAIL;
}

static const char BASH_DESCRIPTION[] =
    "Run a shell command via bash -c (POSIX sh -c where bash is unavailable). Returns combined "
    "stdout+stderr plus exit code.\n"
    "\n"
    "Rules:\n"
    "- Each call starts in the working directory listed under `# Environment`; `cd` does not "
    "persist across calls.\n"
    "- Follow the command preferences under `# Environment` when present.\n"
    "- Usually omit `timeout_seconds`: a command that outlives the default timeout (120s) is "
    "not killed — it detaches into a background task and you will be notified when it "
    "finishes.\n"
    "- Set `background` for commands meant to run alongside other work (servers, watchers, "
    "long builds, subagents): the call returns after a brief initial-output window and the "
    "command continues as a task. No trailing `&`: the task tracks the shell, and processes "
    "orphaned by an exited shell are killed.";

static const struct tool_param BASH_PARAMS[] = {
    {.name = "command", .type = "string", .description = "Shell command to run.", .required = 1},
    {.name = "timeout_seconds",
     .type = "integer",
     .description = "Optional override of the default timeout; rarely needed — on expiry the "
                    "command detaches into a background task instead of dying. The harness "
                    "clamps to a configured maximum.",
     .minimum = 1},
    {.name = "background",
     .type = "boolean",
     .description = "Run as a background task: return after a brief initial-output window while "
                    "the command keeps running; `timeout_seconds` is ignored. A command that "
                    "finishes within the window returns synchronously and creates no task."},
    {.name = "name",
     .type = "string",
     .description = "Optional short task name used instead of the automatic id if the command "
                    "detaches (letters/digits/-/_, max 32 chars, e.g. \"tests\")."},
};

static const char BASH_DESCRIPTION_NO_TASKS[] =
    "Run a shell command via bash -c (POSIX sh -c where bash is unavailable). Returns combined "
    "stdout+stderr plus exit code.\n"
    "\n"
    "Rules:\n"
    "- Each call starts in the working directory listed under `# Environment`; `cd` does not "
    "persist across calls.\n"
    "- Follow the command preferences under `# Environment` when present.\n"
    "- Default timeout is 120s; pass `timeout_seconds` for slow commands (test suites, builds). "
    "The harness enforces a hard ceiling.";

static const struct tool_param BASH_PARAMS_NO_TASKS[] = {
    {.name = "command", .type = "string", .description = "Shell command to run.", .required = 1},
    {.name = "timeout_seconds",
     .type = "integer",
     .description = "Optional override of the default timeout. Use a higher value for slow builds "
                    "or test suites; the harness clamps to a configured maximum.",
     .minimum = 1},
};

static const struct tool_def BASH_DEF_NO_TASKS = {
    .name = "bash",
    .description = BASH_DESCRIPTION_NO_TASKS,
    .params = BASH_PARAMS_NO_TASKS,
    .n_params = sizeof(BASH_PARAMS_NO_TASKS) / sizeof(BASH_PARAMS_NO_TASKS[0]),
};

static const struct tool_def *bash_advertise(void)
{
    return config_bool("no_tasks") ? &BASH_DEF_NO_TASKS : &TOOL_BASH.def;
}

const struct tool TOOL_BASH = {
    .def = {.name = "bash",
            .description = BASH_DESCRIPTION,
            .params = BASH_PARAMS,
            .n_params = sizeof(BASH_PARAMS) / sizeof(BASH_PARAMS[0])},
    .run = run_bash,
    .preprocess_args = preprocess_args,
    .advertise = bash_advertise,
    .display = {.arg_name = "command",
                .preview_mode = TOOL_PREVIEW_HEAD_TAIL,
                .header_rows = 3,
                .select_preview = select_preview},
};
