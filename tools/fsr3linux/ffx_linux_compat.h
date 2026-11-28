// FSR 3 for Linux (s131): force-included (-include) into every FidelityFX SDK v1.1.4 source file the Linux build
// compiles (CMakeLists.txt, target amd_fidelityfx_vk). The SDK is written for Windows; these are the few MSVC CRT /
// Win32 names it uses, with the same meaning. AMD's files are compiled unchanged.
#pragma once
#ifndef _WIN32
#include <cstddef>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cwchar>

#define __declspec(x)
#define __debugbreak() __builtin_trap()
#define _countof(a) (sizeof(a) / sizeof((a)[0]))

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
inline int sprintf_s(char* b, size_t n, const char* f, ...) {
    va_list a;
    va_start(a, f);
    const int r = vsnprintf(b, n, f, a);
    va_end(a);
    return r;
}
inline int swprintf_s(wchar_t* b, size_t n, const wchar_t* f, ...) {
    va_list a;
    va_start(a, f);
    const int r = vswprintf(b, n, f, a);
    va_end(a);
    return r;
}
template <size_t N> inline int swprintf_s(wchar_t (&b)[N], const wchar_t* f, ...) {
    va_list a;
    va_start(a, f);
    const int r = vswprintf(b, N, f, a);
    va_end(a);
    return r;
}
// debug output: the SDK's asserts / messages print to the debugger on Windows; stderr here
inline void OutputDebugStringA(const char* s) { fputs(s, stderr); }
inline void OutputDebugStringW(const wchar_t* s) { fprintf(stderr, "%ls", s); }
#endif
