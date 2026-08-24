/* SPDX-License-Identifier: MIT */
#include "session.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <jansson.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>

#include "config.h"
#include "provider.h"
#include "session_control.h"
#include "session_item.h"
#include "session_prune.h"
#include "util.h"
#include "version.h"
#include "system/fs.h"
#include "system/git.h"
#include "text/width.h"

/* struct stat's sub-second mtime field is spelled differently across
 * platforms. Used to break ties between sessions created in the same
 * second so --continue / the picker reliably pick the most recent. */
#if defined(__APPLE__)
#define ST_MTIME_NSEC(st) ((long)(st).st_mtimespec.tv_nsec)
#else
#define ST_MTIME_NSEC(st) ((long)(st).st_mtim.tv_nsec)
#endif

/* These helpers serialize session control records, not conversation items. */
static void json_set_optional_string(json_t *object, const char *key, const char *value)
{
    if (!value)
        return;
    json_t *string = json_string(value);
    if (string)
        json_object_set_new(object, key, string);
}

/* Unknown origins remain ordinary items; treating them as synthetic could hide a typed prompt. */
static const struct {
    enum item_origin origin;
    const char *name;
} ORIGIN_NAMES[] = {
    {ITEM_ORIGIN_COMPACT_SEED, "compact_seed"}, {ITEM_ORIGIN_CONTINUATION, "continuation"},
    {ITEM_ORIGIN_INTERRUPTED, "interrupted"},   {ITEM_ORIGIN_SKIPPED, "skipped"},
    {ITEM_ORIGIN_REFUSED, "refused"},           {ITEM_ORIGIN_SUMMARIZED, "summarized"},
    {ITEM_ORIGIN_TASK_NOTE, "task_note"},
};

static enum item_origin json_get_item_origin(const json_t *object)
{
    const char *name = json_string_value(json_object_get(object, "origin"));
    if (!name)
        return ITEM_ORIGIN_NONE;
    for (size_t i = 0; i < sizeof(ORIGIN_NAMES) / sizeof(ORIGIN_NAMES[0]); i++)
        if (strcmp(ORIGIN_NAMES[i].name, name) == 0)
            return ORIGIN_NAMES[i].origin;
    return ITEM_ORIGIN_NONE;
}

void session_meta_free(struct session_meta *meta)
{
    if (!meta)
        return;
    free(meta->id);
    free(meta->cwd);
    free(meta->provider);
    free(meta->model);
    free(meta->effort);
    free(meta->preset);
    memset(meta, 0, sizeof(*meta));
}

/* Only the explicit half of the `no_session` tri-state: "auto" isn't a truthy
 * spelling, so it reads as "record" here and the callers who know which
 * provider is live resolve what it means (agent_recording_enabled). */
static int sessions_disabled(void)
{
    return config_bool("no_session");
}

/* The hash disambiguates the readable but non-injective path slug. */
#define CWD_SLUG_MAX 80
static char *encode_cwd(const char *cwd)
{
    if (!cwd || !*cwd)
        cwd = "unknown";

    uint64_t hash = 1469598103934665603ULL;
    for (const char *cursor = cwd; *cursor; cursor++) {
        hash ^= (unsigned char)*cursor;
        hash *= 1099511628211ULL;
    }

    const char *relative = cwd;
    while (*relative == '/')
        relative++;
    if (!*relative)
        relative = "root";
    char slug[CWD_SLUG_MAX];
    size_t length = 0;
    for (; relative[length] && length < sizeof(slug) - 1; length++)
        slug[length] = relative[length] == '/' ? '-' : relative[length];
    slug[length] = '\0';

    return xasprintf("%s.%016llx", slug, (unsigned long long)hash);
}

static char *session_directory(const char *cwd)
{
    char *encoded_cwd = encode_cwd(cwd);
    char *relative_path = xasprintf("sessions/%s", encoded_cwd);
    char *directory = xdg_hax_state_path(relative_path);
    free(relative_path);
    free(encoded_cwd);
    return directory;
}

static int is_uuid(const char *value, size_t length)
{
    if (length != 36)
        return 0;
    for (size_t i = 0; i < length; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-')
                return 0;
        } else if (!isxdigit((unsigned char)value[i])) {
            return 0;
        }
    }
    return 1;
}

static int has_session_timestamp(const char *value)
{
    static const char shape[] = "dddd-dd-ddTdd-dd-ddZ";
    for (size_t i = 0; i < sizeof(shape) - 1; i++) {
        if (shape[i] == 'd') {
            if (!isdigit((unsigned char)value[i]))
                return 0;
        } else if (value[i] != shape[i]) {
            return 0;
        }
    }
    return 1;
}

