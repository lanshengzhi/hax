/* SPDX-License-Identifier: MIT */
#include "providers/codex_login.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "busy.h"
#include "cred_store.h"
#include "json_value.h"
#include "trace.h"
#include "util.h"
#include "providers/codex_auth.h"
#include "terminal/ansi.h"
#include "terminal/clipboard.h"
#include "terminal/ui.h"
#include "transport/api_error.h"
#include "transport/http.h"
#include "transport/retry.h"

/* The codex CLI's public OAuth client. The device flow is OpenAI's own API rather than RFC 8628:
 * the poll response hands back an authorization code plus a server-generated PKCE verifier, which
 * then go through the ordinary authorization-code exchange. */
#define CODEX_OAUTH_CLIENT_ID "app_EMoamEEZ73f0CkXaXp7hrann"

#define CODEX_TOKEN_ENDPOINT           "https://auth.openai.com/oauth/token"
#define CODEX_REVOKE_ENDPOINT          "https://auth.openai.com/oauth/revoke"
#define CODEX_DEVICE_USERCODE_ENDPOINT "https://auth.openai.com/api/accounts/deviceauth/usercode"
#define CODEX_DEVICE_TOKEN_ENDPOINT    "https://auth.openai.com/api/accounts/deviceauth/token"
#define CODEX_DEVICE_VERIFY_URL        "https://auth.openai.com/codex/device"
#define CODEX_DEVICE_REDIRECT_URI      "https://auth.openai.com/deviceauth/callback"

#define CODEX_LOGIN_HTTP_TIMEOUT_S 30
#define CODEX_REVOKE_TIMEOUT_S     10

/* The user code expires after 15 minutes; polling past that can never succeed. */
#define CODEX_DEVICE_DEADLINE_MS (15L * 60L * 1000L)

#define CODEX_POLL_INTERVAL_DEFAULT_S 5
#define CODEX_POLL_INTERVAL_MAX_S     60

/* Refresh this long before access-token expiry: the margin must outlast a request's retry backoff,
 * during which the Authorization header is rebuilt from this credential. */
#define CODEX_TOKEN_REFRESH_MARGIN_S 600

/* ---------- pure protocol helpers ---------- */

static long parse_interval_s(const hax::json::value *interval)
{
    long seconds = 0;
    if (interval && interval->is_integer())
        seconds = (long)interval->integer_value();
    else if (interval && interval->is_string())
        seconds = strtol(interval->string_value().c_str(), NULL, 10);
    if (seconds < 1)
        return CODEX_POLL_INTERVAL_DEFAULT_S;
    if (seconds > CODEX_POLL_INTERVAL_MAX_S)
        return CODEX_POLL_INTERVAL_MAX_S;
    return seconds;
}

static std::optional<hax::json::value> load_json(std::string_view input)
{
    if (input.empty())
        return std::nullopt;
    auto decoded = hax::json::parse_value(
        input,
        {.source = "Codex login response", .max_input_bytes = 0, .allow_unknown_keys = true});
    if (!decoded)
        return std::nullopt;
    return std::move(*decoded);
}

static const hax::json::value *member(const hax::json::value *root, const char *name)
{
    return root && root->is_object() ? root->find(name) : NULL;
}

static const char *string_member(const hax::json::value *root, const char *name)
{
    const hax::json::value *value = member(root, name);
    return value && value->is_string() ? value->string_value().c_str() : NULL;
}

int codex_login_parse_usercode(const char *body, struct codex_device_auth *out)
{
    memset(out, 0, sizeof(*out));
    auto root = load_json(body ? body : "");
    if (!root)
        return -1;

    const char *device_auth_id = string_member(&*root, "device_auth_id");
    const char *user_code = string_member(&*root, "user_code");
    /* Older responses spell the field "usercode". */
    if (!user_code || !*user_code)
        user_code = string_member(&*root, "usercode");
    if (!device_auth_id || !*device_auth_id || !user_code || !*user_code)
        return -1;

    out->device_auth_id = xstrdup(device_auth_id);
    out->user_code = xstrdup(user_code);
    out->interval_s = parse_interval_s(member(&*root, "interval"));
    return 0;
}

