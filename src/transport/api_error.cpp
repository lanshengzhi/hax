/* SPDX-License-Identifier: MIT */
#include "transport/api_error.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#include "json.h"
#include "util.h"
#include "transport/sse.h"

#define MAX_MESSAGE_BYTES 200

static int is_tag_start(const char *text, const char *tag)
{
    size_t tag_len = strlen(tag);

    for (size_t i = 0; i < tag_len; i++) {
        if (text[i] == '\0' || tolower((unsigned char)text[i]) != tag[i])
            return 0;
    }

    char delimiter = text[tag_len];
    return delimiter == ' ' || delimiter == '\t' || delimiter == '\n' || delimiter == '\r' ||
           delimiter == '>' || delimiter == '/';
}

/* This intentionally recognizes only enough HTML to keep gateway error pages readable. */
static char *strip_html_and_flatten(const char *body)
{
    struct buf output;
    buf_init(&output);
    int in_tag = 0;
    int previous_was_space = 1;
    const char *hidden_element = NULL;

    for (const char *cursor = body; *cursor; cursor++) {
        unsigned char byte = (unsigned char)*cursor;

        if (hidden_element) {
            if (byte == '<' && cursor[1] == '/' && is_tag_start(cursor + 2, hidden_element))
                hidden_element = NULL;
            else
                continue;
        }
        if (in_tag) {
            if (byte == '>')
                in_tag = 0;
            continue;
        }
        if (byte == '<') {
            char next = cursor[1];
            int starts_tag = (next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z') ||
                             next == '/' || next == '!' || next == '?';
            if (starts_tag) {
                in_tag = 1;
                if (is_tag_start(cursor + 1, "style"))
                    hidden_element = "style";
                else if (is_tag_start(cursor + 1, "script"))
                    hidden_element = "script";
                continue;
            }
        }
        if (isspace(byte) || byte < 0x20) {
            if (!previous_was_space) {
                buf_append(&output, " ", 1);
                previous_was_space = 1;
            }
            continue;
        }
        buf_append(&output, cursor, 1);
        previous_was_space = 0;
    }

    if (output.len > 0 && output.data[output.len - 1] == ' ') {
        output.data[output.len - 1] = '\0';
        output.len--;
    }
    return buf_steal(&output);
}

static char *truncate_utf8(const char *message, size_t max_bytes)
{
    size_t len = strlen(message);
    if (len <= max_bytes)
        return xstrdup(message);

    size_t end = max_bytes;
    while (end > 0 && ((unsigned char)message[end] & 0xC0) == 0x80)
        end--;

    char *truncated = (char *)xmalloc(end + 4);
    memcpy(truncated, message, end);
    memcpy(truncated + end, "...", 4);
    return truncated;
}

/* The returned pointer is borrowed from `root`. */
static const std::string *extract_json_message(const hax::json::value &root)
{
    const hax::json::value *error = root.find("error");
    if (error && error->is_object()) {
        const hax::json::value *message = error->find("message");
        if (message && message->is_string())
            return &message->string_value();
    } else if (error && error->is_string()) {
        return &error->string_value();
    }

    const hax::json::value *message = root.find("message");
    return message && message->is_string() ? &message->string_value() : nullptr;
}

struct sse_error_capture {
    char *fallback;
    char *error;
};

static int capture_sse_error(const char *event_name, const char *data, void *user)
{
    struct sse_error_capture *capture = (struct sse_error_capture *)user;
    if (!data || !*data)
        return 0;

    int is_error = event_name && strcmp(event_name, "error") == 0;
    if (!is_error) {
        auto root = hax::json::parse_value(data, {.source = "SSE error response"});
        if (root)
            is_error = extract_json_message(*root) != nullptr;
    }
    if (is_error) {
        capture->error = xstrdup(data);
        return 1;
    }
    if (!capture->fallback)
        capture->fallback = xstrdup(data);
    return 0;
}

static char *unwrap_sse_data(const char *body)
{
    struct sse_error_capture capture = {0};
    struct sse_parser parser;

    sse_parser_init(&parser, capture_sse_error, &capture);
    sse_parser_feed(&parser, body, strlen(body));
    sse_parser_finalize(&parser);
    sse_parser_free(&parser);

    if (capture.error) {
        free(capture.fallback);
        return capture.error;
    }
    return capture.fallback;
}

static char *format_status_message(long status, const char *message)
{
    if (status > 0) {
        if (message && *message)
            return xasprintf("HTTP %ld: %s", status, message);
        return xasprintf("HTTP %ld", status);
    }
    return xstrdup(message && *message ? message : "request failed");
}

char *format_api_error(long status, const char *body)
{
    if (!body || !*body)
        return format_status_message(status, NULL);

    char *sse_data = unwrap_sse_data(body);
    const char *content = sse_data ? sse_data : body;

    auto root = hax::json::parse_value(content, {.source = "API error response"});
    if (root) {
        const std::string *message = extract_json_message(*root);
        if (message && !message->empty()) {
            char *truncated = truncate_utf8(message->c_str(), MAX_MESSAGE_BYTES);
            char *formatted = format_status_message(status, truncated);
            free(truncated);
            free(sse_data);
            return formatted;
        }
    }

    char *flattened = strip_html_and_flatten(content);
    char *truncated = truncate_utf8(flattened, MAX_MESSAGE_BYTES);
    char *formatted = format_status_message(status, truncated);
    free(flattened);
    free(truncated);
    free(sse_data);
    return formatted;
}

char *format_model_list_error(const char *name, const char *base_url, int has_key, long status)
{
    if (!name)
        name = "provider";
    if (status == 401 || status == 403) {
        if (has_key)
            return xasprintf("%s rejected the API key (HTTP %ld) — check it and retry", name,
                             status);
        return xasprintf("%s requires an API key (HTTP %ld) — none is configured", name, status);
    }
    if (status >= 200 && status < 300)
        return xasprintf("%s sent an empty or truncated /models response", name);
    if (status != 0)
        return xasprintf("listing %s models failed (HTTP %ld)", name, status);
    return xasprintf("could not reach %s at %s", name, base_url);
}
