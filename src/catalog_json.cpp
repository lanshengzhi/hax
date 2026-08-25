/* SPDX-License-Identifier: MIT */
#include "catalog_json.h"

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <strings.h>
#include <utility>
#include <vector>

#include "catalog.h"
#include "effort.h"
#include "json.h"
#include "util.h"

namespace hax::catalog_json::detail
{

using raw_json = glz::raw_json;

struct raw_provider {
    std::optional<raw_json> models;
    std::optional<raw_json> npm;
};

struct raw_model {
    std::optional<raw_json> api;
    std::optional<raw_json> cost;
    std::optional<raw_json> limit;
    std::optional<raw_json> reasoning;
    std::optional<raw_json> reasoning_options;
    std::optional<raw_json> interleaved;
    std::optional<raw_json> modalities;
    std::optional<raw_json> provider;
};

struct raw_cost {
    std::optional<raw_json> input;
    std::optional<raw_json> output;
    std::optional<raw_json> cache_read;
    std::optional<raw_json> cache_write;
    std::optional<raw_json> cache_write_1h;
    std::optional<raw_json> tiers;
};

struct raw_limit {
    std::optional<raw_json> context;
    std::optional<raw_json> output;
};

struct raw_tier {
    std::optional<raw_json> input;
    std::optional<raw_json> output;
    std::optional<raw_json> cache_read;
    std::optional<raw_json> cache_write;
    std::optional<raw_json> cache_write_1h;
    std::optional<raw_json> tier;
};

struct raw_selector {
    std::optional<raw_json> type;
    std::optional<raw_json> size;
};

struct raw_reasoning_option {
    std::optional<raw_json> type;
    std::optional<raw_json> values;
};

struct raw_modalities {
    std::optional<raw_json> input;
};

struct raw_model_provider {
    std::optional<raw_json> npm;
};

struct raw_interleaved {
    std::optional<raw_json> field;
};

} // namespace hax::catalog_json::detail

#define HAX_CATALOG_JSON_META(TYPE, ...)                                                           \
    template <> struct glz::meta<hax::catalog_json::detail::TYPE> {                                \
        using T = hax::catalog_json::detail::TYPE;                                                 \
        static constexpr auto value = glz::object(__VA_ARGS__);                                    \
    }

HAX_CATALOG_JSON_META(raw_provider, "models", &T::models, "npm", &T::npm);
HAX_CATALOG_JSON_META(raw_model, "api", &T::api, "cost", &T::cost, "limit", &T::limit, "reasoning",
                      &T::reasoning, "reasoning_options", &T::reasoning_options, "interleaved",
                      &T::interleaved, "modalities", &T::modalities, "provider", &T::provider);
HAX_CATALOG_JSON_META(raw_cost, "input", &T::input, "output", &T::output, "cache_read",
                      &T::cache_read, "cache_write", &T::cache_write, "cache_write_1h",
                      &T::cache_write_1h, "tiers", &T::tiers);
HAX_CATALOG_JSON_META(raw_limit, "context", &T::context, "output", &T::output);
HAX_CATALOG_JSON_META(raw_tier, "input", &T::input, "output", &T::output, "cache_read",
                      &T::cache_read, "cache_write", &T::cache_write, "cache_write_1h",
                      &T::cache_write_1h, "tier", &T::tier);
HAX_CATALOG_JSON_META(raw_selector, "type", &T::type, "size", &T::size);
HAX_CATALOG_JSON_META(raw_reasoning_option, "type", &T::type, "values", &T::values);
HAX_CATALOG_JSON_META(raw_modalities, "input", &T::input);
HAX_CATALOG_JSON_META(raw_model_provider, "npm", &T::npm);
HAX_CATALOG_JSON_META(raw_interleaved, "field", &T::field);

#undef HAX_CATALOG_JSON_META

