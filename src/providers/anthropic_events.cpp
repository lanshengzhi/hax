/* SPDX-License-Identifier: MIT */
#include "providers/anthropic_events.h"

#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>

#include "provider.h"
#include "util.h"
#include "providers/anthropic_json.h"

void anthropic_events_init(struct anthropic_events *parser, stream_cb callback, void *callback_user)
{
    memset(parser, 0, sizeof(*parser));
    parser->callback = callback;
    parser->callback_user = callback_user;
    parser->usage.input_tokens = -1;
    parser->usage.output_tokens = -1;
    parser->usage.cached_tokens = -1;
    parser->usage.cache_write_tokens = -1;
    parser->usage.cache_write_1h_tokens = -1;
    parser->usage.cost = -1;
}

void anthropic_events_free(struct anthropic_events *parser)
{
    for (size_t i = 0; i < parser->n_blocks; i++) {
        free(parser->blocks[i].tool_call_id);
        free(parser->blocks[i].tool_name);
        free(parser->blocks[i].redacted_data);
        buf_free(&parser->blocks[i].thinking);
        buf_free(&parser->blocks[i].signature);
    }
    free(parser->blocks);
    parser->blocks = NULL;
    parser->n_blocks = parser->block_capacity = 0;
    free(parser->stop_reason);
    parser->stop_reason = NULL;
    free(parser->response_id);
    parser->response_id = NULL;
    free(parser->served_model);
    parser->served_model = NULL;
}

static int emit(struct anthropic_events *parser, const struct stream_event *event)
{
    return parser->callback(event, parser->callback_user);
}

static struct stream_response response_of(const struct anthropic_events *parser)
{
    return (struct stream_response){.id = parser->response_id, .model = parser->served_model};
}

static struct anthropic_content_block *find_block(struct anthropic_events *parser, int index)
{
    for (size_t i = 0; i < parser->n_blocks; i++) {
        if (parser->blocks[i].index == index)
            return &parser->blocks[i];
    }
    return NULL;
}

static struct anthropic_content_block *add_block(struct anthropic_events *parser, int index)
{
    if (parser->n_blocks == parser->block_capacity) {
        size_t capacity = parser->block_capacity ? parser->block_capacity * 2 : 4;
        parser->blocks =
            (anthropic_content_block *)xrealloc(parser->blocks, capacity * sizeof(*parser->blocks));
        parser->block_capacity = capacity;
    }

    struct anthropic_content_block *block = &parser->blocks[parser->n_blocks++];
    memset(block, 0, sizeof(*block));
    block->index = index;
    buf_init(&block->thinking);
    buf_init(&block->signature);
    return block;
}

/* An empty delta starts the reasoning indicator when plaintext is omitted. */
static void emit_reasoning_start(struct anthropic_events *parser)
{
    struct stream_event event = {
        .kind = EV_REASONING_DELTA,
        .u = {.reasoning_delta = {.text = ""}},
    };
    emit(parser, &event);
}

static void handle_content_block_start(struct anthropic_events *parser,
                                       const hax::anthropic_json::parsed_event &event)
{
    const int index = event.index ? (int)*event.index : 0;
    if (!event.content_block || !event.content_block->type)
        return;

    const std::string &type = *event.content_block->type;
    struct anthropic_content_block *block = find_block(parser, index);
    if (!block)
        block = add_block(parser, index);

    if (type == "text") {
        block->kind = ANTHROPIC_CONTENT_TEXT;
    } else if (type == "thinking") {
        block->kind = ANTHROPIC_CONTENT_THINKING;
        emit_reasoning_start(parser);
    } else if (type == "redacted_thinking") {
        block->kind = ANTHROPIC_CONTENT_REDACTED_THINKING;
        const std::string *data = event.content_block->data ? &*event.content_block->data : NULL;
        if (data)
            block->redacted_data = xstrdup(data->c_str());
        emit_reasoning_start(parser);
    } else if (type == "tool_use") {
        block->kind = ANTHROPIC_CONTENT_TOOL_USE;
        const std::string *id = event.content_block->id ? &*event.content_block->id : NULL;
        const std::string *name = event.content_block->name ? &*event.content_block->name : NULL;
        block->tool_call_id = xstrdup(id ? id->c_str() : "");
        block->tool_name = xstrdup(name ? name->c_str() : "");
        struct stream_event start = {
            .kind = EV_TOOL_CALL_START,
            .u = {.tool_call_start = {.id = block->tool_call_id, .name = block->tool_name}},
        };
        emit(parser, &start);
        block->tool_started = 1;
    } else {
        block->kind = ANTHROPIC_CONTENT_OTHER;
    }
}

