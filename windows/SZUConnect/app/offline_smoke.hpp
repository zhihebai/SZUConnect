#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "runtime.hpp"
#include "canvas.hpp"
#include "smoke_options.hpp"
namespace szu::app {
// Finite, internal-only capture names. Existing labels remain stable for older tooling.
inline constexpr std::array<const char*, 24> kSmokeFrameLabels{
    "connection", "records", "settings", "settings-detail",
    "connection-wide", "records-wide", "settings-wide", "connection-detail",
    "connection-dark", "settings-dark", "connection-text150", "text-readability",
    "about", "about-detail", "about-text150",
    "help", "help-expanded", "help-detail", "help-text150",
    "network", "network-text150", "records-detail", "result-chips", "result-chips-text150"};
inline constexpr std::size_t kSmokeBitmapBudgetBytes = 48 * 1024 * 1024;
inline constexpr std::size_t kSmokeBitmapHeaderBytes = 54;
struct SmokeFrame {
    std::string page;
    unsigned width = 0, height = 0;
    float client_width_dip = 0, client_height_dip = 0, text_scale = 1;
    bool dark = false;
    bool app_icon_painted = false;
    std::string navigation_tier;
    int branding_pixel_samples = 0;
    TextRenderingInfo text_rendering;
    int selection_samples = 0;
    bool selection_check_applicable = true;
    int about_links_checked = 0;
    int header_edge_samples = 0;
    std::vector<std::uint8_t> bmp;
};
struct SmokeResult {
    bool window_created = false, canvas_initialized = false, clean_shutdown = false, fake_auth_passed = false;
    int render_frames = 0, scene_widgets = 0, checked_layouts = 0, about_links_checked = 0;
    int fixed_sheet_headers_checked = 0, help_layouts_checked = 0, records_layouts_checked = 0, network_layouts_checked = 0;
    std::array<int, 3> navigation_tiers_checked{};  // text, icons, brand
    int section_title_spacing_checked = 0;
    int result_chip_layouts_checked = 0, stacked_footer_layouts_checked = 0;
    int interaction_checks = 0;
    int window_presentations_checked = 0, hidden_frames_prepared = 0;
    int background_change_messages = 0, background_model_updates = 0, background_latest_presentations = 0;
    unsigned background_change_rebuilds = 0;
    int background_uia_blocks = 0, background_uia_visible_invocations = 0;
    int caption_feedback_checks = 0;
    unsigned caption_feedback_rebuilds = 0;
    double caption_feedback_microseconds = 0;
    double caption_feedback_cpu_microseconds = 0;
    std::array<int, 4> configured_dpi_layouts_checked{};  // native render targets: 120, 144, 168, 192 DPI
    std::vector<std::string> native_painted_pages;
    std::vector<SmokeFrame> visual_frames;
    std::string visual_unavailable;
    std::string failure;
};
class SmokeReceipt {
public:
    explicit SmokeReceipt(const std::wstring& path);
    ~SmokeReceipt();
    bool valid() const { return file_ != INVALID_HANDLE_VALUE; }
    const std::string& error() const { return error_; }
    bool finish(const SmokeResult& result, const RuntimeEvidence* evidence);
private:
    HANDLE file_ = INVALID_HANDLE_VALUE;
    std::vector<HANDLE> parents_;
    std::wstring frame_prefix_;
    std::string error_;
};
}  // namespace szu::app
