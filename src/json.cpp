/* SPDX-License-Identifier: MIT */
#include "json.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hax::json
{

std::string_view error_code_name(error_code code) noexcept
{
    switch (code) {
    case error_code::none:
        return "none";
    case error_code::syntax:
        return "syntax error";
    case error_code::type:
        return "type error";
    case error_code::schema:
        return "schema error";
    case error_code::invalid_utf8:
        return "invalid UTF-8";
    case error_code::trailing_data:
        return "trailing data";
    case error_code::input_too_large:
        return "input too large";
    case error_code::serialization:
        return "serialization error";
    }
    return "unknown JSON error";
}

std::string format_error(const error &value)
{
    std::string result;
    if (!value.source.empty()) {
        result += value.source;
        result += ": ";
    }
    result += error_code_name(value.code);
    result += " at byte ";
    result += std::to_string(value.offset);
    if (!value.message.empty()) {
        result += ": ";
        result += value.message;
    }
    return result;
}

/* Glaze's signed generic value falls back to double for integer literals outside int64_t. Reject
 * those literals before conversion so provider/config values cannot silently lose integer bits. */
static std::optional<size_t> integer_overflow_offset(std::string_view input)
{
    bool in_string = false;
    for (size_t i = 0; i < input.size();) {
        const char c = input[i];
        if (in_string) {
            if (c == '\\' && i + 1 < input.size())
                i += 2;
            else {
                in_string = c != '"';
                i++;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
            i++;
            continue;
        }
        if (c != '-' && (c < '0' || c > '9')) {
            i++;
            continue;
        }

        const size_t begin = i;
        while (i < input.size() && input[i] != ' ' && input[i] != '\t' && input[i] != '\n' &&
               input[i] != '\r' && input[i] != ',' && input[i] != ']' && input[i] != '}')
            i++;
        const std::string_view token = input.substr(begin, i - begin);
        const size_t first_digit = token.front() == '-' ? 1 : 0;
        bool integer_literal = first_digit < token.size();
        for (size_t digit = first_digit; integer_literal && digit < token.size(); digit++)
            integer_literal = token[digit] >= '0' && token[digit] <= '9';
        if (!integer_literal)
            continue;

        std::int64_t value;
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
        if (parsed.ec == std::errc::result_out_of_range)
            return begin;
    }
    return std::nullopt;
}

/* Glaze's generic_i64 recognizes an exponent-less integer prefix before its exponent. Add a
 * decimal point to exponent forms so the adapter retains their real-number kind and value. */
static std::string normalize_exponent_numbers(std::string_view input)
{
    std::string result;
    result.reserve(input.size());
    bool in_string = false;
    for (size_t i = 0; i < input.size();) {
        const char c = input[i];
        if (in_string) {
            result.push_back(c);
            if (c == '\\' && i + 1 < input.size()) {
                result.push_back(input[i + 1]);
                i += 2;
            } else {
                in_string = c != '"';
                i++;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
            result.push_back(c);
            i++;
            continue;
        }
        if (c != '-' && (c < '0' || c > '9')) {
            result.push_back(c);
            i++;
            continue;
        }

        const size_t begin = i;
        while (i < input.size() && input[i] != ' ' && input[i] != '\t' && input[i] != '\n' &&
               input[i] != '\r' && input[i] != ',' && input[i] != ']' && input[i] != '}')
            i++;
        const std::string_view token = input.substr(begin, i - begin);
        const size_t exponent = token.find_first_of("eE");
        if (exponent != std::string::npos && token.find('.') == std::string::npos) {
            result.append(token.substr(0, exponent));
            result += ".0";
            result.append(token.substr(exponent));
        } else {
            result.append(token);
        }
    }
    return result;
}

static value from_generic(const glz::generic_i64 &source)
{
    if (source.is_null())
        return nullptr;
    if (source.is_boolean())
        return source.get_boolean();
    if (source.is_int64())
        return static_cast<value::integer>(source.get<std::int64_t>());
    if (source.is_double())
        return source.get_number();
    if (source.is_string())
        return source.get_string();
    if (source.is_array()) {
        array result;
        result.reserve(source.get_array().size());
        for (const glz::generic_i64 &item : source.get_array())
            result.emplace_back(from_generic(item));
        return result;
    }

    object result;
    result.reserve(source.get_object().size());
    for (const auto &member : source.get_object())
        result.emplace_back(member.first, from_generic(member.second));
    return result;
}

struct escaped_write_options : glz::opts {
    bool escape_control_characters = true;
};

template <typename T>
static std::expected<void, error> append_scalar(T source, std::string &output, options options)
{
    auto encoded = glz::write<escaped_write_options{}>(source);
    if (!encoded)
        return std::unexpected(detail::from_write_error(encoded.error(), options));
    output += *encoded;
    return {};
}

static std::expected<void, error> append_value(const value &source, std::string &output,
                                               options options)
{
    if (source.is_null())
        return append_scalar(nullptr, output, options);
    if (source.is_boolean())
        return append_scalar(source.boolean_value(), output, options);
    if (source.is_integer())
        return append_scalar(source.integer_value(), output, options);
    if (source.is_real()) {
        auto encoded = glz::write<escaped_write_options{}>(source.real_value());
        if (!encoded)
            return std::unexpected(detail::from_write_error(encoded.error(), options));
        if (std::isfinite(source.real_value()) &&
            encoded->find_first_of(".eE") == std::string::npos)
            encoded->append(".0");
        output += *encoded;
        return {};
    }
    if (source.is_string())
        return append_scalar(source.string_value(), output, options);
    if (source.is_array()) {
        output.push_back('[');
        for (size_t i = 0; i < source.array_items().size(); i++) {
            if (i)
                output.push_back(',');
            auto result = append_value(source.array_items()[i], output, options);
            if (!result)
                return result;
        }
        output.push_back(']');
        return {};
    }

    output.push_back('{');
    for (size_t i = 0; i < source.object_items().size(); i++) {
        if (i)
            output.push_back(',');
        const auto &member = source.object_items()[i];
        auto key = append_scalar(member.first, output, options);
        if (!key)
            return key;
        output.push_back(':');
        auto result = append_value(member.second, output, options);
        if (!result)
            return result;
    }
    output.push_back('}');
    return {};
}

std::expected<value, error> parse_value(std::string_view input, options options)
{
    auto input_check = detail::check_input_size(input, options);
    if (!input_check)
        return std::unexpected(input_check.error());

    if (auto offset = integer_overflow_offset(input)) {
        error value;
        value.code = error_code::type;
        value.offset = *offset;
        value.source = detail::source_name(options);
        value.message = "integer literal is outside the signed 64-bit range";
        return std::unexpected(std::move(value));
    }

    const std::string normalized = normalize_exponent_numbers(input);
    glz::generic_i64 decoded;
    const glz::error_ctx glaze_error = glz::read<detail::read_options{}>(decoded, normalized);
    if (glaze_error)
        return std::unexpected(detail::from_read_error(glaze_error, input, options));
    return from_generic(decoded);
}

std::expected<std::string, error> serialize_value(const value &source, options options)
{
    std::string encoded;
    auto result = append_value(source, encoded, options);
    if (!result)
        return std::unexpected(result.error());
    return encoded;
}

std::expected<std::string, error> serialize_value_pretty(const value &source, options options)
{
    auto encoded = serialize_value(source, options);
    if (!encoded)
        return std::unexpected(encoded.error());

    struct pretty_options : glz::opts {
        uint8_t indentation_width = 2;
    };
    std::string pretty;
    glz::prettify_json<pretty_options{}>(*encoded, pretty);
    return pretty;
}

std::expected<void, error> validate(std::string_view input, options options)
{
    auto input_check = detail::check_input_size(input, options);
    if (!input_check)
        return std::unexpected(input_check.error());

    glz::skip value;
    const glz::error_ctx glaze_error = glz::read<detail::read_options{}>(value, input);
    if (glaze_error)
        return std::unexpected(detail::from_read_error(glaze_error, input, options));
    return {};
}

} // namespace hax::json
