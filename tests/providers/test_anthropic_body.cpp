/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "json_helpers.h"
#include "provider.h"
#include "providers/anthropic_body.h"
#include "providers/wire.h"

static test_json *message_at(test_json *messages, size_t i)
{
    return test_json_array_get(messages, i);
}

static const char *message_role(test_json *message)
{
    return test_json_string(test_json_get(message, "role"));
}

static test_json *message_content(test_json *message)
{
    return test_json_get(message, "content");
}

static const char *block_type(test_json *block)
{
    return test_json_string(test_json_get(block, "type"));
}

static void test_user_message(void)
{
    struct item items[] = {{.kind = ITEM_USER_MESSAGE, .text = "hello"}};
    test_json *messages =
        test_json_owned(anthropic_build_messages(items, 1, "anthropic", "m", 0, -1));
    EXPECT(test_json_size(messages) == 1);
    EXPECT_STR_EQ(message_role(message_at(messages, 0)), "user");
    test_json *blocks = message_content(message_at(messages, 0));
    EXPECT(test_json_size(blocks) == 1);
    EXPECT_STR_EQ(block_type(test_json_array_get(blocks, 0)), "text");
    EXPECT_STR_EQ(test_json_string(test_json_get(test_json_array_get(blocks, 0), "text")), "hello");
    test_json_release(messages);
}

static void test_assistant_group_thinking_text_tool(void)
{
    struct item items[] = {
        {.kind = ITEM_REASONING,
         .reasoning_json = "{\"type\":\"thinking\",\"thinking\":\"reasoned\",\"signature\":\"S\","
                           "\"future\":{\"keep\":true}}",
         .provider = "anthropic",
         .model = "m"},
        {.kind = ITEM_ASSISTANT_MESSAGE, .text = "Running it."},
        {.kind = ITEM_TOOL_CALL,
         .call_id = "toolu_1",
         .tool_name = "bash",
         .tool_arguments_json = "{\"cmd\":\"ls\"}"},
    };
    test_json *messages =
        test_json_owned(anthropic_build_messages(items, 3, "anthropic", "m", 0, -1));
    EXPECT(test_json_size(messages) == 1);
    test_json *assistant = message_at(messages, 0);
    EXPECT_STR_EQ(message_role(assistant), "assistant");
    test_json *blocks = message_content(assistant);
    EXPECT(test_json_size(blocks) == 3);
    EXPECT_STR_EQ(block_type(test_json_array_get(blocks, 0)), "thinking");
    EXPECT_STR_EQ(test_json_string(test_json_get(test_json_array_get(blocks, 0), "signature")),
                  "S");
    EXPECT(test_json_is_object(test_json_get(test_json_array_get(blocks, 0), "future")));
    EXPECT_STR_EQ(block_type(test_json_array_get(blocks, 1)), "text");
    test_json *tool_use = test_json_array_get(blocks, 2);
    EXPECT_STR_EQ(block_type(tool_use), "tool_use");
    EXPECT_STR_EQ(test_json_string(test_json_get(tool_use, "id")), "toolu_1");
    EXPECT_STR_EQ(test_json_string(test_json_get(tool_use, "name")), "bash");
    test_json *input = test_json_get(tool_use, "input");
    EXPECT(test_json_is_object(input));
    EXPECT_STR_EQ(test_json_string(test_json_get(input, "cmd")), "ls");
    test_json_release(messages);
}

static void test_empty_signature_policy(void)
{
    struct item items[] = {
        {.kind = ITEM_REASONING,
         .reasoning_json = "{\"type\":\"thinking\",\"thinking\":\"cot\",\"signature\":\"\"}",
         .provider = "anthropic",
         .model = "m"},
        {.kind = ITEM_ASSISTANT_MESSAGE, .text = "ok"},
    };

    test_json *strict =
        test_json_owned(anthropic_build_messages(items, 2, "anthropic", "m", 0, -1));
    test_json *strict_content = message_content(message_at(strict, 0));
    EXPECT(test_json_size(strict_content) == 2);
    EXPECT_STR_EQ(block_type(test_json_array_get(strict_content, 0)), "text");
    EXPECT_STR_EQ(test_json_string(test_json_get(test_json_array_get(strict_content, 0), "text")),
                  "cot");
    EXPECT_STR_EQ(block_type(test_json_array_get(strict_content, 1)), "text");
    test_json_release(strict);

    test_json *loose = test_json_owned(anthropic_build_messages(items, 2, "anthropic", "m", 1, -1));
    test_json *compat_content = message_content(message_at(loose, 0));
    EXPECT_STR_EQ(block_type(test_json_array_get(compat_content, 0)), "thinking");
    test_json_release(loose);
}

