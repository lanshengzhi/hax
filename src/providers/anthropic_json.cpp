/* SPDX-License-Identifier: MIT */
#include "providers/anthropic_json.h"

#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "effort.h"
#include "json.h"
#include "model_meta.h"
#include "provider.h"
#include "util.h"

namespace hax::anthropic_json::detail
{

using raw_json = glz::raw_json;

/* Provider responses deliberately enter as raw members. Optional fields are commonly extended or
 * shaped differently by compatible endpoints, so each member is converted independently below. */
struct raw_event {
    std::optional<raw_json> type;
    std::optional<raw_json> index;
    std::optional<raw_json> content_block;
    std::optional<raw_json> delta;
    std::optional<raw_json> message;
    std::optional<raw_json> usage;
    std::optional<raw_json> error;
};

struct raw_content_block {
    std::optional<raw_json> type;
    std::optional<raw_json> id;
    std::optional<raw_json> name;
    std::optional<raw_json> data;
};

struct raw_delta {
    std::optional<raw_json> type;
    std::optional<raw_json> text;
    std::optional<raw_json> thinking;
    std::optional<raw_json> signature;
    std::optional<raw_json> partial_json;
    std::optional<raw_json> stop_reason;
};

struct raw_message {
    std::optional<raw_json> id;
    std::optional<raw_json> model;
    std::optional<raw_json> usage;
};

struct raw_usage {
    std::optional<raw_json> input_tokens;
    std::optional<raw_json> cache_read_input_tokens;
    std::optional<raw_json> cache_creation_input_tokens;
    std::optional<raw_json> cache_creation;
    std::optional<raw_json> output_tokens;
};

struct raw_cache_creation {
    std::optional<raw_json> ephemeral_1h_input_tokens;
};

struct raw_error {
    std::optional<raw_json> message;
};

struct raw_reasoning_block {
    std::optional<raw_json> type;
    std::optional<raw_json> thinking;
    std::optional<raw_json> signature;
    std::optional<raw_json> data;
};

struct raw_model_entry {
    std::optional<raw_json> id;
    std::optional<raw_json> max_input_tokens;
    std::optional<raw_json> max_tokens;
    std::optional<raw_json> capabilities;
};

struct raw_capabilities {
    std::optional<raw_json> image_input;
    std::optional<raw_json> effort;
};

struct raw_capability {
    std::optional<raw_json> supported;
};

struct raw_models_response {
    std::optional<raw_json> data;
};

struct raw_models_page {
    raw_json data;
    std::optional<raw_json> has_more;
    std::optional<raw_json> last_id;
};

/* Message content is kept as raw typed blocks so opaque reasoning objects survive the request
 * boundary without exposing Glaze or provider-specific unions to the caller. */
struct body_text_block {
    std::string type;
    std::string text;
};

struct body_image_source {
    std::string type;
    std::string media_type;
    std::string data;
};

struct body_image_block {
    std::string type;
    body_image_source source;
};

struct body_tool_use {
    std::string type;
    std::string id;
    std::string name;
    raw_json input;
};

struct body_tool_result {
    std::string type;
    std::string tool_use_id;
    std::variant<std::string, std::vector<raw_json>> content;
};

struct body_message {
    std::string role;
    std::vector<raw_json> content;
};

struct reasoning_thinking {
    std::string type;
    std::string thinking;
    std::string signature;
};

struct reasoning_redacted {
    std::string type;
    std::string data;
};

} // namespace hax::anthropic_json::detail

#define HAX_ANTHROPIC_META(TYPE, ...)                                                              \
    template <> struct glz::meta<hax::anthropic_json::detail::TYPE> {                              \
        using T = hax::anthropic_json::detail::TYPE;                                               \
        static constexpr auto value = glz::object(__VA_ARGS__);                                    \
    }

HAX_ANTHROPIC_META(raw_event, "type", &T::type, "index", &T::index, "content_block",
                   &T::content_block, "delta", &T::delta, "message", &T::message, "usage",
                   &T::usage, "error", &T::error);
