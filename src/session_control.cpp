/* SPDX-License-Identifier: MIT */
#include "session_control.h"

#include <cctype>
#include <cstdlib>
#include <optional>
#include <string>

#include "json.h"
#include "util.h"

namespace hax::session_control_detail
{

enum class wire_kind {
    session,
    selection,
};

struct wire_control {
    std::optional<wire_kind> type;
    std::optional<long> version;
    std::optional<std::string> hax_version;
    std::optional<std::string> id;
    std::optional<std::string> timestamp;
    std::optional<std::string> cwd;
    std::optional<std::string> provider;
    std::optional<std::string> model;
    std::optional<std::string> model_label;
    std::optional<std::string> effort;
    std::optional<std::string> preset;
    std::optional<std::string> git_branch;
    std::optional<std::string> git_commit;
    std::optional<std::string> git_subject;
    std::optional<std::string> forked_from;
};

} // namespace hax::session_control_detail

template <> struct glz::meta<hax::session_control_detail::wire_kind> {
    using T = hax::session_control_detail::wire_kind;
    static constexpr auto value = glz::enumerate(T::session, T::selection);
};

template <> struct glz::meta<hax::session_control_detail::wire_control> {
    using T = hax::session_control_detail::wire_control;
    static constexpr auto value =
        glz::object("type", &T::type, "version", &T::version, "hax_version", &T::hax_version, "id",
                    &T::id, "timestamp", &T::timestamp, "cwd", &T::cwd, "provider", &T::provider,
                    "model", &T::model, "model_label", &T::model_label, "effort", &T::effort,
                    "preset", &T::preset, "git_branch", &T::git_branch, "git_commit",
                    &T::git_commit, "git_subject", &T::git_subject, "forked_from", &T::forked_from);
};

