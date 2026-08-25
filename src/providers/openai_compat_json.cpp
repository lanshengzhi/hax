/* SPDX-License-Identifier: MIT */
#include "providers/openai_compat_json.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "catalog.h"
#include "effort.h"
#include "json.h"
#include "model_meta.h"
#include "provider.h"
#include "tool_schema.h"
#include "util.h"
#include "providers/llamacpp.h"

namespace hax::openai_compat_json::detail
{

using raw_json = glz::raw_json;

/* Provider responses deliberately enter as raw members. Optional fields in compatible APIs are
 * frequently emitted with different shapes, so one malformed member must not reject the rest of
 * a chunk or catalog entry. */
struct raw_chat_chunk {
    std::optional<raw_json> id;
    std::optional<raw_json> model;
    std::optional<raw_json> provider;
    std::optional<raw_json> choices;
    std::optional<raw_json> usage;
    std::optional<raw_json> prompt_progress;
    std::optional<raw_json> error;
};

struct raw_chat_choice {
    std::optional<raw_json> delta;
    std::optional<raw_json> finish_reason;
};

struct raw_chat_delta {
    std::optional<raw_json> content;
    std::optional<raw_json> reasoning;
    std::optional<raw_json> reasoning_content;
    std::optional<raw_json> reasoning_details;
    std::optional<raw_json> tool_calls;
};

struct raw_chat_tool_call {
    std::optional<raw_json> index;
    std::optional<raw_json> id;
    std::optional<raw_json> function;
};

struct raw_chat_function {
    std::optional<raw_json> name;
    std::optional<raw_json> arguments;
};

struct raw_chat_usage {
    std::optional<raw_json> prompt_tokens;
    std::optional<raw_json> completion_tokens;
    std::optional<raw_json> prompt_tokens_details;
    std::optional<raw_json> cost;
};

struct raw_chat_prompt_details {
    std::optional<raw_json> cached_tokens;
    std::optional<raw_json> cache_write_tokens;
};

struct raw_chat_progress {
    std::optional<raw_json> processed;
    std::optional<raw_json> total;
    std::optional<raw_json> cache;
};

struct raw_chat_error {
    std::optional<raw_json> message;
};

/* A reasoning detail has an intentionally small typed view only for validating that the raw
 * fragment is an object. Its complete JSON representation remains in raw_json for replay. */
struct raw_reasoning_detail {
    std::optional<raw_json> type;
    std::optional<raw_json> text;
    std::optional<raw_json> signature;
    std::optional<raw_json> format;
};

struct raw_openrouter_model {
    std::optional<raw_json> id;
    std::optional<raw_json> context_length;
    std::optional<raw_json> top_provider;
    std::optional<raw_json> architecture;
    std::optional<raw_json> supported_parameters;
    std::optional<raw_json> pricing;
    std::optional<raw_json> reasoning;
    std::optional<raw_json> description;
};

struct raw_openrouter_top_provider {
    std::optional<raw_json> max_completion_tokens;
};

struct raw_openrouter_architecture {
    std::optional<raw_json> input_modalities;
};

struct raw_openrouter_pricing {
    std::optional<raw_json> prompt;
    std::optional<raw_json> completion;
    std::optional<raw_json> input_cache_read;
    std::optional<raw_json> input_cache_write;
    std::optional<raw_json> input_cache_write_1h;
    std::optional<raw_json> overrides;
};

struct raw_openrouter_override {
    std::optional<raw_json> min_prompt_tokens;
    std::optional<raw_json> prompt;
    std::optional<raw_json> completion;
    std::optional<raw_json> input_cache_read;
    std::optional<raw_json> input_cache_write;
    std::optional<raw_json> input_cache_write_1h;
};

struct raw_openrouter_reasoning {
    std::optional<raw_json> supported_efforts;
};

struct raw_openrouter_root {
    std::optional<raw_json> data;
};

struct raw_openrouter_key_data {
    std::optional<raw_json> label;
    std::optional<raw_json> is_free_tier;
    std::optional<raw_json> usage;
    std::optional<raw_json> limit;
    std::optional<raw_json> limit_remaining;
};

struct raw_openrouter_credits_data {
    std::optional<raw_json> total_credits;
    std::optional<raw_json> total_usage;
};

struct raw_llama_root {
    std::optional<raw_json> data;
};

struct raw_llama_entry {
    std::optional<raw_json> id;
    std::optional<raw_json> aliases;
    std::optional<raw_json> status;
    std::optional<raw_json> architecture;
    std::optional<raw_json> meta;
};

struct raw_llama_status {
    std::optional<raw_json> value;
    std::optional<raw_json> failed;
    std::optional<raw_json> exit_code;
};

struct raw_llama_architecture {
    std::optional<raw_json> input_modalities;
};

struct raw_llama_meta {
    std::optional<raw_json> n_ctx;
};

struct raw_llama_props {
    std::optional<raw_json> default_generation_settings;
    std::optional<raw_json> modalities;
};

struct raw_llama_generation_settings {
    std::optional<raw_json> n_ctx;
};

struct raw_llama_modalities {
    std::optional<raw_json> vision;
};

/* Typed Chat Completions request DTOs. The message content union is the one shape that cannot be
 * represented by a single scalar member: compatible servers accept a string, null, or content
 * parts depending on the role and image policy. */
struct body_cache_control {
    std::string type;
    std::optional<std::string> ttl;
};

struct body_image_url {
    std::string url;
};

struct body_content_part {
    std::string type;
    std::optional<std::string> text;
    std::optional<body_image_url> image_url;
    std::optional<body_cache_control> cache_control;
};

struct body_tool_function {
    std::string name;
    std::string arguments;
};

struct body_tool_call {
    std::string id;
    std::string type;
    body_tool_function function;
};

struct body_message {
    std::string role;
    std::variant<std::nullptr_t, std::string, std::vector<body_content_part>> content;
    std::optional<std::string> tool_call_id;
    std::optional<std::vector<body_tool_call>> tool_calls;
    std::optional<std::vector<raw_json>> reasoning_details;
    std::optional<std::string> reasoning_content;
};

} // namespace hax::openai_compat_json::detail