/* Validate the whole basename so pruning cannot claim unrelated UUID-suffixed JSONL files. */
static char *session_id_from_path(const char *path)
{
    const char *basename = strrchr(path, '/');
    basename = basename ? basename + 1 : path;
    if (strlen(basename) != 63 || !has_session_timestamp(basename) || basename[20] != '_' ||
        strcmp(basename + 57, ".jsonl") != 0 || !is_uuid(basename + 21, 36))
        return NULL;
    char *id = (char *)xmalloc(37);
    memcpy(id, basename + 21, 36);
    id[36] = '\0';
    return id;
}

int session_path_is_standard(const char *path)
{
    char *id = session_id_from_path(path);
    int standard = id != NULL;
    free(id);
    return standard;
}

int session_touch(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return -1;
    /* If flock is unsupported, pruning also fails closed; touching can still
     * proceed. On supported filesystems this waits out an in-flight prune. */
    (void)flock(fd, LOCK_SH);
    struct stat st;
    int result = fstat(fd, &st) == 0 && st.st_nlink > 0 ? futimens(fd, NULL) : -1;
    close(fd);
    return result;
}

struct session_log {
    FILE *file; /* NULL until a fresh session is materialized */
    char *path;
    int header_written;
    int selection_pending;
    size_t written_items;
    char *id;
    char *cwd;
    char *timestamp;
    char *provider;
    char *model;
    char *model_label;
    char *effort;
    char *preset;
};

/* Selection fields survive /new; identity and writer state do not. */
static int prepare_fresh_session(struct session_log *log)
{
    char *cwd = getcwd(NULL, 0);
    if (!cwd)
        return -1;
    char uuid[37];
    time_t now = 0;
    char *directory = session_directory(cwd);
    if (!directory) {
        free(cwd);
        return -1;
    }

    gen_uuid_v4(uuid);
    now = time(NULL);
    struct tm utc;
    gmtime_r(&now, &utc);
    char filename_time[32];
    char header_time[32];
    /* Colons are not portable in filenames. */
    strftime(filename_time, sizeof(filename_time), "%Y-%m-%dT%H-%M-%SZ", &utc);
    strftime(header_time, sizeof(header_time), "%Y-%m-%dT%H:%M:%SZ", &utc);

    free(log->path);
    log->path = xasprintf("%s/%s_%s.jsonl", directory, filename_time, uuid);
    free(directory);
    free(log->id);
    log->id = xstrdup(uuid);
    free(log->cwd);
    log->cwd = cwd;
    free(log->timestamp);
    log->timestamp = xstrdup(header_time);
    log->file = NULL;
    log->header_written = 0;
    log->selection_pending = 0;
    log->written_items = 0;
    return 0;
}

enum session_file_mode {
    SESSION_FILE_CREATE,
    SESSION_FILE_APPEND,
};

static FILE *open_session_file(const char *path, enum session_file_mode mode);

struct session_log *session_log_open(const char *provider, const char *model,
                                     const char *model_label, const char *effort,
                                     const char *preset)
{
    if (sessions_disabled())
        return NULL;
    struct session_log *log = (struct session_log *)xcalloc(1, sizeof(*log));
    log->provider = provider ? xstrdup(provider) : NULL;
    log->model = model ? xstrdup(model) : NULL;
    log->model_label = model_label ? xstrdup(model_label) : NULL;
    log->effort = effort ? xstrdup(effort) : NULL;
    log->preset = (preset && *preset) ? xstrdup(preset) : NULL;
    if (prepare_fresh_session(log) < 0) {
        session_log_close(log);
        return NULL;
    }
    return log;
}

struct session_log *session_log_resume(const char *path, const char *provider, const char *model,
                                       const char *effort, const char *preset,
                                       size_t loaded_item_count)
{
    if (sessions_disabled())
        return NULL;
    FILE *file = open_session_file(path, SESSION_FILE_APPEND);
    if (!file) {
        hax_warn("cannot append to session '%s'; this run won't be recorded", path);
        return NULL;
    }
    struct session_log *log = (struct session_log *)xcalloc(1, sizeof(*log));
    log->file = file;
    log->path = xstrdup(path);
    log->id = session_id_from_path(path);
    log->header_written = 1;
    log->written_items = loaded_item_count;
    log->provider = provider ? xstrdup(provider) : NULL;
    log->model = model ? xstrdup(model) : NULL;
    log->effort = effort ? xstrdup(effort) : NULL;
    log->preset = (preset && *preset) ? xstrdup(preset) : NULL;
    return log;
}