void codex_device_auth_release(struct codex_device_auth *device_auth)
{
    free(device_auth->device_auth_id);
    free(device_auth->user_code);
    memset(device_auth, 0, sizeof(*device_auth));
}

/* The endpoint reports errors as error.code, a bare error string, or a top-level code. */
static const char *oauth_error_code(const hax::json::value *root)
{
    const hax::json::value *error = member(root, "error");
    const char *code = string_member(error, "code");
    if (!code && error && error->is_string())
        code = error->string_value().c_str();
    if (!code)
        code = string_member(root, "code");
    return code;
}

enum codex_poll_result codex_login_classify_poll(long http_status, const char *body,
                                                 char **authorization_code, char **code_verifier)
{
    auto root = load_json(body ? body : "");

    if (http_status >= 200 && http_status < 300) {
        const char *code = string_member(root ? &*root : NULL, "authorization_code");
        const char *verifier = string_member(root ? &*root : NULL, "code_verifier");
        if (code && *code && verifier && *verifier) {
            *authorization_code = xstrdup(code);
            *code_verifier = xstrdup(verifier);
            return CODEX_POLL_AUTHORIZED;
        }
        return CODEX_POLL_FAILED;
    }

    const char *error_code = oauth_error_code(root ? &*root : NULL);
    if (error_code && strcmp(error_code, "slow_down") == 0)
        return CODEX_POLL_SLOW_DOWN;
    if (http_status == 403 || http_status == 404 ||
        (error_code && strstr(error_code, "authorization_pending")))
        return CODEX_POLL_PENDING;
    return CODEX_POLL_FAILED;
}

static void buf_append_form_value(struct buf *form, const char *value)
{
    for (const char *cursor = value; *cursor; cursor++) {
        unsigned char c = (unsigned char)*cursor;
        if (isalnum(c) || strchr("-._~", c)) {
            buf_append(form, cursor, 1);
        } else {
            char escaped[4];
            snprintf(escaped, sizeof(escaped), "%%%02X", c);
            buf_append_str(form, escaped);
        }
    }
}

char *codex_login_build_exchange_body(const char *authorization_code, const char *code_verifier)
{
    struct buf form;
    buf_init(&form);
    buf_append_str(&form, "grant_type=authorization_code&code=");
    buf_append_form_value(&form, authorization_code);
    buf_append_str(&form, "&redirect_uri=");
    buf_append_form_value(&form, CODEX_DEVICE_REDIRECT_URI);
    buf_append_str(&form, "&client_id=");
    buf_append_form_value(&form, CODEX_OAUTH_CLIENT_ID);
    buf_append_str(&form, "&code_verifier=");
    buf_append_form_value(&form, code_verifier);
    return buf_steal(&form);
}

static std::optional<std::string> dump_json(const hax::json::value &root)
{
    auto encoded =
        hax::json::serialize_value(root, {.source = "Codex credentials", .max_input_bytes = 0});
    if (!encoded)
        return std::nullopt;
    return std::move(*encoded);
}

static std::optional<std::string> entry_field(std::string_view entry, const char *field)
{
    auto root = load_json(entry);
    const char *value = string_member(root ? &*root : NULL, field);
    return value ? std::optional<std::string>(value) : std::nullopt;
}

std::optional<std::string> codex_login_entry_from_exchange(const char *body)
{
    auto root = load_json(body ? body : "");
    if (!root)
        return std::nullopt;

    const char *id_token = string_member(&*root, "id_token");
    const char *access_token = string_member(&*root, "access_token");
    const char *refresh_token = string_member(&*root, "refresh_token");
    if (!id_token || !*id_token || !access_token || !*access_token || !refresh_token ||
        !*refresh_token)
        return std::nullopt;

    /* The account id is fixed at login: refresh responses may omit the id_token, so it cannot be
     * re-derived later. */
    char *account_id = codex_jwt_account_id(id_token);
    if (!account_id)
        account_id = codex_jwt_account_id(access_token);
    if (!account_id)
        return std::nullopt;

    hax::json::value entry = hax::json::object{{"access_token", access_token},
                                               {"refresh_token", refresh_token},
                                               {"id_token", id_token},
                                               {"account_id", account_id}};
    free(account_id);
    return dump_json(entry);
}

