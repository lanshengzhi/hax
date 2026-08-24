/* SPDX-License-Identifier: MIT */
#include "session_item.h"

#include <climits>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "json.h"
#include "util.h"
#include "text/utf8.h"

namespace hax::session_item_detail
{

enum class wire_kind {
    user,
    assistant,
    tool_call,
    tool_result,
    reasoning,
    turn_boundary,
    turn_usage,
};

struct wire_usage {
    std::optional<long> input;
    std::optional<long> output;
    std::optional<long> cached;
    std::optional<long> cache_write;
    std::optional<long> cache_write_1h;
    std::optional<double> cost;
    std::optional<long> elapsed_ms;
    std::optional<long> in_tokens;
    std::optional<double> cost_in;
    std::optional<double> cost_cache_read;
    std::optional<double> cost_cache_write;
    std::optional<double> cost_out;
    std::optional<double> cost_total;
    std::optional<bool> cost_estimated;
    std::optional<std::string> provider_label;
    std::optional<std::string> model_label;
    std::optional<std::string> effort;
    std::optional<std::string> served_model;
    std::optional<std::string> route;
    std::optional<std::string> response_id;
};

struct wire_image {
    std::optional<std::string> mime;
    std::optional<std::string> data;
    std::optional<long> width;
    std::optional<long> height;
};

struct wire_item {
    std::optional<wire_kind> kind;
    std::optional<std::string> text;
    std::optional<std::string> call_id;
    std::optional<std::string> tool_name;
    std::optional<std::string> arguments;
    std::optional<std::string> output;
    std::optional<long long> output_hidden_tail;
    std::optional<std::string> reasoning_json;
    std::optional<std::string> reasoning_text;
    std::optional<std::string> provider;
    std::optional<std::string> model;
    std::optional<std::string> origin;
    std::optional<wire_usage> usage;
    /* Keep each array element self-contained so one malformed legacy image cannot reject siblings.
     * Valid elements are decoded into the typed wire_image DTO below. */
    std::optional<std::vector<glz::raw_json>> images;
};

} // namespace hax::session_item_detail

template <> struct glz::meta<hax::session_item_detail::wire_kind> {
    using T = hax::session_item_detail::wire_kind;
    static constexpr auto value =
        glz::enumerate(T::user, T::assistant, T::tool_call, T::tool_result, T::reasoning,
                       T::turn_boundary, T::turn_usage);
};

template <> struct glz::meta<hax::session_item_detail::wire_usage> {
    using T = hax::session_item_detail::wire_usage;
    static constexpr auto value = glz::object(
        "input", &T::input, "output", &T::output, "cached", &T::cached, "cache_write",
        &T::cache_write, "cache_write_1h", &T::cache_write_1h, "cost", &T::cost, "elapsed_ms",
        &T::elapsed_ms, "in_tokens", &T::in_tokens, "cost_in", &T::cost_in, "cost_cache_read",
        &T::cost_cache_read, "cost_cache_write", &T::cost_cache_write, "cost_out", &T::cost_out,
        "cost_total", &T::cost_total, "cost_estimated", &T::cost_estimated, "provider_label",
        &T::provider_label, "model_label", &T::model_label, "effort", &T::effort, "served_model",
        &T::served_model, "route", &T::route, "response_id", &T::response_id);
};

template <> struct glz::meta<hax::session_item_detail::wire_image> {
    using T = hax::session_item_detail::wire_image;
    static constexpr auto value =
        glz::object("mime", &T::mime, "data", &T::data, "width", &T::width, "height", &T::height);
};

template <> struct glz::meta<hax::session_item_detail::wire_item> {
    using T = hax::session_item_detail::wire_item;
    static constexpr auto value = glz::object(
        "kind", &T::kind, "text", &T::text, "call_id", &T::call_id, "tool_name", &T::tool_name,
        "arguments", &T::arguments, "output", &T::output, "output_hidden_tail",
        &T::output_hidden_tail, "reasoning_json", &T::reasoning_json, "reasoning_text",
        &T::reasoning_text, "provider", &T::provider, "model", &T::model, "origin", &T::origin,
        "usage", &T::usage, "images", &T::images);
};

