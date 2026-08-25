/* SPDX-License-Identifier: MIT */
#ifndef HAX_CRED_STORE_H
#define HAX_CRED_STORE_H

#include <optional>
#include <string>
#include <string_view>

/* hax-owned credentials: one opaque JSON value per provider id, stored with mode 0600 in the XDG
 * state tree. The store validates only the envelope and each value as JSON; provider code owns the
 * value's schema. Every call reads the file afresh so concurrent hax processes observe each other's
 * updates, and every write serializes against other hax processes through an advisory lock.
 * Foreground-thread state: resolve entries before spawning background work. */

/* Resolved store path for diagnostics, or NULL when no home is available. The caller frees. */
char *cred_store_file_path(void);

enum cred_store_entry_status {
    CRED_STORE_ENTRY_PRESENT,
    CRED_STORE_ENTRY_MISSING,
    CRED_STORE_ENTRY_MALFORMED,
    CRED_STORE_ENTRY_ERROR,
    CRED_STORE_ENTRY_INVALID_ARGUMENT,
};

/* Result of a read or take. `json` is owned by the result and populated only when `status` is
 * CRED_STORE_ENTRY_PRESENT. Missing means either that the file is absent or that the provider id
 * is not present in a valid file. Malformed and error are distinct so callers do not mistake an
 * unreadable store for an absent login; invalid arguments are reported separately. */
struct cred_store_read {
    enum cred_store_entry_status status;
    std::string json;
};

/* Return an owned copy of the opaque entry, or an explicit missing, malformed, error, or
 * invalid-argument status. */
struct cred_store_read cred_store_get(const char *provider_id);

enum cred_store_result {
    CRED_STORE_RESULT_ERROR = -1,
    CRED_STORE_RESULT_MALFORMED = -2,
    CRED_STORE_RESULT_INVALID = -3,
    CRED_STORE_RESULT_UNCHANGED = 0,
    CRED_STORE_RESULT_CHANGED = 1,
};

/* Insert or replace an entry borrowed as one complete JSON value. A malformed existing file is
 * left untouched so replacing one provider cannot discard opaque entries for others; an I/O error
 * is never treated as a missing file. Returns CHANGED, MALFORMED, INVALID for malformed input or
 * arguments, or ERROR. */
enum cred_store_result cred_store_set(const char *provider_id, std::string_view entry_json);

/* Remove the entry. Returns CHANGED when removed, UNCHANGED when absent, MALFORMED when the store
 * cannot be parsed, or ERROR/INVALID for the corresponding failure. */
enum cred_store_result cred_store_delete(const char *provider_id);

/* Remove the entry and return its opaque JSON in one transaction, so the caller acts on exactly
 * what was removed (e.g. revoking its token). The returned string is owned by the caller only for
 * PRESENT; malformed, error, and invalid-argument outcomes return an empty string. */
struct cred_store_read cred_store_take(const char *provider_id);

enum cred_store_verdict {
    CRED_STORE_KEEP,   /* leave the store untouched */
    CRED_STORE_WRITE,  /* store the replacement supplied by the callback */
    CRED_STORE_REMOVE, /* delete the entry */
};

/* Receive the current entry as a borrowed view (an empty optional means absent) and decide the
 * store's new state. For CRED_STORE_WRITE assign one complete JSON value to `*replacement`; the
 * store copies it after the callback returns. The view is valid only during the callback. */
typedef enum cred_store_verdict (*cred_store_update_fn)(std::optional<std::string_view> entry,
                                                        std::string *replacement, void *ctx);

/* Read-modify-write one entry as a single transaction: the cross-process lock is held from read
 * to write, so the entry `update` sees cannot change underneath its decision. Malformed and error
 * stores do not invoke the callback. Returns CHANGED, UNCHANGED, MALFORMED, ERROR, or INVALID. */
enum cred_store_result cred_store_update(const char *provider_id, cred_store_update_fn update,
                                         void *ctx);

#endif /* HAX_CRED_STORE_H */
