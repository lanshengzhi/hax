/* SPDX-License-Identifier: MIT */
#include "catalog.h"

#include <libgen.h>
#include <optional>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <string_view>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <sys/stat.h>

#include "atomics.h"
#include "catalog_json.h"
#include "config.h"
#include "effort.h"
#include "json.h"
#include "util.h"
#include "system/bg_job.h"
#include "system/fs.h"
#include "transport/http.h"

#define CATALOG_CACHE_FILE "catalog.json"
/* Bound a worker even when process shutdown never cancels it. */
#define CATALOG_FETCH_TIMEOUT_S 30
/* Bounds both the HTTP response and cache-file buffer. */
#define CATALOG_MAX_BYTES (32 * 1024 * 1024)
/* Keep the warning threshold well beyond the refresh interval to ignore transient failures. */
#define CATALOG_STALE_WARN_S (30L * 24 * 60 * 60)

/* ---------------- entry parsing (shared by both tiers) ---------------- */

void catalog_entry_init(struct catalog_entry *entry)
{
    memset(entry, 0, sizeof(*entry));
    entry->cost_input = -1;
    entry->cost_output = -1;
    entry->cost_cache_read = -1;
    entry->cost_cache_write = -1;
    entry->cost_cache_write_1h = -1;
    entry->image_input = CATALOG_SUPPORT_UNKNOWN;
}

static int entry_has_metadata(const struct catalog_entry *entry)
{
    /* A tier-only entry can still price requests above its threshold. */
    return entry->cost_input >= 0 || entry->cost_output >= 0 || entry->cost_cache_read >= 0 ||
           entry->cost_cache_write >= 0 || entry->cost_cache_write_1h >= 0 ||
           entry->context_window > 0 || entry->max_output > 0 ||
           entry->image_input != CATALOG_SUPPORT_UNKNOWN || entry->n_tiers > 0 ||
           entry->efforts.known || entry->api != NULL || entry->interleaved_declared;
}

static void merge_entry(struct catalog_entry *dst, const struct catalog_entry *src)
{
    if (dst->cost_input < 0)
        dst->cost_input = src->cost_input;
    if (dst->cost_output < 0)
        dst->cost_output = src->cost_output;
    if (dst->cost_cache_read < 0)
        dst->cost_cache_read = src->cost_cache_read;
    if (dst->cost_cache_write < 0)
        dst->cost_cache_write = src->cost_cache_write;
    if (dst->cost_cache_write_1h < 0)
        dst->cost_cache_write_1h = src->cost_cache_write_1h;
    if (dst->context_window <= 0)
        dst->context_window = src->context_window;
    if (dst->max_output <= 0)
        dst->max_output = src->max_output;
    if (dst->image_input == CATALOG_SUPPORT_UNKNOWN)
        dst->image_input = src->image_input;
    if (!dst->tiers_declared && src->tiers_declared) {
        memcpy(dst->tiers, src->tiers, sizeof(dst->tiers));
        dst->n_tiers = src->n_tiers;
        dst->tiers_declared = 1;
    }
    /* Whole-list, like tiers and for the same reason: a ladder merged
     * level-by-level from two sources would offer a set that never existed
     * on any one model. */
    if (!dst->efforts.known)
        dst->efforts = src->efforts;
    if (!dst->api)
        dst->api = src->api;
    if (!dst->interleaved_declared) {
        dst->interleaved_field = src->interleaved_field;
        dst->interleaved_declared = src->interleaved_declared;
    }
}

/* ---------------- top-level member extraction ---------------- */

/* Tree-parse only the requested member to keep extraction bounded-memory. The structural byte
 * scan is UTF-8-safe because quotes and backslashes cannot occur inside multibyte sequences. */

static const char *scan_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    return p;
}

/* Advance past the string whose opening '"' is at `p`. Returns the
 * position just past the closing quote, NULL on truncated input. */
static const char *scan_string(const char *p)
{
    for (p++; *p; p++) {
        if (*p == '\\') {
            if (!p[1])
                return NULL;
            p++;
        } else if (*p == '"') {
            return p + 1;
        }
    }
    return NULL;
}