namespace
{

using hax::session_item_detail::wire_image;
using hax::session_item_detail::wire_item;
using hax::session_item_detail::wire_kind;
using hax::session_item_detail::wire_usage;

template <typename T> static void set_optional(std::optional<T> &destination, const T &value)
{
    destination = value;
}

static void set_optional_string(std::optional<std::string> &destination, const char *value)
{
    /* Invalid optional UTF-8 values are omitted instead of failing the record. */
    if (value && utf8_is_valid(value, strlen(value)))
        destination = value;
}

struct kind_name {
    enum item_kind item;
    wire_kind wire;
};

static const kind_name KIND_NAMES[] = {
    {ITEM_USER_MESSAGE, wire_kind::user},     {ITEM_ASSISTANT_MESSAGE, wire_kind::assistant},
    {ITEM_TOOL_CALL, wire_kind::tool_call},   {ITEM_TOOL_RESULT, wire_kind::tool_result},
    {ITEM_REASONING, wire_kind::reasoning},   {ITEM_TURN_BOUNDARY, wire_kind::turn_boundary},
    {ITEM_TURN_USAGE, wire_kind::turn_usage},
};

static int item_kind_to_wire(enum item_kind kind, wire_kind *out)
{
    for (const kind_name &name : KIND_NAMES) {
        if (name.item == kind) {
            *out = name.wire;
            return 0;
        }
    }
    return -1;
}

static enum item_kind wire_kind_to_item(wire_kind kind)
{
    for (const kind_name &name : KIND_NAMES)
        if (name.wire == kind)
            return name.item;
    return ITEM_TURN_BOUNDARY;
}

struct origin_name {
    enum item_origin value;
    const char *name;
};

static const origin_name ORIGIN_NAMES[] = {
    {ITEM_ORIGIN_COMPACT_SEED, "compact_seed"}, {ITEM_ORIGIN_CONTINUATION, "continuation"},
    {ITEM_ORIGIN_INTERRUPTED, "interrupted"},   {ITEM_ORIGIN_SKIPPED, "skipped"},
    {ITEM_ORIGIN_REFUSED, "refused"},           {ITEM_ORIGIN_SUMMARIZED, "summarized"},
    {ITEM_ORIGIN_TASK_NOTE, "task_note"},
};

static void usage_to_wire(const struct turn_usage *source, wire_usage *destination)
{
    if (source->usage.input_tokens >= 0)
        set_optional(destination->input, source->usage.input_tokens);
    if (source->usage.output_tokens >= 0)
        set_optional(destination->output, source->usage.output_tokens);
    if (source->usage.cached_tokens >= 0)
        set_optional(destination->cached, source->usage.cached_tokens);
    if (source->usage.cache_write_tokens >= 0)
        set_optional(destination->cache_write, source->usage.cache_write_tokens);
    if (source->usage.cache_write_1h_tokens >= 0)
        set_optional(destination->cache_write_1h, source->usage.cache_write_1h_tokens);
    if (source->usage.cost >= 0)
        set_optional(destination->cost, source->usage.cost);
    if (source->elapsed_ms >= 0)
        set_optional(destination->elapsed_ms, source->elapsed_ms);
    if (source->uncached_input_tokens >= 0)
        set_optional(destination->in_tokens, source->uncached_input_tokens);
    if (source->cost_input >= 0)
        set_optional(destination->cost_in, source->cost_input);
    if (source->cost_cache_read >= 0)
        set_optional(destination->cost_cache_read, source->cost_cache_read);
    if (source->cost_cache_write >= 0)
        set_optional(destination->cost_cache_write, source->cost_cache_write);
    if (source->cost_output >= 0)
        set_optional(destination->cost_out, source->cost_output);
    if (source->cost_total >= 0)
        set_optional(destination->cost_total, source->cost_total);
    if (source->cost_estimated)
        set_optional(destination->cost_estimated, true);

    set_optional_string(destination->provider_label, source->provenance.provider_label);
    set_optional_string(destination->model_label, source->provenance.model_label);
    set_optional_string(destination->effort, source->provenance.effort);
    set_optional_string(destination->served_model, source->provenance.served_model);
    set_optional_string(destination->route, source->provenance.route);
    set_optional_string(destination->response_id, source->provenance.response_id);
}

static void item_origin_to_wire(enum item_origin origin, std::optional<std::string> *out)
{
    for (const origin_name &name : ORIGIN_NAMES) {
        if (name.value == origin) {
            *out = name.name;
            return;
        }
    }
}

static int item_to_wire(const struct item *source, wire_item *destination)
{
    wire_kind kind;
    if (item_kind_to_wire(source->kind, &kind) < 0)
        return -1;
    destination->kind = kind;

    set_optional_string(destination->text, source->text);
    set_optional_string(destination->call_id, source->call_id);
    set_optional_string(destination->tool_name, source->tool_name);
    set_optional_string(destination->arguments, source->tool_arguments_json);
    set_optional_string(destination->output, source->output);
    if (source->output_hidden_tail > 0 &&
        source->output_hidden_tail <= static_cast<size_t>(LLONG_MAX))
        destination->output_hidden_tail = static_cast<long long>(source->output_hidden_tail);
    set_optional_string(destination->reasoning_json, source->reasoning_json);
    set_optional_string(destination->reasoning_text, source->reasoning_text);
    set_optional_string(destination->provider, source->provider);
    set_optional_string(destination->model, source->model);
    item_origin_to_wire(source->origin, &destination->origin);

    if (source->usage) {
        destination->usage.emplace();
        usage_to_wire(source->usage, &*destination->usage);
    }

    if (source->n_images) {
        destination->images.emplace();
        destination->images->reserve(source->n_images);
        for (size_t i = 0; i < source->n_images; i++) {
            const struct item_image &source_image = source->images[i];
            wire_image image;
            set_optional_string(image.mime, source_image.mime);
            set_optional_string(image.data, source_image.data_b64);
            if (source_image.width > 0)
                image.width = source_image.width;
            if (source_image.height > 0)
                image.height = source_image.height;
            auto encoded_image = hax::json::serialize(image, {.source = "session image"});
            if (!encoded_image)
                return -1;
            destination->images->emplace_back(std::move(*encoded_image));
        }
    }
    return 0;
}

static char *duplicate_optional_string(const std::optional<std::string> &value)
{
    return value ? xstrdup(value->c_str()) : NULL;
}

static long value_or_negative(const std::optional<long> &value)
{
    return value ? *value : -1;
}

static double real_or_negative(const std::optional<double> &value)
{
    return value ? *value : -1;
}

static struct turn_usage *usage_from_wire(const wire_usage &source)
{
    struct turn_usage *destination = (struct turn_usage *)xmalloc(sizeof(*destination));
    destination->usage.input_tokens = value_or_negative(source.input);
    destination->usage.output_tokens = value_or_negative(source.output);
    destination->usage.cached_tokens = value_or_negative(source.cached);
    destination->usage.cache_write_tokens = value_or_negative(source.cache_write);
    destination->usage.cache_write_1h_tokens = value_or_negative(source.cache_write_1h);
    destination->usage.cost = real_or_negative(source.cost);
    destination->elapsed_ms = value_or_negative(source.elapsed_ms);
    destination->uncached_input_tokens = value_or_negative(source.in_tokens);
    if (destination->uncached_input_tokens < 0) {
        /* Without in_tokens, plain subtraction is the available accounting fallback. */
        long cached = destination->usage.cached_tokens > 0 ? destination->usage.cached_tokens : 0;
        long written =
            destination->usage.cache_write_tokens > 0 ? destination->usage.cache_write_tokens : 0;
        long uncached = destination->usage.input_tokens - cached - written;
        destination->uncached_input_tokens = uncached > 0 ? uncached : 0;
    }
    destination->cost_input = real_or_negative(source.cost_in);
    destination->cost_cache_read = real_or_negative(source.cost_cache_read);
    destination->cost_cache_write = real_or_negative(source.cost_cache_write);
    destination->cost_output = real_or_negative(source.cost_out);
    destination->cost_total = real_or_negative(source.cost_total);
    destination->cost_estimated = source.cost_estimated.value_or(false);
    destination->provenance.provider_label = duplicate_optional_string(source.provider_label);
    destination->provenance.model_label = duplicate_optional_string(source.model_label);
    destination->provenance.effort = duplicate_optional_string(source.effort);
    destination->provenance.served_model = duplicate_optional_string(source.served_model);
    destination->provenance.route = duplicate_optional_string(source.route);
    destination->provenance.response_id = duplicate_optional_string(source.response_id);
    return destination;
}

static enum item_origin origin_from_wire(const std::optional<std::string> &origin)
{
    if (!origin)
        return ITEM_ORIGIN_NONE;

