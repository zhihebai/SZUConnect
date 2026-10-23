#include <algorithm>
#include <cmath>

#include "shared_copy.hpp"
#include "shared_tokens.hpp"
#include "szu/win/system.hpp"
#include "installation.hpp"
#include "license_documents.hpp"
#include "views.hpp"

namespace szu::app {
namespace {

std::wstring probe_result(const EndpointResult* result) {
    if (!result) return T("compat.result.notRun");
    if (result->succeeded()) {
        const double elapsed = std::isfinite(result->elapsed) ? std::clamp(result->elapsed, 0.0, 120.0) : 0;
        return F("endpoint.success", {{"ms", std::to_string(static_cast<long>(std::lround(elapsed * 1000)))}});
    }
    return T("endpoint." + std::string(to_string(result->state)));
}

void close_button(Builder& b, ViewContext& ctx) {
    b.gap(24);
    b.buttons({{"sheet.close", T("action.close"), ButtonStyle::secondary, [&ctx] { ctx.close(); }, "x"}});
}

}  // namespace

void build_home_section_header(Builder& b, ViewContext& ctx, const std::string& id, const std::string& icon,
    const std::wstring& title, const std::string& info_topic, bool expanded, bool collapsible,
    std::function<void()> toggle, const std::wstring& summary) {
    const float top = b.y();
    const float title_left = b.left() + 18 + 8;
    const float title_width = std::min(std::ceil(b.measure_text(title, TextStyle::headline, 10000).width) + 1,
        std::max(40.0f, b.width() - 18 - 8 - tokens::kSizeMinTarget + tokens::kSpaceXxs));
    const float info_left = std::min(b.left() + b.width() - tokens::kSizeMinTarget,
        title_left + title_width - tokens::kSpaceXxs);
    const float line = std::max(tokens::kSizeWindowsSectionTitleVisualHeight,
        b.measure_text(title, TextStyle::headline, title_width).height);
    if (collapsible) {
        Widget target; target.kind = Kind::panel; target.radius = 8;
        target.rect = D2D1::RectF(b.left(), top + (line - tokens::kSizeMinTarget) / 2,
            info_left, top + (line + tokens::kSizeMinTarget) / 2);
        target.id = id + ".disclosure"; target.name = title + L" · " + T(expanded ? "disclosure.expanded" : "disclosure.collapsed");
        target.role = Role::button; target.focusable = true; target.checked = expanded; target.action = std::move(toggle);
        b.add(std::move(target));
    }
    b.title_row(id, collapsible ? (expanded ? "chevron-down" : "chevron-right") : icon,
        title, TextStyle::headline, "textPrimary", info_label(info_topic),
        [&ctx, info_topic] { ctx.open("info:" + info_topic); }, {}, {}, {}, true);
    if (!summary.empty()) {
        b.gap(tokens::kSpaceS);
        b.text(summary, TextStyle::caption, "textSecondary", Role::text, id + ".summary");
    }
}

void build_inline_account(Builder& b, ViewContext& ctx, const EngineSnapshot& snap) {
    const bool collapsible = snap.has_saved && !snap.unsaved_changes;
    const bool expanded = ctx.ui.account_expanded || !collapsible;
    if (!expanded) ctx.ui.show_password = false;
    build_home_section_header(b, ctx, "home.account", "user", T("account.title"), "account", expanded, collapsible,
        [&ctx] { ctx.ui.account_expanded = !ctx.ui.account_expanded; ctx.ui.show_password = false; ctx.refresh(); },
        expanded ? std::wstring() : F("account.saved", {{"account", snap.saved_account_hint}}));
    if (!expanded) return;
    b.gap(tokens::kSpaceS);
    b.columns({
        [](Builder& col) { col.field("account.account", T("account.accountLabel"), L"", false, false); },
        [&ctx](Builder& col) {
            col.row({[&ctx](Builder& field) { field.field("account.password", T("account.passwordLabel"), L"", !ctx.ui.show_password, false); },
                     [&ctx](Builder& eye) { eye.icon_button("account.show", T(ctx.ui.show_password ? "action.hidePassword" : "action.showPassword"),
                         ctx.ui.show_password ? "eye-off" : "eye", [&ctx] { ctx.ui.show_password = !ctx.ui.show_password; ctx.refresh(); }); }},
                {0, tokens::kSizeMinTarget}, tokens::kSpaceXs, 128);
        }
    }, 150, tokens::kSpaceS);
    b.gap(tokens::kSpaceM);

    // Area selection and draft state share one responsive row.
    const auto account_state = F(snap.unsaved_changes ? "account.unsaved" : snap.has_saved ? "account.saved" : "account.notSaved",
        {{"store", U("app.store")}, {"account", snap.saved_account_hint}});
    const auto area_value = T("area." + std::string(to_string(snap.area)) + ".title");
    const float area_label_width = std::ceil(b.measure_text(T("account.areaTitle"), TextStyle::callout, 10000).width);
    const float area_picker_width = std::max(124.0f, std::ceil(b.measure_text(area_value, TextStyle::callout, 10000).width) + 30);
    const float area_height = std::max(tokens::kSizeMinTarget, b.measure_text(area_value, TextStyle::callout, area_picker_width - 30).height + 12);
    const float area_width = area_label_width + area_picker_width + tokens::kSizeMinTarget + tokens::kSpaceS * 2;
    std::vector<ButtonSpec> draft_actions;
    if (snap.unsaved_changes) {
        draft_actions.push_back({"account.discard", T("action.discard"), ButtonStyle::plain, [&ctx] {
            ctx.app.discard_draft(); ctx.ui.reload_account_inputs = true; ctx.ui.account_feedback.clear(); ctx.refresh();
        }, "", !ctx.app.read_only()});
        draft_actions.push_back({"account.save", T("action.saveAccount"), ButtonStyle::secondary, [&ctx] {
            switch (ctx.app.save_account()) {
                case ConnectionEngine::SaveResult::saved:
                case ConnectionEngine::SaveResult::unchanged: ctx.ui.account_feedback = "account.savedDone"; break;
                case ConnectionEngine::SaveResult::invalid: ctx.ui.account_feedback = "account.invalid"; break;
                case ConnectionEngine::SaveResult::failed: ctx.ui.account_feedback = "account.saveFailed"; break;
            }
            ctx.refresh();
        }, "", !ctx.app.read_only() && snap.draft_complete});
    }
    float draft_width = b.measure_text(account_state, TextStyle::caption, 10000).width;
    for (const auto& action : draft_actions) draft_width += tokens::kSpaceS + b.measure_button(action.label, action.icon);
    b.row({[&ctx, &snap, area_label_width, area_picker_width, area_height, area_value](Builder& area) {
        area.row({[area_height](Builder& label) {
            const auto title = T("account.areaTitle");
            label.gap(std::max(0.0f, (area_height - label.measure_text(title, TextStyle::callout, label.width()).height) / 2));
            label.text(title, TextStyle::callout, "textSecondary");
        }, [&ctx, &snap, area_value](Builder& selector) {
            selector.menu_button("guard.area", area_value, T("account.areaTitle") + L"：" + area_value, [&ctx, selected = snap.area] {
                const CampusArea areas[]{CampusArea::automatic, CampusArea::teaching, CampusArea::dormitory};
                HMENU menu = CreatePopupMenu(); if (!menu) return;
                UINT checked = 1;
                for (UINT i = 0; i < 3; ++i) {
                    AppendMenuW(menu, MF_STRING, i + 1, T("area." + std::string(to_string(areas[i])) + ".title").c_str());
                    if (areas[i] == selected) checked = i + 1;
                }
                CheckMenuRadioItem(menu, 1, 3, checked, MF_BYCOMMAND);
                POINT point{};
                if (!GetCursorPos(&point)) { RECT frame{}; GetWindowRect(ctx.hwnd, &frame); point = {frame.left + 80, frame.top + 160}; }
                SetForegroundWindow(ctx.hwnd);
                const UINT result = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, point.x, point.y, 0, ctx.hwnd, nullptr);
                DestroyMenu(menu);
                if (result >= 1 && result <= 3) { ctx.app.set_area(areas[result - 1]); ctx.refresh(); }
            }, !ctx.app.read_only() && !snap.busy);
        }, [&ctx, area_height](Builder& info) {
            info.gap((area_height - tokens::kSizeMinTarget) / 2);
            info.icon_button("guard.area.info", info_label("area"), "info", [&ctx] { ctx.open("info:area"); });
        }}, {area_label_width, area_picker_width, tokens::kSizeMinTarget}, tokens::kSpaceS, 1);
    }, [account_state, unsaved = snap.unsaved_changes, draft_actions](Builder& state) {
        if (draft_actions.empty()) {
            state.gap(std::max(0.0f, (tokens::kSizeMinTarget - state.measure_text(account_state, TextStyle::caption, state.width()).height) / 2));
            state.text(account_state, TextStyle::caption, "textSecondary");
        } else {
            float actions_width = 0;
            for (const auto& action : draft_actions) actions_width += state.measure_button(action.label, action.icon);
            actions_width += tokens::kSpaceS * static_cast<float>(draft_actions.size() - 1);
            const float status_width = state.measure_text(account_state, TextStyle::caption, 10000).width;
            if (status_width + tokens::kSpaceS + actions_width <= state.width()) {
                state.row({[account_state, unsaved](Builder& label) {
                    label.gap(std::max(0.0f, (tokens::kSizeMinTarget - label.measure_text(account_state, TextStyle::caption, label.width()).height) / 2));
                    label.text(account_state, TextStyle::caption, unsaved ? "warning" : "textSecondary");
                }, [draft_actions](Builder& actions) { actions.buttons(draft_actions); }}, {0, actions_width}, tokens::kSpaceS, 1);
            } else {
                state.text(account_state, TextStyle::caption, "warning"); state.gap(tokens::kSpaceS); state.buttons(draft_actions);
            }
        }
    }}, {area_width, 0}, tokens::kSpaceM, draft_width);
    if (snap.unsaved_changes || !snap.has_saved) {
        b.gap(tokens::kSpaceS);
        b.button("account.loginOnce", T("action.loginOnce"), ButtonStyle::plain, [&ctx] { ctx.app.login_once(); ctx.refresh(); },
            "log-in", false, snap.draft_complete && !ctx.app.read_only() && !snap.busy);
        b.gap(tokens::kSpaceXs); b.text(T("account.loginOnceNote"), TextStyle::caption, "textTertiary");
    }
    if (!ctx.ui.account_feedback.empty()) {
        b.gap(tokens::kSpaceXs); b.text(F(ctx.ui.account_feedback, {{"store", U("app.store")}}), TextStyle::caption,
            ctx.ui.account_feedback == "account.savedDone" ? "success" : "danger", Role::status, "account.feedback");
    }
}

