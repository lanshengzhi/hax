/* SPDX-License-Identifier: MIT */
#include "providers/codex_json.h"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "effort.h"
#include "json.h"
#include "model_meta.h"
#include "provider.h"
#include "util.h"

namespace hax::codex_json::detail
{

using raw_json = glz::raw_json;
using raw_members = std::map<std::string, raw_json>;

struct raw_model_entry {
    std::optional<raw_json> slug;
    std::optional<raw_json> visibility;
    std::optional<raw_json> context_window;
    std::optional<raw_json> max_context_window;
    std::optional<raw_json> input_modalities;
    std::optional<raw_json> description;
    std::optional<raw_json> supported_reasoning_levels;
};

struct raw_effort_level {
    std::optional<raw_json> effort;
};

struct raw_usage {
    std::optional<raw_json> plan_type;
    std::optional<raw_json> rate_limit;
};

struct raw_rate_limit {
    std::optional<raw_json> primary_window;
    std::optional<raw_json> secondary_window;
};

struct raw_usage_window {
    std::optional<raw_json> used_percent;
    std::optional<raw_json> reset_at;
    std::optional<raw_json> limit_window_seconds;
};

} // namespace hax::codex_json::detail

#define HAX_CODEX_META(TYPE, ...)                                                                  \
    template <> struct glz::meta<hax::codex_json::detail::TYPE> {                                  \
        using T = hax::codex_json::detail::TYPE;                                                   \
        static constexpr auto value = glz::object(__VA_ARGS__);                                    \
    }

HAX_CODEX_META(raw_model_entry, "slug", &T::slug, "visibility", &T::visibility, "context_window",
               &T::context_window, "max_context_window", &T::max_context_window, "input_modalities",
               &T::input_modalities, "description", &T::description, "supported_reasoning_levels",
               &T::supported_reasoning_levels);
HAX_CODEX_META(raw_effort_level, "effort", &T::effort);
HAX_CODEX_META(raw_usage, "plan_type", &T::plan_type, "rate_limit", &T::rate_limit);
HAX_CODEX_META(raw_rate_limit, "primary_window", &T::primary_window, "secondary_window",
               &T::secondary_window);
HAX_CODEX_META(raw_usage_window, "used_percent", &T::used_percent, "reset_at", &T::reset_at,
               "limit_window_seconds", &T::limit_window_seconds);

#undef HAX_CODEX_META

