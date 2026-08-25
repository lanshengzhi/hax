/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_RESPONSES_JSON_H
#define HAX_PROVIDERS_RESPONSES_JSON_H

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

struct item;

namespace hax::responses_json
{

struct parsed_usage {
    std::optional<long> input_tokens;
    std::optional<long> output_tokens;
    std::optional<long> cached_tokens;
};

struct parsed_response {
    std::optional<std::string> id;
    std::optional<std::string> model;
    std::optional<parsed_usage> usage;
    std::optional<std::string> error_message;
    std::optional<std::string> incomplete_reason;
};

struct parsed_output_item {
    std::optional<std::string> type;
    std::optional<std::string> id;
    std::optional<std::string> call_id;
    std::optional<std::string> name;
    std::optional<std::string> arguments;
    std::optional<std::string> summary_json;
    /* Present and non-null encrypted_content, kept as its original JSON rather than assuming a
     * provider type for opaque state. */
    std::optional<std::string> encrypted_content_json;
};

/* The event view keeps optional members independent. One malformed extension field must not
 * discard an otherwise usable Responses event. Index-presence flags preserve the wire's choice
 * between summary_index and content_index when the selected member has the wrong type. */
struct parsed_event {
    std::optional<std::string> type;
    std::optional<parsed_output_item> item;
    std::optional<std::string> delta;
    std::optional<std::string> item_id;
    bool summary_index_present = false;
    std::optional<long> summary_index;
    bool content_index_present = false;
    std::optional<long> content_index;
    std::optional<parsed_response> response;
    std::optional<std::string> message;
    std::optional<std::string> code;
};

/* Decode one Responses SSE payload. Unknown keys are accepted, while typed optional members that
 * cannot be decoded are ignored independently. */
std::optional<parsed_event> parse_event(std::string_view input);

/* Encode one completed reasoning item for replay. The returned JSON is owned by the caller; a
 * missing value means that encrypted_content was absent or serialization failed. */
std::optional<std::string> encode_reasoning_item(const parsed_output_item &item);

/* Build the Responses input array through private typed wire DTOs. Opaque reasoning JSON is
 * validated and carried as raw provider state. The returned JSON is owned by the caller. */
std::optional<std::string> build_input_items(const struct item *items, size_t n_items,
                                             const char *provider, const char *model,
                                             int image_input);

} // namespace hax::responses_json

#endif /* HAX_PROVIDERS_RESPONSES_JSON_H */