HAX_ANTHROPIC_META(raw_content_block, "type", &T::type, "id", &T::id, "name", &T::name, "data",
                   &T::data);
HAX_ANTHROPIC_META(raw_delta, "type", &T::type, "text", &T::text, "thinking", &T::thinking,
                   "signature", &T::signature, "partial_json", &T::partial_json, "stop_reason",
                   &T::stop_reason);
HAX_ANTHROPIC_META(raw_message, "id", &T::id, "model", &T::model, "usage", &T::usage);
HAX_ANTHROPIC_META(raw_usage, "input_tokens", &T::input_tokens, "cache_read_input_tokens",
                   &T::cache_read_input_tokens, "cache_creation_input_tokens",
                   &T::cache_creation_input_tokens, "cache_creation", &T::cache_creation,
                   "output_tokens", &T::output_tokens);
HAX_ANTHROPIC_META(raw_cache_creation, "ephemeral_1h_input_tokens", &T::ephemeral_1h_input_tokens);
HAX_ANTHROPIC_META(raw_error, "message", &T::message);
HAX_ANTHROPIC_META(raw_reasoning_block, "type", &T::type, "thinking", &T::thinking, "signature",
                   &T::signature, "data", &T::data);
HAX_ANTHROPIC_META(raw_model_entry, "id", &T::id, "max_input_tokens", &T::max_input_tokens,
                   "max_tokens", &T::max_tokens, "capabilities", &T::capabilities);
HAX_ANTHROPIC_META(raw_capabilities, "image_input", &T::image_input, "effort", &T::effort);
HAX_ANTHROPIC_META(raw_capability, "supported", &T::supported);
HAX_ANTHROPIC_META(raw_models_response, "data", &T::data);
HAX_ANTHROPIC_META(raw_models_page, "data", &T::data, "has_more", &T::has_more, "last_id",
                   &T::last_id);
HAX_ANTHROPIC_META(body_text_block, "type", &T::type, "text", &T::text);
HAX_ANTHROPIC_META(body_image_source, "type", &T::type, "media_type", &T::media_type, "data",
                   &T::data);
HAX_ANTHROPIC_META(body_image_block, "type", &T::type, "source", &T::source);
HAX_ANTHROPIC_META(body_tool_use, "type", &T::type, "id", &T::id, "name", &T::name, "input",
                   &T::input);
HAX_ANTHROPIC_META(body_tool_result, "type", &T::type, "tool_use_id", &T::tool_use_id, "content",
                   &T::content);
HAX_ANTHROPIC_META(body_message, "role", &T::role, "content", &T::content);
HAX_ANTHROPIC_META(reasoning_thinking, "type", &T::type, "thinking", &T::thinking, "signature",
                   &T::signature);
HAX_ANTHROPIC_META(reasoning_redacted, "type", &T::type, "data", &T::data);

#undef HAX_ANTHROPIC_META

