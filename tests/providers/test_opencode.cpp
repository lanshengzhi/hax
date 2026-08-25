/* SPDX-License-Identifier: MIT */
#include <string>

#include "harness.h"
#include "providers/opencode_json.h"

static void test_live_response_shape(void)
{
    auto usage = hax::opencode_json::parse_usage("{\"usage\":{"
                                                 "\"rolling\":{\"status\":\"ok\",\"percent\":5,"
                                                 "\"resetsAt\":\"2026-08-21T21:14:51.969Z\"},"
                                                 "\"weekly\":{\"status\":\"ok\",\"percent\":54,"
                                                 "\"resetsAt\":\"2026-08-24T00:00:00.969Z\"},"
                                                 "\"monthly\":{\"status\":\"ok\",\"percent\":27,"
                                                 "\"resetsAt\":\"2026-09-19T13:32:19.969Z\"}}}");

    EXPECT(usage.has_value());
    if (!usage)
        return;

    EXPECT(usage->windows.size() == 3);
    EXPECT_STR_EQ(usage->windows[0].label.c_str(), "rolling");
    EXPECT(usage->windows[0].used_percent == 5);
    EXPECT(usage->windows[0].reset_at == 1787346891);
    EXPECT(!usage->windows[0].note);
    EXPECT_STR_EQ(usage->windows[1].label.c_str(), "weekly");
    EXPECT(usage->windows[1].used_percent == 54);
    EXPECT(usage->windows[1].reset_at == 1787529600);
    EXPECT_STR_EQ(usage->windows[2].label.c_str(), "monthly");
    EXPECT(usage->windows[2].reset_at == 1789824739);
}

static void test_non_ok_status_becomes_note(void)
{
    auto usage = hax::opencode_json::parse_usage(
        "{\"usage\":{\"weekly\":{\"status\":\"limited\",\"percent\":100,"
        "\"resetsAt\":\"2026-08-24T00:00:00Z\"}}}");

    EXPECT(usage.has_value());
    if (usage && usage->windows.size() == 1) {
        EXPECT(usage->windows[0].note.has_value());
        if (usage->windows[0].note) {
            const std::string note = usage->windows[0].note.value_or("");
            EXPECT_STR_EQ(note.c_str(), "limited");
        }
    }
}

static void test_malformed_windows_skipped(void)
{
    auto usage = hax::opencode_json::parse_usage(
        "{\"usage\":{"
        "\"no_percent\":{\"resetsAt\":\"2026-08-24T00:00:00Z\"},"
        "\"no_reset\":{\"percent\":5},"
        "\"reset_not_utc\":{\"percent\":5,\"resetsAt\":\"2026-08-24T00:00:00+02:00\"},"
        "\"reset_garbage\":{\"percent\":5,\"resetsAt\":\"tomorrow\"},"
        "\"day_overflow\":{\"percent\":5,\"resetsAt\":\"2026-02-31T00:00:00Z\"},"
        "\"nonleap_feb29\":{\"percent\":5,\"resetsAt\":\"2100-02-29T00:00:00Z\"},"
        "\"empty_fraction\":{\"percent\":5,\"resetsAt\":\"2026-08-24T00:00:00.Z\"},"
        "\"not_object\":42,"
        "\"good\":{\"percent\":5,\"resetsAt\":\"2026-08-24T00:00:00Z\"}}}");

    EXPECT(usage.has_value());
    if (usage) {
        EXPECT(usage->windows.size() == 1);
        if (usage->windows.size() == 1)
            EXPECT_STR_EQ(usage->windows[0].label.c_str(), "good");
    }
}

static void test_unknown_members_and_roots(void)
{
    auto usage = hax::opencode_json::parse_usage(
        "{\"usage\":{\"weekly\":{\"percent\":5,"
        "\"resetsAt\":\"2026-08-24T00:00:00Z\",\"future\":{\"opaque\":true}}},"
        "\"future\":{\"new\":true}}");
    EXPECT(usage && usage->windows.size() == 1);

    usage = hax::opencode_json::parse_usage("{\"error\":\"nope\"}");
    EXPECT(usage && usage->windows.empty());
    usage = hax::opencode_json::parse_usage("{\"usage\":[]}");
    EXPECT(usage && usage->windows.empty());
    usage = hax::opencode_json::parse_usage("[]");
    EXPECT(!usage);
    usage = hax::opencode_json::parse_usage("null");
    EXPECT(!usage);
}

static void test_window_count_and_timestamp_conversion(void)
{
    auto usage = hax::opencode_json::parse_usage(
        "{\"usage\":{"
        "\"a\":{\"percent\":1,\"resetsAt\":\"2026-08-24T00:00:00Z\"},"
        "\"b\":{\"percent\":2,\"resetsAt\":\"2026-08-24T00:00:00Z\"},"
        "\"c\":{\"percent\":3,\"resetsAt\":\"2026-08-24T00:00:00Z\"}}}");
    EXPECT(usage && usage->windows.size() == 3);
    if (usage)
        EXPECT_STR_EQ(usage->windows[1].label.c_str(), "b");

    usage = hax::opencode_json::parse_usage(
        "{\"usage\":{"
        "\"epoch\":{\"percent\":0,\"resetsAt\":\"1970-01-01T00:00:00Z\"},"
        "\"leap\":{\"percent\":0,\"resetsAt\":\"2000-02-29T12:00:00Z\"}}}");
    EXPECT(usage && usage->windows.size() == 2);
    if (usage) {
        EXPECT(usage->windows[0].reset_at == 0);
        EXPECT(usage->windows[1].reset_at == 951825600);
    }
}

static void test_invalid_json_reports_error(void)
{
    std::string error;
    EXPECT(!hax::opencode_json::parse_usage("not json", &error));
    EXPECT(!error.empty());
}

int main(void)
{
    test_live_response_shape();
    test_non_ok_status_becomes_note();
    test_malformed_windows_skipped();
    test_unknown_members_and_roots();
    test_window_count_and_timestamp_conversion();
    test_invalid_json_reports_error();
    T_REPORT();
}
