#pragma once
#include <map>
#include <string>
#include <vector>
#include <stdexcept>
#include <cstdint>
#include <cstdio>
#include <limits>
namespace lt {
// Strict bounded JSON reader. Numeric lexemes remain exact (no double rounding
// for archive sizes), duplicate keys and malformed Unicode are rejected.
struct Json {
    enum Kind { Null, Boolean, Number, String, Array, Object } kind = Null;
    std::string text;
    std::vector<Json> array;
    std::map<std::string, Json> object;
    const Json &at(const std::string &k) const {
        if (kind != Object || !object.count(k))
            throw std::runtime_error("missing JSON field: " + k);
        return object.at(k);
    }
    const Json *find(const std::string &k) const {
        auto i = object.find(k);
        return kind == Object && i != object.end() ? &i->second : nullptr;
    }
    std::string string() const {
        if (kind != String)
            throw std::runtime_error("expected JSON string");
        return text;
    }
    uint64_t integer() const {
        if (kind != Number || text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
            throw std::runtime_error("expected unsigned integer");
        uint64_t n = 0;
        for (char c : text) {
            if (n > (UINT64_MAX - (c - '0')) / 10)
                throw std::runtime_error("integer overflow");
            n = n * 10 + c - '0';
        }
        return n;
    }
};
class JsonReader {
    const std::string &s;
    size_t p = 0;
    [[noreturn]] void bad() { throw std::runtime_error("invalid JSON at byte " + std::to_string(p)); }
    void ws() {
        while (p < s.size() && (s[p] == ' ' || s[p] == '\n' || s[p] == '\r' || s[p] == '\t'))
            p++;
    }
    bool take(char c) {
        ws();
        if (p < s.size() && s[p] == c) {
            p++;
            return true;
        }
        return false;
    }
    uint32_t hex() {
        uint32_t n = 0;
        for (int i = 0; i < 4; i++) {
            if (p == s.size())
                bad();
            char c = s[p++];
            n <<= 4;
            if (c >= '0' && c <= '9')
                n += c - '0';
            else if (c >= 'a' && c <= 'f')
                n += c - 'a' + 10;
            else if (c >= 'A' && c <= 'F')
                n += c - 'A' + 10;
            else
                bad();
        }
        return n;
    }
    void utf8(std::string &o, uint32_t n) {
        if (n < 128)
            o += char(n);
        else if (n < 2048) {
            o += char(0xc0 | (n >> 6));
            o += char(0x80 | (n & 63));
        } else if (n < 65536) {
            o += char(0xe0 | (n >> 12));
            o += char(0x80 | ((n >> 6) & 63));
            o += char(0x80 | (n & 63));
        } else {
            o += char(0xf0 | (n >> 18));
            o += char(0x80 | ((n >> 12) & 63));
            o += char(0x80 | ((n >> 6) & 63));
            o += char(0x80 | (n & 63));
        }
    }
    std::string str() {
        if (!take('"'))
            bad();
        std::string o;
        while (p < s.size()) {
            unsigned char c = s[p++];
            if (c == '"')
                return o;
            if (c < 32)
                bad();
            if (c != '\\') {
                if (c < 128) {
                    o += char(c);
                    continue;
                }
                unsigned count = c >= 0xc2 && c <= 0xdf   ? 1
                                 : c >= 0xe0 && c <= 0xef ? 2
                                 : c >= 0xf0 && c <= 0xf4 ? 3
                                                          : 0;
                if (!count)
                    bad();
                uint32_t code = c & ((1u << (6 - count)) - 1u);
                for (unsigned i = 0; i < count; i++) {
                    if (p == s.size())
                        bad();
                    unsigned char next = s[p++];
                    if ((next & 0xc0) != 0x80)
                        bad();
                    code = (code << 6) | (next & 63);
                }
                if ((count == 1 && code < 128) || (count == 2 && code < 2048) || (count == 3 && code < 65536) ||
                    code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
                    bad();
                utf8(o, code);
                continue;
            }
            if (p == s.size())
                bad();
            c = s[p++];
            switch (c) {
            case '"':
            case '\\':
            case '/':
                o += char(c);
                break;
            case 'b':
                o += '\b';
                break;
            case 'f':
                o += '\f';
                break;
            case 'n':
                o += '\n';
                break;
            case 'r':
                o += '\r';
                break;
            case 't':
                o += '\t';
                break;
            case 'u': {
                uint32_t n = hex();
                if (n >= 0xd800 && n <= 0xdbff) {
                    if (p + 2 > s.size() || s[p++] != '\\' || s[p++] != 'u')
                        bad();
                    uint32_t low = hex();
                    if (low < 0xdc00 || low > 0xdfff)
                        bad();
                    n = 0x10000 + ((n - 0xd800) << 10) + (low - 0xdc00);
                } else if (n >= 0xdc00 && n <= 0xdfff)
                    bad();
                utf8(o, n);
                break;
            }
            default:
                bad();
            }
        }
        bad();
    }
    Json value(unsigned depth) {
        if (depth > 64)
            bad();
        ws();
        if (p == s.size())
            bad();
        Json j;
        char c = s[p];
        if (c == '"') {
            j.kind = Json::String;
            j.text = str();
        } else if (c == '{') {
            p++;
            j.kind = Json::Object;
            if (!take('}')) {
                do {
                    std::string k = str();
                    if (!take(':'))
                        bad();
                    if (!j.object.emplace(k, value(depth + 1)).second)
                        bad();
                } while (take(','));
                if (!take('}'))
                    bad();
            }
        } else if (c == '[') {
            p++;
            j.kind = Json::Array;
            if (!take(']')) {
                do {
                    j.array.push_back(value(depth + 1));
                } while (take(','));
                if (!take(']'))
                    bad();
            }
        } else if (c == 't' || c == 'f' || c == 'n') {
            std::string token = c == 't' ? "true" : c == 'f' ? "false" : "null";
            if (s.compare(p, token.size(), token))
                bad();
            p += token.size();
            j.kind = c == 'n' ? Json::Null : Json::Boolean;
            j.text = token;
        } else {
            j.kind = Json::Number;
            size_t start = p;
            if (s[p] == '-')
                p++;
            if (p == s.size())
                bad();
            if (s[p] == '0')
                p++;
            else {
                if (s[p] < '1' || s[p] > '9')
                    bad();
                while (p < s.size() && s[p] >= '0' && s[p] <= '9')
                    p++;
            }
            if (p < s.size() && s[p] == '.') {
                p++;
                size_t at = p;
                while (p < s.size() && s[p] >= '0' && s[p] <= '9')
                    p++;
                if (at == p)
                    bad();
            }
            if (p < s.size() && (s[p] == 'e' || s[p] == 'E')) {
                p++;
                if (p < s.size() && (s[p] == '+' || s[p] == '-'))
                    p++;
                size_t at = p;
                while (p < s.size() && s[p] >= '0' && s[p] <= '9')
                    p++;
                if (at == p)
                    bad();
            }
            j.text = s.substr(start, p - start);
        }
        return j;
    }

  public:
    explicit JsonReader(const std::string &input) : s(input) {}
    Json parse() {
        Json j = value(0);
        ws();
        if (p != s.size())
            bad();
        return j;
    }
};
inline Json parse(const std::string &s) { return JsonReader(s).parse(); }
// Writes a parsed document back out; numbers keep their exact lexemes and keys come out sorted.
inline std::string dump(const Json &j) {
    switch (j.kind) {
    case Json::String: {
        std::string o = "\"";
        for (unsigned char c : j.text) {
            if (c == '"' || c == '\\')
                o += '\\', o += char(c);
            else if (c < 0x20) {
                char escape[8];
                snprintf(escape, sizeof escape, "\\u%04x", c);
                o += escape;
            } else
                o += char(c);
        }
        return o + "\"";
    }
    case Json::Array: {
        std::string o = "[";
        for (size_t i = 0; i < j.array.size(); ++i)
            o += (i ? "," : "") + dump(j.array[i]);
        return o + "]";
    }
    case Json::Object: {
        std::string o = "{";
        for (const auto &pair : j.object) {
            Json key;
            key.kind = Json::String;
            key.text = pair.first;
            o += (o.size() > 1 ? "," : "") + dump(key) + ":" + dump(pair.second);
        }
        return o + "}";
    }
    case Json::Null:
        return "null";
    default:
        return j.text;
    }
}
} // namespace lt
