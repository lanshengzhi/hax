/* SPDX-License-Identifier: MIT */
#include "providers/anthropic.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>
#include <utility>

#include "config.h"
#include "model_meta.h"
#include "provider.h"
#include "util.h"
#include "providers/anthropic_body.h"
#include "providers/anthropic_json.h"
#include "providers/config_provider.h"
#include "providers/http_provider.h"
#include "providers/wire.h"
#include "transport/api_error.h"
#include "transport/http.h"

/* Bound foreground pagination if a server keeps returning advancing cursors. */
#define ANTHROPIC_MODEL_PAGE_SIZE  1000
#define ANTHROPIC_MODEL_PAGE_LIMIT 50

#define MODEL_LIST_TIMEOUT_S  10
#define MODEL_PROBE_TIMEOUT_S 5

void anthropic_parse_model(const json_t *entry, struct model_info *out)
{
    if (!entry || !out)
        return;

    char *encoded = json_dumps(entry, JSON_COMPACT);
    if (encoded) {
        hax::anthropic_json::parse_model(encoded, out);
        free(encoded);
    }
}

static void parse_model_probe_response(const char *response_body, const char *model,
                                       struct model_info *out)
{
    hax::anthropic_json::parse_model_probe_response(response_body ? response_body : "",
                                                    model ? model : "", out);
}

static int anthropic_probe_model(struct provider *provider, const char *model,
                                 struct model_probe *probe)
{
    if (!model || !*model)
        return -1;

    probe->url = xasprintf("%s/models?limit=%d", http_provider_base_url(provider),
                           ANTHROPIC_MODEL_PAGE_SIZE);
    probe->headers = http_provider_metadata_headers(provider);
    probe->timeout_s = MODEL_PROBE_TIMEOUT_S;
    probe->parse = parse_model_probe_response;
    return 0;
}

/* The server-provided cursor is inserted into a query parameter without encoding. */
static int cursor_is_safe(const char *cursor)
{
    if (!cursor || !*cursor)
        return 0;
    for (const char *byte = cursor; *byte; byte++) {
        int safe = (*byte >= 'A' && *byte <= 'Z') || (*byte >= 'a' && *byte <= 'z') ||
                   (*byte >= '0' && *byte <= '9') || *byte == '.' || *byte == '_' || *byte == '-';
        if (!safe)
            return 0;
    }
    return 1;
}

/* On success, `page` owns the parsed response. */
static int fetch_models_page(struct provider *provider, const char *after_id, http_tick_cb tick,
                             void *tick_user, hax::anthropic_json::parsed_model_page *page,
                             char **error)
{
    const char *base_url = http_provider_base_url(provider);
    char *url = after_id ? xasprintf("%s/models?limit=%d&after_id=%s", base_url,
                                     ANTHROPIC_MODEL_PAGE_SIZE, after_id)
                         : xasprintf("%s/models?limit=%d", base_url, ANTHROPIC_MODEL_PAGE_SIZE);
    char **request_headers = http_provider_metadata_headers(provider);

    char *response_body = NULL;
    long status = 0;
    std::optional<hax::anthropic_json::parsed_model_page> parsed;
    int result = -1;
    const int request_result =
        http_get(url, (const char *const *)request_headers, MODEL_LIST_TIMEOUT_S, 0, tick,
                 tick_user, &response_body, &status);
    if (request_result != 0) {
        *error = format_model_list_error(provider->name, base_url,
                                         http_provider_has_api_key(provider), status);
        goto out;
    }

    parsed = hax::anthropic_json::parse_model_page(response_body ? response_body : "");
    if (!parsed) {
        const char *provider_name = provider->name ? provider->name : "provider";
        *error = xasprintf("%s /models response is not valid JSON", provider_name);
        goto out;
    }
    *page = std::move(*parsed);
    result = 0;

out:
    free(response_body);
    string_array_free(request_headers);
    free(url);
    return result == 0 ? 0 : -1;
}

static void append_models(const hax::anthropic_json::parsed_model_page &page,
                          struct model_info **models, size_t *n_models, size_t *capacity,
                          int *saw_entry)
{
    for (const hax::anthropic_json::parsed_model_entry &entry : page.entries) {
        *saw_entry = 1;
        if (!entry.id || entry.id->empty())
            continue;

        if (*n_models == *capacity) {
            const size_t n_entries = page.entries.size();
            *capacity = *capacity ? *capacity * 2 : (n_entries > 8 ? n_entries : 8);
            *models = (model_info *)xrealloc(*models, *capacity * sizeof(**models));
        }
        model_info_init(&(*models)[*n_models]);
        (*models)[*n_models].id = xstrdup(entry.id->c_str());
        hax::anthropic_json::parse_model(entry.json, &(*models)[*n_models]);
        (*n_models)++;
    }
}

