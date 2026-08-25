/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_CODEX_JSON_H
#define HAX_PROVIDERS_CODEX_JSON_H

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct effort_set;
struct model_info;

namespace hax::codex_json
{

struct parsed_model_entry {
    std::optional<std::string> slug;
    std::string json; /* owns the raw catalog entry for a later metadata parse */
};

enum class model_page_models_kind {
    unsupported,
    null_value,
    array,
};

/* Individual entries remain raw so the caller can distinguish an empty catalog from entries
 * without slugs while applying metadata through the same adapter. */
struct parsed_model_page {
    model_page_models_kind models_kind = model_page_models_kind::unsupported;
    std::vector<parsed_model_entry> entries;
};

struct parsed_usage_window {
    std::optional<double> used_percent;
    std::optional<double> reset_at;
    std::optional<long> limit_window_seconds;
};

struct parsed_usage {
    std::optional<std::string> plan_type;
    bool rate_limit_nonnull = false;
    std::optional<parsed_usage_window> primary_window;
    std::optional<parsed_usage_window> secondary_window;
};

/* Codex catalog responses accept forward-compatible members but require a usable `models` array. */
std::optional<parsed_model_page> parse_model_page(std::string_view input);

/* Apply catalog metadata to initialized model state. Malformed optional members leave existing
 * unknown values unchanged. */
void parse_model(std::string_view input, struct model_info *model);
void parse_model_efforts(std::string_view input, struct effort_set *efforts);
bool model_is_hidden(std::string_view input);

/* Find one model by slug in a catalog response and apply its metadata. */
void parse_model_probe_response(std::string_view input, std::string_view model,
                                struct model_info *info);

/* Decode one usage response. The returned object distinguishes a non-null rate_limit from an
 * absent or JSON-null one; individual windows preserve the old unrecognized-shape behavior. On
 * failure, `error` receives an owned parser diagnostic when non-NULL. */
std::optional<parsed_usage> parse_usage(std::string_view input, std::string *error = nullptr);

} // namespace hax::codex_json

#endif /* HAX_PROVIDERS_CODEX_JSON_H */
