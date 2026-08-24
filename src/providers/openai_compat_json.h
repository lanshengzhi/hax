/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_OPENAI_COMPAT_JSON_H
#define HAX_PROVIDERS_OPENAI_COMPAT_JSON_H

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

struct effort_set;
struct item;
struct llamacpp_reconcile;
struct model_info;

namespace hax::openai_compat_json
{

struct chat_tool_call_delta {
    std::optional<long> index;
    std::optional<std::string> id;
    std::optional<std::string> name;
    std::optional<std::string> arguments;
};

struct chat_delta {
    std::optional<std::string> content;
    std::optional<std::string> reasoning;
    std::optional<std::string> reasoning_content;
    std::vector<std::string> reasoning_details;
    bool has_reasoning_details = false;
    std::vector<chat_tool_call_delta> tool_calls;
    bool has_tool_calls = false;
};

struct chat_choice {
    std::optional<chat_delta> delta;
    std::optional<std::string> finish_reason;
};

struct chat_usage {
    std::optional<long> prompt_tokens;
    std::optional<long> completion_tokens;
    std::optional<long> cached_tokens;
    std::optional<long> cache_write_tokens;
    std::optional<double> cost;
};

struct chat_progress {
    std::optional<long> processed;
    std::optional<long> total;
    std::optional<long> cache;
};

struct chat_error {
    std::optional<std::string> message;
};

struct chat_chunk {
    std::optional<std::string> id;
    std::optional<std::string> model;
    std::optional<std::string> provider;
    std::vector<chat_choice> choices;
    std::optional<chat_usage> usage;
    std::optional<chat_progress> progress;
    std::optional<chat_error> error;
};

/* Decode one Chat Completions SSE JSON object. Unknown keys and malformed optional members are
 * ignored according to the provider compatibility policy. */
std::optional<chat_chunk> parse_chat_chunk(std::string_view input);

/* Append one opaque reasoning-details object to the compact JSON array in `current`. The returned
 * string is owned by the caller; NULL means that either input was not a compatible object or the
 * fragment could not be represented. */
char *append_reasoning_detail(const char *current, std::string_view detail);

/* Build Chat Completions messages through private typed wire DTOs. The returned JSON document is
 * owned by the caller; a missing value means that DTO serialization failed. */
std::optional<std::string> build_chat_messages(const char *system_prompt, const ::item *items,
                                               size_t n_items, const char *reasoning_field,
                                               const char *current_provider,
                                               const char *current_model, int image_input);

/* OpenRouter metadata and account-response adapters. They mutate only fields that the wire
 * reported, preserving the caller's initialized unknown values. */
void parse_openrouter_model(std::string_view input, struct model_info *info);
void parse_openrouter_efforts(std::string_view input, struct effort_set *efforts);
void parse_openrouter_model_probe_response(std::string_view input, std::string_view model,
                                           struct model_info *info);

struct openrouter_key_usage {
    std::optional<std::string> label;
    std::optional<bool> free_tier;
    std::optional<double> spent;
    std::optional<double> limit;
    std::optional<double> remaining;
};

struct openrouter_credits {
    std::optional<double> total;
    std::optional<double> spent;
};

std::optional<openrouter_key_usage> parse_openrouter_key_usage(std::string_view input);
std::optional<openrouter_credits> parse_openrouter_credits(std::string_view input);

/* llama.cpp model-list and /props adapters. */
int reconcile_llamacpp_model(std::string_view input, const char *configured_model,
                             struct llamacpp_reconcile *decision);
void parse_llamacpp_props(std::string_view input, const char *model, struct model_info *info);
void parse_llamacpp_model(std::string_view input, struct model_info *info);

} // namespace hax::openai_compat_json

#endif /* HAX_PROVIDERS_OPENAI_COMPAT_JSON_H */
