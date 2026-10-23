import Foundation
import Combine

private actor InformationConnection: ConnectionService {
    private(set) var probes = 0
    private(set) var discoveries: [CampusArea] = []
    private(set) var logins = 0
    func probe() async -> ConnectivityReport { probes += 1; return ConnectivityReport(apple: true, microsoft: true) }
    func confirmedCampusArea(area: CampusArea) async -> CampusArea? { nil }
    func setExpectedPhysicalRoute(_ route: CampusPhysicalRoute?) async -> Bool { true }
    func discoverCampus(area: CampusArea) async -> CampusDiscovery {
        discoveries.append(area)
        return CampusDiscovery(teaching: .confirmed, dormitory: .notChecked)
    }
    func login(credentials: Credentials, area: CampusArea) async throws -> String { logins += 1; return "Unexpected fixture login" }
    func attemptLogin(credentials: Credentials, area: CampusArea) async -> LoginAttempt {
        logins += 1
        return LoginAttempt(outcome: .unavailable)
    }
    func counts() -> (probes: Int, discoveries: Int, logins: Int) { (probes, discoveries.count, logins) }
}

private actor InformationReader {
    private(set) var calls = 0
    var gate: CheckedContinuation<Void, Never>?
    func read(gated: Bool = false, fails: Bool = false, result: PathObservation? = nil) async throws -> PathObservation {
        calls += 1
        if gated { await withCheckedContinuation { gate = $0 } }
        if fails { throw CocoaError(.fileReadUnknown) }
        if let result { return result }
        return PathObservation(usable: true, interfaces: "fixture-wifi", constrained: false,
            continuity: NetworkContinuitySnapshot(identity: NetworkIdentity(fingerprint: "fixture-information-a"), linkActive: true))
    }
    func release() { gate?.resume(); gate = nil }
}

@main @MainActor
struct NetworkInformationAppModelTests {
    private static func staticTeachingEvidence() -> CampusPhysicalNetwork.Evidence {
        let descriptor = NetworkDescriptor(kind: "ethernet", raw: [
            "gwip": "192.0.2.1", "net": "192.0.2.0/24", "dns": "192.0.2.53"
        ])
        let service = CampusPhysicalNetwork.Service(interface: "fixture-private-ethernet", router: "192.0.2.1",
            addresses: ["192.0.2.17"], masks: ["255.255.255.0"], linkActive: true,
            dhcpDNS: [], serviceDNS: ["192.0.2.53"], evidence: "fixture-private-record", descriptor: descriptor)
        let physicalDefault = NetworkContinuitySnapshot.PhysicalDefault(interface: "fixture-private-ethernet", gateway: "192.0.2.1")
        return CampusPhysicalNetwork.Evidence(primaryService: "fixture-private-service", primaryInterface: "fixture-private-ethernet",
            indices: ["fixture-private-ethernet": 17], liveAddresses: ["fixture-private-ethernet": ["192.0.2.17"]],
            services: ["fixture-private-service": service],
            routes: TunnelRouteSnapshot(identityComponents: [], physicalDefaults: [physicalDefault], boundPhysicalDefaults: [physicalDefault]),
            globalEvidence: "fixture-private-global")
    }

    private static func observation(_ report: CampusPhysicalNetwork.CandidateReport) -> PathObservation {
        var continuity = NetworkContinuitySnapshot(identity: nil, linkActive: true)
        continuity.requiresPhysicalBinding = true
        continuity.hasUsablePhysicalLink = true
        return PathObservation(usable: true, interfaces: "fixture-private-ethernet", constrained: false,
            continuity: continuity, physicalCandidates: report.candidates, physicalDiagnostics: report.diagnostics)
    }

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

