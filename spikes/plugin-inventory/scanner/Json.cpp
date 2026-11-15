// Spike code (issue #14). See Json.h.

#include "Json.h"

#include <cstdio>

namespace scan {

std::string sanitiseUtf8 (std::string_view s)
{
    static constexpr const char* replacement = "\xEF\xBF\xBD";
    std::string out;
    out.reserve (s.size());

    std::size_t i = 0;
    while (i < s.size())
    {
        const auto c = static_cast<unsigned char> (s[i]);
        std::size_t len = 0;
        std::uint32_t cp = 0;

        if (c < 0x80)       { len = 1; cp = c; }
        else if (c >= 0xC2 && c <= 0xDF) { len = 2; cp = c & 0x1Fu; }
        else if (c >= 0xE0 && c <= 0xEF) { len = 3; cp = c & 0x0Fu; }
        else if (c >= 0xF0 && c <= 0xF4) { len = 4; cp = c & 0x07u; }
        else { out += replacement; ++i; continue; }

        bool ok = i + len <= s.size();
        for (std::size_t k = 1; ok && k < len; ++k)
        {
            const auto cc = static_cast<unsigned char> (s[i + k]);
            if ((cc & 0xC0) != 0x80) ok = false;
            else cp = (cp << 6) | (cc & 0x3Fu);
        }
        // Reject overlong forms, surrogates and values past U+10FFFF.
        if (ok && ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF))
                   || (cp >= 0xD800 && cp <= 0xDFFF)))
            ok = false;

        if (ok) { out.append (s.substr (i, len)); i += len; }
        else    { out += replacement; ++i; }
    }
    return out;
}

std::string fixedField (const char* field, std::size_t size)
{
    std::size_t n = 0;
    while (n < size && field[n] != '\0') ++n;
    return sanitiseUtf8 (std::string_view (field, n));
}

std::string utf16Field (const char16_t* field, std::size_t size)
{
    std::string out;
    std::size_t i = 0;
    auto put = [&out] (std::uint32_t cp)
    {
        if (cp < 0x80) out += static_cast<char> (cp);
        else if (cp < 0x800)
        {
            out += static_cast<char> (0xC0 | (cp >> 6));
            out += static_cast<char> (0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000)
        {
            out += static_cast<char> (0xE0 | (cp >> 12));
            out += static_cast<char> (0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char> (0x80 | (cp & 0x3F));
        }
        else
        {
            out += static_cast<char> (0xF0 | (cp >> 18));
            out += static_cast<char> (0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char> (0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char> (0x80 | (cp & 0x3F));
        }
    };

    while (i < size && field[i] != 0)
    {
        const std::uint32_t u = field[i];
        if (u >= 0xD800 && u <= 0xDBFF && i + 1 < size
            && field[i + 1] >= 0xDC00 && field[i + 1] <= 0xDFFF)
        {
            put (0x10000 + ((u - 0xD800) << 10) + (static_cast<std::uint32_t> (field[i + 1]) - 0xDC00));
            i += 2;
        }
        else if (u >= 0xD800 && u <= 0xDFFF)
        {
            put (0xFFFD);
            ++i;
        }
        else
        {
            put (u);
            ++i;
        }
    }
    return out;
}

std::string quote (std::string_view input)
{
    const auto s = sanitiseUtf8 (input);
    std::string out;
    out.reserve (s.size() + 2);
    out += '"';
    for (const char ch : s)
    {
        const auto c = static_cast<unsigned char> (ch);
        switch (c)
        {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20)
                {
                    char buf[8];
                    std::snprintf (buf, sizeof (buf), "\\u%04x", c);
                    out += buf;
                }
                else
                {
                    out += ch;
                }
        }
    }
    out += '"';
    return out;
}

void JsonWriter::beforeValue()
{
    if (afterKey) { afterKey = false; return; }
    if (needComma) out += ',';
}

JsonWriter& JsonWriter::beginObject() { beforeValue(); out += '{'; needComma = false; return *this; }
JsonWriter& JsonWriter::endObject()   { out += '}'; needComma = true; return *this; }
JsonWriter& JsonWriter::beginArray()  { beforeValue(); out += '['; needComma = false; return *this; }
JsonWriter& JsonWriter::endArray()    { out += ']'; needComma = true; return *this; }

JsonWriter& JsonWriter::key (std::string_view k)
{
    if (needComma) out += ',';
    out += quote (k);
    out += ':';
    afterKey = true;
    return *this;
}

JsonWriter& JsonWriter::value (std::string_view s)  { beforeValue(); out += quote (s); needComma = true; return *this; }
JsonWriter& JsonWriter::value (std::int64_t n)      { beforeValue(); out += std::to_string (n); needComma = true; return *this; }
JsonWriter& JsonWriter::value (bool b)              { beforeValue(); out += b ? "true" : "false"; needComma = true; return *this; }
JsonWriter& JsonWriter::null()                      { beforeValue(); out += "null"; needComma = true; return *this; }
JsonWriter& JsonWriter::raw (std::string_view json) { beforeValue(); out.append (json); needComma = true; return *this; }

} // namespace scan
