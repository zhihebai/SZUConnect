import Foundation

@main
struct RecordSampleSelectionTests {
    static func main() {
        struct Sample: Identifiable {
            let id = UUID()
            let date: Date
            let passed: Bool
        }
        var checks = 0
        func check(_ condition: @autoclosure () -> Bool, _ message: String) {
            guard condition() else { fatalError("FAIL: \(message)") }
            checks += 1
        }
        let date = Date(timeIntervalSinceReferenceDate: 1_000_000)
        let first = Sample(date: date, passed: true)
        let second = Sample(date: date, passed: false)
        let samples = [first, second]
        var selection = RecordSampleSelection()
        check(selection.selected(from: samples) == nil, "No initial selection")
        selection.toggle(second.id)
        check(samples.filter { $0.id == selection.id }.count == 1, "Same-time samples have one selected cell")
        check(selection.selected(from: samples)?.passed == false, "Second same-time sample shows its own result")
        selection.retain(among: samples.reversed().map(\.id))
        check(selection.id == second.id, "Reordering does not change the selected sample")
        selection.toggle(first.id)
        check(selection.selected(from: samples)?.passed == true, "Selecting the other same-time sample changes details")
        selection.toggle(first.id)
        check(selection.id == nil, "Selecting the same cell again closes details")
        selection.toggle(second.id)
        selection.retain(among: [first.id])
        check(selection.id == nil, "Removing the selected sample clears selection")
        selection.toggle(first.id)
        let newSamples = (0..<24).map { Sample(date: date.addingTimeInterval(Double($0 + 1)), passed: true) }
        let recent = Array(([first] + newSamples).suffix(24))
        check(selection.selected(from: recent) == nil, "A sample outside the visible ribbon cannot supply details")
        selection.retain(among: recent.map(\.id))
        check(selection.id == nil, "Rolling beyond the newest 24 clears selection")
        selection.toggle(second.id)
        selection.retain(among: [])
        check(selection.id == nil, "Empty history clears selection")
        selection.toggle(first.id)
        selection.clear()
        check(selection.id == nil, "Explicit close clears selection")
        print("Record sample selection: \(checks) checks passed")
    }
}