/* Advance past one JSON value starting at `p`: strings and {} / []
 * nesting are honored, everything else is structural-only. NULL on
 * truncated input. */
static const char *scan_value(const char *p)
{
    if (*p == '"')
        return scan_string(p);
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (*p) {
            if (*p == '"') {
                p = scan_string(p);
                if (!p)
                    return NULL;
                continue;
            }
            if (*p == '{' || *p == '[') {
                depth++;
            } else if (*p == '}' || *p == ']') {
                if (--depth == 0)
                    return p + 1;
            }
            p++;
        }
        return NULL;
    }
    /* Scalar token (number / true / false / null): up to a delimiter. */
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\t' && *p != '\n' &&
           *p != '\r')
        p++;
    return p;
}

enum scan_member_result {
    SCAN_MEMBER_INVALID = -1,
    SCAN_MEMBER_MORE,
    SCAN_MEMBER_LAST,
};

struct scanned_member {
    const char *key;
    size_t key_length;
    const char *value_start;
    const char *value_end;
};

/* On success, `cursor` advances to the next key or past the root's closing brace. */
static enum scan_member_result scan_member(const char **cursor, struct scanned_member *member)
{
    const char *p = *cursor;
    if (*p != '"')
        return SCAN_MEMBER_INVALID;
    member->key = p + 1;
    const char *key_end = scan_string(p);
    if (!key_end)
        return SCAN_MEMBER_INVALID;
    member->key_length = (size_t)(key_end - 1 - member->key);
    p = scan_ws(key_end);
    if (*p != ':')
        return SCAN_MEMBER_INVALID;
    p = scan_ws(p + 1);
    member->value_start = p;
    p = scan_value(p);
    if (!p)
        return SCAN_MEMBER_INVALID;
    member->value_end = p;
    p = scan_ws(p);
    if (*p == ',') {
        *cursor = scan_ws(p + 1);
        return SCAN_MEMBER_MORE;
    }
    if (*p == '}') {
        *cursor = p + 1;
        return SCAN_MEMBER_LAST;
    }
    return SCAN_MEMBER_INVALID;
}

/* Position of the first member key in object `text`; NULL for an empty
 * object or a non-object. */
static const char *scan_first_member(const char *text)
{
    const char *p = scan_ws(text);
    if (*p != '{')
        return NULL;
    p = scan_ws(p + 1);
    return *p == '"' ? p : NULL;
}

static std::optional<std::string> decode_scanned_key(const struct scanned_member &member)
{
    std::string encoded(member.key - 1, member.key_length + 2);
    auto decoded = hax::json::parse<std::string>(
        encoded, {.source = "model catalog key", .max_input_bytes = 0});
    return decoded ? std::optional<std::string>(std::move(*decoded)) : std::nullopt;
}

static std::optional<std::string> copy_valid_member_value(const struct scanned_member &member)
{
    std::string value(member.value_start, member.value_end - member.value_start);
    auto valid = hax::json::validate(value, {.source = "model catalog", .max_input_bytes = 0});
    return valid ? std::optional<std::string>(std::move(value)) : std::nullopt;
}

/* Scan every member after the requested key so malformed trailing data cannot be accepted. The
 * structural scan stays bounded-memory; the adapter validates each key and value. */
std::optional<std::string> catalog_extract_member(const char *text, const char *key)
{
    if (!text || !key || !*key)
        return std::nullopt;
    const char *cursor = scan_first_member(text);
    if (!cursor)
        return std::nullopt;

    std::optional<std::string> found;
    for (;;) {
        struct scanned_member member;
        enum scan_member_result result = scan_member(&cursor, &member);
        if (result == SCAN_MEMBER_INVALID)
            return std::nullopt;
        auto member_key = decode_scanned_key(member);
        auto member_value = copy_valid_member_value(member);
        if (!member_key || !member_value)
            return std::nullopt;
        if (*member_key == key)
            found = std::move(*member_value);
        if (result == SCAN_MEMBER_LAST)
            return *scan_ws(cursor) == '\0' ? found : std::nullopt;
    }
}

