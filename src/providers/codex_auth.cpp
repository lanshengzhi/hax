/* SPDX-License-Identifier: MIT */
#include "providers/codex_auth.h"

#include <stdlib.h>
#include <string.h>
#include <string_view>
#include <utility>

#include "cred_store.h"
#include "json_value.h"
#include "util.h"
#include "system/path.h"
#include "text/base64.h"

#define CODEX_CLI_AUTH_PATH "~/.codex/auth.json"

static std::optional<hax::json::value> load_json(std::string_view input)
{
    if (input.empty())
        return std::nullopt;
    auto decoded = hax::json::parse_value(
        input,
        {.source = "Codex authentication", .max_input_bytes = 0, .allow_unknown_keys = true});
    if (!decoded || !decoded->is_object())
        return std::nullopt;
    return std::move(*decoded);
}

static std::optional<hax::json::value> codex_jwt_payload(const char *jwt)
{
    if (!jwt || !*jwt)
        return std::nullopt;

    const char *payload_start = strchr(jwt, '.');
    if (!payload_start)
        return std::nullopt;
    payload_start++;

    const char *payload_end = strchr(payload_start, '.');
    if (!payload_end)
        return std::nullopt;

    unsigned char *payload =
        base64url_decode(payload_start, (size_t)(payload_end - payload_start), NULL);
    if (!payload)
        return std::nullopt;

    auto root = load_json((char *)payload);
    free(payload);
    return root;
}

static const hax::json::value *member(const hax::json::value &root, const char *name)
{
    return root.is_object() ? root.find(name) : NULL;
}

static const char *string_member(const hax::json::value *root, const char *name)
{
    const hax::json::value *value = root ? member(*root, name) : NULL;
    return value && value->is_string() ? value->string_value().c_str() : NULL;
}

char *codex_jwt_email(const char *jwt)
{
    auto payload = codex_jwt_payload(jwt);
    if (!payload)
        return NULL;

    const char *email = string_member(&*payload, "email");
    if (!email || !*email) {
        const hax::json::value *profile = member(*payload, "https://api.openai.com/profile");
        email = string_member(profile, "email");
    }

    return email && *email ? xstrdup(email) : NULL;
}

long codex_jwt_exp(const char *jwt)
{
    auto payload = codex_jwt_payload(jwt);
    if (!payload)
        return 0;

    const hax::json::value *exp = member(*payload, "exp");
    if (!exp || !exp->is_number())
        return 0;
    long result = (long)exp->real_value();
    return result > 0 ? result : 0;
}

char *codex_jwt_account_id(const char *jwt)
{
    auto payload = codex_jwt_payload(jwt);
    if (!payload)
        return NULL;

    const hax::json::value *auth_claim = member(*payload, "https://api.openai.com/auth");
    const char *account_id = string_member(auth_claim, "chatgpt_account_id");
    return account_id && *account_id ? xstrdup(account_id) : NULL;
}

static enum codex_auth_status auth_from_cli_root(const hax::json::value &root,
                                                 struct codex_auth *auth)
{
    memset(auth, 0, sizeof(*auth));

    const hax::json::value *tokens = member(root, "tokens");
    const char *access_token = string_member(tokens, "access_token");
    const char *account_id = string_member(tokens, "account_id");
    if (!access_token || !*access_token || !account_id || !*account_id)
        return CODEX_AUTH_NO_TOKENS;

    auth->access_token = xstrdup(access_token);
    auth->account_id = xstrdup(account_id);
    auth->email = codex_jwt_email(string_member(tokens, "id_token"));
    auth->source = CODEX_AUTH_SOURCE_CODEX_CLI;
    return CODEX_AUTH_OK;
}

static enum codex_auth_status auth_from_store_root(const hax::json::value &entry,
                                                   struct codex_auth *auth)
{
    memset(auth, 0, sizeof(*auth));

    const char *access_token = string_member(&entry, "access_token");
    const char *refresh_token = string_member(&entry, "refresh_token");
    const char *account_id = string_member(&entry, "account_id");
    if (!access_token || !*access_token || !refresh_token || !*refresh_token || !account_id ||
        !*account_id)
        return CODEX_AUTH_NO_TOKENS;

    auth->access_token = xstrdup(access_token);
    auth->refresh_token = xstrdup(refresh_token);
    auth->account_id = xstrdup(account_id);
    auth->email = codex_jwt_email(string_member(&entry, "id_token"));
    auth->source = CODEX_AUTH_SOURCE_HAX;
    return CODEX_AUTH_OK;
}

enum codex_auth_status codex_auth_from_json(std::string_view root_json, struct codex_auth *auth)
{
    memset(auth, 0, sizeof(*auth));
    auto root = load_json(root_json);
    if (!root)
        return CODEX_AUTH_NO_TOKENS;

    return auth_from_cli_root(*root, auth);
}

enum codex_auth_status codex_auth_from_store_entry(std::string_view entry_json,
                                                   struct codex_auth *auth)
{
    memset(auth, 0, sizeof(*auth));
    auto entry = load_json(entry_json);
    if (!entry)
        return CODEX_AUTH_NO_TOKENS;

    return auth_from_store_root(*entry, auth);
}

static enum codex_auth_status load_codex_cli(struct codex_auth *auth, char **detail)
{
    char *path = path_expand_home(CODEX_CLI_AUTH_PATH);
    char *contents = slurp_file(path, NULL);
    if (!contents) {
        if (detail)
            *detail = path;
        else
            free(path);
        return CODEX_AUTH_NO_FILE;
    }

    auto root = load_json(contents);
    if (!root) {
        if (detail) {
            auto error = hax::json::parse_value(
                contents, {.source = path, .max_input_bytes = 0, .allow_unknown_keys = true});
            *detail = error ? xstrdup("expected a JSON object")
                            : xstrdup(hax::json::format_error(error.error()).c_str());
        }
        free(contents);
        free(path);
        return CODEX_AUTH_BAD_JSON;
    }

    enum codex_auth_status status = auth_from_cli_root(*root, auth);
    free(contents);
    free(path);
    return status;
}

enum codex_auth_status codex_auth_load(struct codex_auth *auth, char **detail)
{
    memset(auth, 0, sizeof(*auth));
    if (detail)
        *detail = NULL;

    struct cred_store_read stored = cred_store_get("codex");
    if (stored.status == CRED_STORE_ENTRY_PRESENT) {
        enum codex_auth_status status = codex_auth_from_store_entry(stored.json, auth);
        /* A partial entry falls through to the CLI rather than blocking it. */
        if (status == CODEX_AUTH_OK)
            return status;
    }

    return load_codex_cli(auth, detail);
}

static int same_string(const char *a, const char *b)
{
    return a == b || (a && b && strcmp(a, b) == 0);
}

int codex_auth_equal(const struct codex_auth *a, const struct codex_auth *b)
{
    return same_string(a->access_token, b->access_token) &&
           same_string(a->account_id, b->account_id);
}

const char *codex_auth_status_reason(enum codex_auth_status status)
{
    switch (status) {
    case CODEX_AUTH_OK:
        return NULL;
    case CODEX_AUTH_BAD_JSON:
        return "auth.json not valid JSON";
    case CODEX_AUTH_NO_FILE:
    case CODEX_AUTH_NO_TOKENS:
        return "not logged in (use /login)";
    }
    return "not logged in (use /login)";
}

void codex_auth_release(struct codex_auth *auth)
{
    free(auth->access_token);
    free(auth->account_id);
    free(auth->email);
    free(auth->refresh_token);
    memset(auth, 0, sizeof(*auth));
}