void build_inline_guard(Builder& b, ViewContext& ctx, const EngineSnapshot& snap) {
    const status::Presentation p = status::present(status::input_from(snap));
    const auto feedback = ctx.app.memory_verification_message();
    const bool verification_failed = feedback == "campus.verify.failed";
    const status::Readiness ready = status::recovery_readiness(snap, verification_failed);
    const bool read_only = ctx.app.read_only();
    const bool recover = snap.mode == MonitoringMode::recover && !read_only;
    const bool identification_needed = ctx.app.physical_identification_needed();
    const bool offer_remember = !snap.remembered || snap.needs_network_memory_update;
    const bool selection_needed = snap.needs_selection && !snap.required_campus_confirmed && !read_only && snap.network_available && !snap.paused && snap.holds.empty() &&
        (!snap.cooldown_remaining || *snap.cooldown_remaining <= 0);
    b.title_row("home.guard", "shield-check", T("dashboard.guard"), TextStyle::headline, "textPrimary", info_label("auto"),
        [&ctx] { ctx.open("info:auto"); }, recover ? T(ready.title) : std::wstring(), ready.tone,
        recover ? std::string(tone_icon(ready.tone)) : std::string(), true);
    b.gap(tokens::kSpaceS);
    b.segmented("guard.mode", L"", {T("mode.manual.title"), T("mode.observe.title"), T("mode.recover.title")},
        snap.mode == MonitoringMode::manual ? 0 : snap.mode == MonitoringMode::observe ? 1 : 2,
        [&ctx](int i) { ctx.app.set_mode(i == 0 ? MonitoringMode::manual : i == 1 ? MonitoringMode::observe : MonitoringMode::recover); ctx.refresh(); },
        !read_only && ctx.app.settings_writable());
    b.gap(8);
    if (recover) {
        const auto target = ctx.app.authentication_target_name();
        if (!target.empty()) {
            b.text(F("env.authenticationTarget", {{"name", target}}), TextStyle::callout_strong, "textPrimary");
            b.gap(tokens::kSpaceXs);
        }
        const auto physical_primary = ctx.app.physical_system_primary_name();
        if (!physical_primary.empty()) {
            b.text(F("env.systemPrimary", {{"name", physical_primary}}), TextStyle::caption, "textSecondary", Role::text, "network.systemPrimary");
            b.gap(tokens::kSpaceXs);
        }
        const auto physical_names = ctx.app.physical_connection_names();
        if (physical_names.size() > 1) {
            std::string names;
            for (const auto& name : physical_names) names += (names.empty() ? "" : "、") + name;
            b.text(F("env.connectedPhysical", {{"list", names}}), TextStyle::caption, "textSecondary", Role::text, "network.connectedPhysical");
            b.gap(tokens::kSpaceXs);
        }
        const auto& net = ctx.app.network();
        if (net.tunnel_known && net.tunnel.tunnel && !physical_primary.empty() && target.empty()) {
            b.text(T("env.tunnelSeparate"), TextStyle::caption, "textSecondary", Role::text, "network.tunnelSeparate");
            b.gap(tokens::kSpaceXs);
            b.text(T("env.campusPhysicalChannel"), TextStyle::caption, "textSecondary", Role::text, "network.campusPhysicalChannel");
            b.gap(tokens::kSpaceXs);
        }
    }
    const float network_confirmation_top = b.y();
    const auto campus_status = T(status::campus_verification_key(snap));
    if (read_only) b.text(campus_status, TextStyle::caption, "textSecondary", Role::status, "status.campusVerification");
    else {
        const float top = b.y();
        const float width = std::min(std::max(1.0f, b.width() - tokens::kSizeMinTarget),
            b.measure_text(campus_status, TextStyle::caption, 10000).width);
        const float height = std::max(tokens::kSizeWindowsSectionTitleVisualHeight,
            b.measure_text(campus_status, TextStyle::caption, width).height);
        const float label_height = b.measure_text(campus_status, TextStyle::caption, width).height;
        Widget status; status.kind = Kind::text; status.style = TextStyle::caption; status.color = "textSecondary";
        status.text = campus_status; status.name = campus_status; status.role = Role::status; status.id = "status.campusVerification";
        status.rect = D2D1::RectF(b.left(), top + (height - label_height) / 2,
            b.left() + width, top + (height + label_height) / 2); b.add(std::move(status));
        Widget info; info.kind = Kind::button; info.button = ButtonStyle::plain; info.icon = "info";
        info.id = "guard.memory.info"; info.name = info_label("memory"); info.role = Role::button; info.focusable = true;
        const float left = b.left() + width - tokens::kSpaceXxs;
        info.rect = D2D1::RectF(left, top + (height - tokens::kSizeMinTarget) / 2,
            left + tokens::kSizeMinTarget, top + (height + tokens::kSizeMinTarget) / 2);
        info.action = [&ctx] { ctx.open("info:memory"); }; b.add(std::move(info));
        b.set_y(top + height);
    }
    if (snap.conflicting_campus_hints) {
        b.gap(tokens::kSpaceXs);
        b.text(T("campus.entry.conflicting"), TextStyle::caption, "warning", Role::status, "campus.entries.conflicting");
    }
    if (selection_needed) {
        b.gap(tokens::kSpaceXs);
        b.text(T("campus.entry.bothDetected"), TextStyle::caption, "warning", Role::status, "campus.entries.selection");
    }
    b.gap(tokens::kSpaceS);
    if (read_only) {
        b.text(T("settings.readOnlyLaunch"), TextStyle::caption, "warning");
    }
    if (!read_only && !recover) {
        b.text(T("mode." + std::string(to_string(snap.mode)) + ".detail"), TextStyle::caption, "textSecondary");
    }
    if (recover) {
    // readiness: account / network / protection, each with an icon
    std::vector<ChipSpec> chips;
    for (const auto& item : ready.items) chips.push_back({T(item.key), item.tone, std::string(tone_icon(item.tone)), {}});
    b.chips(std::move(chips));
    if (snap.recovery_setup_has_network && (snap.auto_login == AutoLoginState::ready || snap.auto_login == AutoLoginState::awaitingProof) && feedback.empty()) {
        b.gap(tokens::kSpaceS);
        b.text(T(status::campus_verification_hint_key(snap)), TextStyle::caption, "textSecondary", Role::status, "guard.campus.hint");
    }
    if (p.banner && p.banner->key != "banner.readOnly" && (!selection_needed || p.banner->key.starts_with("hold.")) &&
        !(snap.recovery_setup_has_network && p.banner->key == "banner.awaitingProof")) {
        b.gap(tokens::kSpaceS);
        b.banner("guard.banner", p.banner->tone, T(p.banner->key + ".title"), T(p.banner->key + ".detail"), L"", {});
    }
    if (!snap.proof_wait.empty() && !selection_needed && !(snap.recovery_setup_has_network && (snap.proof_wait == "unconfirmed" || snap.proof_wait == "tunnel"))) {
        b.gap(8);
        b.text(T("memory.wait." + snap.proof_wait), TextStyle::caption, "warning", Role::status);
    }
    if (!snap.online && snap.cooldown_remaining && *snap.cooldown_remaining > 0) {
        b.gap(tokens::kSpaceS);
        b.text(F("guard.cooldown", {{"seconds", std::to_string(static_cast<int>(*snap.cooldown_remaining + 0.99))}}), TextStyle::caption, "info");
    }
    for (const Hold& hold : snap.holds) {
        const char* reason = hold.reason == HoldReason::userPaused ? "userPaused" : hold.reason == HoldReason::security ? "security" :
            hold.reason == HoldReason::uncertain ? "uncertain" : "credentials";
        b.gap(tokens::kSpaceS);
        b.text(F("guard.holdLine", {{"reason", U(std::string("hold.") + reason + ".title")},
            {"area", U("hold.area." + (hold.area ? std::string(to_string(*hold.area)) : "all"))},
            {"scope", U(hold.epoch ? "hold.scope.network" : "hold.scope.sticky")}}), TextStyle::caption, "warning");
    }
    }
    if (identification_needed) {
        b.gap(tokens::kSpaceS);
        b.text(T(ctx.app.physical_identification_detail()), TextStyle::callout, "warning", Role::status, "network.identification.reason");
        b.gap(tokens::kSpaceS);
        const bool copied = ctx.ui.copied == "guard.identification.copy";
        b.buttons({{"network.identification.refresh", T("env.identification.refresh"), ButtonStyle::secondary,
            [&ctx] { ctx.app.refresh_physical_identification(); ctx.refresh(); }, "refresh", !snap.busy},
            {"network.identification.copy", T(copied ? "records.copyDone" : "env.identification.copy"), ButtonStyle::secondary,
            [&ctx] { if (ctx.app.copy_diagnostics(ctx.hwnd)) ctx.copied("guard.identification.copy"); ctx.refresh(); }, copied ? "check" : "copy"}});
        b.gap(tokens::kSpaceS);
        b.text(T("env.identification.localOnly"), TextStyle::caption, "textSecondary");
        const auto identification_feedback = ctx.app.identification_feedback();
        if (!identification_feedback.empty()) {
            b.gap(tokens::kSpaceXs);
            b.text(T(identification_feedback),
                TextStyle::caption, "textSecondary", Role::status, "network.identification.feedback");
        }
    }
    if (read_only) return;
    b.gap(tokens::kSpaceS);
    if (offer_remember && !identification_needed) {
        b.text(T(snap.trusted ? "memory.sessionTrusted" : "memory.notRemembered"),
            TextStyle::caption, "textSecondary", Role::status, "guard.memory.state");
        b.gap(tokens::kSpaceS);
    }
    std::vector<ButtonSpec> actions;
    if (selection_needed && !snap.busy) {
        actions.push_back({"guard.area", T("account.areaTitle"), ButtonStyle::secondary,
            [&ctx] { ctx.open("account"); ctx.refresh(); }, "globe"});
    }
    if (recover && snap.has_saved) {
        if (!snap.paused && snap.auto_login == AutoLoginState::limited)
            actions.push_back({"guard.resumeLimited", T("action.resume"), ButtonStyle::secondary,
                [&ctx] { ctx.app.resume(); ctx.refresh(); }, "play"});
        actions.push_back({"guard.pause", T(snap.paused ? "action.resume" : "action.pause"), ButtonStyle::secondary,
            [&ctx, paused = snap.paused] { if (paused) ctx.app.resume(); else ctx.app.pause(); ctx.refresh(); }, snap.paused ? "play" : "pause"});
    }
    if (snap.verifying_memory || snap.verifying_campus) {
        actions.push_back({"guard.memory.cancel", T("action.cancel"), ButtonStyle::secondary,
            [&ctx] { ctx.app.cancel(); ctx.refresh(); }, "x"});
    } else if (identification_needed) {
        // Local identification actions above explain why this attachment cannot be remembered yet.
    } else if (!offer_remember) {
        actions.push_back({"campus.verify", T("campus.verify.action"), ButtonStyle::secondary,
            [&ctx] { ctx.app.verify_campus_service(); ctx.refresh(); }, "shield-check", snap.can_verify_campus});
        actions.push_back({"guard.forget", T("memory.forget"), ButtonStyle::plain, [&ctx] { ctx.app.forget_network(); ctx.refresh(); }, ""});
    } else {
        actions.push_back({"guard.remember", T(snap.area == CampusArea::teaching ? "memory.rememberTeaching" : "memory.remember"), ButtonStyle::secondary, [&ctx] { ctx.app.remember_network(); ctx.refresh(); },
            "bookmark", snap.can_remember});
        if (snap.trusted) {
            actions.push_back({"guard.revoke", T("action.revokeTrust"), ButtonStyle::plain, [&ctx] { ctx.app.revoke_trust(); ctx.refresh(); }, ""});
        } else {
            actions.push_back({"guard.once", T("memory.once"), ButtonStyle::plain, [&ctx] { ctx.app.trust(); ctx.refresh(); }, "", snap.can_trust});
        }
        if (snap.needs_network_memory_update)
            actions.push_back({"guard.forget", T("memory.forget"), ButtonStyle::plain, [&ctx] { ctx.app.forget_network(); ctx.refresh(); }, ""});
    }
    const float memory_top = b.y();
    float actions_width = actions.empty() ? 0 : tokens::kSpaceS * static_cast<float>(actions.size() - 1);
    for (const auto& action : actions) actions_width += b.measure_button(action.label, action.icon);
    std::vector<ButtonSpec> monitoring, memory;
    for (const auto& action : actions)
        (action.id == "guard.pause" || action.id == "guard.resumeLimited" ? monitoring : memory).push_back(action);
    if (actions_width > b.width() && !monitoring.empty() && !memory.empty()) {
        b.buttons(std::move(monitoring)); b.gap(tokens::kSpaceS); b.buttons(std::move(memory));
    } else b.buttons(std::move(actions));
    Widget anchor; anchor.kind = Kind::panel; anchor.id = "guard.memory";
    anchor.rect = D2D1::RectF(b.left(), b.y() > memory_top ? memory_top : network_confirmation_top,
        b.left() + b.width(), b.y()); b.add(std::move(anchor));
    if (!feedback.empty()) {
        b.gap(tokens::kSpaceS);
        const std::string tone = (feedback == "memory.verification.remembered" || feedback == "campus.verify.confirmed") ? "success" :
            feedback == "memory.verification.checking" || feedback == "memory.verification.pending" || feedback == "campus.verify.checking" ||
            feedback == "memory.verification.busy" ? "textSecondary" : "warning";
        const auto displayed_feedback = (feedback == "campus.verify.failed" || feedback == "campus.verify.autoFailed")
            ? status::campus_verification_hint_key(snap, true) : std::string_view(feedback);
        b.text(T(displayed_feedback), TextStyle::caption, tone, Role::status, "guard.memory.feedback");
    }
    if (!identification_needed && offer_remember && !snap.can_remember && !snap.busy && !snap.remember_blocker.empty()) {
        b.gap(tokens::kSpaceS);
        const std::string blocker = snap.remember_blocker == "unreliable" && !snap.authentication_route && ctx.app.network().path.descriptors.size() > 1
            ? "multiple" : snap.remember_blocker;
        b.text(blocker == "unreadable" ? T("memory.unreadable") : T("memory.hint." + blocker), TextStyle::caption,
            "textSecondary", Role::status, "guard.memory.blocker");
    }
}