/* Validate every member and trailing byte before replacing a working snapshot. Requiring a
 * provider-shaped member also rejects JSON error payloads. Parse each member separately to retain
 * the bounded-memory property of lookups. */
static int catalog_text_valid(const char *text)
{
    struct validated_provider {
        std::string key;
        int has_models;
    };
    std::vector<validated_provider> providers;
    const char *cursor = scan_first_member(text);
    if (!cursor)
        return 0;
    for (;;) {
        struct scanned_member member;
        enum scan_member_result result = scan_member(&cursor, &member);
        if (result == SCAN_MEMBER_INVALID)
            return 0;
        auto member_key = decode_scanned_key(member);
        if (!member_key)
            return 0;
        auto value = copy_valid_member_value(member);
        if (!value)
            return 0;

        const int has_models = hax::catalog_json::provider_has_models(*value);
        int replaced = 0;
        for (auto &provider : providers) {
            if (provider.key == *member_key) {
                provider.has_models = has_models;
                replaced = 1;
                break;
            }
        }
        if (!replaced)
            providers.push_back({std::move(*member_key), has_models});
        if (result == SCAN_MEMBER_LAST)
            break;
    }

    for (const auto &provider : providers)
        if (provider.has_models)
            return *scan_ws(cursor) == '\0';
    return 0;
}

/* ---------------- config tier: the catalog.models block ---------------- */

static void fill_from_config(const char *provider_id, const char *model,
                             struct catalog_entry *entry)
{
    const hax::json::value *models = config_json_node("catalog.models");
    const hax::json::value *provider = models ? models->find(provider_id) : NULL;
    if (!provider || !provider->is_object())
        return;

    auto encoded = hax::json::serialize_value(*provider, {.source = "catalog configuration"});
    if (encoded)
        hax::catalog_json::fill_config_model(*encoded, model, entry);
}

/* ---------------- cache tier: the fetched snapshot ---------------- */

/* Return an owned provider JSON slice, or an empty optional when the snapshot is unavailable or
 * lacks the provider. The slice is parsed by the private catalog adapter. */
static std::optional<std::string> cache_provider_slice(const char *provider_id)
{
    char *path = xdg_hax_cache_path(CATALOG_CACHE_FILE);
    if (!path)
        return std::nullopt;
    size_t len;
    int truncated;
    char *text = slurp_file_capped(path, CATALOG_MAX_BYTES, &len, &truncated);
    free(path);
    if (!text)
        return std::nullopt;
    auto provider = truncated ? std::nullopt : catalog_extract_member(text, provider_id);
    free(text);
    return provider;
}

static void fill_from_slice(std::string_view provider, const char *model,
                            struct catalog_entry *entry)
{
    hax::catalog_json::fill_provider_model(provider, model, entry);
}

static void fill_from_cache(const char *provider_id, const char *model, struct catalog_entry *entry)
{
    auto provider = cache_provider_slice(provider_id);
    if (provider)
        fill_from_slice(*provider, model, entry);
}

/* ---------------- cache-tier memo (foreground thread) ---------------- */

/* Only the foreground accesses the memo; the worker publishes refreshes via the generation. */
struct memo_entry {
    char *provider_id;
    char *model;
    struct catalog_entry entry;
    int resolved;
};

static struct memo_entry *g_memo;
static size_t g_memo_count, g_memo_capacity;

/* Bumped by the fetch worker when a fresh snapshot lands; synced on lookup
 * so memoized misses don't outlive the refresh that could turn them into
 * hits. */
static atomic_int g_cache_generation;
static int g_memo_generation;

static void memo_clear(void)
{
    for (size_t i = 0; i < g_memo_count; i++) {
        free(g_memo[i].provider_id);
        free(g_memo[i].model);
    }
    free(g_memo);
    g_memo = NULL;
    g_memo_count = g_memo_capacity = 0;
}

static struct memo_entry *memo_find(const char *provider_id, const char *model)
{
    for (size_t i = 0; i < g_memo_count; i++)
        if (strcmp(g_memo[i].provider_id, provider_id) == 0 && strcmp(g_memo[i].model, model) == 0)
            return &g_memo[i];
    return NULL;
}