#define HAX_RAW_META(TYPE, ...)                                                                    \
    template <> struct glz::meta<hax::openai_compat_json::detail::TYPE> {                          \
        using T = hax::openai_compat_json::detail::TYPE;                                           \
        static constexpr auto value = glz::object(__VA_ARGS__);                                    \
    }

HAX_RAW_META(raw_chat_chunk, "id", &T::id, "model", &T::model, "provider", &T::provider, "choices",
             &T::choices, "usage", &T::usage, "prompt_progress", &T::prompt_progress, "error",
             &T::error);
HAX_RAW_META(raw_chat_choice, "delta", &T::delta, "finish_reason", &T::finish_reason);
HAX_RAW_META(raw_chat_delta, "content", &T::content, "reasoning", &T::reasoning,
             "reasoning_content", &T::reasoning_content, "reasoning_details", &T::reasoning_details,
             "tool_calls", &T::tool_calls);
HAX_RAW_META(raw_chat_tool_call, "index", &T::index, "id", &T::id, "function", &T::function);
HAX_RAW_META(raw_chat_function, "name", &T::name, "arguments", &T::arguments);
HAX_RAW_META(raw_chat_usage, "prompt_tokens", &T::prompt_tokens, "completion_tokens",
             &T::completion_tokens, "prompt_tokens_details", &T::prompt_tokens_details, "cost",
             &T::cost);
HAX_RAW_META(raw_chat_prompt_details, "cached_tokens", &T::cached_tokens, "cache_write_tokens",
             &T::cache_write_tokens);
HAX_RAW_META(raw_chat_progress, "processed", &T::processed, "total", &T::total, "cache", &T::cache);
HAX_RAW_META(raw_chat_error, "message", &T::message);
HAX_RAW_META(raw_reasoning_detail, "type", &T::type, "text", &T::text, "signature", &T::signature,
             "format", &T::format);
HAX_RAW_META(raw_openrouter_model, "id", &T::id, "context_length", &T::context_length,
             "top_provider", &T::top_provider, "architecture", &T::architecture,
             "supported_parameters", &T::supported_parameters, "pricing", &T::pricing, "reasoning",
             &T::reasoning, "description", &T::description);
HAX_RAW_META(raw_openrouter_top_provider, "max_completion_tokens", &T::max_completion_tokens);
HAX_RAW_META(raw_openrouter_architecture, "input_modalities", &T::input_modalities);
HAX_RAW_META(raw_openrouter_pricing, "prompt", &T::prompt, "completion", &T::completion,
             "input_cache_read", &T::input_cache_read, "input_cache_write", &T::input_cache_write,
             "input_cache_write_1h", &T::input_cache_write_1h, "overrides", &T::overrides);
HAX_RAW_META(raw_openrouter_override, "min_prompt_tokens", &T::min_prompt_tokens, "prompt",
             &T::prompt, "completion", &T::completion, "input_cache_read", &T::input_cache_read,
             "input_cache_write", &T::input_cache_write, "input_cache_write_1h",
             &T::input_cache_write_1h);
HAX_RAW_META(raw_openrouter_reasoning, "supported_efforts", &T::supported_efforts);
HAX_RAW_META(raw_openrouter_root, "data", &T::data);
HAX_RAW_META(raw_openrouter_key_data, "label", &T::label, "is_free_tier", &T::is_free_tier, "usage",
             &T::usage, "limit", &T::limit, "limit_remaining", &T::limit_remaining);
HAX_RAW_META(raw_openrouter_credits_data, "total_credits", &T::total_credits, "total_usage",
             &T::total_usage);
HAX_RAW_META(raw_llama_root, "data", &T::data);
HAX_RAW_META(raw_llama_entry, "id", &T::id, "aliases", &T::aliases, "status", &T::status,
             "architecture", &T::architecture, "meta", &T::meta);
HAX_RAW_META(raw_llama_status, "value", &T::value, "failed", &T::failed, "exit_code",
             &T::exit_code);
HAX_RAW_META(raw_llama_architecture, "input_modalities", &T::input_modalities);
HAX_RAW_META(raw_llama_meta, "n_ctx", &T::n_ctx);
HAX_RAW_META(raw_llama_props, "default_generation_settings", &T::default_generation_settings,
             "modalities", &T::modalities);
