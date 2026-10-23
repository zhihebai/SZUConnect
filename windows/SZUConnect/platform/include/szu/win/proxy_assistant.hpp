#pragma once
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "szu/proxy_assistant.hpp"

namespace szu::win {

// Only explicitly selected files are accessed. No client discovery, network calls or reloads.
class ProxyConfigTransaction {
public:
    using Transform = std::function<proxyassist::Patch(std::string_view)>;
    struct Hooks { std::function<void()> before_backup_create; };
    explicit ProxyConfigTransaction(std::wstring data_root, Hooks hooks = {});
    ~ProxyConfigTransaction();
    ProxyConfigTransaction(const ProxyConfigTransaction&) = delete;
    ProxyConfigTransaction& operator=(const ProxyConfigTransaction&) = delete;

    proxyassist::Preview prepare(const std::wstring& path, proxyassist::Format format, const Transform& transform);
    proxyassist::Result apply();
    proxyassist::Result restore();
    proxyassist::Result discard_backup();
    void clear_preview();
    bool has_backup() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace szu::win