/* Session contents may contain secrets, so both new and resumed files are owner-only. */
static FILE *open_session_file(const char *path, enum session_file_mode mode)
{
    /* Append omits O_CREAT so a removed session is not recreated without a header. */
    FILE *file;
    int flags = O_CLOEXEC;
    flags |= mode == SESSION_FILE_APPEND ? O_RDWR | O_APPEND : O_CREAT | O_WRONLY | O_TRUNC;
    int fd = open(path, flags, 0600);
    if (fd < 0)
        return NULL;
    (void)fchmod(fd, 0600);

    /* A successful pruner lock may already refer to an unlinked inode. */
    if (flock(fd, LOCK_SH) == 0) {
        struct stat locked_stat;
        if (fstat(fd, &locked_stat) != 0 || locked_stat.st_nlink == 0)
            goto error;
    }
    if (mode == SESSION_FILE_APPEND) {
        /* Separate a partial crash record from the next valid JSON object. */
        struct stat file_stat;
        char last_byte;
        if (fstat(fd, &file_stat) != 0)
            goto error;
        if (file_stat.st_size > 0 && (pread(fd, &last_byte, 1, file_stat.st_size - 1) != 1 ||
                                      (last_byte != '\n' && write(fd, "\n", 1) != 1)))
            goto error;
    }
    file = fdopen(fd, mode == SESSION_FILE_APPEND ? "a" : "w");
    if (!file)
        goto error;
    setvbuf(file, NULL, _IOLBF, 0);
    return file;

error:
    close(fd);
    return NULL;
}

static int materialize_log(struct session_log *log)
{
    if (log->file)
        return 0;
    if (!log->path)
        return -1;

    char *filename_separator = strrchr(log->path, '/');
    if (filename_separator) {
        *filename_separator = '\0';
        int result = fs_mkdir_p(log->path);
        if (result == 0) {
            /* Project paths in directory names must not be visible to other users. */
            (void)chmod(log->path, 0700);
            char *sessions_separator = strrchr(log->path, '/');
            if (sessions_separator) {
                *sessions_separator = '\0';
                (void)chmod(log->path, 0700);
                *sessions_separator = '/';
            }
        }
        *filename_separator = '/';
        if (result < 0)
            return -1;
    }
    log->file = open_session_file(log->path, SESSION_FILE_CREATE);
    return log->file ? 0 : -1;
}

/* Recorded only when it says something the wire id doesn't, so a reader can treat its absence as
 * "the id is the label" — which is also what files predating labels mean. */
static const char *differing_model_label(const struct session_log *log)
{
    if (!log->model_label || !log->model || strcmp(log->model_label, log->model) == 0)
        return NULL;
    return log->model_label;
}

static int write_json_line(FILE *file, const json_t *object)
{
    char *json = json_dumps(object, JSON_COMPACT);
    if (!json)
        return -1;
    int result = fputs(json, file) == EOF || fputc('\n', file) == EOF ? -1 : 0;
    free(json);
    return result;
}

static int write_text_line(FILE *file, std::string_view text)
{
    return fwrite(text.data(), 1, text.size(), file) != text.size() || fputc('\n', file) == EOF ? -1
                                                                                                : 0;
}

static int write_header(struct session_log *log)
{
    json_t *header = json_object();
    json_object_set_new(header, "type", json_string("session"));
    json_object_set_new(header, "version", json_integer(SESSION_FORMAT_VERSION));
    json_object_set_new(header, "hax_version", json_string(HAX_VERSION));
    json_set_optional_string(header, "id", log->id);
    json_set_optional_string(header, "timestamp", log->timestamp);
    json_set_optional_string(header, "cwd", log->cwd);
    json_set_optional_string(header, "provider", log->provider);
    json_set_optional_string(header, "model", log->model);
    json_set_optional_string(header, "model_label", differing_model_label(log));
    json_set_optional_string(header, "effort", log->effort);
    json_set_optional_string(header, "preset", log->preset);

    /* Probed at materialization rather than at open: the position recorded is the one the
     * conversation actually started from, and runs that never send a message pay nothing. */
    struct git_state git;
    git_state_probe(&git);
    json_set_optional_string(header, "git_branch", git.branch);
    json_set_optional_string(header, "git_commit", git.commit);
    json_set_optional_string(header, "git_subject", git.subject);
    git_state_free(&git);

    int result = write_json_line(log->file, header);
    json_decref(header);
    return result;
}

/* Same-string test tolerating NULL on either side, with "" and NULL treated
 * as the same absence — the selection fields arrive from config resolution,
 * where an unset value can be spelled either way. */
