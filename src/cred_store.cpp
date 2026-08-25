/* SPDX-License-Identifier: MIT */
#include "cred_store.h"

#include <cstdint>
#include <errno.h>
#include <fcntl.h>
#include <map>
#include <optional>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <sys/file.h>

#include "json.h"
#include "util.h"
#include "system/fs.h"

namespace
{

using store_document = std::map<std::string, glz::raw_json>;

enum class document_status {
    missing,
    valid,
    malformed,
    error,
};

struct loaded_document {
    document_status status;
    store_document document;
};

struct pretty_options : glz::opts {
    uint8_t indentation_width = 2;
};

static int valid_provider_id(const char *provider_id)
{
    return provider_id && *provider_id;
}

static int valid_entry_json(std::string_view entry_json)
{
    return hax::json::validate(entry_json,
                               {.source = "credential store entry", .max_input_bytes = 0})
        .has_value();
}

static loaded_document load_root(const char *path)
{
    errno = 0;
    size_t contents_length = 0;
    char *contents = slurp_file(path, &contents_length);
    if (!contents) {
        return {errno == ENOENT ? document_status::missing : document_status::error, {}};
    }

    auto parsed = hax::json::parse<store_document>(
        std::string_view(contents, contents_length),
        {.source = "credential store", .max_input_bytes = 0, .allow_unknown_keys = true});
    free(contents);
    if (!parsed)
        return {document_status::malformed, {}};
    return {document_status::valid, std::move(*parsed)};
}

static enum cred_store_entry_status read_status(document_status status)
{
    switch (status) {
    case document_status::missing:
        return CRED_STORE_ENTRY_MISSING;
    case document_status::malformed:
        return CRED_STORE_ENTRY_MALFORMED;
    case document_status::error:
        return CRED_STORE_ENTRY_ERROR;
    case document_status::valid:
        break;
    }
    return CRED_STORE_ENTRY_ERROR;
}

} // namespace

char *cred_store_file_path(void)
{
    return xdg_hax_state_path("auth.json");
}

static int ensure_parent_dir(const char *path)
{
    const char *slash = strrchr(path, '/');
    if (!slash)
        return 0;
    char *parent = xstrdup(path);
    parent[slash - path] = '\0';
    int result = fs_mkdir_p(parent);
    free(parent);
    return result;
}

/* Serialize store writes across hax processes. The lock lives in a sidecar file because the store
 * itself is replaced by rename, which would silently split lockers between inodes. Returns the
 * lock's fd, or -1 when the lock cannot be taken — writers must fail then, or token-rotation
 * coordination silently degrades to lost updates. */
static int store_lock(const char *path)
{
    if (ensure_parent_dir(path) != 0)
        return -1;
    char *lock_path = xasprintf("%s.lock", path);
    int fd = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    free(lock_path);
    if (fd < 0)
        return -1;
    while (flock(fd, LOCK_EX) != 0) {
        if (errno != EINTR) {
            close(fd);
            return -1;
        }
    }
    return fd;
}

static void store_unlock(int lock_fd)
{
    if (lock_fd >= 0)
        close(lock_fd);
}

/* Flush the directory entry after a rename so a crash cannot resurrect the replaced file. */
static void sync_parent_dir(const char *path)
{
    const char *slash = strrchr(path, '/');
    if (!slash)
        return;
    char *parent = xstrdup(path);
    parent[slash - path] = '\0';
    int fd = open(parent, O_RDONLY | O_CLOEXEC);
    free(parent);
    if (fd < 0)
        return;
    /* Best-effort: not every filesystem supports directory fsync. */
    fsync(fd);
    close(fd);
}

static std::optional<std::string> encode_root(const store_document &root)
{
    auto compact = hax::json::serialize(root, {.source = "credential store"});
    if (!compact)
        return std::nullopt;

    std::string pretty;
    glz::prettify_json<pretty_options{}>(*compact, pretty);
    return pretty;
}

/* mkstemp creates the temporary file with mode 0600, which is also the mode credentials need, so
 * the rename never exposes a readable window. The fsync before rename is load-bearing: a rotated
 * refresh token restored from a stale page after a crash is spent and unrecoverable. */
static int save_root(const char *path, const store_document &root)
{
    if (ensure_parent_dir(path) != 0)
        return -1;

    auto encoded = encode_root(root);
    if (!encoded)
        return -1;

    char *temp_path = xasprintf("%s.XXXXXX", path);
    int fd = mkstemp(temp_path);
    if (fd < 0)
        goto err_temp;

    if (write_all(fd, encoded->data(), encoded->size()) != 0 || write_all(fd, "\n", 1) != 0)
        goto err_fd;
    if (fsync(fd) != 0)
        goto err_fd;
    if (close(fd) != 0) {
        fd = -1;
        goto err_fd;
    }
    fd = -1;
    if (rename(temp_path, path) != 0)
        goto err_fd;
    sync_parent_dir(path);

    free(temp_path);
    return 0;

err_fd:
    if (fd >= 0)
        close(fd);
    unlink(temp_path);
err_temp:
    free(temp_path);
    return -1;
}

struct cred_store_read cred_store_get(const char *provider_id)
{
    if (!valid_provider_id(provider_id))
        return {CRED_STORE_ENTRY_INVALID_ARGUMENT, {}};

    char *path = cred_store_file_path();
    if (!path)
        return {CRED_STORE_ENTRY_ERROR, {}};
    loaded_document loaded = load_root(path);
    free(path);
    if (loaded.status != document_status::valid)
        return {read_status(loaded.status), {}};