static void memo_add(const char *provider_id, const char *model, const struct catalog_entry *entry,
                     int resolved)
{
    if (g_memo_count == g_memo_capacity) {
        g_memo_capacity = g_memo_capacity ? g_memo_capacity * 2 : 4;
        g_memo = (memo_entry *)xrealloc(g_memo, g_memo_capacity * sizeof(*g_memo));
    }
    struct memo_entry *memo = &g_memo[g_memo_count++];
    memo->provider_id = xstrdup(provider_id);
    memo->model = xstrdup(model);
    memo->entry = *entry;
    memo->resolved = resolved;
}

static int cache_lookup(const char *provider_id, const char *model, struct catalog_entry *out)
{
    int generation = atomic_load(&g_cache_generation);
    if (generation != g_memo_generation) {
        memo_clear();
        g_memo_generation = generation;
    }
    struct memo_entry *memo = memo_find(provider_id, model);
    if (memo) {
        *out = memo->entry;
        return memo->resolved;
    }
    catalog_entry_init(out);
    fill_from_cache(provider_id, model, out);
    int resolved = entry_has_metadata(out);
    memo_add(provider_id, model, out, resolved);
    return resolved;
}

/* ---------------- metadata resolution ---------------- */

/* Keep single and batch resolution policy independent of how cached metadata is loaded. */
struct cache_source {
    int (*fill)(const struct cache_source *source, const char *model, struct catalog_entry *out);
    const char *provider_id;
    const std::string *slice;
};

static int cache_source_memo(const struct cache_source *source, const char *model,
                             struct catalog_entry *out)
{
    return cache_lookup(source->provider_id, model, out);
}

static int cache_source_slice(const struct cache_source *source, const char *model,
                              struct catalog_entry *out)
{
    catalog_entry_init(out);
    if (source->slice)
        fill_from_slice(*source->slice, model, out);
    return entry_has_metadata(out);
}

/* Config fields take precedence; a NULL cache source resolves from config alone. */
static int resolve_entry(const char *provider_id, const char *model,
                         const struct cache_source *cache, struct catalog_entry *out)
{
    catalog_entry_init(out);
    if (!provider_id || !*provider_id || !model || !*model)
        return 0;
    /* Always consult the cache: some fields (the SDK-derived api hint) exist only there, and
     * merging fills gaps without disturbing configured values. */
    fill_from_config(provider_id, model, out);
    struct catalog_entry cached;
    if (cache && cache->fill(cache, model, &cached))
        merge_entry(out, &cached);
    return entry_has_metadata(out);
}

int catalog_lookup(const char *provider_id, const char *model, struct catalog_entry *out)
{
    struct cache_source memo = {.fill = cache_source_memo, .provider_id = provider_id};
    return resolve_entry(provider_id, model, &memo, out) ? 0 : -1;
}

void catalog_lookup_many(const char *provider_id, const char *const *models, size_t model_count,
                         struct catalog_entry *out, int *found)
{
    for (size_t i = 0; i < model_count; i++) {
        catalog_entry_init(&out[i]);
        if (found)
            found[i] = 0;
    }
    if (!provider_id || !*provider_id || model_count == 0)
        return;

    /* A missing provider slice falls back to config without retrying the file per model. */
    auto provider = cache_provider_slice(provider_id);
    struct cache_source cache = {.fill = cache_source_slice,
                                 .provider_id = provider_id,
                                 .slice = provider ? &*provider : NULL};
    for (size_t i = 0; i < model_count; i++) {
        int resolved = resolve_entry(provider_id, models[i], provider ? &cache : NULL, &out[i]);
        if (found)
            found[i] = resolved;
    }
}

struct price_rates {
    double input;
    double output;
    double cache_read;
    double cache_write;
    double cache_write_1h;
};