static int optional_strings_equal(const char *a, const char *b)
{
    if (!a || !*a)
        return !b || !*b;
    return b && strcmp(a, b) == 0;
}

static int write_selection(struct session_log *log)
{
    json_t *selection = json_object();
    json_object_set_new(selection, "type", json_string("selection"));
    json_set_optional_string(selection, "provider", log->provider);
    json_set_optional_string(selection, "model", log->model);
    json_set_optional_string(selection, "model_label", differing_model_label(log));
    json_set_optional_string(selection, "effort", log->effort);
    json_set_optional_string(selection, "preset", log->preset);
    int result = write_json_line(log->file, selection);
    json_decref(selection);
    return result;
}

static int selection_matches_log(const struct session_meta *meta, const struct session_log *log)
{
    return optional_strings_equal(meta->provider, log->provider) &&
           optional_strings_equal(meta->model, log->model) &&
           optional_strings_equal(meta->effort, log->effort) &&
           optional_strings_equal(meta->preset, log->preset);
}

void session_log_append(struct session_log *log, const struct item *items, size_t item_count)
{
    if (!log || item_count <= log->written_items || materialize_log(log) < 0)
        return;
    if (!log->header_written) {
        if (write_header(log) < 0)
            return;
        log->header_written = 1;
    }
    if (log->selection_pending) {
        if (write_selection(log) < 0)
            return;
        log->selection_pending = 0;
    }
    for (size_t i = log->written_items; i < item_count; i++) {
        std::string encoded;
        if (session_item_encode(&items[i], &encoded) < 0 || write_text_line(log->file, encoded) < 0)
            return;
        log->written_items = i + 1;
    }
}

void session_log_set_meta(struct session_log *log, const char *provider, const char *model,
                          const char *model_label, const char *effort, const char *preset)
{
    if (!log)
        return;
    /* A label renders the model it belongs to, so it cannot differ on its own: keep it current
     * without letting it stage a selection record. */
    free(log->model_label);
    log->model_label = model_label ? xstrdup(model_label) : NULL;

    if (optional_strings_equal(log->provider, provider) &&
        optional_strings_equal(log->model, model) && optional_strings_equal(log->effort, effort) &&
        optional_strings_equal(log->preset, preset))
        return;

    free(log->provider);
    log->provider = provider ? xstrdup(provider) : NULL;
    free(log->model);
    log->model = model ? xstrdup(model) : NULL;
    free(log->effort);
    log->effort = effort ? xstrdup(effort) : NULL;
    free(log->preset);
    log->preset = (preset && *preset) ? xstrdup(preset) : NULL;

    /* Defer the record until this selection produces an item. */
    if (log->header_written)
        log->selection_pending = 1;
}

void session_log_discard_selection(struct session_log *log)
{
    if (log)
        log->selection_pending = 0;
}

void session_log_reset(struct session_log *log)
{
    if (!log)
        return;
    if (log->file) {
        fclose(log->file);
        log->file = NULL;
    }
    if (prepare_fresh_session(log) < 0) {
        /* State dir vanished mid-run (unlikely) — mark unavailable so
         * subsequent appends no-op rather than crash. */
        free(log->path);
        log->path = NULL;
    }
}

void session_log_close(struct session_log *log)
{
    if (!log)
        return;
    if (log->file)
        fclose(log->file);
    free(log->path);
    free(log->id);
    free(log->cwd);
    free(log->timestamp);
    free(log->provider);
    free(log->model);
    free(log->model_label);
    free(log->effort);
    free(log->preset);
    free(log);
}

const char *session_log_path(const struct session_log *log)
{
    return log ? log->path : NULL;
}

const char *session_log_resume_hint(const struct session_log *log)
{
    if (!log || !log->header_written)
        return NULL;
    return log->id;
}

/* Keep this predicate aligned with agent.cpp's in-memory typed-prompt scan. */
static int json_line_is_typed_prompt(const json_t *object)
{
    const char *kind = json_string_value(json_object_get(object, "kind"));
    return kind && strcmp(kind, "user") == 0 && json_get_item_origin(object) == ITEM_ORIGIN_NONE;
}

