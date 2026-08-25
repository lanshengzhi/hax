/* SPDX-License-Identifier: MIT */
#ifndef HAX_TOOL_SCHEMA_H
#define HAX_TOOL_SCHEMA_H

struct json_t;

#include "json_value.h"

struct tool_def;

/* Build the project-owned JSON Schema value describing a tool's parameters. */
hax::json::value tool_schema_value(const struct tool_def *def);

/* Build the legacy Jansson JSON Schema object for protocol boundaries. Returns a new reference the
 * caller owns; never NULL. A tool with no parameters yields a property-less object schema. */
struct json_t *tool_schema_build(const struct tool_def *def);

#endif /* HAX_TOOL_SCHEMA_H */
