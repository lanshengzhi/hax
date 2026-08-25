/* SPDX-License-Identifier: MIT */
#include "providers/responses_events.h"

#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>

#include "provider.h"
#include "util.h"
#include "providers/responses_json.h"

struct responses_tool_call {
    char *item_id;
    char *call_id;
    int saw_args_delta;
};

static void init_usage(struct stream_usage *usage)
{
    *usage = (struct stream_usage){
        .input_tokens = -1,
        .output_tokens = -1,
        .cached_tokens = -1,
        .cache_write_tokens = -1,
        .cache_write_1h_tokens = -1,
        .cost = -1,
    };
}

void responses_events_init(struct responses_events *events, stream_cb callback, void *callback_user)
{
    memset(events, 0, sizeof(*events));
    events->callback = callback;
    events->callback_user = callback_user;
}

void responses_events_free(struct responses_events *events)
{
    for (size_t i = 0; i < events->tool_call_count; i++) {
        free(events->tool_calls[i].item_id);
        free(events->tool_calls[i].call_id);
    }
    free(events->tool_calls);
    events->tool_calls = NULL;
    events->tool_call_count = 0;
    events->tool_call_capacity = 0;
    free(events->reasoning_item_id);
    events->reasoning_item_id = NULL;
    free(events->response_id);
    events->response_id = NULL;
    free(events->served_model);
    events->served_model = NULL;
}

/* Events consumers close the reasoning block at; see turn_consume and the interactive renderer.
 * Tool argument and end events are invisible there, leaving an open reasoning block open. */
static int event_is_reasoning_seam(enum stream_event_kind kind)
{
    return kind == EV_TEXT_DELTA || kind == EV_TOOL_CALL_START || kind == EV_REASONING_ITEM ||
           kind == EV_DONE || kind == EV_ERROR;
}

static void emit_event(struct responses_events *events, const struct stream_event *event)
{
    /* Part tracking must not survive a seam: a separator injected after one would open the
     * next reasoning block with a stray blank line. */
    if (event_is_reasoning_seam(event->kind)) {
        free(events->reasoning_item_id);
        events->reasoning_item_id = NULL;
    }
    events->callback(event, events->callback_user);
}

static struct responses_tool_call *find_tool_call(struct responses_events *events,
                                                  const char *item_id)
{
    if (!item_id)
        return NULL;

    for (size_t i = 0; i < events->tool_call_count; i++) {
        if (strcmp(events->tool_calls[i].item_id, item_id) == 0)
            return &events->tool_calls[i];
    }
    return NULL;
}

static void add_tool_call(struct responses_events *events, const char *item_id, const char *call_id)
{
    if (events->tool_call_count == events->tool_call_capacity) {
        size_t capacity = events->tool_call_capacity ? events->tool_call_capacity * 2 : 4;
        events->tool_calls = (responses_tool_call *)xrealloc(
            events->tool_calls, capacity * sizeof(*events->tool_calls));
        events->tool_call_capacity = capacity;
    }

    struct responses_tool_call *tool_call = &events->tool_calls[events->tool_call_count++];
    tool_call->item_id = xstrdup(item_id);
    tool_call->call_id = xstrdup(call_id);
    tool_call->saw_args_delta = 0;
}

static void handle_output_item_added(struct responses_events *events,
                                     const hax::responses_json::parsed_event &event)
{
    if (!event.item || !event.item->type || *event.item->type != "function_call")
        return;

    const hax::responses_json::parsed_output_item &item = *event.item;
    if (!item.id || !item.call_id || !item.name)
        return;

    add_tool_call(events, item.id->c_str(), item.call_id->c_str());
    struct stream_event stream_event = {
        .kind = EV_TOOL_CALL_START,
        .u = {.tool_call_start = {.id = item.call_id->c_str(), .name = item.name->c_str()}},
    };
    emit_event(events, &stream_event);
}

static void handle_tool_call_done(struct responses_events *events,
                                  const hax::responses_json::parsed_output_item &item)
{
    struct responses_tool_call *tool_call =
        find_tool_call(events, item.id ? item.id->c_str() : NULL);
    if (!tool_call)
        return;

    /* Some backends (OpenCode's Grok, for one) skip argument delta events and deliver the
     * complete arguments only on the item itself. */
    if (!tool_call->saw_args_delta && item.arguments && !item.arguments->empty()) {
        struct stream_event delta_event = {
            .kind = EV_TOOL_CALL_DELTA,
            .u = {.tool_call_delta = {.id = tool_call->call_id,
                                      .args_delta = item.arguments->c_str()}},
        };
        emit_event(events, &delta_event);
    }

    struct stream_event stream_event = {
        .kind = EV_TOOL_CALL_END,
        .u = {.tool_call_end = {.id = tool_call->call_id}},
    };
    emit_event(events, &stream_event);
}