/* The cut includes the retained turn's response and excludes the next turn's boundary. */
static long find_turn_cut_offset(const char *path, size_t keep_turns)
{
    FILE *file = fopen(path, "r");
    if (!file)
        return -1;

    char *line = NULL;
    size_t line_capacity = 0;
    ssize_t bytes_read;
    long current_offset = 0;
    long previous_offset = -1;
    int previous_was_boundary = 0;
    long cut_offset = -1;
    size_t turn_count = 0;

    while ((bytes_read = getline(&line, &line_capacity, file)) >= 0) {
        long line_offset = current_offset;
        current_offset += bytes_read;

        int is_boundary = 0;
        int is_typed_prompt = 0;
        json_t *object = json_loads(line, 0, NULL);
        if (object) {
            const char *kind = json_string_value(json_object_get(object, "kind"));
            if (kind && strcmp(kind, "turn_boundary") == 0)
                is_boundary = 1;
            else
                is_typed_prompt = json_line_is_typed_prompt(object);
            json_decref(object);
        }

        if (is_typed_prompt) {
            if (turn_count == keep_turns && cut_offset < 0)
                cut_offset = previous_was_boundary ? previous_offset : line_offset;
            turn_count++;
        }
        previous_offset = line_offset;
        previous_was_boundary = is_boundary;
    }
    int read_failed = ferror(file);
    free(line);
    fclose(file);
    if (read_failed)
        return -1;

    return cut_offset < 0 ? current_offset : cut_offset;
}

int session_log_truncate(struct session_log *log, size_t keep_turns, size_t new_item_count)
{
    if (!log || !log->path)
        return 0;
    if (!log->file)
        return 0;
    if (fflush(log->file) != 0)
        return -1;
    long cut_offset = find_turn_cut_offset(log->path, keep_turns);
    if (cut_offset < 0)
        return -1;

    /* Never extend a file shortened by another process after the offset scan. */
    struct stat file_stat;
    if (fstat(fileno(log->file), &file_stat) != 0 || (off_t)cut_offset > file_stat.st_size)
        return -1;

    /* A plain "w" stream must be repositioned before truncation or the next write leaves a hole. */
    long original_offset = ftell(log->file);
    if (original_offset < 0)
        return -1;
    if (fseek(log->file, cut_offset, SEEK_SET) != 0) {
        fseek(log->file, original_offset, SEEK_SET);
        return -1;
    }
    if (ftruncate(fileno(log->file), cut_offset) != 0) {
        fseek(log->file, original_offset, SEEK_SET);
        return -1;
    }
    log->written_items = new_item_count;

    /* Restate a live selection whose record was removed by the cut. */
    struct session_meta metadata;
    if (session_read_meta(log->path, &metadata) == 0 && !selection_matches_log(&metadata, log))
        log->selection_pending = 1;
    session_meta_free(&metadata);
    return 0;
}

int session_log_materialized(const struct session_log *log)
{
    return log && log->header_written;
}

static char *fork_session_path(const char *source_path, const char *filename_time, const char *uuid)
{
    const char *separator = strrchr(source_path, '/');
    if (!separator)
        return xasprintf("%s_%s.jsonl", filename_time, uuid);

    size_t directory_length = (size_t)(separator - source_path);
    char *directory = (char *)xmalloc(directory_length + 1);
    memcpy(directory, source_path, directory_length);
    directory[directory_length] = '\0';
    char *path = xasprintf("%s/%s_%s.jsonl", directory, filename_time, uuid);
    free(directory);
    return path;
}

static int copy_bytes(FILE *source, FILE *destination, long byte_count)
{
    char buffer[65536];
    while (byte_count > 0) {
        size_t wanted = byte_count < (long)sizeof(buffer) ? (size_t)byte_count : sizeof(buffer);
        size_t bytes_read = fread(buffer, 1, wanted, source);
        if (bytes_read == 0 || fwrite(buffer, 1, bytes_read, destination) != bytes_read)
            return -1;
        byte_count -= (long)bytes_read;
    }
    return 0;
}