static void handle_content_block_delta(struct anthropic_events *parser,
                                       const hax::anthropic_json::parsed_event &event)
{
    const int index = event.index ? (int)*event.index : 0;
    if (!event.delta || !event.delta->type)
        return;

    const hax::anthropic_json::parsed_delta &delta = *event.delta;
    const std::string &type = *delta.type;
    struct anthropic_content_block *block = find_block(parser, index);
    if (type == "text_delta") {
        if (delta.text && !delta.text->empty()) {
            struct stream_event text = {
                .kind = EV_TEXT_DELTA,
                .u = {.text_delta = {.text = delta.text->c_str()}},
            };
            emit(parser, &text);
        }
    } else if (type == "thinking_delta") {
        if (delta.thinking && !delta.thinking->empty()) {
            if (block)
                buf_append_str(&block->thinking, delta.thinking->c_str());
            struct stream_event reasoning = {
                .kind = EV_REASONING_DELTA,
                .u = {.reasoning_delta = {.text = delta.thinking->c_str()}},
            };
            emit(parser, &reasoning);
        }
    } else if (type == "signature_delta") {
        if (delta.signature && !delta.signature->empty() && block)
            buf_append_str(&block->signature, delta.signature->c_str());
    } else if (type == "input_json_delta") {
        if (delta.partial_json && !delta.partial_json->empty() && block && block->tool_started) {
            struct stream_event arguments = {
                .kind = EV_TOOL_CALL_DELTA,
                .u = {.tool_call_delta = {.id = block->tool_call_id,
                                          .args_delta = delta.partial_json->c_str()}},
            };
            emit(parser, &arguments);
        }
    }
}

/* Anthropic requires thinking blocks and signatures to be replayed on the next request. */
static void emit_reasoning_item(struct anthropic_events *parser,
                                struct anthropic_content_block *block)
{
    std::optional<std::string> encoded;
    if (block->kind == ANTHROPIC_CONTENT_REDACTED_THINKING) {
        if (!block->redacted_data)
            return;
        encoded = hax::anthropic_json::encode_redacted_thinking_item(block->redacted_data);
    } else {
        const char *text = block->thinking.data ? block->thinking.data : "";
        const char *signature = block->signature.data ? block->signature.data : "";
        if (!*text && !*signature)
            return;
        encoded = hax::anthropic_json::encode_thinking_item(text, signature);
    }
    if (!encoded)
        return;

    struct stream_event event = {
        .kind = EV_REASONING_ITEM,
        .u = {.reasoning_item = {.json = encoded->c_str()}},
    };
    emit(parser, &event);
}

static void handle_content_block_stop(struct anthropic_events *parser,
                                      const hax::anthropic_json::parsed_event &event)
{
    const int index = event.index ? (int)*event.index : 0;
    struct anthropic_content_block *block = find_block(parser, index);
    if (!block)
        return;

    if (block->kind == ANTHROPIC_CONTENT_TOOL_USE && block->tool_started) {
        struct stream_event end = {
            .kind = EV_TOOL_CALL_END,
            .u = {.tool_call_end = {.id = block->tool_call_id}},
        };
        emit(parser, &end);
    } else if (block->kind == ANTHROPIC_CONTENT_THINKING ||
               block->kind == ANTHROPIC_CONTENT_REDACTED_THINKING) {
        emit_reasoning_item(parser, block);
    }
}

