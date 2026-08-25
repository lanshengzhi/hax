/* SPDX-License-Identifier: MIT */
#include "providers/responses_json.h"

#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "json.h"
#include "provider.h"
#include "util.h"

namespace hax::responses_json::detail
{

using raw_json = glz::raw_json;
using raw_members = std::map<std::string, raw_json>;

/* Provider events enter as raw members so optional extensions can be decoded independently. */
struct raw_event {
    std::optional<raw_json> type;
    std::optional<raw_json> item;
    std::optional<raw_json> delta;
    std::optional<raw_json> item_id;
    std::optional<raw_json> summary_index;
    std::optional<raw_json> content_index;
    std::optional<raw_json> response;
    std::optional<raw_json> message;
    std::optional<raw_json> code;
};

struct raw_output_item {
    std::optional<raw_json> type;
    std::optional<raw_json> id;
    std::optional<raw_json> call_id;
    std::optional<raw_json> name;
    std::optional<raw_json> arguments;
    std::optional<raw_json> summary;
    std::optional<raw_json> encrypted_content;
};

struct raw_response {
    std::optional<raw_json> id;
    std::optional<raw_json> model;
    std::optional<raw_json> usage;
    std::optional<raw_json> error;
    std::optional<raw_json> incomplete_details;
};

struct raw_usage {
    std::optional<raw_json> input_tokens;
    std::optional<raw_json> output_tokens;
    std::optional<raw_json> input_tokens_details;
};

struct raw_input_tokens_details {
    std::optional<raw_json> cached_tokens;
};

struct raw_response_error {
    std::optional<raw_json> message;
};

struct raw_incomplete_details {
    std::optional<raw_json> reason;
};

/* Request DTOs keep heterogeneous input items typed; raw_json preserves opaque nested state. */
struct input_text_part {
    std::string type;
    std::string text;
};

struct input_image_part {
    std::string type;
    std::string image_url;
};

struct input_message {
    std::string type;
    std::string role;
    std::vector<raw_json> content;
};

struct input_function_call {
    std::string type;
    std::string call_id;
    std::string name;
    std::string arguments;
};

struct input_function_output_text {
    std::string type;
    std::string call_id;
    std::string output;
};

struct input_function_output_parts {
    std::string type;
    std::string call_id;
    std::vector<raw_json> output;
};

struct input_reasoning {
    std::string type;
    raw_json summary;
    raw_json encrypted_content;
};

} // namespace hax::responses_json::detail

#define HAX_RESPONSES_META(TYPE, ...)                                                              \
    template <> struct glz::meta<hax::responses_json::detail::TYPE> {                              \
        using T = hax::responses_json::detail::TYPE;                                               \
        static constexpr auto value = glz::object(__VA_ARGS__);                                    \
    }

HAX_RESPONSES_META(raw_event, "type", &T::type, "item", &T::item, "delta", &T::delta, "item_id",
                   &T::item_id, "summary_index", &T::summary_index, "content_index",
                   &T::content_index, "response", &T::response, "message", &T::message, "code",
                   &T::code);
HAX_RESPONSES_META(raw_output_item, "type", &T::type, "id", &T::id, "call_id", &T::call_id, "name",
                   &T::name, "arguments", &T::arguments, "summary", &T::summary,
                   "encrypted_content", &T::encrypted_content);
HAX_RESPONSES_META(raw_response, "id", &T::id, "model", &T::model, "usage", &T::usage, "error",
                   &T::error, "incomplete_details", &T::incomplete_details);
HAX_RESPONSES_META(raw_usage, "input_tokens", &T::input_tokens, "output_tokens", &T::output_tokens,
                   "input_tokens_details", &T::input_tokens_details);
HAX_RESPONSES_META(raw_input_tokens_details, "cached_tokens", &T::cached_tokens);
HAX_RESPONSES_META(raw_response_error, "message", &T::message);
HAX_RESPONSES_META(raw_incomplete_details, "reason", &T::reason);
HAX_RESPONSES_META(input_text_part, "type", &T::type, "text", &T::text);
HAX_RESPONSES_META(input_image_part, "type", &T::type, "image_url", &T::image_url);
HAX_RESPONSES_META(input_message, "type", &T::type, "role", &T::role, "content", &T::content);
HAX_RESPONSES_META(input_function_call, "type", &T::type, "call_id", &T::call_id, "name", &T::name,
                   "arguments", &T::arguments);
HAX_RESPONSES_META(input_function_output_text, "type", &T::type, "call_id", &T::call_id, "output",
                   &T::output);
HAX_RESPONSES_META(input_function_output_parts, "type", &T::type, "call_id", &T::call_id, "output",
                   &T::output);
HAX_RESPONSES_META(input_reasoning, "type", &T::type, "summary", &T::summary, "encrypted_content",
                   &T::encrypted_content);

#undef HAX_RESPONSES_META