    auto entry = loaded.document.find(provider_id);
    if (entry == loaded.document.end())
        return {CRED_STORE_ENTRY_MISSING, {}};
    return {CRED_STORE_ENTRY_PRESENT, entry->second.str};
}

enum cred_store_result cred_store_set(const char *provider_id, std::string_view entry_json)
{
    if (!valid_provider_id(provider_id) || !valid_entry_json(entry_json))
        return CRED_STORE_RESULT_INVALID;

    char *path = NULL;
    int lock_fd = -1;
    enum cred_store_result result = CRED_STORE_RESULT_ERROR;
    loaded_document loaded = {document_status::error, {}};
    store_document root;

    path = cred_store_file_path();
    if (!path)
        goto out;
    lock_fd = store_lock(path);
    if (lock_fd < 0)
        goto out;

    loaded = load_root(path);
    if (loaded.status == document_status::malformed) {
        result = CRED_STORE_RESULT_MALFORMED;
        goto out;
    }
    if (loaded.status == document_status::error)
        goto out;

    root = std::move(loaded.document);
    root[provider_id] = glz::raw_json{std::string(entry_json)};
    result = save_root(path, root) == 0 ? CRED_STORE_RESULT_CHANGED : CRED_STORE_RESULT_ERROR;

out:
    store_unlock(lock_fd);
    free(path);
    return result;
}

struct cred_store_read cred_store_take(const char *provider_id)
{
    if (!valid_provider_id(provider_id))
        return {CRED_STORE_ENTRY_INVALID_ARGUMENT, {}};

    char *path = NULL;
    int lock_fd = -1;
    enum cred_store_entry_status status = CRED_STORE_ENTRY_ERROR;
    loaded_document loaded = {document_status::error, {}};
    store_document::iterator entry;
    std::string removed;

    path = cred_store_file_path();
    if (!path)
        goto out;
    lock_fd = store_lock(path);
    if (lock_fd < 0)
        goto out;

    loaded = load_root(path);
    if (loaded.status != document_status::valid) {
        status = read_status(loaded.status);
        goto out;
    }

    entry = loaded.document.find(provider_id);
    if (entry == loaded.document.end()) {
        status = CRED_STORE_ENTRY_MISSING;
        goto out;
    }

    removed = entry->second.str;
    loaded.document.erase(entry);
    if (save_root(path, loaded.document) != 0) {
        removed.clear();
        goto out;
    }
    status = CRED_STORE_ENTRY_PRESENT;

out:
    store_unlock(lock_fd);
    free(path);
    return {status, std::move(removed)};
}

enum cred_store_result cred_store_delete(const char *provider_id)
{
    struct cred_store_read removed = cred_store_take(provider_id);
    switch (removed.status) {
    case CRED_STORE_ENTRY_PRESENT:
        return CRED_STORE_RESULT_CHANGED;
    case CRED_STORE_ENTRY_MISSING:
        return CRED_STORE_RESULT_UNCHANGED;
    case CRED_STORE_ENTRY_MALFORMED:
        return CRED_STORE_RESULT_MALFORMED;
    case CRED_STORE_ENTRY_INVALID_ARGUMENT:
        return CRED_STORE_RESULT_INVALID;
    case CRED_STORE_ENTRY_ERROR:
        return CRED_STORE_RESULT_ERROR;
    }
    return CRED_STORE_RESULT_ERROR;
}

enum cred_store_result cred_store_update(const char *provider_id, cred_store_update_fn update,
                                         void *ctx)
{
    if (!valid_provider_id(provider_id) || !update)
        return CRED_STORE_RESULT_INVALID;

    char *path = NULL;
    int lock_fd = -1;
    loaded_document loaded = {document_status::error, {}};
    std::optional<std::string_view> entry;
    std::string replacement_json;
    enum cred_store_verdict verdict = CRED_STORE_KEEP;
    enum cred_store_result result = CRED_STORE_RESULT_ERROR;

    path = cred_store_file_path();
    if (!path)
        goto out;
    lock_fd = store_lock(path);
    if (lock_fd < 0)
        goto out;

    loaded = load_root(path);
    if (loaded.status == document_status::malformed) {
        result = CRED_STORE_RESULT_MALFORMED;
        goto out;
    }
    if (loaded.status == document_status::error)
        goto out;

    if (loaded.status == document_status::valid) {
        auto found = loaded.document.find(provider_id);
        if (found != loaded.document.end())
            entry = found->second.str;
    }

    verdict = update(entry, &replacement_json, ctx);
    result = CRED_STORE_RESULT_UNCHANGED;
    if (verdict == CRED_STORE_WRITE) {
        if (!valid_entry_json(replacement_json)) {
            result = CRED_STORE_RESULT_INVALID;
        } else {
            loaded.document[provider_id] = glz::raw_json{replacement_json};
            result = save_root(path, loaded.document) == 0 ? CRED_STORE_RESULT_CHANGED
                                                           : CRED_STORE_RESULT_ERROR;
        }
    } else if (verdict == CRED_STORE_REMOVE && entry) {
        loaded.document.erase(provider_id);
        result = save_root(path, loaded.document) == 0 ? CRED_STORE_RESULT_CHANGED
                                                       : CRED_STORE_RESULT_ERROR;
    } else if (verdict != CRED_STORE_KEEP && verdict != CRED_STORE_REMOVE) {
        result = CRED_STORE_RESULT_INVALID;
    }

out:
    store_unlock(lock_fd);
    free(path);
    return result;
}