namespace {
void campus_diagnosis_result(Builder& b, ViewContext& ctx) {
    auto& app = ctx.app;
    const CompatState compat = app.compat();
    if (compat.running) b.text(T("compat.running"), TextStyle::callout, "textPrimary", Role::status, "compat.progress");
    if (compat.done) {
        const float label_width = std::ceil(std::max(b.measure_text(T("compat.row.campus"), TextStyle::callout, 2048).width,
            b.measure_text(T("compat.row.public"), TextStyle::callout, 2048).width));
        const auto table_rule = [&b](const char* id) {
            b.gap(tokens::kSpaceS);
            Widget line; line.kind = Kind::divider; line.id = std::string("compat.table.divider.") + id;
            line.rect = D2D1::RectF(b.left(), b.y(), b.left() + b.width(), b.y()); b.add(std::move(line));
            b.gap(tokens::kSizeBorderWidth + tokens::kSpaceS);
        };
        b.row({[](Builder&) {},
            [](Builder& col) { col.text(T("compat.column.direct"), TextStyle::callout_strong, "textPrimary", Role::heading, "compat.table.direct"); },
            [](Builder& col) { col.text(T("compat.column.system"), TextStyle::callout_strong, "textPrimary", Role::heading, "compat.table.system"); }},
            {label_width, 0, 0}, tokens::kSpaceM, 1);
        table_rule("header");
        const auto diagnosis_row = [&b, label_width, &table_rule](const char* id, const char* key,
            const std::string& direct, const std::string& system) {
            const auto result_cell = [id](Builder& col, const std::string& result, const char* path) {
                const char* tone = result == "reachable" ? "success" : result == "notRun" || result == "cancelled" ? "textSecondary" : "warning";
                col.text(T("compat.result." + result), TextStyle::callout, tone, Role::status, std::string("compat.table.") + id + "." + path);
            };
            b.row({[id, key](Builder& col) { col.text(T(key), TextStyle::callout, "textPrimary", Role::text, std::string("compat.table.") + id + ".label"); },
                [direct, result_cell](Builder& col) { result_cell(col, direct, "direct"); },
                [system, result_cell](Builder& col) { result_cell(col, system, "system"); }}, {label_width, 0, 0}, tokens::kSpaceM, 1);
            table_rule(id);
        };
        diagnosis_row("campus", "compat.row.campus", compat.input.campus_direct, compat.input.campus_system);
        diagnosis_row("public", "compat.row.public", compat.input.public_direct, compat.input.public_system);
        b.gap(tokens::kSpaceXs);
        const std::string verdict = compat.verdict.verdict;
        const std::string tone = verdict == "allReachable" ? "success" : verdict == "cancelled" ? "neutral" : "warning";
        b.banner("compat.verdict", tone, T("compat.verdict." + verdict + ".title"), T("compat.verdict." + verdict + ".detail"), L"", {});
        for (const auto& rec : compat.verdict.recommendations) {
            b.gap(tokens::kSpaceS); b.text(T("compat.rec." + rec), TextStyle::callout, "textPrimary");
        }
    }
}

void manual_proxy_rules(Builder& b, ViewContext& ctx) {
    const auto rules_title = T("proxyAssistant.manualRules");
    const float rules_text_height = b.measure_text(rules_title, TextStyle::callout_strong,
        std::max(1.0f, b.width() - 22)).height;
    const float rules_height = std::max(tokens::kSizeWindowsSectionTitleVisualHeight, rules_text_height);
    const float rules_target_height = std::max(tokens::kSizeMinTarget, rules_height);
    const float rules_top = b.y();
    Widget disclosure; disclosure.kind = Kind::panel; disclosure.id = "proxy.manualRules";
    disclosure.role = Role::button; disclosure.focusable = true; disclosure.checked = ctx.ui.proxy_rules_expanded;
    disclosure.name = rules_title + L" · " + T(ctx.ui.proxy_rules_expanded ? "disclosure.expanded" : "disclosure.collapsed");
    disclosure.rect = D2D1::RectF(b.left(), rules_top + (rules_height - rules_target_height) / 2,
        b.left() + b.width(), rules_top + (rules_height + rules_target_height) / 2);
    disclosure.radius = tokens::kRadiusSmall;
    disclosure.action = [&ctx] { ctx.ui.proxy_rules_expanded = !ctx.ui.proxy_rules_expanded; ctx.refresh(); };
    b.add(std::move(disclosure));
    b.gap((rules_height - rules_text_height) / 2);
    b.icon_row(ctx.ui.proxy_rules_expanded ? "chevron-down" : "chevron-right", "textSecondary",
        rules_title, TextStyle::callout_strong, "textPrimary", 14);
    b.set_y(rules_top + rules_height);
    if (ctx.ui.proxy_rules_expanded) {
        b.gap(tokens::kSpaceS);
        b.text(T("compat.rules.detail"), TextStyle::callout, "textSecondary", Role::text, "proxy.manualRules.summary");
        b.gap(tokens::kSpaceS);
        b.text(win::widen(compat::rules_text()), TextStyle::mono, "textPrimary");
        b.gap(tokens::kSpaceS);
        const bool copied = ctx.ui.copied == "compat.copyRules";
        b.button("compat.copyRules", T(copied ? "compat.rules.copied" : "compat.rules.copy"), ButtonStyle::secondary, [&ctx] {
            if (ctx.app.copy_rules(ctx.hwnd)) ctx.copied("compat.copyRules"); ctx.refresh();
        }, copied ? "check" : "copy");
    }
}

void network_sheet(Builder& b, ViewContext& ctx, const EngineSnapshot& snap, bool inline_content = false) {
    AppController& app = ctx.app;
    const auto& net = app.network();
    if (!inline_content) b.heading(T("env.details"));
    b.card_begin("surfaceMuted", "", tokens::kSpaceM, tokens::kRadiusMedium);
    const float refresh_width = b.measure_button(T("env.refresh"), "refresh");
    b.row({[&ctx](Builder& title) {
        title.title_row("network.proxyModes", "", T("env.proxyModes"), TextStyle::callout_strong, "textPrimary",
            info_label("proxyModes"), [&ctx] { ctx.open("info:proxyModes"); });
    }, [&ctx, &snap](Builder& button) {
        button.button("network.refresh", T("env.refresh"), ButtonStyle::secondary,
            [&ctx] { ctx.app.refresh_environment(); ctx.refresh(); }, "refresh", false, !snap.busy);
    }}, {0, refresh_width}, tokens::kSpaceS, 120);
    b.gap(tokens::kSpaceS);
    float label_width = 72;
    for (const auto* key : {"env.layer.access", "env.layer.proxy", "env.layer.tunnel", "env.connectionDetails.interfaces",
        "env.connectionDetails.authenticationTarget", "env.connectionDetails.systemPrimary", "env.connectionDetails.campusService"})
        label_width = std::max(label_width, b.measure_text(T(key), TextStyle::callout, 2048).width);
    const auto row = [&b, label_width](const char* key, const std::wstring& value, const char* id, const char* tone = "textPrimary") {
        b.row({[key](Builder& label) { label.text(T(key), TextStyle::callout, "textSecondary"); },
            [value, id, tone](Builder& text) { text.text(value, TextStyle::callout, tone, Role::text, id); }},
            {label_width, 0}, tokens::kSpaceM, 96);
        b.gap(tokens::kSpaceS);
    };
    row("env.layer.access", T("env.access." + net.access), "network.environment.access");
    row("env.layer.proxy", T("env.proxy." + app.proxy_summary().state), "network.environment.proxy");
    row("env.layer.tunnel", T("env.tunnel." + std::string(net.tunnel_known ? net.tunnel.tunnel ? "detected" : "notDetected" : "unknown")),
        "network.environment.tunnel");
    b.divider();
    if (!net.path.interfaces.empty())
        row("env.connectionDetails.interfaces", win::widen(net.path.interfaces), "network.environment.interfaces");
    if (const auto target = app.authentication_target_name(); !target.empty()) {
        row("env.connectionDetails.authenticationTarget", win::widen(target), "network.environment.authenticationTarget");
    }
    if (const auto primary = app.physical_system_primary_name(); !primary.empty()) {
        row("env.connectionDetails.systemPrimary", win::widen(primary), "network.environment.systemPrimary");
    }
    const auto campus = status::campus_verification_key(snap);
    const char* campus_value = campus == "env.campusVerification.checking" ? "checking" :
        campus == "env.campusVerification.confirmed" ? "confirmed" :
        campus == "env.campusVerification.unconfirmed" ? "unconfirmed" : "notChecked";
    row("env.connectionDetails.campusService", T(std::string("env.connectionDetails.") + campus_value),
        "network.environment.campusService", campus == "env.campusVerification.confirmed" ? "success" : "textPrimary");
    if (const auto refreshed = app.environment_refreshed()) {
        b.text(F(app.environment_refresh_incomplete() ? "env.refreshedPartial" : "env.refreshed",
            {{"time", clock_seconds(*refreshed)}}), TextStyle::caption,
            app.environment_refresh_incomplete() ? "warning" : "textSecondary", Role::status, "network.refresh.feedback");
    }
    b.card_end();
    if (!inline_content) close_button(b, ctx);
}

void sheet_section_heading(Builder& b, const std::string& id, const std::string& icon, const std::wstring& title) {
    const float icon_size = tokens::kSizeIconMedium;
    const float text_width = std::max(1.0f, b.width() - icon_size - tokens::kSpaceS);
    const float height = std::max(icon_size, b.measure_text(title, TextStyle::headline, text_width).height);
    b.row({[icon, icon_size, height](Builder& col) {
        Widget glyph; glyph.kind = Kind::icon; glyph.icon = icon; glyph.color = "accent";
        glyph.rect = D2D1::RectF(col.left(), col.y() + (height - icon_size) / 2,
            col.left() + icon_size, col.y() + (height + icon_size) / 2);
        col.add(std::move(glyph)); col.gap(height);
    }, [title, id](Builder& col) { col.text(title, TextStyle::headline, "textPrimary", Role::heading, id); }},
        {icon_size, 0}, tokens::kSpaceS, 1);
}

void sheet_rule(Builder& b, float spacing = tokens::kSpaceM) {
    b.gap(spacing);
    Widget line; line.kind = Kind::divider;
    line.rect = D2D1::RectF(b.left(), b.y(), b.left() + b.width(), b.y());
    b.add(std::move(line));
    b.gap(tokens::kSizeBorderWidth + spacing);
}

void sheet_external_link(Builder& b, std::string id, const std::wstring& title, std::string_view url) {
    const float icon_size = tokens::kSizeSheetAccessoryIcon;
    const float inset = tokens::kSpaceXs;
    const float label_width = std::max(1.0f, b.width() - icon_size - inset * 3);
    const auto label = b.measure_text(title, TextStyle::callout_strong, label_width);
    const float height = std::max(tokens::kSizeMinTarget, label.height + inset * 2);
    const float top = b.y(), text_top = top + (height - label.height) / 2;
    Widget target; target.kind = Kind::panel; target.id = std::move(id); target.text = title;
    target.detail = win::widen(url); target.name = title == target.detail ? title : title + L" · " + target.detail;
    target.rect = D2D1::RectF(b.left(), top, b.left() + b.width(), top + height);
    target.radius = tokens::kRadiusSmall; target.role = Role::link; target.button = ButtonStyle::link; target.focusable = true;
    target.action = [url = std::string(url)] { win::open_official_link(url); };
    b.add(std::move(target));
    Widget text; text.kind = Kind::text; text.text = title; text.style = TextStyle::callout_strong; text.color = "accent";
    text.rect = D2D1::RectF(b.left() + inset, text_top, b.left() + inset + label_width, text_top + label.height);
    b.add(std::move(text));
    Widget glyph; glyph.kind = Kind::icon; glyph.icon = "external-link"; glyph.color = "accent";
    const float icon_x = b.left() + inset * 2 + std::min(label_width, label.width);
    glyph.rect = D2D1::RectF(icon_x, top + (height - icon_size) / 2, icon_x + icon_size, top + (height + icon_size) / 2);
    b.add(std::move(glyph)); b.gap(height);
}

void help_link_group(Builder& b, const std::string& title_key, std::initializer_list<const char*> keys) {
    b.text(T(title_key), TextStyle::caption_strong, "textSecondary", Role::heading);
    b.gap(tokens::kSpaceXs);
    std::vector<std::function<void(Builder&)>> cells;
    std::vector<float> widths;
    float line_width = 0;
    const auto flush = [&] {
        if (!cells.empty()) b.row(std::move(cells), std::move(widths), tokens::kSpaceM, 1);
        cells.clear(); widths.clear(); line_width = 0;
    };
    for (const char* key : keys) {
        for (const Link& link : kOfficialLinks) {
            if (std::string_view(link.key) != key) continue;
            const auto title = T(std::string("link.") + key);
            const float natural_width = b.measure_text(title, TextStyle::callout_strong, 10000).width
                + tokens::kSizeSheetAccessoryIcon + tokens::kSpaceXs * 3;
            const float width = std::min(b.width(), std::max(tokens::kSizeMinTarget, natural_width));
            if (!cells.empty() && line_width + tokens::kSpaceM + width > b.width()) {
                flush(); b.gap(tokens::kSpaceM);
            }
            const char* url = link.url;
            cells.push_back([key = std::string(key), title, url](Builder& col) {
                sheet_external_link(col, "help.link." + key, title, url);
            });
            widths.push_back(width);
            line_width += (cells.size() > 1 ? tokens::kSpaceM : 0) + width;
        }
    }
    flush();
}

void help_faq_row(Builder& b, ViewContext& ctx, const std::string& key) {
    const auto question = T("help.faq." + key + ".q");
    const bool expanded = std::find(ctx.ui.expanded_help_faq.begin(), ctx.ui.expanded_help_faq.end(), key) != ctx.ui.expanded_help_faq.end();
    const float icon_size = tokens::kSizeSheetAccessoryIcon;
    const float text_width = std::max(1.0f, b.width() - icon_size - tokens::kSpaceM);
    const float text_height = b.measure_text(question, TextStyle::callout_strong, text_width).height;
    const float height = std::max(tokens::kSizeMinTarget, text_height + tokens::kSpaceXs * 2);
    // Keep the minimum target exactly representable even far down a long FAQ.
    // Fractional text heights otherwise accumulate until (top + 44) - top < 44.
    const float top = std::ceil(b.y());
    b.set_y(top);
    Widget target; target.kind = Kind::panel; target.id = "help.faq." + key; target.text = question;
    target.rect = D2D1::RectF(b.left(), top, b.left() + b.width(), top + height);
    target.radius = tokens::kRadiusSmall; target.role = Role::button; target.focusable = true; target.checked = expanded;
    target.name = question + L" · " + T(expanded ? "disclosure.expanded" : "disclosure.collapsed");
    target.action = [&ctx, key] {
        auto& opened = ctx.ui.expanded_help_faq;
        const auto found = std::find(opened.begin(), opened.end(), key);
        if (found == opened.end()) opened.push_back(key); else opened.erase(found);
        ctx.refresh();
    };
    b.add(std::move(target));
    Widget text; text.kind = Kind::text; text.text = question; text.style = TextStyle::callout_strong; text.color = "textPrimary";
    text.rect = D2D1::RectF(b.left(), top + (height - text_height) / 2, b.left() + text_width, top + (height + text_height) / 2);
    b.add(std::move(text));
    Widget glyph; glyph.kind = Kind::icon; glyph.icon = expanded ? "chevron-down" : "chevron-right"; glyph.color = "textSecondary";
    glyph.rect = D2D1::RectF(b.left() + b.width() - icon_size, top + (height - icon_size) / 2,
        b.left() + b.width(), top + (height + icon_size) / 2);
    b.add(std::move(glyph)); b.gap(height);
    if (expanded) {
        b.text(T("help.faq." + key + ".a"), TextStyle::callout, "textSecondary", Role::text, "help.faq." + key + ".answer");
        b.gap(tokens::kSpaceM);
    }
}

void help_sheet(Builder& b, ViewContext& ctx) {
    // Help uses three separate cards with answers disclosed on demand.
    b.card_begin("surface", "border", tokens::kSpaceL, tokens::kRadiusLarge);
    const bool expanded = ctx.ui.help_quick_start_expanded;
    const auto title = T("setup.helpTitle");
    const float icon_size = tokens::kSizeSheetAccessoryIcon;
    const float text_width = std::max(1.0f, b.width() - icon_size - tokens::kSpaceM);
    const float text_height = b.measure_text(title, TextStyle::headline, text_width).height;
    const float height = std::max(tokens::kSizeMinTarget, text_height + tokens::kSpaceXs * 2);
    const float top = std::ceil(b.y());
    b.set_y(top);
    Widget disclosure; disclosure.kind = Kind::panel; disclosure.id = "help.quickStart.disclosure";
    disclosure.text = title; disclosure.role = Role::button; disclosure.focusable = true;
    disclosure.checked = expanded; disclosure.radius = tokens::kRadiusSmall;
    disclosure.name = title + L" · " + T(expanded ? "disclosure.expanded" : "disclosure.collapsed");
    disclosure.rect = D2D1::RectF(b.left(), top, b.left() + b.width(), top + height);
    disclosure.action = [&ctx] { ctx.ui.help_quick_start_expanded = !ctx.ui.help_quick_start_expanded; ctx.refresh(); };
    b.add(std::move(disclosure));
    Widget heading; heading.kind = Kind::text; heading.id = "help.quickStart"; heading.role = Role::heading;
    heading.text = title; heading.style = TextStyle::headline; heading.color = "textPrimary";
    heading.rect = D2D1::RectF(b.left(), top + (height - text_height) / 2,
        b.left() + text_width, top + (height + text_height) / 2);
    b.add(std::move(heading));
    Widget glyph; glyph.kind = Kind::icon; glyph.icon = expanded ? "chevron-down" : "chevron-right"; glyph.color = "textSecondary";
    glyph.rect = D2D1::RectF(b.left() + b.width() - icon_size, top + (height - icon_size) / 2,
        b.left() + b.width(), top + (height + icon_size) / 2);
    b.add(std::move(glyph)); b.gap(height);
    if (expanded) {
        b.gap(tokens::kSpaceM);
        b.text(T("setup.helpDetail"), TextStyle::caption, "textSecondary", Role::text, "help.quickStart.intro");
        const std::pair<const char*, const char*> steps[] = {
            {"setup.helpNetworkTitle", "setup.helpNetwork"}, {"setup.helpAccountTitle", "setup.helpAccount"},
            {"setup.helpCheckTitle", "setup.helpCheck"}, {"setup.helpMemoryTitle", "setup.helpMemory"},
            {"setup.helpRecoveryTitle", "setup.helpRecovery"}, {"setup.helpWifiTitle", "setup.wifi.body"}
        };
        std::size_t index = 0;
        for (const auto& step : steps) {
            const auto id = "help.quickStart.step." + std::to_string(index++);
            b.gap(tokens::kSpaceM);
            b.text(T(step.first), TextStyle::callout_strong, "textPrimary", Role::heading, id + ".title");
            b.gap(tokens::kSpaceXs);
            b.text(T(step.second), TextStyle::caption, "textSecondary", Role::text, id + ".detail");
        }
    }
    b.card_end();
    b.gap(tokens::kSpaceL);
    b.card_begin("surface", "border", tokens::kSpaceL, tokens::kRadiusLarge);
    sheet_section_heading(b, "help.faqTitle", "help", T("help.faqTitle"));
    b.gap(tokens::kSpaceM);
    for (const char* q : {"wifi", "cooldown", "noInternet", "rejected", "vpn", "nonCampus", "dorm", "cleanup", "official"}) {
        help_faq_row(b, ctx, q);
        if (std::string_view(q) != "official") sheet_rule(b, 0);
    }
    // Keep only spaceXs below the final FAQ disclosure row.
    b.gap(tokens::kSpaceXs - tokens::kSpaceL);
    b.card_end();
    b.gap(tokens::kSpaceL);
    b.card_begin("surface", "border", tokens::kSpaceL, tokens::kRadiusLarge);
    sheet_section_heading(b, "help.linksTitle", "globe", T("help.linksTitle"));
    b.gap(tokens::kSpaceM);
    help_link_group(b, "help.links.account", {"activation", "forgot", "identity", "self"});
    sheet_rule(b);
    help_link_group(b, "help.links.guides", {"teachingGuide", "library", "dormAccount", "dormFAQ"});
    sheet_rule(b);
    help_link_group(b, "help.links.support", {"nc", "download", "contact"});
    b.gap(tokens::kSpaceM);
    b.text(T("link.leaveNote"), TextStyle::caption, "textSecondary");
    b.card_end();
}

void about_licenses(Builder& b, ViewContext& ctx) {
    const auto& documents = license_documents();
    const auto license_title = T("about.licenses.title") + L" · " + F("about.licenses.count", {{"count", std::to_string(documents.size())}});
    const float license_icon = tokens::kSizeSheetAccessoryIcon;
    const float license_label_width = std::max(1.0f, b.width() - license_icon * 2 - tokens::kSpaceS * 2);
    const float license_text_height = b.measure_text(license_title, TextStyle::headline, license_label_width).height;
    const float license_row_height = std::max(tokens::kSizeMinTarget, license_text_height + tokens::kSpaceXs * 2);
    const float license_top = std::ceil(b.y());
    Widget license_group; license_group.kind = Kind::panel; license_group.id = "about.licenses.disclosure";
    license_group.text = license_title; license_group.role = Role::button; license_group.focusable = true;
    license_group.checked = ctx.ui.licenses_expanded; license_group.radius = tokens::kRadiusSmall;
    license_group.name = license_title + L" · " + T(ctx.ui.licenses_expanded ? "disclosure.expanded" : "disclosure.collapsed");
    license_group.rect = D2D1::RectF(b.left(), license_top, b.left() + b.width(), license_top + license_row_height);
    license_group.action = [&ctx] { ctx.ui.licenses_expanded = !ctx.ui.licenses_expanded; ctx.refresh(); };
    b.add(std::move(license_group));
    Widget license_book; license_book.kind = Kind::icon; license_book.icon = "book"; license_book.color = "accent";
    license_book.rect = D2D1::RectF(b.left(), license_top + (license_row_height - license_icon) / 2,
        b.left() + license_icon, license_top + (license_row_height + license_icon) / 2);
    b.add(std::move(license_book));
    Widget license_heading;
    license_heading.kind = Kind::text; license_heading.id = "about.licenses"; license_heading.role = Role::heading;
    license_heading.text = license_title; license_heading.style = TextStyle::headline; license_heading.color = "textPrimary";
    license_heading.rect = D2D1::RectF(b.left() + license_icon + tokens::kSpaceS, license_top + (license_row_height - license_text_height) / 2,
        b.left() + license_icon + tokens::kSpaceS + license_label_width, license_top + (license_row_height + license_text_height) / 2);
    b.add(std::move(license_heading));
    Widget license_chevron; license_chevron.kind = Kind::icon;
    license_chevron.icon = ctx.ui.licenses_expanded ? "chevron-down" : "chevron-right"; license_chevron.color = "textSecondary";
    license_chevron.rect = D2D1::RectF(b.left() + b.width() - license_icon, license_top + (license_row_height - license_icon) / 2,
        b.left() + b.width(), license_top + (license_row_height + license_icon) / 2);
    b.add(std::move(license_chevron)); b.set_y(license_top + license_row_height);
    if (!ctx.ui.licenses_expanded) return;
    b.gap(tokens::kSpaceM);
    b.text(T("about.licenses.detail"), TextStyle::callout, "textSecondary");
    for (const auto& document : documents) {
        sheet_rule(b);
        const std::string id = document.id;
        const auto& opened = ctx.ui.expanded_licenses;
        const bool expanded = std::find(opened.begin(), opened.end(), id) != opened.end();
        const float disclosure_icon_size = tokens::kSizeSheetAccessoryIcon;
        const float disclosure_text_width = std::max(1.0f, b.width() - disclosure_icon_size - tokens::kSpaceM);
        const float disclosure_text_height = b.measure_text(document.title, TextStyle::body_strong, disclosure_text_width).height;
        const float disclosure_height = std::max(tokens::kSizeMinTarget, disclosure_text_height + tokens::kSpaceXs * 2);
        const float top = b.y();
        Widget disclosure; disclosure.kind = Kind::panel; disclosure.id = "about.license." + id;
        disclosure.text = document.title; disclosure.name = document.title; disclosure.role = Role::button;
        disclosure.focusable = true; disclosure.checked = expanded; disclosure.radius = tokens::kRadiusSmall;
        disclosure.rect = D2D1::RectF(b.left(), top, b.left() + b.width(), top + disclosure_height);
        disclosure.action = [&ctx, id] {
            auto& current = ctx.ui.expanded_licenses;
            const auto found = std::find(current.begin(), current.end(), id);
            if (found == current.end()) current.push_back(id); else current.erase(found);
            ctx.refresh();
        };
        b.add(std::move(disclosure));
        b.text_at(document.title, TextStyle::body_strong, "textPrimary", b.left(), disclosure_text_width,
            top + (disclosure_height - disclosure_text_height) / 2);
        Widget glyph; glyph.kind = Kind::icon; glyph.icon = expanded ? "chevron-down" : "chevron-right";
        glyph.color = "textSecondary";
        glyph.rect = D2D1::RectF(b.left() + b.width() - disclosure_icon_size, top + (disclosure_height - disclosure_icon_size) / 2,
            b.left() + b.width(), top + (disclosure_height + disclosure_icon_size) / 2);
        b.add(std::move(glyph)); b.gap(disclosure_height);
        if (!expanded) continue;
        if (document.text.empty()) {
            b.text(T("about.licenses.unavailable"), TextStyle::callout, "warning");
        } else {
            const auto copy_id = "about.license.copy." + id;
            const bool copied = ctx.ui.copied == copy_id;
            b.button(copy_id, T(copied ? "action.copied" : "action.copy"), ButtonStyle::secondary,
                [&ctx, text = document.text, copy_id] {
                    if (!ctx.app.offline_smoke() && win::copy_to_clipboard(ctx.hwnd, text)) ctx.copied(copy_id);
                    ctx.refresh();
                }, copied ? "check" : "copy", false, !ctx.app.offline_smoke());
            b.gap(tokens::kSpaceM);
            b.text(win::widen(document.text), TextStyle::callout, "textPrimary", Role::text, "about.license.text." + id);
        }
        b.gap(tokens::kSpaceS);
    }
}

void about_sheet(Builder& b, ViewContext& ctx) {
    // About presents brand, developer, licenses, then storage and privacy.
    b.card_begin("surface", "border", tokens::kSpaceL, tokens::kRadiusLarge);
    const auto name = T("about.productName"), tagline = T("app.tagline");
    const auto version = F("about.version", {{"version", kVersion}, {"build", kBuild}});
    const float icon_size = tokens::kSizeSheetBrandIcon;
    const float text_width = std::max(1.0f, b.width() - icon_size - tokens::kSpaceM);
    const float text_height = b.measure_text(name, TextStyle::title, text_width).height
        + b.measure_text(tagline, TextStyle::callout, text_width).height
        + b.measure_text(version, TextStyle::caption, text_width).height + tokens::kSpaceXs * 2;
    const float height = std::max(icon_size, text_height);
    b.row({[icon_size, height](Builder& col) {
        Widget icon; icon.kind = Kind::icon; icon.icon = "appBrand";
        icon.rect = D2D1::RectF(col.left(), col.y() + (height - icon_size) / 2,
            col.left() + icon_size, col.y() + (height + icon_size) / 2);
        col.add(std::move(icon)); col.gap(height);
    }, [name, tagline, version](Builder& col) {
        col.text(name, TextStyle::title, "textPrimary", Role::heading);
        col.gap(tokens::kSpaceXs);
        col.text(tagline, TextStyle::callout, "textSecondary");
        col.gap(tokens::kSpaceXs);
        col.text(version, TextStyle::caption, "textSecondary", Role::text, "about.version");
    }}, {icon_size, 0}, tokens::kSpaceM, 1);
    sheet_rule(b);
    b.text(T("app.unofficial"), TextStyle::caption, "textSecondary");
    b.card_end();
    b.gap(tokens::kSpaceL);
    b.card_begin("surface", "border", tokens::kSpaceL, tokens::kRadiusLarge);
    sheet_section_heading(b, "about.developer.title", "globe", T("about.developer.title"));
    b.gap(tokens::kSpaceM);
    b.text(T("about.developer.description"), TextStyle::callout, "textSecondary");
    b.gap(tokens::kSpaceM);
    sheet_external_link(b, "about.project", win::widen(links::kProjectUrl), links::kProjectUrl);
    b.gap(tokens::kSpaceM);
    b.text(T("about.project.star"), TextStyle::caption, "textSecondary");
    sheet_rule(b);
    b.text(win::widen(links::kFeedbackEmail), TextStyle::callout, "textPrimary", Role::text, "about.feedback.email");
    b.gap(tokens::kSpaceS);
    b.button("about.feedback.copy", T(ctx.ui.copied == "about.feedback.copy" ? "action.copied" : "about.feedback.copyEmail"),
        ButtonStyle::plain, [&ctx] {
            if (!ctx.app.offline_smoke() && win::copy_to_clipboard(ctx.hwnd, std::string(links::kFeedbackEmail))) ctx.copied("about.feedback.copy");
            ctx.refresh();
        }, "copy");
    b.gap(tokens::kSpaceS);
    b.text(T("about.feedback.detail"), TextStyle::callout, "textSecondary");
    b.gap(tokens::kSpaceS);
    b.text(T("about.feedback.subject"), TextStyle::caption, "textSecondary", Role::text, "about.feedback.subject");
    b.gap(tokens::kSpaceXs);
    b.text(T("about.feedback.faultHint"), TextStyle::caption, "textSecondary");
    b.gap(tokens::kSpaceS);
    b.button("about.feedback.example", T("about.feedback.exampleTitle"), ButtonStyle::plain, [&ctx] {
        ctx.ui.feedback_example_expanded = !ctx.ui.feedback_example_expanded; ctx.refresh();
    }, ctx.ui.feedback_example_expanded ? "chevron-down" : "chevron-right");
    if (ctx.ui.feedback_example_expanded) {
        b.gap(tokens::kSpaceS);
        b.text(T("about.feedback.example"),
            TextStyle::callout, "textSecondary", Role::text, "about.feedback.example.body");
    }
    b.card_end();
    b.gap(tokens::kSpaceL);
    b.card_begin("surface", "border", tokens::kSpaceL, tokens::kRadiusLarge);
    about_licenses(b, ctx);
    b.card_end();
    b.gap(tokens::kSpaceL);
    b.card_begin("surface", "border", tokens::kSpaceL, tokens::kRadiusLarge);
    sheet_section_heading(b, "about.storage.title", "lock", T("about.storage.title"));
    b.gap(tokens::kSpaceM);
    b.text(T("about.storage.body"), TextStyle::callout, "textSecondary");
    b.gap(tokens::kSpaceM);
    b.button("about.deleteAccount", T("account.delete"), ButtonStyle::secondary,
        [&ctx] { ctx.ui.account_feedback.clear(); ctx.open("accountDelete"); }, "", false,
        ctx.app.can_delete_account());
    if (ctx.ui.account_feedback == "account.deleted") {
        b.gap(tokens::kSpaceS); b.text(T("account.deleted"), TextStyle::caption, "success", Role::status);
    }
    b.gap(tokens::kSpaceM);
    b.button("about.uninstall", T("uninstall.open"), ButtonStyle::secondary, [&ctx] {
        if (ctx.app.read_only() || ctx.app.offline_smoke()) return;
        const auto result = open_uninstaller();
        ctx.ui.uninstall_feedback = result.error_key;
        ctx.refresh();
    }, "", false, !ctx.app.read_only() && !ctx.app.offline_smoke());
    b.gap(tokens::kSpaceS);
    b.text(T("uninstall.hint"), TextStyle::caption, "textSecondary");
    if (!ctx.ui.uninstall_feedback.empty()) {
        b.gap(tokens::kSpaceS); b.text(T(ctx.ui.uninstall_feedback), TextStyle::caption, "danger", Role::status);
    }
    b.card_end();
}

void tutorial(Builder& b, ViewContext& ctx, const EngineSnapshot& snap, const status::Presentation& p) {
    const int step = std::clamp(ctx.ui.tutorial_step, 1, static_cast<int>(kGuideSteps.size()));
    const std::string name = kGuideSteps[static_cast<std::size_t>(step - 1)];
    const auto finish = [&ctx](const char* status) {
        if (std::string_view(status) == "completed" || ctx.app.settings().tutorial == "none") ctx.app.set_tutorial(status);
        ctx.ui.tutorial_visible = false; ctx.refresh();
    };
    const auto title = T("guide." + name + ".title");
    const auto progress = std::to_wstring(step) + L" / " + std::to_wstring(kGuideSteps.size());
    const auto progress_size = b.measure_text(progress, TextStyle::caption, 10000);
    const float progress_width = std::ceil(progress_size.width) + 2;
    const auto title_size = b.measure_text(title, TextStyle::headline,
        std::max(1.0f, b.width() - progress_width - tokens::kSizeMinTarget - 20));
    const float header_height = std::max(tokens::kSizeMinTarget, std::max(title_size.height, progress_size.height));
    b.row({[title, title_size, header_height](Builder& cell) {
            cell.gap((header_height - title_size.height) / 2);
            cell.text(title, TextStyle::headline, "textPrimary", Role::heading, "tutorial.coach.title");
        }, [progress, progress_size, header_height](Builder& cell) {
            cell.gap((header_height - progress_size.height) / 2);
            cell.text(progress, TextStyle::caption, "textSecondary");
        }, [finish, header_height](Builder& close) {
            close.gap((header_height - tokens::kSizeMinTarget) / 2);
            close.icon_button("tutorial.close", T("guide.exit"), "x", [finish] { finish("skipped"); });
        }}, {0, progress_width, tokens::kSizeMinTarget}, 10, 1);
    b.gap(9);
    b.text(T("guide." + name + ".body"), TextStyle::callout, "textSecondary");
    const bool fresh = !snap.busy && ctx.app.last_checked() && *ctx.app.last_checked() >= ctx.ui.tutorial_entered_at;
    std::wstring status_text;
    if (name == "detection") {
        if (snap.busy) status_text = T("guide.check.running");
        else if (fresh) {
            const auto system = ctx.app.system_connectivity();
            const auto result = system && system->online() && !snap.online && status::system_path_can_lead(snap) ? U("env.systemOnline.title") : U(p.headline);
            status_text = F("guide.check.done", {{"result", result}});
        } else status_text = T("guide.check.waiting");
    } else if (name == "account") status_text = T(snap.unsaved_changes ? "guide.account.unsaved" : snap.has_saved ? "guide.account.saved" : "guide.account.empty");
    else if (name == "login") {
        const auto system = ctx.app.system_connectivity();
        status_text = T(snap.busy ? "guide.login.running" : snap.online ? "guide.login.online" :
            system && system->online() && status::system_path_can_lead(snap) ? "guide.login.systemOnline" : "guide.login.waiting");
    } else if (name == "memory") status_text = T(snap.recovery_setup_has_network ? "guide.memory.saved" : "guide.memory.waiting");
    else if (name == "recovery") status_text = F("guide.recovery.mode", {{"mode", U("mode." + std::string(to_string(snap.mode)) + ".title")}});
    if (!status_text.empty()) { b.gap(9); b.text(status_text, TextStyle::caption, "accent", Role::status, "tutorial.coach.status"); }
    b.gap(9);
    std::vector<ButtonSpec> actions;
    if (step > 1) actions.push_back({"tutorial.back", T("tutorial.back"), ButtonStyle::secondary, [&ctx, step] {
        ctx.ui.tutorial_step = step - 1; ctx.ui.tutorial_entered_at = AppController::wall_now(); focus_tutorial(ctx);
    }, ""});
    if (name != "welcome" && name != "finish") actions.push_back({"tutorial.focus", T("guide.focus"), ButtonStyle::plain, [&ctx] { focus_tutorial(ctx); }, ""});
    const bool needs_action = (name == "detection" && !fresh) || (name == "account" && !snap.has_saved);
    const auto next_title = T(needs_action ? "guide.skipStep" : name == "finish" ? "tutorial.finish" : name == "welcome" ? "guide.start" : "tutorial.next");
    const auto next = [&ctx, step, finish] {
        if (step == static_cast<int>(kGuideSteps.size())) finish("completed");
        else { ctx.ui.tutorial_step = step + 1; ctx.ui.tutorial_entered_at = AppController::wall_now(); focus_tutorial(ctx); }
    };
    const float next_width = std::min(b.width(), b.measure_button(next_title, ""));
    float actions_width = actions.empty() ? 0 : tokens::kSpaceS * static_cast<float>(actions.size() - 1);
    for (const auto& action : actions) actions_width += b.measure_button(action.label, action.icon);
    const auto primary = [next_title, next](Builder& row) { row.button("tutorial.next", next_title, ButtonStyle::primary, next); };
    if (actions_width + tokens::kSpaceS + next_width <= b.width()) {
        b.row({[actions](Builder& row) { row.buttons(actions); }, primary}, {0, next_width}, tokens::kSpaceS, 1);
    } else {
        b.buttons(actions); b.gap(tokens::kSpaceS);
        b.row({[](Builder&) {}, primary}, {0, next_width}, tokens::kSpaceS, 1);
    }
}

}  // namespace