std::optional<std::string> codex_login_apply_refresh(std::string_view entry, const char *body)
{
    auto entry_root = load_json(entry);
    auto root = load_json(body ? body : "");
    if (!entry_root || !entry_root->is_object() || !root)
        return std::nullopt;

    const char *access_token = string_member(&*root, "access_token");
    if (!access_token || !*access_token)
        return std::nullopt;

    static const char *const FIELDS[] = {"access_token", "refresh_token", "id_token"};
    for (const char *field : FIELDS) {
        const char *value = string_member(&*root, field);
        if (value && *value)
            entry_root->set(field, value);
    }
    return dump_json(*entry_root);
}

/* ---------- refresh lifecycle ---------- */

int codex_login_token_as_fresh(const char *candidate, const char *current)
{
    long candidate_exp = codex_jwt_exp(candidate);
    long current_exp = codex_jwt_exp(current);
    if (candidate_exp == 0 || current_exp == 0)
        return 1;
    return candidate_exp >= current_exp;
}

int codex_login_token_expiring(const char *access_token, long margin_s)
{
    long exp = codex_jwt_exp(access_token);
    /* An unreadable exp is left to the 401 recovery path rather than refreshing every request. */
    if (exp == 0)
        return 0;
    return exp <= time(NULL) + margin_s;
}

static char *dump_compact(const hax::json::value &root)
{
    auto encoded =
        hax::json::serialize_value(root, {.source = "Codex login request", .max_input_bytes = 0});
    return encoded ? xstrdup(encoded->c_str()) : NULL;
}

struct refresh_tx {
    /* the credential the caller holds */
    const char *current_access_token;
    const char *current_refresh_token;
    const char *current_account_id;
    int force;
    http_tick_cb tick;
    void *tick_user;
    std::optional<std::string> adopted; /* out: owned entry the caller should adopt */
    int refreshed;                      /* out: a token POST was attempted */
    int ran; /* out: the update callback reached its decision; false when it was not invoked */
    int transient; /* out: the failure was retryable rather than a rejected login */
};

int codex_login_refresh_rejected(long http_status, const char *body)
{
    if (http_status < 400 || http_status >= 500)
        return 0;

    auto root = load_json(body ? body : "");
    const char *code = oauth_error_code(root ? &*root : NULL);
    static const char *const TERMINAL[] = {"invalid_grant", "refresh_token_expired",
                                           "refresh_token_reused", "refresh_token_invalidated"};
    for (size_t i = 0; code && i < sizeof(TERMINAL) / sizeof(TERMINAL[0]); i++)
        if (strcmp(code, TERMINAL[i]) == 0)
            return 1;
    return 0;
}

/* A dispatched rotation must complete even when the user cancels the surrounding operation: the
 * server may already have consumed the refresh token, and abandoning the response would strand
 * the login on a spent token. Tick calls still animate progress; only the abort signal is
 * withheld, bounded by the request timeout. */
struct rotation_tick_guard {
    http_tick_cb tick;
    void *tick_user;
};

static int rotation_tick(void *user)
{
    struct rotation_tick_guard *guard = (struct rotation_tick_guard *)user;
    if (guard->tick)
        guard->tick(guard->tick_user);
    return 0;
}

/* The whole rotation runs under the store lock, so a sibling process that also needs a refresh
 * blocks here, then adopts the rotated entry instead of spending the same refresh token twice.
 * Only store writers wait on the lock; plain reads never do. A missing entry is a concurrent
 * /logout and must not be resurrected. An entry *behind* the caller's credential is the residue
 * of an earlier failed persist (only refresh leaves memory ahead of disk), so the refresh
 * proceeds from the freshest refresh token of the lineage and overwrites forward. */
