/* SPDX-License-Identifier: MIT */
#include <string.h>

#include "harness.h"
#include "json_helpers.h"
#include "provider.h"
#include "providers/responses_body.h"
#include "providers/wire.h"

static const char *item_type(test_json *item)
{
    return test_json_string(test_json_get(item, "type"));
}

static void test_input_item_shapes(void)
{
    struct item items[] = {
        {.kind = ITEM_USER_MESSAGE, .text = "hi"},
        {.kind = ITEM_ASSISTANT_MESSAGE, .text = "yo"},
        {.kind = ITEM_TOOL_CALL,
         .call_id = "c1",
         .tool_name = "bash",
         .tool_arguments_json = "{\"command\":\"ls\"}"},
        {.kind = ITEM_TURN_BOUNDARY},
        {.kind = ITEM_TOOL_RESULT, .call_id = "c1", .output = "out"},
    };
    test_json *input = test_json_owned(responses_build_input_items(items, 5, "codex", "o3", -1));
    EXPECT(test_json_size(input) == 4);

    test_json *user_message = test_json_array_get(input, 0);
    EXPECT_STR_EQ(item_type(user_message), "message");
    EXPECT_STR_EQ(test_json_string(test_json_get(user_message, "role")), "user");
    test_json *user_content = test_json_array_get(test_json_get(user_message, "content"), 0);
    EXPECT_STR_EQ(item_type(user_content), "input_text");
    EXPECT_STR_EQ(test_json_string(test_json_get(user_content, "text")), "hi");

    test_json *assistant_message = test_json_array_get(input, 1);
    EXPECT_STR_EQ(test_json_string(test_json_get(assistant_message, "role")), "assistant");
    test_json *assistant_content =
        test_json_array_get(test_json_get(assistant_message, "content"), 0);
    EXPECT_STR_EQ(item_type(assistant_content), "output_text");

    test_json *tool_call = test_json_array_get(input, 2);
    EXPECT_STR_EQ(item_type(tool_call), "function_call");
    EXPECT_STR_EQ(test_json_string(test_json_get(tool_call, "call_id")), "c1");

    test_json *tool_result = test_json_array_get(input, 3);
    EXPECT_STR_EQ(item_type(tool_result), "function_call_output");
    EXPECT(test_json_is_string(test_json_get(tool_result, "output")));
    EXPECT_STR_EQ(test_json_string(test_json_get(tool_result, "output")), "out");
    test_json_release(input);
}

static void test_tool_result_image(void)
{
    struct item_image images[] = {
        {.mime = "image/png", .data_b64 = "QUJD", .width = 4, .height = 2},
    };
    struct item items[] = {
        {.kind = ITEM_TOOL_RESULT,
         .call_id = "c9",
         .output = "note",
         .images = images,
         .n_images = 1},
    };

    test_json *input = test_json_owned(responses_build_input_items(items, 1, "codex", "o3", 1));
    test_json *output = test_json_get(test_json_array_get(input, 0), "output");
    EXPECT(test_json_is_array(output));
    EXPECT_STR_EQ(item_type(test_json_array_get(output, 0)), "input_text");
    test_json *image = test_json_array_get(output, 1);
    EXPECT_STR_EQ(item_type(image), "input_image");
    EXPECT_STR_EQ(test_json_string(test_json_get(image, "image_url")),
                  "data:image/png;base64,QUJD");
    test_json_release(input);

    input = test_json_owned(responses_build_input_items(items, 1, "codex", "o3", 0));
    output = test_json_get(test_json_array_get(input, 0), "output");
    test_json *placeholder = test_json_array_get(output, 1);
    EXPECT_STR_EQ(item_type(placeholder), "input_text");
    EXPECT(strstr(test_json_string(test_json_get(placeholder, "text")), "[image:") != NULL);
    test_json_release(input);
}

static void test_reasoning_provenance(void)
{
    struct item items[] = {
        {.kind = ITEM_REASONING,
         .reasoning_json = "{\"type\":\"reasoning\",\"summary\":[{\"future\":{\"keep\":true}}],"
                           "\"encrypted_content\":\"abc==\"}",
         .provider = "codex",
         .model = "o3"},
        {.kind = ITEM_ASSISTANT_MESSAGE, .text = "done"},
    };

    test_json *input = test_json_owned(responses_build_input_items(items, 2, "codex", "o3", -1));
    EXPECT(test_json_size(input) == 2);
    EXPECT_STR_EQ(item_type(test_json_array_get(input, 0)), "reasoning");
    test_json *summary =
        test_json_array_get(test_json_get(test_json_array_get(input, 0), "summary"), 0);
    EXPECT(test_json_is_object(test_json_get(summary, "future")));
    test_json_release(input);

    input = test_json_owned(responses_build_input_items(items, 2, "codex", "o4", -1));
    EXPECT(test_json_size(input) == 1);
    EXPECT_STR_EQ(test_json_string(test_json_get(test_json_array_get(input, 0), "role")),
                  "assistant");
    test_json_release(input);

    input = test_json_owned(responses_build_input_items(items, 2, "openai", "o3", -1));
    EXPECT(test_json_size(input) == 1);
    EXPECT_STR_EQ(test_json_string(test_json_get(test_json_array_get(input, 0), "role")),
                  "assistant");
    test_json_release(input);

    struct item scalar_reasoning = {
        .kind = ITEM_REASONING,
        .reasoning_json = "null",
        .provider = "codex",
        .model = "o3",
    };
    input = test_json_owned(responses_build_input_items(&scalar_reasoning, 1, "codex", "o3", -1));
    EXPECT(test_json_size(input) == 0);
    test_json_release(input);

    scalar_reasoning.reasoning_json = "[]";
    input = test_json_owned(responses_build_input_items(&scalar_reasoning, 1, "codex", "o3", -1));
    EXPECT(test_json_size(input) == 1);
    EXPECT(test_json_is_array(test_json_array_get(input, 0)));
    test_json_release(input);
}