namespace
{

using hax::session_control_detail::wire_control;
using hax::session_control_detail::wire_kind;

static int hex_digit(char value)
{
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

static bool decode_key(std::string_view encoded, std::string *decoded)
{
    decoded->clear();
    for (size_t i = 0; i < encoded.size(); i++) {
        unsigned codepoint;
        if (encoded[i] != '\\') {
            codepoint = static_cast<unsigned char>(encoded[i]);
        } else {
            if (++i >= encoded.size())
                return false;
            const char escaped = encoded[i];
            if (escaped == 'u') {
                if (i + 4 >= encoded.size())
                    return false;
                codepoint = 0;
                for (size_t digit = 1; digit <= 4; digit++) {
                    const int value = hex_digit(encoded[i + digit]);
                    if (value < 0)
                        return false;
                    codepoint = codepoint * 16 + static_cast<unsigned>(value);
                }
                i += 4;
            } else {
                switch (escaped) {
                case '"':
                case '\\':
                case '/':
                    codepoint = static_cast<unsigned>(static_cast<unsigned char>(escaped));
                    break;
                case 'b':
                    codepoint = '\b';
                    break;
                case 'f':
                    codepoint = '\f';
                    break;
                case 'n':
                    codepoint = '\n';
                    break;
                case 'r':
                    codepoint = '\r';
                    break;
                case 't':
                    codepoint = '\t';
                    break;
                default:
                    return false;
                }
            }
        }
        if (codepoint > 0x7f)
            return false;
        decoded->push_back(static_cast<char>(codepoint));
    }
    return true;
}

static const char *canonical_control_key(std::string_view key)
{
    static const char *const keys[] = {
        "type",   "version",    "hax_version", "id",          "timestamp",
        "cwd",    "provider",   "model",       "model_label", "effort",
        "preset", "git_branch", "git_commit",  "git_subject", "forked_from",
    };
    for (const char *candidate : keys)
        if (key == candidate)
            return candidate;
    return NULL;
}

/* Item records may contain arbitrary JSON, so only a top-level `type` key enters the control
 * adapter. Normalize escaped spellings of known keys before Glaze reads the typed DTO, preserving
 * the old reader's handling of valid JSON headers without parsing every item through the DTO. */
static bool normalize_control_keys(std::string_view input, std::string *normalized)
{
    size_t object_depth = 0;
    size_t string_start = 0;
    size_t copied = 0;
    bool in_string = false;
    bool escaped = false;
    bool found_type = false;
    bool changed = false;
    std::string decoded;

    for (size_t i = 0; i < input.size(); i++) {
        const char current = input[i];
        if (in_string) {
            if (escaped) {
                escaped = false;
                continue;
            }
            if (current == '\\') {
                escaped = true;
                continue;
            }
            if (current != '"')
                continue;

            size_t next = i + 1;
            while (next < input.size() && std::isspace(static_cast<unsigned char>(input[next])))
                next++;
            if (object_depth == 1 && next < input.size() && input[next] == ':' &&
                decode_key(input.substr(string_start, i - string_start), &decoded)) {
                if (decoded == "type")
                    found_type = true;
                const char *canonical = canonical_control_key(decoded);
                if (canonical && input.substr(string_start, i - string_start) != canonical) {
                    if (!changed)
                        normalized->reserve(input.size());
                    normalized->append(input.substr(copied, string_start - copied));
                    normalized->append(canonical);
                    copied = i;
                    changed = true;
                }
            }
            in_string = false;
            continue;
        }

        if (current == '"') {
            in_string = true;
            string_start = i + 1;
        } else if (current == '{') {
            object_depth++;
        } else if (current == '}' && object_depth > 0) {
            object_depth--;
        }
    }
    if (!found_type)
        return false;
    if (changed) {
        normalized->append(input.substr(copied));
    } else {
        normalized->assign(input);
    }
    return true;
}

static char *duplicate_optional_string(const std::optional<std::string> &value)
{
    return value ? xstrdup(value->c_str()) : NULL;
}

static void copy_wire_control(const wire_control &source, struct session_control *destination)
{
    destination->kind = source.type.value_or(wire_kind::selection) == wire_kind::session
                            ? SESSION_CONTROL_HEADER
                            : SESSION_CONTROL_SELECTION;
    destination->has_version = source.version.has_value();
    destination->version = source.version.value_or(0);
    destination->hax_version = duplicate_optional_string(source.hax_version);
    destination->id = duplicate_optional_string(source.id);
    destination->timestamp = duplicate_optional_string(source.timestamp);
    destination->cwd = duplicate_optional_string(source.cwd);
    destination->provider = duplicate_optional_string(source.provider);
    destination->model = duplicate_optional_string(source.model);
    destination->model_label = duplicate_optional_string(source.model_label);
    destination->effort = duplicate_optional_string(source.effort);
    destination->preset = duplicate_optional_string(source.preset);
    destination->git_branch = duplicate_optional_string(source.git_branch);
    destination->git_commit = duplicate_optional_string(source.git_commit);
    destination->git_subject = duplicate_optional_string(source.git_subject);
    destination->forked_from = duplicate_optional_string(source.forked_from);
}

} // namespace

enum session_control_decode_result session_control_decode(std::string_view input,
                                                          struct session_control *out)
{
    if (!out)
        return SESSION_CONTROL_INVALID;
    *out = {};
    std::string normalized;
    if (!normalize_control_keys(input, &normalized))
        return SESSION_CONTROL_NOT_FOUND;

    /* Control records are internal session files. Their bounded file readers already cap work, and
     * accepting unknown fields is part of the on-disk forward-compatibility contract. */
    auto decoded = hax::json::parse<wire_control>(
        normalized,
        {.source = "session control", .max_input_bytes = 0, .allow_unknown_keys = true});
    if (!decoded || !decoded->type)
        return SESSION_CONTROL_INVALID;

    copy_wire_control(*decoded, out);
    return SESSION_CONTROL_DECODED;
}

void session_control_free(struct session_control *control)
{
    if (!control)
        return;
    free(control->hax_version);
    free(control->id);
    free(control->timestamp);
    free(control->cwd);
    free(control->provider);
    free(control->model);
    free(control->model_label);
    free(control->effort);
    free(control->preset);
    free(control->git_branch);
    free(control->git_commit);
    free(control->git_subject);
    free(control->forked_from);
    *control = {};
}
