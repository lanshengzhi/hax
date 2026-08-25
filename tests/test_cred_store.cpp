/* SPDX-License-Identifier: MIT */
#include <optional>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <string_view>
#include <utility>
#include <sys/stat.h>

#include "cred_store.h"
#include "harness.h"
#include "json.h"
#include "util.h"

/* Point the store at a scratch state directory the test controls. */
static void scratch_state_home(void)
{
    char *dir = t_tempdir();
    setenv("XDG_STATE_HOME", dir, 1);
}

static void test_missing_store(void)
{
    scratch_state_home();
    struct cred_store_read loaded = cred_store_get("codex");
    EXPECT(loaded.status == CRED_STORE_ENTRY_MISSING);
    EXPECT(loaded.json.empty());
    EXPECT(cred_store_delete("codex") == CRED_STORE_RESULT_UNCHANGED);
}

static void test_set_get_delete_roundtrip(void)
{
    scratch_state_home();

    const char *entry = "{\"access_token\":\"at\",\"refresh_token\":\"rt\"}";
    EXPECT(cred_store_set("codex", entry) == CRED_STORE_RESULT_CHANGED);

    struct cred_store_read loaded = cred_store_get("codex");
    EXPECT(loaded.status == CRED_STORE_ENTRY_PRESENT);
    if (loaded.status == CRED_STORE_ENTRY_PRESENT) {
        EXPECT(loaded.json.find("\"access_token\"") != std::string::npos);
        EXPECT(loaded.json.find("\"refresh_token\"") != std::string::npos);
    }

    EXPECT(cred_store_delete("codex") == CRED_STORE_RESULT_CHANGED);
    EXPECT(cred_store_get("codex").status == CRED_STORE_ENTRY_MISSING);
    EXPECT(cred_store_delete("codex") == CRED_STORE_RESULT_UNCHANGED);
}

/* Credentials must never be world- or group-readable, including right after creation. */
static void test_store_mode_0600(void)
{
    scratch_state_home();

    EXPECT(cred_store_set("codex", "{\"access_token\":\"at\"}") == CRED_STORE_RESULT_CHANGED);

    char *path = cred_store_file_path();
    EXPECT(path != NULL);
    if (path) {
        struct stat file_stat;
        EXPECT(stat(path, &file_stat) == 0);
        EXPECT((file_stat.st_mode & 0777) == 0600);
        free(path);
    }
}

static void test_entries_are_opaque_and_independent(void)
{
    scratch_state_home();

    const char *opaque = "{\"access_token\":\"codex-token\",\"future\":{\"array\":[1,true,null]}}";
    EXPECT(cred_store_set("codex", opaque) == CRED_STORE_RESULT_CHANGED);
    EXPECT(cred_store_set("other", "{\"api_key\":\"other-key\"}") == CRED_STORE_RESULT_CHANGED);

    EXPECT(cred_store_delete("codex") == CRED_STORE_RESULT_CHANGED);
    struct cred_store_read loaded = cred_store_get("other");
    EXPECT(loaded.status == CRED_STORE_ENTRY_PRESENT);
    if (loaded.status == CRED_STORE_ENTRY_PRESENT)
        EXPECT(loaded.json.find("\"api_key\"") != std::string::npos);

    /* The store never needs to know the provider's future fields to validate or retain them. */
    EXPECT(cred_store_set("codex", opaque) == CRED_STORE_RESULT_CHANGED);
    loaded = cred_store_get("codex");
    EXPECT(loaded.status == CRED_STORE_ENTRY_PRESENT);
    if (loaded.status == CRED_STORE_ENTRY_PRESENT) {
        auto parsed = hax::json::parse_value(loaded.json, {.source = "test", .max_input_bytes = 0});
        EXPECT(parsed.has_value());
        if (parsed && parsed->is_object()) {
            const hax::json::value *future = parsed->find("future");
            EXPECT(future && future->is_object());
        }
    }
}

static void write_store_bytes(const char *contents, size_t length)
{
    char *path = cred_store_file_path();
    EXPECT(path != NULL);
    if (!path)
        return;
    FILE *file = fopen(path, "wb");
    EXPECT(file != NULL);
    if (file) {
        EXPECT(fwrite(contents, 1, length, file) == length);
        fclose(file);
    }
    free(path);
}

