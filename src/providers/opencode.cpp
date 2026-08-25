/* SPDX-License-Identifier: MIT */
#include "providers/opencode.h"

#include <optional>
#include <stdio.h>
#include <stdlib.h>
#include <string>

#include "busy.h"
#include "provider.h"
#include "util.h"
#include "providers/config_provider.h"
#include "providers/http_provider.h"
#include "providers/opencode_json.h"
#include "providers/usage_render.h"
#include "terminal/ansi.h"
#include "terminal/ui.h"
#include "transport/http.h"

#define OPENCODE_USAGE_TIMEOUT_S   30
#define OPENCODE_USAGE_WINDOWS_MAX 8

char **opencode_usage_headers(const struct provider *provider)
{
    /* Not http_provider_metadata_headers: those follow the model wire and switch to x-api-key
     * when the gateway is overridden to the Messages dialect, but the usage endpoint accepts
     * only Bearer auth. */
    char *authorization = xasprintf("Authorization: Bearer %s", http_provider_api_key(provider));
    const char *fixed[] = {authorization, "Accept: application/json", NULL};
    char *prefix = xasprintf("providers.%s", provider->id);
    char **extra_headers = provider_extra_headers(prefix);
    char **headers = string_array_concat(fixed, (const char *const *)extra_headers);
    string_array_free(extra_headers);
    free(prefix);
    free(authorization);
    return headers;
}

int opencode_go_query_usage(struct provider *provider)
{
    if (!http_provider_has_api_key(provider)) {
        ui_error("no OPENCODE_API_KEY configured");
        return -1;
    }

    char *url = xasprintf("%s/usage", http_provider_base_url(provider));
    char **headers = opencode_usage_headers(provider);
    char *body = NULL;
    std::optional<hax::opencode_json::parsed_usage> usage;
    std::string parse_error;
    long status = 0;
    int result = -1;

    struct busy *busy = busy_begin("fetching usage...");
    int request_result = http_get(url, (const char *const *)headers, OPENCODE_USAGE_TIMEOUT_S, 0,
                                  busy_tick, NULL, &body, &status);
    int cancelled = busy_end(busy);
    string_array_free(headers);

    if (cancelled)
        goto out;
    if (request_result != 0 || !body) {
        if (status == 401)
            ui_error("opencode rejected the configured API key (401)");
        else
            ui_error("failed to fetch usage from %s", url);
        goto out;
    }

    usage = hax::opencode_json::parse_usage(body, &parse_error);
    if (!usage) {
        ui_error("usage response is not valid JSON: %s", parse_error.c_str());
        goto out;
    }

    if (usage->windows.empty()) {
        ui_error("unrecognized usage response shape (no usage windows)");
        goto out;
    }

    printf(ANSI_DIM "%s" ANSI_RESET "\n", provider->name);
    for (size_t i = 0; i < usage->windows.size() && i < OPENCODE_USAGE_WINDOWS_MAX; i++) {
        const hax::opencode_json::parsed_usage_window &parsed = usage->windows[i];
        struct usage_window window = {
            .label = parsed.label.c_str(),
            .used_percent = parsed.used_percent,
            .reset_at = parsed.reset_at,
            .note = parsed.note ? parsed.note->c_str() : NULL,
        };
        usage_window_print(&window);
    }
    result = 0;

out:
    free(body);
    free(url);
    return result;
}