namespace
{

using hax::catalog_json::detail::raw_cost;
using hax::catalog_json::detail::raw_interleaved;
using hax::catalog_json::detail::raw_json;
using hax::catalog_json::detail::raw_limit;
using hax::catalog_json::detail::raw_modalities;
using hax::catalog_json::detail::raw_model;
using hax::catalog_json::detail::raw_model_provider;
using hax::catalog_json::detail::raw_provider;
using hax::catalog_json::detail::raw_reasoning_option;
using hax::catalog_json::detail::raw_selector;
using hax::catalog_json::detail::raw_tier;

constexpr hax::json::options JSON_OPTIONS = {
    .source = "model catalog",
    .max_input_bytes = 0,
    .allow_unknown_keys = true,
};

template <typename T> std::optional<T> decode(std::string_view input)
{
    auto parsed = hax::json::parse<T>(input, JSON_OPTIONS);
    if (!parsed)
        return std::nullopt;
    return std::move(*parsed);
}

template <typename T> std::optional<T> decode_raw(const std::optional<raw_json> &source)
{
    if (!source)
        return std::nullopt;
    return decode<T>(std::string_view(source->str));
}

static std::optional<std::string> decoded_string(const std::optional<raw_json> &source)
{
    return decode_raw<std::string>(source);
}

static std::optional<std::int64_t> decoded_integer(const std::optional<raw_json> &source)
{
    /* Exponent and decimal spellings remain real even when they are integral; preserve that
     * lexical distinction before integer conversion accepts them. */
    if (!source || source->str.find_first_of(".eE") != std::string::npos)
        return std::nullopt;
    return decode_raw<std::int64_t>(source);
}

static std::optional<double> decoded_number(const std::optional<raw_json> &source)
{
    if (!source)
        return std::nullopt;
    if (source->str.find_first_of(".eE") == std::string::npos) {
        auto integer = decoded_integer(source);
        return integer ? std::optional<double>(static_cast<double>(*integer)) : std::nullopt;
    }
    return decode_raw<double>(source);
}

static double member_rate(const std::optional<raw_json> &source)
{
    if (auto number = decoded_number(source); number && *number >= 0)
        return *number;

    auto text = decoded_string(source);
    if (!text || text->empty() || text->find('\0') != std::string::npos)
        return -1;
    errno = 0;
    char *end = NULL;
    double rate = std::strtod(text->c_str(), &end);
    if (end == text->c_str() || *end != '\0' || errno == ERANGE || !std::isfinite(rate) || rate < 0)
        return -1;
    return rate;
}

static long member_tokens(const std::optional<raw_json> &source)
{
    if (auto integer = decoded_integer(source);
        integer && *integer > 0 && *integer <= std::numeric_limits<long>::max())
        return static_cast<long>(*integer);

    auto text = decoded_string(source);
    return text && text->find('\0') == std::string::npos ? parse_size(text->c_str()) : 0;
}

static const char *canonical_api(const char *api)
{
    static const char *const DIALECTS[] = {"openai-completions", "openai-responses",
                                           "anthropic-messages"};
    for (const char *dialect : DIALECTS)
        if (strcasecmp(api, dialect) == 0)
            return dialect;
    return "unsupported";
}

static const char *npm_dialect(const char *npm)
{
    if (!npm)
        return NULL;
    if (strcmp(npm, "@ai-sdk/openai-compatible") == 0)
        return "openai-completions";
    if (strcmp(npm, "@ai-sdk/openai") == 0)
        return "openai-responses";
    if (strcmp(npm, "@ai-sdk/anthropic") == 0)
        return "anthropic-messages";
    return "unsupported";
}

static void fill_tiers(struct catalog_entry *entry, const std::optional<raw_json> &source)
{
    if (entry->tiers_declared)
        return;
    auto values = decode_raw<std::vector<raw_json>>(source);
    if (!values)
        return;

    entry->tiers_declared = 1; /* An empty array explicitly selects flat pricing. */
    for (const raw_json &value : *values) {
        if (entry->n_tiers >= CATALOG_TIERS_MAX)
            break;
        auto tier = decode<raw_tier>(std::string_view(value.str));
        if (!tier)
            continue;
        auto selector = decode_raw<raw_selector>(tier->tier);
        if (!selector)
            continue;
        auto type = decoded_string(selector->type);
        if (!type || *type != "context")
            continue;
        long threshold = member_tokens(selector->size);
        if (threshold <= 0)
            continue;

        struct catalog_tier *destination = &entry->tiers[entry->n_tiers++];
        destination->context_threshold = threshold;
        destination->cost_input = member_rate(tier->input);
        destination->cost_output = member_rate(tier->output);
        destination->cost_cache_read = member_rate(tier->cache_read);
        destination->cost_cache_write = member_rate(tier->cache_write);
        destination->cost_cache_write_1h = member_rate(tier->cache_write_1h);
    }
}

static void fill_efforts(struct catalog_entry *entry, const raw_model &model)
{
    if (entry->efforts.known)
        return;

    auto reasoning = decode_raw<bool>(model.reasoning);
    if (reasoning && !*reasoning) {
        entry->efforts.known = 1;
        return;
    }

    auto options = decode_raw<std::vector<raw_json>>(model.reasoning_options);
    if (!options)
        return;
    for (const raw_json &option_value : *options) {
        auto option = decode<raw_reasoning_option>(std::string_view(option_value.str));
        if (!option)
            continue;
        auto type = decoded_string(option->type);
        if (!type || *type != "effort")
            continue;
        auto values = decode_raw<std::vector<raw_json>>(option->values);
        if (!values)
            continue;
        for (const raw_json &value : *values) {
            auto text = decode<std::string>(std::string_view(value.str));
            if (text)
                effort_set_add(&entry->efforts, text->c_str());
        }
    }
    entry->efforts.known = 1;
}

static const char *canonical_interleaved_field(const std::optional<raw_json> &source, int *declared)
{
    auto disabled = decode_raw<bool>(source);
    if (disabled && !*disabled) {
        *declared = 1;
        return NULL;
    }

    auto field = decoded_string(source);
    if (!field) {
        auto object = decode_raw<raw_interleaved>(source);
        if (object)
            field = decoded_string(object->field);
    }
    if (!field)
        return NULL;

    static const char *const FIELDS[] = {"reasoning", "reasoning_content"};
    for (const char *candidate : FIELDS)
        if (strcasecmp(field->c_str(), candidate) == 0) {
            *declared = 1;
            return candidate;
        }
    *declared = 1;
    return NULL;
}

static void fill_entry_from_model(const raw_model &model, struct catalog_entry *entry)
{
    auto api = decoded_string(model.api);
    if (api && !entry->api)
        entry->api = canonical_api(api->c_str());

    auto cost = decode_raw<raw_cost>(model.cost);
    if (cost) {
        if (entry->cost_input < 0)
            entry->cost_input = member_rate(cost->input);
        if (entry->cost_output < 0)
            entry->cost_output = member_rate(cost->output);
        if (entry->cost_cache_read < 0)
            entry->cost_cache_read = member_rate(cost->cache_read);
        if (entry->cost_cache_write < 0)
            entry->cost_cache_write = member_rate(cost->cache_write);
        if (entry->cost_cache_write_1h < 0)
            entry->cost_cache_write_1h = member_rate(cost->cache_write_1h);
        fill_tiers(entry, cost->tiers);
    }

    auto limit = decode_raw<raw_limit>(model.limit);
    if (limit) {
        if (entry->context_window <= 0)
            entry->context_window = member_tokens(limit->context);
        if (entry->max_output <= 0)
            entry->max_output = member_tokens(limit->output);
    }

    fill_efforts(entry, model);
    if (!entry->interleaved_declared)
        entry->interleaved_field =
            canonical_interleaved_field(model.interleaved, &entry->interleaved_declared);

    if (entry->image_input == CATALOG_SUPPORT_UNKNOWN) {
        auto modalities = decode_raw<raw_modalities>(model.modalities);
        auto inputs =
            modalities ? decode_raw<std::vector<raw_json>>(modalities->input) : std::nullopt;
        if (inputs) {
            entry->image_input = CATALOG_SUPPORT_NO;
            for (const raw_json &input : *inputs) {
                auto name = decode<std::string>(std::string_view(input.str));
                if (name && *name == "image")
                    entry->image_input = CATALOG_SUPPORT_YES;
            }
        }
    }
}

template <typename Map>
static std::optional<raw_json> find_model(const Map &models, std::string_view model)
{
    auto found = models.find(std::string(model));
    return found == models.end() ? std::nullopt : std::optional<raw_json>(found->second);
}

static std::optional<raw_json> model_from_provider(const raw_provider &provider,
                                                   std::string_view model)
{
    auto models = decode_raw<std::map<std::string, raw_json>>(provider.models);
    return models ? find_model(*models, model) : std::nullopt;
}

static std::optional<raw_json> model_from_config(std::string_view provider_models,
                                                 std::string_view model)
{
    auto models = decode<std::map<std::string, raw_json>>(std::string_view(provider_models));
    return models ? find_model(*models, model) : std::nullopt;
}

static void fill_api_from_provider(const raw_provider &provider, const raw_model &model,
                                   struct catalog_entry *entry)
{
    if (entry->api)
        return;