static void write_store_file(const char *contents)
{
    write_store_bytes(contents, strlen(contents));
}

static std::string read_store_file(void)
{
    char *path = cred_store_file_path();
    EXPECT(path != NULL);
    if (!path)
        return {};
    size_t length = 0;
    char *contents = slurp_file(path, &length);
    std::string result = contents ? std::string(contents, length) : std::string();
    free(contents);
    free(path);
    return result;
}

static void test_corrupt_store(void)
{
    scratch_state_home();

    const char *entry = "{\"access_token\":\"at\"}";
    EXPECT(cred_store_set("codex", entry) == CRED_STORE_RESULT_CHANGED);
    const char *corrupt = "{not json";
    write_store_file(corrupt);

    /* Mutating operations refuse to guess at another provider's malformed data. */
    EXPECT(cred_store_get("codex").status == CRED_STORE_ENTRY_MALFORMED);
    EXPECT(cred_store_delete("codex") == CRED_STORE_RESULT_MALFORMED);
    EXPECT(cred_store_set("codex", entry) == CRED_STORE_RESULT_MALFORMED);
    std::string unchanged = read_store_file();
    EXPECT_STR_EQ(unchanged.c_str(), corrupt);
}

static void test_embedded_nul_store_is_malformed(void)
{
    scratch_state_home();
    EXPECT(cred_store_set("codex", "{\"access_token\":\"old\"}") == CRED_STORE_RESULT_CHANGED);

    const char malformed[] = "{\"codex\":{\"access_token\":\"old\"}}\0garbage";
    write_store_bytes(malformed, sizeof(malformed) - 1);
    EXPECT(cred_store_get("codex").status == CRED_STORE_ENTRY_MALFORMED);
    EXPECT(cred_store_set("codex", "{\"access_token\":\"new\"}") == CRED_STORE_RESULT_MALFORMED);

    std::string unchanged = read_store_file();
    EXPECT_MEM_EQ(unchanged.data(), unchanged.size(), malformed, sizeof(malformed) - 1);
}

static void test_invalid_entry_rejected(void)
{
    scratch_state_home();

    EXPECT(cred_store_set("codex", "{\"access_token\":\"old\"}") == CRED_STORE_RESULT_CHANGED);
    EXPECT(cred_store_set("codex", "not json") == CRED_STORE_RESULT_INVALID);
    EXPECT(cred_store_set("", "{}") == CRED_STORE_RESULT_INVALID);
    EXPECT(cred_store_set(NULL, "{}") == CRED_STORE_RESULT_INVALID);

    struct cred_store_read loaded = cred_store_get("codex");
    EXPECT(loaded.status == CRED_STORE_ENTRY_PRESENT);
    if (loaded.status == CRED_STORE_ENTRY_PRESENT)
        EXPECT(loaded.json.find("old") != std::string::npos);
}

static int update_called;

static enum cred_store_verdict bump_counter(std::optional<std::string_view> entry,
                                            std::string *replacement, void *ctx)
{
    (void)ctx;
    update_called++;
    if (!entry)
        return CRED_STORE_KEEP;

    auto parsed = hax::json::parse_value(*entry, {.source = "test", .max_input_bytes = 0});
    if (!parsed || !parsed->is_object())
        return CRED_STORE_KEEP;
    const hax::json::value *count = parsed->find("count");
    if (!count || !count->is_integer())
        return CRED_STORE_KEEP;
    const hax::json::value::integer next = count->integer_value() + 1;
    parsed->set("count", next);
    auto encoded = hax::json::serialize_value(*parsed, {.source = "test", .max_input_bytes = 0});
    if (!encoded)
        return CRED_STORE_KEEP;
    *replacement = std::move(*encoded);
    return CRED_STORE_WRITE;
}

static enum cred_store_verdict decline_update(std::optional<std::string_view> entry,
                                              std::string *replacement, void *ctx)
{
    (void)replacement;
    *(int *)ctx = entry.has_value();
    update_called++;
    return CRED_STORE_KEEP;
}

static enum cred_store_verdict create_entry(std::optional<std::string_view> entry,
                                            std::string *replacement, void *ctx)
{
    (void)entry;
    (void)ctx;
    update_called++;
    *replacement = "{\"access_token\":\"created\"}";
    return CRED_STORE_WRITE;
}

