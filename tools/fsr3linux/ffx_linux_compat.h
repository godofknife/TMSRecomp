// FSR 3 for Linux (s131): force-included (-include) into every FidelityFX SDK v1.1.4 source file the Linux build
// compiles (CMakeLists.txt, target amd_fidelityfx_vk). The SDK is written for Windows; these are the MSVC CRT / Win32
// names it uses, with the same meaning. AMD's files are compiled unchanged.
// The library is built with -fshort-wchar: wchar_t is 2 bytes as on Windows, so the SDK's structs keep their Windows
// sizes (its context-size static_asserts count on it). The C library's wide functions assume 4-byte wchar_t, so the
// ones the SDK calls are redirected to 2-byte versions here. wchar_t never crosses the API to tms except the debug
// message callback (vk_fsr3.cpp reads it as 16-bit).
#pragma once
#ifndef _WIN32
#include <cstddef>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cmath>
#include <new>
#include <string>
#include <vector>

#define __declspec(x)
#define __debugbreak() __builtin_trap()
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#ifndef FFX_UNUSED
#define FFX_UNUSED(x) ((void)(x))
#endif

inline int nx_wcscmp(const wchar_t* a, const wchar_t* b) {
    for (; *a && *a == *b; a++, b++) {}
    return (int)(unsigned)*a - (int)(unsigned)*b;
}
#define wcscmp nx_wcscmp

// *_s copies: always terminate, truncate to the destination (the SDK only copies short debug names)
inline int wcscpy_s(wchar_t* d, size_t n, const wchar_t* s) {
    if (!d || !n) return 1;
    size_t i = 0;
    for (; s && s[i] && i + 1 < n; i++) d[i] = s[i];
    d[i] = 0;
    return 0;
}
template <size_t N> inline int wcscpy_s(wchar_t (&d)[N], const wchar_t* s) { return wcscpy_s(d, N, s); }
inline int strcpy_s(char* d, size_t n, const char* s) {
    if (!d || !n) return 1;
    size_t i = 0;
    for (; s && s[i] && i + 1 < n; i++) d[i] = s[i];
    d[i] = 0;
    return 0;
}
template <size_t N> inline int strcpy_s(char (&d)[N], const char* s) { return strcpy_s(d, N, s); }
// wide -> narrow debug names (ASCII; anything else becomes '?')
inline int wcstombs_s(size_t* done, char* d, size_t n, const wchar_t* s, size_t) {
    size_t i = 0;
    if (d && n) {
        for (; s && s[i] && i + 1 < n; i++) d[i] = s[i] < 128 ? (char)s[i] : '?';
        d[i] = 0;
    }
    if (done) *done = i + 1;
    return 0;
}
inline int sprintf_s(char* b, size_t n, const char* f, ...) {
    va_list a;
    va_start(a, f);
    const int r = vsnprintf(b, n, f, a);
    va_end(a);
    return r;
}
// the SDK's only wide format strings are "<text>%ls\n" (ffx_message.cpp): %ls / %s take a wide string, %% is '%'
inline int swprintf_s(wchar_t* b, size_t n, const wchar_t* f, ...) {
    va_list a;
    va_start(a, f);
    size_t o = 0;
    auto put = [&](wchar_t c) { if (o + 1 < n) b[o++] = c; };
    for (; *f; f++) {
        if (*f != L'%') { put(*f); continue; }
        if (f[1] == L'%') { put(L'%'), f++; continue; }
        const wchar_t* p = f + 1;
        if (*p == L'l') p++;
        if (*p == L's') {
            for (const wchar_t* s = va_arg(a, const wchar_t*); s && *s; s++) put(*s);
            f = p;
        } else put(*f);
    }
    if (n) b[o] = 0;
    va_end(a);
    return (int)o;
}
// debug output: the SDK's asserts print to the debugger on Windows; stderr here
inline void OutputDebugStringA(const char* s) { fputs(s, stderr); }
inline void OutputDebugStringW(const wchar_t* s) {
    for (; s && *s; s++) fputc(*s < 128 ? (char)*s : '?', stderr);
}

#ifdef NX_FFX_VK_TU
// ffx_vk.cpp converts debug names with std::wstring_convert (in <locale>, which it does not include). libstdc++'s
// compiled wide-string code assumes 4-byte wchar_t, so this TU gets a header-only stand-in with the two calls it
// makes (from_bytes / to_bytes, ASCII names).
namespace std {
template <class Codecvt, class Elem> struct wstring_convert {
    struct wide {
        std::vector<Elem> v;
        const Elem* c_str() const { return v.data(); }
    };
    wide from_bytes(const char* s) const {
        wide w;
        for (; s && *s; s++) w.v.push_back((Elem)(unsigned char)*s);
        w.v.push_back(0);
        return w;
    }
    std::string to_bytes(const Elem* s) const {
        std::string r;
        for (; s && *s; s++) r += *s < 128 ? (char)*s : '?';
        return r;
    }
};
}  // namespace std
#endif
#endif