HAX_RAW_META(raw_llama_generation_settings, "n_ctx", &T::n_ctx);
HAX_RAW_META(raw_llama_modalities, "vision", &T::vision);
HAX_RAW_META(body_cache_control, "type", &T::type, "ttl", &T::ttl);
HAX_RAW_META(body_image_url, "url", &T::url);
HAX_RAW_META(body_content_part, "type", &T::type, "text", &T::text, "image_url", &T::image_url,
             "cache_control", &T::cache_control);
HAX_RAW_META(body_tool_function, "name", &T::name, "arguments", &T::arguments);
HAX_RAW_META(body_tool_call, "id", &T::id, "type", &T::type, "function", &T::function);
HAX_RAW_META(body_message, "role", &T::role, "content", &T::content, "tool_call_id",
             &T::tool_call_id, "tool_calls", &T::tool_calls, "reasoning_details",
             &T::reasoning_details, "reasoning_content", &T::reasoning_content);

#undef HAX_RAW_META

namespace
{

using hax::openai_compat_json::detail::raw_json;
using raw_array = std::vector<raw_json>;
using raw_object = std::vector<std::pair<std::string, raw_json>>;

constexpr hax::json::options PROVIDER_JSON_OPTIONS = {
    .source = "OpenAI-compatible provider",
    .max_input_bytes = 0,
    .allow_unknown_keys = true,
};

template <typename T> std::optional<T> decode(const raw_json &source)
{
    auto value = hax::json::parse<T>(source.str, PROVIDER_JSON_OPTIONS);
    if (!value)
        return std::nullopt;
    return std::move(*value);
}

template <typename T> std::optional<T> decode(const std::optional<raw_json> &source)
{
    if (!source)
        return std::nullopt;
    return decode<T>(*source);
}

static std::optional<std::string> decoded_string(const std::optional<raw_json> &source)
{
    return decode<std::string>(source);
}

static std::optional<long> decoded_long(const std::optional<raw_json> &source)
{
    return decode<long>(source);
}

static std::optional<double> decoded_number(const std::optional<raw_json> &source)
{
    auto value = decode<double>(source);
    if (value)
        return value;

    auto integer = decode<long>(source);
    if (integer)
        return static_cast<double>(*integer);
    return std::nullopt;
}

static std::optional<bool> decoded_bool(const std::optional<raw_json> &source)
{
    return decode<bool>(source);
}

static std::optional<std::vector<raw_json>> decoded_array(const std::optional<raw_json> &source)
{
    return decode<std::vector<raw_json>>(source);
}

static std::optional<hax::openai_compat_json::detail::raw_reasoning_detail>
decoded_reasoning_detail(const raw_json &source)
{
    return decode<hax::openai_compat_json::detail::raw_reasoning_detail>(source);
}

static bool array_contains_string(const std::optional<raw_json> &source, const char *expected)
{
    auto values = decoded_array(source);
    if (!values)
        return false;

    for (const raw_json &value : *values) {
        auto text = decode<std::string>(value);
        if (text && *text == expected)
            return true;
    }
    return false;
}

static enum provider_cap capability_from_array(const std::optional<raw_json> &source,
                                               const char *value)
{
    if (!decoded_array(source))
        return PROVIDER_CAP_UNKNOWN;
    return array_contains_string(source, value) ? PROVIDER_CAP_YES : PROVIDER_CAP_NO;
}

static double parse_token_rate(const std::optional<raw_json> &source)
{
    auto text = decoded_string(source);
    if (!text || text->empty())
        return -1;

    errno = 0;
    char *end = NULL;
    double rate = strtod(text->c_str(), &end);
    if (end == text->c_str() || *end != '\0' || errno == ERANGE || !std::isfinite(rate) || rate < 0)
        return -1;

    rate *= 1e6;
    return std::isfinite(rate) ? rate : -1;
}

static char *description_lead(const std::optional<raw_json> &source)
{
    auto description = decoded_string(source);
    if (!description)
        return NULL;

    size_t length = strcspn(description->c_str(), "\r\n");
    while (length > 0 && ((*description)[length - 1] == ' ' || (*description)[length - 1] == '\t'))
        length--;
    if (length == 0)
        return NULL;

    char *lead = (char *)xmalloc(length + 1);
    memcpy(lead, description->data(), length);
    lead[length] = '\0';
    return lead;
}

static bool entry_names_model(const hax::openai_compat_json::detail::raw_llama_entry &entry,
                              const char *model)
{
    auto id = decoded_string(entry.id);
    if (id && *id == model)
        return true;

    auto aliases = decoded_array(entry.aliases);
    if (!aliases)
        return false;
    for (const raw_json &alias : *aliases) {
        auto value = decode<std::string>(alias);
        if (value && *value == model)
            return true;
    }
    return false;
}

static const raw_json *raw_member(const raw_object &object, const char *name)
{
    for (const auto &member : object)
        if (member.first == name)
            return &member.second;
    return NULL;
}

static raw_json *raw_member(raw_object &object, const char *name)
{
    for (auto &member : object)
        if (member.first == name)
            return &member.second;
    return NULL;
}

static bool raw_is_null(const raw_json &source)
{
    size_t first = 0;
    while (first < source.str.size() && (source.str[first] == ' ' || source.str[first] == '\t' ||
                                         source.str[first] == '\n' || source.str[first] == '\r'))
        first++;
    return source.str.substr(first, 4) == "null";
}

static std::optional<raw_object> parse_raw_object(std::string_view input)
{
    if (!hax::json::validate(input, PROVIDER_JSON_OPTIONS))
        return std::nullopt;
    auto document = glz::lazy_json(input);
    if (!document || !document->root().is_object())
        return std::nullopt;

    raw_object result;
    for (const auto &member : document->root())
        result.emplace_back(std::string(member.key()), raw_json{std::string(member.raw_json())});
    return result;
}

static std::optional<raw_array> parse_raw_array(std::string_view input)
{
    if (!hax::json::validate(input, PROVIDER_JSON_OPTIONS))
        return std::nullopt;
    auto document = glz::lazy_json(input);
    if (!document || !document->root().is_array())
        return std::nullopt;

    raw_array result;
    for (const auto &member : document->root())
        result.emplace_back(std::string(member.raw_json()));
    return result;
}

static std::optional<std::string> serialize_raw_object(const raw_object &object)
{
    std::string encoded = "{";
    for (size_t i = 0; i < object.size(); i++) {
        if (i)
            encoded += ',';
        auto key = hax::json::serialize_escaped(object[i].first, PROVIDER_JSON_OPTIONS);
        if (!key)
            return std::nullopt;
        encoded += *key;
        encoded += ':';
        encoded += object[i].second.str;
    }
    encoded += '}';
    return encoded;
}

static std::optional<std::string> serialize_raw_array(const raw_array &array)
{
    std::string encoded = "[";
    for (size_t i = 0; i < array.size(); i++) {
        if (i)
            encoded += ',';
        encoded += array[i].str;
    }
    encoded += ']';
    return encoded;
}

static bool is_reasoning_text(const raw_object &detail)
{
    const raw_json *type = raw_member(detail, "type");
    auto decoded = type ? decode<std::string>(*type) : std::nullopt;
    return decoded && *decoded == "reasoning.text";
}

static bool has_member(const raw_object &detail, const char *name)
{
    const raw_json *value = raw_member(detail, name);
    if (!value || raw_is_null(*value))
        return false;
    auto text = decode<std::string>(*value);
    return !text || !text->empty();
}

static char *append_reasoning_detail_raw(const char *current, std::string_view detail)
{
    auto block = parse_raw_object(detail);
    if (!block)
        return NULL;

    raw_array details;
    if (current) {
        auto decoded = parse_raw_array(current);
        if (!decoded)
            return NULL;
        details = std::move(*decoded);
    }

    if (!details.empty()) {
        auto last = parse_raw_object(details.back().str);
        if (last && is_reasoning_text(*last) && is_reasoning_text(*block)) {
            const raw_json *tail = raw_member(*block, "text");
            auto tail_text = tail ? decode<std::string>(*tail) : std::nullopt;
            if (tail_text && !tail_text->empty()) {
                const raw_json *head = raw_member(*last, "text");
                auto head_text = head ? decode<std::string>(*head) : std::nullopt;
                std::string joined = head_text ? *head_text : "";
                joined += *tail_text;
                auto encoded = hax::json::serialize_escaped(joined, PROVIDER_JSON_OPTIONS);
                if (!encoded)
                    return NULL;
                if (auto *text = raw_member(*last, "text"))
                    *text = raw_json{std::move(*encoded)};
                else
                    last->emplace_back("text", raw_json{std::move(*encoded)});
            }
            static const char *const CLOSING[] = {"signature", "format"};
            for (const char *key : CLOSING) {
                const raw_json *source = raw_member(*block, key);
                if (has_member(*block, key) && !has_member(*last, key))
                    last->emplace_back(key, *source);
            }
            auto merged = serialize_raw_object(*last);
            if (!merged)
                return NULL;
            details.back() = raw_json{std::move(*merged)};
        } else {
            auto encoded = serialize_raw_object(*block);
            if (!encoded)
                return NULL;
            details.emplace_back(std::move(*encoded));
        }
    } else {
        auto encoded = serialize_raw_object(*block);
        if (!encoded)
            return NULL;
        details.emplace_back(std::move(*encoded));
    }

    auto encoded = serialize_raw_array(details);
    return encoded ? xstrdup(encoded->c_str()) : NULL;
}

static void append_body_reasoning(std::optional<std::vector<raw_json>> *destination,
                                  const char *reasoning_json)
{
    if (!reasoning_json)
        return;

    auto details = hax::json::parse<std::vector<raw_json>>(
        reasoning_json,
        {.source = "Chat reasoning details", .max_input_bytes = 0, .allow_unknown_keys = true});
    if (!details)
        return;

    if (!*destination)
        destination->emplace();
    /* Request replay treats each array element as opaque provider state, including fragments whose
     * shape a newer backend does not yet understand. */
    for (const raw_json &detail : *details)
        destination->value().push_back(detail);
}

static hax::openai_compat_json::detail::body_tool_call
body_tool_call_from_item(const struct item *item)
{
    return {
        .id = item->call_id ? item->call_id : "",
        .type = "function",
        .function = {.name = item->tool_name ? item->tool_name : "",
                     .arguments = item->tool_arguments_json ? item->tool_arguments_json : "{}"},
    };
}

static std::string tool_result_text(const struct item *item)
{
    std::string text = item->output ? item->output : "";
    for (size_t i = 0; i < item->n_images; i++) {
        char *placeholder = item_image_placeholder(&item->images[i]);
        if (!text.empty())
            text += '\n';
        text += placeholder ? placeholder : "";
        free(placeholder);
    }
    return text;
}

static hax::openai_compat_json::detail::body_content_part text_part(std::string text)
{
    return {.type = "text", .text = std::move(text)};
}

static size_t
append_assistant_message(std::vector<hax::openai_compat_json::detail::body_message> *messages,
                         const struct item *items, size_t index, size_t n_items,
                         const char *reasoning_field, const char *current_provider,
                         const char *current_model)
{
    std::string text;
    std::string reasoning;
    std::optional<std::vector<raw_json>> details;
    std::vector<hax::openai_compat_json::detail::body_tool_call> tool_calls;

    while (index < n_items &&
           (items[index].kind == ITEM_ASSISTANT_MESSAGE || items[index].kind == ITEM_TOOL_CALL ||
            items[index].kind == ITEM_REASONING)) {
        const struct item *item = &items[index++];
        switch (item->kind) {
        case ITEM_ASSISTANT_MESSAGE:
            if (item->text)
                text += item->text;
            break;
        case ITEM_REASONING:
            if (!provider_provenance_matches(item, current_provider, current_model))
                break;
            if (item->reasoning_text && *item->reasoning_text) {
                if (!reasoning.empty())
                    reasoning += '\n';
                reasoning += item->reasoning_text;
            }
            append_body_reasoning(&details, item->reasoning_json);
            break;
        case ITEM_TOOL_CALL:
            tool_calls.push_back(body_tool_call_from_item(item));
            break;
        default:
            break;
        }
    }

    const bool include_reasoning = reasoning_field && !reasoning.empty() && !details;
    if (text.empty() && tool_calls.empty() && !include_reasoning && !details)
        return index;

    hax::openai_compat_json::detail::body_message message = {
        .role = "assistant",
        .content =
            text.empty()
                ? std::variant<std::nullptr_t, std::string,
                               std::vector<hax::openai_compat_json::detail::body_content_part>>(
                      nullptr)
                : std::variant<std::nullptr_t, std::string,
                               std::vector<hax::openai_compat_json::detail::body_content_part>>(
                      std::move(text)),
    };
    if (!tool_calls.empty())
        message.tool_calls = std::move(tool_calls);
    if (details)
        message.reasoning_details = std::move(details);
    else if (include_reasoning)
        message.reasoning_content = std::move(reasoning);
    messages->push_back(std::move(message));
    return index;
}

static size_t
append_tool_results(std::vector<hax::openai_compat_json::detail::body_message> *messages,
                    const struct item *items, size_t index, size_t n_items, int image_input)
{
    const size_t first = index;
    while (index < n_items && items[index].kind == ITEM_TOOL_RESULT) {
        const struct item *item = &items[index++];
        std::string content = item->output ? item->output : "";
        if (item->n_images > 0 && image_input == 0)
            content = tool_result_text(item);
        messages->push_back({.role = "tool",
                             .content = std::move(content),
                             .tool_call_id = item->call_id ? item->call_id : ""});
    }

    if (image_input != 0) {
        std::vector<hax::openai_compat_json::detail::body_content_part> parts;
        for (size_t i = first; i < index; i++) {
            for (size_t j = 0; j < items[i].n_images; j++) {
                if (parts.empty())
                    parts.push_back(text_part("Image(s) from the preceding tool result(s):"));
                const struct item_image *image = &items[i].images[j];
                std::string url = "data:";
                url += image->mime ? image->mime : "image/png";
                url += ";base64,";
                url += image->data_b64 ? image->data_b64 : "";
                parts.push_back({.type = "image_url",
                                 .image_url = hax::openai_compat_json::detail::body_image_url{
                                     .url = std::move(url)}});
            }
        }
        if (!parts.empty())
            messages->push_back({.role = "user", .content = std::move(parts)});
    }
    return index;
}

static std::optional<std::string> rename_reasoning_field(std::string encoded,
                                                         const char *reasoning_field)
{
    if (!reasoning_field || strcmp(reasoning_field, "reasoning_content") == 0)
        return encoded;

    auto messages = parse_raw_array(encoded);
    if (!messages)
        return std::nullopt;

    for (raw_json &message_json : *messages) {
        auto message = parse_raw_object(message_json.str);
        if (!message)
            continue;
        raw_json *reasoning = raw_member(*message, "reasoning_content");
        if (!reasoning)
            continue;

        auto custom = raw_member(*message, reasoning_field);
        if (custom) {
            *custom = std::move(*reasoning);
            for (auto it = message->begin(); it != message->end(); ++it) {
                if (it->first == "reasoning_content") {
                    message->erase(it);
                    break;
                }
            }
        } else {
            raw_json moved = std::move(*reasoning);
            for (auto it = message->begin(); it != message->end(); ++it) {
                if (it->first == "reasoning_content") {
                    message->erase(it);
                    break;
                }
            }
            message->emplace_back(reasoning_field, std::move(moved));
        }
        auto rewritten = serialize_raw_object(*message);
        if (!rewritten)
            return std::nullopt;
        message_json = raw_json{std::move(*rewritten)};
    }

    auto result = serialize_raw_array(*messages);
    if (!result)
        return std::nullopt;
    return result;
}

static void
parse_openrouter_model_value(const hax::openai_compat_json::detail::raw_openrouter_model &entry,
                             struct model_info *info)
{
    auto context_length = decoded_long(entry.context_length);
    if (context_length && *context_length > 0)
        info->context = *context_length;

