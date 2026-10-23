#include "offline_smoke.hpp"
#include "shared_tokens.hpp"
#include "controller.hpp"
#include "szu/json.hpp"
#include <algorithm>
#include <cmath>
namespace szu::app {
SmokeReceipt::SmokeReceipt(const std::wstring& path) {
    // Atomic create-new rejects existing formal files and avoids a check/truncate race.
    if (!absolute_receipt_path(path)) { error_ = "invalidReceiptPath"; return; }
    const std::wstring root = path.substr(0, 2) + L"\\";
    const UINT drive = GetDriveTypeW(root.c_str());
    if (drive != DRIVE_FIXED && drive != DRIVE_REMOVABLE && drive != DRIVE_RAMDISK) { error_ = "nonLocalDrive"; return; }
    std::vector<wchar_t> buffer(32768, L'\0');
    const DWORD length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (!length || length >= buffer.size()) { error_ = "canonicalizationFailed"; return; }
    std::wstring normalized(buffer.data(), length);
    std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
    if (!absolute_receipt_path(normalized)) { error_ = "invalidReceiptPath"; return; }
    // Open each parent without following that component's reparse point. Earlier parents
    // stay pinned without FILE_SHARE_DELETE, so they cannot be renamed/replaced between checks.
    std::size_t boundary = 2;
    while (boundary != std::wstring::npos) {
        const std::wstring parent = boundary == 2 ? normalized.substr(0, 3) : normalized.substr(0, boundary);
        if (parents_.size() >= 128) { error_ = "tooManyParents"; return; }
        HANDLE folder = CreateFileW(parent.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (folder == INVALID_HANDLE_VALUE) { error_ = "parentPathUnusable"; return; }
        parents_.push_back(folder);
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(folder, &info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { error_ = "parentPathUnusable"; return; }
        if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { error_ = "reparseParent"; return; }
        boundary = normalized.find(L'\\', boundary + 1);
    }
    file_ = CreateFileW(normalized.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (!valid()) error_ = "receiptExistsOrUnwritable";
    else frame_prefix_ = normalized + L".ui-";
}
SmokeReceipt::~SmokeReceipt() { if (valid()) CloseHandle(file_); for (HANDLE parent : parents_) CloseHandle(parent); }
bool SmokeReceipt::finish(const SmokeResult& r, const RuntimeEvidence* e) {
    if (!valid()) return false;
    const auto count = [](const std::atomic<int>& v) { return static_cast<double>(v.load()); };
    const bool isolated = e && e->formal_services_created == 0 && e->production_storage_accesses == 0 && e->vault_accesses == 0 &&
        e->network_calls == 0 && e->wifi_calls == 0 && e->watchers_started == 0 && e->autostart_writes == 0;
    const bool passed = r.failure.empty() && isolated && r.window_created && r.canvas_initialized && r.clean_shutdown &&
        r.fake_auth_passed && r.native_painted_pages.size() == 3 && r.render_frames >= 3 && r.scene_widgets > 0 && r.interaction_checks == 65 &&
        r.window_presentations_checked == 6 && r.hidden_frames_prepared >= 5 &&
        r.background_change_messages == 40 && r.background_change_rebuilds == 0 &&
        r.background_model_updates == 4 && r.background_latest_presentations == 4 &&
        r.background_uia_blocks == 4 && r.background_uia_visible_invocations == 4 &&
        r.caption_feedback_checks == 200 && r.caption_feedback_rebuilds == 0 &&
        std::all_of(r.configured_dpi_layouts_checked.begin(), r.configured_dpi_layouts_checked.end(), [](int count) { return count == 12; });
    json::Value root = json::Value::object();
    root.set("schemaVersion", json::Value::number(2)); root.set("version", json::Value::string(kVersion));
    root.set("build", json::Value::string(kBuild)); root.set("mode", json::Value::string("offlineSmoke"));
    root.set("passed", json::Value::boolean(passed));
    root.set("failure", json::Value::string(r.failure.empty() && !passed ? "receiptChecksIncomplete" : r.failure));
    json::Value pages = json::Value::array();
    for (const auto& page : r.native_painted_pages) pages.push(json::Value::string(page));
    root.set("nativePaintedPages", std::move(pages)); root.set("fakeAuthPassed", json::Value::boolean(r.fake_auth_passed));
    root.set("formalServicesCreated", json::Value::number(e ? count(e->formal_services_created) : 0));
    json::Value c = json::Value::object();
    c.set("windowCreated", json::Value::boolean(r.window_created)); c.set("canvasInitialized", json::Value::boolean(r.canvas_initialized));
    c.set("cleanShutdown", json::Value::boolean(r.clean_shutdown)); c.set("renderFrames", json::Value::number(r.render_frames));
    c.set("sceneWidgets", json::Value::number(r.scene_widgets));
    c.set("checkedLayouts", json::Value::number(r.checked_layouts));
    c.set("aboutLinksChecked", json::Value::number(r.about_links_checked));
    c.set("fixedSheetHeadersChecked", json::Value::number(r.fixed_sheet_headers_checked));
    c.set("helpLayoutsChecked", json::Value::number(r.help_layouts_checked));
    c.set("recordsLayoutsChecked", json::Value::number(r.records_layouts_checked));
    c.set("networkLayoutsChecked", json::Value::number(r.network_layouts_checked));
    c.set("navigationTextLayoutsChecked", json::Value::number(r.navigation_tiers_checked[0]));
    c.set("navigationIconLayoutsChecked", json::Value::number(r.navigation_tiers_checked[1]));
    c.set("navigationBrandLayoutsChecked", json::Value::number(r.navigation_tiers_checked[2]));
    c.set("sectionTitleSpacingChecked", json::Value::number(r.section_title_spacing_checked));
    c.set("resultChipLayoutsChecked", json::Value::number(r.result_chip_layouts_checked));
    c.set("stackedFooterLayoutsChecked", json::Value::number(r.stacked_footer_layouts_checked));
    c.set("interactionChecks", json::Value::number(r.interaction_checks));
    c.set("windowPresentationsChecked", json::Value::number(r.window_presentations_checked));
    c.set("hiddenFramesPrepared", json::Value::number(r.hidden_frames_prepared));
    c.set("backgroundChangeMessages", json::Value::number(r.background_change_messages));
    c.set("backgroundChangeRebuilds", json::Value::number(r.background_change_rebuilds));
    c.set("backgroundModelUpdates", json::Value::number(r.background_model_updates));
    c.set("backgroundLatestPresentations", json::Value::number(r.background_latest_presentations));
    c.set("backgroundUiaBlocks", json::Value::number(r.background_uia_blocks));
    c.set("backgroundUiaVisibleInvocations", json::Value::number(r.background_uia_visible_invocations));
    c.set("captionFeedbackChecks", json::Value::number(r.caption_feedback_checks));
    c.set("captionFeedbackRebuilds", json::Value::number(r.caption_feedback_rebuilds));
    c.set("captionFeedbackMicroseconds", json::Value::number(r.caption_feedback_microseconds));
    c.set("captionFeedbackCpuMicroseconds", json::Value::number(r.caption_feedback_cpu_microseconds));
    c.set("fakeProbeCalls", json::Value::number(e ? count(e->fake_probe_calls) : 0));
    c.set("fakeAuthSubmissions", json::Value::number(e ? count(e->fake_auth_submissions) : 0)); root.set("checks", std::move(c));
    json::Value i = json::Value::object(); i.set("passed", json::Value::boolean(isolated));
    i.set("productionStorageAccesses", json::Value::number(e ? count(e->production_storage_accesses) : 0));
    i.set("vaultAccesses", json::Value::number(e ? count(e->vault_accesses) : 0)); i.set("networkCalls", json::Value::number(e ? count(e->network_calls) : 0));
    i.set("wifiCalls", json::Value::number(e ? count(e->wifi_calls) : 0));
    i.set("watchersStarted", json::Value::number(e ? count(e->watchers_started) : 0));
    i.set("autostartWrites", json::Value::number(e ? count(e->autostart_writes) : 0)); root.set("isolation", std::move(i));
    // Visual fixtures never substitute for native paint or relax the isolation gates.
    // Every allow-listed internal scene must be available for this layout verification to pass.
    json::Value visual = json::Value::object(), frames = json::Value::array(), text_profiles = json::Value::array();
    std::vector<std::string> text_profile_keys;
    std::string unavailable = r.visual_unavailable;
    std::size_t total = 0;
    std::vector<std::string> seen;
    if (isolated && r.visual_frames.size() <= kSmokeFrameLabels.size()) for (const auto& frame : r.visual_frames) {
        const bool label = std::any_of(kSmokeFrameLabels.begin(), kSmokeFrameLabels.end(),
            [&frame](const char* allowed) { return frame.page == allowed; });
        const bool about_label = frame.page == "about" || frame.page == "about-detail" || frame.page == "about-text150";
        const bool help_label = frame.page == "help" || frame.page == "help-expanded" || frame.page == "help-detail" || frame.page == "help-text150";
        const bool selection_checked = about_label || help_label ? !frame.selection_check_applicable && frame.selection_samples == 0 &&
            frame.about_links_checked == (about_label ? 1 : 0) :
            frame.selection_check_applicable && frame.selection_samples > 0 && frame.about_links_checked == 0;
        const bool branding_checked = frame.branding_pixel_samples >= 12 && frame.navigation_tier == "brand";
        const auto size = static_cast<std::size_t>(frame.width) * frame.height * 4 + 54;
        const bool bounded = label && frame.width >= 64 && frame.height >= 64 && frame.width <= 2048 && frame.height <= 2048 &&
            frame.bmp.size() == size && frame.bmp[0] == 'B' && frame.bmp[1] == 'M' && total + size <= kSmokeBitmapBudgetBytes &&
            std::isfinite(frame.client_width_dip) && std::isfinite(frame.client_height_dip) &&
            frame.client_width_dip >= 64 && frame.client_height_dip >= 64 &&
            (frame.text_scale == 1.0f || frame.text_scale == 1.5f) &&
            (frame.text_rendering.antialias_mode == "cleartype" || frame.text_rendering.antialias_mode == "grayscale") &&
            selection_checked && frame.header_edge_samples == 9 && branding_checked && frame.text_rendering.target_opaque && frame.text_rendering.custom_parameters &&
            frame.text_rendering.rendering_mode == "automatic" && frame.text_rendering.follows_system_policy &&
            std::find(seen.begin(), seen.end(), frame.page) == seen.end();
        if (!bounded) { if (unavailable.empty()) unavailable = "invalidCaptureFrame"; continue; }
        total += size; seen.push_back(frame.page);
        const auto path = frame_prefix_ + win::widen(frame.page) + L".bmp";
        HANDLE image = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (image == INVALID_HANDLE_VALUE) { if (unavailable.empty()) unavailable = "captureExistsOrUnwritable"; continue; }
        DWORD bytes = 0;
        const bool saved = WriteFile(image, frame.bmp.data(), static_cast<DWORD>(frame.bmp.size()), &bytes, nullptr) &&
            bytes == frame.bmp.size() && FlushFileBuffers(image);
        CloseHandle(image);
        if (!saved) { if (unavailable.empty()) unavailable = "captureWriteFailed"; continue; }
        auto item = json::Value::object();
        const auto slash = path.find_last_of(L"\\");
        item.set("file", json::Value::string(win::narrow(path.substr(slash + 1))));
        item.set("page", json::Value::string(frame.page));
        item.set("width", json::Value::number(frame.width)); item.set("height", json::Value::number(frame.height));
        item.set("clientWidthDIP", json::Value::number(frame.client_width_dip));
        item.set("clientHeightDIP", json::Value::number(frame.client_height_dip));
        item.set("textScale", json::Value::number(frame.text_scale));
        item.set("dark", json::Value::boolean(frame.dark));
        item.set("selectionPixelSamples", json::Value::number(frame.selection_samples));
        item.set("selectionPixelCheck", json::Value::string(frame.selection_check_applicable ? "sampled" : "notApplicableModal"));
        item.set("aboutLinksChecked", json::Value::number(frame.about_links_checked));
        item.set("headerEdgePixelSamples", json::Value::number(frame.header_edge_samples));
        item.set("appIconPainted", json::Value::boolean(frame.app_icon_painted));
        item.set("navigationTier", json::Value::string(frame.navigation_tier));
        item.set("brandingPixelSamples", json::Value::number(frame.branding_pixel_samples));
        json::Value text = json::Value::object();
        const auto& rendering = frame.text_rendering;
        text.set("antialiasMode", json::Value::string(rendering.antialias_mode));
        text.set("renderingMode", json::Value::string(rendering.rendering_mode));
        text.set("pixelGeometry", json::Value::string(rendering.pixel_geometry));
        text.set("fontFamily", json::Value::string(win::narrow(rendering.font_family)));
        text.set("primaryFontHasCjk", json::Value::boolean(rendering.primary_font_has_cjk));
        text.set("dpi", json::Value::number(rendering.dpi));
        text.set("gamma", json::Value::number(rendering.gamma));
        text.set("enhancedContrast", json::Value::number(rendering.enhanced_contrast));
        text.set("clearTypeLevel", json::Value::number(rendering.cleartype_level));
        text.set("opaqueTarget", json::Value::boolean(rendering.target_opaque));
        text.set("monitorParameters", json::Value::boolean(rendering.monitor_parameters));
        text.set("customParameters", json::Value::boolean(rendering.custom_parameters));
        // Reuse only byte-identical complete objects. Each actual frame above still
        // passes the original rendering checks before its profile can be referenced.
        const auto profile_key = json::serialize(text);
        const auto profile = std::find(text_profile_keys.begin(), text_profile_keys.end(), profile_key);
        const auto profile_index = static_cast<std::size_t>(profile - text_profile_keys.begin());
        if (profile == text_profile_keys.end()) {
            text_profile_keys.push_back(profile_key);
            text_profiles.push(std::move(text));
        }
        item.set("textRenderingProfile", json::Value::number(static_cast<double>(profile_index)));
        item.set("bytes", json::Value::number(static_cast<double>(frame.bmp.size()))); frames.push(item);
    }
    if (frames.size() != kSmokeFrameLabels.size() && unavailable.empty()) unavailable = isolated ? "captureIncomplete" : "captureIsolationUnavailable";
    const bool visual_passed = unavailable.empty() && frames.size() == kSmokeFrameLabels.size() &&
        r.checked_layouts == static_cast<int>(kSmokeFrameLabels.size()) && r.about_links_checked == 3 &&
        r.fixed_sheet_headers_checked == 7 && r.help_layouts_checked == 4 && r.records_layouts_checked == 3 && r.network_layouts_checked == 7 &&
        r.navigation_tiers_checked[0] > 0 && r.navigation_tiers_checked[1] > 0 &&
        r.navigation_tiers_checked[2] == static_cast<int>(kSmokeFrameLabels.size()) &&
        r.navigation_tiers_checked[0] + r.navigation_tiers_checked[1] == static_cast<int>(kSmokeFrameLabels.size()) &&
        r.section_title_spacing_checked == 21 && r.result_chip_layouts_checked == 4 && r.stacked_footer_layouts_checked == 2;
    visual.set("available", json::Value::boolean(visual_passed));
    visual.set("expectedFrames", json::Value::number(static_cast<double>(kSmokeFrameLabels.size())));
    visual.set("defaultClientWidthDIP", json::Value::number(tokens::kSizeWindowsDefaultClientWidth));
    visual.set("preferredClientHeightDIP", json::Value::number(tokens::kSizeWindowsDefaultClientHeight));
    visual.set("textScaleMeaning", json::Value::string("accessibilityTextSizeOnlyNotDisplayDPI"));
    visual.set("textRenderingProfiles", std::move(text_profiles));
    visual.set("unavailable", json::Value::string(unavailable)); visual.set("frames", std::move(frames));
    auto dpi_matrix = json::Value::object(), dpi_cases = json::Value::array();
    const int render_dpis[]{120, 144, 168, 192};
    for (std::size_t index = 0; index < r.configured_dpi_layouts_checked.size(); ++index) {
        auto item = json::Value::object();
        item.set("dpi", json::Value::number(render_dpis[index]));
        item.set("displayPercent", json::Value::number(render_dpis[index] * 100 / 96));
        item.set("nativePaintedLayouts", json::Value::number(r.configured_dpi_layouts_checked[index]));
        dpi_cases.push(std::move(item));
    }
    dpi_matrix.set("method", json::Value::string("configuredNativeD2DDirectWriteTarget"));
    dpi_matrix.set("clientWidthDIP", json::Value::number(480));
    json::Value client_widths = json::Value::array();
    client_widths.push(json::Value::number(480)); client_widths.push(json::Value::number(500));
    dpi_matrix.set("clientWidthsDIP", std::move(client_widths));
    auto text_scales = json::Value::array(); text_scales.push(json::Value::number(1)); text_scales.push(json::Value::number(1.5));
    dpi_matrix.set("textScales", std::move(text_scales));
    dpi_matrix.set("liveMonitorSwitchTested", json::Value::boolean(false));
    dpi_matrix.set("pixelsInspected", json::Value::boolean(false));
    dpi_matrix.set("cases", std::move(dpi_cases));
    visual.set("configuredDpiMatrix", std::move(dpi_matrix));
    root.set("visualQA", std::move(visual));
    root.set("passed", json::Value::boolean(passed && visual_passed));
    auto data = json::serialize(root, true);
    if (data.size() > 16384) data = json::serialize(root, false);
    DWORD written = 0;
    const bool written_ok = data.size() <= 16384 && WriteFile(file_, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
        written == data.size() && FlushFileBuffers(file_);
    return written_ok && passed && visual_passed;
}
}  // namespace szu::app
