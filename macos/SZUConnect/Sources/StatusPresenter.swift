import Foundation

// Shared status contract (shared/status-fixtures.json, 31 cases). Mirrors
// windows/SZUConnect/core/src/presentation.cpp and tools/reference/status_reference.py.
struct StatusInput: Equatable {
    var status = "idle"
    var diagnosis: String?
    var probeMode = "quick"
    var passedCount = 0
    var mode = "manual"
    var readOnly = false
    var hasDraft = false
    var autoLogin = "off"
    var hold: String?
    var offerForcedLogin = false
}

struct StatusBanner: Equatable {
    var key: String
    var tone: String
    var action: String?
}

struct StatusPresentation: Equatable {
    var tone = "neutral"
    var headline = ""
    var detail = ""
    var primary: String?
    var secondary: String?
    var chip = ""
    var chipTone = "neutral"
    var banner: StatusBanner?
}

enum StatusPresenter {
    static let busyStatuses: Set<String> = ["checking", "discovering", "loggingIn", "verifying"]

    private static func authAction(_ action: String?) -> Bool {
        guard let action else { return false }
        return ["login", "resume", "trust", "forceLogin", "partialLogin"].contains(action)
    }
    private static func canLogin(_ i: StatusInput) -> Bool { i.hasDraft && !i.readOnly && i.mode != "observe" }
    private static func offlineActions(_ i: StatusInput, _ p: inout StatusPresentation) {
        if i.readOnly || i.mode == "observe" { p.primary = "check"; p.secondary = nil; return }
        if i.hasDraft { p.primary = "login"; p.secondary = "check"; return }
        p.primary = "account"
        p.secondary = "check"
    }

    static func present(_ i: StatusInput) -> StatusPresentation {
        let s = i.status
        var p = StatusPresentation()
        p.headline = "status.\(s).headline"
        p.detail = "status.\(s).detail"
        if s == "noNetwork" { p.tone = "warning"; p.secondary = "network" }
        else if s == "sleeping" { p.tone = "neutral" }
        else if busyStatuses.contains(s) { p.tone = "busy"; p.primary = "cancel" }
        else if s == "online" {
            p.tone = "success"
            p.primary = "check"
            p.detail = i.probeMode == "complete" ? (i.passedCount >= 2 ? "status.online.complete" : "status.online.partial") : "status.online.quick"
            if i.offerForcedLogin { p.secondary = "forceLogin" }
        } else if s == "accepted" { p.tone = "info"; p.primary = "check" }
        else if s == "rejected" { p.tone = "danger"; p.primary = "account"; if canLogin(i) { p.secondary = "login" } }
        else if s == "uncertain" { p.tone = "warning"; p.primary = "check" }
        else if s == "security" { p.tone = "danger"; p.primary = "network"; p.secondary = "check" }
        else if s == "unavailable" { p.tone = "warning"; offlineActions(i, &p) }
        else if s == "unconfirmed" { p.tone = "warning"; p.primary = "network"; p.secondary = "check" }
        else if s == "needsTrust" { p.tone = "warning"; p.primary = "trust"; p.secondary = "check" }
        else if s == "paused" {
            p.tone = "warning"
            p.primary = "resume"
            if canLogin(i) { p.secondary = "login" }
            p.detail = i.hold.map { "hold.\($0).detail" } ?? "status.paused.detail"
        } else if s == "coolingDown" { p.tone = "info"; p.primary = "check" }
        else if s == "confirming" { p.tone = "info"; offlineActions(i, &p) }
        else if s == "needsAccount" { p.tone = "warning"; p.primary = "account"; p.secondary = "check" }
        else if s == "offline" {
            p.tone = "warning"
            if i.diagnosis == "portal" { p.headline = "status.portal.headline" }
            p.detail = i.diagnosis.map { "diagnosis.\($0).detail" } ?? "status.offline.detail"
            offlineActions(i, &p)
        } else if s == "cancelled" { p.tone = "neutral"; p.primary = "check" }
        else { p.tone = "neutral"; p.primary = "check"; if canLogin(i) { p.secondary = "login" } }

        if i.readOnly || i.mode == "observe" {
            if authAction(p.primary) {
                if s != "noNetwork" && s != "sleeping" { p.primary = "check" } else { p.primary = nil }
            }
            if authAction(p.secondary) || p.secondary == p.primary { p.secondary = nil }
        }
        if i.readOnly { p.chip = "auto.readOnly.chip"; p.chipTone = "neutral" }
        else if i.mode != "recover" { p.chip = "auto.\(i.mode).chip"; p.chipTone = "neutral" }
        else {
            p.chip = "auto.\(i.autoLogin).chip"
            let a = i.autoLogin
            p.chipTone = a == "paused" ? "danger" : a == "coolingDown" ? "info" : a == "ready" ? "success" : "warning"
        }
        if i.readOnly { p.banner = StatusBanner(key: "banner.readOnly", tone: "info", action: nil) }
        else if i.mode == "recover" && i.autoLogin == "paused" {
            let hold = i.hold ?? "credentials"
            let tone = (hold == "security" || hold == "credentials") ? "danger" : hold == "uncertain" ? "warning" : "neutral"
            p.banner = StatusBanner(key: "hold.\(hold)", tone: tone, action: "resume")
        } else if i.mode == "recover" && i.autoLogin == "needsTrust" {
            p.banner = StatusBanner(key: "banner.needsTrust", tone: "warning", action: "trust")
        } else if i.mode == "recover" && i.autoLogin == "limited" {
            p.banner = StatusBanner(key: "banner.limited", tone: "warning", action: "guard")
        } else if i.mode == "recover" && i.autoLogin == "awaitingProof" {
            p.banner = StatusBanner(key: "banner.awaitingProof", tone: "info", action: nil)
        }
        return p
    }
}
