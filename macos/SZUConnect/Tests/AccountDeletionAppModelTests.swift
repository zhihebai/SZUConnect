import Foundation

private actor DeletionFixture {
    private(set) var calls = 0
    private(set) var removed = false
    var fails: Bool
    var gate: CheckedContinuation<Void, Never>?
    init(fails: Bool = false) { self.fails = fails }
    func delete(gated: Bool = false) async throws {
        calls += 1
        if gated { await withCheckedContinuation { gate = $0 } }
        if fails { throw CocoaError(.fileWriteNoPermission) }
        removed = true
    }
    func release() { gate?.resume(); gate = nil }
}

private actor DeletionConnection: ConnectionService {
    private(set) var submissions = 0
    private(set) var waiting = false
    private var gate: CheckedContinuation<Void, Never>?
    func probe() async -> ConnectivityReport { ConnectivityReport(apple: false, microsoft: false) }
    func confirmedCampusArea(area: CampusArea) async -> CampusArea? { .teaching }
    func login(credentials: Credentials, area: CampusArea) async throws -> String { fatalError("Use classified fixture") }
    func attemptLogin(credentials: Credentials, area: CampusArea) async -> LoginAttempt {
        submissions += 1
        waiting = true
        await withCheckedContinuation { gate = $0 }
        waiting = false
        return LoginAttempt(outcome: .success, message: "fixture-late-accepted")
    }
    func release() { gate?.resume(); gate = nil }
}

@main @MainActor
struct AccountDeletionAppModelTests {
    static func main() async throws {
        setbuf(stdout, nil)
        var checks = 0
        func expect(_ condition: Bool, _ message: String) {
            precondition(condition, message); checks += 1; print("PASS: " + message)
        }
        func wait(_ condition: () async -> Bool) async throws {
            let end = ContinuousClock.now.advanced(by: .seconds(3))
            while !(await condition()) {
                precondition(.now < end, "Fixture timed out")
                try await Task.sleep(for: .milliseconds(2))
            }
        }
        let credentials = Credentials(account: "fixture-delete-account", password: "fixture-delete-password")
        let service = DeletionConnection()
        let store = DeletionFixture()
        let model = AppModel(client: service, isolated: true, isolatedCredentials: credentials,
                             credentialDeletion: { try await store.delete(gated: true) })
        model.area = .teaching
        model.setMonitoringMode(.recover)
        model.receivePath(usable: true, interfaces: "fixture-wifi", identity: NetworkIdentity(fingerprint: "fixture-delete-network"))
        model.connect()
        try await wait { await service.waiting }
        let deletion = Task { await model.deleteSavedCredentials() }
        try await wait { await store.calls == 1 }
        expect(!model.busy && model.credentialsDeletionInProgress && !model.canAuthenticate,
               "Login ownership is cancelled before deletion yields to the credential store")
        expect(model.cooldownRemaining != nil && model.holds.contains { $0.reason == .uncertain },
               "Deleting during a possible submission retains uncertainty and cooldown")
        model.connect(); model.resumeAutomaticRecovery(); await model.runAutomaticCycle()
        expect(await service.submissions == 1, "No queued or new login starts while deletion is pending")
        await service.release()
        for _ in 0..<20 { await Task.yield() }
        expect(model.lastAttemptOutcome == nil, "The cancelled login's late success cannot mutate the model")
        await store.release(); await deletion.value
        expect(await store.removed && !model.hasSavedCredentials && model.account.isEmpty && model.password.isEmpty,
               "Successful deletion removes the saved snapshot and both input drafts")
        expect(model.cooldownRemaining != nil && model.holds.contains { $0.reason == .uncertain },
               "Successful deletion does not release authentication protections")
        expect(!model.credentialsDeletionFailed && model.credentialsDeletionMessage == SharedCopy.t("account.deleted"),
               "Successful deletion has a clear local result")

        let failing = DeletionFixture(fails: true)
        let failed = AppModel(isolated: true, isolatedCredentials: credentials,
                             credentialDeletion: { try await failing.delete() })
        failed.setMonitoringMode(.recover)
        await failed.deleteSavedCredentials()
        expect(failed.credentialsDeletionFailed && failed.hasSavedCredentials && failed.automaticRecoveryPaused,
               "Failure keeps the saved snapshot, pauses automatic login and exposes a retry")
        await failed.deleteSavedCredentials()
        expect(await failing.calls == 2, "A failed deletion remains retryable")

        let unreadable = DeletionFixture()
        let corrupt = AppModel(isolated: true, credentialDeletion: { try await unreadable.delete() })
        await corrupt.deleteSavedCredentials()
        expect(await unreadable.removed, "Deletion works without decoding a missing or damaged saved credential")

        let readonlyStore = DeletionFixture()
        let readonly = AppModel(isolated: true, readOnly: true, isolatedCredentials: credentials,
            credentialDeletion: { try await readonlyStore.delete() })
        await readonly.deleteSavedCredentials()
        expect(await readonlyStore.calls == 0 && readonly.hasSavedCredentials && readonly.account == credentials.account,
               "Read-only mode cannot invoke even an injected credential deletion")
        print("Account deletion: \(checks) assertions passed; fake credentials only")
    }
}