static enum cred_store_verdict refresh_transaction(std::optional<std::string_view> entry,
                                                   std::string *replacement, void *user)
{
    struct refresh_tx *tx = (struct refresh_tx *)user;
    tx->ran = 1;
    if (!entry)
        return CRED_STORE_KEEP;

    /* Never adopt or refresh across an account boundary: a /login into a different account must
     * not silently take over this session, nor have this lineage's tokens merged into its entry. */
    std::optional<std::string> entry_account = entry_field(*entry, "account_id");
    if (!entry_account || *entry_account != tx->current_account_id)
        return CRED_STORE_KEEP;

    std::optional<std::string> entry_access = entry_field(*entry, "access_token");
    std::optional<std::string> entry_refresh = entry_field(*entry, "refresh_token");
    if (!entry_access || entry_access->empty() || !entry_refresh || entry_refresh->empty())
        return CRED_STORE_KEEP;

    int entry_differs =
        *entry_access != tx->current_access_token || *entry_refresh != tx->current_refresh_token;
    int entry_as_fresh =
        codex_login_token_as_fresh(entry_access->c_str(), tx->current_access_token);
    if (entry_differs && entry_as_fresh &&
        (tx->force ||
         !codex_login_token_expiring(entry_access->c_str(), CODEX_TOKEN_REFRESH_MARGIN_S))) {
        /* Another process already rotated; adopt its credential without a POST. */
        tx->adopted = std::string(*entry);
        return CRED_STORE_KEEP;
    }

    std::string refresh_token =
        entry_differs && !entry_as_fresh ? tx->current_refresh_token : *entry_refresh;
    trace_register_secret(refresh_token.c_str());
    char *request_body = dump_compact(hax::json::value{hax::json::object{
        {"client_id", CODEX_OAUTH_CLIENT_ID},
        {"grant_type", "refresh_token"},
        {"refresh_token", refresh_token},
    }});
    char *response = NULL;
    long status = 0;
    struct rotation_tick_guard tick_guard = {.tick = tx->tick, .tick_user = tx->tick_user};
    int result = http_post(CODEX_TOKEN_ENDPOINT, NULL, "application/json", request_body,
                           strlen(request_body), CODEX_LOGIN_HTTP_TIMEOUT_S, 0, rotation_tick,
                           &tick_guard, &response, &status);
    free(request_body);
    tx->refreshed = 1;
    if (result != 0 || status < 200 || status >= 300 || !response) {
        int rejected = result == 0 && codex_login_refresh_rejected(status, response);
        tx->transient = !rejected;
        free(response);
        /* A definitively rejected entry is removed so later loads reach valid fallback
         * credentials (the codex CLI's) instead of resending a dead token — but only when the
         * rejected token is still the entry's own. */
        if (rejected && refresh_token == *entry_refresh)
            return CRED_STORE_REMOVE;
        return CRED_STORE_KEEP;
    }

    std::optional<std::string> refreshed = codex_login_apply_refresh(*entry, response);
    free(response);
    if (!refreshed) {
        /* A 2xx that does not parse as a token response is gateway interference, not a verdict
         * on the grant. */
        tx->transient = 1;
        return CRED_STORE_KEEP;
    }
    tx->adopted = std::move(*refreshed);
    *replacement = *tx->adopted;
    return CRED_STORE_WRITE;
}

enum codex_refresh_result codex_login_ensure_fresh(struct codex_auth *auth, int force,
                                                   http_tick_cb tick, void *tick_user)
{
    if (auth->source != CODEX_AUTH_SOURCE_HAX)
        return force ? CODEX_REFRESH_DEAD : CODEX_REFRESH_FRESH;

    if (!force && !codex_login_token_expiring(auth->access_token, CODEX_TOKEN_REFRESH_MARGIN_S))
        return CODEX_REFRESH_FRESH;

    struct refresh_tx tx = {
        .current_access_token = auth->access_token,
        .current_refresh_token = auth->refresh_token,
        .current_account_id = auth->account_id,
        .force = force,
        .tick = tick,
        .tick_user = tick_user,
        .adopted = std::nullopt,
        .refreshed = 0,
        .ran = 0,
        .transient = 0,
    };
    enum cred_store_result stored = cred_store_update("codex", refresh_transaction, &tx);
    if (!tx.adopted) {
        /* A lock/read failure or malformed store is an environment hiccup, not a verdict on the
         * login; an invoked callback that declines is a definitive dead/missing credential. */
        return tx.transient || !tx.ran ? CODEX_REFRESH_TRANSIENT : CODEX_REFRESH_DEAD;
    }

    /* Losing this write leaves the store a rotation behind; the in-memory adoption below carries
     * this session, and the next rotation's stale-behind overwrite retries the persist. */
    if (tx.refreshed && stored != CRED_STORE_RESULT_CHANGED) {
        char *path = cred_store_file_path();
        hax_warn("cannot persist refreshed codex login to %s", path ? path : "the state directory");
        free(path);
    }

    struct codex_auth refreshed;
    enum codex_auth_status adopted_status = codex_auth_from_store_entry(*tx.adopted, &refreshed);
    if (adopted_status != CODEX_AUTH_OK)
        return CODEX_REFRESH_DEAD;
    trace_register_secret(refreshed.refresh_token);

    codex_auth_release(auth);
    *auth = refreshed;
    return CODEX_REFRESH_FRESH;
}

