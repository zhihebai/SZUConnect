// UTF-8 <-> UTF-16 at the Win32 boundary. Core code is UTF-8 everywhere.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>
#include <string_view>

namespace szu::win {

inline std::wstring widen(std::string_view text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(length > 0 ? length : 0), L'\0');
    if (length > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
    return out;
}

inline std::string narrow(std::wstring_view text) {
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(length > 0 ? length : 0), '\0');
    if (length > 0) WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length, nullptr, nullptr);
    return out;
}

// Overwrites a secret before its storage is released.
inline void wipe(std::string& secret) {
    if (!secret.empty()) SecureZeroMemory(secret.data(), secret.size());
    secret.clear();
}
inline void wipe(std::wstring& secret) {
    if (!secret.empty()) SecureZeroMemory(secret.data(), secret.size() * sizeof(wchar_t));
    secret.clear();
}

}  // namespace szu::win
