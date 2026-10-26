#include "Json.h"

#include <cmath>
#include <locale>
#include <sstream>

namespace tfmt
{
namespace
{
    const Json nullJson;

    // Numbers are written and read in the "C" locale whatever the program's own is, so a file
    // written in Germany reads in England.
    std::string formatNumber (double v)
    {
        if (! std::isfinite (v)) return "null";
        if (std::abs (v) < 1.0e15 && sameNumber (std::floor (v), v))
        {
            std::ostringstream o;
            o.imbue (std::locale::classic());
            o << (long long) v;
            return o.str();
        }
        // The shortest text that reads back as exactly the same number.
        for (int digits = 15; digits <= 17; ++digits)
        {
            std::ostringstream o;
            o.imbue (std::locale::classic());
            o.precision (digits);
            o << v;
            std::istringstream back (o.str());
            back.imbue (std::locale::classic());
            double r = 0;
            back >> r;
            if (sameNumber (r, v) || digits == 17) return o.str();
        }
        return "0";
    }

    void writeString (std::string& out, const std::string& s)
    {
        out += '"';
        for (char ch : s)
        {
            const auto c = (unsigned char) ch;
            switch (c)
            {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                default:
                    if (c < 0x20)
                    {
                        const char* hex = "0123456789abcdef";
                        out += "\\u00";
                        out += hex[c >> 4];
                        out += hex[c & 15];
                    }
                    else
                    {
                        out += (char) c;   // UTF-8 passes through as it is
                    }
            }
        }
        out += '"';
    }

    class Parser
    {
    public:
        explicit Parser (std::string_view t) : text (t) {}

        std::optional<Json> document (std::string* error)
        {
            auto v = value (0);
            skipSpace();
            if (v && pos != text.size()) fail ("text after the end of the document");
            if (! problem.empty())
            {
                if (error != nullptr) *error = problem;
                return std::nullopt;
            }
            return v;
        }

    private:
        static constexpr int maxDepth = 64;   // files come from anywhere: keep a hostile one from exhausting the stack

        std::optional<Json> value (int depth)
        {
            if (depth > maxDepth) { fail ("nested too deeply"); return std::nullopt; }
            skipSpace();
            if (pos >= text.size()) { fail ("unexpected end"); return std::nullopt; }
            auto c = text[pos];
            if (c == '{') return objectValue (depth);
            if (c == '[') return arrayValue (depth);
            if (c == '"') { auto s = stringValue(); if (! s) return std::nullopt; return Json (std::move (*s)); }
            if (c == 't') return literal ("true", Json (true));
            if (c == 'f') return literal ("false", Json (false));
            if (c == 'n') return literal ("null", Json());
            if (c == '-' || (c >= '0' && c <= '9')) return numberValue();
            fail ("unexpected character");
            return std::nullopt;
        }

        std::optional<Json> objectValue (int depth)
        {
            ++pos;
            auto j = Json::object();
            skipSpace();
            if (peek ('}')) { ++pos; return j; }
            for (;;)
            {
                skipSpace();
                if (! peek ('"')) { fail ("expected a member name"); return std::nullopt; }
                auto key = stringValue();
                if (! key) return std::nullopt;
                skipSpace();
                if (! peek (':')) { fail ("expected ':'"); return std::nullopt; }
                ++pos;
                auto v = value (depth + 1);
                if (! v) return std::nullopt;
                j.set (*key) = std::move (*v);
                skipSpace();
                if (peek (',')) { ++pos; continue; }
                if (peek ('}')) { ++pos; return j; }
                fail ("expected ',' or '}'");
                return std::nullopt;
            }
        }

        std::optional<Json> arrayValue (int depth)
        {
            ++pos;
            auto j = Json::array();
            skipSpace();
            if (peek (']')) { ++pos; return j; }
            for (;;)
            {
                auto v = value (depth + 1);
                if (! v) return std::nullopt;
                j.push (std::move (*v));
                skipSpace();
                if (peek (',')) { ++pos; continue; }
                if (peek (']')) { ++pos; return j; }
                fail ("expected ',' or ']'");
                return std::nullopt;
            }
        }