namespace
{

namespace detail = hax::codex_json::detail;
using detail::raw_json;

constexpr hax::json::options CODEX_JSON_OPTIONS = {
    .source = "Codex response",
    .max_input_bytes = 0,
    .allow_unknown_keys = true,
};

template <typename T> std::optional<T> decode(const raw_json &source)
{
    auto value = hax::json::parse<T>(source.str, CODEX_JSON_OPTIONS);
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

static std::optional<std::vector<raw_json>> decoded_array(const std::optional<raw_json> &source)
{
    return decode<std::vector<raw_json>>(source);
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

static bool starts_with_array(std::string_view input)
{
    const size_t first = first_json_byte(input);
    return first < input.size() && input[first] == '[';
}

static bool is_json_null(const raw_json &source)
{
    return source.str.substr(first_json_byte(source.str), 4) == "null";
}

static void parse_efforts(const detail::raw_model_entry &entry, struct effort_set *efforts)
{
    if (!efforts)
        return;

    auto levels = decoded_array(entry.supported_reasoning_levels);
    if (!levels)
        return;

    efforts->known = 1;
    if (levels->empty())
        return;

    effort_set_add(efforts, "none");
    for (const raw_json &level : *levels) {
        auto effort = decode<std::string>(level);
        if (!effort) {
            auto object = decode<detail::raw_effort_level>(level);
            if (object)
                effort = decoded_string(object->effort);
        }
        if (effort && *effort != "ultra")
            effort_set_add(efforts, effort->c_str());
    }
}

static void parse_model_value(const detail::raw_model_entry &entry, struct model_info *model)
{
    auto context = decoded_long(entry.context_window);
    if (!context || *context <= 0)
        context = decoded_long(entry.max_context_window);
    if (context && *context > 0)
        model->context = *context;

    auto modalities = decoded_array(entry.input_modalities);
    if (modalities) {
        model->image_input = PROVIDER_CAP_NO;
        for (const raw_json &modality : *modalities) {
            auto name = decode<std::string>(modality);
            if (name && *name == "image")
                model->image_input = PROVIDER_CAP_YES;
        }
    }

    auto description = decoded_string(entry.description);
    if (description && !description->empty())
        model->description = xstrdup(description->c_str());

    parse_efforts(entry, &model->efforts);
}

static std::optional<hax::codex_json::parsed_usage_window>
parse_usage_window(const std::optional<raw_json> &source)
{
    if (!source || is_json_null(*source))
        return std::nullopt;

    auto window = decode<detail::raw_usage_window>(source);
    if (!window)
        return hax::codex_json::parsed_usage_window{};

    return hax::codex_json::parsed_usage_window{
        .used_percent = decoded_number(window->used_percent),
        .reset_at = decoded_number(window->reset_at),
        .limit_window_seconds = decoded_long(window->limit_window_seconds),
    };
}

} // namespace

namespace hax::codex_json
{

std::optional<parsed_model_page> parse_model_page(std::string_view input)
{
    if (!hax::json::validate(input, CODEX_JSON_OPTIONS))
        return std::nullopt;

    parsed_model_page result;
    if (!starts_with_object(input)) {
        if (starts_with_array(input))
            return result;
        return std::nullopt;
    }

    auto members = hax::json::parse<detail::raw_members>(input, CODEX_JSON_OPTIONS);
    if (!members)
        return result;
    auto models_value = members->find("models");
    if (models_value == members->end())
        return result;
    if (is_json_null(models_value->second)) {
        result.models_kind = model_page_models_kind::null_value;
        return result;
    }

    auto models = decode<std::vector<raw_json>>(models_value->second);
    if (!models)
        return result;

    result.models_kind = model_page_models_kind::array;
    result.entries.reserve(models->size());
    for (raw_json &value : *models) {
        auto model = decode<detail::raw_model_entry>(value);
        parsed_model_entry entry = {.slug = std::nullopt, .json = value.str};
        if (model)
            entry.slug = decoded_string(model->slug);
        result.entries.push_back(std::move(entry));
    }
    return result;
}

void parse_model(std::string_view input, struct model_info *model)
{
    if (!model)
        return;
    auto entry = hax::json::parse<detail::raw_model_entry>(input, CODEX_JSON_OPTIONS);
    if (entry)
        parse_model_value(*entry, model);
}

void parse_model_efforts(std::string_view input, struct effort_set *efforts)
{
    if (!efforts)
        return;
    auto entry = hax::json::parse<detail::raw_model_entry>(input, CODEX_JSON_OPTIONS);
    if (entry)
        parse_efforts(*entry, efforts);
}

bool model_is_hidden(std::string_view input)
{
    auto entry = hax::json::parse<detail::raw_model_entry>(input, CODEX_JSON_OPTIONS);
    if (!entry)
        return false;
    auto visibility = decoded_string(entry->visibility);
    return visibility && *visibility == "hide";
}

void parse_model_probe_response(std::string_view input, std::string_view model,
                                struct model_info *info)
{
    if (!info)
        return;
    auto page = parse_model_page(input);
    if (!page || page->models_kind != model_page_models_kind::array)
        return;

    for (const parsed_model_entry &entry : page->entries) {
        if (entry.slug && *entry.slug == model) {
            parse_model(entry.json, info);
            return;
        }
    }
}

std::optional<parsed_usage> parse_usage(std::string_view input, std::string *error)
{
    auto valid = hax::json::validate(input, CODEX_JSON_OPTIONS);
    if (!valid) {
        if (error)
            *error = valid.error().message;
        return std::nullopt;
    }

    parsed_usage result;
    if (!starts_with_object(input)) {
        if (starts_with_array(input))
            return result;
        if (error)
            *error = "root must be a JSON object or array";
        return std::nullopt;
    }

    auto decoded = hax::json::parse<detail::raw_usage>(input, CODEX_JSON_OPTIONS);
    if (!decoded) {
        if (error)
            *error = decoded.error().message;
        return std::nullopt;
    }

    result.plan_type = decoded_string(decoded->plan_type);
    if (!decoded->rate_limit || is_json_null(*decoded->rate_limit))
        return result;

    result.rate_limit_nonnull = true;
    if (auto rate_limit = decode<detail::raw_rate_limit>(decoded->rate_limit)) {
        result.primary_window = parse_usage_window(rate_limit->primary_window);
        result.secondary_window = parse_usage_window(rate_limit->secondary_window);
    }
    return result;
}

} // namespace hax::codex_json
