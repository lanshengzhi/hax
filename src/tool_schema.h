/* SPDX-License-Identifier: MIT */
#ifndef HAX_TOOL_SCHEMA_H
#define HAX_TOOL_SCHEMA_H

#include "json_value.h"

struct tool_def;

/* Build the project-owned JSON Schema value describing a tool's parameters. */
hax::json::value tool_schema_value(const struct tool_def *def);

#endif /* HAX_TOOL_SCHEMA_H */