static void handle_reasoning_item_done(struct responses_events *events,
                                       const hax::responses_json::parsed_output_item &item)
{
    auto encoded = hax::responses_json::encode_reasoning_item(item);
    if (!encoded)
        return;

    struct stream_event stream_event = {
        .kind = EV_REASONING_ITEM,
        .u = {.reasoning_item = {.json = encoded->c_str()}},
    };
    emit_event(events, &stream_event);
}

static void handle_output_item_done(struct responses_events *events,
                                    const hax::responses_json::parsed_event &event)
{
    if (!event.item || !event.item->type)
        return;

    if (*event.item->type == "function_call")
        handle_tool_call_done(events, *event.item);
    else if (*event.item->type == "reasoning")
        handle_reasoning_item_done(events, *event.item);
}

static void handle_text_delta(struct responses_events *events,
                              const hax::responses_json::parsed_event &event)
{
    if (!event.delta)
        return;

    struct stream_event stream_event = {
        .kind = EV_TEXT_DELTA,
        .u = {.text_delta = {.text = event.delta->c_str()}},
    };
    emit_event(events, &stream_event);
}

/* Reasoning summaries and raw reasoning stream as indexed parts with no separator on the wire,
 * so adjacent parts would render glued together. A hard line break puts each part on its own
 * line. Display-only: replay uses the opaque reasoning item, whose summary is copied verbatim,
 * so injected bytes never reach the provider. */
static void emit_reasoning_part_break(struct responses_events *events,
                                      const hax::responses_json::parsed_event &event)
{
    const std::string *item_id = event.item_id ? &*event.item_id : NULL;
    const bool is_content = !event.summary_index_present;
    const std::optional<long> &index_value = is_content ? event.content_index : event.summary_index;
    if (!item_id || !index_value)
        return;

    const int part_index = (int)*index_value;
    const int same_item =
        events->reasoning_item_id && strcmp(events->reasoning_item_id, item_id->c_str()) == 0;
    /* A tracked previous part means no EV_REASONING_ITEM sealed it, so an item change needs an
     * injected boundary just like a part change: backends that return no encrypted content give
     * consumers no other seam between consecutive reasoning items. */
    const int part_changed =
        events->reasoning_item_id && (!same_item || part_index != events->reasoning_part_index ||
                                      (int)is_content != events->reasoning_part_is_content);
    if (part_changed) {
        struct stream_event stream_event = {
            .kind = EV_REASONING_DELTA,
            .u = {.reasoning_delta = {.text = "  \n"}},
        };
        emit_event(events, &stream_event);
    }
    if (!same_item) {
        free(events->reasoning_item_id);
        events->reasoning_item_id = xstrdup(item_id->c_str());
    }
    events->reasoning_part_index = part_index;
    events->reasoning_part_is_content = is_content;
}

static void handle_reasoning_delta(struct responses_events *events,
                                   const hax::responses_json::parsed_event &event)
{
    if (!event.delta || event.delta->empty())
        return;

    emit_reasoning_part_break(events, event);
    struct stream_event stream_event = {
        .kind = EV_REASONING_DELTA,
        .u = {.reasoning_delta = {.text = event.delta->c_str()}},
    };
    emit_event(events, &stream_event);
}

static void handle_tool_call_delta(struct responses_events *events,
                                   const hax::responses_json::parsed_event &event)
{
    if (!event.item_id || !event.delta || event.delta->empty())
        return;

    struct responses_tool_call *tool_call = find_tool_call(events, event.item_id->c_str());
    if (!tool_call)
        return;

    tool_call->saw_args_delta = 1;
    struct stream_event stream_event = {
        .kind = EV_TOOL_CALL_DELTA,
        .u = {.tool_call_delta = {.id = tool_call->call_id, .args_delta = event.delta->c_str()}},
    };
    emit_event(events, &stream_event);
}

static void capture_response(struct responses_events *events,
                             const hax::responses_json::parsed_event &event)
{
    if (!event.response)
        return;

    const hax::responses_json::parsed_response &response = *event.response;
    if (response.id && !response.id->empty() && !events->response_id)
        events->response_id = xstrdup(response.id->c_str());
    if (response.model && !response.model->empty() && !events->served_model)
        events->served_model = xstrdup(response.model->c_str());
}