static void test_body_shape(void)
{
    struct item items[] = {{.kind = ITEM_USER_MESSAGE, .text = "hi"}};
    struct tool_def tools[] = {{.name = "bash", .description = "run a command"}};
    struct context context = {
        .system_prompt = "be brief",
        .items = items,
        .n_items = 1,
        .tools = tools,
        .n_tools = 1,
        .effort = "medium",
        .image_input = -1,
    };

    test_json *body = test_json_owned(responses_build_body(&context, "openai", "gpt-5", NULL));
    EXPECT_STR_EQ(test_json_string(test_json_get(body, "model")), "gpt-5");
    EXPECT(test_json_is_true(test_json_get(body, "stream")));
    EXPECT(test_json_is_false(test_json_get(body, "store")));
    EXPECT_STR_EQ(test_json_string(test_json_get(body, "instructions")), "be brief");
    EXPECT(test_json_size(test_json_get(body, "input")) == 1);

    /* Responses declares function schemas flat, unlike the Chat Completions nesting. */
    test_json *tool = test_json_array_get(test_json_get(body, "tools"), 0);
    EXPECT_STR_EQ(item_type(tool), "function");
    EXPECT_STR_EQ(test_json_string(test_json_get(tool, "name")), "bash");
    EXPECT(test_json_get(tool, "function") == NULL);
    EXPECT(test_json_is_true(test_json_get(body, "parallel_tool_calls")));

    test_json *reasoning = test_json_get(body, "reasoning");
    EXPECT_STR_EQ(test_json_string(test_json_get(reasoning, "effort")), "medium");
    EXPECT_STR_EQ(test_json_string(test_json_get(reasoning, "summary")), "auto");
    EXPECT(test_json_get(body, "reasoning_effort") == NULL);
    EXPECT_STR_EQ(test_json_string(test_json_array_get(test_json_get(body, "include"), 0)),
                  "reasoning.encrypted_content");
    test_json_release(body);
}

static void test_body_reasoning_variants(void)
{
    struct context context = {.system_prompt = "sys", .image_input = -1};

    /* An unset effort picks no level but still reasons, so its encrypted output must be
     * requested — otherwise a store:false turn has nothing to replay across its tool calls. */
    test_json *body = test_json_owned(responses_build_body(&context, "openai", "gpt-5", NULL));
    EXPECT(test_json_get(body, "reasoning") == NULL);
    EXPECT_STR_EQ(test_json_string(test_json_array_get(test_json_get(body, "include"), 0)),
                  "reasoning.encrypted_content");
    EXPECT(test_json_get(body, "tools") == NULL);
    test_json_release(body);

    /* An empty effort is the same absence of a choice, not a request to disable reasoning. */
    context.effort = "";
    body = test_json_owned(responses_build_body(&context, "openai", "gpt-5", NULL));
    EXPECT(test_json_get(body, "reasoning") == NULL);
    EXPECT(test_json_size(test_json_get(body, "include")) == 1);
    test_json_release(body);

    /* Only an explicit "none" rules reasoning out, leaving nothing to replay. */
    context.effort = "none";
    body = test_json_owned(responses_build_body(&context, "openai", "gpt-5", NULL));
    test_json *reasoning = test_json_get(body, "reasoning");
    EXPECT_STR_EQ(test_json_string(test_json_get(reasoning, "effort")), "none");
    EXPECT(test_json_get(reasoning, "summary") == NULL);
    EXPECT(test_json_get(body, "include") == NULL);
    test_json_release(body);
}

static void test_control_characters_remain_json_safe(void)
{
    char text[] = {'a', '\x01', 'b', '\0'};
    struct item items[] = {{.kind = ITEM_USER_MESSAGE, .text = text}};
    test_json *input =
        test_json_owned(responses_build_input_items(items, 1, "openai", "gpt-5", -1));

    EXPECT(test_json_size(input) == 1);
    /* Responses content is always a typed array, so inspect its first text part. */
    const char *decoded = test_json_string(test_json_get(
        test_json_array_get(test_json_get(test_json_array_get(input, 0), "content"), 0), "text"));
    EXPECT(decoded != NULL);
    if (decoded)
        EXPECT(decoded[0] == 'a' && decoded[1] == '\x01' && decoded[2] == 'b' &&
               decoded[3] == '\0');
    test_json_release(input);
}

static void test_body_session_cache_key(void)
{
    struct context context = {.system_prompt = "sys", .image_input = -1};

    struct wire_body_opts opts = {.session_cache_key = "sess-2"};
    test_json *body = test_json_owned(responses_build_body(&context, "openai", "gpt-5", &opts));
    EXPECT_STR_EQ(test_json_string(test_json_get(body, "prompt_cache_key")), "sess-2");
    test_json_release(body);

    /* NULL opts serve callers that layer their own routing fields. */
    body = test_json_owned(responses_build_body(&context, "openai", "gpt-5", NULL));
    EXPECT(test_json_get(body, "prompt_cache_key") == NULL);
    test_json_release(body);
}

int main(void)
{
    test_input_item_shapes();
    test_tool_result_image();
    test_reasoning_provenance();
    test_body_shape();
    test_body_reasoning_variants();
    test_control_characters_remain_json_safe();
    test_body_session_cache_key();
    T_REPORT();
}
