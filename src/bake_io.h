#pragma once
// The baked scene's binary coder (scene_bake.cpp, anim_bake.cpp): little-endian PODs, length-prefixed strings and
// vectors, and a reader that turns every short or damaged read into `bad` instead of reading past the end.
#include "core/types.h"

#include <cstring>
#include <string>
#include <vector>

namespace bakeio {

struct Writer {
    std::vector<u8>& out;
    template <typename T> void pod(const T& v) { const u8* p = reinterpret_cast<const u8*>(&v); out.insert(out.end(), p, p + sizeof(T)); }
    void u(u32 v) { pod(v); }
    void b(bool v) { pod(static_cast<u8>(v)); }
    void f(float v) { pod(v); }
    void fa(const float* a, int n) { for (int i = 0; i < n; ++i) f(a[i]); }
    void str(const std::string& s) { u(static_cast<u32>(s.size())); out.insert(out.end(), s.begin(), s.end()); }
    template <typename T> void vec(const std::vector<T>& v) {
        u(static_cast<u32>(v.size()));
        if (!v.empty()) { const u8* p = reinterpret_cast<const u8*>(v.data()); out.insert(out.end(), p, p + v.size() * sizeof(T)); }
    }
};

struct Reader {
    const u8* p;
    const u8* end;
    bool bad = false;
    bool need(size_t n) { if (bad || static_cast<size_t>(end - p) < n) { bad = true; return false; } return true; }
    template <typename T> T pod() { T v{}; if (need(sizeof(T))) { std::memcpy(&v, p, sizeof(T)); p += sizeof(T); } return v; }
    u32 u() { return pod<u32>(); }
    bool b() { return pod<u8>() != 0; }
    float f() { return pod<float>(); }
    void fa(float* a, int n) { for (int i = 0; i < n; ++i) a[i] = f(); }
    std::string str() { const u32 n = u(); std::string s; if (need(n)) { s.assign(reinterpret_cast<const char*>(p), n); p += n; } return s; }
    // An element count, checked against what is left (each element takes at least `minBytes`): a damaged count
    // must not size a vector of billions before the reads run out.
    u32 count(size_t minBytes) {
        const u32 n = u();
        if (bad || static_cast<size_t>(end - p) / minBytes < n) { bad = true; return 0; }
        return n;
    }
    template <typename T> std::vector<T> vec() {
        const u32 n = u();
        std::vector<T> v;
        if (n && need(static_cast<size_t>(n) * sizeof(T))) { v.resize(n); std::memcpy(v.data(), p, static_cast<size_t>(n) * sizeof(T)); p += static_cast<size_t>(n) * sizeof(T); }
        return v;
    }
};

}  // namespace bakeio
