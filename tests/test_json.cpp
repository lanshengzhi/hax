/* SPDX-License-Identifier: MIT */
#include <cstdint>
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

static void test_dynamic_value_preserves_integers_and_order(void)
{
    const std::string input =
        R"({"first":9223372036854775807,"middle":{"large":4294967296},"last":[-9007199254740993]})";
    const auto parsed = hax::json::parse_value(input);
    EXPECT(parsed.has_value());
    if (!parsed)
        return;

    const hax::json::value *first = parsed->find("first");
    const hax::json::value *middle = parsed->find("middle");
    const hax::json::value *large = middle ? middle->find("large") : nullptr;
    const hax::json::value *last = parsed->find("last");
    const hax::json::value *negative = last && last->is_array() && !last->array_items().empty()
                                           ? &last->array_items()[0]
                                           : nullptr;
    EXPECT(first && first->is_integer() && first->integer_value() == INT64_MAX);
    EXPECT(large && large->is_integer() && large->integer_value() == 4294967296);
    EXPECT(negative && negative->is_integer() && negative->integer_value() == -9007199254740993LL);

    const auto encoded = hax::json::serialize_value(*parsed);
    EXPECT(encoded.has_value());
    if (encoded)
        EXPECT(*encoded == input);

    const auto real = hax::json::parse_value("1e3");
    EXPECT(real.has_value() && real->is_real());
    if (real) {
        const auto real_encoded = hax::json::serialize_value(*real);
        EXPECT(real_encoded.has_value());
        if (real_encoded)
            EXPECT(*real_encoded == "1000.0");
    }

    hax::json::value duplicate = hax::json::object{{"key", 1}, {"key", 2}};
    const hax::json::value *duplicate_last = duplicate.find("key");
    EXPECT(duplicate_last && duplicate_last->is_integer() && duplicate_last->integer_value() == 2);

    std::string control_text = "a";
    control_text.push_back(static_cast<char>(1));
    control_text += "b";
    hax::json::value controls = hax::json::object{{"text", hax::json::value(control_text)}};
    const auto escaped = hax::json::serialize_value(controls);
    EXPECT(escaped.has_value());
    if (escaped)
        EXPECT(escaped->find("\\u0001") != std::string::npos);
}

static void test_dynamic_integer_overflow_is_rejected(void)
{
    EXPECT(!hax::json::parse_value("9223372036854775808"));
    EXPECT(!hax::json::parse_value("-9223372036854775809"));
    EXPECT(hax::json::parse_value("9223372036854775808.0"));
    EXPECT(hax::json::parse_value(R"("9223372036854775808")"));
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

static void test_unknown_keys_are_explicitly_permissive(void)
{
    const std::string input = R"({"name":"Ada","count":3,"future":true})";
    const auto strict = hax::json::parse<json_record>(input);
    EXPECT(!strict.has_value());

    const auto relaxed = hax::json::parse<json_record>(input, {.allow_unknown_keys = true});
    EXPECT(relaxed.has_value());
    if (relaxed) {
        EXPECT(relaxed->name == "Ada");
        EXPECT(relaxed->count == 3);
    }
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

int main(void) // NOLINT(bugprone-exception-escape)
{
    test_valid_parse_and_serialize();
    test_dynamic_value_preserves_integers_and_order();
    test_dynamic_integer_overflow_is_rejected();
    test_syntax_error_has_source_context();
    test_type_error_is_project_error();
    test_unknown_keys_are_explicitly_permissive();
    test_invalid_utf8_is_rejected();
    test_trailing_data_is_rejected();
    test_input_bound_is_checked_before_parse();
    test_non_null_terminated_view_is_bounded();
    test_non_null_terminated_scalar_view_is_bounded();
    T_REPORT();
}
