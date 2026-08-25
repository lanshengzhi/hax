/* SPDX-License-Identifier: MIT */
#ifndef HAX_JSON_H
#define HAX_JSON_H

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include "json_value.h"

/* This is the only project header that includes Glaze. Domain and provider interfaces use the
 * project-owned types below and do not depend on Glaze's representation. */
#include <glaze/json.hpp>

namespace hax::json
{

/* Validate one complete JSON document. The input is borrowed for the duration of the call; failures
 * own their source label and diagnostic. */
std::expected<void, error> validate(std::string_view input, options options = {});

namespace detail
{

inline std::string source_name(const options &options)
{
    return options.source.empty() ? "<json>" : std::string(options.source);
}

inline std::expected<void, error> check_input_size(std::string_view input, const options &options)
{
    if (options.max_input_bytes == 0 || input.size() <= options.max_input_bytes)
        return {};

    error value;
    value.code = error_code::input_too_large;
    value.offset = options.max_input_bytes;
    value.source = source_name(options);
    value.message = "input is " + std::to_string(input.size()) + " bytes; maximum is " +
                    std::to_string(options.max_input_bytes);
    return std::unexpected(std::move(value));
}

struct read_options : glz::opts {
    bool null_terminated = false;
    bool validate_skipped = true;
    bool validate_trailing_whitespace = true;
};

struct relaxed_read_options : read_options {
    bool error_on_unknown_keys = false;
};

inline bool valid_prefix(std::string_view input, size_t end)
{
    if (end == 0 || end > input.size())
        return false;

    glz::skip value;
    return !glz::read<read_options{}>(value, input.substr(0, end));
}

inline error_code map_read_error(glz::error_code code) noexcept
{
    switch (code) {
    case glz::error_code::invalid_utf8:
        return error_code::invalid_utf8;
    case glz::error_code::parse_number_failure:
    case glz::error_code::unexpected_enum:
    case glz::error_code::invalid_nullable_read:
    case glz::error_code::invalid_variant_object:
    case glz::error_code::invalid_variant_array:
    case glz::error_code::invalid_variant_string:
    case glz::error_code::no_matching_variant_type:
    case glz::error_code::get_wrong_type:
    case glz::error_code::elements_not_convertible_to_design:
        return error_code::type;
    case glz::error_code::key_not_found:
    case glz::error_code::unknown_key:
    case glz::error_code::missing_key:
        return error_code::schema;
    default:
        return error_code::syntax;
    }
}

inline error from_read_error(const glz::error_ctx &glaze_error, std::string_view input,
                             const options &options)
{
    error value;
    value.code = map_read_error(glaze_error.ec);
    if (glaze_error.ec == glz::error_code::syntax_error && valid_prefix(input, glaze_error.count))
        value.code = error_code::trailing_data;
    value.offset = glaze_error.count;
    value.source = source_name(options);
    value.message = glz::format_error(glaze_error, input);
    return value;
}

inline error from_write_error(const glz::error_ctx &glaze_error, const options &options)
{
    error value;
    value.code = error_code::serialization;
    value.offset = glaze_error.count;
    value.source = source_name(options);
    value.message = glz::format_error(glaze_error);
    return value;
}

} // namespace detail

/* Parse one complete JSON document into an owning `T`. The input is borrowed for the duration of
 * the call; malformed, oversized, or type-incompatible input returns an owning `error`. `T` must
 * provide the adapter metadata required to read its JSON shape. */
template <typename T> std::expected<T, error> parse(std::string_view input, options options = {})
{
    auto input_check = detail::check_input_size(input, options);
    if (!input_check)
        return std::unexpected(input_check.error());

    T value{};
    const glz::error_ctx glaze_error = options.allow_unknown_keys
                                           ? glz::read<detail::relaxed_read_options{}>(value, input)
                                           : glz::read<detail::read_options{}>(value, input);
    if (glaze_error)
        return std::unexpected(detail::from_read_error(glaze_error, input, options));

    return value;
}

/* Serialize a borrowed `T` into an owning JSON string. Serialization failures return an owning
 * `error`; `options::source` labels that failure and does not constrain output size. */
template <typename T>
std::expected<std::string, error> serialize(const T &value, options options = {})
{
    auto encoded = glz::write_json(value);
    if (!encoded)
        return std::unexpected(detail::from_write_error(encoded.error(), options));
    return std::move(*encoded);
}

/* Provider request strings may contain arbitrary user or tool text. Enable strict JSON escaping
 * before the result crosses a C-library parser, while keeping the faster default for trusted
 * internal records. */
template <typename T>
std::expected<std::string, error> serialize_escaped(const T &value, options options = {})
{
    struct escaped_write_options : glz::opts {
        bool escape_control_characters = true;
    };
    auto encoded = glz::write<escaped_write_options{}>(value);
    if (!encoded)
        return std::unexpected(detail::from_write_error(encoded.error(), options));
    return std::move(*encoded);
}

} // namespace hax::json

#endif /* HAX_JSON_H */
