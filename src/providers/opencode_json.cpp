/* SPDX-License-Identifier: MIT */
#include "providers/opencode_json.h"

#include <cstdio>
#include <cstring>
#include <utility>

#include "json.h"

namespace
{

/* "2026-08-21T21:14:51.969Z" -> epoch seconds, or -1 on any other shape. Fractional seconds are
 * ignored. Only UTC is accepted: a nonzero offset would shift the reset time silently. */
static time_t parse_utc_timestamp(const char *text)
{
    int year, month, day, hour, minute, second, parsed_length = 0;
    if (std::sscanf(text, "%4d-%2d-%2dT%2d:%2d:%2d%n", &year, &month, &day, &hour, &minute, &second,
                    &parsed_length) != 6)
        return -1;
    if (year < 1970 || month < 1 || month > 12 || hour < 0 || hour > 23 || minute < 0 ||
        minute > 59 || second < 0 || second > 60)
        return -1;

    /* Enforce real calendar days; the day-count formula below would silently normalize an
     * overflow like Feb 31 into the next month. */
    static const int MONTH_DAYS[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const int leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    if (day < 1 || day > MONTH_DAYS[month - 1] + (month == 2 && leap))
        return -1;

    const char *rest = text + parsed_length;
    if (*rest == '.') {
        if (rest[1] < '0' || rest[1] > '9')
            return -1;
        for (rest++; *rest >= '0' && *rest <= '9'; rest++)
            ;
    }
    if (std::strcmp(rest, "Z") != 0)
        return -1;

    /* Civil date -> days since 1970-01-01 (Hinnant's days_from_civil). The library converters
     * are out of reach: timegm (BSD, standard only since C23) is hidden by -std=c11 plus our
     * feature macros, mktime works in local time, and curl_getdate does not parse ISO 8601. */
    const int shifted_year = year - (month <= 2);
    const int era = shifted_year / 400;
    const int year_of_era = shifted_year - era * 400;
    const int day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    const long long days = (long long)era * 146097 + day_of_era - 719468;
    return (time_t)(days * 86400 + hour * 3600 + minute * 60 + second);
}

} // namespace

namespace hax::opencode_json
{

std::optional<parsed_usage> parse_usage(std::string_view input, std::string *error)
{
    auto decoded =
        hax::json::parse_value(input, {.source = "OpenCode usage response", .max_input_bytes = 0});
    if (!decoded) {
        if (error)
            *error = decoded.error().message;
        return std::nullopt;
    }

    if (!decoded->is_object()) {
        if (error)
            *error = "root must be a JSON object";
        return std::nullopt;
    }

    parsed_usage result;
    const hax::json::value *usage = decoded->find("usage");
    if (!usage || !usage->is_object())
        return result;

    for (const auto &member : usage->object_items()) {
        const hax::json::value &window = member.second;
        if (!window.is_object())
            continue;

        const hax::json::value *percent = window.find("percent");
        const hax::json::value *resets_at = window.find("resetsAt");
        if (!percent || !percent->is_number() || !resets_at || !resets_at->is_string())
            continue;

        const time_t reset_at = parse_utc_timestamp(resets_at->string_value().c_str());
        if (reset_at < 0)
            continue;

        parsed_usage_window parsed = {
            .label = member.first,
            .used_percent = percent->real_value(),
            .reset_at = reset_at,
            .note = std::nullopt,
        };
        const hax::json::value *status = window.find("status");
        if (status && status->is_string() && status->string_value() != "ok")
            parsed.note = status->string_value();
        result.windows.push_back(std::move(parsed));
    }
    return result;
}

} // namespace hax::opencode_json