namespace
{

namespace detail = hax::anthropic_json::detail;
using hax::anthropic_json::detail::raw_json;

constexpr hax::json::options RESPONSE_JSON_OPTIONS = {
    .source = "Anthropic Messages response",
    .max_input_bytes = 0,
    .allow_unknown_keys = true,
};

constexpr hax::json::options REQUEST_JSON_OPTIONS = {
    .source = "Anthropic Messages request",
    .max_input_bytes = 0,
    .allow_unknown_keys = true,
};

template <typename T> std::optional<T> decode(const raw_json &source, const auto &options)
{
    auto value = hax::json::parse<T>(source.str, options);
    if (!value)
        return std::nullopt;
    return std::move(*value);
}

template <typename T>
std::optional<T> decode(const std::optional<raw_json> &source, const auto &options)
{
    if (!source)
        return std::nullopt;
    return decode<T>(*source, options);
}

static std::optional<std::string> decoded_string(const std::optional<raw_json> &source)
{
    return decode<std::string>(source, RESPONSE_JSON_OPTIONS);
}

static std::optional<long> decoded_long(const std::optional<raw_json> &source)
{
    return decode<long>(source, RESPONSE_JSON_OPTIONS);
}

static std::optional<bool> decoded_bool(const std::optional<raw_json> &source)
{
    return decode<bool>(source, RESPONSE_JSON_OPTIONS);
}

static std::optional<std::vector<raw_json>> decoded_array(const std::optional<raw_json> &source)
{
    return decode<std::vector<raw_json>>(source, RESPONSE_JSON_OPTIONS);
}

static hax::anthropic_json::parsed_usage parsed_usage_from(const detail::raw_usage &usage)
{
    hax::anthropic_json::parsed_usage result{};
    result.input_tokens = decoded_long(usage.input_tokens);
    result.cache_read_input_tokens = decoded_long(usage.cache_read_input_tokens);
    result.cache_creation_input_tokens = decoded_long(usage.cache_creation_input_tokens);
    if (auto cache =
            decode<detail::raw_cache_creation>(usage.cache_creation, RESPONSE_JSON_OPTIONS))
        result.cache_write_1h_input_tokens = decoded_long(cache->ephemeral_1h_input_tokens);
    result.output_tokens = decoded_long(usage.output_tokens);
    return result;
}

static size_t first_json_byte(std::string_view input)
{
    size_t first = 0;
    while (first < input.size() && (input[first] == ' ' || input[first] == '\t' ||
                                    input[first] == '\n' || input[first] == '\r'))
        first++;
    return first;
}

static bool starts_with_object(std::string_view input)
{
    const size_t first = first_json_byte(input);
    return first < input.size() && input[first] == '{';
}

static bool starts_with_null(std::string_view input)
{
    const size_t first = first_json_byte(input);
    return input.substr(first, 4) == "null";
}

static std::optional<raw_json> parse_raw_json(std::string_view input,
                                              const hax::json::options &options)
{
    if (!hax::json::validate(input, options))
        return std::nullopt;
    return raw_json{std::string(input)};
}

static bool is_object_json(std::string_view input)
{
    return starts_with_object(input) && parse_raw_json(input, REQUEST_JSON_OPTIONS).has_value();
}

template <typename T> static std::optional<raw_json> encode_raw(const T &value)
{
    auto encoded = hax::json::serialize_escaped(value, REQUEST_JSON_OPTIONS);
    if (!encoded)
        return std::nullopt;
    return raw_json{std::move(*encoded)};
}

static std::optional<raw_json> encode_text_block(std::string_view text)
{
    return encode_raw(detail::body_text_block{.type = "text", .text = std::string(text)});
}

static std::optional<raw_json> encode_image_block(const struct item_image &image)
{
    detail::body_image_source source = {
        .type = "base64",
        .media_type = image.mime ? image.mime : "image/png",
        .data = image.data_b64 ? image.data_b64 : "",
    };
    return encode_raw(detail::body_image_block{.type = "image", .source = std::move(source)});
}

static std::optional<raw_json> encode_tool_use(const struct item *item)
{
    raw_json input{"{}"};
    if (item->tool_arguments_json && is_object_json(item->tool_arguments_json))
        input = raw_json{std::string(item->tool_arguments_json)};

    return encode_raw(detail::body_tool_use{
        .type = "tool_use",
        .id = item->call_id ? item->call_id : "",
        .name = item->tool_name ? item->tool_name : "",
        .input = std::move(input),
    });
}

static std::optional<raw_json> encode_tool_result(const struct item *item, int image_input)
{
    if (item->n_images == 0) {
        return encode_raw(detail::body_tool_result{
            .type = "tool_result",
            .tool_use_id = item->call_id ? item->call_id : "",
            .content = std::string(item->output ? item->output : ""),
        });
    }

    std::vector<raw_json> content;
    if (item->output && *item->output) {
        auto text = encode_text_block(item->output);
        if (!text)
            return std::nullopt;
        content.push_back(std::move(*text));
    }
    for (size_t i = 0; i < item->n_images; i++) {
        std::optional<raw_json> image;
        if (image_input != 0) {
            image = encode_image_block(item->images[i]);
        } else {
            char *placeholder = item_image_placeholder(&item->images[i]);
            image = encode_text_block(placeholder ? placeholder : "");
            free(placeholder);
        }
        if (!image)
            return std::nullopt;
        content.push_back(std::move(*image));
    }

    return encode_raw(detail::body_tool_result{
        .type = "tool_result",
        .tool_use_id = item->call_id ? item->call_id : "",
        .content = std::move(content),
    });
}

static bool append_reasoning_block(std::vector<raw_json> *content, const struct item *item,
                                   int allow_empty_signature)
{
    if (!item->reasoning_json)
        return true;

    auto parsed = parse_raw_json(item->reasoning_json, REQUEST_JSON_OPTIONS);
    if (!parsed)
        return true;
    if (!starts_with_object(item->reasoning_json)) {
        content->push_back(std::move(*parsed));
        return true;
    }

    auto block = decode<detail::raw_reasoning_block>(*parsed, REQUEST_JSON_OPTIONS);
    if (!block) {
        content->push_back(std::move(*parsed));
        return true;
    }

    auto type = decoded_string(block->type);
    if (type && *type == "thinking" && !allow_empty_signature) {
        auto signature = decoded_string(block->signature);
        if (!signature || signature->empty()) {
            auto thinking = decoded_string(block->thinking);
            if (!thinking || thinking->empty())
                return true;
            auto text = encode_text_block(*thinking);
            if (!text)
                return false;
            content->push_back(std::move(*text));
            return true;
        }
    }

    content->emplace_back(std::string(item->reasoning_json));
    return true;
}

static size_t append_assistant_message(std::vector<detail::body_message> *messages,
                                       const struct item *items, size_t index, size_t n_items,
                                       const char *current_provider, const char *current_model,
                                       int allow_empty_signature, bool *success)
{
    std::vector<raw_json> content;
    while (index < n_items &&
           (items[index].kind == ITEM_ASSISTANT_MESSAGE || items[index].kind == ITEM_TOOL_CALL ||
            items[index].kind == ITEM_REASONING)) {
        const struct item *item = &items[index++];
        switch (item->kind) {
        case ITEM_ASSISTANT_MESSAGE:
            if (item->text && *item->text) {
                auto text = encode_text_block(item->text);
                if (!text) {
                    *success = false;
                    return index;
                }
                content.push_back(std::move(*text));
            }
            break;
        case ITEM_REASONING:
            if (provider_provenance_matches(item, current_provider, current_model) &&
                !append_reasoning_block(&content, item, allow_empty_signature)) {
                *success = false;
                return index;
            }
            break;
        case ITEM_TOOL_CALL: {
            auto tool = encode_tool_use(item);
            if (!tool) {
                *success = false;
                return index;
            }
            content.push_back(std::move(*tool));
            break;
        }
        default:
            break;
        }
    }

    if (!content.empty())
        messages->push_back({.role = "assistant", .content = std::move(content)});
    return index;
}

static size_t append_tool_results(std::vector<detail::body_message> *messages,
                                  const struct item *items, size_t index, size_t n_items,
                                  int image_input, bool *success)
{
    std::vector<raw_json> content;
    while (index < n_items && items[index].kind == ITEM_TOOL_RESULT) {
        auto result = encode_tool_result(&items[index++], image_input);
        if (!result) {
            *success = false;
            return index;
        }
        content.push_back(std::move(*result));
    }
    messages->push_back({.role = "user", .content = std::move(content)});
    return index;
}

static void parse_model_efforts(const std::optional<raw_json> &source, struct effort_set *out)
{
    if (!source)
        return;

    auto effort = hax::json::parse_value(source->str, RESPONSE_JSON_OPTIONS);
    if (!effort || !effort->is_object())
        return;

    const hax::json::value *supported = effort->find("supported");
    if (supported && supported->is_boolean() && !supported->boolean_value()) {
        out->known = 1;
        return;
    }

    static const char *const LADDER[] = {"low", "medium", "high", "xhigh", "max"};
    for (const char *level : LADDER) {
        const hax::json::value *entry = effort->find(level);
        const hax::json::value *entry_supported = entry ? entry->find("supported") : NULL;
        if (entry && entry->is_object() && entry_supported && entry_supported->is_boolean() &&
            entry_supported->boolean_value())
            effort_set_add(out, level);
    }

    for (const auto &member : effort->object_items()) {
        if (member.first == "supported" || !member.second.is_object())
            continue;
        const hax::json::value *entry_supported = member.second.find("supported");
        if (entry_supported && entry_supported->is_boolean() && entry_supported->boolean_value())
            effort_set_add(out, member.first.c_str());
    }
}

static void parse_model_value(const detail::raw_model_entry &entry, struct model_info *out)
{
    auto context = decoded_long(entry.max_input_tokens);
    if (context && *context > 0)
        out->context = *context;

    auto max_output = decoded_long(entry.max_tokens);
    if (max_output && *max_output > 0)
        out->max_output = *max_output;

    auto capabilities = decode<detail::raw_capabilities>(entry.capabilities, RESPONSE_JSON_OPTIONS);
    if (!capabilities)
        return;

    auto image = decode<detail::raw_capability>(capabilities->image_input, RESPONSE_JSON_OPTIONS);
    if (image) {
        auto supported = decoded_bool(image->supported);
        if (supported)
            out->image_input = *supported ? PROVIDER_CAP_YES : PROVIDER_CAP_NO;
    }
    parse_model_efforts(capabilities->effort, &out->efforts);
}

} // namespace