int session_fork_file(const char *source_path, size_t keep_turns, char **out_path)
{
    *out_path = NULL;
    int result = -1;
    int destination_created = 0;
    int destination_fd = -1;
    FILE *source = NULL;
    FILE *destination = NULL;
    json_t *header = NULL;
    char *header_line = NULL;
    char *destination_path = NULL;
    const char *source_id = NULL;
    size_t header_capacity = 0;
    ssize_t header_length = 0;
    char uuid[37];
    time_t now = 0;

    long cut_offset = find_turn_cut_offset(source_path, keep_turns);
    if (cut_offset < 0)
        goto out;

    source = fopen(source_path, "r");
    if (!source)
        goto out;
    header_length = getline(&header_line, &header_capacity, source);
    if (header_length < 0)
        goto out;
    header = json_loads(header_line, 0, NULL);
    if (!json_is_object(header))
        goto out;

    gen_uuid_v4(uuid);
    now = time(NULL);
    struct tm utc;
    gmtime_r(&now, &utc);
    char filename_time[32];
    char header_time[32];
    strftime(filename_time, sizeof(filename_time), "%Y-%m-%dT%H-%M-%SZ", &utc);
    strftime(header_time, sizeof(header_time), "%Y-%m-%dT%H:%M:%SZ", &utc);

    source_id = json_string_value(json_object_get(header, "id"));
    if (source_id)
        json_object_set_new(header, "forked_from", json_string(source_id));
    json_object_set_new(header, "id", json_string(uuid));
    json_object_set_new(header, "timestamp", json_string(header_time));

    destination_path = fork_session_path(source_path, filename_time, uuid);
    destination_fd = open(destination_path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    if (destination_fd < 0)
        goto out;
    destination_created = 1;
    destination = fdopen(destination_fd, "w");
    if (!destination)
        goto out;
    destination_fd = -1;

    if (write_json_line(destination, header) < 0)
        goto out;
    if (cut_offset > header_length &&
        (fseek(source, header_length, SEEK_SET) != 0 ||
         copy_bytes(source, destination, cut_offset - header_length) < 0))
        goto out;
    if (fclose(destination) != 0) {
        destination = NULL;
        goto out;
    }
    destination = NULL;

    *out_path = destination_path;
    destination_path = NULL;
    result = 0;

out:
    if (destination)
        fclose(destination);
    if (destination_fd >= 0)
        close(destination_fd);
    if (result < 0 && destination_created)
        unlink(destination_path);
    free(destination_path);
    free(header_line);
    if (header)
        json_decref(header);
    if (source)
        fclose(source);
    return result;
}

static void push_item(struct item **items, size_t *count, size_t *capacity, struct item item)
{
    if (*count == *capacity) {
        *capacity = *capacity ? *capacity * 2 : 16;
        *items = (struct item *)xrealloc(*items, *capacity * sizeof(**items));
    }
    (*items)[(*count)++] = item;
}

/* Keep earliest images when a file exceeds the current request limits. Only the model-visible
 * window is budgeted: images a compaction summarized away are never sent, so charging the limit
 * for them would degrade the live images that replaced them. */
static void degrade_excess_images(struct item *items, size_t item_count)
{
    size_t encoded_bytes = 0;
    size_t image_count = 0;
    for (size_t i = items_context_floor(items, item_count); i < item_count; i++) {
        struct item *item = &items[i];
        if (item->n_images == 0)
            continue;

        size_t item_bytes = 0;
        for (size_t image_index = 0; image_index < item->n_images; image_index++)
            if (item->images[image_index].data_b64)
                item_bytes += strlen(item->images[image_index].data_b64);
        if (encoded_bytes + item_bytes <= IMAGE_REQUEST_BASE64_BUDGET_BYTES &&
            image_count + item->n_images <= IMAGE_REQUEST_MAX_COUNT) {
            encoded_bytes += item_bytes;
            image_count += item->n_images;
            continue;
        }

        char *output = xstrdup(item->output ? item->output : "");
        for (size_t image_index = 0; image_index < item->n_images; image_index++) {
            char *placeholder = item_image_placeholder(&item->images[image_index]);
            char *extended_output = xasprintf("%s\n%s", output, placeholder);
            free(output);
            free(placeholder);
            output = extended_output;
            free(item->images[image_index].mime);
            free(item->images[image_index].data_b64);
        }
        free(item->images);
        item->images = NULL;
        item->n_images = 0;
        free(item->output);
        item->output = output;
    }
}

/* Selection records are complete snapshots, so absent fields clear previous values. */
static void apply_selection_record(struct session_meta *meta, const struct session_control *control)
{
    free(meta->provider);
    meta->provider = control->provider ? xstrdup(control->provider) : NULL;
    free(meta->model);
    meta->model = control->model ? xstrdup(control->model) : NULL;
    free(meta->effort);
    meta->effort = control->effort ? xstrdup(control->effort) : NULL;
    free(meta->preset);
    meta->preset = control->preset ? xstrdup(control->preset) : NULL;
}

static FILE *open_session_reader(const char *path)
{
    int fd = open_regular_file(path);
    if (fd < 0)
        return NULL;

    FILE *file = fdopen(fd, "r");
    if (!file) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
    }
    return file;
}

