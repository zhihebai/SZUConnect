#pragma once
#include <array>
#include <string>
#include <utility>
#include "installation.hpp"

namespace szu::app {
struct LicenseDocument {
    const char* id;
    const wchar_t* title;
    std::string text;
};

inline std::string embedded_license_document(HINSTANCE instance, int id) {
    const HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(id), RT_RCDATA);
    const DWORD size = resource ? SizeofResource(instance, resource) : 0;
    const HGLOBAL loaded = resource ? LoadResource(instance, resource) : nullptr;
    const void* bytes = loaded ? LockResource(loaded) : nullptr;
    if (!bytes || !size || size > 1024 * 1024) return {};
    return {static_cast<const char*>(bytes), size};
}

inline const std::array<LicenseDocument, 4>& license_documents() {
    static const auto documents = [] {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        auto notices = embedded_license_document(instance, kInstallNoticesResource);
        const auto start = notices.find("MIT License");
        // Preserve the bundled upstream notice and its full copyright lines.
        const auto yaml = notices.find("=== yaml-cpp ===");
        auto upstream = start == std::string::npos ? std::string{} : notices.substr(start, yaml == std::string::npos ? std::string::npos : yaml - start);
        const auto yaml_start = yaml == std::string::npos ? std::string::npos : notices.find("Copyright (c) 2008-2015 Jesse Beder.", yaml);
        auto yaml_license = yaml_start == std::string::npos ? std::string{} : notices.substr(yaml_start);
        return std::array<LicenseDocument, 4>{{
            {"project", L"SZUConnect · MIT", embedded_license_document(instance, kInstallLicenseResource)},
            {"autoszuweb", L"AutoSZUWeb · MIT", std::move(upstream)},
            {"curl", L"libcurl · curl", embedded_license_document(instance, kInstallCurlResource)},
            {"yamlcpp", L"yaml-cpp · MIT", std::move(yaml_license)}
        }};
    }();
    return documents;
}
} // namespace szu::app
