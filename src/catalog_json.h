/* SPDX-License-Identifier: MIT */
#ifndef HAX_CATALOG_JSON_H
#define HAX_CATALOG_JSON_H

#include <string_view>

struct catalog_entry;

namespace hax::catalog_json
{

/* Parse one models.dev provider object borrowed by `provider_json`, find the borrowed `model` key,
 * and merge its metadata into the initialized non-null `out` entry. Returns 1 when the provider and
 * model parse and 0 for a null output, malformed input, missing model, or incompatible shape. */
int fill_provider_model(std::string_view provider_json, std::string_view model,
                        struct catalog_entry *out);

/* Parse one catalog.models provider object borrowed by `provider_models_json`, find the borrowed
 * `model` key, and merge metadata into initialized non-null `out`. Return 1 when the model parses;
 * return 0 for null output, malformed input, missing model, or incompatible shape. */
int fill_config_model(std::string_view provider_models_json, std::string_view model,
                      struct catalog_entry *out);

/* Parse a borrowed provider object and return 1 when its `models` member exists and is an object;
 * malformed input, a missing member, or another JSON shape returns 0. */
int provider_has_models(std::string_view provider_json);

} // namespace hax::catalog_json

#endif /* HAX_CATALOG_JSON_H */
