import Foundation

// Pure hold and planning scenarios. AppModelOfflineTests covers orchestration;
// the C++ core tests cover the shared scenario matrix.
@main enum AutoRecoveryScenarioTests {
    static var failures = 0
    static var passed = 0
    static func check(_ condition: Bool, _ message: String) {
        if condition { passed += 1 } else { failures += 1; print("FAIL:", message) }
    }
    static let both = CampusDiscovery(teaching: .confirmed, dormitory: .confirmed)
    static let teachingOnly = CampusDiscovery(teaching: .confirmed, dormitory: .unconfirmed)
    static let areaRefusal = LoginAttempt(outcome: .rejected, rejection: .areaMismatch, error: .rejected)

    static func main() {
        // A teaching-area refusal must not block a separately confirmed dormitory portal.
        var book = HoldBook()
        book.recordFailure(automaticChoice: true, area: .teaching, generation: 1, epoch: 1, attempt: areaRefusal)
        check(book.globalHold(generation: 1, epoch: 1) == nil, "candidate refusal is not global")
        check(CampusPlan.decide(manual: false, choice: .automatic, discovery: both, trusted: true, holds: book, generation: 1, epoch: 1)
              == .submit(.dormitory), "background still submits to the dormitory")
        check(CampusPlan.decide(manual: false, choice: .automatic, discovery: teachingOnly, trusted: true, holds: book, generation: 1, epoch: 1)
              == .paused, "held teaching candidate pauses on a teaching-only network")
        // Network change releases a non-sticky candidate hold.
        book.pathChanged(to: 2)
        check(book.blocking(.teaching, generation: 1, epoch: 2) == nil, "path change releases candidate hold")
        // Repeated area mismatch describes this network, never permanent credential failure.
        book.recordFailure(automaticChoice: true, area: .teaching, generation: 1, epoch: 2, attempt: areaRefusal)
        let mismatch = book.blocking(.teaching, generation: 1, epoch: 2)
        check(mismatch?.sticky == false && mismatch?.epoch == 2, "second area mismatch remains network scoped")
        book.pathChanged(to: 3)
        check(book.blocking(.teaching, generation: 1, epoch: 3) == nil, "repeated area mismatch expires on network change")
        // Repeated unknown candidate refusals retain persistent protection.
        let unknownRefusal = LoginAttempt(outcome: .rejected, rejection: .unspecified, error: .rejected)
        var unknown = HoldBook()
        unknown.recordFailure(automaticChoice: true, area: .teaching, generation: 1, epoch: 1, attempt: unknownRefusal)
        check(unknown.blocking(.teaching, generation: 1, epoch: 1)?.sticky == false, "first unknown refusal remains network scoped")
        unknown.pathChanged(to: 2)
        unknown.recordFailure(automaticChoice: true, area: .teaching, generation: 1, epoch: 2, attempt: unknownRefusal)
        check(unknown.blocking(.teaching, generation: 1, epoch: 3)?.sticky == true, "second unknown candidate refusal is sticky")
        // Success on that area with the same generation releases it.
        let released = unknown.recordSuccess(area: .teaching, generation: 1, epoch: 3)
        check(!released.isEmpty && unknown.holds.isEmpty, "success releases same-generation holds")

        // An unspecified refusal of an explicit area creates a sticky area hold.
        var explicit = HoldBook()
        explicit.recordFailure(automaticChoice: false, area: .teaching, generation: 1, epoch: 1,
                               attempt: LoginAttempt(outcome: .rejected, error: .rejected))
        check(explicit.blocking(.teaching, generation: 1, epoch: 9)?.sticky == true, "explicit area refusal is sticky")
        check(CampusPlan.decide(manual: true, choice: .teaching, discovery: both, trusted: false, holds: explicit, generation: 1, epoch: 9)
              == .submit(.teaching), "manual rounds are never blocked by holds")

        // Password refusal of an automatic candidate is sticky immediately.
        var password = HoldBook()
        password.recordFailure(automaticChoice: true, area: .dormitory, generation: 1, epoch: 1,
                               attempt: LoginAttempt(outcome: .rejected, rejection: .password, error: .rejected))
        check(password.blocking(.dormitory, generation: 1, epoch: 5)?.sticky == true, "password refusal is sticky")

        // Unsaved draft never creates holds for the saved account.
        var draft = HoldBook()
        check(draft.recordFailure(automaticChoice: true, area: .teaching, generation: nil, epoch: 1, attempt: areaRefusal) == nil, "draft rejection holds nothing")
        check(draft.recordFailure(automaticChoice: false, area: .teaching, generation: nil, epoch: 1,
                                  attempt: LoginAttempt(outcome: .invalidCredentials)) == nil, "draft invalid credentials holds nothing")

        // Unavailable / challengeRefused / cancelled never hold.
        var none = HoldBook()
        for outcome in [LoginOutcome.unavailable, .challengeRefused, .cancelled] {
            check(none.recordFailure(automaticChoice: true, area: .teaching, generation: 1, epoch: 1, attempt: LoginAttempt(outcome: outcome)) == nil,
                  "\(outcome) creates no hold")
        }

        // Security and uncertain are global; verified connectivity settles only uncertain.
        var global = HoldBook()
        global.recordFailure(automaticChoice: true, area: .teaching, generation: 1, epoch: 1, attempt: LoginAttempt(outcome: .uncertain))
        global.recordFailure(automaticChoice: true, area: .teaching, generation: 1, epoch: 1, attempt: LoginAttempt(outcome: .security))
        check(global.globalHold(generation: 1, epoch: 1)?.reason == .security, "security outranks uncertain")
        check(global.settleUncertain() && global.globalHold(generation: 1, epoch: 1)?.reason == .security, "settle keeps security")

        // Credentials change prunes other generations; user pause is global until resume.
        var change = HoldBook()
        change.recordFailure(automaticChoice: false, area: .dormitory, generation: 1, epoch: 1, attempt: areaRefusal)
        change.credentialsChanged(to: 2)
        check(change.holds.isEmpty, "saving a different account prunes holds of the old one")
        change.pauseByUser()
        check(change.globalHold(generation: 2, epoch: 1)?.reason == .userPaused, "user pause is global")
        change.resume()
        check(change.holds.isEmpty, "resume clears every hold")

        // Dormitory choice needs trust in the background, never for manual rounds.
        check(CampusPlan.decide(manual: false, choice: .dormitory, discovery: both, trusted: false, holds: HoldBook(), generation: 1, epoch: 1)
              == .needsTrust, "dormitory background needs trust")
        check(CampusPlan.decide(manual: true, choice: .dormitory, discovery: CampusDiscovery(teaching: .unconfirmed, dormitory: .notChecked),
                                trusted: false, holds: HoldBook(), generation: 1, epoch: 1) == .submit(.dormitory), "manual dormitory submits")

        print("\(passed) auto-recovery assertions passed, \(failures) failed.")
        if failures > 0 { exit(1) }
    }
}
