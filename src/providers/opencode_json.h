/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_OPENCODE_JSON_H
#define HAX_PROVIDERS_OPENCODE_JSON_H

#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hax::opencode_json
{

struct parsed_usage_window {
    std::string label;
    double used_percent;
    time_t reset_at;
    std::optional<std::string> note;
};

struct parsed_usage {
    std::vector<parsed_usage_window> windows;
};

/* Decode OpenCode Go's /usage response. Window members are optional and malformed windows are
 * skipped, while unknown root and window members remain harmless. On invalid JSON, `error`
 * receives an owning parser diagnostic when non-NULL. */
std::optional<parsed_usage> parse_usage(std::string_view input, std::string *error = nullptr);

} // namespace hax::opencode_json

#endif /* HAX_PROVIDERS_OPENCODE_JSON_H */