    auto top_provider =
        decode<hax::openai_compat_json::detail::raw_openrouter_top_provider>(entry.top_provider);
    if (top_provider) {
        auto max_completion = decoded_long(top_provider->max_completion_tokens);
        if (max_completion && *max_completion > 0)
            info->max_output = *max_completion;
    }

    auto architecture =
        decode<hax::openai_compat_json::detail::raw_openrouter_architecture>(entry.architecture);
    info->image_input = architecture
                            ? capability_from_array(architecture->input_modalities, "image")
                            : PROVIDER_CAP_UNKNOWN;
    info->tools = capability_from_array(entry.supported_parameters, "tools");

    auto pricing = decode<hax::openai_compat_json::detail::raw_openrouter_pricing>(entry.pricing);
    if (pricing) {
        info->cost_input = parse_token_rate(pricing->prompt);
        info->cost_output = parse_token_rate(pricing->completion);
        info->cost_cache_read = parse_token_rate(pricing->input_cache_read);
        info->cost_cache_write = parse_token_rate(pricing->input_cache_write);
        info->cost_cache_write_1h = parse_token_rate(pricing->input_cache_write_1h);

        auto overrides = decoded_array(pricing->overrides);
        if (overrides) {
            for (const raw_json &value : *overrides) {
                if (info->n_tiers >= CATALOG_TIERS_MAX)
                    break;
                auto override =
                    decode<hax::openai_compat_json::detail::raw_openrouter_override>(value);
                if (!override)
                    continue;
                auto threshold = decoded_long(override->min_prompt_tokens);
                if (!threshold || *threshold <= 0)
                    continue;
                struct catalog_tier *tier = &info->tiers[info->n_tiers++];
                tier->context_threshold = *threshold;
                tier->cost_input = parse_token_rate(override->prompt);
                tier->cost_output = parse_token_rate(override->completion);
                tier->cost_cache_read = parse_token_rate(override->input_cache_read);
                tier->cost_cache_write = parse_token_rate(override->input_cache_write);
                tier->cost_cache_write_1h = parse_token_rate(override->input_cache_write_1h);
            }
        }
    }

