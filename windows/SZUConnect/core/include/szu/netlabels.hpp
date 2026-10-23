// Local-only names, separate from NetworkMemory eligibility and diagnostic data.
#pragma once
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace szu {
struct NetworkLabel {
    std::string display_name;
    std::string kind = "other";
    std::optional<std::string> ssid;
    bool manual_name = false;
    bool operator==(const NetworkLabel&) const = default;
};
class NetworkLabels {
public:
    static std::string clean_name(const std::string& text);
    static std::optional<std::string> valid_ssid(const std::optional<std::string>& text);
    static std::string fallback(const std::string& kind = "other");
    std::string display_name(const std::string& id) const;
    bool update_automatic(const std::string& id, const NetworkLabel& observation);
    bool rename(const std::string& id, const std::string& name);
    enum class MergeResult { unchanged, updated, conflict, unavailable };
    MergeResult preserve_merged_metadata(const std::string& survivor, const std::vector<std::string>& sources);
    bool retain(const std::set<std::string>& ids);
    std::string encode() const;
    static std::optional<NetworkLabels> decode(const std::string& data);
    const std::map<std::string, NetworkLabel>& entries() const { return entries_; }
private:
    std::map<std::string, NetworkLabel> entries_;
};
}  // namespace szu
