/* SPDX-License-Identifier: MIT */
#include "harness.h"
#include "session_control.h"

static void expect_optional_string(const char *actual, const char *expected)
{
    EXPECT((actual == NULL) == (expected == NULL));
    if (actual && expected)
        EXPECT_STR_EQ(actual, expected);
}

static void test_header_fixture(void)
{
    const char *fixture =
        R"({"type":"session","version":1,"hax_version":"v0.4.0","id":"session-id","timestamp":"2026-08-25T00:00:00Z","cwd":"/work/project","provider":"alpha","model":"model-id","model_label":"Model label","effort":"high","preset":"review","git_branch":"modern-cpp","git_commit":"abc123","git_subject":"Migrate records","forked_from":"parent-id","future_control":{"enabled":true}})";
    struct session_control control = {};

    EXPECT(session_control_decode(fixture, &control) == SESSION_CONTROL_DECODED);
    EXPECT(control.kind == SESSION_CONTROL_HEADER);
    EXPECT(control.has_version);
    EXPECT(control.version == 1);
    expect_optional_string(control.hax_version, "v0.4.0");
    expect_optional_string(control.id, "session-id");
    expect_optional_string(control.timestamp, "2026-08-25T00:00:00Z");
    expect_optional_string(control.cwd, "/work/project");
    expect_optional_string(control.provider, "alpha");
    expect_optional_string(control.model, "model-id");
    expect_optional_string(control.model_label, "Model label");
    expect_optional_string(control.effort, "high");
    expect_optional_string(control.preset, "review");
    expect_optional_string(control.git_branch, "modern-cpp");
    expect_optional_string(control.git_commit, "abc123");
    expect_optional_string(control.git_subject, "Migrate records");
    expect_optional_string(control.forked_from, "parent-id");
    session_control_free(&control);
}

static void test_old_header_fixture(void)
{
    const char *fixture =
        R"({"type":"session","version":1,"id":"legacy-id","cwd":"/legacy","provider":"alpha","model":"model-id"})";
    struct session_control control = {};

    EXPECT(session_control_decode(fixture, &control) == SESSION_CONTROL_DECODED);
    EXPECT(control.kind == SESSION_CONTROL_HEADER);
    EXPECT(control.has_version);
    EXPECT(control.version == 1);
    expect_optional_string(control.id, "legacy-id");
    expect_optional_string(control.cwd, "/legacy");
    expect_optional_string(control.provider, "alpha");
    expect_optional_string(control.model, "model-id");
    expect_optional_string(control.hax_version, NULL);
    expect_optional_string(control.model_label, NULL);
    expect_optional_string(control.git_branch, NULL);
    expect_optional_string(control.git_commit, NULL);
    expect_optional_string(control.git_subject, NULL);
    session_control_free(&control);
}

static void test_escaped_type_key_fixture(void)
{
    const char *fixture =
        R"({"\u0074ype":"session","version":1,"id":"escaped-id","cwd":"/escaped","provider":"alpha","model":"model-id"})";
    struct session_control control = {};

    EXPECT(session_control_decode(fixture, &control) == SESSION_CONTROL_DECODED);
    EXPECT(control.kind == SESSION_CONTROL_HEADER);
    expect_optional_string(control.id, "escaped-id");
    expect_optional_string(control.cwd, "/escaped");
    session_control_free(&control);
}

static void test_selection_fixture_and_absence(void)
{
    const char *fixture =
        R"({"type":"selection","provider":"beta","model":"next-model","model_label":"Next label","future_control":"ignored"})";
    struct session_control control = {};

    EXPECT(session_control_decode(fixture, &control) == SESSION_CONTROL_DECODED);
    EXPECT(control.kind == SESSION_CONTROL_SELECTION);
    EXPECT(!control.has_version);
    expect_optional_string(control.provider, "beta");
    expect_optional_string(control.model, "next-model");
    expect_optional_string(control.model_label, "Next label");
    expect_optional_string(control.effort, NULL);
    expect_optional_string(control.preset, NULL);
    expect_optional_string(control.git_branch, NULL);
    expect_optional_string(control.git_commit, NULL);
    expect_optional_string(control.git_subject, NULL);
    session_control_free(&control);
}

static void test_non_control_and_invalid_records(void)
{
    struct session_control control = {};
    const char *item = R"({"kind":"user","text":"a type field is not control metadata"})";
    EXPECT(session_control_decode(item, &control) == SESSION_CONTROL_NOT_FOUND);
    EXPECT(control.kind == SESSION_CONTROL_NONE);

    const char *invalid = R"({"type":"selection","provider":7})";
    EXPECT(session_control_decode(invalid, &control) == SESSION_CONTROL_INVALID);
    EXPECT(control.kind == SESSION_CONTROL_NONE);
}

int main(void)
{
    test_header_fixture();
    test_old_header_fixture();
    test_escaped_type_key_fixture();
    test_selection_fixture_and_absence();
    test_non_control_and_invalid_records();
    T_REPORT();
}