namespace hax::anthropic_json
{

std::optional<parsed_event> parse_event(std::string_view input)
{
    auto decoded = hax::json::parse<detail::raw_event>(input, RESPONSE_JSON_OPTIONS);
    if (!decoded)
        return std::nullopt;

    parsed_event result;
    result.type = decoded_string(decoded->type);
    result.index = decoded_long(decoded->index);

    if (auto content =
            decode<detail::raw_content_block>(decoded->content_block, RESPONSE_JSON_OPTIONS)) {
        result.content_block = parsed_content_block{
            .type = decoded_string(content->type),
            .id = decoded_string(content->id),
            .name = decoded_string(content->name),
            .data = decoded_string(content->data),
        };
    }

    if (auto delta = decode<detail::raw_delta>(decoded->delta, RESPONSE_JSON_OPTIONS)) {
        result.delta = parsed_delta{
            .type = decoded_string(delta->type),
            .text = decoded_string(delta->text),
            .thinking = decoded_string(delta->thinking),
            .signature = decoded_string(delta->signature),
            .partial_json = decoded_string(delta->partial_json),
            .stop_reason = decoded_string(delta->stop_reason),
        };
    }

    if (auto message = decode<detail::raw_message>(decoded->message, RESPONSE_JSON_OPTIONS)) {
        parsed_message parsed{};
        parsed.id = decoded_string(message->id);
        parsed.model = decoded_string(message->model);
        if (auto usage = decode<detail::raw_usage>(message->usage, RESPONSE_JSON_OPTIONS))
            parsed.usage = parsed_usage_from(*usage);
        result.message = std::move(parsed);
    }

    if (auto usage = decode<detail::raw_usage>(decoded->usage, RESPONSE_JSON_OPTIONS))
        result.usage = parsed_usage_from(*usage);

    if (auto error = decode<detail::raw_error>(decoded->error, RESPONSE_JSON_OPTIONS))
        result.error = parsed_error{.message = decoded_string(error->message)};

    return result;
}

std::optional<std::string> build_messages(const struct item *items, size_t n_items,
                                          const char *current_provider, const char *current_model,
                                          int allow_empty_signature, int image_input)
{
    std::vector<detail::body_message> messages;
    size_t index = 0;
    bool success = true;
    while (index < n_items) {
        switch (items[index].kind) {
        case ITEM_USER_MESSAGE: {
            auto text = encode_text_block(items[index].text ? items[index].text : "");
            if (!text)
                return std::nullopt;
            messages.push_back({.role = "user", .content = {std::move(*text)}});
            index++;
            break;
        }
        case ITEM_ASSISTANT_MESSAGE:
        case ITEM_TOOL_CALL:
        case ITEM_REASONING:
            index = append_assistant_message(&messages, items, index, n_items, current_provider,
                                             current_model, allow_empty_signature, &success);
            if (!success)
                return std::nullopt;
            break;
        case ITEM_TOOL_RESULT:
            index = append_tool_results(&messages, items, index, n_items, image_input, &success);
            if (!success)
                return std::nullopt;
            break;
        case ITEM_TURN_BOUNDARY:
        case ITEM_TURN_USAGE:
            index++;
            break;
        }
    }

    auto encoded = hax::json::serialize_escaped(messages, REQUEST_JSON_OPTIONS);
    if (!encoded)
        return std::nullopt;
    return std::move(*encoded);
}

std::optional<std::string> encode_thinking_item(std::string_view thinking,
                                                std::string_view signature)
{
    auto encoded = encode_raw(detail::reasoning_thinking{
        .type = "thinking",
        .thinking = std::string(thinking),
        .signature = std::string(signature),
    });
    if (!encoded)
        return std::nullopt;
    return std::move(encoded->str);
}

std::optional<std::string> encode_redacted_thinking_item(std::string_view data)
{
    auto encoded = encode_raw(detail::reasoning_redacted{
        .type = "redacted_thinking",
        .data = std::string(data),
    });
    if (!encoded)
        return std::nullopt;
    return std::move(encoded->str);
}

void parse_model(std::string_view input, struct model_info *info)
{
    if (!info)
        return;
    auto entry = hax::json::parse<detail::raw_model_entry>(input, RESPONSE_JSON_OPTIONS);
    if (entry)
        parse_model_value(*entry, info);
}

void parse_model_probe_response(std::string_view input, std::string_view model,
                                struct model_info *info)
{
    if (!info)
        return;
    auto root = hax::json::parse<detail::raw_models_response>(input, RESPONSE_JSON_OPTIONS);
    if (!root)
        return;