static int anthropic_list_models(struct provider *provider, struct model_info **models,
                                 size_t *n_models, char **error, http_tick_cb tick, void *tick_user)
{
    *models = NULL;
    *n_models = 0;

    struct model_info *available = NULL;
    size_t n_available = 0;
    size_t capacity = 0;
    char *after_id = NULL;
    int saw_entry = 0;

    for (int page_number = 0; page_number < ANTHROPIC_MODEL_PAGE_LIMIT; page_number++) {
        hax::anthropic_json::parsed_model_page page{};
        if (fetch_models_page(provider, after_id, tick, tick_user, &page, error) != 0)
            goto fail;

        if (page.data_kind != hax::anthropic_json::model_page_data_kind::array &&
            page.data_kind != hax::anthropic_json::model_page_data_kind::null_value) {
            const char *name = provider->name ? provider->name : "provider";
            *error = xasprintf("%s /models response has no model list", name);
            goto fail;
        }

        const char *last_id = page.last_id ? page.last_id->c_str() : NULL;
        /* Discard a repeated page before it can duplicate models already collected. */
        if (after_id && last_id && strcmp(after_id, last_id) == 0)
            break;

        append_models(page, &available, &n_available, &capacity, &saw_entry);
        free(after_id);
        after_id = page.has_more && cursor_is_safe(last_id) ? xstrdup(last_id) : NULL;
        if (!after_id)
            break;
    }

    if (saw_entry && n_available == 0) {
        const char *name = provider->name ? provider->name : "provider";
        *error = xasprintf("%s /models response contains no usable model ids", name);
        goto fail;
    }

    *models = available;
    *n_models = n_available;
    free(after_id);
    after_id = NULL;
    return 0;

fail:
    free(after_id);
    model_info_free(available, n_available);
    return -1;
}

struct provider *anthropic_provider_new_preset(const struct http_provider_preset *preset)
{
    const struct http_provider_preset empty = {0};
    struct http_provider_preset messages = preset ? *preset : empty;
    messages.wire = &WIRE_ANTHROPIC_MESSAGES;
    messages.efforts = ANTHROPIC_EFFORT_LADDER;
    messages.n_efforts = ANTHROPIC_EFFORT_LADDER_N;

    struct provider *provider = http_provider_new_preset(&messages);
    if (!provider)
        return NULL;
    provider->list_models = anthropic_list_models;
    provider->probe_model = anthropic_probe_model;

    const char *configured_model = config_str("model");
    model_meta_refresh(provider, configured_model && *configured_model ? configured_model : NULL);
    return provider;
}

struct provider *anthropic_provider_new(const char *id)
{
    provider_warn_unused_wire_fields(id, &WIRE_ANTHROPIC_MESSAGES, NULL);
    /* Tweaks resolve from the provider's own block; the pinned endpoint keeps ANTHROPIC_API_KEY
     * from being redirected to a custom URL. */
    struct http_provider_preset preset = {
        .display_name = "anthropic",
        .default_base_url = "https://api.anthropic.com/v1",
        .api_key_env = "ANTHROPIC_API_KEY",
        .config_prefix = "providers.anthropic",
        .pin_base_url = 1,
        .catalog_id = "anthropic",
        .default_thinking_mode = ANTHROPIC_THINKING_ADAPTIVE,
        .allow_empty_signature = 0,
        .send_cache_control_default = 1,
    };
    struct provider *provider = anthropic_provider_new_preset(&preset);
    if (provider)
        provider->id = id;
    return provider;
}

static void anthropic_prepare_availability(const char *id, struct provider_availability *out)
{
    (void)id;
    out->available = provider_api_key("providers.anthropic", "ANTHROPIC_API_KEY") != NULL;
    out->reason = out->available ? NULL : xstrdup("ANTHROPIC_API_KEY not set");
}

const struct provider_factory PROVIDER_ANTHROPIC = {
    .id = "anthropic",
    .create = anthropic_provider_new,
    .prepare_availability = anthropic_prepare_availability,
};
