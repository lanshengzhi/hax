/* SPDX-License-Identifier: MIT */
#include <limits.h>
#include <stdio.h>

#include "config.h"
#include "json_value.h"
#include "provider.h"
#include "tool.h"
#include "util.h"
#include "tools/task_registry.h"

static char *run_task_wait(const char *args_json, struct tool_run_ctx *ctx)
{
    if (config_bool("no_tasks"))
        return xstrdup("background tasks are disabled");

    auto arguments = hax::json::parse_value(
        args_json ? args_json : "{}", {.source = "task_wait arguments", .max_input_bytes = 0});
    if (!arguments)
        return xasprintf("invalid arguments: %s",
                         hax::json::format_error(arguments.error()).c_str());

    const hax::json::value *id_value = arguments->find("id");
    const char *id = id_value && id_value->is_string() ? id_value->string_value().c_str() : NULL;
    if (!id || !*id)
        return xstrdup("missing 'id': name the task to wait on, e.g. \"t1\"");

    const hax::json::value *kill_value = arguments->find("kill");
    if (kill_value && !kill_value->is_boolean())
        return xstrdup("'kill' must be a boolean");
    int kill_on_timeout = kill_value && kill_value->boolean_value();

    /* A kill with no timeout is immediate; a plain wait falls back to the configured window. */
    long timeout_ms = kill_on_timeout ? 0 : config_duration_ms("task.wait_timeout");
    const hax::json::value *timeout_value = arguments->find("timeout_seconds");
    if (timeout_value) {
        if (!timeout_value->is_integer() || timeout_value->integer_value() < 0)
            return xstrdup("'timeout_seconds' must be an integer >= 0");
        long seconds = (long)timeout_value->integer_value();
        timeout_ms = seconds > LONG_MAX / 1000L ? LONG_MAX : seconds * 1000L;
    }

    return task_wait_stream(id, timeout_ms, kill_on_timeout, ctx ? ctx->display : NULL,
                            ctx ? ctx->display_data : NULL);
}

static const char TASK_WAIT_DESCRIPTION[] =
    "Wait on one background task; returns the output it produced since you last saw it plus "
    "its status.\n"
    "\n"
    "Returns immediately for an already-finished task (this is also how you collect a task "
    "announced as finished), and returns early when a different task finishes so you can react "
    "to it. Wait on the task whose result you need next; do not poll in a loop of short waits.\n"
    "\n"
    "With `kill`, the task is stopped (SIGTERM, then SIGKILL after a grace period) and its "
    "final output is returned; add `timeout_seconds` to first give the task that long to "
    "finish on its own.";

static const struct tool_param TASK_WAIT_PARAMS[] = {
    {.name = "id",
     .type = "string",
     .description = "Task id to wait on (e.g. \"t1\").",
     .required = 1},
    {.name = "timeout_seconds",
     .type = "integer",
     .description = "How long to block waiting for the task to finish; 0 does not block. "
                    "Defaults to a configured value (10 minutes unless changed); with `kill` "
                    "it defaults to 0 (kill immediately)."},
    {.name = "kill",
     .type = "boolean",
     .description = "Kill the task and report its final output. With `timeout_seconds`, the "
                    "task first gets that window to finish on its own; the kill fires only if "
                    "it is still running when the timeout elapses."},
};

static const struct tool_def *task_wait_advertise(void)
{
    return config_bool("no_tasks") ? NULL : &TOOL_TASK_WAIT.def;
}

/* "t1", "t1 (up to 30s)", "t1 (kill)", "t1 (up to 30s, then kill)" — malformed arguments fall
 * back to raw JSON. */
static char *format_wait_argument(const char *args_json)
{
    auto arguments =
        hax::json::parse_value(args_json, {.source = "task_wait arguments", .max_input_bytes = 0});
    if (!arguments)
        return NULL;
    const hax::json::value *id_value = arguments->find("id");
    const char *id = id_value && id_value->is_string() ? id_value->string_value().c_str() : NULL;
    if (!id || !*id)
        return NULL;
    struct buf out;
    buf_init(&out);
    buf_append_str(&out, id);
    const hax::json::value *kill_value = arguments->find("kill");
    int kill = kill_value && kill_value->is_boolean() && kill_value->boolean_value();
    const hax::json::value *timeout = arguments->find("timeout_seconds");
    if (timeout && timeout->is_integer() && timeout->integer_value() > 0) {
        long seconds = (long)timeout->integer_value();
        char duration[32];
        format_duration(duration, sizeof(duration),
                        seconds > LONG_MAX / 1000L ? LONG_MAX : seconds * 1000L);
        char suffix[64];
        if (kill)
            snprintf(suffix, sizeof(suffix), " (up to %s, then kill)", duration);
        else
            snprintf(suffix, sizeof(suffix), " (up to %s)", duration);
        buf_append_str(&out, suffix);
    } else if (kill) {
        buf_append_str(&out, " (kill)");
    }
    return buf_steal(&out);
}

const struct tool TOOL_TASK_WAIT = {
    .def = {.name = "task_wait",
            .description = TASK_WAIT_DESCRIPTION,
            .params = TASK_WAIT_PARAMS,
            .n_params = sizeof(TASK_WAIT_PARAMS) / sizeof(TASK_WAIT_PARAMS[0])},
    .run = run_task_wait,
    .advertise = task_wait_advertise,
    .display = {.format_argument = format_wait_argument, .preview_mode = TOOL_PREVIEW_HEAD_TAIL},
};