    auto entries = decoded_array(root->data);
    if (!entries)
        return;
    for (const detail::raw_json &value : *entries) {
        auto entry = decode<detail::raw_model_entry>(value, RESPONSE_JSON_OPTIONS);
        if (!entry)
            continue;
        auto id = decoded_string(entry->id);
        if (id && *id == model) {
            parse_model_value(*entry, info);
            return;
        }
    }
}

std::optional<parsed_model_page> parse_model_page(std::string_view input)
{
    auto decoded = hax::json::parse<detail::raw_models_page>(input, RESPONSE_JSON_OPTIONS);
    if (!decoded)
        return std::nullopt;

    parsed_model_page result{};
    result.data_kind = starts_with_null(decoded->data.str) ? model_page_data_kind::null_value
                                                           : model_page_data_kind::unsupported;
    if (auto entries =
            decode<std::vector<detail::raw_json>>(decoded->data, RESPONSE_JSON_OPTIONS)) {
        result.data_kind = model_page_data_kind::array;
        result.entries.reserve(entries->size());
        for (detail::raw_json &value : *entries) {
            auto model = decode<detail::raw_model_entry>(value, RESPONSE_JSON_OPTIONS);
            parsed_model_entry entry{};
            entry.json = std::move(value.str);
            if (model)
                entry.id = decoded_string(model->id);
            result.entries.push_back(std::move(entry));
        }
    }
    result.has_more = decoded_bool(decoded->has_more).value_or(false);
    result.last_id = decoded_string(decoded->last_id);
    return result;
}

} // namespace hax::anthropic_json
