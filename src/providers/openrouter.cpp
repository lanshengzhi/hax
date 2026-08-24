/* SPDX-License-Identifier: MIT */
#include "providers/openrouter.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "busy.h"
#include "config.h"
#include "model_meta.h"
#include "provider.h"
#include "util.h"
#include "providers/chat_body.h"
#include "providers/config_provider.h"
#include "providers/http_provider.h"
#include "providers/openai_common.h"
#include "providers/openai_compat_json.h"
#include "providers/wire.h"
#include "render/ctrl_strip.h"
#include "terminal/ansi.h"
#include "terminal/ui.h"
#include "transport/http.h"

#define OPENROUTER_BASE_URL         "https://openrouter.ai/api/v1"
#define OPENROUTER_MODELS_ENDPOINT  OPENROUTER_BASE_URL "/models"
#define OPENROUTER_KEY_ENDPOINT     OPENROUTER_BASE_URL "/key"
#define OPENROUTER_CREDITS_ENDPOINT OPENROUTER_BASE_URL "/credits"

#define MODEL_PROBE_TIMEOUT_S 5
#define USAGE_TIMEOUT_S       30

static const char *openrouter_api_key(void)
{
    return provider_api_key("providers.openrouter", "OPENROUTER_API_KEY");
}

void openrouter_parse_model(const json_t *entry, struct model_info *info)
{
    char *encoded = entry ? json_dumps(entry, JSON_COMPACT) : NULL;
    if (encoded) {
        hax::openai_compat_json::parse_openrouter_model(encoded, info);
        free(encoded);
    }
}

void openrouter_parse_efforts(const json_t *entry, struct effort_set *efforts)
{
    char *encoded = entry ? json_dumps(entry, JSON_COMPACT) : NULL;
    if (encoded) {
        hax::openai_compat_json::parse_openrouter_efforts(encoded, efforts);
        free(encoded);
    }
}

void openrouter_parse_model_probe_response(const char *body, const char *model,
                                           struct model_info *info)
{
    hax::openai_compat_json::parse_openrouter_model_probe_response(body ? body : "",
                                                                   model ? model : "", info);
}

static char *encode_query_value(const char *value)
{
    static const char HEX[] = "0123456789ABCDEF";
    struct buf encoded;
    buf_init(&encoded);

    for (const unsigned char *byte = (const unsigned char *)value; *byte; byte++) {
        if ((*byte >= 'A' && *byte <= 'Z') || (*byte >= 'a' && *byte <= 'z') ||
            (*byte >= '0' && *byte <= '9') || *byte == '-' || *byte == '_' || *byte == '.' ||
            *byte == '~') {
            buf_append(&encoded, (const char *)byte, 1);
        } else {
            char escape[3] = {'%', HEX[*byte >> 4], HEX[*byte & 0xf]};
            buf_append(&encoded, escape, sizeof(escape));
        }
    }
    return buf_steal(&encoded);
}

int openrouter_probe_model(struct provider *provider, const char *model, struct model_probe *probe)
{
    (void)provider;
    if (!model || !*model)
        return -1;

    char *encoded_model = encode_query_value(model);
    probe->url = xasprintf(OPENROUTER_MODELS_ENDPOINT "?q=%s", encoded_model);
    free(encoded_model);

    const char *api_key = openrouter_api_key();
    char *authorization = api_key ? xasprintf("Authorization: Bearer %s", api_key) : NULL;
    const char *fixed[] = {authorization, NULL};
    char **extra_headers = provider_extra_headers("providers.openrouter");
    probe->headers = string_array_concat(fixed, (const char *const *)extra_headers);
    string_array_free(extra_headers);
    free(authorization);
    probe->timeout_s = MODEL_PROBE_TIMEOUT_S;
    probe->parse = openrouter_parse_model_probe_response;
    return 0;
}

#define USAGE_LABEL_WIDTH 11

static void print_key_usage(const hax::openai_compat_json::openrouter_key_usage &data)
{
    const char *label = data.label ? data.label->c_str() : NULL;
    printf(ANSI_DIM "openrouter");
    /* Default labels contain a masked API key, which should not enter scrollback; a custom
     * label is server text, so keep terminal controls out of it. */
    if (label && *label && strncmp(label, "sk-", 3) != 0) {
        char *safe_label = ctrl_strip_line_dup(label);
        printf(" · %s", safe_label);
        free(safe_label);
    }
    if (data.free_tier && *data.free_tier)
        printf(" · free tier");
    printf(ANSI_RESET "\n");

    char amount[32];
    if (data.spent) {
        format_cost(amount, sizeof(amount), *data.spent);
        printf("  " ANSI_DIM "%-*s%s" ANSI_RESET "\n", USAGE_LABEL_WIDTH, "spent", amount);
    }

    if (!data.limit)
        return;

    format_cost(amount, sizeof(amount), *data.limit);
    printf("  " ANSI_DIM "%-*s%s", USAGE_LABEL_WIDTH, "key limit", amount);
    if (data.remaining) {
        format_cost(amount, sizeof(amount), *data.remaining);
        printf(" · %s remaining", amount);
    }
    printf(ANSI_RESET "\n");
}

