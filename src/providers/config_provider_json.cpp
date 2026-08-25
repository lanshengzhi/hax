/* SPDX-License-Identifier: MIT */
#include "providers/config_provider_json.h"

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "json.h"

namespace hax::config_provider_json::detail
{

using raw_json = glz::raw_json;
using raw_members = std::map<std::string, raw_json>;

struct raw_model_entry {
    std::optional<raw_json> id;
};

} // namespace hax::config_provider_json::detail

#define HAX_CONFIG_PROVIDER_META(TYPE, ...)                                                        \
    template <> struct glz::meta<hax::config_provider_json::detail::TYPE> {                        \
        using T = hax::config_provider_json::detail::TYPE;                                         \
        static constexpr auto value = glz::object(__VA_ARGS__);                                    \
    }

HAX_CONFIG_PROVIDER_META(raw_model_entry, "id", &T::id);

#undef HAX_CONFIG_PROVIDER_META

namespace
{

namespace detail = hax::config_provider_json::detail;
using detail::raw_json;

constexpr hax::json::options CONFIG_PROVIDER_JSON_OPTIONS = {
    .source = "configured-provider response",
    .max_input_bytes = 0,
    .allow_unknown_keys = true,
};

template <typename T> std::optional<T> decode(const raw_json &source)
{
    auto value = hax::json::parse<T>(source.str, CONFIG_PROVIDER_JSON_OPTIONS);
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

static bool is_json_null(const raw_json &source)
{
    const size_t first = first_json_byte(source.str);
    return source.str.substr(first, 4) == "null";
}

} // namespace

namespace hax::config_provider_json
{

std::optional<parsed_model_page> parse_model_page(std::string_view input)
{
    if (!hax::json::validate(input, CONFIG_PROVIDER_JSON_OPTIONS))
        return std::nullopt;

    if (!starts_with_object(input))
        return std::nullopt;

    parsed_model_page result;

    auto root = hax::json::parse<detail::raw_members>(input, CONFIG_PROVIDER_JSON_OPTIONS);
    if (!root)
        return result;
    auto data = root->find("data");
    if (data == root->end())
        return result;
    if (is_json_null(data->second)) {
        result.data_kind = model_page_data_kind::null_value;
        return result;
    }

    auto entries = decode<std::vector<raw_json>>(data->second);
    if (!entries)
        return result;

    result.data_kind = model_page_data_kind::array;
    result.entries.reserve(entries->size());
    for (const raw_json &value : *entries) {
        auto entry = decode<detail::raw_model_entry>(value);
        result.entries.push_back({
            .id = entry ? decoded_string(entry->id) : std::nullopt,
            .json = value.str,
        });
    }
    return result;
}

} // namespace hax::config_provider_json