int session_read_meta(const char *path, struct session_meta *out)
{
    memset(out, 0, sizeof(*out));
    FILE *file = open_session_reader(path);
    if (!file)
        return -1;

    char *line = NULL;
    size_t capacity = 0;
    while (getline(&line, &capacity, file) >= 0) {
        struct session_control control;
        if (session_control_decode(line, &control) != SESSION_CONTROL_DECODED)
            continue;
        if (control.kind == SESSION_CONTROL_HEADER) {
            free(out->id);
            out->id = control.id ? xstrdup(control.id) : NULL;
            free(out->cwd);
            out->cwd = control.cwd ? xstrdup(control.cwd) : NULL;
        }
        apply_selection_record(out, &control);
        session_control_free(&control);
    }
    int result = ferror(file) ? -1 : 0;
    free(line);
    fclose(file);
    if (result < 0)
        session_meta_free(out);
    return result;
}

static int tool_call_has_result(const struct item *items, size_t count, const struct item *call)
{
    if (!call->call_id)
        return 0;
    for (size_t i = 0; i < count; i++)
        if (items[i].kind == ITEM_TOOL_RESULT && items[i].call_id &&
            strcmp(items[i].call_id, call->call_id) == 0)
            return 1;
    return 0;
}

static size_t remove_incomplete_tool_calls(struct item *items, size_t count)
{
    size_t kept = 0;
    for (size_t i = 0; i < count; i++) {
        if (items[i].kind == ITEM_TOOL_CALL && !tool_call_has_result(items, count, &items[i])) {
            item_free(&items[i]);
            continue;
        }
        items[kept++] = items[i];
    }
    return kept;
}

int session_load(const char *path, struct item **out_items, size_t *out_count,
                 struct session_meta *out_meta)
{
    if (out_meta)
        memset(out_meta, 0, sizeof(*out_meta));
    *out_items = NULL;
    *out_count = 0;

    FILE *file = open_session_reader(path);
    if (!file)
        return -1;

    struct item *items = NULL;
    size_t count = 0;
    size_t capacity = 0;
    char *header_provider = NULL;
    char *header_model = NULL;
    char *line = NULL;
    size_t line_capacity = 0;

    while (getline(&line, &line_capacity, file) >= 0) {
        struct session_control control;
        if (session_control_decode(line, &control) == SESSION_CONTROL_DECODED) {
            if (control.kind == SESSION_CONTROL_HEADER) {
                if (!header_provider && control.provider)
                    header_provider = xstrdup(control.provider);
                if (!header_model && control.model)
                    header_model = xstrdup(control.model);
                if (out_meta) {
                    free(out_meta->id);
                    out_meta->id = control.id ? xstrdup(control.id) : NULL;
                    free(out_meta->cwd);
                    out_meta->cwd = control.cwd ? xstrdup(control.cwd) : NULL;
                    apply_selection_record(out_meta, &control);
                }
            } else if (out_meta) {
                apply_selection_record(out_meta, &control);
            }
            session_control_free(&control);
            continue;
        }

        struct item item;
        if (session_item_decode(line, &item) == 0) {
            /* Old reasoning records inherit the header provenance needed for safe replay. */
            if (item.kind == ITEM_REASONING) {
                if (!item.provider && header_provider)
                    item.provider = xstrdup(header_provider);
                if (!item.model && header_model)
                    item.model = xstrdup(header_model);
            }
            push_item(&items, &count, &capacity, item);
        }
    }

    int read_failed = ferror(file);
    free(line);
    fclose(file);
    free(header_provider);
    free(header_model);
    if (read_failed) {
        for (size_t i = 0; i < count; i++)
            item_free(&items[i]);
        free(items);
        if (out_meta)
            session_meta_free(out_meta);
        return -1;
    }

    /* Providers reject tool calls that lack a corresponding result after a crash. */
    count = remove_incomplete_tool_calls(items, count);
    degrade_excess_images(items, count);
    *out_items = items;
    *out_count = count;
    return 0;
}

/* A prompt should occur before this bound; avoid reading an early multi-megabyte result. */
#define LABEL_SCAN_CAP (64 * 1024)

