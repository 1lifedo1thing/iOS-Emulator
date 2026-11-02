#pragma once
#include "Internal.hpp"
namespace lt {
inline std::string base64_encode(const std::string &s) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < s.size(); i += 3) {
        uint32_t n = uint32_t(uint8_t(s[i])) << 16;
        if (i + 1 < s.size())
            n |= uint32_t(uint8_t(s[i + 1])) << 8;
        if (i + 2 < s.size())
            n |= uint8_t(s[i + 2]);
        out += table[(n >> 18) & 63];
        out += table[(n >> 12) & 63];
        out += i + 1 < s.size() ? table[(n >> 6) & 63] : '=';
        out += i + 2 < s.size() ? table[n & 63] : '=';
    }
    return out;
}
inline std::string base64_decode(const std::string &s) {
    require(s.size() % 4 == 0 && s.size() <= 5500, "invalid bounded guest base64");
    std::string out;
    for (size_t i = 0; i < s.size(); i += 4) {
        uint32_t n = 0;
        unsigned pad = 0;
        for (unsigned k = 0; k < 4; k++) {
            unsigned char c = s[i + k];
            unsigned v = 0;
            if (c >= 'A' && c <= 'Z')
                v = c - 'A';
            else if (c >= 'a' && c <= 'z')
                v = c - 'a' + 26;
            else if (c >= '0' && c <= '9')
                v = c - '0' + 52;
            else if (c == '+')
                v = 62;
            else if (c == '/')
                v = 63;
            else if (c == '=') {
                require(k >= 2 && i + 4 == s.size(), "invalid guest base64 padding");
                pad++;
            } else
                throw Failure(LT_INVALID, "invalid guest base64 character");
            require(!pad || c == '=', "invalid guest base64 padding order");
            n = (n << 6) | v;
        }
        require(pad <= 2, "invalid base64 padding");
        out += char(n >> 16);
        if (pad < 2)
            out += char(n >> 8);
        if (pad < 1)
            out += char(n);
        if (pad)
            require((n & ((1u << (pad * 8)) - 1u)) == 0, "noncanonical base64 padding");
    }
    return out;
}
} // namespace lt