static struct price_rates price_rates_for_input(const struct catalog_entry *entry,
                                                long input_tokens)
{
    struct price_rates rates = {
        .input = entry->cost_input,
        .output = entry->cost_output,
        .cache_read = entry->cost_cache_read,
        .cache_write = entry->cost_cache_write,
        .cache_write_1h = entry->cost_cache_write_1h,
    };
    long matched_threshold = -1;
    for (int i = 0; i < entry->n_tiers; i++) {
        const struct catalog_tier *tier = &entry->tiers[i];
        if (tier->context_threshold <= 0 || input_tokens <= tier->context_threshold ||
            tier->context_threshold <= matched_threshold)
            continue;
        matched_threshold = tier->context_threshold;
        rates.input = tier->cost_input >= 0 ? tier->cost_input : entry->cost_input;
        rates.output = tier->cost_output >= 0 ? tier->cost_output : entry->cost_output;
        rates.cache_read =
            tier->cost_cache_read >= 0 ? tier->cost_cache_read : entry->cost_cache_read;
        rates.cache_write =
            tier->cost_cache_write >= 0 ? tier->cost_cache_write : entry->cost_cache_write;
        rates.cache_write_1h =
            tier->cost_cache_write_1h >= 0 ? tier->cost_cache_write_1h : entry->cost_cache_write_1h;
    }
    return rates;
}

static int cache_write_replaces_input(double input_rate, double write_rate)
{
    return input_rate < 0 || write_rate < 0 || write_rate >= input_rate;
}

int catalog_cache_write_replaces_input(const struct catalog_entry *entry)
{
    return cache_write_replaces_input(entry->cost_input, entry->cost_cache_write);
}

double catalog_price(const struct catalog_entry *entry, long input_tokens, long output_tokens,
                     long cache_read_tokens, long cache_write_tokens, long cache_write_1h_tokens,
                     struct catalog_split *split)
{
    if (split)
        *split = (struct catalog_split){0};

    struct price_rates rates = price_rates_for_input(entry, input_tokens);
    if (rates.input < 0 || rates.output < 0)
        return -1;

    input_tokens = input_tokens > 0 ? input_tokens : 0;
    output_tokens = output_tokens > 0 ? output_tokens : 0;
    cache_read_tokens = cache_read_tokens > 0 ? cache_read_tokens : 0;
    cache_write_tokens = cache_write_tokens > 0 ? cache_write_tokens : 0;
    cache_write_1h_tokens = cache_write_1h_tokens > 0 ? cache_write_1h_tokens : 0;
    if (cache_write_1h_tokens > cache_write_tokens)
        cache_write_1h_tokens = cache_write_tokens;

    if (rates.cache_read < 0)
        rates.cache_read = rates.input;
    int writes_replace_input = cache_write_replaces_input(rates.input, rates.cache_write);
    if (rates.cache_write < 0)
        rates.cache_write = rates.input;
    if (rates.cache_write_1h < 0)
        rates.cache_write_1h = 2 * rates.input; /* Anthropic's standard 1h write multiplier. */

    long uncached_input_tokens =
        input_tokens - cache_read_tokens - (writes_replace_input ? cache_write_tokens : 0);
    if (uncached_input_tokens < 0)
        uncached_input_tokens = 0;

    double input_cost = (double)uncached_input_tokens * rates.input / 1e6;
    double cache_read_cost = (double)cache_read_tokens * rates.cache_read / 1e6;
    double cache_write_cost =
        ((double)(cache_write_tokens - cache_write_1h_tokens) * rates.cache_write +
         (double)cache_write_1h_tokens * rates.cache_write_1h) /
        1e6;
    double output_cost = (double)output_tokens * rates.output / 1e6;
    if (split) {
        split->uncached_input_tokens = uncached_input_tokens;
        split->cost_input = input_cost;
        split->cost_cache_read = cache_read_cost;
        split->cost_cache_write = cache_write_cost;
        split->cost_output = output_cost;
    }
    return input_cost + cache_read_cost + cache_write_cost + output_cost;
}

/* ---------------- background fetch ---------------- */

static struct bg_job *g_fetch_job;
static int g_prefetch_attempted;
/* bg_job has no timed join, so catalog_drain polls this worker-owned flag. */
static atomic_int g_fetch_done;