static void test_reasoning_provenance_mismatch_dropped(void)
{
    struct item items[] = {
        {.kind = ITEM_REASONING,
         .reasoning_json = "{\"type\":\"thinking\",\"thinking\":\"x\",\"signature\":\"S\"}",
         .provider = "anthropic",
         .model = "old-model"},
        {.kind = ITEM_ASSISTANT_MESSAGE, .text = "hi"},
    };
    test_json *messages =
        test_json_owned(anthropic_build_messages(items, 2, "anthropic", "new-model", 0, -1));
    test_json *blocks = message_content(message_at(messages, 0));
    EXPECT(test_json_size(blocks) == 1);
    EXPECT_STR_EQ(block_type(test_json_array_get(blocks, 0)), "text");
    test_json_release(messages);
}

static void test_tool_results_coalesced(void)
{
    struct item items[] = {
        {.kind = ITEM_TOOL_RESULT, .call_id = "a", .output = "out-a"},
        {.kind = ITEM_TOOL_RESULT, .call_id = "b", .output = "out-b"},
    };
    test_json *messages =
        test_json_owned(anthropic_build_messages(items, 2, "anthropic", "m", 0, -1));
    EXPECT(test_json_size(messages) == 1);
    test_json *user = message_at(messages, 0);
    EXPECT_STR_EQ(message_role(user), "user");
    test_json *blocks = message_content(user);
    EXPECT(test_json_size(blocks) == 2);
    EXPECT_STR_EQ(block_type(test_json_array_get(blocks, 0)), "tool_result");
    EXPECT_STR_EQ(test_json_string(test_json_get(test_json_array_get(blocks, 0), "tool_use_id")),
                  "a");
    EXPECT_STR_EQ(test_json_string(test_json_get(test_json_array_get(blocks, 1), "tool_use_id")),
                  "b");
    test_json_release(messages);
}

static void test_redacted_thinking_replayed(void)
{
    struct item items[] = {
        {.kind = ITEM_REASONING,
         .reasoning_json = "{\"type\":\"redacted_thinking\",\"data\":\"ENC\"}",
         .provider = "anthropic",
         .model = "m"},
        {.kind = ITEM_TOOL_CALL, .call_id = "t", .tool_name = "x", .tool_arguments_json = "{}"},
    };
    test_json *messages =
        test_json_owned(anthropic_build_messages(items, 2, "anthropic", "m", 0, -1));
    test_json *blocks = message_content(message_at(messages, 0));
    EXPECT(test_json_size(blocks) == 2);
    EXPECT_STR_EQ(block_type(test_json_array_get(blocks, 0)), "redacted_thinking");
    EXPECT_STR_EQ(test_json_string(test_json_get(test_json_array_get(blocks, 0), "data")), "ENC");
    test_json_release(messages);
}

static void test_tool_call_bad_args_empty_object(void)
{
    struct item items[] = {
        {.kind = ITEM_TOOL_CALL,
         .call_id = "t",
         .tool_name = "x",
         .tool_arguments_json = "not json"},
    };
    test_json *messages =
        test_json_owned(anthropic_build_messages(items, 1, "anthropic", "m", 0, -1));
    test_json *tool_use = test_json_array_get(message_content(message_at(messages, 0)), 0);
    test_json *input = test_json_get(tool_use, "input");
    EXPECT(test_json_is_object(input));
    EXPECT(test_json_object_size(input) == 0);
    test_json_release(messages);
}

