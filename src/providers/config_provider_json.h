/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_CONFIG_PROVIDER_JSON_H
#define HAX_PROVIDERS_CONFIG_PROVIDER_JSON_H

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hax::config_provider_json
{

enum class model_page_data_kind {
    unsupported,
    null_value,
    array,
};

struct parsed_model_entry {
    std::optional<std::string> id;
    std::string json; /* owns the raw model entry for a later metadata parse */
};

/* The generic configured-provider endpoint uses the OpenAI-compatible /models envelope. Keep the
 * data kind distinct so a reachable server with `data: null` remains an empty result, while a
 * different response shape is reported as a protocol error by the caller. */
struct parsed_model_page {
    model_page_data_kind data_kind = model_page_data_kind::unsupported;
    std::vector<parsed_model_entry> entries;
};

/* Decode one configured-provider /models response. Unknown members are accepted and each model
 * entry retains its opaque JSON for the provider's optional metadata callback. */
std::optional<parsed_model_page> parse_model_page(std::string_view input);

} // namespace hax::config_provider_json

#endif /* HAX_PROVIDERS_CONFIG_PROVIDER_JSON_H */
