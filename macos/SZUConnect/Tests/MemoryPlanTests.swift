import Foundation

// CampusPlan with remembered-network matches, network-scoped area-mismatch holds,
// and the auto-recovery readiness checklist (shared fixtures). Pure value logic.

@main
struct MemoryPlanTests {
    static func main() {
        var passed = 0
        func check(_ condition: @autoclosure () -> Bool, _ name: String) {
            guard condition() else { fatalError("FAIL: \(name)") }
            passed += 1
            print("PASS: \(name)")
        }
        let root = URL(fileURLWithPath: CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : ".")
        func fixture(_ name: String) -> [String: Any] {
            guard let data = try? Data(contentsOf: root.appendingPathComponent("shared/fixtures/" + name)),
                  let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { fatalError("cannot load \(name)") }
            return object
        }
        func state(_ text: String?) -> DiscoveryState { DiscoveryState(rawValue: text ?? "") ?? .notChecked }
        let doc = fixture("network-memory-cases.json")
        for item in doc["plan"] as? [[String: Any]] ?? [] {
            let id = item["id"] as? String ?? "?"
            let discovery = item["discovery"] as? [String: String] ?? [:]
            var holds = HoldBook()
            for name in item["blocked"] as? [String] ?? [] {
                holds.add(AuthenticationHold(reason: .credentials, area: CampusArea(rawValue: name), generation: 1, epoch: nil,
                                             origin: .explicitArea, rejection: "password"))
            }
            let plan = CampusPlan.decide(manual: item["kind"] as? String == "manual",
                                         choice: CampusArea(rawValue: item["choice"] as? String ?? "") ?? .automatic,
                                         discovery: CampusDiscovery(teaching: state(discovery["teaching"]), dormitory: state(discovery["dormitory"])),
                                         trusted: item["trusted"] as? Bool ?? false, holds: holds, generation: 1, epoch: 1,
                                         memory: MatchLevel(rawValue: item["memory"] as? String ?? "") ?? .none,
                                         ambiguous: item["ambiguous"] as? Bool ?? false)
            let expected = item["expected"] as? [String: Any] ?? [:]
            let decision = expected["decision"] as? String
            let target = expected["target"] as? String
            switch plan {
            case .submit(let area): check(decision == "submit" && target == area.rawValue, "plan \(id)")
            case .paused: check(decision == "paused", "plan \(id)")
            case .unconfirmed: check(decision == "unconfirmed", "plan \(id)")
            case .needsTrust: check(decision == "needsTrust", "plan \(id)")
            }
        }

        // An area mismatch describes the network, never the credentials.
        var book = HoldBook()
        let mismatch = LoginAttempt(outcome: .rejected, rejection: .areaMismatch, error: .rejected)
        _ = book.recordFailure(automaticChoice: true, area: .teaching, generation: 1, epoch: 5, attempt: mismatch)
        let second = book.recordFailure(automaticChoice: true, area: .teaching, generation: 1, epoch: 5, attempt: mismatch)
        check(second != nil && second?.sticky == false && second?.epoch == 5, "repeated area mismatch stays network-scoped")
        book.pathChanged(to: 6)
        check(book.holds.isEmpty, "area mismatch disappears with the network")
        var unknown = HoldBook()
        let other = LoginAttempt(outcome: .rejected, rejection: .unspecified, error: .rejected)
        _ = unknown.recordFailure(automaticChoice: true, area: .teaching, generation: 1, epoch: 5, attempt: other)
        check(unknown.recordFailure(automaticChoice: true, area: .teaching, generation: 1, epoch: 5, attempt: other)?.sticky == true,
              "a second refusal of unknown cause still becomes sticky")

        // ---- readiness checklist
        for item in fixture("readiness-cases.json")["cases"] as? [[String: Any]] ?? [] {
            let id = item["id"] as? String ?? "?"
            let raw = item["input"] as? [String: Any] ?? [:]
            let input = ReadinessInput(mode: raw["mode"] as? String ?? "manual", readOnly: raw["readOnly"] as? Bool ?? false,
                                       hasSaved: raw["hasSaved"] as? Bool ?? false, unsavedChanges: raw["unsavedChanges"] as? Bool ?? false,
                                       autoLogin: raw["autoLogin"] as? String ?? "off", hold: raw["hold"] as? String,
                                       sessionTrusted: raw["sessionTrusted"] as? Bool ?? false, remembered: raw["remembered"] as? Bool ?? false,
                                       memoryConflict: raw["memoryConflict"] as? Bool ?? false,
                                       networkIdentified: raw["networkIdentified"] as? Bool ?? true,
                                       sessionOnlyNetwork: raw["sessionOnlyNetwork"] as? Bool ?? false,
                                       campusVerification: raw["campusVerification"] as? String ?? "notChecked")
            let got = ReadinessPresenter.make(input)
            let expected = item["expected"] as? [String: Any] ?? [:]
            let items = expected["items"] as? [[String: String]] ?? []
            check(got.title == expected["title"] as? String && got.tone == expected["tone"] as? String, "readiness \(id) title and tone")
            check(got.items.count == items.count && zip(got.items, items).allSatisfy { $0.kind == $1["kind"] && $0.key == $1["key"] && $0.tone == $1["tone"] },
                  "readiness \(id) items")
        }
        print("memory plan tests: \(passed) checks passed")
    }
}
