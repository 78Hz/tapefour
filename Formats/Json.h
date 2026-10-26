#pragma once

/*
    A small JSON value, reader and writer for the cassette and deck formats.
    Plain C++17, no dependencies. Objects keep their members in the order they were added,
    so a file that is read and written again comes out the same.
*/

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tfmt
{
/** Exact comparison of two numbers (written so, because exact is what is meant). */
inline bool sameNumber (double a, double b) noexcept { return std::equal_to<double>() (a, b); }

class Json
{
public:
    enum class Type { null, boolean, number, string, array, object };

    Json() = default;
    Json (std::nullptr_t) {}
    Json (bool v) : kind (Type::boolean), b (v) {}
    Json (int v) : kind (Type::number), n (v) {}
    Json (int64_t v) : kind (Type::number), n ((double) v) {}
    Json (double v) : kind (Type::number), n (v) {}
    Json (const char* v) : kind (Type::string), s (v) {}
    Json (std::string v) : kind (Type::string), s (std::move (v)) {}

    static Json array()  { Json j; j.kind = Type::array; return j; }
    static Json object() { Json j; j.kind = Type::object; return j; }

    Type type() const noexcept      { return kind; }
    bool isNull() const noexcept    { return kind == Type::null; }
    bool isObject() const noexcept  { return kind == Type::object; }
    bool isArray() const noexcept   { return kind == Type::array; }
    bool isNumber() const noexcept  { return kind == Type::number; }
    bool isString() const noexcept  { return kind == Type::string; }
    bool isBool() const noexcept    { return kind == Type::boolean; }

    // ---- reading, with a default when the value is missing or of another type ----
    double number (double fallback = 0.0) const noexcept      { return kind == Type::number ? n : fallback; }
    int64_t integer (int64_t fallback = 0) const noexcept;
    bool boolean (bool fallback = false) const noexcept       { return kind == Type::boolean ? b : fallback; }
    std::string string (const std::string& fallback = {}) const { return kind == Type::string ? s : fallback; }

    // ---- objects ----
    /** The member, or a null value if there is none. */
    const Json& operator[] (std::string_view key) const noexcept;
    /** The member, added (as null) if there is none; turns a null value into an object. */
    Json& set (std::string_view key);
    Json& set (std::string_view key, Json value) { return set (key) = std::move (value); }
    bool has (std::string_view key) const noexcept;
    const std::vector<std::pair<std::string, Json>>& members() const noexcept { return obj; }

    // ---- arrays ----
    size_t size() const noexcept { return kind == Type::array ? arr.size() : (kind == Type::object ? obj.size() : 0); }
    const Json& operator[] (size_t index) const noexcept;
    Json& push (Json value);
    const std::vector<Json>& elements() const noexcept { return arr; }

    bool operator== (const Json& other) const noexcept;
    bool operator!= (const Json& other) const noexcept { return ! (*this == other); }

    /** UTF-8 text; indent 0 writes it on one line. */
    std::string dump (int indent = 2) const;

    /** Parses UTF-8 text; on failure, nothing, and why in error (with the line). */
    static std::optional<Json> parse (std::string_view text, std::string* error = nullptr);

private:
    void write (std::string& out, int indent, int depth) const;

    Type kind = Type::null;
    bool b = false;
    double n = 0.0;
    std::string s;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;
};
} // namespace tfmt