static struct stream_response response_of(const struct responses_events *events)
{
    return (struct stream_response){.id = events->response_id, .model = events->served_model};
}

/* Usage arrives on terminal events under response.usage. A missing cached-token field is
 * unknown rather than a known cache miss. */
static void parse_usage(const hax::responses_json::parsed_event *event, struct stream_usage *usage)
{
    init_usage(usage);
    if (!event || !event->response || !event->response->usage)
        return;

    const hax::responses_json::parsed_usage &response_usage = *event->response->usage;
    if (response_usage.input_tokens)
        usage->input_tokens = *response_usage.input_tokens;
    if (response_usage.output_tokens)
        usage->output_tokens = *response_usage.output_tokens;
    if (response_usage.cached_tokens)
        usage->cached_tokens = *response_usage.cached_tokens;
}

static void emit_terminal_error(struct responses_events *events, const char *message,
                                const hax::responses_json::parsed_event *event)
{
    if (events->terminal_emitted)
        return;

    events->terminal_emitted = 1;
    struct stream_usage usage;
    parse_usage(event, &usage);
    struct stream_response response = response_of(events);
    struct stream_event stream_event = {
        .kind = EV_ERROR,
        .u = {.error =
                  {.message = message, .http_status = 0, .usage = &usage, .response = &response}},
    };
    emit_event(events, &stream_event);
}

static void handle_failed(struct responses_events *events,
                          const hax::responses_json::parsed_event &event)
{
    const char *message = NULL;
    if (event.response && event.response->error_message)
        message = event.response->error_message->c_str();
    emit_terminal_error(events, message ? message : "response.failed", &event);
}

/* A bare `error` event carries the failure at the top level rather than under `response`, and no
 * terminal response event follows it. */
static void handle_stream_error(struct responses_events *events,
                                const hax::responses_json::parsed_event &event)
{
    const char *message = event.message ? event.message->c_str() : NULL;
    if (!message && event.code)
        message = event.code->c_str();
    emit_terminal_error(events, message ? message : "provider error", &event);
}

static void handle_incomplete(struct responses_events *events,
                              const hax::responses_json::parsed_event &event)
{
    const char *reason = event.response && event.response->incomplete_reason
                             ? event.response->incomplete_reason->c_str()
                             : NULL;
    char *message = xasprintf("response incomplete: %s", reason ? reason : "unknown");
    emit_terminal_error(events, message, &event);
    free(message);
}

static void handle_completed(struct responses_events *events,
                             const hax::responses_json::parsed_event &event)
{
    if (events->terminal_emitted)
        return;

    events->terminal_emitted = 1;
    struct stream_event stream_event = {
        .kind = EV_DONE,
        .u = {.done = {.stop_reason = "completed", .response = response_of(events)}},
    };
    parse_usage(&event, &stream_event.u.done.usage);
    emit_event(events, &stream_event);
}

void responses_events_feed(struct responses_events *events, const char *data)
{
    if (!data || !*data)
        return;
    if (strcmp(data, "[DONE]") == 0) {
        handle_completed(events, hax::responses_json::parsed_event{});
        return;
    }

    auto event = hax::responses_json::parse_event(data);
    if (!event || !event->type)
        return;

    capture_response(events, *event);

    const std::string &type = *event->type;
    if (type == "response.output_item.added")
        handle_output_item_added(events, *event);
    else if (type == "response.output_item.done")
        handle_output_item_done(events, *event);
    /* A refusal is the assistant's answer, carried in its own content part. Dropping it would
     * complete the response with no text at all. */
    else if (type == "response.output_text.delta" || type == "response.refusal.delta")
        handle_text_delta(events, *event);
    else if (type == "response.reasoning_summary_text.delta" ||
             type == "response.reasoning_text.delta")
        handle_reasoning_delta(events, *event);
    else if (type == "response.function_call_arguments.delta")
        handle_tool_call_delta(events, *event);
    else if (type == "response.completed" || type == "response.done")
        handle_completed(events, *event);
    else if (type == "response.incomplete")
        handle_incomplete(events, *event);
    else if (type == "response.failed")
        handle_failed(events, *event);
    else if (type == "error")
        handle_stream_error(events, *event);
}

void responses_events_finalize(struct responses_events *events)
{
    emit_terminal_error(events, "stream ended before completion", NULL);
}
