/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "json_value.h"
#include "provider.h"
#include "tool.h"
#include "util.h"
#include "system/fs.h"
#include "system/path.h"
#include "tools/output_cap.h"
#include "tools/path_preprocess.h"

#define EDIT_READ_CAP (4 * 1024 * 1024)

static size_t count_occurrences(const char *content, size_t content_len, const char *search,
                                size_t search_len)
{
    if (search_len == 0 || search_len > content_len)
        return 0;

    size_t count = 0;
    size_t offset = 0;
    while (offset + search_len <= content_len) {
        if (memcmp(content + offset, search, search_len) == 0) {
            count++;
            offset += search_len;
        } else {
            offset++;
        }
    }
    return count;
}

static char *replace_occurrences(const char *content, size_t content_len, const char *search,
                                 size_t search_len, const char *replacement, size_t replacement_len,
                                 size_t *result_len)
{
    struct buf result;
    buf_init(&result);

    size_t offset = 0;
    size_t unchanged_start = 0;
    while (offset + search_len <= content_len) {
        if (memcmp(content + offset, search, search_len) != 0) {
            offset++;
            continue;
        }

        buf_append(&result, content + unchanged_start, offset - unchanged_start);
        buf_append(&result, replacement, replacement_len);
        offset += search_len;
        unchanged_start = offset;
    }
    buf_append(&result, content + unchanged_start, content_len - unchanged_start);

    *result_len = result.len;
    return result.data ? buf_steal(&result) : xstrdup("");
}

static char *run(const char *args_json, struct tool_run_ctx *ctx)
{
    (void)ctx;
    auto root = hax::json::parse_value(args_json ? args_json : "{}",
                                       {.source = "edit arguments", .max_input_bytes = 0});
    if (!root)
        return xasprintf("invalid arguments: %s", hax::json::format_error(root.error()).c_str());

    char *result = NULL;
    char *path = NULL;
    char *original = NULL;
    char *updated = NULL;
    char *error = NULL;
    const char *old_string = NULL;
    size_t old_string_len = 0;
    const char *new_string = NULL;
    size_t new_string_len = 0;
    int replace_all = 0;
    size_t original_len = 0;
    int truncated = 0;
    original_len = 0;
    truncated = 0;
    size_t match_count = 0;
    size_t updated_len = 0;

    const hax::json::value *path_value = root->find("path");
    const hax::json::value *old_string_json = root->find("old_string");
    const hax::json::value *new_string_json = root->find("new_string");
    const hax::json::value *replace_all_json = root->find("replace_all");
    const char *raw_path =
        path_value && path_value->is_string() ? path_value->string_value().c_str() : NULL;

    if (!raw_path || !*raw_path) {
        result = xstrdup("missing 'path' argument");
        goto out;
    }
    if (!old_string_json || !old_string_json->is_string()) {
        result = xstrdup("missing 'old_string' argument");
        goto out;
    }
    if (!new_string_json || !new_string_json->is_string()) {
        result = xstrdup("missing 'new_string' argument");
        goto out;
    }

    old_string = old_string_json->string_value().data();
    old_string_len = old_string_json->string_value().size();
    new_string = new_string_json->string_value().data();
    new_string_len = new_string_json->string_value().size();
    replace_all =
        replace_all_json && replace_all_json->is_boolean() && replace_all_json->boolean_value();

    if (old_string_len == 0) {
        result = xstrdup("'old_string' must be non-empty");
        goto out;
    }
    if (old_string_len == new_string_len && memcmp(old_string, new_string, old_string_len) == 0) {
        result = xstrdup("'old_string' and 'new_string' are identical — nothing to do");
        goto out;
    }

    path = path_expand_home(raw_path);

    /* Avoid blocking on FIFOs and replacing special files with regular files. */
    struct stat st;
    if (stat(path, &st) == 0 && !S_ISREG(st.st_mode)) {
        result = xasprintf("%s exists but is not a regular file", path);
        goto out;
    }

    original_len = 0;
    truncated = 0;
    original = slurp_file_capped(path, EDIT_READ_CAP, &original_len, &truncated);
    if (!original) {
        result = xasprintf("error reading %s: %s", path, strerror(errno));
        goto out;
    }
    if (truncated) {
        result =
            xasprintf("file %s is larger than %d bytes — refusing to edit", path, EDIT_READ_CAP);
        goto out;
    }

    match_count = count_occurrences(original, original_len, old_string, old_string_len);
    if (match_count == 0) {
        result = xstrdup("'old_string' not found in file");
        goto out;
    }
    if (match_count > 1 && !replace_all) {
        result = xasprintf("'old_string' matches %zu places in %s — provide more context "
                           "to disambiguate, or set replace_all=true",
                           match_count, path);
        goto out;
    }

    updated_len = 0;
    updated = replace_occurrences(original, original_len, old_string, old_string_len, new_string,
                                  new_string_len, &updated_len);

    result = fs_write_with_diff(path, updated, updated_len, &error, NULL);
    if (error) {
        free(result);
        result = error;
    }

out:
    free(updated);
    free(original);
    free(path);
    return result;
}

static const char EDIT_DESCRIPTION[] =
    "Replace an exact string in a file. The `old_string` must match a byte sequence in the file "
    "exactly once unless `replace_all` is true. The `read` tool prefixes each line with a line "
    "number and a " READ_LINE_DELIM " arrow for display; that prefix is NOT part of the file on "
    "disk, so do not include it in `old_string` or `new_string`. Returns a unified diff of the "
    "change.";

static const struct tool_param EDIT_PARAMS[] = {
    {.name = "path", .type = "string", .description = "Path to the file.", .required = 1},
    {.name = "old_string",
     .type = "string",
     .description = "Exact text to find. Must be unique unless replace_all is set.",
     .required = 1},
    {.name = "new_string", .type = "string", .description = "Replacement text.", .required = 1},
    {.name = "replace_all",
     .type = "boolean",
     .description = "Replace every occurrence instead of requiring uniqueness."},
};

const struct tool TOOL_EDIT = {
    .def = {.name = "edit",
            .description = EDIT_DESCRIPTION,
            .params = EDIT_PARAMS,
            .n_params = sizeof(EDIT_PARAMS) / sizeof(EDIT_PARAMS[0])},
    .run = run,
    .preprocess_args = tool_relativize_path_args,
    .display = {.arg_name = "path", .output_style = TOOL_OUTPUT_UNIFIED_DIFF},
};