struct fetch_args {
    char *url;
    char *path;
};

static void fetch_args_free(struct fetch_args *args)
{
    if (!args)
        return;
    free(args->url);
    free(args->path);
    free(args);
}

/* Rename a sibling temporary file so concurrent readers never observe a partial snapshot. */
static int write_cache_atomic(const char *path, const char *body, size_t body_length)
{
    char *path_copy = xstrdup(path);
    fs_mkdir_p(dirname(path_copy));
    free(path_copy);

    char *temp_path = xasprintf("%s.tmp.XXXXXX", path);
    int fd = mkstemp(temp_path);
    if (fd < 0) {
        free(temp_path);
        return -1;
    }
    int result = write_all(fd, body, body_length);
    if (close(fd) != 0)
        result = -1;
    if (result == 0 && rename(temp_path, path) != 0)
        result = -1;
    if (result != 0)
        unlink(temp_path);
    free(temp_path);
    return result;
}

static void fetch_worker(struct bg_job *job, void *arg)
{
    struct fetch_args *args = (struct fetch_args *)arg;
    if (!bg_job_cancel_requested(job)) {
        char *body = NULL;
        if (http_get(args->url, NULL, CATALOG_FETCH_TIMEOUT_S, CATALOG_MAX_BYTES,
                     bg_job_cancel_tick, job, &body, NULL) == 0 &&
            body) {
            if (catalog_text_valid(body) && write_cache_atomic(args->path, body, strlen(body)) == 0)
                atomic_fetch_add(&g_cache_generation, 1);
        }
        free(body);
    }
    fetch_args_free(args);
    atomic_store(&g_fetch_done, 1);
}

long catalog_prefetch(void)
{
    if (g_prefetch_attempted)
        return 0;
    g_prefetch_attempted = 1;

    const char *url = config_str("catalog.url");
    if (!url || !*url)
        return 0;
    long refresh_ms = config_duration_ms("catalog.refresh");
    if (refresh_ms <= 0)
        return 0;
    char *path = xdg_hax_cache_path(CATALOG_CACHE_FILE);
    if (!path)
        return 0;

    long stale_days = 0;
    struct fetch_args *args = NULL;
    struct stat status;
    if (stat(path, &status) == 0) {
        long snapshot_age_s = (long)(time(NULL) - status.st_mtime);
        if (snapshot_age_s < refresh_ms / 1000)
            goto out;
        if (snapshot_age_s > CATALOG_STALE_WARN_S)
            stale_days = snapshot_age_s / (24L * 60 * 60);
    }

    args = (struct fetch_args *)xcalloc(1, sizeof(*args));
    args->url = xstrdup(url);
    args->path = path;
    path = NULL;
    g_fetch_job = bg_job_spawn(fetch_worker, args);
    if (!g_fetch_job)
        fetch_args_free(args);

out:
    free(path);
    return stale_days;
}

void catalog_wait(long max_wait_ms)
{
    if (!g_fetch_job)
        return;
    for (long waited_ms = 0; waited_ms < max_wait_ms && !atomic_load(&g_fetch_done);) {
        long delay_ms = max_wait_ms - waited_ms;
        if (delay_ms > 20)
            delay_ms = 20;
        struct timespec delay = {delay_ms / 1000, (delay_ms % 1000) * 1000 * 1000};
        nanosleep(&delay, NULL);
        waited_ms += delay_ms;
    }
}

void catalog_drain(long max_wait_ms)
{
    if (!g_fetch_job)
        return;
    catalog_wait(max_wait_ms);
    if (!atomic_load(&g_fetch_done))
        bg_job_cancel(g_fetch_job);
    bg_job_join(g_fetch_job);
    g_fetch_job = NULL;
}

void catalog_shutdown(void)
{
    if (g_fetch_job) {
        bg_job_cancel(g_fetch_job);
        bg_job_join(g_fetch_job);
        g_fetch_job = NULL;
    }
    memo_clear();
    g_memo_generation = atomic_load(&g_cache_generation);
}