/* ---------- interactive login / logout ---------- */

char *codex_login_status(void)
{
    struct codex_auth auth;
    if (codex_auth_load(&auth, NULL) != CODEX_AUTH_OK)
        return NULL;

    char *description;
    if (auth.source == CODEX_AUTH_SOURCE_HAX)
        description = auth.email ? xasprintf("logged in as %s — hax manages this token", auth.email)
                                 : xstrdup("logged in — hax manages this token");
    else
        description = xasprintf("using codex CLI credentials%s%s (read-only) — log in to let hax "
                                "manage its own token",
                                auth.email ? " of " : "", auth.email ? auth.email : "");
    codex_auth_release(&auth);
    return description;
}

int codex_login_present(void)
{
    struct cred_store_read stored = cred_store_get("codex");
    if (stored.status != CRED_STORE_ENTRY_PRESENT)
        return 0;

    struct codex_auth auth;
    int present = codex_auth_from_store_entry(stored.json, &auth) == CODEX_AUTH_OK;
    if (present)
        codex_auth_release(&auth);
    return present;
}

static int request_usercode(struct codex_device_auth *device_auth)
{
    char *request_body =
        dump_compact(hax::json::value{hax::json::object{{"client_id", CODEX_OAUTH_CLIENT_ID}}});

    struct busy *busy = busy_begin("contacting auth.openai.com...");
    char *response = NULL;
    long status = 0;
    int result = http_post(CODEX_DEVICE_USERCODE_ENDPOINT, NULL, "application/json", request_body,
                           strlen(request_body), CODEX_LOGIN_HTTP_TIMEOUT_S, 0, busy_tick, NULL,
                           &response, &status);
    int cancelled = busy_end(busy);
    free(request_body);
    if (cancelled) {
        free(response);
        return 1;
    }

    if (result != 0 || status < 200 || status >= 300) {
        if (status == 404) {
            ui_error("device login is not enabled for this endpoint — log in with the codex CLI "
                     "and hax will pick up its credentials");
        } else {
            char *message = format_api_error(status, response);
            ui_error("device login request failed: %s", message);
            free(message);
        }
        free(response);
        return -1;
    }

    int parsed = codex_login_parse_usercode(response, device_auth);
    free(response);
    if (parsed != 0) {
        ui_error("unrecognized device login response from auth.openai.com");
        return -1;
    }
    return 0;
}

/* Returns like codex_login_run; on 0 the outputs are owned. */
static int poll_for_authorization(const struct codex_device_auth *device_auth,
                                  char **authorization_code, char **code_verifier)
{
    char *poll_body = dump_compact(hax::json::value{hax::json::object{
        {"device_auth_id", device_auth->device_auth_id},
        {"user_code", device_auth->user_code},
    }});
    long interval_ms = device_auth->interval_s * 1000;
    long deadline = monotonic_ms() + CODEX_DEVICE_DEADLINE_MS;
    char *error = NULL;
    int cancelled = 0;

    struct busy *busy = busy_begin("waiting for browser approval...");
    for (;;) {
        if (retry_sleep_with_tick(interval_ms, busy_tick, NULL)) {
            cancelled = 1;
            break;
        }
        if (monotonic_ms() >= deadline) {
            error = xstrdup("login timed out — the code expired before it was approved");
            break;
        }

        char *response = NULL;
        long status = 0;
        int result = http_post(CODEX_DEVICE_TOKEN_ENDPOINT, NULL, "application/json", poll_body,
                               strlen(poll_body), CODEX_LOGIN_HTTP_TIMEOUT_S, 0, busy_tick, NULL,
                               &response, &status);
        if (result != 0) {
            free(response);
            error = xstrdup("cannot reach auth.openai.com — check your network");
            break;
        }

        enum codex_poll_result poll_result =
            codex_login_classify_poll(status, response, authorization_code, code_verifier);
        if (poll_result == CODEX_POLL_FAILED) {
            char *message = format_api_error(status, response);
            error = xasprintf("login was rejected: %s", message);
            free(message);
        }
        free(response);
        if (poll_result == CODEX_POLL_AUTHORIZED || poll_result == CODEX_POLL_FAILED)
            break;
        if (poll_result == CODEX_POLL_SLOW_DOWN)
            interval_ms += 5000;
    }
    cancelled |= busy_end(busy);
    free(poll_body);

    if (cancelled) {
        free(error);
        free(*authorization_code);
        free(*code_verifier);
        *authorization_code = NULL;
        *code_verifier = NULL;
        return 1;
    }
    if (error) {
        ui_error("%s", error);
        free(error);
        return -1;
    }
    return 0;
}