void build_tutorial_coach(Builder& b, ViewContext& ctx, const EngineSnapshot& snap, const status::Presentation& p) {
    tutorial(b, ctx, snap, p);
}

void build_network_comparison(Builder& b, ViewContext& ctx, const EngineSnapshot& snap) {
    AppController& app = ctx.app;
    const auto system = app.system_connectivity();
    const auto direct = app.internet_report();
    b.card_begin("surfaceMuted", "", tokens::kSpaceM, tokens::kRadiusMedium);
    const bool public_check_running = snap.busy && !snap.verifying_memory && !snap.verifying_campus;
    const bool system_check_running = app.system_connectivity_running();
    const bool checking = public_check_running || system_check_running;
    const auto check_label = T(checking ? "action.cancel" : "action.check");
    const auto check_icon = checking ? "x" : "refresh";
    const float check_width = b.measure_button(check_label, check_icon);
    const auto comparison_title = T("env.pathComparison");
    const float title_width = std::min(std::ceil(b.measure_text(comparison_title, TextStyle::callout_strong, 10000).width) + 1,
        std::max(1.0f, b.width() - tokens::kSizeMinTarget));
    const float title_height = b.measure_text(comparison_title, TextStyle::callout_strong, title_width).height;
    const float heading_height = std::max(title_height, tokens::kSizeMinTarget);
    const float heading_top = b.y();
    Widget title; title.kind = Kind::text; title.style = TextStyle::callout_strong; title.color = "textPrimary";
    title.text = title.name = comparison_title; title.role = Role::heading; title.id = "network.comparison.heading";
    title.rect = D2D1::RectF(b.left(), heading_top + (heading_height - title_height) / 2,
        b.left() + title_width, heading_top + (heading_height + title_height) / 2); b.add(std::move(title));
    Widget help; help.kind = Kind::button; help.button = ButtonStyle::plain; help.icon = "info";
    help.id = "network.comparison.info"; help.name = info_label("networkComparison"); help.role = Role::button;
    help.focusable = true; help.rect = D2D1::RectF(b.left() + title_width, heading_top + (heading_height - tokens::kSizeMinTarget) / 2,
        b.left() + title_width + tokens::kSizeMinTarget, heading_top + (heading_height + tokens::kSizeMinTarget) / 2);
    help.action = [&ctx] { ctx.open("info:networkComparison"); }; b.add(std::move(help));
    b.set_y(heading_top + heading_height);
    b.gap(tokens::kSpaceS);
    float option_width = 0;
    for (const auto& label : {T("probe.quick.title"), T("probe.complete.title")})
        option_width = std::max(option_width, b.measure_text(label, TextStyle::body_strong, 10000).width + 16);
    const float picker_width = std::max(190.0f, option_width * 2 + 8);
    const float mode_width = picker_width + tokens::kSizeMinTarget + tokens::kSpaceS;
    b.row({[&ctx, &snap, picker_width](Builder& mode) {
        mode.selector_row("network.probe", L"", {T("probe.quick.title"), T("probe.complete.title")},
            snap.probe_mode == ProbeMode::quick ? 0 : 1,
            [&ctx](int i) { ctx.app.set_probe(i == 0 ? ProbeMode::quick : ProbeMode::complete); ctx.refresh(); },
            info_label("probe"), [&ctx] { ctx.open("info:probe"); }, !snap.busy, picker_width);
    }, [&ctx, &snap, checking, check_label, check_icon](Builder& action) {
        action.button("network.check", check_label, ButtonStyle::secondary, [&ctx, checking] {
            if (checking) ctx.app.cancel(); else ctx.app.check();
            ctx.refresh();
        }, check_icon, false, checking || !snap.busy);
    }}, {0, check_width}, tokens::kSpaceS, mode_width);
    b.gap(tokens::kSpaceM);
    const float site_width = std::max(70.0f, std::ceil(b.measure_text(L"Microsoft", TextStyle::callout_strong, 10000).width));
    const auto path_heading = [](Builder& c, const std::string& path, const std::wstring& label,
                                 const std::optional<double>& checked_at, bool running) {
        c.text(label, TextStyle::callout_strong, "textPrimary", Role::heading, "network.comparison." + path + ".heading");
        if (!running && checked_at) {
            c.gap(tokens::kSpaceXs);
            c.text(win::widen(clock_seconds(*checked_at)), TextStyle::caption, "textSecondary", Role::text,
                "network.comparison." + path + ".time");
        }
    };
    b.row({[](Builder&) {},
        [&app, &system, system_check_running, path_heading](Builder& c) {
            path_heading(c, "system", T("env.systemProbe"), system ? app.system_checked_at() : std::nullopt, system_check_running);
        }, [&app, &direct, public_check_running, path_heading](Builder& c) {
            path_heading(c, "physical", T("env.physicalProbe"), direct ? app.last_checked() : std::nullopt, public_check_running);
        }}, {site_width, 0, 0}, tokens::kSpaceM, 56);
    const auto table_divider = [&b](const std::string& id) {
        b.gap(tokens::kSpaceM);
        Widget line; line.kind = Kind::divider; line.id = "network.comparison.divider." + id;
        line.rect = D2D1::RectF(b.left(), b.y(), b.left() + b.width(), b.y()); b.add(std::move(line));
        b.gap(tokens::kSizeBorderWidth + tokens::kSpaceM);
    };
    const auto endpoint = [](Builder& c, const std::string& id, const EndpointResult* result, bool running) {
        c.text(running ? T("dashboard.checking") : probe_result(result), TextStyle::body,
            !running && result && result->succeeded() ? "success" : "textPrimary", Role::text, id);
    };
    const auto comparison_row = [&b, site_width, system_check_running, public_check_running, endpoint](const std::string& id,
        const std::wstring& label, const EndpointResult* system_result, const EndpointResult* direct_result) {
        b.row({[label](Builder& c) { c.text(label, TextStyle::callout_strong, "textPrimary"); },
            [id, system_result, system_check_running, endpoint](Builder& c) {
                endpoint(c, "network.comparison.system." + id, system_result, system_check_running);
            }, [id, direct_result, public_check_running, endpoint](Builder& c) {
                endpoint(c, "network.comparison.physical." + id, direct_result, public_check_running);
            }}, {site_width, 0, 0}, tokens::kSpaceM, 56);
    };
    table_divider("heading");
    comparison_row("apple", L"Apple", system ? &system->apple : nullptr, direct ? &direct->base.apple : nullptr);
    table_divider("apple");
    comparison_row("microsoft", L"Microsoft", system ? &system->microsoft : nullptr, direct ? &direct->base.microsoft : nullptr);
    b.gap(tokens::kSpaceM); b.text(T("env.pathComparisonNote"), TextStyle::callout, "textSecondary");
    b.card_end();
}

