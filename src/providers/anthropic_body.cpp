/* SPDX-License-Identifier: MIT */
#include "providers/anthropic_body.h"

#include <string.h>
#include <strings.h>
#include <utility>

#include "provider.h"
#include "tool_schema.h"
#include "providers/anthropic_json.h"
#include "providers/wire.h"

const char *const ANTHROPIC_EFFORT_LADDER[] = {"low", "medium", "high", "xhigh", "max"};
const size_t ANTHROPIC_EFFORT_LADDER_N =
    sizeof(ANTHROPIC_EFFORT_LADDER) / sizeof(ANTHROPIC_EFFORT_LADDER[0]);

hax::json::value anthropic_build_messages(const struct item *items, size_t n_items,
                                          const char *current_provider, const char *current_model,
                                          int allow_empty_signature, int image_input)
{
    auto encoded = hax::anthropic_json::build_messages(
        items, n_items, current_provider, current_model, allow_empty_signature, image_input);
    auto decoded = encoded
                       ? hax::json::parse_array(*encoded, {.source = "Anthropic Messages request",
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

static hax::json::value build_tools(const struct tool_def *tools, size_t n_tools, int cache_last,
                                    const char *ttl)
{
    hax::json::array tool_list;
    for (size_t i = 0; i < n_tools; i++) {
        hax::json::value tool = hax::json::object{
            {"name", tools[i].name},
            {"description", tools[i].description},
            {"input_schema", tool_schema_value(&tools[i])},
        };
        if (cache_last && i == n_tools - 1)
            tool.set("cache_control", build_cache_control(ttl));
        tool_list.emplace_back(std::move(tool));
    }
    return tool_list;
}

static void attach_cache_to_last_message(hax::json::value *messages, const char *ttl)
{
    if (!messages->is_array() || messages->array_items().empty())
        return;

    hax::json::value &message = messages->array_items().back();
    hax::json::value *content = message.find("content");
    if (!content || !content->is_array() || content->array_items().empty())
        return;

    hax::json::value &block = content->array_items().back();
    if (!block.is_object())
        return;
    const hax::json::value *type = block.find("type");
    if (type && type->is_string() &&
        (type->string_value() == "thinking" || type->string_value() == "redacted_thinking"))
        return;
    block.set("cache_control", build_cache_control(ttl));
}

static void apply_thinking(hax::json::value *body, const struct context *context,
                           const struct wire_body_opts *opts)
{
    if (opts->thinking_mode == ANTHROPIC_THINKING_OFF)
        return;

    if (opts->thinking_mode == ANTHROPIC_THINKING_ADAPTIVE) {
        const char *display = opts->show_reasoning ? "summarized" : "omitted";
        body->set("thinking", hax::json::object{{"type", "adaptive"}, {"display", display}});
        if (context->effort && *context->effort)
            body->set("output_config", hax::json::object{{"effort", context->effort}});
        return;
    }

    /* Anthropic requires 1 <= budget_tokens < max_tokens. */
    if (opts->max_tokens < 2)
        return;
    int budget_tokens = opts->thinking_budget;
    if (budget_tokens <= 0 || budget_tokens >= opts->max_tokens)
        budget_tokens = opts->max_tokens - 1;
    body->set("thinking", hax::json::object{{"type", "enabled"}, {"budget_tokens", budget_tokens}});
}

hax::json::value anthropic_build_body(const struct context *context, const char *provider_id,
                                      const char *model, const struct wire_body_opts *opts)
{
    hax::json::value messages =
        anthropic_build_messages(context->items, context->n_items, provider_id, model,
                                 opts->allow_empty_signature, context->image_input);
    hax::json::value body = hax::json::object{
        {"model", model},
        {"max_tokens", opts->max_tokens},
        {"stream", true},
        {"messages", std::move(messages)},
    };

    if (context->system_prompt && *context->system_prompt) {
        hax::json::value system_block =
            hax::json::object{{"type", "text"}, {"text", context->system_prompt}};
        if (opts->cache_markers)
            system_block.set("cache_control", build_cache_control(opts->cache_ttl));
        body.set("system", hax::json::array{std::move(system_block)});
    }

    if (context->n_tools > 0)
        body.set("tools", build_tools(context->tools, context->n_tools, opts->cache_markers,
                                      opts->cache_ttl));
    if (opts->cache_markers)
        attach_cache_to_last_message(body.find("messages"), opts->cache_ttl);

    apply_thinking(&body, context, opts);
    return body;
}