/* The percent-encoded form of a credential is as sensitive as the raw value and is what the
 * traced request body actually contains. */
static void register_form_secret(const char *value)
{
    trace_register_secret(value);
    struct buf encoded;
    buf_init(&encoded);
    buf_append_form_value(&encoded, value);
    char *encoded_value = buf_steal(&encoded);
    trace_register_secret(encoded_value);
    free(encoded_value);
}

static std::optional<std::string> exchange_authorization_code(const char *authorization_code,
                                                              const char *code_verifier,
                                                              int *cancelled)
{
    register_form_secret(authorization_code);
    register_form_secret(code_verifier);
    char *exchange_body = codex_login_build_exchange_body(authorization_code, code_verifier);

    struct busy *busy = busy_begin("completing login...");
    char *response = NULL;
    long status = 0;
    int result = http_post(CODEX_TOKEN_ENDPOINT, NULL, "application/x-www-form-urlencoded",
                           exchange_body, strlen(exchange_body), CODEX_LOGIN_HTTP_TIMEOUT_S, 0,
                           busy_tick, NULL, &response, &status);
    int interrupted = busy_end(busy);
    free(exchange_body);

    /* An Esc queued behind a completed exchange is outranked by it: the authorization code is
     * consumed, so completing the login beats discarding an active credential. Cancellation is
     * honored only when the exchange did not succeed. */
    *cancelled = 0;
    if (result != 0 || status < 200 || status >= 300) {
        if (interrupted) {
            *cancelled = 1;
            free(response);
            return std::nullopt;
        }
        char *message = format_api_error(status, response);
        ui_error("token exchange failed: %s", message);
        free(message);
        free(response);
        return std::nullopt;
    }

    std::optional<std::string> entry = codex_login_entry_from_exchange(response);
    free(response);
    if (!entry)
        ui_error("token exchange returned an unusable response");
    return entry;
}

/* Best-effort: a token no longer stored anywhere would otherwise stay authorized server-side
 * with no way to revoke it from hax. Returns 0 when the endpoint acknowledged the revocation. */
static int revoke_refresh_token(const char *refresh_token, const char *busy_label)
{
    trace_register_secret(refresh_token);
    char *request_body = dump_compact(hax::json::value{hax::json::object{
        {"token", refresh_token},
        {"token_type_hint", "refresh_token"},
        {"client_id", CODEX_OAUTH_CLIENT_ID},
    }});
    struct busy *busy = busy_begin(busy_label);
    char *response = NULL;
    long status = 0;
    int result = http_post(CODEX_REVOKE_ENDPOINT, NULL, "application/json", request_body,
                           strlen(request_body), CODEX_REVOKE_TIMEOUT_S, 0, busy_tick, NULL,
                           &response, &status);
    busy_end(busy);
    free(response);
    free(request_body);
    return result == 0 && status >= 200 && status < 300 ? 0 : -1;
}

struct login_install_ctx {
    std::string_view replacement;
    std::optional<std::string> superseded; /* out: refresh token the install displaced */
};

/* Capturing the displaced token in the same transaction that installs the replacement keeps the
 * revocation aimed at exactly what was overwritten, even against a concurrent rotation. */