static void print_account_credits(const char *body)
{
    if (!body)
        return;

    auto data = hax::openai_compat_json::parse_openrouter_credits(body);
    if (!data || !data->total || !data->spent || *data->total <= 0)
        return;

    double remaining = *data->total - *data->spent;
    if (remaining < 0)
        remaining = 0;

    char remaining_text[32], total_text[32];
    format_cost(remaining_text, sizeof(remaining_text), remaining);
    format_cost(total_text, sizeof(total_text), *data->total);
    printf("  " ANSI_DIM "%-*s%s of %s remaining" ANSI_RESET "\n", USAGE_LABEL_WIDTH, "credits",
           remaining_text, total_text);
}

static int openrouter_query_usage(struct provider *provider)
{
    (void)provider;
    const char *api_key = openrouter_api_key();
    if (!api_key) {
        ui_error("no OpenRouter API key configured");
        return -1;
    }

    char *authorization = xasprintf("Authorization: Bearer %s", api_key);
    const char *fixed[] = {authorization, "Accept: application/json", NULL};
    char **extra_headers = provider_extra_headers("providers.openrouter");
    char **headers = string_array_concat(fixed, (const char *const *)extra_headers);
    string_array_free(extra_headers);
    free(authorization);
    char *key_body = NULL, *credits_body = NULL;
    std::optional<hax::openai_compat_json::openrouter_key_usage> data;
    int result = -1;
    long status = 0;

    struct busy *busy = busy_begin("fetching usage...");
    int request_result = http_get(OPENROUTER_KEY_ENDPOINT, (const char *const *)headers,
                                  USAGE_TIMEOUT_S, 0, busy_tick, NULL, &key_body, &status);
    if (request_result == 0 && key_body)
        http_get(OPENROUTER_CREDITS_ENDPOINT, (const char *const *)headers, USAGE_TIMEOUT_S, 0,
                 busy_tick, NULL, &credits_body, NULL);
    int cancelled = busy_end(busy);
    string_array_free(headers);

    if (cancelled)
        goto out;
    if (request_result != 0 || !key_body) {
        if (status == 401)
            ui_error("OpenRouter rejected the configured API key (401)");
        else
            ui_error("failed to fetch usage from %s", OPENROUTER_KEY_ENDPOINT);
        goto out;
    }

    data = hax::openai_compat_json::parse_openrouter_key_usage(key_body);
    if (!data) {
        ui_error("unrecognized usage response shape (no data object)");
        goto out;
    }

    print_key_usage(*data);
    print_account_credits(credits_body);
    result = 0;

out:
    free(key_body);
    free(credits_body);
    return result;
}

struct provider *openrouter_provider_new(const char *id)
{
    provider_warn_unused_wire_fields(id, &WIRE_OPENAI_CHAT, NULL);
    const char *title = config_str("providers.openrouter.title");
    const char *referer = config_str("providers.openrouter.referer");

    char *title_header = (title && *title) ? xasprintf("X-Title: %s", title) : NULL;
    char *referer_header = (referer && *referer) ? xasprintf("HTTP-Referer: %s", referer) : NULL;

    const char *headers[4];
    size_t n_headers = 0;
    if (title_header)
        headers[n_headers++] = title_header;
    /* Categories refine app attribution and have no effect without a referer. */
    if (referer_header) {
        headers[n_headers++] = referer_header;
        headers[n_headers++] = "X-OpenRouter-Categories: cli-agent";
    }
    headers[n_headers] = NULL;

    struct http_provider_preset preset = {
        .display_name = "openrouter",
        .default_base_url = OPENROUTER_BASE_URL,
        .api_key_env = "OPENROUTER_API_KEY",
        /* Keep the key and attribution headers pinned to openrouter.ai. */
        .config_prefix = "providers.openrouter",
        .pin_base_url = 1,
        .send_cache_key_default = 1,
        /* OpenRouter requires explicit cache markers for routed Anthropic models. */
        .cache_auto_default = 1,
        .request_cost = 1,
        .reasoning_format = CHAT_REASONING_NESTED,
        .extra_headers = headers,
        .efforts = OPENAI_EFFORT_LADDER,
        .n_efforts = OPENAI_EFFORT_LADDER_N,
        .parse_model = openrouter_parse_model,
    };
    struct provider *provider = http_provider_new_preset(&preset);
    free(title_header);
    free(referer_header);
    if (provider) {
        provider->id = id;
        provider->probe_model = openrouter_probe_model;
        provider->query_usage = openrouter_query_usage;
        model_meta_refresh(provider, config_str("model"));
    }
    return provider;
}

static void openrouter_prepare_availability(const char *id,
                                            struct provider_availability *availability)
{
    (void)id;
    availability->available = openrouter_api_key() != NULL;
    availability->reason = availability->available ? NULL : xstrdup("OPENROUTER_API_KEY not set");
}

const struct provider_factory PROVIDER_OPENROUTER = {
    .id = "openrouter",
    .create = openrouter_provider_new,
    .prepare_availability = openrouter_prepare_availability,
};
