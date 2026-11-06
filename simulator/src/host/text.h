// Text for the studio: strings of code points (std::u32string, as Python's str), UTF-8, Latin-1 and hex.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace host {

using Bytes = std::vector<uint8_t>;

inline std::u32string from_utf8(const std::string& s) {
    std::u32string out;
    for (size_t i = 0; i < s.size();) {
        uint8_t c = (uint8_t)s[i];
        int n = c < 0x80 ? 0 : c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3;
        if (c >= 0x80 && c < 0xC0) {                     // a stray continuation byte: kept as that code
            out += c;
            i++;
            continue;
        }
        char32_t v = n == 0 ? c : n == 1 ? c & 0x1F : n == 2 ? c & 0x0F : c & 0x07;
        size_t j = 1;
        for (; j <= (size_t)n && i + j < s.size() && ((uint8_t)s[i + j] & 0xC0) == 0x80; j++)
            v = v << 6 | ((uint8_t)s[i + j] & 0x3F);
        out += v;
        i += j;
    }
    return out;
}

inline void put_utf8(std::string& out, char32_t c) {
    if (c < 0x80)
        out += (char)c;
    else if (c < 0x800) {
        out += (char)(0xC0 | c >> 6);
        out += (char)(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += (char)(0xE0 | c >> 12);
        out += (char)(0x80 | (c >> 6 & 0x3F));
        out += (char)(0x80 | (c & 0x3F));
    } else {
        out += (char)(0xF0 | c >> 18);
        out += (char)(0x80 | (c >> 12 & 0x3F));
        out += (char)(0x80 | (c >> 6 & 0x3F));
        out += (char)(0x80 | (c & 0x3F));
    }
}

inline std::string to_utf8(const std::u32string& s) {
    std::string out;
    for (char32_t c : s)
        put_utf8(out, c);
    return out;
}

inline std::string utf8_bytes(char32_t c) {
    std::string out;
    put_utf8(out, c);
    return out;
}

// Latin-1: one byte per code; throws when a code is above 255 (or puts '?' with replace).
inline Bytes latin1(const std::u32string& s, bool replace = false) {
    Bytes out;
    for (char32_t c : s) {
        if (c > 0xFF) {
            if (!replace)
                throw std::invalid_argument("only characters 0..255 exist in the sign's fonts");
            c = '?';
        }
        out.push_back((uint8_t)c);
    }
    return out;
}

inline std::u32string from_latin1(const Bytes& b) { return std::u32string(b.begin(), b.end()); }

// "1b 30 7c 00": lower case, separated by sep (Python's bytes.hex(" ")).
inline std::string hex(const Bytes& b, const char* sep = " ") {
    static const char* d = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < b.size(); i++) {
        if (i && sep)
            out += sep;
        out += d[b[i] >> 4];
        out += d[b[i] & 15];
    }
    return out;
}

// Hex digits with any spaces or newlines between them; throws on anything else.
inline Bytes unhex(const std::string& s) {
    Bytes out;
    int hi = -1;
    for (char c : s) {
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t')
            continue;
        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (v < 0)
            throw std::invalid_argument("not hex: " + s);
        if (hi < 0)
            hi = v;
        else {
            out.push_back((uint8_t)(hi << 4 | v));
            hi = -1;
        }
    }
    if (hi >= 0)
        throw std::invalid_argument("odd number of hex digits");
    return out;
}

inline void be16(Bytes& o, uint32_t v) {
    o.push_back((uint8_t)(v >> 8));
    o.push_back((uint8_t)v);
}
inline void be32(Bytes& o, uint32_t v) {
    be16(o, v >> 16);
    be16(o, v & 0xFFFF);
}
inline uint32_t rd16(const uint8_t* p) { return (uint32_t)p[0] << 8 | p[1]; }
inline uint32_t rd32(const uint8_t* p) { return rd16(p) << 16 | rd16(p + 2); }
inline uint32_t le16(const uint8_t* p) { return (uint32_t)p[1] << 8 | p[0]; }
inline uint32_t le32(const uint8_t* p) { return le16(p + 2) << 16 | le16(p); }

}  // namespace host