        let folder = FileManager.default.temporaryDirectory.appendingPathComponent("szu-information-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: folder) }
        let evidence = staticTeachingEvidence()
        let staticReport = CampusPhysicalNetwork.candidateReportStable(evidence, evidence)
        expect(staticReport.candidates.count == 1 && staticReport.candidates[0].descriptor?.kind == "ethernet"
            && staticReport.candidates[0].descriptor?.raw["dhcp"] == nil,
            "Production physical capture admits a stable static teaching Ethernet fixture without DHCP or SSID")
        let teachingService = InformationConnection()
        let teachingMemory = folder.appendingPathComponent("teaching-memory.json")
        let teaching = AppModel(client: teachingService, isolated: true, memoryStore: teachingMemory)
        teaching.area = .teaching
        teaching.applyEnvironmentObservation(observation(staticReport))
        expect(teaching.physicalNetworkIdentified && teaching.canVerifyAndRememberNetwork && teaching.lastDiscovery == nil,
            "A static teaching Ethernet route can start verification without any previous campus discovery")
        expect(teaching.physicalDiagnostics == staticReport.diagnostics,
            "Physical diagnostics survive selection of the authentication route")
        teaching.verifyAndRememberCurrentNetwork()
        try await wait { !teaching.busy }
        let teachingCalls = await teachingService.counts()
        expect(teaching.networkRemembered && teaching.rememberedNetworks.count == 1
            && teaching.rememberedNetworks[0].area == "teaching" && teaching.rememberedNetworks[0].proofTeaching
            && !teaching.rememberedNetworks[0].proofDormitory && !teaching.currentNetworkAuthorized,
            "Fresh simulated teaching proof saves only teaching memory without granting dormitory trust")
        expect(await teachingService.discoveries == [.teaching] && teachingCalls.probes == 0 && teachingCalls.logins == 0,
            "Remembering static teaching Ethernet performs only teaching discovery, with zero probes or credential submissions")

        var rejectedReport = staticReport
        rejectedReport.candidates = []
        rejectedReport.diagnostics.candidateCount = 0
        rejectedReport.diagnostics.rejections = ["missingDNS": 1, "inactiveLink": 1, "fixture-private-marker-192.0.2.17": 9]
        rejectedReport.diagnostics.physicalInterfaceCount = 5000
        rejectedReport.diagnostics.serviceCount = -1
        let rejectedObservation = observation(rejectedReport)
        let staticObservation = observation(staticReport)
        let quietService = InformationConnection()
        let quiet = AppModel(client: quietService, isolated: true, readOnly: true)
        quiet.applyEnvironmentObservation(staticObservation)
        let quietRevision = quiet.networkRevision
        var modelPublications = 0
        var diagnosticsPublications = 0
        let modelSubscription = quiet.objectWillChange.sink { modelPublications += 1 }
        let diagnosticsSubscription = quiet.$physicalDiagnostics.dropFirst().sink { _ in diagnosticsPublications += 1 }
        for _ in 0..<100 { quiet.applyEnvironmentObservation(staticObservation) }
        print("Repeated unchanged environment: \(modelPublications) model publications, \(diagnosticsPublications) diagnostics publications")
        expect(modelPublications == 0 && diagnosticsPublications == 0,
            "An unchanged environment poll publishes nothing, avoiding dashboard and sheet invalidation")
        var diagnosticChange = staticObservation
        diagnosticChange.physicalDiagnostics?.rejections["inactiveLink"] = 1
        quiet.applyEnvironmentObservation(diagnosticChange)
        expect(modelPublications == 1 && diagnosticsPublications == 1
            && quiet.physicalDiagnostics == diagnosticChange.physicalDiagnostics && quiet.networkRevision == quietRevision,
            "Changed anonymous diagnostics publish once without changing network identity or recovery ownership")
        var diagnosticClear = staticObservation
        diagnosticClear.physicalDiagnostics = nil
        quiet.applyEnvironmentObservation(diagnosticClear)
        quiet.applyEnvironmentObservation(diagnosticClear)
        expect(modelPublications == 2 && diagnosticsPublications == 2 && quiet.physicalDiagnostics == nil,
            "Removing available diagnostics publishes once; a repeated absent report stays silent")
        let quietCalls = await quietService.counts()
        expect(quietCalls.probes == 0 && quietCalls.discoveries == 0 && quietCalls.logins == 0
            && quiet.sessionReadOnly && !quiet.currentNetworkAuthorized,
            "Diagnostic publication coalescing performs no network operation and grants no trust")
        withExtendedLifetime((modelSubscription, diagnosticsSubscription)) {}

        for readOnly in [false, true] {
            let localService = InformationConnection()
            let localReader = InformationReader()
            let systemService = InformationConnection()
            let model = AppModel(client: localService, isolated: true, readOnly: readOnly,
                isolatedCredentials: Credentials(account: "fixture-information-account", password: "fixture-information-password"),
                runAutomaticLoopInIsolation: true,
                systemConnectivityProbe: { _ in await systemService.probe() },
                networkInformationReader: { try await localReader.read(result: rejectedObservation) })
            model.setMonitoringMode(.recover)
            let revision = model.networkRevision
            let previousWiFiStatus = model.wifiRecovery.status
            let previousPermission = model.wifiRecovery.permissionStatus
            model.refreshPhysicalIdentification()
            model.refreshPhysicalIdentification()
            try await wait { !model.networkInformationRefreshing }
            let calls = await localService.counts()
            let systemCalls = await systemService.counts()
            expect(await localReader.calls == 1 && calls.probes == 0 && calls.discoveries == 0 && calls.logins == 0 && systemCalls.probes == 0,
                "Local identification refresh coalesces without any probe, discovery or authentication in \(readOnly ? "read-only" : "automatic") mode")
            expect(model.networkRevision == revision && !model.networkAvailable && model.nextCheckAt == nil
                && !model.currentNetworkAuthorized && !model.physicalNetworkIdentified && !model.canVerifyAndRememberNetwork,
                "Local diagnostics never apply new identity, authorize a route, enable verification or schedule automatic recovery")
            expect(model.wifiRecovery.status == previousWiFiStatus && model.wifiRecovery.permissionStatus == previousPermission,
                "Local diagnostics preserve Wi-Fi recovery and permission state")
            expect(model.physicalIdentificationDetail == SharedCopy.t("env.identification.reason.missingDNS"),
                "An inactive second interface does not hide the concrete missing-DNS rejection")
            let pendingExport = try JSONSerialization.jsonObject(with: model.diagnosticExportData) as! [String: Any]
            let pendingEnvironment = pendingExport["environment"] as? [String: Any]
            expect(pendingEnvironment?["refreshing"] as? Bool == true && pendingExport["physicalNetwork"] == nil,
                "Local-only identification does not advertise an unapplied environment as a current physical snapshot")

            // Applying a separately sampled observation establishes diagnostic freshness.
            // Keep it separate from the local-only action: that action must not schedule recovery.
            let projection = AppModel(client: InformationConnection(), isolated: true, readOnly: true,
                isolatedCredentials: Credentials(account: "fixture-information-account", password: "fixture-information-password"))
            projection.applyEnvironmentObservation(rejectedObservation)
            let encoded = projection.diagnosticExportData
            let exported = try JSONSerialization.jsonObject(with: encoded) as! [String: Any]
            guard let physical = exported["physicalNetwork"] as? [String: Any],
                  let reasons = physical["rejections"] as? [String: Int] else {
                fatalError("Fresh diagnostic physical snapshot was not exported")
            }
            expect(physical["physicalInterfaceCount"] as? Int == 4096 && physical["serviceCount"] as? Int == 0
                && reasons == ["missingDNS": 1, "inactiveLink": 1],
                "Anonymous physical export bounds counts and permits only fixed rejection codes")
            let exportText = String(data: encoded, encoding: .utf8)!
            expect(!exportText.contains("192.0.2.") && !exportText.contains("fixture-private")
                && !exportText.contains("fixture-information-account") && !exportText.contains("fixture-information-password")
                && projection.diagnosticSummary.contains(projection.physicalIdentificationDetail),
                "Copied and exported diagnostics include the same explanation without addresses, service names or credentials")
            expect(projection.diagnosticSummary.contains("physical interfaces=4096 services=0 candidates=0 reasons=inactiveLink=1,missingDNS=1")
                && !projection.diagnosticSummary.contains("fixture-private-marker"),
                "Copied diagnostics contain bounded counts and every sorted fixed rejection code without raw sentinel values")
        }

        var unavailableContinuity = NetworkContinuitySnapshot(identity: nil, linkActive: false)
        unavailableContinuity.requiresPhysicalBinding = true
        let unavailableObservation = PathObservation(usable: false, interfaces: "", constrained: false,
            continuity: unavailableContinuity,
            physicalDiagnostics: .init(physicalInterfaceCount: 1, serviceCount: 0, candidateCount: 0,
                rejections: ["noIPv4Service": 1]))
        let unavailable = AppModel(isolated: true, readOnly: true)
        unavailable.applyEnvironmentObservation(unavailableObservation)
        expect(!unavailable.networkAvailable && !unavailable.physicalNetworkIdentified && unavailable.physicalIdentificationNeeded,
            "An unsatisfied path with no live physical link still offers diagnostics actions when a local rejection was captured")
        expect(unavailable.physicalIdentificationDetail == SharedCopy.t("env.identification.reason.noIPv4Service"),
            "An unavailable IPv4 path explains the actual captured rejection without claiming the network is available")

        let previouslyIdentifiedReader = InformationReader()
        let previouslyIdentified = AppModel(isolated: true,
            networkInformationReader: { try await previouslyIdentifiedReader.read(result: rejectedObservation) })
        previouslyIdentified.applyEnvironmentObservation(staticObservation)
        expect(previouslyIdentified.physicalNetworkIdentified
            && previouslyIdentified.physicalIdentificationDetail == SharedCopy.t("env.identification.ready"),
            "A selected route and a currently eligible candidate report may be summarized as identified")
        previouslyIdentified.refreshPhysicalIdentification()
        try await wait { !previouslyIdentified.networkInformationRefreshing }
        expect(previouslyIdentified.physicalNetworkIdentified
            && previouslyIdentified.physicalIdentificationDetail == SharedCopy.t("env.identification.reason.missingDNS"),
            "A fresh rejected local snapshot is not concealed by the model's previously identified route")

        let localFailure = AppModel(isolated: true, networkInformationReader: { throw CocoaError(.fileReadNoPermission) })
        localFailure.refreshPhysicalIdentification()
        try await wait { !localFailure.networkInformationRefreshing }
        expect(localFailure.physicalDiagnostics == nil && localFailure.networkInformationFeedback == SharedCopy.t("env.identification.readFailed"),
            "Local capture failure stays explicit rather than inventing an empty network configuration")

        let localDelayedReader = InformationReader()
        let localDelayed = AppModel(isolated: true,
            networkInformationReader: { try await localDelayedReader.read(gated: true, result: staticObservation) })
        localDelayed.applyEnvironmentObservation(rejectedObservation)
        localDelayed.refreshPhysicalIdentification()
        try await wait { await localDelayedReader.calls == 1 }
        localDelayed.receivePath(usable: true, interfaces: "fixture-new-attachment", identity: NetworkIdentity(fingerprint: "fixture-new-attachment"))
        await localDelayedReader.release()
        try await wait { !localDelayed.networkInformationRefreshing }
        expect(localDelayed.physicalDiagnostics == rejectedReport.diagnostics
            && localDelayed.networkInformationFeedback == SharedCopy.t("env.refreshChanged"),
            "An outdated local capture cannot replace diagnostics after a real path boundary")
        print("Network information refresh: \(checks) assertions passed; isolated observations only")
    }
}
