/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <sys/stat.h>

#include "config.h"
#include "harness.h"
#include "provider.h"
#include "providers/codex.h"
#include "providers/codex_json.h"

static void test_token_expired(void)
{
    char *message = codex_model_catalog_error(401, "codex login expired — run /login again");
    EXPECT_STR_EQ(message, "codex login expired — run /login again");
    free(message);
}

static void test_empty_success_response(void)
{
    char *message = codex_model_catalog_error(200, "expired");
    EXPECT_STR_EQ(message, "codex sent an empty or truncated model catalog response");
    free(message);
}

static void test_http_error(void)
{
    char *message = codex_model_catalog_error(503, "expired");
    EXPECT_STR_EQ(message, "codex model catalog fetch failed (HTTP 503)");
    free(message);
}

static void test_unreachable(void)
{
    char *message = codex_model_catalog_error(0, "expired");
    EXPECT_STR_EQ(message, "could not reach chatgpt.com to list models — check your network");
    free(message);
}

static void test_json_adapter_preserves_compatibility_policy(void)
{
    auto page = hax::codex_json::parse_model_page(
        "{\"models\":[{\"slug\":\"m\",\"context_window\":1e3,"
        "\"max_context_window\":123,\"input_modalities\":[\"text\",7],"
        "\"supported_reasoning_levels\":[{\"effort\":\"low\"},\"medium\","
        "{\"effort\":\"ultra\"},{\"effort\":7}],\"future\":true},7]}");
    EXPECT(page.has_value());
    if (!page)
        return;
    EXPECT(page->models_kind == hax::codex_json::model_page_models_kind::array);
    EXPECT(page->entries.size() == 2);
    EXPECT(page->entries[0].slug.has_value());
    std::string slug = page->entries[0].slug.value_or("");
    EXPECT_STR_EQ(slug.c_str(), "m");
    EXPECT(!page->entries[1].slug);
    auto null_models = hax::codex_json::parse_model_page("{\"models\":null}");
    EXPECT(null_models &&
           null_models->models_kind == hax::codex_json::model_page_models_kind::null_value);

    struct model_info model;
    model_info_init(&model);
    hax::codex_json::parse_model(page->entries[0].json, &model);
    EXPECT(model.context == 123);
    EXPECT(model.image_input == PROVIDER_CAP_NO);
    EXPECT(model.efforts.known);
    EXPECT(model.efforts.count == 3);
    model_info_clear(&model);

    auto usage = hax::codex_json::parse_usage(
        "{\"plan_type\":\"plus\",\"rate_limit\":{"
        "\"primary_window\":{\"used_percent\":12.5,\"reset_at\":1700000000,"
        "\"limit_window_seconds\":1e3},\"secondary_window\":{"
        "\"used_percent\":\"bad\",\"reset_at\":1700000001}},\"future\":[]}");
    EXPECT(usage.has_value());
    if (usage) {
        EXPECT(usage->plan_type.has_value());
        if (usage->plan_type)
            EXPECT_STR_EQ(usage->plan_type->c_str(), "plus");
        EXPECT(usage->rate_limit_nonnull);
        EXPECT(usage->primary_window.has_value());
        if (usage->primary_window) {
            EXPECT(usage->primary_window->used_percent.has_value());
            EXPECT(!usage->primary_window->limit_window_seconds.has_value());
        }
        EXPECT(usage->secondary_window.has_value());
        if (usage->secondary_window)
            EXPECT(!usage->secondary_window->used_percent.has_value());
    }

    std::string error;
    EXPECT(!hax::codex_json::parse_usage("null", &error));
    EXPECT(!error.empty());
    auto array_root = hax::codex_json::parse_usage("[]", &error);
    EXPECT(array_root && !array_root->rate_limit_nonnull);
    auto malformed_window =
        hax::codex_json::parse_usage("{\"rate_limit\":{\"primary_window\":7}}", &error);
    EXPECT(malformed_window && malformed_window->primary_window);
    if (malformed_window && malformed_window->primary_window)
        EXPECT(!malformed_window->primary_window->used_percent);
}

/* Constructs against a scratch $HOME so no real codex login is touched. */
static void test_display_name_from_own_block(void)
{
    char *home = t_tempdir();
    if (!home)
        T_SKIP("cannot create a scratch home");

    char path[4096];
    snprintf(path, sizeof(path), "%s/.codex", home);
    if (mkdir(path, 0700) != 0)
        T_SKIP("cannot create a scratch ~/.codex");
    snprintf(path, sizeof(path), "%s/.codex/auth.json", home);
    FILE *auth_file = fopen(path, "w");
    if (!auth_file)
        T_SKIP("cannot write a scratch auth.json");
    fputs("{\"tokens\": {\"access_token\": \"t\", \"account_id\": \"a\"}}", auth_file);
    fclose(auth_file);

    setenv("HOME", home, 1);
    /* Keep the developer's own hax credential store out of the constructor's auth lookup. */
    unsetenv("XDG_STATE_HOME");
    unsetenv("HAX_MODEL");

    struct provider *codex = codex_provider_new("codex");
    EXPECT(codex != NULL);
    if (codex) {
        EXPECT_STR_EQ(codex->name, "codex");
        codex->destroy(codex);
    }

    /* The provider's own block labels the banner; reasoning provenance keeps the stable id. */
    config_set_override("providers.codex.display_name", "Work ChatGPT");
    codex = codex_provider_new("codex");
    EXPECT(codex != NULL);
    if (codex) {
        EXPECT_STR_EQ(codex->name, "Work ChatGPT");
        EXPECT_STR_EQ(codex->id, "codex");
        codex->destroy(codex);
    }
    config_set_override("providers.codex.display_name", NULL);
}

int main(void)
{
    test_token_expired();
    test_empty_success_response();
    test_http_error();
    test_unreachable();
    test_json_adapter_preserves_compatibility_policy();
    test_display_name_from_own_block();
    T_REPORT();
}