static enum cred_store_verdict install_login_entry(std::optional<std::string_view> entry,
                                                   std::string *replacement, void *user)
{
    struct login_install_ctx *ctx = (struct login_install_ctx *)user;
    std::optional<std::string> previous =
        entry ? entry_field(*entry, "refresh_token") : std::nullopt;
    std::optional<std::string> incoming = entry_field(ctx->replacement, "refresh_token");
    if (previous && !previous->empty() &&
        (!incoming || incoming->empty() || *previous != *incoming))
        ctx->superseded = std::move(*previous);
    *replacement = std::string(ctx->replacement);
    return CRED_STORE_WRITE;
}

int codex_login_run(void)
{
    struct codex_device_auth device_auth;
    int result = request_usercode(&device_auth);
    if (result != 0)
        return result;

    /* Until approval completes, this pair lets any client claim the authorization, so it must not
     * reach a trace file through the poll request body. */
    trace_register_secret(device_auth.device_auth_id);
    trace_register_secret(device_auth.user_code);

    const char *clipboard_error = NULL;
    int copied =
        clipboard_copy(device_auth.user_code, strlen(device_auth.user_code), &clipboard_error) == 0;
    printf("  open " ANSI_BOLD CODEX_DEVICE_VERIFY_URL ANSI_BOLD_OFF "\n");
    printf("  and enter code " ANSI_BOLD "%s" ANSI_BOLD_OFF "%s\n", device_auth.user_code,
           copied ? ANSI_DIM " (copied to clipboard)" ANSI_RESET : "");
    /* A blank line keeps the eventual outcome from running into the instructions. */
    putchar('\n');

    char *authorization_code = NULL;
    char *code_verifier = NULL;
    result = poll_for_authorization(&device_auth, &authorization_code, &code_verifier);
    codex_device_auth_release(&device_auth);
    if (result != 0)
        return result;

    int cancelled = 0;
    std::optional<std::string> entry =
        exchange_authorization_code(authorization_code, code_verifier, &cancelled);
    free(authorization_code);
    free(code_verifier);
    if (!entry)
        return cancelled ? 1 : -1;

    std::optional<std::string> refresh_token = entry_field(*entry, "refresh_token");
    if (refresh_token)
        trace_register_secret(refresh_token->c_str());
    struct login_install_ctx install = {.replacement = *entry, .superseded = std::nullopt};
    enum cred_store_result stored = cred_store_update("codex", install_login_entry, &install);
    if (stored != CRED_STORE_RESULT_CHANGED) {
        char *path = cred_store_file_path();
        ui_error("cannot write %s", path ? path : "the hax state directory");
        free(path);
        /* The grant would otherwise stay active with its token stored nowhere. */
        if (refresh_token)
            revoke_refresh_token(refresh_token->c_str(), "revoking unsaved login...");
        return -1;
    }
    if (install.superseded)
        revoke_refresh_token(install.superseded->c_str(), "revoking previous login...");

    std::optional<std::string> id_token = entry_field(*entry, "id_token");
    char *email = codex_jwt_email(id_token ? id_token->c_str() : NULL);
    ui_note("logged in%s%s — hax now manages this token", email ? " as " : "", email ? email : "");
    free(email);
    return 0;
}

int codex_logout_run(void)
{
    /* Fetch-and-delete is one transaction so the revocation targets exactly the removed token; a
     * separate read could revoke a token that a concurrent refresh had already rotated away. */
    struct cred_store_read taken = cred_store_take("codex");
    if (taken.status == CRED_STORE_ENTRY_ERROR || taken.status == CRED_STORE_ENTRY_MALFORMED ||
        taken.status == CRED_STORE_ENTRY_INVALID_ARGUMENT) {
        char *path = cred_store_file_path();
        ui_error("cannot update %s", path ? path : "the hax state directory");
        free(path);
        return -1;
    }
    if (taken.status == CRED_STORE_ENTRY_MISSING)
        return 0;

    /* Local removal always succeeds — logging out must work offline — so a failed revocation is
     * reported rather than blocking it. */
    std::optional<std::string> refresh_token = entry_field(taken.json, "refresh_token");
    if (refresh_token && !refresh_token->empty() &&
        revoke_refresh_token(refresh_token->c_str(), "revoking token...") != 0)
        ui_note("could not revoke the token server-side — it expires on its own");
    return 1;
}
