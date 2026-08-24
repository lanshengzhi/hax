/* SPDX-License-Identifier: MIT */
#include "providers/chat_events.h"

#include <stdlib.h>
#include <string.h>
#include <string>

#include "provider.h"
#include "util.h"
#include "providers/openai_compat_json.h"

void chat_events_init(struct chat_events *parser, stream_cb callback, void *callback_user)
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

void chat_events_free(struct chat_events *parser)
{
    for (size_t i = 0; i < parser->n_tool_calls; i++) {
        free(parser->tool_calls[i].id);
        free(parser->tool_calls[i].name);
        buf_free(&parser->tool_calls[i].arguments_before_start);
    }
    free(parser->tool_calls);
    parser->tool_calls = NULL;
    parser->n_tool_calls = parser->tool_call_capacity = 0;

    free(parser->finish_reason);
    parser->finish_reason = NULL;
    free(parser->finish_error);
    parser->finish_error = NULL;
    free(parser->response_id);
    parser->response_id = NULL;
    free(parser->served_model);
    parser->served_model = NULL;
    free(parser->route);
    parser->route = NULL;
    free(parser->reasoning_details_json);
    parser->reasoning_details_json = NULL;
}

static struct stream_response response_of(const struct chat_events *parser)
{
    return (struct stream_response){
        .id = parser->response_id,
        .model = parser->served_model,
        .route = parser->route,
    };
}

static struct chat_tool_call *find_tool_call(struct chat_events *parser, int index)
{
    for (size_t i = 0; i < parser->n_tool_calls; i++) {
        if (parser->tool_calls[i].index == index)
            return &parser->tool_calls[i];
    }
    return NULL;
}

static struct chat_tool_call *get_tool_call(struct chat_events *parser, int index)
{
    struct chat_tool_call *call = find_tool_call(parser, index);
    if (call)
        return call;

    if (parser->n_tool_calls == parser->tool_call_capacity) {
        size_t capacity = parser->tool_call_capacity ? parser->tool_call_capacity * 2 : 4;
        parser->tool_calls =
            (chat_tool_call *)xrealloc(parser->tool_calls, capacity * sizeof(*parser->tool_calls));
        parser->tool_call_capacity = capacity;
    }

    call = &parser->tool_calls[parser->n_tool_calls++];
    memset(call, 0, sizeof(*call));
    call->index = index;
    return call;
}

static void emit_event(struct chat_events *parser, const struct stream_event *event)
{
    parser->callback(event, parser->callback_user);
}

static void capture_response(struct chat_events *parser,
                             const hax::openai_compat_json::chat_chunk &chunk)
{
    if (!parser->response_id && chunk.id && !chunk.id->empty())
        parser->response_id = xstrdup(chunk.id->c_str());
    if (!parser->served_model && chunk.model && !chunk.model->empty())
        parser->served_model = xstrdup(chunk.model->c_str());
    if (!parser->route && chunk.provider && !chunk.provider->empty())
        parser->route = xstrdup(chunk.provider->c_str());
}

static void start_tool_call(struct chat_events *parser, struct chat_tool_call *call)
{
    if (call->started || !call->name)
        return;

    /* Some compatible servers omit ids; the id only needs to survive the result round trip. */
    if (!call->id)
        call->id = xasprintf("call_%d", call->index);

    struct stream_event start = {
        .kind = EV_TOOL_CALL_START,
        .u = {.tool_call_start = {.id = call->id, .name = call->name}},
    };
    emit_event(parser, &start);
    call->started = 1;

    if (call->arguments_before_start.len > 0) {
        struct stream_event arguments = {
            .kind = EV_TOOL_CALL_DELTA,
            .u = {.tool_call_delta =
                      {
                          .id = call->id,
                          .args_delta = call->arguments_before_start.data,
                      }},
        };
        emit_event(parser, &arguments);
        buf_reset(&call->arguments_before_start);
    }
}

static void handle_text_delta(struct chat_events *parser, const char *text)
{
    if (!text || !*text)
        return;

    struct stream_event event = {
        .kind = EV_TEXT_DELTA,
        .u = {.text_delta = {.text = text}},
    };
    emit_event(parser, &event);
}

