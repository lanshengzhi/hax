/* SPDX-License-Identifier: MIT */
#include "providers/responses_body.h"

#include <jansson.h>
#include <string.h>

#include "provider.h"
#include "tool_schema.h"
#include "providers/responses_json.h"
#include "providers/wire.h"

json_t *responses_build_input_items(const struct item *items, size_t n_items, const char *provider,
                                    const char *model, int image_input)
{
    json_t *input = json_array();
    json_t *decoded = NULL;
    json_t *result = input;
    auto encoded =
        hax::responses_json::build_input_items(items, n_items, provider, model, image_input);
    if (!encoded)
        goto out;

    decoded = json_loads(encoded->c_str(), 0, NULL);
    if (!json_is_array(decoded))
        goto out;

    json_decref(input);
    input = NULL;
    result = decoded;
    decoded = NULL;

out:
    json_decref(decoded);
    return result; /* input remains owned by result on every failure path. */
}

json_t *responses_build_tools(const struct tool_def *tools, size_t n_tools)
{
    json_t *definitions = json_array();
    for (size_t i = 0; i < n_tools; i++) {
        json_t *parameters = tool_schema_build(&tools[i]);
        json_array_append_new(definitions,
                              json_pack("{s:s, s:s, s:s, s:o}", "type", "function", "name",
                                        tools[i].name, "description", tools[i].description,
                                        "parameters", parameters));
    }
    return definitions;
}

/* An unset effort leaves the level to the backend, which still reasons on a reasoning model, so
 * only an explicit "none" rules reasoning out. Encrypted reasoning is requested whenever reasoning
 * remains possible: with store:false, replaying it is the only way a model carries a chain of
 * thought across the tool calls of one turn. Models that never reason ignore the request. */
static void apply_reasoning(json_t *body, const char *effort)
{
    int disabled = effort && strcmp(effort, "none") == 0;
    if (!disabled) {
        json_t *include = json_array();
        json_array_append_new(include, json_string("reasoning.encrypted_content"));
        json_object_set_new(body, "include", include);
    }

    if (!effort || !*effort)
        return;

    json_t *reasoning = json_object();
    json_object_set_new(reasoning, "effort", json_string(effort));
    if (!disabled)
        json_object_set_new(reasoning, "summary", json_string("auto"));
    json_object_set_new(body, "reasoning", reasoning);
}

json_t *responses_build_body(const struct context *context, const char *provider, const char *model,
                             const struct wire_body_opts *opts)
{
    json_t *input = responses_build_input_items(context->items, context->n_items, provider, model,
                                                context->image_input);
    json_t *body = json_pack("{s:s, s:b, s:b, s:s, s:o}", "model", model, "stream", 1, "store", 0,
                             "instructions", context->system_prompt ? context->system_prompt : "",
                             "input", input);

    if (context->n_tools > 0) {
        json_object_set_new(body, "tools", responses_build_tools(context->tools, context->n_tools));
        json_object_set_new(body, "tool_choice", json_string("auto"));
        json_object_set_new(body, "parallel_tool_calls", json_true());
    }
    apply_reasoning(body, context->effort);
    if (opts && opts->session_cache_key)
        json_object_set_new(body, "prompt_cache_key", json_string(opts->session_cache_key));
    return body;
}