static void test_tool_result_image(void)
{
    struct item_image images[] = {
        {.mime = "image/png", .data_b64 = "QUJD", .width = 4, .height = 2},
    };
    struct item items[] = {
        {.kind = ITEM_TOOL_RESULT,
         .call_id = "toolu_9",
         .output = "Read image x.png",
         .images = images,
         .n_images = 1},
    };

    test_json *messages =
        test_json_owned(anthropic_build_messages(items, 1, "anthropic", "m", 0, 1));
    test_json *content = message_content(message_at(messages, 0));
    test_json *tool_result = test_json_array_get(content, 0);
    EXPECT_STR_EQ(block_type(tool_result), "tool_result");
    test_json *blocks = test_json_get(tool_result, "content");
    EXPECT(test_json_is_array(blocks));
    EXPECT(test_json_size(blocks) == 2);
    EXPECT_STR_EQ(block_type(test_json_array_get(blocks, 0)), "text");
    test_json *image = test_json_array_get(blocks, 1);
    EXPECT_STR_EQ(block_type(image), "image");
    test_json *src = test_json_get(image, "source");
    EXPECT_STR_EQ(test_json_string(test_json_get(src, "type")), "base64");
    EXPECT_STR_EQ(test_json_string(test_json_get(src, "media_type")), "image/png");
    EXPECT_STR_EQ(test_json_string(test_json_get(src, "data")), "QUJD");
    test_json_release(messages);

    messages = test_json_owned(anthropic_build_messages(items, 1, "anthropic", "m", 0, 0));
    tool_result = test_json_array_get(message_content(message_at(messages, 0)), 0);
    blocks = test_json_get(tool_result, "content");
    EXPECT(test_json_size(blocks) == 2);
    test_json *placeholder = test_json_array_get(blocks, 1);
    EXPECT_STR_EQ(block_type(placeholder), "text");
    const char *text = test_json_string(test_json_get(placeholder, "text"));
    EXPECT(text && strstr(text, "[image:") != NULL);
    EXPECT(strstr(text, "image/png") != NULL);
    test_json_release(messages);

    struct item plain[] = {
        {.kind = ITEM_TOOL_RESULT, .call_id = "toolu_9", .output = "ok"},
    };
    messages = test_json_owned(anthropic_build_messages(plain, 1, "anthropic", "m", 0, 1));
    tool_result = test_json_array_get(message_content(message_at(messages, 0)), 0);
    EXPECT(test_json_is_string(test_json_get(tool_result, "content")));
    test_json_release(messages);
}

static const struct item BODY_ITEMS[] = {{.kind = ITEM_USER_MESSAGE, .text = "hello"}};

static const struct tool_def BODY_TOOLS[] = {{.name = "read", .description = "read a file"}};

static const struct context BODY_CONTEXT = {
    .system_prompt = "be brief",
    .items = BODY_ITEMS,
    .n_items = 1,
    .tools = BODY_TOOLS,
    .n_tools = 1,
    .effort = "high",
    .image_input = 1,
};

static void test_build_body_budget_thinking(void)
{
    struct wire_body_opts opts = {
        .cache_markers = 1,
        .cache_ttl = "1h",
        .max_tokens = 1000,
        .thinking_mode = ANTHROPIC_THINKING_BUDGET,
        .thinking_budget = 200,
    };
    test_json *body =
        test_json_owned(anthropic_build_body(&BODY_CONTEXT, "prov", "model-1", &opts));

    EXPECT_STR_EQ(test_json_string(test_json_get(body, "model")), "model-1");
    EXPECT(test_json_integer(test_json_get(body, "max_tokens")) == 1000);
    EXPECT(test_json_is_true(test_json_get(body, "stream")));
    test_json *thinking = test_json_get(body, "thinking");
    EXPECT_STR_EQ(test_json_string(test_json_get(thinking, "type")), "enabled");
    EXPECT(test_json_integer(test_json_get(thinking, "budget_tokens")) == 200);

    /* Cache markers land on the system block, the last tool, and the conversation tail. */
    test_json *system_block = test_json_array_get(test_json_get(body, "system"), 0);
    EXPECT_STR_EQ(test_json_string(test_json_get(system_block, "text")), "be brief");
    test_json *system_cache = test_json_get(system_block, "cache_control");
    EXPECT_STR_EQ(test_json_string(test_json_get(system_cache, "ttl")), "1h");
    test_json *tool = test_json_array_get(test_json_get(body, "tools"), 0);
    EXPECT(test_json_is_object(test_json_get(tool, "input_schema")));
    EXPECT(test_json_is_object(test_json_get(tool, "cache_control")));
    test_json *messages = test_json_get(body, "messages");
    test_json *last = test_json_array_get(messages, test_json_size(messages) - 1);
    test_json *content = test_json_get(last, "content");
    test_json *block = test_json_array_get(content, test_json_size(content) - 1);
    EXPECT(test_json_is_object(test_json_get(block, "cache_control")));

    test_json_release(body);
}

