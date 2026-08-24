/* SPDX-License-Identifier: MIT */
#include "providers/chat_body.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "catalog.h"
#include "provider.h"
#include "tool_schema.h"
#include "util.h"
#include "providers/openai_compat_json.h"
#include "providers/wire.h"

/* AUTO sends explicit cache markers only when writes replace ordinary input processing. */
struct chat_cache_plan chat_plan_cache(const struct catalog_entry *rates, enum chat_cache_mode mode,
                                       const char *ttl)
{
    struct chat_cache_plan plan = {0};
    plan.send_breakpoints = mode == CHAT_CACHE_ON ||
                            (mode == CHAT_CACHE_AUTO && catalog_cache_write_replaces_input(rates));
    plan.writes_bill_1h = plan.send_breakpoints && ttl && strcasecmp(ttl, "1h") == 0 &&
                          rates->cost_cache_write_1h >= 0;
    return plan;
}

json_t *chat_build_messages(const char *system_prompt, const struct item *items, size_t n_items,
                            const char *reasoning_field, const char *current_provider,
                            const char *current_model, int image_input)
{
    json_t *messages = json_array();
    json_t *decoded = NULL;
    json_t *result = messages;
    auto encoded =
        hax::openai_compat_json::build_chat_messages(system_prompt, items, n_items, reasoning_field,
                                                     current_provider, current_model, image_input);
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

static int attach_cache_control(json_t *message, const char *ttl)
{
    json_t *content = json_object_get(message, "content");
    if (json_is_string(content)) {
        json_t *part = json_pack("{s:s, s:O}", "type", "text", "text", content);
        json_object_set_new(part, "cache_control", build_cache_control(ttl));
        json_t *parts = json_array();
        json_array_append_new(parts, part);
        json_object_set_new(message, "content", parts);
        return 1;
    }
    if (!json_is_array(content) || json_array_size(content) == 0)
        return 0;

    json_t *last = json_array_get(content, json_array_size(content) - 1);
    json_object_set_new(last, "cache_control", build_cache_control(ttl));
    return 1;
}

void chat_apply_cache_breakpoints(json_t *messages, const char *ttl)
{
    size_t n_messages = json_array_size(messages);
    if (n_messages == 0)
        return;

    size_t tail_floor = 0;
    json_t *first = json_array_get(messages, 0);
    const char *role = json_string_value(json_object_get(first, "role"));
    if (role && strcmp(role, "system") == 0 && attach_cache_control(first, ttl))
        tail_floor = 1;

    /* A contentless tool-call message cannot carry the tail breakpoint. */
    for (size_t i = n_messages; i-- > tail_floor;) {
        if (attach_cache_control(json_array_get(messages, i), ttl))
            return;
    }
}

enum chat_reasoning_format chat_reasoning_format_parse(const char *value,
                                                       enum chat_reasoning_format fallback)
{
    if (!value || !*value)
        return fallback;
    if (strcasecmp(value, "flat") == 0)
        return CHAT_REASONING_FLAT;
    if (strcasecmp(value, "nested") == 0)
        return CHAT_REASONING_NESTED;

    hax_warn("unknown reasoning format %s (expected 'flat' or 'nested') — using default", value);
    return fallback;
}

void chat_apply_reasoning(json_t *body, enum chat_reasoning_format format, const char *effort)
{
    if (!effort || !*effort)
        return;

    switch (format) {
    case CHAT_REASONING_FLAT:
        json_object_set_new(body, "reasoning_effort", json_string(effort));
        break;
    case CHAT_REASONING_NESTED: {
        int enabled = strcmp(effort, "none") != 0;
        json_t *reasoning = json_pack("{s:b}", "enabled", enabled);
        if (enabled)
            json_object_set_new(reasoning, "effort", json_string(effort));
        json_object_set_new(body, "reasoning", reasoning);
        break;
    }
    }
}

static json_t *build_tools(const struct tool_def *tools, size_t n_tools)
{
    json_t *tool_list = json_array();
    for (size_t i = 0; i < n_tools; i++) {
        json_t *parameters = tool_schema_build(&tools[i]);
        json_array_append_new(tool_list, json_pack("{s:s, s:{s:s, s:s, s:o}}", "type", "function",
                                                   "function", "name", tools[i].name, "description",
                                                   tools[i].description, "parameters", parameters));
    }
    return tool_list;
}

json_t *chat_build_body(const struct context *context, const char *provider_id, const char *model,
                        const struct wire_body_opts *opts)
{
    json_t *messages =
        chat_build_messages(context->system_prompt, context->items, context->n_items,
                            opts->reasoning_field, provider_id, model, context->image_input);
    if (opts->cache_markers)
        chat_apply_cache_breakpoints(messages, opts->cache_ttl);

    /* Usage is requested on every stream so terminal events can report token counts. */
    json_t *body = json_pack("{s:s, s:b, s:o, s:{s:b}}", "model", model, "stream", 1, "messages",
                             messages, "stream_options", "include_usage", 1);

    if (context->n_tools > 0)
        json_object_set_new(body, "tools", build_tools(context->tools, context->n_tools));
    if (opts->session_cache_key)
        json_object_set_new(body, "prompt_cache_key", json_string(opts->session_cache_key));
    if (opts->emit_progress)
        json_object_set_new(body, "return_progress", json_true());
    if (opts->request_cost)
        json_object_set_new(body, "usage", json_pack("{s:b}", "include", 1));

    chat_apply_reasoning(body, opts->reasoning_format, context->effort);
    return body;
}
