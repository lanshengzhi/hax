/* SPDX-License-Identifier: MIT */
#include "tools/path_preprocess.h"

#include <limits.h>
#include <stdlib.h>
#include <unistd.h>

#include "json_value.h"
#include "util.h"
#include "system/path.h"

char *tool_relativize_path_args(const char *args_json)
{
    if (!args_json)
        return NULL;

    auto root =
        hax::json::parse_value(args_json, {.source = "tool arguments", .max_input_bytes = 0});
    if (!root || !root->is_object())
        return NULL;

    const hax::json::value *path_value = root->find("path");
    if (!path_value || !path_value->is_string())
        return NULL;

    char *expanded_path = path_expand_home(path_value->string_value().c_str());
    char cwd[PATH_MAX];
    char *relative_path = getcwd(cwd, sizeof(cwd)) ? path_relativize(expanded_path, cwd) : NULL;
    free(expanded_path);
    if (!relative_path)
        return NULL;

    root->set("path", relative_path);
    free(relative_path);
    auto encoded =
        hax::json::serialize_value(*root, {.source = "tool arguments", .max_input_bytes = 0});
    return encoded ? xstrdup(encoded->c_str()) : NULL;
}