    auto reasoning =
        decode<hax::openai_compat_json::detail::raw_openrouter_reasoning>(entry.reasoning);
    if (reasoning) {
        auto levels = decoded_array(reasoning->supported_efforts);
        if (levels) {
            info->efforts.known = 1;
            for (const raw_json &level : *levels) {
                auto effort = decode<std::string>(level);
                if (effort)
                    effort_set_add(&info->efforts, effort->c_str());
            }
        }
    } else {
        auto parameters = decoded_array(entry.supported_parameters);
        if (parameters) {
            /* A supported reasoning switch without categorical levels is unknown. */
            bool has_reasoning = false;
            for (const raw_json &parameter : *parameters) {
                auto value = decode<std::string>(parameter);
                if (value && (*value == "reasoning" || *value == "reasoning_effort")) {
                    has_reasoning = true;
                    break;
                }
            }
            if (!has_reasoning)
                info->efforts.known = 1;
        }
    }

    info->description = description_lead(entry.description);
}

} // namespace

namespace hax::openai_compat_json
{

std::optional<chat_chunk> parse_chat_chunk(std::string_view input)
{
    auto decoded = hax::json::parse<detail::raw_chat_chunk>(
        input,
        {.source = "Chat Completions stream", .max_input_bytes = 0, .allow_unknown_keys = true});
    if (!decoded)
        return std::nullopt;

    chat_chunk result;
    result.id = decoded_string(decoded->id);
    result.model = decoded_string(decoded->model);
    result.provider = decoded_string(decoded->provider);

    if (auto choices = decoded_array(decoded->choices)) {
        for (const detail::raw_json &value : *choices) {
            auto choice = decode<detail::raw_chat_choice>(value);
            if (!choice)
                continue;
            chat_choice converted;
            converted.finish_reason = decoded_string(choice->finish_reason);
            auto delta = decode<detail::raw_chat_delta>(choice->delta);
            if (delta) {
                chat_delta converted_delta;
                converted_delta.content = decoded_string(delta->content);
                converted_delta.reasoning = decoded_string(delta->reasoning);
                converted_delta.reasoning_content = decoded_string(delta->reasoning_content);

                if (auto details = decoded_array(delta->reasoning_details)) {
                    converted_delta.has_reasoning_details = true;
                    for (const detail::raw_json &fragment : *details)
                        if (decoded_reasoning_detail(fragment))
                            converted_delta.reasoning_details.push_back(fragment.str);
                }

                if (auto calls = decoded_array(delta->tool_calls)) {
                    converted_delta.has_tool_calls = true;
                    for (const detail::raw_json &call_value : *calls) {
                        auto call = decode<detail::raw_chat_tool_call>(call_value);
                        if (!call)
                            continue;
                        chat_tool_call_delta converted_call;
                        converted_call.index = decoded_long(call->index);
                        converted_call.id = decoded_string(call->id);
                        auto function = decode<detail::raw_chat_function>(call->function);
                        if (function) {
                            converted_call.name = decoded_string(function->name);
                            converted_call.arguments = decoded_string(function->arguments);
                        }
                        converted_delta.tool_calls.push_back(std::move(converted_call));
                    }
                }
                converted.delta = std::move(converted_delta);
            }
            result.choices.push_back(std::move(converted));
        }
    }

    if (auto usage = decode<detail::raw_chat_usage>(decoded->usage)) {
        chat_usage converted;
        converted.prompt_tokens = decoded_long(usage->prompt_tokens);
        converted.completion_tokens = decoded_long(usage->completion_tokens);
        converted.cost = decoded_number(usage->cost);
        if (auto details = decode<detail::raw_chat_prompt_details>(usage->prompt_tokens_details)) {
            converted.cached_tokens = decoded_long(details->cached_tokens);
            converted.cache_write_tokens = decoded_long(details->cache_write_tokens);
        }
        result.usage = converted;
    }

    if (auto progress = decode<detail::raw_chat_progress>(decoded->prompt_progress)) {
        result.progress = chat_progress{
            .processed = decoded_long(progress->processed),
            .total = decoded_long(progress->total),
            .cache = decoded_long(progress->cache),
        };
    }

    if (auto error = decode<detail::raw_chat_error>(decoded->error))
        result.error = chat_error{.message = decoded_string(error->message)};

    return result;
}

char *append_reasoning_detail(const char *current, std::string_view detail)
{
    return append_reasoning_detail_raw(current, detail);
}

std::optional<std::string> build_chat_messages(const char *system_prompt, const ::item *items,
                                               size_t n_items, const char *reasoning_field,
                                               const char *current_provider,
                                               const char *current_model, int image_input)
{
    std::vector<detail::body_message> messages;
    if (system_prompt && *system_prompt)
        messages.push_back({.role = "system", .content = std::string(system_prompt)});

    size_t index = 0;
    while (index < n_items) {
        switch (items[index].kind) {
        case ITEM_USER_MESSAGE:
            messages.push_back(
                {.role = "user",
                 .content = std::string(items[index].text ? items[index].text : "")});
            index++;
            break;
        case ITEM_ASSISTANT_MESSAGE:
        case ITEM_TOOL_CALL:
            index = append_assistant_message(&messages, items, index, n_items, reasoning_field,
                                             current_provider, current_model);
            break;
        case ITEM_TOOL_RESULT:
            index = append_tool_results(&messages, items, index, n_items, image_input);
            break;
        case ITEM_REASONING:
            if (items[index].reasoning_text || items[index].reasoning_json)
                index = append_assistant_message(&messages, items, index, n_items, reasoning_field,
                                                 current_provider, current_model);
            else
                index++;
            break;
        case ITEM_TURN_BOUNDARY:
        case ITEM_TURN_USAGE:
            index++;
            break;
        }
    }

    auto encoded = hax::json::serialize_escaped(messages, {.source = "Chat Completions messages"});
    if (!encoded)
        return std::nullopt;
    return rename_reasoning_field(std::move(*encoded), reasoning_field);
}

void parse_openrouter_model(std::string_view input, struct model_info *info)
{
    if (!info)
        return;
    auto entry = hax::json::parse<detail::raw_openrouter_model>(
        input, {.source = "OpenRouter model", .max_input_bytes = 0, .allow_unknown_keys = true});
    if (entry)
        parse_openrouter_model_value(*entry, info);
}

void parse_openrouter_efforts(std::string_view input, struct effort_set *efforts)
{
    if (!efforts)
        return;
    auto entry = hax::json::parse<detail::raw_openrouter_model>(
        input, {.source = "OpenRouter model", .max_input_bytes = 0, .allow_unknown_keys = true});
    if (!entry)
        return;

    auto reasoning = decode<detail::raw_openrouter_reasoning>(entry->reasoning);
    if (reasoning) {
        auto levels = decoded_array(reasoning->supported_efforts);
        if (!levels)
            return;
        efforts->known = 1;
        for (const detail::raw_json &level : *levels) {
            auto value = decode<std::string>(level);
            if (value)
                effort_set_add(efforts, value->c_str());
        }
        return;
    }

    auto parameters = decoded_array(entry->supported_parameters);
    if (!parameters)
        return;
    for (const detail::raw_json &parameter : *parameters) {
        auto value = decode<std::string>(parameter);
        if (value && (*value == "reasoning" || *value == "reasoning_effort"))
            return;
    }
    efforts->known = 1;
}

void parse_openrouter_model_probe_response(std::string_view input, std::string_view model,
                                           struct model_info *info)
{
    if (!info)
        return;
    auto root = hax::json::parse<detail::raw_openrouter_root>(
        input,
        {.source = "OpenRouter model probe", .max_input_bytes = 0, .allow_unknown_keys = true});
    if (!root)
        return;
    auto entries = decoded_array(root->data);
    if (!entries)
        return;
    for (const detail::raw_json &value : *entries) {
        auto entry = decode<detail::raw_openrouter_model>(value);
        if (!entry)
            continue;
        auto id = decoded_string(entry->id);
        if (id && *id == model) {
            parse_openrouter_model_value(*entry, info);
            return;
        }
    }
}

std::optional<openrouter_key_usage> parse_openrouter_key_usage(std::string_view input)
{
    auto root = hax::json::parse<detail::raw_openrouter_root>(
        input,
        {.source = "OpenRouter key usage", .max_input_bytes = 0, .allow_unknown_keys = true});
    if (!root)
        return std::nullopt;
    auto data = decode<detail::raw_openrouter_key_data>(root->data);
    if (!data)
        return std::nullopt;

    return openrouter_key_usage{
        .label = decoded_string(data->label),
        .free_tier = decoded_bool(data->is_free_tier),
        .spent = decoded_number(data->usage),
        .limit = decoded_number(data->limit),
        .remaining = decoded_number(data->limit_remaining),
    };
}

std::optional<openrouter_credits> parse_openrouter_credits(std::string_view input)
{
    auto root = hax::json::parse<detail::raw_openrouter_root>(
        input, {.source = "OpenRouter credits", .max_input_bytes = 0, .allow_unknown_keys = true});
    if (!root)
        return std::nullopt;
    auto data = decode<detail::raw_openrouter_credits_data>(root->data);
    if (!data)
        return std::nullopt;

    return openrouter_credits{
        .total = decoded_number(data->total_credits),
        .spent = decoded_number(data->total_usage),
    };
}

int reconcile_llamacpp_model(std::string_view input, const char *configured_model,
                             struct llamacpp_reconcile *decision)
{
    if (!decision)
        return -1;
    memset(decision, 0, sizeof(*decision));

