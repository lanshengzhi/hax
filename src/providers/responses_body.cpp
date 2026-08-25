/* SPDX-License-Identifier: MIT */
#include "providers/responses_body.h"

#include <string.h>
#include <utility>

#include "provider.h"
#include "tool_schema.h"
#include "providers/responses_json.h"
#include "providers/wire.h"

hax::json::value responses_build_input_items(const struct item *items, size_t n_items,
                                             const char *provider, const char *model,
                                             int image_input)
{
    auto encoded =
        hax::responses_json::build_input_items(items, n_items, provider, model, image_input);
    auto decoded = encoded ? hax::json::parse_array(*encoded, {.source = "OpenAI Responses request",
                                                               .max_input_bytes = 0,
                                                               .allow_unknown_keys = true})
                           : std::nullopt;
    return decoded ? std::move(*decoded) : hax::json::value(hax::json::array{});
}

hax::json::value responses_build_tools(const struct tool_def *tools, size_t n_tools)
{
    hax::json::array definitions;
    for (size_t i = 0; i < n_tools; i++)
        definitions.emplace_back(hax::json::object{
            {"type", "function"},
            {"name", tools[i].name},
            {"description", tools[i].description},
            {"parameters", tool_schema_value(&tools[i])},
        });
    return definitions;
}

/* An unset effort leaves the level to the backend. Encrypted reasoning is requested while
 * reasoning remains possible so store:false turns can carry it across tool calls. */
static void apply_reasoning(hax::json::value *body, const char *effort)
{
    const bool disabled = effort && strcmp(effort, "none") == 0;
    if (!disabled)
        body->set("include", hax::json::array{"reasoning.encrypted_content"});

    if (!effort || !*effort)
        return;

    hax::json::value reasoning = hax::json::object{{"effort", effort}};
    if (!disabled)
        reasoning.set("summary", "auto");
    body->set("reasoning", std::move(reasoning));
}

hax::json::value responses_build_body(const struct context *context, const char *provider,
                                      const char *model, const struct wire_body_opts *opts)
{
    hax::json::value input = responses_build_input_items(context->items, context->n_items, provider,
                                                         model, context->image_input);
    hax::json::value body = hax::json::object{
        {"model", model},
        {"stream", true},
        {"store", false},
        {"instructions", context->system_prompt ? context->system_prompt : ""},
        {"input", std::move(input)},
    };

    if (context->n_tools > 0) {
        body.set("tools", responses_build_tools(context->tools, context->n_tools));
        body.set("tool_choice", "auto");
        body.set("parallel_tool_calls", true);
    }
    apply_reasoning(&body, context->effort);
    if (opts && opts->session_cache_key)
        body.set("prompt_cache_key", opts->session_cache_key);
    return body;
}
