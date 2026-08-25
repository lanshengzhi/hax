/* SPDX-License-Identifier: MIT */
#ifndef HAX_JSON_VALUE_H
#define HAX_JSON_VALUE_H

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace hax::json
{

enum class error_code {
    none,
    syntax,
    type,
    schema,
    invalid_utf8,
    trailing_data,
    input_too_large,
    serialization,
};

/* Options for one adapter operation. `source` is borrowed and copied into failures. A zero
 * `max_input_bytes` disables the bound; the default protects untrusted input. */
struct options {
    std::string_view source = "<json>";
    size_t max_input_bytes = 1 << 20;
    bool allow_unknown_keys =
        false; /* Typed adapters may ignore extension fields when permitted. */
};

/* An owning, provider-independent JSON failure. `source` and `message` remain valid after the
 * input buffer and any Glaze context have gone away. */
struct error {
    error_code code = error_code::none; /* Stable project category; none denotes success. */
    size_t offset = 0;                  /* Byte offset reported by the adapter. */
    std::string source;                 /* Source label copied from options. */
    std::string message;                /* Actionable parser or serializer diagnostic. */
};

/* A project-owned dynamic JSON value. Integers retain their signed 64-bit representation instead
 * of passing through double, and object members retain their input order. The adapter owns parsing
 * and serialization; domain code only observes or mutates this representation. */
struct value;
using array = std::vector<value>;
using object = std::vector<std::pair<std::string, value>>;

class value
{
  public:
    using integer = std::int64_t;
    using storage = std::variant<std::nullptr_t, bool, integer, double, std::string, array, object>;

  private:
    storage data = nullptr;

  public:
    value() = default;
    value(std::nullptr_t) : data(nullptr)
    {
    }
    value(bool source) : data(source)
    {
    }
    template <typename T>
        requires std::integral<T> && (!std::same_as<std::remove_cv_t<T>, bool>)
    value(T source) : data(static_cast<integer>(source))
    {
    }
    value(double source) : data(source)
    {
    }
    value(std::string source) : data(std::move(source))
    {
    }
    value(const char *source) : data(source ? std::string(source) : std::string())
    {
    }
    value(array source) : data(std::move(source))
    {
    }
    value(object source) : data(std::move(source))
    {
    }

    bool is_null() const
    {
        return std::holds_alternative<std::nullptr_t>(data);
    }
    bool is_boolean() const
    {
        return std::holds_alternative<bool>(data);
    }
    bool is_integer() const
    {
        return std::holds_alternative<integer>(data);
    }
    bool is_real() const
    {
        return std::holds_alternative<double>(data);
    }
    bool is_number() const
    {
        return is_integer() || is_real();
    }
    bool is_string() const
    {
        return std::holds_alternative<std::string>(data);
    }
    bool is_array() const
    {
        return std::holds_alternative<array>(data);
    }
    bool is_object() const
    {
        return std::holds_alternative<object>(data);
    }

    bool boolean_value() const
    {
        return std::get<bool>(data);
    }
    integer integer_value() const
    {
        return std::get<integer>(data);
    }
    double real_value() const
    {
        return is_integer() ? static_cast<double>(integer_value()) : std::get<double>(data);
    }
    const std::string &string_value() const
    {
        return std::get<std::string>(data);
    }

    const array &array_items() const
    {
        return std::get<array>(data);
    }
    array &array_items()
    {
        return std::get<array>(data);
    }
    const object &object_items() const
    {
        return std::get<object>(data);
    }
    object &object_items()
    {
        return std::get<object>(data);
    }

    const value *find(std::string_view key) const
    {
        const auto *members = std::get_if<object>(&data);
        if (!members)
            return nullptr;
        for (auto it = members->rbegin(); it != members->rend(); ++it)
            if (it->first == key)
                return &it->second;
        return nullptr;
    }

    value *find(std::string_view key)
    {
        auto *members = std::get_if<object>(&data);
        if (!members)
            return nullptr;
        for (auto it = members->rbegin(); it != members->rend(); ++it)
            if (it->first == key)
                return &it->second;
        return nullptr;
    }

    /* Create or replace one object member while preserving the original member position. */
    void set(std::string key, value source)
    {
        if (!is_object())
            data = object{};
        auto &members = object_items();
        for (auto it = members.end(); it != members.begin();) {
            --it;
            if (it->first == key) {
                it->second = std::move(source);
                return;
            }
        }
        members.emplace_back(std::move(key), std::move(source));
    }

    bool erase(std::string_view key)
    {
        if (!is_object())
            return false;
        auto &members = object_items();
        for (auto it = members.end(); it != members.begin();) {
            --it;
            if (it->first == key) {
                members.erase(it);
                return true;
            }
        }
        return false;
    }

    size_t size() const
    {
        if (is_array())
            return array_items().size();
        if (is_object())
            return object_items().size();
        if (is_string())
            return string_value().size();
        return 0;
    }
};

/* Return the stable human-readable name for a project error category. */
std::string_view error_code_name(error_code code) noexcept;

/* Format an owning error with its source label, byte offset, and parser or serializer
 * diagnostic. */
std::string format_error(const error &value);

/* Parse one complete JSON document into the project-owned dynamic value. */
std::expected<value, error> parse_value(std::string_view input, options options = {});

/* Serialize a project-owned dynamic value into compact JSON. */
std::expected<std::string, error> serialize_value(const value &source, options options = {});

/* Serialize a project-owned dynamic value using two-space indentation. */
std::expected<std::string, error> serialize_value_pretty(const value &source, options options = {});

/* Pretty-print one complete JSON document without changing its raw number or string fragments.
 * This path accepts valid values that do not fit the dynamic value's signed integer model. */
std::expected<std::string, error> pretty_json(std::string_view input, options options = {});

/* Return an optional string member from one complete JSON object. Unknown members remain raw, so
 * unrelated opaque values do not need to fit a typed or signed-integer representation. */
std::expected<std::optional<std::string>, error>
object_string_member(std::string_view input, std::string_view key, options options = {});

} // namespace hax::json

#endif /* HAX_JSON_VALUE_H */
