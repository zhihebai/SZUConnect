import Foundation

private actor InformationConnection: ConnectionService {
    private(set) var probes = 0
    func probe() async -> ConnectivityReport { probes += 1; return ConnectivityReport(apple: true, microsoft: true) }
    func confirmedCampusArea(area: CampusArea) async -> CampusArea? { nil }
    func login(credentials: Credentials, area: CampusArea) async throws -> String { fatalError("No authentication") }
}

private actor InformationReader {
    private(set) var calls = 0
    var gate: CheckedContinuation<Void, Never>?
    func read(gated: Bool = false, fails: Bool = false) async throws -> PathObservation {
        calls += 1
        if gated { await withCheckedContinuation { gate = $0 } }
        if fails { throw CocoaError(.fileReadUnknown) }
        return PathObservation(usable: true, interfaces: "fixture-wifi", constrained: false,
            continuity: NetworkContinuitySnapshot(identity: NetworkIdentity(fingerprint: "fixture-information-a"), linkActive: true))
    }
    func release() { gate?.resume(); gate = nil }
}

@main @MainActor
struct NetworkInformationAppModelTests {
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
        let service = InformationConnection()
        let reader = InformationReader()
        let model = AppModel(client: service, isolated: true, readOnly: true,
            networkInformationReader: { try await reader.read() })
        model.applyEnvironmentObservation(try await reader.read())
        model.checkConnection(); try await wait { !model.busy }
        let previous = model.lastCheckedAt
        let probes = await service.probes
        model.refreshDisplayedNetwork(); try await wait { !model.networkInformationRefreshing }
        expect(model.networkInformationFeedback?.contains("连接信息已刷新") == true,
               "A completed information refresh provides feedback and a time")
        let afterRefresh = await service.probes
        expect(model.lastCheckedAt == previous && model.lastInternetCheck?.base.online == true && afterRefresh == probes,
               "Information refresh preserves detection results without launching another connectivity probe")
        expect(!model.currentNetworkAuthorized && model.sessionReadOnly, "Read-only information refresh cannot grant campus trust")
        let failedReader = InformationReader()
        let failed = AppModel(isolated: true, networkInformationReader: { try await failedReader.read(fails: true) })
        failed.refreshDisplayedNetwork(); try await wait { !failed.networkInformationRefreshing }
        expect(failed.networkInformationFeedback == SharedCopy.t("env.refreshFailed"), "An information read failure is visible")
        let delayedReader = InformationReader()
        let delayed = AppModel(isolated: true, networkInformationReader: { try await delayedReader.read(gated: true) })
        delayed.receivePath(usable: true, interfaces: "fixture-wifi", identity: NetworkIdentity(fingerprint: "fixture-information-a"))
        delayed.refreshDisplayedNetwork(); delayed.refreshDisplayedNetwork()
        try await wait { await delayedReader.calls == 1 }
        delayed.receivePath(usable: true, interfaces: "fixture-wifi-b", identity: NetworkIdentity(fingerprint: "fixture-information-b"))
        await delayedReader.release(); try await wait { !delayed.networkInformationRefreshing }
        expect(await delayedReader.calls == 1 && delayed.networkInformationFeedback == SharedCopy.t("env.refreshChanged"),
               "Duplicate refreshes coalesce and a stale observation cannot replace a changed attachment")
        print("Network information refresh: \(checks) assertions passed; isolated observations only")
    }
}