    auto root = hax::json::parse<detail::raw_llama_root>(
        input,
        {.source = "llama.cpp model list", .max_input_bytes = 0, .allow_unknown_keys = true});
    if (!root)
        return -1;
    auto models = decoded_array(root->data);
    if (!models)
        return -1;

    const bool configured = configured_model && *configured_model;
    const char *first_model = NULL;
    const char *running_model = NULL;
    const char *configured_id = NULL;
    size_t running_count = 0;
    int router = 0;

    std::vector<std::string> served_names;
    served_names.reserve(models->size());
    for (const detail::raw_json &value : *models) {
        auto entry = decode<detail::raw_llama_entry>(value);
        if (!entry)
            continue;
        auto served_model = decoded_string(entry->id);
        if (!served_model)
            continue;
        served_names.push_back(*served_model);
        const char *served = served_names.back().c_str();
        if (!first_model)
            first_model = served;

        auto status = decode<detail::raw_llama_status>(entry->status);
        if (status) {
            router = 1;
            auto state = decoded_string(status->value);
            if (state && (*state == "loaded" || *state == "loading" || *state == "sleeping")) {
                running_model = served;
                running_count++;
            }
        }
        if (configured && !configured_id && entry_names_model(*entry, configured_model))
            configured_id = served;
    }