    auto model_provider = decode_raw<raw_model_provider>(model.provider);
    auto npm = model_provider ? decoded_string(model_provider->npm) : std::nullopt;
    if (!npm)
        npm = decoded_string(provider.npm);
    entry->api = npm_dialect(npm ? npm->c_str() : NULL);
}

} // namespace

namespace hax::catalog_json
{

int fill_provider_model(std::string_view provider_json, std::string_view model,
                        struct catalog_entry *out)
{
    if (!out)
        return 0;
    auto provider = decode<detail::raw_provider>(std::string_view(provider_json));
    if (!provider)
        return 0;
    auto raw_model_value = model_from_provider(*provider, model);
    if (!raw_model_value)
        return 0;
    auto raw_model = decode<detail::raw_model>(std::string_view(raw_model_value->str));
    if (!raw_model)
        return 0;
    fill_entry_from_model(*raw_model, out);
    fill_api_from_provider(*provider, *raw_model, out);
    return 1;
}

int fill_config_model(std::string_view provider_models_json, std::string_view model,
                      struct catalog_entry *out)
{
    if (!out)
        return 0;
    auto raw_model_value = model_from_config(provider_models_json, model);
    if (!raw_model_value)
        return 0;
    auto raw_model = decode<detail::raw_model>(std::string_view(raw_model_value->str));
    if (!raw_model)
        return 0;
    fill_entry_from_model(*raw_model, out);
    return 1;
}

int provider_has_models(std::string_view provider_json)
{
    auto provider = decode<detail::raw_provider>(std::string_view(provider_json));
    if (!provider)
        return 0;
    auto models = decode_raw<std::map<std::string, detail::raw_json>>(provider->models);
    return models.has_value();
}

} // namespace hax::catalog_json
