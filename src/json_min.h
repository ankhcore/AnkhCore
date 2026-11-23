// AnkhCore 2.5 — a small strict JSON reader for nested documents (the .acbackup manifest, UniGetUI
// bundles). Pure; unit-tested in tests/test_base.cpp. The settings store keeps its own flat parser.
// Strict on purpose: duplicate keys, trailing text, lone surrogates and deep nesting are refused,
// because these documents decide what a restore writes.
#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <utility>

namespace ak {

struct JVal {
    enum Type { Null, Bool, Num, Str, Arr, Obj };
    Type t = Null; bool b = false; double n = 0; std::string s;
    std::vector<JVal> a;                                  // array items, or object values
    std::vector<std::string> keys;                        // object keys, same order as a
    const JVal* Get(const std::string& k) const {
        if (t != Obj) return nullptr;
        for (size_t i = 0; i < keys.size(); ++i) if (keys[i] == k) return &a[i];
        return nullptr;
    }
    bool Is(Type x) const { return t == x; }
    // A whole number in [0, 2^53]: sizes and counts.
    bool AsUInt(uint64_t& v) const {
        if (t != Num || !(n >= 0) || n > 9007199254740992.0 || std::floor(n) != n) return false;
        v = static_cast<uint64_t>(n); return true;
    }
};

namespace jsondetail {
struct Parser {
    const char* p; const char* e; int maxDepth;
    void Ws() { while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p; }
    static void Utf8(std::string& s, uint32_t c) {
        if (c < 0x80) s += static_cast<char>(c);
        else if (c < 0x800) { s += static_cast<char>(0xC0 | (c >> 6)); s += static_cast<char>(0x80 | (c & 63)); }
        else if (c < 0x10000) { s += static_cast<char>(0xE0 | (c >> 12)); s += static_cast<char>(0x80 | ((c >> 6) & 63)); s += static_cast<char>(0x80 | (c & 63)); }
        else { s += static_cast<char>(0xF0 | (c >> 18)); s += static_cast<char>(0x80 | ((c >> 12) & 63)); s += static_cast<char>(0x80 | ((c >> 6) & 63)); s += static_cast<char>(0x80 | (c & 63)); }
    }
    bool Hex4(uint32_t& v) {
        if (e - p < 4) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = *p++; uint32_t d;
            if (c >= '0' && c <= '9') d = c - '0'; else if (c >= 'a' && c <= 'f') d = c - 'a' + 10; else if (c >= 'A' && c <= 'F') d = c - 'A' + 10; else return false;
            v = v * 16 + d;
        }
        return true;
    }
    bool String(std::string& out) {      // *p == '"'
        ++p;
        while (p < e) {
            const unsigned char c = static_cast<unsigned char>(*p++);
            if (c == '"') return true;
            if (c < 0x20) return false;
            if (c != '\\') { out += static_cast<char>(c); continue; }
            if (p >= e) return false;
            switch (*p++) {
            case '"': out += '"'; break;   case '\\': out += '\\'; break; case '/': out += '/'; break;
            case 'b': out += '\b'; break;  case 'f': out += '\f'; break;  case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;  case 't': out += '\t'; break;
            case 'u': {
                uint32_t u; if (!Hex4(u)) return false;
                if (u >= 0xD800 && u <= 0xDBFF) {
                    if (e - p < 6 || p[0] != '\\' || p[1] != 'u') return false;
                    p += 2; uint32_t lo; if (!Hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
                    u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                } else if (u >= 0xDC00 && u <= 0xDFFF) return false;
                Utf8(out, u); break;
            }
            default: return false;
            }
        }
        return false;
    }
    bool Digits() { if (p >= e || *p < '0' || *p > '9') return false; while (p < e && *p >= '0' && *p <= '9') ++p; return true; }
    bool Number(double& v) {
        const char* s = p;
        if (p < e && *p == '-') ++p;
        if (p >= e) return false;
        if (*p == '0') ++p; else if (!Digits()) return false;
        if (p < e && *p == '.') { ++p; if (!Digits()) return false; }
        if (p < e && (*p == 'e' || *p == 'E')) { ++p; if (p < e && (*p == '+' || *p == '-')) ++p; if (!Digits()) return false; }
        const std::string t(s, p);
        v = std::strtod(t.c_str(), nullptr);
        return std::isfinite(v);
    }
    bool Lit(const char* w) { const size_t n = strlen(w); if (static_cast<size_t>(e - p) < n || memcmp(p, w, n) != 0) return false; p += n; return true; }
    bool Value(JVal& v, int depth) {
        Ws();
        if (p >= e) return false;
        switch (*p) {
        case '{': {
            if (depth >= maxDepth) return false;
            ++p; v.t = JVal::Obj; Ws();
            if (p < e && *p == '}') { ++p; return true; }
            for (;;) {
                Ws();
                if (p >= e || *p != '"') return false;
                std::string k; if (!String(k)) return false;
                for (const auto& have : v.keys) if (have == k) return false;      // duplicate key
                Ws(); if (p >= e || *p != ':') return false; ++p;
                JVal c; if (!Value(c, depth + 1)) return false;
                v.keys.push_back(std::move(k)); v.a.push_back(std::move(c));
                Ws(); if (p >= e) return false;
                if (*p == ',') { ++p; continue; }
                if (*p == '}') { ++p; return true; }
                return false;
            }
        }
        case '[': {
            if (depth >= maxDepth) return false;
            ++p; v.t = JVal::Arr; Ws();
            if (p < e && *p == ']') { ++p; return true; }
            for (;;) {
                JVal c; if (!Value(c, depth + 1)) return false;
                v.a.push_back(std::move(c));
                Ws(); if (p >= e) return false;
                if (*p == ',') { ++p; continue; }
                if (*p == ']') { ++p; return true; }
                return false;
            }
        }
        case '"': v.t = JVal::Str; return String(v.s);
        case 't': v.t = JVal::Bool; v.b = true; return Lit("true");
        case 'f': v.t = JVal::Bool; v.b = false; return Lit("false");
        case 'n': v.t = JVal::Null; return Lit("null");
        default: v.t = JVal::Num; return Number(v.n);
        }
    }
};
}  // namespace jsondetail

// Parses one complete JSON document (UTF-8, an optional BOM). False on any deviation.
inline bool JsonParse(const std::string& text, JVal& out, int maxDepth = 32) {
    const char* b = text.data(); const char* e = b + text.size();
    if (text.size() >= 3 && static_cast<unsigned char>(b[0]) == 0xEF && static_cast<unsigned char>(b[1]) == 0xBB && static_cast<unsigned char>(b[2]) == 0xBF) b += 3;
    jsondetail::Parser ps{b, e, maxDepth};
    JVal v;
    if (!ps.Value(v, 0)) return false;
    ps.Ws();
    if (ps.p != ps.e) return false;
    out = std::move(v);
    return true;
}

// A JSON string literal body (without the quotes) for UTF-8 text.
inline std::string JsonEsc(const std::string& s) {
    static const char* x = "0123456789abcdef";
    std::string o;
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c == '"') o += "\\\""; else if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n"; else if (c == '\r') o += "\\r"; else if (c == '\t') o += "\\t";
        else if (c < 0x20) { o += "\\u00"; o += x[c >> 4]; o += x[c & 15]; }
        else o += ch;
    }
    return o;
}

}  // namespace ak
