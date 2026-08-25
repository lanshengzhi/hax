/* SPDX-License-Identifier: MIT */
#ifndef HAX_PROVIDERS_OPENCODE_H
#define HAX_PROVIDERS_OPENCODE_H

#include "providers/usage_render.h"

struct provider;

/* OpenCode gateway extras beyond the shared recipes: the Go subscription reports rate-limit
 * windows on <base_url>/usage; Zen exposes no balance or usage API yet. */

/* Owned NULL-terminated headers for a usage request: Bearer auth independent of the model wire,
 * plus the provider's configured extra headers. The provider must hold a resolved API key. Free
 * with string_array_free. */
char **opencode_usage_headers(const struct provider *provider);

/* /usage backend for the opencode-go recipe. */
int opencode_go_query_usage(struct provider *provider);

#endif /* HAX_PROVIDERS_OPENCODE_H */
