import Foundation

@main
struct ConnectionFormDisclosureTests {
    static func main() {
        var passed = 0
        func check(_ condition: Bool, _ name: String) {
            guard condition else { fatalError("FAIL: \(name)") }
            passed += 1
            print("PASS: \(name)")
        }
        var state = ConnectionFormDisclosureState()
        check(!state.accountExpanded(saved: true, hasDraft: false), "saved account starts collapsed")
        check(state.accountExpanded(saved: false, hasDraft: false), "first account remains reachable")
        check(state.accountExpanded(saved: true, hasDraft: true), "unsaved draft is visible")
        check(!state.websitesExpanded, "websites start collapsed")
        state.setAccountExpanded(false, saved: true, hasDraft: true)
        check(state.accountExpanded(saved: true, hasDraft: true), "collapse cannot hide an unsaved save/discard path")
        state.credentialStateChanged(saved: true, hasDraft: false)
        check(!state.accountExpanded(saved: true, hasDraft: false), "saving or discarding returns clean account to summary")
        state.reveal("account")
        check(state.accountExpanded(saved: true, hasDraft: false), "account navigation opens editor before focus")
        check(!state.websitesExpanded, "account navigation does not open websites")
        state.reveal("websites")
        check(state.websitesExpanded && state.accountExpanded(saved: true, hasDraft: false), "website navigation is independent of account editor")
        state.setWebsitesExpanded(false)
        check(!state.websitesExpanded && state.accountExpanded(saved: true, hasDraft: false), "website collapse preserves account disclosure")
        state.setAccountExpanded(false, saved: true, hasDraft: false)
        check(!state.accountExpanded(saved: true, hasDraft: false), "user can collapse a clean saved account")
        state.setAccountExpanded(false, saved: false, hasDraft: false)
        check(state.accountExpanded(saved: false, hasDraft: false), "unsaved first account cannot be hidden")
        state.reveal("network")
        check(!state.websitesExpanded, "unrelated navigation leaves optional websites collapsed")
        var licenses = LicenseDisclosureState()
        check(!licenses.groupExpanded && licenses.expandedIDs.isEmpty, "new About license group starts collapsed")
        licenses.groupExpanded = true
        licenses.setExpanded("project", true)
        licenses.setExpanded("curl", true)
        licenses.groupExpanded = false
        check(licenses.isExpanded("project") && licenses.isExpanded("curl"), "group collapse preserves individual license choices")
        licenses.groupExpanded = true
        licenses.setExpanded("project", false)
        check(!licenses.isExpanded("project") && licenses.isExpanded("curl"), "each license remains independently collapsible")
        let reopenedAbout = LicenseDisclosureState()
        check(!reopenedAbout.groupExpanded && reopenedAbout.expandedIDs.isEmpty, "reopened About starts with fresh disclosure state")
        print("\(passed) form and license disclosure assertions passed")
    }
}