static enum cred_store_verdict remove_entry(std::optional<std::string_view> entry,
                                            std::string *replacement, void *ctx)
{
    (void)entry;
    (void)replacement;
    (void)ctx;
    update_called++;
    return CRED_STORE_REMOVE;
}

static void test_update_transaction(void)
{
    scratch_state_home();
    update_called = 0;

    /* Declining against an absent entry writes nothing and observes an empty optional. */
    int seen = 1;
    EXPECT(cred_store_update("codex", decline_update, &seen) == CRED_STORE_RESULT_UNCHANGED);
    EXPECT(seen == 0);
    EXPECT(cred_store_get("codex").status == CRED_STORE_ENTRY_MISSING);

    /* A returned entry is written; the callback owns only its replacement string. */
    EXPECT(cred_store_set("codex", "{\"count\":1}") == CRED_STORE_RESULT_CHANGED);
    EXPECT(cred_store_update("codex", bump_counter, NULL) == CRED_STORE_RESULT_CHANGED);

    struct cred_store_read loaded = cred_store_get("codex");
    EXPECT(loaded.status == CRED_STORE_ENTRY_PRESENT);
    if (loaded.status == CRED_STORE_ENTRY_PRESENT)
        EXPECT(loaded.json.find('2') != std::string::npos);

    /* Declining leaves the stored entry untouched. */
    seen = 0;
    EXPECT(cred_store_update("codex", decline_update, &seen) == CRED_STORE_RESULT_UNCHANGED);
    EXPECT(seen == 1);

    /* An update may also create the entry. */
    EXPECT(cred_store_delete("codex") == CRED_STORE_RESULT_CHANGED);
    EXPECT(cred_store_update("codex", create_entry, NULL) == CRED_STORE_RESULT_CHANGED);
    loaded = cred_store_get("codex");
    EXPECT(loaded.status == CRED_STORE_ENTRY_PRESENT);
    if (loaded.status == CRED_STORE_ENTRY_PRESENT)
        EXPECT(loaded.json.find("created") != std::string::npos);

    /* An update may remove the entry; removing an absent one changes nothing. */
    EXPECT(cred_store_update("codex", remove_entry, NULL) == CRED_STORE_RESULT_CHANGED);
    EXPECT(cred_store_get("codex").status == CRED_STORE_ENTRY_MISSING);
    EXPECT(cred_store_update("codex", remove_entry, NULL) == CRED_STORE_RESULT_UNCHANGED);
}

static void test_update_refuses_malformed_store(void)
{
    scratch_state_home();
    EXPECT(cred_store_set("codex", "{\"access_token\":\"at\"}") == CRED_STORE_RESULT_CHANGED);
    write_store_file("[] trailing");

    update_called = 0;
    EXPECT(cred_store_update("codex", create_entry, NULL) == CRED_STORE_RESULT_MALFORMED);
    EXPECT(update_called == 0);
    EXPECT(cred_store_get("codex").status == CRED_STORE_ENTRY_MALFORMED);
}

static void test_take_returns_removed_entry(void)
{
    scratch_state_home();

    struct cred_store_read taken = cred_store_take("codex");
    EXPECT(taken.status == CRED_STORE_ENTRY_MISSING);
    EXPECT(taken.json.empty());

    EXPECT(cred_store_set("codex", "{\"refresh_token\":\"rt\"}") == CRED_STORE_RESULT_CHANGED);
    taken = cred_store_take("codex");
    EXPECT(taken.status == CRED_STORE_ENTRY_PRESENT);
    if (taken.status == CRED_STORE_ENTRY_PRESENT)
        EXPECT(taken.json.find("rt") != std::string::npos);
    EXPECT(cred_store_get("codex").status == CRED_STORE_ENTRY_MISSING);
}

int main(void)
{
    test_missing_store();
    test_set_get_delete_roundtrip();
    test_store_mode_0600();
    test_entries_are_opaque_and_independent();
    test_corrupt_store();
    test_embedded_nul_store_is_malformed();
    test_invalid_entry_rejected();
    test_update_transaction();
    test_update_refuses_malformed_store();
    test_take_returns_removed_entry();
    T_REPORT();
}
