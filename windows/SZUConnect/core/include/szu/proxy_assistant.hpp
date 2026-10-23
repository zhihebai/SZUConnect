#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace szu::proxyassist {
enum class Format { clashVergeScript, mihomoYaml, singBoxJson };
inline constexpr std::size_t kMaxConfigBytes = 1024 * 1024;
struct Patch {
    std::string text;                 // RAM-only configuration; never diagnostics or events.
    std::vector<std::string> changes;  // Fixed copy-key suffixes, never values from the file.
    std::string error;                // Fixed error token; no parser/system text.
    bool changed = false;
    bool ok() const { return error.empty(); }
};
// These transforms perform no I/O and never evaluate supplied JavaScript.
std::string v2rayn_rules();
std::string clash_verge_script();
Patch patch_clash_verge_script(std::string_view text);
Patch patch_sing_box(std::string_view text);

struct Preview {
    Format format = Format::clashVergeScript;
    std::string filename;             // Display only; not included in exported reports.
    std::vector<std::string> changes;
    std::string error;
    bool ready = false, changed = false;
};
struct Result {
    std::string error;
    bool success = false, changed = false;
};
}  // namespace szu::proxyassist