    if (!first_model) {
        decision->no_models = 1;
    } else if (!configured) {
        if (!router)
            decision->replacement = xstrdup(first_model);
        else if (running_count == 1)
            decision->replacement = xstrdup(running_model);
    } else if (!configured_id) {
        if (router)
            decision->clear_configured = 1;
        else
            decision->replacement = xstrdup(first_model);
    } else if (strcmp(configured_id, configured_model) != 0) {
        decision->canonical = xstrdup(configured_id);
    }
    return 0;
}

void parse_llamacpp_props(std::string_view input, const char *model, struct model_info *info)
{
    (void)model;
    if (!info)
        return;
    auto root = hax::json::parse<detail::raw_llama_props>(
        input,
        {.source = "llama.cpp properties", .max_input_bytes = 0, .allow_unknown_keys = true});
    if (!root)
        return;

    auto settings =
        decode<detail::raw_llama_generation_settings>(root->default_generation_settings);
    auto context = settings ? decoded_long(settings->n_ctx) : std::nullopt;
    if (context && *context > 0)
        info->context = *context;

    auto modalities = decode<detail::raw_llama_modalities>(root->modalities);
    if (modalities) {
        auto vision = decoded_bool(modalities->vision);
        if (vision)
            info->image_input = *vision ? PROVIDER_CAP_YES : PROVIDER_CAP_NO;
    }
}

void parse_llamacpp_model(std::string_view input, struct model_info *info)
{
    if (!info)
        return;
    auto entry = hax::json::parse<detail::raw_llama_entry>(
        input, {.source = "llama.cpp model", .max_input_bytes = 0, .allow_unknown_keys = true});
    if (!entry)
        return;

    auto meta = decode<detail::raw_llama_meta>(entry->meta);
    auto context = meta ? decoded_long(meta->n_ctx) : std::nullopt;
    if (context && *context > 0)
        info->context = *context;

    auto architecture = decode<detail::raw_llama_architecture>(entry->architecture);
    if (architecture)
        info->image_input = capability_from_array(architecture->input_modalities, "image");

    auto status = decode<detail::raw_llama_status>(entry->status);
    if (!status)
        return;
    auto failed = decoded_bool(status->failed);
    if (failed && *failed) {
        auto exit_code = decoded_long(status->exit_code);
        info->description =
            exit_code ? xasprintf("failed (exit %ld)", *exit_code) : xstrdup("failed");
    } else {
        auto state = decoded_string(status->value);
        if (state && *state != "unloaded")
            info->description = xstrdup(state->c_str());
    }
}

} // namespace hax::openai_compat_json