static void handle_reasoning_delta(struct chat_events *parser,
                                   const hax::openai_compat_json::chat_delta &delta)
{
    const std::string *text = delta.reasoning ? &*delta.reasoning : NULL;
    if (!text)
        text = delta.reasoning_content ? &*delta.reasoning_content : NULL;
    if (!text || text->empty())
        return;

    struct stream_event event = {
        .kind = EV_REASONING_DELTA,
        .u = {.reasoning_delta = {.text = text->c_str()}},
    };
    emit_event(parser, &event);
}

/* Reasoning arrives as an ordered sequence of opaque blocks. The adapter joins only adjacent text
 * fragments and keeps all other provider fields intact before the item crosses the stream seam. */
static void collect_reasoning_details(struct chat_events *parser,
                                      const hax::openai_compat_json::chat_delta &delta)
{
    if (!delta.has_reasoning_details)
        return;

    for (const std::string &detail : delta.reasoning_details) {
        char *joined = hax::openai_compat_json::append_reasoning_detail(
            parser->reasoning_details_json, detail);
        if (!joined)
            continue;
        free(parser->reasoning_details_json);
        parser->reasoning_details_json = joined;
    }
}

/* Chat Completions marks no end of reasoning, so the collected sequence is sealed at the first
 * seam that follows it: content, a tool call, or the end of the stream. */
static void flush_reasoning_details(struct chat_events *parser)
{
    if (!parser->reasoning_details_json)
        return;

    struct stream_event event = {
        .kind = EV_REASONING_ITEM,
        .u = {.reasoning_item = {.json = parser->reasoning_details_json}},
    };
    emit_event(parser, &event);
    free(parser->reasoning_details_json);
    parser->reasoning_details_json = NULL;
}

static void handle_tool_call_delta(struct chat_events *parser,
                                   const hax::openai_compat_json::chat_tool_call_delta &delta)
{
    /* The specification requires index, but single-call compatible streams often omit it. */
    int index = delta.index ? (int)*delta.index : 0;
    struct chat_tool_call *call = get_tool_call(parser, index);

    if (delta.id && !call->id)
        call->id = xstrdup(delta.id->c_str());
    if (delta.name && !call->name)
        call->name = xstrdup(delta.name->c_str());

    start_tool_call(parser, call);

    if (!delta.arguments || delta.arguments->empty())
        return;
    if (!call->started) {
        buf_append_str(&call->arguments_before_start, delta.arguments->c_str());
        return;
    }

    struct stream_event event = {
        .kind = EV_TOOL_CALL_DELTA,
        .u = {.tool_call_delta = {.id = call->id, .args_delta = delta.arguments->c_str()}},
    };
    emit_event(parser, &event);
}

static void finish_tool_calls(struct chat_events *parser)
{
    for (size_t i = 0; i < parser->n_tool_calls; i++) {
        struct chat_tool_call *call = &parser->tool_calls[i];
        if (!call->started || call->finished)
            continue;

        struct stream_event event = {
            .kind = EV_TOOL_CALL_END,
            .u = {.tool_call_end = {.id = call->id}},
        };
        emit_event(parser, &event);
        call->finished = 1;
    }
}

static void capture_usage(struct chat_events *parser,
                          const std::optional<hax::openai_compat_json::chat_usage> &usage)
{
    if (!usage)
        return;

    if (usage->prompt_tokens)
        parser->usage.input_tokens = *usage->prompt_tokens;
    if (usage->completion_tokens)
        parser->usage.output_tokens = *usage->completion_tokens;
    if (usage->cached_tokens)
        parser->usage.cached_tokens = *usage->cached_tokens;
    if (usage->cache_write_tokens) {
        parser->usage.cache_write_tokens = *usage->cache_write_tokens;
        /* The response does not identify the TTL; only the request does. */
        if (parser->cache_write_1h)
            parser->usage.cache_write_1h_tokens = *usage->cache_write_tokens;
    }
    if (usage->cost && *usage->cost >= 0)
        parser->usage.cost = *usage->cost;
}

static void handle_progress(struct chat_events *parser,
                            const std::optional<hax::openai_compat_json::chat_progress> &progress)
{
    if (!parser->emit_progress || !progress)
        return;

    struct stream_event event = {
        .kind = EV_PROGRESS,
        .u = {.progress = {0}},
    };
    if (progress->processed)
        event.u.progress.processed = *progress->processed;
    if (progress->total)
        event.u.progress.total = *progress->total;
    if (progress->cache)
        event.u.progress.cache = *progress->cache;
    emit_event(parser, &event);
}