        std::optional<std::string> stringValue()
        {
            ++pos;   // the opening quote
            std::string out;
            while (pos < text.size())
            {
                auto c = (unsigned char) text[pos++];
                if (c == '"') return out;
                if (c < 0x20) { fail ("control character in a string"); return std::nullopt; }
                if (c != '\\') { out += (char) c; continue; }
                if (pos >= text.size()) break;
                auto e = text[pos++];
                switch (e)
                {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u':
                    {
                        auto cp = hex4();
                        if (cp < 0) return std::nullopt;
                        if (cp >= 0xd800 && cp <= 0xdbff)
                        {
                            // a surrogate pair
                            if (pos + 1 < text.size() && text[pos] == '\\' && text[pos + 1] == 'u')
                            {
                                pos += 2;
                                auto lo = hex4();
                                if (lo < 0xdc00 || lo > 0xdfff) { fail ("bad surrogate pair"); return std::nullopt; }
                                cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                            }
                            else { fail ("lone surrogate"); return std::nullopt; }
                        }
                        appendUtf8 (out, (uint32_t) cp);
                        break;
                    }
                    default: fail ("bad escape"); return std::nullopt;
                }
            }
            fail ("unterminated string");
            return std::nullopt;
        }

        long hex4()
        {
            if (pos + 4 > text.size()) { fail ("short \\u escape"); return -1; }
            long v = 0;
            for (int i = 0; i < 4; ++i)
            {
                auto c = text[pos++];
                v <<= 4;
                if (c >= '0' && c <= '9') v += c - '0';
                else if (c >= 'a' && c <= 'f') v += c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') v += c - 'A' + 10;
                else { fail ("bad \\u escape"); return -1; }
            }
            return v;
        }

        static void appendUtf8 (std::string& out, uint32_t cp)
        {
            if (cp < 0x80) out += (char) cp;
            else if (cp < 0x800) { out += (char) (0xc0 | (cp >> 6)); out += (char) (0x80 | (cp & 0x3f)); }
            else if (cp < 0x10000) { out += (char) (0xe0 | (cp >> 12)); out += (char) (0x80 | ((cp >> 6) & 0x3f)); out += (char) (0x80 | (cp & 0x3f)); }
            else { out += (char) (0xf0 | (cp >> 18)); out += (char) (0x80 | ((cp >> 12) & 0x3f)); out += (char) (0x80 | ((cp >> 6) & 0x3f)); out += (char) (0x80 | (cp & 0x3f)); }
        }

