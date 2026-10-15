import Foundation

// Auto-recovery readiness of credentials, network eligibility and protection state.
// Mirrors C++ status::readiness and tools/reference/memory_reference.py (shared/fixtures/readiness-cases.json).

struct ReadinessInput: Equatable, Sendable {
    var mode = "manual"
    var readOnly = false
    var hasSaved = false
    var unsavedChanges = false
    var autoLogin = "off"
    var hold: String?
    var sessionTrusted = false
    var remembered = false
    var memoryConflict = false
    var networkIdentified = true
    var sessionOnlyNetwork = false
    var campusVerification = "notChecked"
}

struct ReadinessItem: Equatable, Identifiable, Sendable {
    let kind: String
    let key: String
    let tone: String
    var id: String { kind }
}

struct Readiness: Equatable, Sendable {
    var title: String
    var tone: String
    var items: [ReadinessItem]
}

enum ReadinessPresenter {
    static func make(_ input: ReadinessInput) -> Readiness {
        guard input.mode == "recover", !input.readOnly else { return Readiness(title: "ready.title.off", tone: "neutral", items: []) }
        let a = input.autoLogin
        let account: ReadinessItem
        if !input.hasSaved { account = ReadinessItem(kind: "account", key: "ready.account.missing", tone: "warning") }
        else if input.unsavedChanges { account = ReadinessItem(kind: "account", key: "ready.account.draft", tone: "warning") }
        else { account = ReadinessItem(kind: "account", key: "ready.account.saved", tone: "success") }
        let network: ReadinessItem
        if input.hasSaved && !input.networkIdentified {
            network = ReadinessItem(kind: "network", key: "ready.network.identifying", tone: "warning")
        } else if a == "needsTrust" {
            network = input.memoryConflict ? ReadinessItem(kind: "network", key: "ready.network.conflict", tone: "danger")
                                           : ReadinessItem(kind: "network", key: "ready.network.needsTrust", tone: "warning")
        } else if a == "awaitingProof" { network = ReadinessItem(kind: "network", key: "ready.network.awaiting", tone: "warning") }
        else if input.sessionTrusted && !input.remembered { network = ReadinessItem(kind: "network", key: input.sessionOnlyNetwork ? "ready.network.trustedCombination" : "ready.network.trusted", tone: input.campusVerification == "confirmed" ? "success" : "info") }
        else if input.sessionOnlyNetwork { network = ReadinessItem(kind: "network", key: "ready.network.sessionAvailable", tone: "info") }
        else if input.remembered { network = ReadinessItem(kind: "network", key: "ready.network.remembered", tone: input.campusVerification == "confirmed" ? "success" : "info") }
        else { network = ReadinessItem(kind: "network", key: "ready.network.notNeeded", tone: "neutral") }
        let hold = input.hold ?? ""
        let protection: ReadinessItem
        if a == "paused" {
            protection = ReadinessItem(kind: "protection", key: "ready.protection.paused",
                                       tone: (hold == "credentials" || hold == "security") ? "danger" : "warning")
        } else if a == "coolingDown" { protection = ReadinessItem(kind: "protection", key: "ready.protection.cooldown", tone: "info") }
        else if a == "limited" { protection = ReadinessItem(kind: "protection", key: "ready.protection.limited", tone: "warning") }
        else { protection = ReadinessItem(kind: "protection", key: "ready.protection.ok", tone: "success") }
        let items = [account, network, protection]
        if a == "paused" { return Readiness(title: "ready.title.paused", tone: protection.tone, items: items) }
        let danger = items.contains { $0.tone == "danger" }
        let warning = items.contains { $0.tone == "warning" }
        let campusPending = input.campusVerification != "confirmed"
        return Readiness(title: (danger || warning) ? "ready.title.attention" : campusPending ? "ready.title.campusPending" : "ready.title.ready",
                         tone: danger ? "danger" : warning ? "warning" : campusPending ? "info" : "success", items: items)
    }
}
