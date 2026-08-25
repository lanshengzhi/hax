/* SPDX-License-Identifier: MIT */
#ifndef HAX_TESTS_JSON_HELPERS_H
#define HAX_TESTS_JSON_HELPERS_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdlib.h>
#include <string_view>
#include <utility>

#include "json.h"

using test_json = hax::json::value;

/* Return an owned parsed value; callers release it with test_json_release(). */
inline test_json *test_json_parse(const char *input)
{
    auto parsed = hax::json::parse_value(input ? std::string_view(input) : std::string_view{});
    if (!parsed)
        return nullptr;
    auto *storage = static_cast<test_json *>(malloc(sizeof(test_json)));
    return storage ? std::construct_at(storage, std::move(*parsed)) : nullptr;
}

/* Release a value returned by test_json_parse() or test_json_owned(). */
inline void test_json_release(test_json *value)
{
    if (value) {
        std::destroy_at(value);
        free(value);
    }
}

/* Move one temporary value into storage with the same ownership contract as test_json_parse(). */
inline test_json *test_json_owned(test_json value)
{
    auto *storage = static_cast<test_json *>(malloc(sizeof(test_json)));
    return storage ? std::construct_at(storage, std::move(value)) : nullptr;
}

inline const test_json *test_json_get(const test_json *value, std::string_view key)
{
    return value ? value->find(key) : nullptr;
}

inline test_json *test_json_get(test_json *value, std::string_view key)
{
    return value ? value->find(key) : nullptr;
}

inline const test_json *test_json_array_get(const test_json *value, size_t index)
{
    const auto *items = value ? value->array_ptr() : nullptr;
    return items && index < items->size() ? &(*items)[index] : nullptr;
}

inline test_json *test_json_array_get(test_json *value, size_t index)
{
    auto *items = value ? value->array_ptr() : nullptr;
    return items && index < items->size() ? &(*items)[index] : nullptr;
}

inline size_t test_json_size(const test_json *value)
{
    return value ? value->size() : 0;
}

inline const char *test_json_string(const test_json *value)
{
    const std::string *text = value ? value->string_ptr() : nullptr;
    return text ? text->c_str() : nullptr;
}

inline size_t test_json_string_size(const test_json *value)
{
    const std::string *text = value ? value->string_ptr() : nullptr;
    return text ? text->size() : 0;
}

inline bool test_json_is_object(const test_json *value)
{
    return value && value->is_object();
}
inline bool test_json_is_array(const test_json *value)
{
    return value && value->is_array();
}
inline bool test_json_is_string(const test_json *value)
{
    return value && value->is_string();
}
inline bool test_json_is_integer(const test_json *value)
{
    return value && value->is_integer();
}
inline bool test_json_is_real(const test_json *value)
{
    return value && value->is_real();
}
inline bool test_json_is_number(const test_json *value)
{
    return value && value->is_number();
}
inline bool test_json_is_boolean(const test_json *value)
{
    return value && value->is_boolean();
}
inline bool test_json_is_true(const test_json *value)
{
    const bool *boolean = value ? value->boolean_ptr() : nullptr;
    return boolean && *boolean;
}
inline bool test_json_is_false(const test_json *value)
{
    const bool *boolean = value ? value->boolean_ptr() : nullptr;
    return boolean && !*boolean;
}
inline bool test_json_is_null(const test_json *value)
{
    return value && value->is_null();
}

inline std::int64_t test_json_integer(const test_json *value)
{
    const std::int64_t *integer = value ? value->integer_ptr() : nullptr;
    return integer ? *integer : 0;
}

inline double test_json_real(const test_json *value)
{
    if (!value)
        return 0;
    if (const std::int64_t *integer = value->integer_ptr())
        return static_cast<double>(*integer);
    if (const double *real = value->real_ptr())
        return *real;
    return 0;
}

inline size_t test_json_object_size(const test_json *value)
{
    const auto *members = value ? value->object_ptr() : nullptr;
    return members ? members->size() : 0;
}

#endif /* HAX_TESTS_JSON_HELPERS_H */
