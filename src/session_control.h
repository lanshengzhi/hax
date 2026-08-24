/* SPDX-License-Identifier: MIT */
#ifndef HAX_SESSION_CONTROL_H
#define HAX_SESSION_CONTROL_H

#include <string>
#include <string_view>

/* Session control records are append-only snapshots. The adapter accepts only the two control
 * kinds hax currently understands; callers can ignore all other JSONL records. */
enum session_control_kind {
    SESSION_CONTROL_NONE,
    SESSION_CONTROL_HEADER,
    SESSION_CONTROL_SELECTION,
};

enum session_control_decode_result {
    SESSION_CONTROL_INVALID = -1,
    SESSION_CONTROL_NOT_FOUND = 0,
    SESSION_CONTROL_DECODED = 1,
};

/* Project-owned view of a session header or selection snapshot. All strings are owned after a
 * successful decode, and optional fields remain NULL when absent from the wire record. */
struct session_control {
    enum session_control_kind kind;
    int has_version;
    long version;
    char *hax_version;
    char *id;
    char *timestamp;
    char *cwd;
    char *provider;
    char *model;
    char *model_label;
    char *effort;
    char *preset;
    char *git_branch;
    char *git_commit;
    char *git_subject;
    char *forked_from;
};

/* Borrowed control fields for encoding one append-only record. The encoder never retains or frees
 * these values. Header-only fields are ignored for selections. */
struct session_control_input {
    enum session_control_kind kind;
    int has_version;
    long version;
    const char *hax_version;
    const char *id;
    const char *timestamp;
    const char *cwd;
    const char *provider;
    const char *model;
    const char *model_label;
    const char *effort;
    const char *preset;
    const char *git_branch;
    const char *git_commit;
    const char *git_subject;
    const char *forked_from;
};

/* Encode borrowed project-owned fields as one compact JSON document. Returns zero on success or -1
 * when the record kind or a wire value cannot be encoded. */
int session_control_encode(const struct session_control_input *input, std::string *out);
/* Rewrite a forked header with a new identity and timestamp. Unknown fields and values are
 * retained. Returns zero on success or -1 for an invalid header or argument. */
int session_control_rewrite_header(std::string_view input, const char *id, const char *timestamp,
                                   std::string *out);

/* Zeroes the output and decodes one complete JSONL record. Unknown fields are accepted for
 * forward-compatible control records; malformed, unknown, or non-control records are not decoded.
 * Returns SESSION_CONTROL_DECODED, SESSION_CONTROL_NOT_FOUND, or SESSION_CONTROL_INVALID. */
enum session_control_decode_result session_control_decode(std::string_view input,
                                                          struct session_control *out);

void session_control_free(struct session_control *control);

#endif /* HAX_SESSION_CONTROL_H */
