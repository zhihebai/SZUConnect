import Foundation

private actor VerificationDelayFixture {
    private(set) var waiting: Int?
    func sleep(_ seconds: Int) async throws {
        waiting = seconds
        defer { waiting = nil }
        try await Task.sleep(for: .seconds(seconds))
    }
}

private actor VerificationConnectionFixture: ConnectionService {
    private(set) var probes = 0
    private(set) var submissions = 0
    func probe() async -> ConnectivityReport {
        probes += 1
        return ConnectivityReport(apple: false, microsoft: false)
    }
    func confirmedCampusArea(area: CampusArea) async -> CampusArea? { .teaching }
    func discoverCampus(area: CampusArea) async -> CampusDiscovery {
        CampusDiscovery(teaching: .confirmed, dormitory: .unconfirmed)
    }
    func login(credentials: Credentials, area: CampusArea) async throws -> String {
        fatalError("The fixture requires classified submissions")
    }
    func attemptLogin(credentials: Credentials, area: CampusArea) async -> LoginAttempt {
        submissions += 1
        return LoginAttempt(outcome: .success, message: "fixture-accepted-submission")
    }
}

@main @MainActor
struct VerificationCancellationAppModelTests {
    static func main() async throws {
        setbuf(stdout, nil)
        var checks = 0
        func expect(_ condition: Bool, _ message: String) {
            precondition(condition, message)
            checks += 1
            print("PASS: " + message)
        }
        func wait(_ condition: () async -> Bool) async throws {
            let deadline = ContinuousClock.now.advanced(by: .seconds(4))
            while !(await condition()) {
                precondition(ContinuousClock.now < deadline, "Fixture did not reach its expected stage")
                try await Task.sleep(for: .milliseconds(2))
            }
        }
        let date = Date(timeIntervalSince1970: 1_800_000_000)
        for delay in [2, 5] {
            for action in ["cancel", "network", "sleep"] {
                let service = VerificationConnectionFixture()
                let delays = VerificationDelayFixture()
                let model = AppModel(client: service, isolated: true, now: { date },
                    isolatedCredentials: Credentials(account: "fixture-verification-account", password: "fixture-verification-password"),
                    verificationSleep: { try await delays.sleep($0) })
                model.area = .teaching
                model.setMonitoringMode(.recover)
                model.receivePath(usable: true, interfaces: "Wi-Fi en0",
                    identity: NetworkIdentity(fingerprint: "fixture-verification-network-a"))
                model.connect()
                try await wait { await delays.waiting == delay }
                expect(await service.submissions == 1, "The isolated flow submitted exactly once before waiting")
                let start = ContinuousClock.now
                switch action {
                case "cancel": model.cancelCurrentOperation()
                case "network":
                    model.receivePath(usable: true, interfaces: "Wi-Fi en0",
                        identity: NetworkIdentity(fingerprint: "fixture-verification-network-b"))
                default:
                    model.setSleeping(true)
                }
                try await wait { await delays.waiting == nil }
                expect(start.duration(to: .now) < .milliseconds(500), "\(action) interrupts the \(delay)-second post-login wait promptly")
                expect(!model.busy && model.cooldownRemaining != nil,
                    "Cancellation releases work while retaining duplicate-submission protection")
                if action == "sleep" {
                    model.setSleeping(false)
                    model.receivePath(usable: true, interfaces: "Wi-Fi en0",
                        identity: NetworkIdentity(fingerprint: "fixture-verification-network-b"))
                }
                let probes = await service.probes
                model.checkConnection()
                try await wait { !model.busy }
                expect(await service.probes > probes, "A new credential-free check is not blocked by the old wait")
                await model.runAutomaticCycle()
                await model.runAutomaticCycle()
                expect(await service.submissions == 1, "Cancelling a post-login wait cannot bypass the automatic login deadline")
            }
        }
        print("Post-login wait cancellation: \(checks) assertions passed")
    }
}
