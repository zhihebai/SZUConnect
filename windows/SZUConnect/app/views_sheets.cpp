#include <algorithm>
#include <cmath>

#include "shared_copy.hpp"
#include "shared_tokens.hpp"
#include "szu/win/system.hpp"
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

void build_inline_account(Builder& b, ViewContext& ctx, const EngineSnapshot& snap) {
    b.title_row("home.account", "user", T("account.title"), TextStyle::headline, "textPrimary", info_label("account"),
        [&ctx] { ctx.open("info:account"); }, {}, {}, {}, true);
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
    const status::Readiness ready = status::readiness(status::readiness_input_from(snap));
    const bool read_only = ctx.app.read_only();
    const bool recover = snap.mode == MonitoringMode::recover && !read_only;
    b.title_row("home.guard", "shield-check", T("dashboard.guard"), TextStyle::headline, "textPrimary", info_label("auto"),
        [&ctx] { ctx.open("info:auto"); }, recover ? T(ready.title) : std::wstring(), ready.tone,
        recover ? std::string(tone_icon(ready.tone)) : std::string(), true);
    b.gap(tokens::kSpaceS);
    b.segmented("guard.mode", L"", {T("mode.manual.title"), T("mode.observe.title"), T("mode.recover.title")},
        snap.mode == MonitoringMode::manual ? 0 : snap.mode == MonitoringMode::observe ? 1 : 2,
        [&ctx](int i) { ctx.app.set_mode(i == 0 ? MonitoringMode::manual : i == 1 ? MonitoringMode::observe : MonitoringMode::recover); ctx.refresh(); },
        !read_only && ctx.app.settings_writable());
    b.gap(8);
    if (recover && snap.authentication_route) {
        b.text(F("env.authenticationTarget", {{"name", snap.authentication_route->display_name}}), TextStyle::callout_strong, "textPrimary");
        if (!snap.system_interfaces.empty()) {
            b.gap(tokens::kSpaceXs);
            b.text(F("env.systemPrimary", {{"name", snap.system_interfaces}}), TextStyle::caption_strong, "textSecondary");
        }
        b.gap(8);
    }
    b.text(T(status::campus_verification_key(snap)), TextStyle::caption, "textSecondary", Role::status, "status.campusVerification");
    b.gap(tokens::kSpaceS);
    if (read_only) {
        b.text(T("settings.readOnlyLaunch"), TextStyle::caption, "warning");
        return;
    }
    if (!recover) {
        b.text(T("mode." + std::string(to_string(snap.mode)) + ".detail"), TextStyle::caption, "textSecondary");
    }
    if (recover) {
    // readiness: account / network / protection, each with an icon
    std::vector<ChipSpec> chips;
    for (const auto& item : ready.items) chips.push_back({T(item.key), item.tone, std::string(tone_icon(item.tone)), {}});
    b.chips(std::move(chips));
    if (p.banner && p.banner->key != "banner.readOnly") {
        b.gap(tokens::kSpaceS);
        b.banner("guard.banner", p.banner->tone, T(p.banner->key + ".title"), T(p.banner->key + ".detail"), L"", {});
    }
    if (!snap.proof_wait.empty()) {
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
    b.gap(tokens::kSpaceS);
    b.text(T(snap.remembered ? "memory.remembered" : snap.trusted ? "memory.sessionTrusted" : "memory.notRemembered"),
        TextStyle::caption, "textSecondary", Role::status, "guard.memory.state");
    b.gap(tokens::kSpaceS);
    std::vector<ButtonSpec> actions;
    if (recover && snap.has_saved) {
        if (!snap.paused && snap.auto_login == AutoLoginState::limited)
            actions.push_back({"guard.resumeLimited", T("action.resume"), ButtonStyle::secondary,
                [&ctx] { ctx.app.resume(); ctx.refresh(); }, "play"});
        actions.push_back({"guard.pause", T(snap.paused ? "action.resume" : "action.pause"), ButtonStyle::secondary,
            [&ctx, paused = snap.paused] { if (paused) ctx.app.resume(); else ctx.app.pause(); ctx.refresh(); }, snap.paused ? "play" : "pause"});
    }
    if (snap.verifying_memory) {
        actions.push_back({"guard.memory.cancel", T("action.cancel"), ButtonStyle::secondary,
            [&ctx] { ctx.app.cancel(); ctx.refresh(); }, "x"});
    } else if (snap.remembered) {
        actions.push_back({"guard.forget", T("memory.forget"), ButtonStyle::plain, [&ctx] { ctx.app.forget_network(); ctx.refresh(); }, ""});
    } else if (snap.area == CampusArea::teaching) {
        actions.push_back({"guard.remember", T("memory.rememberTeaching"), ButtonStyle::secondary, [&ctx] { ctx.app.remember_network(); ctx.refresh(); },
            "bookmark", snap.can_remember});
    } else if (snap.trusted) {
            actions.push_back({"guard.remember", T("memory.remember"), ButtonStyle::secondary, [&ctx] { ctx.app.remember_network(); ctx.refresh(); },
                "bookmark", snap.can_remember});
            actions.push_back({"guard.revoke", T("action.revokeTrust"), ButtonStyle::plain, [&ctx] { ctx.app.revoke_trust(); ctx.refresh(); }, ""});
    } else {
            actions.push_back({"guard.remember", T("memory.remember"), ButtonStyle::secondary, [&ctx] { ctx.app.remember_network(); ctx.refresh(); },
                "bookmark", snap.can_remember});
            actions.push_back({"guard.once", T("memory.once"), ButtonStyle::plain, [&ctx] { ctx.app.trust(); ctx.refresh(); }, "", snap.can_trust});
    }
    const float memory_top = b.y();
    float action_height = tokens::kSizeMinTarget;
    b.row({[actions = std::move(actions), &action_height](Builder& row) {
            const float top = row.y(); row.buttons(actions); action_height = row.y() - top;
        }, [&ctx, &action_height](Builder& info) {
            info.gap(std::max(0.0f, (action_height - tokens::kSizeMinTarget) / 2));
            info.icon_button("guard.memory.info", info_label("memory"), "info", [&ctx] { ctx.open("info:memory"); });
        }}, {0, tokens::kSizeMinTarget}, tokens::kSpaceS, 1);
    Widget anchor; anchor.kind = Kind::panel; anchor.id = "guard.memory";
    anchor.rect = D2D1::RectF(b.left(), memory_top, b.left() + b.width(), b.y()); b.add(std::move(anchor));
    const auto& feedback = ctx.app.memory_verification_message();
    if (!feedback.empty()) {
        b.gap(tokens::kSpaceS);
        const std::string tone = feedback == "memory.verification.remembered" ? "success" :
            feedback == "memory.verification.checking" || feedback == "memory.verification.pending" ||
            feedback == "memory.verification.busy" ? "textSecondary" : "warning";
        b.text(T(feedback), TextStyle::caption, tone, Role::status, "guard.memory.feedback");
    }
    if (!snap.remembered && !snap.can_remember && !snap.remember_blocker.empty()) {
        b.gap(tokens::kSpaceS);
        const std::string blocker = snap.remember_blocker == "unreliable" && !snap.authentication_route && ctx.app.network().path.descriptors.size() > 1
            ? "multiple" : snap.remember_blocker;
        b.text(blocker == "unreadable" ? T("memory.unreadable") : T("memory.hint." + blocker), TextStyle::caption, "textSecondary");
    }
}

namespace {
void network_sheet(Builder& b, ViewContext& ctx, const EngineSnapshot& snap, bool inline_content = false) {
    AppController& app = ctx.app;
    const auto& net = app.network();
    const auto& proxy = app.proxy_summary();
    const auto rule = [&b] {
        b.gap(tokens::kSpaceM);
        Widget line; line.kind = Kind::divider;
        line.rect = D2D1::RectF(b.left(), b.y(), b.left() + b.width(), b.y()); b.add(std::move(line));
        b.gap(tokens::kSizeBorderWidth + tokens::kSpaceM);
    };
    if (!inline_content) b.heading(T("env.details"));
    // Grouped values precede strategy, diagnosis and optional rules.
    b.text(T("env.intro"), TextStyle::caption, "textSecondary", Role::text, "network.technical.summary");
    b.gap(tokens::kSpaceM);
    if (!net.path.interfaces.empty()) {
        b.text(F("env.interfaces", {{"list", net.path.interfaces}}), TextStyle::callout, "textPrimary");
        b.gap(tokens::kSpaceXs);
    }
    if (snap.authentication_route) {
        b.gap(tokens::kSpaceXs);
        b.text(F("env.authenticationTarget", {{"name", snap.authentication_route->display_name}}), TextStyle::callout_strong, "textPrimary");
    }
    if (!snap.system_interfaces.empty()) {
        b.gap(tokens::kSpaceXs); b.text(F("env.systemPrimary", {{"name", snap.system_interfaces}}), TextStyle::caption, "textSecondary");
    }
    b.gap(tokens::kSpaceXs); b.text(T(status::campus_verification_key(snap)), TextStyle::caption, "textSecondary");

    b.gap(tokens::kSpaceM);
    std::wstring proxy_text = T("dashboard.proxy") + L"：" + T("env.proxy." + proxy.state);
    std::string kinds;
    for (const auto& kind : proxy.kinds) kinds += (kinds.empty() ? "" : "、") + U("env.proxy.kind." + kind);
    if (!kinds.empty()) proxy_text += L"（" + win::widen(kinds) + L"）";
    if (proxy.state == "on") proxy_text += L" · " + T("env.bypass." + proxy.campus_bypass);
    b.text(proxy_text, TextStyle::callout, "textPrimary");
    b.gap(tokens::kSpaceXs); b.text(T("env.note.proxy"), TextStyle::caption, "textSecondary");

    // Windows exposes both actual probe paths; neither is campus identity proof.
    const auto system = app.system_connectivity();
    const auto direct = app.internet_report();
    b.gap(tokens::kSpaceM);
    b.card_begin("surfaceMuted", "", tokens::kSpaceM, tokens::kRadiusMedium);
    b.text(T("env.pathComparison"), TextStyle::callout_strong, "textPrimary"); b.gap(tokens::kSpaceS);
    const float site_width = std::max(72.0f, std::ceil(b.measure_text(L"Microsoft", TextStyle::caption, 10000).width));
    const auto comparison_row = [&b, site_width](const std::wstring& label, const std::wstring& system_text, const std::wstring& direct_text) {
        b.row({[label](Builder& c) { c.text(label, TextStyle::caption, "textSecondary"); },
            [system_text](Builder& c) { c.text(system_text, TextStyle::caption, "textPrimary"); },
            [direct_text](Builder& c) { c.text(direct_text, TextStyle::caption, "textPrimary"); }}, {site_width, 0, 0}, tokens::kSpaceM, 56);
    };
    comparison_row(L"", T("env.systemProbe"), T("env.physicalProbe")); b.gap(tokens::kSpaceS);
    const bool public_check_running = snap.busy && !snap.verifying_memory;
    const bool system_waiting = app.system_connectivity_running() && !system;
    comparison_row(L"Apple", system_waiting ? T("dashboard.checking") : probe_result(system ? &system->apple : nullptr),
        public_check_running ? T("dashboard.checking") : probe_result(direct ? &direct->base.apple : nullptr)); b.gap(tokens::kSpaceS);
    comparison_row(L"Microsoft", system_waiting ? T("dashboard.checking") : probe_result(system ? &system->microsoft : nullptr),
        public_check_running ? T("dashboard.checking") : probe_result(direct ? &direct->base.microsoft : nullptr));
    if (system) {
        b.gap(tokens::kSpaceS);
        b.text(T(system->online() ? "env.systemOnline.title" : "internet.unavailable.title"), TextStyle::callout, "textPrimary");
        b.gap(tokens::kSpaceXs); b.text(T("env.systemProbeNote"), TextStyle::caption, "textSecondary");
    }
    b.card_end();

    rule();
    b.text(T("internet.strategy"), TextStyle::callout_strong, "textPrimary", Role::heading);
    b.gap(tokens::kSpaceS);
    b.segmented("internet.strategy", L"", {T("website.strategy.system.title"), T("website.strategy.direct.title")},
        app.website_follow_system_proxy() ? 0 : 1, [&ctx](int i) { ctx.app.set_website_proxy(i == 0); ctx.refresh(); }, !snap.busy && !app.read_only());
    b.gap(tokens::kSpaceS);
    b.text(T(app.website_follow_system_proxy() ? "website.strategy.system.detail" : "website.strategy.direct.detail"), TextStyle::caption, "textSecondary");
    if (!app.website_configuration_error().empty()) {
        b.gap(tokens::kSpaceXs); b.text(T(app.website_configuration_error()), TextStyle::caption, "warning", Role::status);
    }

    rule();
    b.text(T("compat.title"), TextStyle::callout_strong, "textPrimary", Role::heading);
    b.gap(tokens::kSpaceS); b.text(T("compat.intro"), TextStyle::caption, "textSecondary");
    b.gap(tokens::kSpaceS);
    const CompatState compat = app.compat();
    if (compat.running) {
        b.icon_row("loader", "busy", T("compat.running"), TextStyle::callout, "textPrimary", 14);
        b.gap(tokens::kSpaceS);
        b.button("compat.stop", T("action.stopDiagnosis"), ButtonStyle::secondary, [&ctx] {
            ctx.app.cancel_compat(); ctx.refresh();
        }, "x");
    } else {
        b.button("compat.run", T("action.runDiagnosis"), ButtonStyle::primary, [&ctx] {
            ctx.app.run_compat(); ctx.refresh();
        }, "search");
    }
    if (compat.done) {
        b.gap(tokens::kSpaceS);
        auto cell = [](const std::string& token) { return U("compat.result." + token); };
        const std::string rows[2][3] = {
            {U("compat.row.campus"), cell(compat.input.campus_direct), cell(compat.input.campus_system)},
            {U("compat.row.public"), cell(compat.input.public_direct), cell(compat.input.public_system)}};
        const auto diagnosis_row = [&b](const std::wstring& label, const std::wstring& direct_text, const std::wstring& system_text) {
            b.row({[label](Builder& col) { col.text(label, TextStyle::callout, "textPrimary"); },
                [direct_text](Builder& col) { col.text(direct_text, TextStyle::callout, "textPrimary"); },
                [system_text](Builder& col) { col.text(system_text, TextStyle::callout, "textPrimary"); }}, {72, 0, 0}, tokens::kSpaceL, 56);
        };
        diagnosis_row(L"", T("compat.column.direct"), T("compat.column.system"));
        for (const auto& row : rows) {
            b.gap(tokens::kSpaceS); diagnosis_row(win::widen(row[0]), win::widen(row[1]), win::widen(row[2]));
        }
        b.gap(tokens::kSpaceS);
        const std::string verdict = compat.verdict.verdict;
        const std::string tone = verdict == "allReachable" ? "success" : verdict == "cancelled" ? "neutral" : "warning";
        b.banner("compat.verdict", tone, T("compat.verdict." + verdict + ".title"), T("compat.verdict." + verdict + ".detail"), L"", {});
        for (const auto& rec : compat.verdict.recommendations) {
            b.gap(tokens::kSpaceS); b.text(T("compat.rec." + rec), TextStyle::callout, "textPrimary");
        }
        if (compat.verdict.offer_rules) {
            rule();
            b.text(T("compat.rules.title"), TextStyle::callout_strong, "textPrimary", Role::heading);
            b.gap(tokens::kSpaceS); b.text(T("compat.rules.detail"), TextStyle::caption, "textSecondary");
            b.gap(tokens::kSpaceS);
            b.text(win::widen(compat::rules_text()), TextStyle::mono, "textPrimary");
            b.gap(tokens::kSpaceS);
            const bool copied = ctx.ui.copied == "compat.copyRules";
            b.button("compat.copyRules", T(copied ? "compat.rules.copied" : "compat.rules.copy"), ButtonStyle::secondary, [&ctx] {
                if (ctx.app.copy_rules(ctx.hwnd)) ctx.copied("compat.copyRules"); ctx.refresh();
            }, copied ? "check" : "copy");
        }
    }
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
    b.text(T("setup.helpTitle"), TextStyle::headline, "textPrimary", Role::heading, "help.quickStart");
    b.gap(tokens::kSpaceM);
    b.text(T("setup.helpDetail"), TextStyle::caption, "textSecondary");
    const std::pair<const char*, const char*> steps[] = {
        {"network", "setup.helpNetwork"}, {"memory", "setup.helpMemory"}, {"account", "setup.helpAccount"},
        {"recovery", "setup.helpRecovery"}, {"wifi", "setup.wifi.body"}
    };
    for (const auto& step : steps) {
        b.gap(tokens::kSpaceM);
        b.text(T(std::string("setup.") + step.first + ".title"), TextStyle::callout_strong, "textPrimary", Role::heading);
        b.gap(tokens::kSpaceXs);
        b.text(T(step.second), TextStyle::caption, "textSecondary");
    }
    b.card_end();
    b.gap(tokens::kSpaceL);
    b.card_begin("surface", "border", tokens::kSpaceL, tokens::kRadiusLarge);
    sheet_section_heading(b, "help.faqTitle", "help", T("help.faqTitle"));
    b.gap(tokens::kSpaceM);
    for (const char* q : {"wifi", "cooldown", "noInternet", "rejected", "vpn", "nonCampus", "dorm", "official"}) {
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

void about_sheet(Builder& b, ViewContext&) {
    // About presents brand, developer, credits, then storage and privacy.
    b.card_begin("surface", "border", tokens::kSpaceL, tokens::kRadiusLarge);
    const auto name = T("app.name"), tagline = T("app.tagline");
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
    b.card_end();
    b.gap(tokens::kSpaceL);
    b.card_begin("surface", "border", tokens::kSpaceL, tokens::kRadiusLarge);
    sheet_section_heading(b, "about.creditsTitle", "book", T("about.creditsTitle"));
    b.gap(tokens::kSpaceM);
    b.text(T("about.creditsBody"), TextStyle::callout, "textSecondary");
    for (const Link& link : kCreditLinks) {
        sheet_rule(b);
        sheet_external_link(b, std::string("about.") + link.key, T(std::string("about.") + link.key + ".name"), link.url);
        b.gap(tokens::kSpaceXs);
        b.text(T(std::string("about.") + link.key + ".detail"), TextStyle::caption, "textSecondary");
    }
    sheet_rule(b);
    b.text(T("about.notices"), TextStyle::caption, "textSecondary");
    b.card_end();
    b.gap(tokens::kSpaceL);
    b.card_begin("surface", "border", tokens::kSpaceL, tokens::kRadiusLarge);
    sheet_section_heading(b, "about.storage.title", "lock", T("about.storage.title"));
    b.gap(tokens::kSpaceM);
    b.text(T("about.storage.body"), TextStyle::callout, "textSecondary");
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
    } else if (name == "memory") status_text = T(snap.remembered ? "guide.memory.saved" : "guide.memory.waiting");
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

void build_network_details(Builder& b, ViewContext& ctx, const EngineSnapshot& snap) {
    network_sheet(b, ctx, snap, true);
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
    if (name == "websiteResult" && ctx.ui.website_details) {
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
        b.text(T("memory.namesPrivacy"), TextStyle::caption, "textSecondary");
        if (!ctx.ui.network_name_error.empty()) b.text(T(ctx.ui.network_name_error), TextStyle::caption, "danger", Role::status);
        b.gap(tokens::kSpaceM);
        dialog_actions({{"memory.nameCancel", T("memory.nameCancel"), ButtonStyle::plain, [&ctx] { ctx.close(); }, ""},
            {"memory.nameSave", T("memory.nameSave"), ButtonStyle::primary, [&ctx] {
            if (ctx.app.rename_network(ctx.ui.network_name_id, win::narrow(ctx.ui.network_name_draft))) ctx.close();
            else { ctx.ui.network_name_error = "error.storage"; ctx.refresh(); }
        }, "", !ctx.app.read_only() && !NetworkLabels::clean_name(win::narrow(ctx.ui.network_name_draft)).empty()}});
    } else if (name == "network") network_sheet(b, ctx, snap);
    else if (name == "help") help_sheet(b, ctx);
    else if (name == "about") about_sheet(b, ctx);
    else tutorial(b, ctx, snap, p);
}

}  // namespace szu::app