static void emit_terminal_event(struct chat_events *parser)
{
    struct stream_response response = response_of(parser);
    if (parser->finish_error) {
        struct stream_event event = {
            .kind = EV_ERROR,
            .u = {.error =
                      {
                          .message = parser->finish_error,
                          .http_status = 0,
                          .usage = &parser->usage,
                          .response = &response,
                      }},
        };
        emit_event(parser, &event);
        return;
    }

    struct stream_event event = {
        .kind = EV_DONE,
        .u = {.done =
                  {
                      .stop_reason = parser->finish_reason ? parser->finish_reason : "stop",
                      .usage = parser->usage,
                      .response = response,
                  }},
    };
    emit_event(parser, &event);
}

static void handle_finish_reason(struct chat_events *parser, const char *reason)
{
    if (parser->terminal_emitted || parser->finish_received)
        return;

    flush_reasoning_details(parser);
    finish_tool_calls(parser);
    parser->finish_received = 1;

    int truncated =
        reason && (strcmp(reason, "length") == 0 || strcmp(reason, "content_filter") == 0);
    if (!truncated) {
        parser->finish_reason = xstrdup(reason ? reason : "stop");
        return;
    }

    if (strcmp(reason, "length") == 0 && parser->length_hint)
        parser->finish_error = xasprintf("response incomplete: length — %s", parser->length_hint);
    else
        parser->finish_error = xasprintf("response incomplete: %s", reason);
}

static void handle_done(struct chat_events *parser)
{
    if (parser->terminal_emitted)
        return;

    flush_reasoning_details(parser);
    finish_tool_calls(parser);
    parser->terminal_emitted = 1;
    emit_terminal_event(parser);
}

static void handle_error(struct chat_events *parser,
                         const hax::openai_compat_json::chat_error &error)
{
    if (parser->terminal_emitted)
        return;

    parser->terminal_emitted = 1;
    const char *message = error.message ? error.message->c_str() : "provider error";
    struct stream_response response = response_of(parser);
    struct stream_event event = {
        .kind = EV_ERROR,
        .u = {.error =
                  {
                      .message = message,
                      .http_status = 0,
                      .usage = &parser->usage,
                      .response = &response,
                  }},
    };
    emit_event(parser, &event);
}

static void handle_choice_delta(struct chat_events *parser,
                                const hax::openai_compat_json::chat_choice &choice)
{
    if (choice.delta) {
        collect_reasoning_details(parser, *choice.delta);
        handle_reasoning_delta(parser, *choice.delta);

        const char *content = choice.delta->content ? choice.delta->content->c_str() : NULL;
        if ((content && *content) || choice.delta->has_tool_calls)
            flush_reasoning_details(parser);

        handle_text_delta(parser, content);
        if (choice.delta->has_tool_calls)
            for (const auto &tool_call : choice.delta->tool_calls)
                handle_tool_call_delta(parser, tool_call);
    }

    if (choice.finish_reason)
        handle_finish_reason(parser, choice.finish_reason->c_str());
}

void chat_events_feed(struct chat_events *parser, const char *data)
{
    if (parser->terminal_emitted || !data || !*data)
        return;
    if (strcmp(data, "[DONE]") == 0) {
        handle_done(parser);
        return;
    }

    auto chunk = hax::openai_compat_json::parse_chat_chunk(data);
    if (!chunk)
        return;

    if (chunk->error) {
        handle_error(parser, *chunk->error);
        return;
    }

    /* Usage and progress chunks may have no choices. */
    capture_response(parser, *chunk);
    capture_usage(parser, chunk->usage);
    handle_progress(parser, chunk->progress);
    if (!chunk->choices.empty())
        handle_choice_delta(parser, chunk->choices[0]);
}

void chat_events_finalize(struct chat_events *parser)
{
    if (parser->terminal_emitted)
        return;

    parser->terminal_emitted = 1;
    if (parser->finish_received) {
        emit_terminal_event(parser);
        return;
    }

    struct stream_response response = response_of(parser);
    struct stream_event event = {
        .kind = EV_ERROR,
        .u = {.error =
                  {
                      .message = "stream ended before completion",
                      .http_status = 0,
                      .usage = &parser->usage,
                      .response = &response,
                  }},
    };
    emit_event(parser, &event);
}