void build_network_details(Builder& b, ViewContext& ctx, const EngineSnapshot& snap) {
    network_sheet(b, ctx, snap, true);
}

void build_website_strategy(Builder& b, ViewContext& ctx, const EngineSnapshot& snap) {
    b.title_row("internet.strategy.heading", "", T("internet.strategy"), TextStyle::callout_strong, "textPrimary",
        info_label("websiteStrategy"), [&ctx] { ctx.open("info:websiteStrategy"); });
    b.gap(tokens::kSpaceXs);
    b.text(T("internet.strategyScope"), TextStyle::caption, "textSecondary", Role::text, "internet.strategy.scope");
    b.gap(tokens::kSpaceS);
    b.segmented("internet.strategy", L"", {T("website.strategy.system.title"), T("website.strategy.direct.title")},
        ctx.app.website_follow_system_proxy() ? 0 : 1,
        [&ctx](int i) { ctx.app.set_website_proxy(i == 0); ctx.refresh(); }, !snap.busy && !ctx.app.read_only());
    b.gap(tokens::kSpaceS);
    b.text(T(ctx.app.website_follow_system_proxy() ? "website.strategy.system.detail" : "website.strategy.direct.detail"),
        TextStyle::caption, "textSecondary", Role::text, "internet.strategy.detail");
}