static void test_build_body_budget_clamped(void)
{
    struct wire_body_opts opts = {
        .max_tokens = 100,
        .thinking_mode = ANTHROPIC_THINKING_BUDGET,
    };

    /* An unset budget claims everything below the output cap. */
    test_json *body =
        test_json_owned(anthropic_build_body(&BODY_CONTEXT, "prov", "model-1", &opts));
    test_json *thinking = test_json_get(body, "thinking");
    EXPECT(test_json_integer(test_json_get(thinking, "budget_tokens")) == 99);
    test_json_release(body);

    /* A budget at or above max_tokens violates the API bound and clamps the same way. */
    opts.thinking_budget = 100;
    body = test_json_owned(anthropic_build_body(&BODY_CONTEXT, "prov", "model-1", &opts));
    thinking = test_json_get(body, "thinking");
    EXPECT(test_json_integer(test_json_get(thinking, "budget_tokens")) == 99);
    test_json_release(body);

    /* No room for budget_tokens >= 1 below max_tokens: thinking is omitted entirely. */
    opts.max_tokens = 1;
    body = test_json_owned(anthropic_build_body(&BODY_CONTEXT, "prov", "model-1", &opts));
    EXPECT(test_json_get(body, "thinking") == NULL);
    test_json_release(body);
}

static void test_build_body_adaptive_thinking(void)
{
    struct wire_body_opts opts = {
        .max_tokens = 1000,
        .thinking_mode = ANTHROPIC_THINKING_ADAPTIVE,
        .show_reasoning = 1,
    };
    test_json *body =
        test_json_owned(anthropic_build_body(&BODY_CONTEXT, "prov", "model-1", &opts));

    test_json *thinking = test_json_get(body, "thinking");
    EXPECT_STR_EQ(test_json_string(test_json_get(thinking, "type")), "adaptive");
    EXPECT_STR_EQ(test_json_string(test_json_get(thinking, "display")), "summarized");
    test_json *output_config = test_json_get(body, "output_config");
    EXPECT_STR_EQ(test_json_string(test_json_get(output_config, "effort")), "high");
    /* No cache markers requested: the system block stays unannotated. */
    test_json *system_block = test_json_array_get(test_json_get(body, "system"), 0);
    EXPECT(test_json_get(system_block, "cache_control") == NULL);
    test_json_release(body);

    /* Hidden reasoning omits the display, and no effort choice means no output_config. */
    struct context context = BODY_CONTEXT;
    context.effort = NULL;
    opts.show_reasoning = 0;
    body = test_json_owned(anthropic_build_body(&context, "prov", "model-1", &opts));
    thinking = test_json_get(body, "thinking");
    EXPECT_STR_EQ(test_json_string(test_json_get(thinking, "display")), "omitted");
    EXPECT(test_json_get(body, "output_config") == NULL);
    test_json_release(body);
}

static void test_build_body_thinking_off(void)
{
    struct wire_body_opts opts = {
        .max_tokens = 1000,
        .thinking_mode = ANTHROPIC_THINKING_OFF,
    };
    test_json *body =
        test_json_owned(anthropic_build_body(&BODY_CONTEXT, "prov", "model-1", &opts));
    EXPECT(test_json_get(body, "thinking") == NULL);
    EXPECT(test_json_get(body, "output_config") == NULL);
    test_json_release(body);
}

int main(void)
{
    test_user_message();
    test_assistant_group_thinking_text_tool();
    test_empty_signature_policy();
    test_reasoning_provenance_mismatch_dropped();
    test_tool_results_coalesced();
    test_redacted_thinking_replayed();
    test_tool_call_bad_args_empty_object();
    test_tool_result_image();
    test_build_body_budget_thinking();
    test_build_body_budget_clamped();
    test_build_body_adaptive_thinking();
    test_build_body_thinking_off();
    T_REPORT();
}
