/* SPDX-License-Identifier: MIT */
#include "tool_schema.h"

#include <jansson.h>
#include <stddef.h>
#include <utility>

#include "provider.h"

hax::json::value tool_schema_value(const struct tool_def *def)
{
    hax::json::object properties;
    hax::json::array required;

    for (size_t i = 0; def && i < def->n_params; i++) {
        const struct tool_param *param = &def->params[i];
        hax::json::object property;

        if (param->type)
            property.emplace_back("type", param->type);
        if (param->item_type)
            property.emplace_back("items", hax::json::object{{"type", param->item_type}});
        if (param->description)
            property.emplace_back("description", param->description);
        if (param->minimum)
            property.emplace_back("minimum", param->minimum);
        properties.emplace_back(param->name, hax::json::value(std::move(property)));

        if (param->required)
            required.emplace_back(param->name);
    }

    hax::json::object schema;
    schema.emplace_back("type", "object");
    schema.emplace_back("properties", hax::json::value(std::move(properties)));
    if (!required.empty())
        schema.emplace_back("required", hax::json::value(std::move(required)));
    return schema;
}

static json_t *to_jansson(const hax::json::value &source)
{
    if (source.is_null())
        return json_null();
    if (source.is_boolean())
        return source.boolean_value() ? json_true() : json_false();
    if (source.is_integer())
        return json_integer(source.integer_value());
    if (source.is_real())
        return json_real(source.real_value());
    if (source.is_string())
        return json_string(source.string_value().c_str());
    if (source.is_array()) {
        json_t *result = json_array();
        for (const hax::json::value &item : source.array_items())
            json_array_append_new(result, to_jansson(item));
        return result;
    }

    json_t *result = json_object();
    for (const auto &member : source.object_items())
        json_object_setn_new(result, member.first.data(), member.first.size(),
                             to_jansson(member.second));
    return result;
}

json_t *tool_schema_build(const struct tool_def *def)
{
    return to_jansson(tool_schema_value(def));
}