void build_proxy_assistant(Builder& b, ViewContext& ctx, const EngineSnapshot& snap) {
    auto& app = ctx.app;
    const bool writable = !app.read_only() && !snap.busy;
    b.text(T("proxyAssistant.intro"), TextStyle::callout, "textSecondary", Role::text, "proxy.intro");
    b.gap(tokens::kSpaceM);
    b.card_begin("surface", "border", tokens::kSpaceM, tokens::kRadiusLarge);
    const char* names[]{"verge", "mihomo", "singbox"};
    const auto select_client = [&ctx](int client) {
        ctx.app.close_proxy_assistant(); ctx.app.open_proxy_assistant();
        if (client >= 0 && client < 3) ctx.app.set_proxy_config_format(static_cast<proxyassist::Format>(client));
        ctx.ui.proxy_assistant_client = client; ctx.ui.proxy_client_picker = false;
        ctx.ui.proxy_advanced_expanded = false; ctx.ui.proxy_rules_expanded = false;
        ctx.ui.proxy_verge_tun_expanded = false;
        ctx.refresh();
    };
    const auto client_button = [&b, select_client](const char* id, int client, const std::wstring& title) {
        const float icon_size = tokens::kSizeSheetAccessoryIcon;
        const float label_width = std::max(1.0f, b.width() - icon_size - tokens::kSpaceM);
        const float label_height = b.measure_text(title, TextStyle::body, label_width).height;
        const float height = std::max(tokens::kSizeMinTarget, label_height + tokens::kSpaceS * 2);
        const float top = b.y();
        Widget item; item.kind = Kind::panel; item.id = id; item.text = title; item.name = title;
        item.role = Role::button; item.focusable = true; item.radius = tokens::kRadiusSmall;
        item.rect = D2D1::RectF(b.left(), top, b.left() + b.width(), top + height);
        item.action = [select_client, client] { select_client(client); }; b.add(std::move(item));
        b.text_at(title, TextStyle::body, "textPrimary", b.left(), label_width, top + (height - label_height) / 2);
        Widget arrow; arrow.kind = Kind::icon; arrow.icon = "chevron-right"; arrow.color = "textSecondary";
        arrow.rect = D2D1::RectF(b.left() + b.width() - icon_size, top + (height - icon_size) / 2,
            b.left() + b.width(), top + (height + icon_size) / 2);
        b.add(std::move(arrow)); b.set_y(top + height);
    };
    const auto fixed_target = [&b, &ctx](const std::string& id, const std::string& copy_id, const std::string& label,
                                       const std::string& copy_label, const std::string& value) {
        b.gap(tokens::kSpaceS);
        const auto button_label = T(ctx.ui.copied == copy_id ? "action.copied" : copy_label);
        const float button_width = b.measure_button(button_label, "copy");
        const auto target_text = T(label);
        const auto copy_action = [&ctx, copy_id, value] {
            if (!ctx.app.offline_smoke() && win::copy_to_clipboard(ctx.hwnd, value)) ctx.copied(copy_id);
            ctx.refresh();
        };
        const float target_width = b.width() - (button_width + tokens::kSpaceS);
        if (target_width < 144) {
            b.text(target_text, TextStyle::callout, "textPrimary", Role::text, id);
            b.gap(tokens::kSpaceXs);
            b.button(copy_id, button_label, ButtonStyle::plain, copy_action, "copy");
            return;
        }
        const float target_height = b.measure_text(target_text, TextStyle::callout, target_width).height;
        const float button_height = std::max(tokens::kSizeMinTarget, b.measure_text(button_label, TextStyle::body_strong, 10000).height + 20);
        const float row_height = std::max(target_height, button_height), top = b.y();
        b.row({[target_text, id, top, row_height, target_height](Builder& col) {
                col.set_y(top + (row_height - target_height) / 2);
                col.text(target_text, TextStyle::callout, "textPrimary", Role::text, id);
            }, [copy_action, copy_id, button_label, top, row_height, button_height](Builder& col) {
                col.set_y(top + (row_height - button_height) / 2);
                col.button(copy_id, button_label, ButtonStyle::plain, copy_action, "copy");
            }}, {0, button_width}, tokens::kSpaceS, 144);
        b.set_y(top + row_height);
    };
    const int selected_client = ctx.ui.proxy_client_picker ? -1 : ctx.ui.proxy_assistant_client;
    if (ctx.ui.proxy_client_picker || ctx.ui.proxy_assistant_client < 0) {
        b.text(T("proxyAssistant.client"), TextStyle::headline, "textPrimary", Role::heading, "proxy.client.heading");
        b.gap(tokens::kSpaceS); client_button("proxy.format.verge", 0, T("proxyAssistant.format.verge"));
        sheet_rule(b, 0); client_button("proxy.client.v2rayn", 4, T("proxyAssistant.format.v2rayn"));
        sheet_rule(b, 0); client_button("proxy.client.party", 5, T("proxyAssistant.format.party"));
        sheet_rule(b, 0); client_button("proxy.client.generic", 3, T("proxyAssistant.format.generic"));
        b.gap(tokens::kSpaceM);
        b.button("proxy.fileOptions", T("proxyAssistant.fileOptions"), ButtonStyle::plain, [&ctx] {
            ctx.ui.proxy_file_choices_expanded = !ctx.ui.proxy_file_choices_expanded; ctx.refresh();
        }, ctx.ui.proxy_file_choices_expanded ? "chevron-down" : "chevron-right");
        if (ctx.ui.proxy_file_choices_expanded) {
            for (int i = 1; i < 3; ++i) {
                b.gap(tokens::kSpaceS);
                b.button(std::string("proxy.format.") + names[i], T(std::string("proxyAssistant.format.") + names[i]), ButtonStyle::plain,
                    [select_client, i] { select_client(i); });
            }
        }
    } else {
        const auto client = ctx.ui.proxy_assistant_client;
        const auto key = client == 3 ? "generic" : client == 4 ? "v2rayn" : client == 5 ? "party" :
            client >= 0 && client < 3 ? names[client] : "generic";
        const auto title = T(std::string("proxyAssistant.format.") + key);
        const auto change = T("proxyAssistant.changeClient");
        const auto style = TextStyle::body_strong;
        const float icon = tokens::kSizeSheetAccessoryIcon;
        const auto change_size = b.measure_text(change, style, 10000);
        const float change_width = std::ceil(change_size.width) + 1;
        const float trailing_width = change_width + tokens::kSpaceS + icon;
        const bool inline_title = b.measure_text(title, style, 10000).width + tokens::kSpaceM + trailing_width <= b.width();
        const float title_width = inline_title ? b.width() - tokens::kSpaceM - trailing_width : b.width();
        const auto title_size = b.measure_text(title, style, title_width);
        const float line_height = std::max(tokens::kSizeMinTarget, std::max(title_size.height, change_size.height));
        const float header_height = inline_title ? line_height : title_size.height + tokens::kSpaceXs + std::max(tokens::kSizeMinTarget, change_size.height);
        const float top = b.y();
        Widget target; target.kind = Kind::panel; target.id = "proxy.changeClient"; target.role = Role::button;
        target.name = title + L" · " + change; target.focusable = true; target.radius = tokens::kRadiusSmall;
        target.rect = D2D1::RectF(b.left(), top, b.left() + b.width(), top + header_height);
        target.action = [&ctx] { ctx.ui.proxy_client_picker = true; ctx.refresh(); }; b.add(std::move(target));
        const float title_top = inline_title ? top + (header_height - title_size.height) / 2 : top;
        Widget label; label.kind = Kind::text; label.id = "proxy.selectedClient"; label.role = Role::heading;
        label.text = title; label.name = title; label.style = style;
        label.rect = D2D1::RectF(b.left(), title_top, b.left() + title_width, title_top + title_size.height); b.add(std::move(label));
        const float change_top = top + header_height - (std::max(tokens::kSizeMinTarget, change_size.height) + change_size.height) / 2;
        Widget action_label; action_label.kind = Kind::text; action_label.id = "proxy.changeClient.label";
        action_label.text = change; action_label.name = change; action_label.style = style; action_label.color = "accent"; action_label.role = Role::text;
        action_label.rect = D2D1::RectF(b.left() + b.width() - trailing_width, change_top,
            b.left() + b.width() - tokens::kSpaceS - icon, change_top + change_size.height); b.add(std::move(action_label));
        Widget arrow; arrow.kind = Kind::icon; arrow.id = "proxy.changeClient.icon"; arrow.icon = "chevron-down"; arrow.color = "accent";
        const float center = change_top + change_size.height / 2;
        arrow.rect = D2D1::RectF(b.left() + b.width() - icon, center - icon / 2, b.left() + b.width(), center + icon / 2);
        b.add(std::move(arrow)); b.set_y(top + header_height);
    }
    const char* name = names[static_cast<int>(app.proxy_config_format())];
    const bool file_client = selected_client >= 0 && selected_client < 3;
    const bool verge = selected_client == 0;
    if (selected_client >= 0) {
        sheet_rule(b, tokens::kSpaceS);
        b.text(T("proxyAssistant.stepsTitle"), TextStyle::callout_strong, "textPrimary", Role::heading, "proxy.steps.heading");
        b.gap(tokens::kSpaceM);
    } else b.gap(tokens::kSpaceS);
    if (selected_client == 3) {
        b.text(T("proxyAssistant.generic.scope"), TextStyle::callout, "textSecondary", Role::status, "proxy.generic.scope");
        b.gap(tokens::kSpaceM);
        b.text(T("proxyAssistant.generic.step1"), TextStyle::callout, "textPrimary");
        b.gap(tokens::kSpaceM);
        b.text(T("proxyAssistant.generic.step2"), TextStyle::callout, "textPrimary");
        fixed_target("proxy.generic.domain", "proxy.generic.copyDomain", "proxyAssistant.generic.domain", "proxyAssistant.generic.copyDomain", "net.szu.edu.cn");
        fixed_target("proxy.generic.address", "proxy.generic.copyAddress", "proxyAssistant.generic.address", "proxyAssistant.generic.copyAddress", "172.30.255.42");
        b.gap(tokens::kSpaceM);
        b.text(T("proxyAssistant.generic.step3"), TextStyle::callout, "textPrimary");
    } else if (selected_client == 4) {
        b.text(T("proxyAssistant.v2rayn.scope"), TextStyle::callout, "textSecondary");
        b.gap(tokens::kSpaceM); b.text(T("proxyAssistant.v2rayn.step1"), TextStyle::callout, "textPrimary");
        b.gap(tokens::kSpaceM);
        b.button("proxy.v2rayn.copyRules", T(ctx.ui.copied == "proxy.v2rayn.copyRules" ? "action.copied" : "proxyAssistant.v2rayn.copy"),
            ButtonStyle::secondary, [&ctx] {
                if (!ctx.app.offline_smoke() && win::copy_to_clipboard(ctx.hwnd, proxyassist::v2rayn_rules())) ctx.copied("proxy.v2rayn.copyRules");
                ctx.refresh();
            }, "copy");
        b.gap(tokens::kSpaceS); b.text(T("proxyAssistant.v2rayn.step2"), TextStyle::callout, "textPrimary");
        b.gap(tokens::kSpaceM); b.text(T("proxyAssistant.v2rayn.step3"), TextStyle::callout, "textPrimary");
        b.gap(tokens::kSpaceS); b.text(T("proxyAssistant.v2rayn.limit"), TextStyle::caption, "textSecondary");
    } else if (selected_client == 5) {
        b.text(T("proxyAssistant.party.scope"), TextStyle::callout, "textSecondary");
        b.gap(tokens::kSpaceM); b.text(T("proxyAssistant.party.step1"), TextStyle::callout, "textPrimary");
        b.gap(tokens::kSpaceM); b.text(T("proxyAssistant.party.step2"), TextStyle::callout, "textPrimary");
        fixed_target("proxy.party.domain", "proxy.party.domain.copy", "proxyAssistant.verge.domain", "proxyAssistant.generic.copyDomain", "net.szu.edu.cn");
        fixed_target("proxy.party.address", "proxy.party.address.copy", "proxyAssistant.verge.address", "proxyAssistant.party.copyAddress", "172.30.255.42/32");
        b.gap(tokens::kSpaceM); b.text(T("proxyAssistant.party.step3"), TextStyle::callout, "textPrimary");
    } else if (!file_client) {
        b.text(T("proxyAssistant.chooseClient"), TextStyle::callout, "textSecondary", Role::status, "proxy.chooseClient");
    }
    if (file_client) {
        b.text(T(std::string("proxyAssistant.guide.") + name), TextStyle::callout, "textSecondary");
        b.gap(tokens::kSpaceM);
        if (verge) {
            b.text(T("proxyAssistant.verge.step1"), TextStyle::callout, "textPrimary");
            b.gap(tokens::kSpaceM); b.text(T("proxyAssistant.verge.step2"), TextStyle::callout, "textPrimary");
            fixed_target("proxy.verge.domain", "proxy.verge.copyDomain", "proxyAssistant.verge.domain", "proxyAssistant.generic.copyDomain", "net.szu.edu.cn");
            fixed_target("proxy.verge.address", "proxy.verge.copyAddress", "proxyAssistant.verge.address", "proxyAssistant.party.copyAddress", "172.30.255.42/32");
            b.gap(tokens::kSpaceS); b.text(T("proxyAssistant.verge.step3"), TextStyle::callout, "textPrimary");
            b.gap(tokens::kSpaceM);
            b.button("proxy.verge.tunSteps", T("proxyAssistant.verge.tunSteps"), ButtonStyle::plain, [&ctx] {
                ctx.ui.proxy_verge_tun_expanded = !ctx.ui.proxy_verge_tun_expanded; ctx.refresh();
            }, ctx.ui.proxy_verge_tun_expanded ? "chevron-down" : "chevron-right");
            if (ctx.ui.proxy_verge_tun_expanded) { b.gap(tokens::kSpaceS); b.text(T("proxyAssistant.verge.tunDetail"), TextStyle::callout, "textSecondary", Role::text, "proxy.verge.tunDetail"); }
            b.gap(tokens::kSpaceM);
            b.button("proxy.advanced", T("proxyAssistant.advanced"), ButtonStyle::plain, [&ctx] {
                ctx.ui.proxy_advanced_expanded = !ctx.ui.proxy_advanced_expanded; ctx.refresh();
            }, ctx.ui.proxy_advanced_expanded ? "chevron-down" : "chevron-right");
        } else {
            b.text(T(std::string("proxyAssistant.fileSteps.") + name), TextStyle::callout, "textPrimary", Role::text, "proxy.fileSteps");
        }
    }
    const bool file_editor = file_client && (!verge || ctx.ui.proxy_advanced_expanded);
    if (file_editor) {
        if (verge) {
            b.gap(tokens::kSpaceM); b.text(T("proxyAssistant.script.step1"), TextStyle::callout, "textPrimary");
            b.gap(tokens::kSpaceM); b.text(T("proxyAssistant.script.step2"), TextStyle::callout, "textPrimary");
            b.gap(tokens::kSpaceM); b.text(T("proxyAssistant.script.step3"), TextStyle::callout, "textPrimary");
            b.gap(tokens::kSpaceM);
            b.button("proxy.copyScript", T(ctx.ui.copied == "proxy.copyScript" ? "action.copied" : "proxyAssistant.copy"), ButtonStyle::secondary, [&ctx] {
                if (ctx.app.copy_proxy_script(ctx.hwnd)) ctx.copied("proxy.copyScript"); ctx.refresh();
            }, ctx.ui.copied == "proxy.copyScript" ? "check" : "copy");
            b.gap(tokens::kSpaceS); b.text(T("proxyAssistant.copyDetail"), TextStyle::caption, "textSecondary");
            b.gap(tokens::kSpaceM); b.text(T("proxyAssistant.scriptFile"), TextStyle::callout, "textSecondary");
        }
        b.gap(tokens::kSpaceS);
        b.text(T("proxyAssistant.scope"), TextStyle::callout, "textSecondary");
        b.gap(tokens::kSpaceM);
        b.button("proxy.select", T("proxyAssistant.select"), ButtonStyle::secondary, [&ctx] {
            ctx.app.prepare_proxy_config(ctx.hwnd); ctx.refresh();
        }, "search", false, writable);
        const auto& preview = app.proxy_config_preview();
        if (preview.ready) {
            b.gap(tokens::kSpaceM);
            b.text(F("proxyAssistant.file", {{"name", preview.filename}}), TextStyle::callout, "textSecondary", Role::text, "proxy.file");
            if (preview.changed) {
                b.gap(tokens::kSpaceS);
                b.text(T("proxyAssistant.preview"), TextStyle::callout_strong, "textPrimary", Role::heading);
                for (const auto& change : preview.changes) {
                    b.gap(tokens::kSpaceS);
                    b.icon_row("check", "accent", T("proxyAssistant.change." + change), TextStyle::callout, "textPrimary", 16);
                }
                b.gap(tokens::kSpaceM);
                b.button("proxy.save", T("proxyAssistant.save"), ButtonStyle::primary, [&ctx] {
                    ctx.app.apply_proxy_config(); ctx.refresh();
                }, "check", false, writable);
            }
        }
        if (!app.proxy_config_feedback().empty()) {
            b.gap(tokens::kSpaceM);
            b.text(T(app.proxy_config_feedback()), TextStyle::callout,
                app.proxy_config_feedback().starts_with("proxyAssistant.error.") ? "warning" : "textPrimary", Role::status, "proxy.feedback");
        }
        b.gap(tokens::kSpaceM);
        b.text(T(std::string("proxyAssistant.reload.") + name), TextStyle::callout, "textSecondary");
    }
    if (selected_client == 0 || selected_client >= 3) {
        sheet_rule(b);
        b.text(T("proxyAssistant.ruleMode"), TextStyle::caption, "textSecondary", Role::text, "proxy.modeHint");
    }
    if (file_editor && selected_client != 2) { b.gap(tokens::kSpaceM); manual_proxy_rules(b, ctx); }
    b.card_end();
    b.gap(tokens::kSpaceM);
    b.card_begin("surface", "border", tokens::kSpaceM, tokens::kRadiusLarge);
    b.title_row("proxy.check", "", T("proxyAssistant.checkTitle"), TextStyle::headline, "textPrimary",
        info_label("addressResolution"), [&ctx] { ctx.open("info:addressResolution"); }, {}, {}, {}, true);
    b.gap(tokens::kSpaceS);
    b.text(T("proxyAssistant.testDetail"), TextStyle::caption, "textSecondary", Role::text, "proxy.check.detail");
    b.gap(tokens::kSpaceM);
    const auto compat = app.compat();
    b.button("proxy.test", T(compat.running ? "action.stopDiagnosis" : "proxyAssistant.test"), ButtonStyle::secondary, [&ctx] {
        if (ctx.app.compat().running) ctx.app.cancel_compat(); else ctx.app.run_compat(true);
        ctx.refresh();
    }, compat.running ? "x" : "refresh", false, !snap.busy);
    if (compat.running || compat.done) {
        b.gap(tokens::kSpaceM); campus_diagnosis_result(b, ctx);
    }
    b.card_end();
    const bool has_backup = app.proxy_config_has_backup();
    if (file_editor || has_backup) { b.gap(tokens::kSpaceL); b.text(T("proxyAssistant.backup"), TextStyle::caption, "textSecondary"); }
    if (app.read_only()) { b.gap(tokens::kSpaceS); b.text(T("proxyAssistant.readonly"), TextStyle::caption, "textSecondary"); }
    if (file_editor || has_backup) {
        b.gap(tokens::kSpaceS);
        b.button("proxy.restore", T("proxyAssistant.restore"), ButtonStyle::plain, [&ctx] { ctx.open("proxyRestore"); },
            "", false, writable && has_backup);
    }
    if (has_backup) {
        b.gap(tokens::kSpaceS);
        b.button("proxy.discard", T("proxyAssistant.discard"), ButtonStyle::plain, [&ctx] { ctx.open("proxyDiscard"); }, "", false, writable);
    }
}

