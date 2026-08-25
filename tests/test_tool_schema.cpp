/* SPDX-License-Identifier: MIT */
#include <stddef.h>

#include "harness.h"
#include "json_helpers.h"
#include "provider.h"
#include "tool_schema.h"

static void test_empty_def_yields_object_schema(void)
{
    struct tool_def def = {.name = "noop"};
    test_json schema = tool_schema_value(&def);
    EXPECT_STR_EQ(test_json_string(test_json_get(&schema, "type")), "object");
    EXPECT(test_json_get(&schema, "properties") != NULL);
    EXPECT(test_json_get(&schema, "required") == NULL);
}

static void test_primitive_params(void)
{
    static const struct tool_param params[] = {
        {.name = "command", .type = "string", .description = "Shell command.", .required = 1},
        {.name = "timeout_seconds", .type = "integer", .minimum = 1},
    };
    struct tool_def def = {.name = "bash", .params = params, .n_params = 2};
    test_json schema = tool_schema_value(&def);

    const test_json *properties = test_json_get(&schema, "properties");
    const test_json *command = test_json_get(properties, "command");
    EXPECT_STR_EQ(test_json_string(test_json_get(command, "type")), "string");
    EXPECT_STR_EQ(test_json_string(test_json_get(command, "description")), "Shell command.");
    const test_json *timeout = test_json_get(properties, "timeout_seconds");
    EXPECT(test_json_integer(test_json_get(timeout, "minimum")) == 1);
    EXPECT(test_json_get(timeout, "items") == NULL);

    const test_json *required = test_json_get(&schema, "required");
    EXPECT(test_json_size(required) == 1);
    EXPECT_STR_EQ(test_json_string(test_json_array_get(required, 0)), "command");
}

static void test_array_param_emits_item_type(void)
{
    static const struct tool_param params[] = {
        {.name = "ids", .type = "array", .item_type = "string", .required = 1},
    };
    struct tool_def def = {.name = "batch", .params = params, .n_params = 1};
    test_json schema = tool_schema_value(&def);

    const test_json *ids = test_json_get(test_json_get(&schema, "properties"), "ids");
    EXPECT_STR_EQ(test_json_string(test_json_get(ids, "type")), "array");
    const test_json *items = test_json_get(ids, "items");
    EXPECT(test_json_is_object(items));
    EXPECT_STR_EQ(test_json_string(test_json_get(items, "type")), "string");
}

int main(void)
{
    test_empty_def_yields_object_schema();
    test_primitive_params();
    test_array_param_emits_item_type();
    T_REPORT();
}
