/* SPDX-License-Identifier: MIT */
#include <string>
#include <string_view>

#include "harness.h"
#include "json.h"

struct json_record {
    std::string name;
    int count = 0;
};

template <> struct glz::meta<json_record> {
    using T = json_record;
    static constexpr auto value = glz::object("name", &T::name, "count", &T::count);
};

static void expect_code(const hax::json::error &error, hax::json::error_code code)
{
    EXPECT(error.code == code);
    EXPECT(!error.source.empty());
    EXPECT(!error.message.empty());
}

static void test_valid_parse_and_serialize(void)
{
    const auto parsed = hax::json::parse<json_record>(R"({"name":"Ada","count":3})");
    EXPECT(parsed.has_value());
    if (!parsed)
        return;

    EXPECT(parsed->name == "Ada");
    EXPECT(parsed->count == 3);

    const auto encoded = hax::json::serialize(*parsed);
    EXPECT(encoded.has_value());
    if (encoded)
        EXPECT(*encoded == R"({"name":"Ada","count":3})");
}

static void test_syntax_error_has_source_context(void)
{
    const auto result = hax::json::validate(R"({"name":,"count":1})", {.source = "wire.json"});
    EXPECT(!result.has_value());
    if (result)
        return;

    expect_code(result.error(), hax::json::error_code::syntax);
    EXPECT(result.error().source == "wire.json");
    EXPECT(result.error().message.find('^') != std::string::npos);
    EXPECT(hax::json::format_error(result.error()).find("wire.json") != std::string::npos);
}

static void test_type_error_is_project_error(void)
{
    const auto result =
        hax::json::parse<json_record>(R"({"name":"Ada","count":"three"})", {.source = "item.json"});
    EXPECT(!result.has_value());
    if (result)
        return;

    expect_code(result.error(), hax::json::error_code::type);
    EXPECT(hax::json::format_error(result.error()).find("type error") != std::string::npos);
}

static void test_invalid_utf8_is_rejected(void)
{
    std::string input = R"({"name":"Ada)";
    input.push_back(static_cast<char>(0xff));
    input += R"(","count":3})";

    const auto result = hax::json::validate(input, {.source = "input.json"});
    EXPECT(!result.has_value());
    if (result)
        return;

    expect_code(result.error(), hax::json::error_code::invalid_utf8);
    EXPECT(result.error().offset < input.size());
}

static void test_trailing_data_is_rejected(void)
{
    const std::string input = "{\"name\":\"Ada\",\"count\":3}\n{}";
    const auto result = hax::json::parse<json_record>(input, {.source = "session.json"});
    EXPECT(!result.has_value());
    if (result)
        return;

    expect_code(result.error(), hax::json::error_code::trailing_data);
    EXPECT(result.error().offset == input.find('{', 1));
    EXPECT(hax::json::format_error(result.error()).find("trailing data") != std::string::npos);
}

static void test_input_bound_is_checked_before_parse(void)
{
    const std::string input = R"({"name":"Ada","count":3})";
    const auto result = hax::json::parse<json_record>(
        input, {.source = "bounded.json", .max_input_bytes = input.size() - 1});
    EXPECT(!result.has_value());
    if (result)
        return;

    expect_code(result.error(), hax::json::error_code::input_too_large);
    EXPECT(result.error().offset == input.size() - 1);
    EXPECT(result.error().message.find("maximum") != std::string::npos);
}

static void test_non_null_terminated_view_is_bounded(void)
{
    const std::string document = R"({"name":"Ada","count":3})";
    const std::string backing = document + "x";
    const std::string_view bounded(backing.data(), document.size());

    const auto validation = hax::json::validate(bounded, {.source = "bounded-view.json"});
    EXPECT(validation.has_value());

    const auto parsed = hax::json::parse<json_record>(bounded, {.source = "bounded-view.json"});
    EXPECT(parsed.has_value());
}

static void test_non_null_terminated_scalar_view_is_bounded(void)
{
    const std::string backing = "12";
    const std::string_view bounded(backing.data(), 1);
    const auto parsed = hax::json::parse<int>(bounded, {.source = "scalar-view.json"});

    EXPECT(parsed.has_value());
    if (parsed)
        EXPECT(*parsed == 1);
}

int main(void)
{
    test_valid_parse_and_serialize();
    test_syntax_error_has_source_context();
    test_type_error_is_project_error();
    test_invalid_utf8_is_rejected();
    test_trailing_data_is_rejected();
    test_input_bound_is_checked_before_parse();
    test_non_null_terminated_view_is_bounded();
    test_non_null_terminated_scalar_view_is_bounded();
    T_REPORT();
}