void build_overlay(Builder& b, ViewContext& ctx, const std::string& name, const EngineSnapshot& snap, const status::Presentation& p) {
    const auto dialog_actions = [&b](std::vector<ButtonSpec> actions) {
        std::vector<std::function<void(Builder&)>> cells{[](Builder&) {}};
        std::vector<float> widths{0};
        for (const auto& action : actions) {
            widths.push_back(std::min(b.width(), b.measure_button(action.label, action.icon)));
            cells.push_back([action](Builder& row) { row.button(action.id, action.label, action.style, action.action, action.icon, false, action.enabled); });
        }
        b.row(std::move(cells), std::move(widths), tokens::kSpaceS, 1);
    };
    if (name == "proxyAssistant") build_proxy_assistant(b, ctx, snap);
    else if (name == "proxyDiscard") {
        b.text(T("proxyAssistant.discardDetail"), TextStyle::callout, "textSecondary");
        b.gap(tokens::kSpaceM);
        dialog_actions({{"proxy.discardCancel", T("action.cancel"), ButtonStyle::plain, [&ctx] { ctx.close(); }, ""},
            {"proxy.discardConfirm", T("proxyAssistant.discardTitle"), ButtonStyle::secondary, [&ctx] {
                ctx.app.discard_proxy_config_backup(); ctx.close();
            }, "", !ctx.app.read_only() && !snap.busy && ctx.app.proxy_config_has_backup()}});
    } else if (name == "proxyRestore") {
        b.text(T("proxyAssistant.restoreDetail"), TextStyle::callout, "textSecondary");
        b.gap(tokens::kSpaceM);
        dialog_actions({{"proxy.restoreCancel", T("action.cancel"), ButtonStyle::plain, [&ctx] { ctx.close(); }, ""},
            {"proxy.restoreConfirm", T("proxyAssistant.restore"), ButtonStyle::secondary, [&ctx] {
                ctx.app.restore_proxy_config(); ctx.close();
            }, "", !ctx.app.read_only() && !snap.busy && ctx.app.proxy_config_has_backup()}});
    } else if (name == "websiteResult" && ctx.ui.website_details) {
        const auto& details = *ctx.ui.website_details;
        const auto& result = details.check.result;
        const std::string state = result.state;
        b.text(win::widen(details.check.target.url), TextStyle::caption, "textSecondary");
        b.gap(tokens::kSpaceM);
        b.text(T("website.state." + state + ".title"), TextStyle::headline, "textSecondary");
        b.gap(tokens::kSpaceM);
        b.text(F("website.state." + state + ".detail", {{"code", std::to_string(result.status)}}), TextStyle::callout, "textSecondary");
        if (result.status >= 100 && result.status <= 599) {
            b.gap(8);
            b.text(L"HTTP " + std::to_wstring(result.status), TextStyle::caption, "textSecondary");
        }
        b.gap(tokens::kSpaceM);
        b.text(T("website.result.strategy"), TextStyle::caption_strong, "textSecondary");
        const std::string strategy = details.follow_system_proxy ? "system" : "direct";
        b.gap(tokens::kSpaceXs);
        b.text(T("website.strategy." + strategy + ".title"), TextStyle::callout, "textSecondary");
        b.gap(tokens::kSpaceXs);
        b.text(T("website.strategy." + strategy + ".detail"), TextStyle::callout, "textSecondary");
        if (state != "accessible" && state != "cancelled" && state != "invalidInput" && state != "securityPolicy") {
            b.gap(tokens::kSpaceM);
            b.text(T("internet.routingWarning.title"), TextStyle::headline, "textSecondary");
            b.gap(tokens::kSpaceM);
            b.text(T("internet.routingWarning.body"), TextStyle::callout, "textSecondary");
        }
        b.gap(tokens::kSpaceM);
        b.text(T("website.result.scope"), TextStyle::callout, "textSecondary");
    } else if (name == "wifiName") {
        b.field("memory.name", T("wifiRecovery.windows.manualSSID"), L"", false, false);
        b.gap(tokens::kSpaceM); b.text(T("wifiRecovery.windows.manualSSID.detail"), TextStyle::caption, "textSecondary");
        if (!ctx.ui.network_name_error.empty()) b.text(T(ctx.ui.network_name_error), TextStyle::caption, "danger", Role::status);
        b.gap(tokens::kSpaceM);
        dialog_actions({{"memory.nameCancel", T("memory.nameCancel"), ButtonStyle::plain, [&ctx] { ctx.close(); }, ""},
            {"memory.nameSave", T("wifiRecovery.windows.manualSSID.save"), ButtonStyle::primary, [&ctx] {
            const auto ssid = win::narrow(ctx.ui.network_name_draft);
            if (!NetworkLabels::valid_ssid(ssid)) ctx.ui.network_name_error = "wifiRecovery.windows.manualSSID.invalid";
            else if (ctx.app.set_wifi_name(ctx.ui.network_name_id, ssid)) { ctx.close(); return; }
            else ctx.ui.network_name_error = "wifiRecovery.saveFailed";
            ctx.refresh();
        }, "", !ctx.app.read_only() && !ctx.app.wifi_unreadable()}});
    } else if (name == "memoryRename") {
        b.field("memory.name", T("memory.renameTitle"), L"", false, false);
        b.gap(tokens::kSpaceM);
        b.text(T("memory.renameDetail"), TextStyle::caption, "textSecondary");
        if (!ctx.ui.network_name_error.empty()) b.text(T(ctx.ui.network_name_error), TextStyle::caption, "danger", Role::status);
        b.gap(tokens::kSpaceM);
        dialog_actions({{"memory.nameCancel", T("memory.nameCancel"), ButtonStyle::plain, [&ctx] { ctx.close(); }, ""},
            {"memory.nameSave", T("memory.nameSave"), ButtonStyle::primary, [&ctx] {
            if (ctx.app.rename_network(ctx.ui.network_name_id, win::narrow(ctx.ui.network_name_draft))) ctx.close();
            else { ctx.ui.network_name_error = "error.storage"; ctx.refresh(); }
        }, "", !ctx.app.read_only() && !NetworkLabels::clean_name(win::narrow(ctx.ui.network_name_draft)).empty()}});
    } else if (name == "memoryForget") {
        b.text(F("memory.forgetConfirmDetail", {{"name", ctx.app.network_display_name(ctx.ui.network_name_id)}}), TextStyle::callout, "textSecondary");
        b.gap(tokens::kSpaceM);
        dialog_actions({{"memory.forgetCancel", T("action.cancel"), ButtonStyle::plain, [&ctx] { ctx.close(); }, ""},
            {"memory.forgetConfirm", T("memory.forget"), ButtonStyle::secondary, [&ctx] {
                ctx.app.forget_network(ctx.ui.network_name_id); ctx.close();
            }, "", !ctx.app.read_only()}});
    } else if (name == "memoryForgetAll") {
        b.text(T("memory.forgetAllConfirmDetail"), TextStyle::callout, "textSecondary");
        b.gap(tokens::kSpaceM);
        dialog_actions({{"memory.forgetAllCancel", T("action.cancel"), ButtonStyle::plain, [&ctx] { ctx.close(); }, ""},
            {"memory.forgetAllConfirm", T("memory.forgetAll"), ButtonStyle::secondary, [&ctx] {
                ctx.app.forget_all_networks(); ctx.close();
            }, "", !ctx.app.read_only()}});
    } else if (name == "accountDelete") {
        b.text(T("account.deleteDetail"), TextStyle::callout, "textSecondary");
        if (ctx.ui.account_feedback == "account.deleteFailed") {
            b.gap(tokens::kSpaceS); b.text(T("account.deleteFailed"), TextStyle::caption, "danger", Role::status);
        }
    } else if (name == "network") network_sheet(b, ctx, snap);
    else if (name == "help") help_sheet(b, ctx);
    else if (name == "about") about_sheet(b, ctx);
    else tutorial(b, ctx, snap, p);
}

void build_account_deletion_actions(Builder& b, ViewContext& ctx) {
    std::vector<ButtonSpec> actions{
        {"account.deleteCancel", T("action.cancel"), ButtonStyle::plain, [&ctx] { ctx.close(); }, ""},
        {"account.deleteConfirm", T("account.delete"), ButtonStyle::secondary, [&ctx] {
            ctx.ui.reload_account_inputs = true;
            if (ctx.app.delete_account()) {
                ctx.ui.account_feedback = "account.deleted"; ctx.ui.reload_account_inputs = true; ctx.close();
            } else { ctx.ui.account_feedback = "account.deleteFailed"; ctx.refresh(); }
        }, "", ctx.app.can_delete_account()}
    };
    std::vector<std::function<void(Builder&)>> cells{[](Builder&) {}};
    std::vector<float> widths{0};
    for (const auto& action : actions) {
        widths.push_back(std::min(b.width(), b.measure_button(action.label, action.icon)));
        cells.push_back([action](Builder& row) {
            row.button(action.id, action.label, action.style, action.action, action.icon, false, action.enabled);
        });
    }
    b.row(std::move(cells), std::move(widths), tokens::kSpaceM, 1);
}

}  // namespace szu::app
