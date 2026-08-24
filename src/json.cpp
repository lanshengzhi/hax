/* SPDX-License-Identifier: MIT */
#include "json.h"

#include <string>
#include <string_view>

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
