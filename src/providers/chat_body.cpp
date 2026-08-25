/* SPDX-License-Identifier: MIT */
#include "providers/chat_body.h"

#include <string.h>
#include <strings.h>
#include <utility>

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

hax::json::value chat_build_messages(const char *system_prompt, const struct item *items,
                                     size_t n_items, const char *reasoning_field,
                                     const char *current_provider, const char *current_model,
                                     int image_input)
{
    auto encoded =
        hax::openai_compat_json::build_chat_messages(system_prompt, items, n_items, reasoning_field,
                                                     current_provider, current_model, image_input);
    auto decoded = encoded ? hax::json::parse_array(*encoded, {.source = "Chat Completions request",
                                                               .max_input_bytes = 0,
                                                               .allow_unknown_keys = true})
                           : std::nullopt;
    return decoded ? std::move(*decoded) : hax::json::value(hax::json::array{});
}

static hax::json::value build_cache_control(const char *ttl)
{
    hax::json::value cache_control = hax::json::object{{"type", "ephemeral"}};
    if (ttl && strcasecmp(ttl, "1h") == 0)
        cache_control.set("ttl", "1h");
    return cache_control;
}

static int attach_cache_control(hax::json::value *message, const char *ttl)
{
    hax::json::value *content = message->find("content");
    if (!content)
        return 0;
    if (content->is_string()) {
        hax::json::value part = hax::json::object{{"type", "text"}, {"text", *content}};
        part.set("cache_control", build_cache_control(ttl));
        message->set("content", hax::json::array{std::move(part)});
        return 1;
    }
    if (!content->is_array() || content->array_items().empty())
        return 0;

    if (!content->array_items().back().is_object())
        return 0;
    content->array_items().back().set("cache_control", build_cache_control(ttl));
    return 1;
}

void chat_apply_cache_breakpoints(hax::json::value *messages, const char *ttl)
{
    if (!messages->is_array() || messages->array_items().empty())
        return;

    size_t tail_floor = 0;
    hax::json::value &first = messages->array_items().front();
    const hax::json::value *role = first.find("role");
    if (role && role->is_string() && role->string_value() == "system" &&
        attach_cache_control(&first, ttl))
        tail_floor = 1;

    /* A contentless tool-call message cannot carry the tail breakpoint. */
    for (size_t i = messages->array_items().size(); i-- > tail_floor;)
        if (attach_cache_control(&messages->array_items()[i], ttl))
            return;
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

void chat_apply_reasoning(hax::json::value *body, enum chat_reasoning_format format,
                          const char *effort)
{
    if (!effort || !*effort)
        return;

    switch (format) {
    case CHAT_REASONING_FLAT:
        body->set("reasoning_effort", effort);
        break;
    case CHAT_REASONING_NESTED: {
        const bool enabled = strcmp(effort, "none") != 0;
        hax::json::value reasoning = hax::json::object{{"enabled", enabled}};
        if (enabled)
            reasoning.set("effort", effort);
        body->set("reasoning", std::move(reasoning));
        break;
    }
    }
}

static hax::json::value build_tools(const struct tool_def *tools, size_t n_tools)
{
    hax::json::array tool_list;
    for (size_t i = 0; i < n_tools; i++) {
        hax::json::value function = hax::json::object{
            {"name", tools[i].name},
            {"description", tools[i].description},
            {"parameters", tool_schema_value(&tools[i])},
        };
        tool_list.emplace_back(
            hax::json::object{{"type", "function"}, {"function", std::move(function)}});
    }
    return tool_list;
}

hax::json::value chat_build_body(const struct context *context, const char *provider_id,
                                 const char *model, const struct wire_body_opts *opts)
{
    hax::json::value messages =
        chat_build_messages(context->system_prompt, context->items, context->n_items,
                            opts->reasoning_field, provider_id, model, context->image_input);
    if (opts->cache_markers)
        chat_apply_cache_breakpoints(&messages, opts->cache_ttl);

    hax::json::value body = hax::json::object{
        {"model", model},
        {"stream", true},
        {"messages", std::move(messages)},
        {"stream_options", hax::json::object{{"include_usage", true}}},
    };

    if (context->n_tools > 0)
        body.set("tools", build_tools(context->tools, context->n_tools));
    if (opts->session_cache_key)
        body.set("prompt_cache_key", opts->session_cache_key);
    if (opts->emit_progress)
        body.set("return_progress", true);
    if (opts->request_cost)
        body.set("usage", hax::json::object{{"include", true}});

    chat_apply_reasoning(&body, opts->reasoning_format, context->effort);
    return body;
}