/* Anthropic reports cached input in addition to input_tokens, not as a subset of it. */
static void capture_usage(struct anthropic_events *parser,
                          const hax::anthropic_json::parsed_usage *usage)
{
    if (!usage)
        return;

    if (usage->input_tokens && *usage->input_tokens >= 0) {
        parser->usage.input_tokens = *usage->input_tokens;
        if (usage->cache_read_input_tokens && *usage->cache_read_input_tokens > 0)
            parser->usage.input_tokens += *usage->cache_read_input_tokens;
        if (usage->cache_creation_input_tokens && *usage->cache_creation_input_tokens > 0)
            parser->usage.input_tokens += *usage->cache_creation_input_tokens;
    }
    if (usage->cache_read_input_tokens && *usage->cache_read_input_tokens >= 0)
        parser->usage.cached_tokens = *usage->cache_read_input_tokens;
    if (usage->cache_creation_input_tokens && *usage->cache_creation_input_tokens >= 0)
        parser->usage.cache_write_tokens = *usage->cache_creation_input_tokens;
    if (usage->cache_write_1h_input_tokens)
        parser->usage.cache_write_1h_tokens = *usage->cache_write_1h_input_tokens;
    if (usage->output_tokens && *usage->output_tokens >= 0)
        parser->usage.output_tokens = *usage->output_tokens;
}

static void handle_message_start(struct anthropic_events *parser,
                                 const hax::anthropic_json::parsed_event &event)
{
    if (!event.message)
        return;
    const hax::anthropic_json::parsed_message &message = *event.message;
    if (message.id && !message.id->empty() && !parser->response_id)
        parser->response_id = xstrdup(message.id->c_str());
    if (message.model && !message.model->empty() && !parser->served_model)
        parser->served_model = xstrdup(message.model->c_str());
    capture_usage(parser, message.usage ? &*message.usage : NULL);
}

static void handle_message_delta(struct anthropic_events *parser,
                                 const hax::anthropic_json::parsed_event &event)
{
    if (event.delta && event.delta->stop_reason) {
        free(parser->stop_reason);
        parser->stop_reason = xstrdup(event.delta->stop_reason->c_str());
    }
    capture_usage(parser, event.usage ? &*event.usage : NULL);
}

static void emit_terminal_error(struct anthropic_events *parser, const char *message)
{
    parser->terminal_emitted = 1;
    struct stream_response response = response_of(parser);
    struct stream_event event = {
        .kind = EV_ERROR,
        .u = {.error = {.message = message,
                        .http_status = 0,
                        .usage = &parser->usage,
                        .response = &response}},
    };
    emit(parser, &event);
}

static void handle_message_stop(struct anthropic_events *parser)
{
    if (parser->terminal_emitted)
        return;

    const char *reason = parser->stop_reason;
    if (reason && strcmp(reason, "max_tokens") == 0) {
        emit_terminal_error(parser, "response incomplete: max_tokens — raise the provider's "
                                    "max_tokens or lower the effort level");
        return;
    }
    /* pause_turn requires replaying server-side tool state, which hax does not drive. */
    if (reason && strcmp(reason, "pause_turn") == 0) {
        emit_terminal_error(parser, "response paused before completion (pause_turn)");
        return;
    }

    parser->terminal_emitted = 1;
    struct stream_event event = {
        .kind = EV_DONE,
        .u = {.done = {.stop_reason = reason ? reason : "end_turn",
                       .usage = parser->usage,
                       .response = response_of(parser)}},
    };
    emit(parser, &event);
}

static void handle_error(struct anthropic_events *parser,
                         const hax::anthropic_json::parsed_event &event)
{
    if (parser->terminal_emitted)
        return;

    const char *message = NULL;
    if (event.error && event.error->message)
        message = event.error->message->c_str();
    emit_terminal_error(parser, message ? message : "provider error");
}

void anthropic_events_feed(struct anthropic_events *parser, const char *event_name,
                           const char *data)
{
    (void)event_name;
    if (parser->terminal_emitted || !data || !*data)
        return;

    auto event = hax::anthropic_json::parse_event(data);
    if (!event || !event->type)
        return;

    const std::string &type = *event->type;
    if (type == "message_start")
        handle_message_start(parser, *event);
    else if (type == "content_block_start")
        handle_content_block_start(parser, *event);
    else if (type == "content_block_delta")
        handle_content_block_delta(parser, *event);
    else if (type == "content_block_stop")
        handle_content_block_stop(parser, *event);
    else if (type == "message_delta")
        handle_message_delta(parser, *event);
    else if (type == "message_stop")
        handle_message_stop(parser);
    else if (type == "error")
        handle_error(parser, *event);
}

void anthropic_events_finalize(struct anthropic_events *parser)
{
    if (!parser->terminal_emitted)
        emit_terminal_error(parser, "stream ended before completion");
}
