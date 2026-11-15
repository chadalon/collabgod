// Minimal JSON writing for the scanner. Spike code (issue #14).
//
// Strings from plugins are untrusted: a plugin may fill its name with bytes in
// a local code page, or leave a field without a terminator. Everything that
// goes into a JSON string passes through sanitiseUtf8 first.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace scan {

// Returns s with every invalid UTF-8 sequence replaced by U+FFFD.
std::string sanitiseUtf8 (std::string_view s);

// Reads a fixed-size char field that may lack a terminating zero.
std::string fixedField (const char* field, std::size_t size);

// Converts a fixed-size UTF-16 field that may lack a terminating zero.
std::string utf16Field (const char16_t* field, std::size_t size);

// Returns a quoted, escaped JSON string. Input is sanitised first.
std::string quote (std::string_view s);

// Builds a JSON document by appending. Keeps track of commas, nothing more.
class JsonWriter
{
public:
    JsonWriter& beginObject();
    JsonWriter& endObject();
    JsonWriter& beginArray();
    JsonWriter& endArray();

    JsonWriter& key (std::string_view k);
    JsonWriter& value (std::string_view s);
    JsonWriter& value (const char* s) { return value (std::string_view (s)); }
    JsonWriter& value (std::int64_t n);
    JsonWriter& value (bool b);
    JsonWriter& null();
    // Inserts text that is already valid JSON, such as a probe's output.
    JsonWriter& raw (std::string_view json);

    template <typename T>
    JsonWriter& field (std::string_view k, T v) { key (k); return value (v); }

    const std::string& str() const { return out; }

private:
    void beforeValue();

    std::string out;
    bool needComma = false;
    bool afterKey = false;
};

} // namespace scan
