import SwiftUI

@main
struct NetworkDetailsPresentationTests {
    @MainActor static func main() {
        var passed = 0
        func check(_ condition: Bool, _ name: String) {
            precondition(condition, name)
            passed += 1
            print("PASS: \(name)")
        }

        let dashboard = NetworkDetailsPresentationState()
        check(!dashboard.expanded, "A new dashboard keeps network details collapsed")

        // These are the binding semantics used by the row, including a rebuilt page.
        func pageBinding(_ state: NetworkDetailsPresentationState) -> Binding<Bool> {
            Binding(get: { state.expanded }, set: { state.expanded = $0 })
        }
        var row: Binding<Bool>? = pageBinding(dashboard)
        row?.wrappedValue = true
        check(dashboard.expanded, "Opening the row updates its dashboard-owned presentation state")
        row = nil
        row = pageBinding(dashboard)
        check(row?.wrappedValue == true, "Recreating a page retains the open row")
        row?.wrappedValue = false
        row = nil
        check(!pageBinding(dashboard).wrappedValue, "Recreating a page also retains an explicitly collapsed row")

        dashboard.reveal()
        check(pageBinding(dashboard).wrappedValue, "A destination request reveals a previously collapsed row")
        dashboard.reveal()
        check(dashboard.expanded, "Repeating a destination request cannot toggle the row closed")
        pageBinding(dashboard).wrappedValue = false
        check(!dashboard.expanded, "The user can collapse details again after following help")

        let anotherDashboard = NetworkDetailsPresentationState()
        dashboard.reveal()
        check(!anotherDashboard.expanded, "A separate dashboard starts collapsed without inheriting another session")
        check(dashboard.expanded, "A separate dashboard does not reset the existing session")
        print("\(passed) network details presentation assertions passed")
    }
}
