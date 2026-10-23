import Foundation

/// Editing choices are transient presentation state; hiding a field never changes its draft.
struct ConnectionFormDisclosureState {
    private(set) var accountRequested = false
    private(set) var websitesExpanded = false

    func accountExpanded(saved: Bool, hasDraft: Bool) -> Bool {
        !saved || hasDraft || accountRequested
    }
    mutating func setAccountExpanded(_ expanded: Bool, saved: Bool, hasDraft: Bool) {
        // A first account or unsaved changes always retain a visible save/discard path.
        accountRequested = expanded || !saved || hasDraft
    }
    mutating func credentialStateChanged(saved: Bool, hasDraft: Bool) {
        if saved && !hasDraft { accountRequested = false }
    }
    mutating func setWebsitesExpanded(_ expanded: Bool) { websitesExpanded = expanded }
    mutating func reveal(_ section: String) {
        if section == "account" { accountRequested = true }
        if section == "websites" { websitesExpanded = true }
    }

}

/// Individual license choices survive a temporary group collapse, within this About presentation only.
struct LicenseDisclosureState {
    var groupExpanded = false
    private(set) var expandedIDs: Set<String> = []
    func isExpanded(_ id: String) -> Bool { expandedIDs.contains(id) }
    mutating func setExpanded(_ id: String, _ expanded: Bool) {
        if expanded { expandedIDs.insert(id) } else { expandedIDs.remove(id) }
    }
}
