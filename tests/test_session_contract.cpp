/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "harness.h"
#include "provider.h"
#include "session.h"
#include "util.h"

static char *fixture_path(const char *name)
{
    const char *directory = getenv("HAX_SESSION_FIXTURES");
    if (!directory || !*directory)
        return NULL;
    char *path = xasprintf("%s/%s", directory, name);
    if (access(path, R_OK) != 0) {
        free(path);
        return NULL;
    }
    return path;
}

static void free_items(struct item *items, size_t count)
{
    for (size_t i = 0; i < count; i++)
        item_free(&items[i]);
    free(items);
}

static char *read_capture(FILE *file)
{
    EXPECT(fflush(file) == 0);
    EXPECT(fseek(file, 0, SEEK_END) == 0);
    long length = ftell(file);
    EXPECT(length >= 0);
    if (length < 0)
        return xstrdup("");
    EXPECT(fseek(file, 0, SEEK_SET) == 0);
    char *contents = (char *)xmalloc((size_t)length + 1);
    size_t bytes_read = fread(contents, 1, (size_t)length, file);
    contents[bytes_read] = '\0';
    return contents;
}

static int load_fixture(const char *path, struct item **items, size_t *count,
                        struct session_meta *meta)
{
    FILE *capture = tmpfile();
    int saved_stderr = dup(STDERR_FILENO);
    if (!capture || saved_stderr < 0) {
        if (capture)
            fclose(capture);
        if (saved_stderr >= 0)
            close(saved_stderr);
        return session_load(path, items, count, meta);
    }

    fflush(stderr);
    EXPECT(dup2(fileno(capture), STDERR_FILENO) >= 0);
    int result = session_load(path, items, count, meta);
    fflush(stderr);
    EXPECT(dup2(saved_stderr, STDERR_FILENO) >= 0);
    close(saved_stderr);

    char *diagnostics = read_capture(capture);
    EXPECT(strstr(diagnostics, "glz::") == NULL);
    EXPECT(strstr(diagnostics, "Glaze") == NULL);
    free(diagnostics);
    fclose(capture);
    return result;
}

static void test_legacy_compatibility_fixture(void)
{
    char *path = fixture_path("legacy_compatibility.jsonl");
    if (!path)
        T_SKIP("HAX_SESSION_FIXTURES/legacy_compatibility.jsonl is unavailable");

    struct item *items = NULL;
    size_t count = 0;
    struct session_meta meta = {};
    EXPECT(load_fixture(path, &items, &count, &meta) == 0);
    EXPECT(count == 8);
    EXPECT_STR_EQ(meta.id, "legacy-header-id");
    EXPECT_STR_EQ(meta.cwd, "/legacy/project");
    EXPECT_STR_EQ(meta.provider, "new-provider");
    EXPECT_STR_EQ(meta.model, "new-model");
    EXPECT(meta.effort == NULL);
    EXPECT(meta.preset == NULL);

    if (count == 8) {
        EXPECT(items[0].kind == ITEM_USER_MESSAGE);
        EXPECT_STR_EQ(items[0].text, "legacy prompt");

        EXPECT(items[1].kind == ITEM_REASONING);
        EXPECT_STR_EQ(items[1].reasoning_text, "inherited thought");
        EXPECT_STR_EQ(items[1].provider, "legacy-provider");
        EXPECT_STR_EQ(items[1].model, "legacy-model");

        EXPECT(items[2].kind == ITEM_TURN_USAGE);
        EXPECT(items[2].usage != NULL);
        if (items[2].usage) {
            EXPECT(items[2].usage->usage.input_tokens == 100);
            EXPECT(items[2].usage->usage.output_tokens == -1);
            EXPECT(items[2].usage->usage.cached_tokens == 20);
            EXPECT(items[2].usage->usage.cache_write_tokens == 10);
            EXPECT(items[2].usage->uncached_input_tokens == 70);
            EXPECT(items[2].usage->cost_total == -1);
        }

        EXPECT(items[3].kind == ITEM_TOOL_CALL);
        EXPECT_STR_EQ(items[3].call_id, "complete");
        EXPECT_STR_EQ(items[3].tool_name, "write");

        EXPECT(items[4].kind == ITEM_TOOL_RESULT);
        EXPECT_STR_EQ(items[4].call_id, "complete");
        EXPECT_STR_EQ(items[4].output, "legacy result");

        EXPECT(items[5].kind == ITEM_TURN_BOUNDARY);
        EXPECT(items[6].kind == ITEM_ASSISTANT_MESSAGE);
        EXPECT_STR_EQ(items[6].text, "legacy answer");
        EXPECT(items[7].kind == ITEM_USER_MESSAGE);
        EXPECT(items[7].text == NULL);
    }

    free_items(items, count);
    session_meta_free(&meta);
    free(path);
}

static void test_legacy_record_fixture(void)
{
    char *path = fixture_path("legacy_records.jsonl");
    if (!path)
        T_SKIP("HAX_SESSION_FIXTURES/legacy_records.jsonl is unavailable");

    struct item *items = NULL;
    size_t count = 0;
    struct session_meta meta = {};
    EXPECT(load_fixture(path, &items, &count, &meta) == 0);
    EXPECT(count == 9);
    EXPECT_STR_EQ(meta.id, "legacy-records-id");
    EXPECT_STR_EQ(meta.cwd, "/legacy/records");
    EXPECT_STR_EQ(meta.provider, "legacy-provider");
    EXPECT_STR_EQ(meta.model, "legacy-model");

    if (count == 9) {
        EXPECT(items[0].kind == ITEM_REASONING);
        EXPECT_STR_EQ(items[0].reasoning_json, "{\"opaque\":\"state\",\"nonce\":7}");
        EXPECT_STR_EQ(items[0].reasoning_text, "opaque thought");
        EXPECT_STR_EQ(items[0].provider, "legacy-provider");
        EXPECT_STR_EQ(items[0].model, "legacy-model");

        EXPECT(items[1].kind == ITEM_TOOL_RESULT);
        EXPECT(items[1].origin == ITEM_ORIGIN_SUMMARIZED);
        EXPECT(items[1].output_hidden_tail == 15);
        EXPECT(items[1].n_images == 1);
        if (items[1].n_images == 1) {
            EXPECT_STR_EQ(items[1].images[0].mime, "image/png");
            EXPECT_STR_EQ(items[1].images[0].data_b64, "AQ==");
            EXPECT(items[1].images[0].width == 1);
            EXPECT(items[1].images[0].height == 1);
        }

        EXPECT(items[2].origin == ITEM_ORIGIN_COMPACT_SEED);
        EXPECT(items[3].origin == ITEM_ORIGIN_CONTINUATION);
        EXPECT(items[4].origin == ITEM_ORIGIN_INTERRUPTED);
        EXPECT(items[5].origin == ITEM_ORIGIN_SKIPPED);
        EXPECT(items[6].origin == ITEM_ORIGIN_REFUSED);
        EXPECT(items[7].origin == ITEM_ORIGIN_TASK_NOTE);
        EXPECT(items[8].origin == ITEM_ORIGIN_NONE);
    }

    free_items(items, count);
    session_meta_free(&meta);
    free(path);
}

int main(void)
{
    test_legacy_compatibility_fixture();
    test_legacy_record_fixture();
    T_REPORT();
}
