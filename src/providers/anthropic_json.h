/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_ANTHROPIC_JSON_H
#define HAX_PROVIDERS_ANTHROPIC_JSON_H

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct item;
struct effort_set;
struct model_info;

namespace hax::anthropic_json
{

struct parsed_usage {
    std::optional<long> input_tokens;
    std::optional<long> cache_read_input_tokens;
    std::optional<long> cache_creation_input_tokens;
    std::optional<long> cache_write_1h_input_tokens;
    std::optional<long> output_tokens;
};

struct parsed_content_block {
    std::optional<std::string> type;
    std::optional<std::string> id;
    std::optional<std::string> name;
    std::optional<std::string> data;
};

struct parsed_delta {
    std::optional<std::string> type;
    std::optional<std::string> text;
    std::optional<std::string> thinking;
    std::optional<std::string> signature;
    std::optional<std::string> partial_json;
    std::optional<std::string> stop_reason;
};

struct parsed_message {
    std::optional<std::string> id;
    std::optional<std::string> model;
    std::optional<parsed_usage> usage;
};

struct parsed_error {
    std::optional<std::string> message;
};

/* Optional members are decoded independently so one provider extension with an unexpected shape
 * does not discard the rest of an otherwise usable SSE event. */
struct parsed_event {
    std::optional<std::string> type;
    std::optional<long> index;
    std::optional<parsed_content_block> content_block;
    std::optional<parsed_delta> delta;
    std::optional<parsed_message> message;
    std::optional<parsed_usage> usage;
    std::optional<parsed_error> error;
};

/* Decode one Anthropic Messages SSE payload. Unknown keys and malformed optional members are
 * ignored according to the provider compatibility policy. */
std::optional<parsed_event> parse_event(std::string_view input);

/* Build the Messages content array through private typed wire DTOs. The returned JSON document is
 * owned by the caller; a missing value means that DTO serialization failed. */
std::optional<std::string> build_messages(const struct item *items, size_t n_items,
                                          const char *current_provider, const char *current_model,
                                          int allow_empty_signature, int image_input);

/* Encode one opaque thinking block. The returned string is owned by the caller; a missing value
 * means that serialization failed. */
std::optional<std::string> encode_thinking_item(std::string_view thinking,
                                                std::string_view signature);

/* Encode one opaque redacted-thinking block. The returned string is owned by the caller; a missing
 * value means that serialization failed. */
std::optional<std::string> encode_redacted_thinking_item(std::string_view data);

struct parsed_model_entry {
    std::optional<std::string> id;
    std::string json; /* owns the raw model-entry JSON for a later metadata parse */
};

enum class model_page_data_kind {
    unsupported,
    null_value,
    array,
};

/* A parsed /models page. `data_kind` distinguishes a model array, JSON null, and other valid
 * data. */
struct parsed_model_page {
    model_page_data_kind data_kind;
    bool has_more;
    std::optional<std::string> last_id;
    std::vector<parsed_model_entry> entries;
};

/* Apply fields from one model-entry JSON object to initialized metadata. The input is borrowed;
 * malformed or absent fields leave the corresponding metadata unchanged. */
void parse_model(std::string_view input, struct model_info *info);

/* Find `model` in one /models response and apply its fields to initialized metadata. The input is
 * borrowed; absent or malformed data leaves the corresponding metadata unchanged. */
void parse_model_probe_response(std::string_view input, std::string_view model,
                                struct model_info *info);

/* Parse one complete /models response. The returned page owns its strings; a missing value means
 * the response was not a decodable model-page object. */
std::optional<parsed_model_page> parse_model_page(std::string_view input);

} // namespace hax::anthropic_json

#endif /* HAX_PROVIDERS_ANTHROPIC_JSON_H */