namespace
{

namespace detail = hax::responses_json::detail;
using detail::raw_json;

constexpr hax::json::options RESPONSE_JSON_OPTIONS = {
    .source = "OpenAI Responses response",
    .max_input_bytes = 0,
    .allow_unknown_keys = true,
};

constexpr hax::json::options REQUEST_JSON_OPTIONS = {
    .source = "OpenAI Responses request",
    .max_input_bytes = 0,
    .allow_unknown_keys = true,
};

template <typename T> std::optional<T> decode(const raw_json &source)
{
    auto value = hax::json::parse<T>(source.str, RESPONSE_JSON_OPTIONS);
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

static bool is_integer_literal(std::string_view source)
{
    size_t first = 0;
    while (first < source.size() && (source[first] == ' ' || source[first] == '\t' ||
                                     source[first] == '\n' || source[first] == '\r'))
        first++;
    size_t last = source.size();
    while (last > first && (source[last - 1] == ' ' || source[last - 1] == '\t' ||
                            source[last - 1] == '\n' || source[last - 1] == '\r'))
        last--;
    if (first == last)
        return false;

    if (source[first] == '-')
        first++;
    if (first == last)
        return false;
    if (source[first] == '0')
        return first + 1 == last;
    if (source[first] < '1' || source[first] > '9')
        return false;
    for (size_t i = first + 1; i < last; i++)
        if (source[i] < '0' || source[i] > '9')
            return false;
    return true;
}

static std::optional<long> decoded_long(const std::optional<raw_json> &source)
{
    if (!source || !is_integer_literal(source->str))
        return std::nullopt;
    return decode<long>(*source);
}

static size_t first_json_byte(std::string_view input)
{
    size_t first = 0;
    while (first < input.size() && (input[first] == ' ' || input[first] == '\t' ||
                                    input[first] == '\n' || input[first] == '\r'))
        first++;
    return first;
}

static bool starts_with_container(std::string_view input)
{
    const size_t first = first_json_byte(input);
    return first < input.size() && (input[first] == '{' || input[first] == '[');
}

static bool is_json_null(const raw_json &source)
{
    return source.str.substr(first_json_byte(source.str), 4) == "null";
}

static const raw_json *find_member(const detail::raw_members *object, std::string_view name)
{
    if (!object)
        return NULL;
    auto member = object->find(std::string(name));
    return member == object->end() ? NULL : &member->second;
}

template <typename T> static std::optional<raw_json> encode_raw(const T &value)
{
    auto encoded = hax::json::serialize_escaped(value, REQUEST_JSON_OPTIONS);
    if (!encoded)
        return std::nullopt;
    return raw_json{std::move(*encoded)};
}

static std::optional<raw_json> encode_text_part(const char *type, std::string_view text)
{
    return encode_raw(
        detail::input_text_part{.type = type ? type : "input_text", .text = std::string(text)});
}

static std::optional<raw_json> encode_image_part(const struct item_image &image)
{
    std::string url = "data:";
    url += image.mime ? image.mime : "image/png";
    url += ";base64,";
    url += image.data_b64 ? image.data_b64 : "";
    return encode_raw(detail::input_image_part{.type = "input_image", .image_url = std::move(url)});
}

static std::optional<raw_json> encode_message(const char *role, const char *content_type,
                                              const char *text)
{
    auto content = encode_text_part(content_type, text ? text : "");
    if (!content)
        return std::nullopt;

    detail::input_message message = {
        .type = "message",
        .role = role ? role : "",
        .content = {std::move(*content)},
    };
    return encode_raw(message);
}

static std::optional<std::vector<raw_json>> encode_tool_output_parts(const struct item *item,
                                                                     int image_input)
{
    std::vector<raw_json> parts;
    if (item->output && *item->output) {
        auto text = encode_text_part("input_text", item->output);
        if (!text)
            return std::nullopt;
        parts.push_back(std::move(*text));
    }

    for (size_t i = 0; i < item->n_images; i++) {
        std::optional<raw_json> image;
        if (image_input != 0)
            image = encode_image_part(item->images[i]);
        else {
            char *placeholder = item_image_placeholder(&item->images[i]);
            image = encode_text_part("input_text", placeholder ? placeholder : "");
            free(placeholder);
        }
        if (!image)
            return std::nullopt;
        parts.push_back(std::move(*image));
    }
    return parts;
}

static std::optional<raw_json> encode_input_item(const struct item *item, const char *provider,
                                                 const char *model, int image_input)
{
    switch (item->kind) {
    case ITEM_USER_MESSAGE:
        return encode_message("user", "input_text", item->text);
    case ITEM_ASSISTANT_MESSAGE:
        return encode_message("assistant", "output_text", item->text);
    case ITEM_TOOL_CALL:
        return encode_raw(detail::input_function_call{
            .type = "function_call",
            .call_id = item->call_id ? item->call_id : "",
            .name = item->tool_name ? item->tool_name : "",
            .arguments = item->tool_arguments_json ? item->tool_arguments_json : "{}",
        });
    case ITEM_TOOL_RESULT:
        if (item->n_images == 0)
            return encode_raw(detail::input_function_output_text{
                .type = "function_call_output",
                .call_id = item->call_id ? item->call_id : "",
                .output = item->output ? item->output : "",
            });
        if (auto parts = encode_tool_output_parts(item, image_input))
            return encode_raw(detail::input_function_output_parts{
                .type = "function_call_output",
                .call_id = item->call_id ? item->call_id : "",
                .output = std::move(*parts),
            });
        return std::nullopt;
    case ITEM_REASONING:
        if (!item->reasoning_json || !provider_provenance_matches(item, provider, model))
            return std::nullopt;
        if (!starts_with_container(item->reasoning_json) ||
            !hax::json::validate(item->reasoning_json, REQUEST_JSON_OPTIONS))
            return std::nullopt;
        return raw_json{std::string(item->reasoning_json)};
    case ITEM_TURN_BOUNDARY:
    case ITEM_TURN_USAGE:
        return std::nullopt;
    }
    return std::nullopt;
}

static hax::responses_json::parsed_usage parsed_usage_from(const detail::raw_usage &usage)
{
    hax::responses_json::parsed_usage result{};
    result.input_tokens = decoded_long(usage.input_tokens);
    result.output_tokens = decoded_long(usage.output_tokens);
    if (auto details = decode<detail::raw_input_tokens_details>(usage.input_tokens_details))
        result.cached_tokens = decoded_long(details->cached_tokens);
    return result;
}

} // namespace

namespace hax::responses_json
{

std::optional<parsed_event> parse_event(std::string_view input)
{
    auto decoded = hax::json::parse<detail::raw_event>(input, RESPONSE_JSON_OPTIONS);
    if (!decoded)
        return std::nullopt;

    auto members = hax::json::parse<detail::raw_members>(input, RESPONSE_JSON_OPTIONS);
    const detail::raw_members *root = members ? &*members : NULL;
    parsed_event result;
    result.type = decoded_string(decoded->type);
    result.delta = decoded_string(decoded->delta);
    result.item_id = decoded_string(decoded->item_id);
    result.message = decoded_string(decoded->message);
    result.code = decoded_string(decoded->code);
    result.summary_index_present = find_member(root, "summary_index") != NULL;
    result.content_index_present = find_member(root, "content_index") != NULL;
    if (decoded->summary_index)
        result.summary_index = decoded_long(decoded->summary_index);
    if (decoded->content_index)
        result.content_index = decoded_long(decoded->content_index);

    if (auto item = decode<detail::raw_output_item>(decoded->item)) {
        auto item_members =
            decoded->item ? decode<detail::raw_members>(*decoded->item) : std::nullopt;
        const detail::raw_members *item_root = item_members ? &*item_members : NULL;
        const raw_json *summary_value = find_member(item_root, "summary");
        const raw_json *encrypted_value = find_member(item_root, "encrypted_content");
        result.item = parsed_output_item{
            .type = decoded_string(item->type),
            .id = decoded_string(item->id),
            .call_id = decoded_string(item->call_id),
            .name = decoded_string(item->name),
            .arguments = decoded_string(item->arguments),
            .summary_json =
                summary_value ? std::optional<std::string>(summary_value->str) : std::nullopt,
            .encrypted_content_json = encrypted_value && !is_json_null(*encrypted_value)
                                          ? std::optional<std::string>(encrypted_value->str)
                                          : std::nullopt,
        };
    }

    if (auto response = decode<detail::raw_response>(decoded->response)) {
        parsed_response parsed;
        parsed.id = decoded_string(response->id);
        parsed.model = decoded_string(response->model);
        if (auto usage = decode<detail::raw_usage>(response->usage))
            parsed.usage = parsed_usage_from(*usage);
        if (auto error = decode<detail::raw_response_error>(response->error))
            parsed.error_message = decoded_string(error->message);
        if (auto details = decode<detail::raw_incomplete_details>(response->incomplete_details))
            parsed.incomplete_reason = decoded_string(details->reason);
        result.response = std::move(parsed);
    }
    return result;
}

std::optional<std::string> encode_reasoning_item(const parsed_output_item &item)
{
    if (!item.encrypted_content_json)
        return std::nullopt;

    detail::input_reasoning reasoning = {
        .type = "reasoning",
        .summary = raw_json{item.summary_json ? *item.summary_json : "[]"},
        .encrypted_content = raw_json{*item.encrypted_content_json},
    };
    auto encoded = hax::json::serialize_escaped(reasoning, REQUEST_JSON_OPTIONS);
    if (!encoded)
        return std::nullopt;
    return std::move(*encoded);
}

std::optional<std::string> build_input_items(const struct item *items, size_t n_items,
                                             const char *provider, const char *model,
                                             int image_input)
{
    std::vector<detail::raw_json> input;
    input.reserve(n_items);
    for (size_t i = 0; i < n_items; i++) {
        auto item = encode_input_item(&items[i], provider, model, image_input);
        if (item)
            input.push_back(std::move(*item));
    }

    auto encoded = hax::json::serialize_escaped(input, REQUEST_JSON_OPTIONS);
    if (!encoded)
        return std::nullopt;
    return std::move(*encoded);
}

} // namespace hax::responses_json