void session_label_read(const char *path, int max_cells, struct session_label *out)
{
    *out = (struct session_label){0};
    char *data = slurp_file_capped(path, LABEL_SCAN_CAP, NULL, NULL);
    if (!data)
        return;

    char *save = NULL;
    int saw_compaction_seed = 0;
    for (char *line = strtok_r(data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (!*line)
            continue;
        struct session_control control;
        const enum session_control_decode_result control_result =
            session_control_decode(line, &control);
        if (control_result != SESSION_CONTROL_NOT_FOUND) {
            /* Only records ahead of the opening prompt are seen, so a later /model switch does not
             * show up — the label describes what the conversation started as. */
            if (control_result == SESSION_CONTROL_DECODED) {
                free(out->provider);
                out->provider = control.provider ? xstrdup(control.provider) : NULL;
                free(out->model);
                out->model = control.model_label ? xstrdup(control.model_label)
                                                 : (control.model ? xstrdup(control.model) : NULL);
                free(out->effort);
                out->effort = control.effort ? xstrdup(control.effort) : NULL;
                free(out->preset);
                out->preset = control.preset ? xstrdup(control.preset) : NULL;
                if (control.kind == SESSION_CONTROL_HEADER) {
                    free(out->git_branch);
                    out->git_branch = control.git_branch ? xstrdup(control.git_branch) : NULL;
                    free(out->git_subject);
                    out->git_subject = control.git_subject ? xstrdup(control.git_subject) : NULL;
                }
            }
            session_control_free(&control);
            continue;
        }

        json_t *object = json_loads(line, 0, NULL);
        if (!object)
            continue;

        const char *kind = json_string_value(json_object_get(object, "kind"));
        if (kind && strcmp(kind, "user") == 0) {
            enum item_origin origin = json_get_item_origin(object);
            if (origin != ITEM_ORIGIN_NONE) {
                if (origin == ITEM_ORIGIN_COMPACT_SEED)
                    saw_compaction_seed = 1;
                json_decref(object);
                continue;
            }
            const char *text = json_string_value(json_object_get(object, "text"));
            if (text) {
                char *flattened = flatten_for_display(text);
                out->prompt = truncate_for_display(flattened, (size_t)max_cells);
                free(flattened);
            }
            json_decref(object);
            break;
        }
        json_decref(object);
    }
    free(data);
    if (!out->prompt && saw_compaction_seed)
        out->prompt = xstrdup("(compacted)");
}

void session_label_free(struct session_label *label)
{
    free(label->prompt);
    free(label->provider);
    free(label->model);
    free(label->effort);
    free(label->preset);
    free(label->git_branch);
    free(label->git_subject);
    *label = (struct session_label){0};
}

static int compare_session_mtime_desc(const void *left_pointer, const void *right_pointer)
{
    const struct session_entry *left = (const struct session_entry *)left_pointer;
    const struct session_entry *right = (const struct session_entry *)right_pointer;
    if (left->mtime != right->mtime)
        return left->mtime < right->mtime ? 1 : -1;
    if (left->mtime_nsec != right->mtime_nsec)
        return left->mtime_nsec < right->mtime_nsec ? 1 : -1;
    /* Sessions written within one timestamp tick carry no recorded order, and not every filesystem
     * records a sub-second one: OpenBSD stamps rapid writes with an identical mtime. Order them by
     * path so listings and --continue stay reproducible rather than left to an unstable sort. */
    return strcmp(right->path, left->path);
}

static int has_jsonl_extension(const char *name)
{
    size_t length = strlen(name);
    return length >= 6 && strcmp(name + length - 6, ".jsonl") == 0;
}

int session_list(const char *cwd, struct session_entry **out_entries, size_t *out_count)
{
    *out_entries = NULL;
    *out_count = 0;
    char *directory = session_directory(cwd);
    if (!directory)
        return 0;
    DIR *directory_stream = opendir(directory);
    if (!directory_stream) {
        free(directory);
        return 0;
    }

    struct session_entry *entries = NULL;
    size_t count = 0;
    size_t capacity = 0;
    time_t cutoff = session_retention_cutoff();
    struct dirent *directory_entry;
    while ((directory_entry = readdir(directory_stream))) {
        if (!has_jsonl_extension(directory_entry->d_name))
            continue;
        char *path = xasprintf("%s/%s", directory, directory_entry->d_name);
        struct stat file_stat;
        if (stat(path, &file_stat) != 0 || !S_ISREG(file_stat.st_mode)) {
            free(path);
            continue;
        }
        char *id = session_id_from_path(path);
        if (id && cutoff && file_stat.st_mtime < cutoff) {
            free(id);
            free(path);
            continue;
        }
        struct session_entry entry = {
            .path = path,
            .id = id,
            .mtime = (long)file_stat.st_mtime,
            .mtime_nsec = ST_MTIME_NSEC(file_stat),
        };
        if (count == capacity) {
            capacity = capacity ? capacity * 2 : 8;
            entries = (session_entry *)xrealloc(entries, capacity * sizeof(*entries));
        }
        entries[count++] = entry;
    }
    closedir(directory_stream);
    free(directory);

    qsort(entries, count, sizeof(*entries), compare_session_mtime_desc);
    *out_entries = entries;
    *out_count = count;
    return 0;
}

void session_list_free(struct session_entry *entries, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        free(entries[i].path);
        free(entries[i].id);
        session_label_free(&entries[i].label);
    }
    free(entries);
}
