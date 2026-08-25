/* SPDX-License-Identifier: MIT */
#include "tool_schema.h"

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
