/* SPDX-License-Identifier: MIT */
#include "providers/anthropic_body.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "provider.h"
#include "tool_schema.h"
#include "providers/anthropic_json.h"
#include "providers/wire.h"

const char *const ANTHROPIC_EFFORT_LADDER[] = {"low", "medium", "high", "xhigh", "max"};
const size_t ANTHROPIC_EFFORT_LADDER_N =
    sizeof(ANTHROPIC_EFFORT_LADDER) / sizeof(ANTHROPIC_EFFORT_LADDER[0]);

json_t *anthropic_build_messages(const struct item *items, size_t n_items,
                                 const char *current_provider, const char *current_model,
                                 int allow_empty_signature, int image_input)
{
    json_t *messages = json_array();
    json_t *decoded = NULL;
    json_t *result = messages;
    auto encoded = hax::anthropic_json::build_messages(
        items, n_items, current_provider, current_model, allow_empty_signature, image_input);
    if (!encoded)
        goto out;

    decoded = json_loads(encoded->c_str(), 0, NULL);
    if (!json_is_array(decoded))
        goto out;

    json_decref(messages);
    result = decoded;
    decoded = NULL;

out:
    json_decref(decoded);
    return result;
}

static json_t *build_cache_control(const char *ttl)
{
    json_t *cache_control = json_pack("{s:s}", "type", "ephemeral");
    if (ttl && strcasecmp(ttl, "1h") == 0)
        json_object_set_new(cache_control, "ttl", json_string("1h"));
    return cache_control;
}

static json_t *build_tools(const struct tool_def *tools, size_t n_tools, int cache_last,
                           const char *ttl)
{
    json_t *tool_list = json_array();
    for (size_t i = 0; i < n_tools; i++) {
        json_t *schema = tool_schema_build(&tools[i]);
        json_t *tool = json_pack("{s:s, s:s, s:o}", "name", tools[i].name, "description",
                                 tools[i].description, "input_schema", schema);
        if (cache_last && i == n_tools - 1)
            json_object_set_new(tool, "cache_control", build_cache_control(ttl));
        json_array_append_new(tool_list, tool);
    }
    return tool_list;
}

static void attach_cache_to_last_message(json_t *messages, const char *ttl)
{
    size_t n_messages = json_array_size(messages);
    if (n_messages == 0)
        return;

    json_t *message = json_array_get(messages, n_messages - 1);
    json_t *content = json_object_get(message, "content");
    if (!json_is_array(content) || json_array_size(content) == 0)
        return;

    json_t *block = json_array_get(content, json_array_size(content) - 1);
    const char *type = json_string_value(json_object_get(block, "type"));
    /* Anthropic rejects cache_control on thinking blocks. */
    if (type && (strcmp(type, "thinking") == 0 || strcmp(type, "redacted_thinking") == 0))
        return;
    json_object_set_new(block, "cache_control", build_cache_control(ttl));
}

static void apply_thinking(json_t *body, const struct context *context,
                           const struct wire_body_opts *opts)
{
    if (opts->thinking_mode == ANTHROPIC_THINKING_OFF)
        return;

    if (opts->thinking_mode == ANTHROPIC_THINKING_ADAPTIVE) {
        const char *display = opts->show_reasoning ? "summarized" : "omitted";
        json_object_set_new(body, "thinking",
                            json_pack("{s:s, s:s}", "type", "adaptive", "display", display));
        if (context->effort && *context->effort) {
            json_object_set_new(body, "output_config",
                                json_pack("{s:s}", "effort", context->effort));
        }
        return;
    }

    /* Anthropic requires 1 <= budget_tokens < max_tokens. */
    if (opts->max_tokens < 2)
        return;
    int budget_tokens = opts->thinking_budget;
    if (budget_tokens <= 0 || budget_tokens >= opts->max_tokens)
        budget_tokens = opts->max_tokens - 1;
    json_object_set_new(body, "thinking",
                        json_pack("{s:s, s:i}", "type", "enabled", "budget_tokens", budget_tokens));
}

json_t *anthropic_build_body(const struct context *context, const char *provider_id,
                             const char *model, const struct wire_body_opts *opts)
{
    json_t *messages =
        anthropic_build_messages(context->items, context->n_items, provider_id, model,
                                 opts->allow_empty_signature, context->image_input);
    json_t *body = json_pack("{s:s, s:i, s:b, s:o}", "model", model, "max_tokens", opts->max_tokens,
                             "stream", 1, "messages", messages);

    if (context->system_prompt && *context->system_prompt) {
        json_t *system_block =
            json_pack("{s:s, s:s}", "type", "text", "text", context->system_prompt);
        if (opts->cache_markers) {
            json_object_set_new(system_block, "cache_control",
                                build_cache_control(opts->cache_ttl));
        }
        json_t *system = json_array();
        json_array_append_new(system, system_block);
        json_object_set_new(body, "system", system);
    }

    if (context->n_tools > 0) {
        json_object_set_new(
            body, "tools",
            build_tools(context->tools, context->n_tools, opts->cache_markers, opts->cache_ttl));
    }
    if (opts->cache_markers)
        attach_cache_to_last_message(messages, opts->cache_ttl);

    apply_thinking(body, context, opts);
    return body;
}