    for (const origin_name &name : ORIGIN_NAMES)
        if (*origin == name.name)
            return name.value;
    /* Unknown origins remain ordinary items for forward compatibility. */
    return ITEM_ORIGIN_NONE;
}

static void images_from_wire(const std::optional<std::vector<glz::raw_json>> &source,
                             struct item *destination)
{
    if (!source || source->empty())
        return;

    destination->images =
        (struct item_image *)xcalloc(source->size(), sizeof(*destination->images));
    for (const glz::raw_json &source_image : *source) {
        auto decoded_image = hax::json::parse<wire_image>(
            source_image.str,
            {.source = "session image", .max_input_bytes = 0, .allow_unknown_keys = true});
        if (!decoded_image || !decoded_image->mime || !decoded_image->data)
            continue;
        struct item_image &image = destination->images[destination->n_images++];
        image.mime = xstrdup(decoded_image->mime->c_str());
        image.data_b64 = xstrdup(decoded_image->data->c_str());
        image.width = decoded_image->width.value_or(0);
        image.height = decoded_image->height.value_or(0);
    }
    if (destination->n_images == 0) {
        free(destination->images);
        destination->images = NULL;
    }
}

static int item_from_wire(const wire_item &source, struct item *destination)
{
    if (!source.kind)
        return -1;

    struct item result = {};
    result.kind = wire_kind_to_item(*source.kind);
    result.text = duplicate_optional_string(source.text);
    result.call_id = duplicate_optional_string(source.call_id);
    result.tool_name = duplicate_optional_string(source.tool_name);
    result.tool_arguments_json = duplicate_optional_string(source.arguments);
    result.output = duplicate_optional_string(source.output);
    if (source.output_hidden_tail && *source.output_hidden_tail > 0)
        result.output_hidden_tail = static_cast<size_t>(*source.output_hidden_tail);
    result.reasoning_json = duplicate_optional_string(source.reasoning_json);
    result.reasoning_text = duplicate_optional_string(source.reasoning_text);
    result.provider = duplicate_optional_string(source.provider);
    result.model = duplicate_optional_string(source.model);
    result.origin = origin_from_wire(source.origin);
    if (result.kind == ITEM_TURN_USAGE && source.usage)
        result.usage = usage_from_wire(*source.usage);
    images_from_wire(source.images, &result);

    *destination = result;
    return 0;
}

} // namespace

int session_item_encode(const struct item *item, std::string *out)
{
    if (!item || !out)
        return -1;

    wire_item value;
    if (item_to_wire(item, &value) < 0)
        return -1;

    auto encoded = hax::json::serialize(value, {.source = "session item"});
    if (!encoded)
        return -1;
    *out = std::move(*encoded);
    return 0;
}

int session_item_decode(std::string_view input, struct item *out)
{
    if (!out)
        return -1;
    memset(out, 0, sizeof(*out));

    auto decoded = hax::json::parse<wire_item>(
        input, {.source = "session item", .max_input_bytes = 0, .allow_unknown_keys = true});
    if (!decoded)
        return -1;

    struct item result = {};
    if (item_from_wire(*decoded, &result) < 0)
        return -1;
    *out = result;
    return 0;
}