        std::optional<Json> numberValue()
        {
            auto start = pos;
            if (peek ('-')) ++pos;
            auto digits = [this] { size_t n = 0; while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') { ++pos; ++n; } return n; };
            if (digits() == 0) { fail ("bad number"); return std::nullopt; }
            if (peek ('.')) { ++pos; if (digits() == 0) { fail ("bad number"); return std::nullopt; } }
            if (peek ('e') || peek ('E'))
            {
                ++pos;
                if (peek ('+') || peek ('-')) ++pos;
                if (digits() == 0) { fail ("bad number"); return std::nullopt; }
            }
            std::istringstream in (std::string (text.substr (start, pos - start)));
            in.imbue (std::locale::classic());
            double v = 0;
            in >> v;
            if (in.fail()) { fail ("number out of range"); return std::nullopt; }
            return Json (v);
        }

        std::optional<Json> literal (std::string_view word, Json v)
        {
            if (text.substr (pos, word.size()) != word) { fail ("unexpected word"); return std::nullopt; }
            pos += word.size();
            return v;
        }

        void skipSpace()
        {
            while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' || text[pos] == '\r'))
                ++pos;
        }

        bool peek (char c) const noexcept { return pos < text.size() && text[pos] == c; }

        void fail (const char* why)
        {
            if (! problem.empty()) return;
            int line = 1;
            for (size_t i = 0; i < pos && i < text.size(); ++i) if (text[i] == '\n') ++line;
            problem = std::string (why) + " (line " + std::to_string (line) + ")";
        }

        std::string_view text;
        size_t pos = 0;
        std::string problem;
    };
}

int64_t Json::integer (int64_t fallback) const noexcept
{
    if (kind != Type::number || ! std::isfinite (n) || std::abs (n) > 9.0e18) return fallback;
    return (int64_t) std::llround (n);
}

const Json& Json::operator[] (std::string_view key) const noexcept
{
    if (kind == Type::object)
        for (auto& m : obj)
            if (m.first == key) return m.second;
    return nullJson;
}

Json& Json::set (std::string_view key)
{
    if (kind != Type::object) { *this = object(); }
    for (auto& m : obj)
        if (m.first == key) return m.second;
    obj.emplace_back (std::string (key), Json());
    return obj.back().second;
}

bool Json::has (std::string_view key) const noexcept
{
    if (kind == Type::object)
        for (auto& m : obj)
            if (m.first == key) return true;
    return false;
}

const Json& Json::operator[] (size_t index) const noexcept
{
    return kind == Type::array && index < arr.size() ? arr[index] : nullJson;
}

Json& Json::push (Json value)
{
    if (kind != Type::array) { *this = array(); }
    arr.push_back (std::move (value));
    return arr.back();
}

bool Json::operator== (const Json& o) const noexcept
{
    if (kind != o.kind) return false;
    switch (kind)
    {
        case Type::null:    return true;
        case Type::boolean: return b == o.b;
        case Type::number:  return sameNumber (n, o.n);
        case Type::string:  return s == o.s;
        case Type::array:   return arr == o.arr;
        case Type::object:  return obj == o.obj;
    }
    return false;
}

std::string Json::dump (int indent) const
{
    std::string out;
    write (out, indent, 0);
    if (indent > 0) out += '\n';
    return out;
}

void Json::write (std::string& out, int indent, int depth) const
{
    auto newline = [&] (int d) { if (indent > 0) { out += '\n'; out.append ((size_t) (indent * d), ' '); } };
    switch (kind)
    {
        case Type::null:    out += "null"; break;
        case Type::boolean: out += b ? "true" : "false"; break;
        case Type::number:  out += formatNumber (n); break;
        case Type::string:  writeString (out, s); break;
        case Type::array:
        {
            // Short arrays of numbers stay on one line, so a file stays readable.
            bool flat = true;
            for (auto& e : arr) flat = flat && e.kind != Type::object && e.kind != Type::array;
            out += '[';
            for (size_t i = 0; i < arr.size(); ++i)
            {
                if (i > 0) out += flat ? ", " : ",";
                if (! flat) newline (depth + 1);
                arr[i].write (out, indent, depth + 1);
            }
            if (! flat && ! arr.empty()) newline (depth);
            out += ']';
            break;
        }
        case Type::object:
        {
            out += '{';
            for (size_t i = 0; i < obj.size(); ++i)
            {
                if (i > 0) out += ',';
                newline (depth + 1);
                writeString (out, obj[i].first);
                out += indent > 0 ? ": " : ":";
                obj[i].second.write (out, indent, depth + 1);
            }
            if (! obj.empty()) newline (depth);
            out += '}';
            break;
        }
    }
}

std::optional<Json> Json::parse (std::string_view text, std::string* error)
{
    // A UTF-8 byte order mark is allowed and ignored.
    if (text.size() >= 3 && (unsigned char) text[0] == 0xef && (unsigned char) text[1] == 0xbb && (unsigned char) text[2] == 0xbf)
        text.remove_prefix (3);
    return Parser (text).document (error);
}
} // namespace tfmt
