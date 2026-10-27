#if os(macOS)
import Foundation

private final class FakeRadio: WiFiRecoveryRadio, @unchecked Sendable {
    var permitted = true
    var connection: WiFiPhysicalState = .disconnected
    var powered: Bool? = true
    var results: [WiFiScanCandidate] = []
    var scanned: [String] = []
    var powers = 0
    var joins = 0
    var scansOnMain = false
    var changeAfterScan: WiFiPhysicalState?
    var onScan: (() -> Void)?
    var onPower: (() -> Void)?
    var onPowerState: (() -> Void)?
    var failScan = false
    var scanFailures: Set<String> = []
    var associationFailures: Set<String> = []
    var associated: [String] = []
    var onAssociate: (() -> Void)?
    func permissionGranted() -> Bool { permitted }
    func physicalState() -> WiFiPhysicalState { connection }
    func powerState() -> Bool? { onPowerState?(); return powered }
    func setPowerOn() throws { powers += 1; powered = true; onPower?() }
    func scan(ssid: String) throws -> [WiFiScanCandidate] {
        scansOnMain = Thread.isMainThread
        scanned.append(ssid)
        onScan?()
        if let changeAfterScan { connection = changeAfterScan }
        if failScan || scanFailures.contains(ssid) { throw NSError(domain: "SSID-and-secret-must-not-escape", code: 1) }
        return results
    }
    func associateOpen(_ candidate: WiFiScanCandidate) throws {
        joins += 1; associated.append(candidate.ssid); onAssociate?()
        if associationFailures.contains(candidate.ssid) { throw NSError(domain: "association-error-must-not-escape", code: 2) }
    }
}

private final class BlockingScan: @unchecked Sendable {
    private let lock = NSLock()
    private var began = false
    private let completion = DispatchSemaphore(value: 0)
    var started: Bool { lock.lock(); defer { lock.unlock() }; return began }
    func block() { lock.lock(); began = true; lock.unlock(); completion.wait() }
    func resume() { completion.signal() }
}

private actor ControlledService: WiFiRecoveryServicing {
    var requests: [WiFiRecoveryRequest] = []
    var tokens: [WiFiRecoveryAuthorization] = []
    var cancelledOnEntry: [Bool] = []
    private var pending: CheckedContinuation<WiFiRecoveryResult, Never>?
    func recover(_ request: WiFiRecoveryRequest, authorization: WiFiRecoveryAuthorization) async -> WiFiRecoveryResult {
        requests.append(request); tokens.append(authorization); cancelledOnEntry.append(Task.isCancelled)
        return await withCheckedContinuation { pending = $0 }
    }
    func release(_ result: WiFiRecoveryResult = .joined) { pending?.resume(returning: result); pending = nil }
    var count: Int { requests.count }
    var latestValid: Bool { tokens.last?.isValid ?? false }
    var latestRequest: WiFiRecoveryRequest? { requests.last }
    func validCalls(_ expected: Int) -> Bool { count == expected && latestValid && !cancelledOnEntry.contains(true) }
}

/// Only explicit advances move retry time; host load cannot cross a deadline.
private final class ManualClock: @unchecked Sendable {
    private let lock = NSLock()
    private var date = Date(timeIntervalSince1970: 1_800_000_000)
    func now() -> Date { lock.lock(); defer { lock.unlock() }; return date }
    func advance(_ seconds: TimeInterval) { lock.lock(); date.addTimeInterval(seconds); lock.unlock() }
    func set(_ value: Date) { lock.lock(); date = value; lock.unlock() }
}

private actor ManualRetrySleeper {
    struct Ticket: Equatable, Sendable {
        let id: UUID
        let registeredAt: Date
        let deadline: Date
    }
    private struct Pending {
        let ticket: Ticket
        let continuation: CheckedContinuation<Void, Error>
    }
    private let clock: ManualClock
    private let ignoresCancellation: Bool
    private var waiting: [Pending] = []
    private var registered: [Ticket] = []
    private var cancellations: Set<UUID> = []
    private var completed: Set<UUID> = []

    init(_ clock: ManualClock, ignoresCancellation: Bool = false) {
        self.clock = clock
        self.ignoresCancellation = ignoresCancellation
    }
    func sleep(_ seconds: TimeInterval) async throws {
        let id = UUID()
        if !ignoresCancellation { try Task.checkCancellation() }
        try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
                // Cancellation can precede insertion, including while the actor is busy.
                guard ignoresCancellation || (!Task.isCancelled && !cancellations.contains(id)) else {
                    completed.insert(id)
                    continuation.resume(throwing: CancellationError())
                    return
                }
                let date = clock.now()
                let ticket = Ticket(id: id, registeredAt: date, deadline: date.addingTimeInterval(seconds))
                registered.append(ticket)
                waiting.append(Pending(ticket: ticket, continuation: continuation))
            }
        } onCancel: {
            Task { await self.cancel(id) }
        }
        if !ignoresCancellation { try Task.checkCancellation() }
    }
    private func cancel(_ id: UUID) {
        guard !completed.contains(id) else { return }
        cancellations.insert(id)
        guard !ignoresCancellation, let index = waiting.firstIndex(where: { $0.ticket.id == id }) else { return }
        let pending = waiting.remove(at: index)
        completed.insert(id)
        pending.continuation.resume(throwing: CancellationError())
    }
    func tickets() -> [Ticket] { waiting.map(\.ticket) }
    func history() -> [Ticket] { registered }
    func cancellationRequested(_ id: UUID) -> Bool { cancellations.contains(id) }
    func cancelledAndEmpty(_ id: UUID) -> Bool { cancellations.contains(id) && waiting.isEmpty }
    @discardableResult func releaseDue() -> Int {
        let due = waiting.filter { $0.ticket.deadline <= clock.now() }
        let ids = Set(due.map { $0.ticket.id })
        waiting.removeAll { ids.contains($0.ticket.id) }
        for pending in due {
            completed.insert(pending.ticket.id)
            pending.continuation.resume()
        }
        return due.count
    }
    func cancelAll() {
        let pending = waiting
        waiting.removeAll()
        for item in pending {
            completed.insert(item.ticket.id)
            item.continuation.resume(throwing: CancellationError())
        }
    }
}

private actor ObservedRadioService: WiFiRecoveryServicing {
    private let service: CoreWLANRecoveryService
    var results: [WiFiRecoveryResult] = []
    init(_ radio: FakeRadio) { service = CoreWLANRecoveryService(radio: radio) }
    func recover(_ request: WiFiRecoveryRequest, authorization: WiFiRecoveryAuthorization) async -> WiFiRecoveryResult {
        let result = await service.recover(request, authorization: authorization)
        results.append(result)
        return result
    }
}

@main
struct WiFiRecoveryTests {
    @MainActor static func main() async throws {
        var passed = 0
        func check(_ value: Bool, _ title: String) {
            guard value else { fatalError("FAIL: \(title)") }
            passed += 1
            print("PASS: \(title)")
        }
        let first = WiFiRecoveryNetwork(id: "record-a", ssid: "Campus A", displayName: "校园 A")
        let second = WiFiRecoveryNetwork(id: "record-b", ssid: "Campus B", displayName: "校园 B")
        let open = WiFiScanCandidate(id: UUID(), ssid: first.ssid, signal: -40, isOpen: true)
        let encrypted = WiFiScanCandidate(id: UUID(), ssid: first.ssid, signal: -20, isOpen: false)
        let request = WiFiRecoveryRequest(networks: [first], allowPowerOn: false)
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent("szu-wifi-offline-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }

        // The real serialized service, driven exclusively by an in-memory radio.
        func run(_ radio: FakeRadio, power: Bool = false, token: WiFiRecoveryAuthorization = .init(),
                 networks: [WiFiRecoveryNetwork]? = nil) async -> WiFiRecoveryResult {
            await CoreWLANRecoveryService(radio: radio).recover(.init(networks: networks ?? [first], allowPowerOn: power), authorization: token)
        }
        let connected = FakeRadio(); connected.connection = .connected; connected.results = [open]
        check(await run(connected) == .alreadyConnected, "any existing physical network blocks joining")
        check(connected.scanned.isEmpty && connected.joins == 0 && connected.powers == 0, "connected gate causes zero radio actions")
        let uncertain = FakeRadio(); uncertain.connection = .unknown
        check(await run(uncertain) == .connectionUnclear, "unknown physical state fails closed")
        let permission = FakeRadio(); permission.permitted = false
        check(await run(permission) == .permissionRequired && permission.scanned.isEmpty, "missing permission does not scan or request authorization")
        let powerWithoutSetup = FakeRadio(); powerWithoutSetup.permitted = false; powerWithoutSetup.powered = false
        check(await run(powerWithoutSetup, power: true, networks: []) == .poweredOn,
              "explicit power permission opens an off radio without location permission or an SSID selection")
        check(powerWithoutSetup.powers == 1 && powerWithoutSetup.scanned.isEmpty && powerWithoutSetup.joins == 0,
              "power-only recovery does not scan or associate")
        check(await run(powerWithoutSetup, power: true, networks: []) == .permissionRequired && powerWithoutSetup.scanned.isEmpty,
              "powered-on radio still requires location before any scan")
        let wiredWhileOff = FakeRadio(); wiredWhileOff.connection = .connected
        wiredWhileOff.powered = false; wiredWhileOff.permitted = false
        check(await run(wiredWhileOff, power: true, networks: []) == .alreadyConnected
              && wiredWhileOff.powers == 0 && wiredWhileOff.scanned.isEmpty,
              "an existing wired connection still blocks power-on even without name permission or targets")
        let noTarget = FakeRadio()
        check(await run(noTarget, power: true, networks: []) == .noSelection && noTarget.scanned.isEmpty && noTarget.joins == 0,
              "authorized radio with no selected target never broad-scans or associates")
        let offWithoutPermission = FakeRadio(); offWithoutPermission.permitted = false; offWithoutPermission.powered = false
        check(await run(offWithoutPermission, networks: []) == .wifiOff && offWithoutPermission.powers == 0,
              "missing power permission still preserves an off radio even before setup")
        let powerRevoked = FakeRadio(); powerRevoked.permitted = false; powerRevoked.powered = false
        let powerToken = WiFiRecoveryAuthorization(); powerRevoked.onPower = { powerToken.invalidate() }
        check(await run(powerRevoked, power: true, token: powerToken, networks: []) == .cancelled
              && powerRevoked.powers == 1 && powerRevoked.scanned.isEmpty,
              "revocation during an OS power call discards its result and prevents later scan")
        let off = FakeRadio(); off.powered = false
        check(await run(off) == .wifiOff && off.powers == 0, "power permission defaults off")
        check(await run(off, power: true) == .poweredOn && off.powers == 1 && off.scanned.isEmpty, "explicit power permission permits one power action then waits")
        let cancelled = FakeRadio(); cancelled.results = [open]
        let cancelledToken = WiFiRecoveryAuthorization(); cancelledToken.invalidate()
        check(await run(cancelled, token: cancelledToken) == .cancelled && cancelled.scanned.isEmpty, "invalid generation does no work")
        let late = FakeRadio(); late.results = [open]
        let lateToken = WiFiRecoveryAuthorization(); late.onScan = { lateToken.invalidate() }
        check(await run(late, token: lateToken) == .cancelled && late.joins == 0, "late scan after revocation cannot associate")
        let wiredArrives = FakeRadio(); wiredArrives.results = [open]; wiredArrives.changeAfterScan = .connected
        check(await run(wiredArrives) == .alreadyConnected && wiredArrives.joins == 0, "wired or another Wi-Fi arriving during scan prevents association")
        let unknownArrives = FakeRadio(); unknownArrives.results = [open]; unknownArrives.changeAfterScan = .unknown
        check(await run(unknownArrives) == .connectionUnclear && unknownArrives.joins == 0, "uncertain state after scan prevents association")
        let personal = FakeRadio(); personal.results = [encrypted]
        check(await run(personal) == .systemConnectionRequired && personal.joins == 0, "encrypted Wi-Fi requires system connection without keychain lookup")
        let foreign = FakeRadio(); foreign.results = [.init(id: UUID(), ssid: second.ssid, signal: -10, isOpen: true)]
        check(await run(foreign) == .notFound && foreign.joins == 0, "foreign SSID in directed results is rejected")
        check(foreign.scanned == [first.ssid], "only explicitly selected SSID is scanned")
        let successful = FakeRadio(); successful.results = [open]
        check(await run(successful) == .joined && successful.joins == 1, "selected open campus Wi-Fi can associate")
        check(!successful.scansOnMain, "blocking CoreWLAN operations execute off main thread")
        let secondOpen = WiFiScanCandidate(id: UUID(), ssid: second.ssid, signal: -30, isOpen: true)
        let fallback = FakeRadio(); fallback.results = [open, secondOpen]; fallback.associationFailures = [first.ssid]
        check(await run(fallback, networks: [first, second]) == .joined && fallback.associated == [first.ssid, second.ssid],
              "a failed association falls back to the next allowed remembered Wi-Fi")
        check(fallback.scanned == [first.ssid, second.ssid], "fallback scans only the next exact allowed SSID")
        let scanFallback = FakeRadio(); scanFallback.results = [secondOpen]; scanFallback.scanFailures = [first.ssid]
        check(await run(scanFallback, networks: [first, second]) == .joined && scanFallback.associated == [second.ssid],
              "a directed scan failure still permits the next allowed remembered Wi-Fi")
        let connectedAfterFailure = FakeRadio(); connectedAfterFailure.results = [open, secondOpen]
        connectedAfterFailure.associationFailures = [first.ssid]
        connectedAfterFailure.onAssociate = { connectedAfterFailure.connection = .connected }
        check(await run(connectedAfterFailure, networks: [first, second]) == .alreadyConnected
              && connectedAfterFailure.scanned == [first.ssid] && connectedAfterFailure.associated == [first.ssid],
              "a user connection arriving during failed association stops every later target")
        let unclearAfterFailure = FakeRadio(); unclearAfterFailure.results = [open, secondOpen]
        unclearAfterFailure.associationFailures = [first.ssid]
        unclearAfterFailure.onAssociate = { unclearAfterFailure.connection = .unknown }
        check(await run(unclearAfterFailure, networks: [first, second]) == .connectionUnclear && unclearAfterFailure.joins == 1,
              "uncertain physical state after an association failure stops fallback")
        let revokedAfterFailure = FakeRadio(); revokedAfterFailure.results = [open, secondOpen]
        revokedAfterFailure.associationFailures = [first.ssid]
        let fallbackToken = WiFiRecoveryAuthorization(); revokedAfterFailure.onAssociate = { fallbackToken.invalidate() }
        check(await run(revokedAfterFailure, token: fallbackToken, networks: [first, second]) == .cancelled
              && revokedAfterFailure.scanned == [first.ssid] && revokedAfterFailure.joins == 1,
              "cancellation during failed association prevents scanning or joining another target")
        let failedThenAbsent = FakeRadio(); failedThenAbsent.results = [open]; failedThenAbsent.associationFailures = [first.ssid]
        check(await run(failedThenAbsent, networks: [first, second]) == .failed && failedThenAbsent.scanned == [first.ssid, second.ssid],
              "an earlier OS failure takes precedence over later not-found results")
        let duplicateTargets = FakeRadio(); duplicateTargets.results = [open, secondOpen]
        duplicateTargets.associationFailures = [first.ssid, second.ssid]
        check(await run(duplicateTargets, networks: [first, first, second]) == .failed
              && duplicateTargets.associated == [first.ssid, second.ssid],
              "each allowed SSID receives at most one association attempt in a recovery round")
        let throwing = FakeRadio(); throwing.failScan = true
        check(await run(throwing) == .failed, "OS error is mapped to generic outcome without its contents")
        let noInterface = FakeRadio(); noInterface.powered = nil
        check(await run(noInterface) == .unavailable, "missing interface is handled")
        let invalidSSID = WiFiRecoveryNetwork(id: "record-x", ssid: "", displayName: "")
        let invalidResult = await CoreWLANRecoveryService(radio: FakeRadio()).recover(
            .init(networks: [invalidSSID], allowPowerOn: true), authorization: .init())
        check(invalidResult == .cancelled, "empty SSID never produces a broad scan")

        func link(_ active: Bool?, address: Bool = false, running: Bool? = nil,
                  powered: Bool = true, visible: Bool = false, service: Bool = true) -> WiFiPhysicalState {
            CoreWLANRecoveryRadio.classifyLink(powered: powered, visibleAssociation: visible, linkActive: active,
                hasAddress: address, interfaceRunning: running, serviceActive: service)
        }
        check(link(true) == .connected, "redacted SSID with active link remains connected")
        check(link(nil, address: true) == .connected, "redacted SSID with physical IP remains connected")
        check(link(nil) == .unknown && link(nil, service: false) == .unknown, "service state alone cannot prove disconnection")
        check(link(false) == .disconnected, "explicit inactive link can prove disconnection")
        check(link(nil, powered: false) == .disconnected, "powered-off interface is disconnected")
        check(link(false, address: true) == .disconnected, "unplugged stale address is disconnected")
        check(link(nil, address: true, powered: false) == .disconnected, "powered-off Wi-Fi overrides stale IPv6 address")
        check(link(nil, address: true, running: false) == .disconnected, "down interface overrides stale IP address")
        check(link(true, address: false) == .connected, "active link remains connected when IP or SSID is unavailable")
        for address in ["169.254.1.2", "127.0.0.1", "0.0.0.0", "224.0.0.1", "::", "::1", "fe80::1234", "febf::1", "ff02::1"] {
            check(!CoreWLANRecoveryRadio.usableAddress(address), "local or multicast address cannot prove physical connection")
        }
        for address in ["10.0.0.3", "172.30.1.20", "192.168.1.2", "2001:db8::1234", "fd01::1"] {
            check(CoreWLANRecoveryRadio.usableAddress(address), "ordinary physical interface address remains usable evidence")
        }
        check(link(false, visible: true) == .connected, "visible association overrides inactive link")

        // A live connection and the optional prerequisites for the next disconnection remain distinct.
        let readiness = WiFiRecoveryController(preferencesURL: nil, service: PreviewWiFiRecoveryService(),
            startAutomatically: false, requestsSystemPermission: false)
        readiness.updateContext(allowRecovery: true, hasPhysicalConnection: true, sleeping: false, allowedNetworks: [first])
        readiness.setNetworkSelected(first.id, selected: false)
        readiness.setAutoJoinEnabled(true)
        let permissionMissingStatus = readiness.status
        readiness.requestWiFiNamePermission()
        check(readiness.status == .connected && readiness.setupIssue == .noSelection,
              "an existing connection stays connected while explicit Wi-Fi exclusions remain visible as a setup issue")
        check(permissionMissingStatus == .connected,
              "missing optional Wi-Fi name permission does not mislabel an existing physical connection")

        check(readiness.setupIssue == .noSelection, "explicitly excluding every known Wi-Fi has a distinct setup issue")
        readiness.setNetworkSelected(first.id, selected: true)
        check(readiness.setupIssue == nil && readiness.status == .connected,
              "ready configuration can show the existing physical connection")
        readiness.updateContext(allowRecovery: true, hasPhysicalConnection: false, sleeping: false, allowedNetworks: [])
        check(readiness.setupIssue == .noKnownNetworks && readiness.status == .noKnownNetworks,
              "an empty Wi-Fi candidate list has a distinct status instead of asking the user to select from nothing")
        readiness.setAutoJoinEnabled(false)
        check(readiness.status == .disabled && readiness.setupIssue == .noKnownNetworks,
              "master off retains setup information while displaying disabled")
        readiness.applyPermissionStatus(.denied)
        check(readiness.setupIssue == .permissionRequired, "denied permission remains an actionable setup issue")

        // Displaying an empty optional Wi-Fi list must not change the user's saved choices or touch the radio.
        let wiredOnlyURL = directory.appendingPathComponent("wired-only-presentation.json")
        var wiredOnlyPreferences = WiFiRecoveryPreferences()
        wiredOnlyPreferences.autoJoinEnabled = true
        wiredOnlyPreferences.powerOnWhenDisconnected = true
        try wiredOnlyPreferences.write(wiredOnlyURL)
        let wiredOnlyBytes = try Data(contentsOf: wiredOnlyURL)
        let wiredOnlyService = ControlledService()
        let wiredOnly = WiFiRecoveryController(preferencesURL: wiredOnlyURL, service: wiredOnlyService,
            startAutomatically: false, requestsSystemPermission: false)
        wiredOnly.applyPermissionStatus(.authorized)
        wiredOnly.updateContext(allowRecovery: true, hasPhysicalConnection: true, sleeping: false, allowedNetworks: [])
        check(wiredOnly.hasExistingConnection && wiredOnly.status == .connected && wiredOnly.setupIssue == .noKnownNetworks,
              "a wired-only user sees the existing connection separately from the missing optional Wi-Fi list")
        await wiredOnly.refreshOnce()
        check(await wiredOnlyService.count == 0,
              "an existing wired connection prevents every radio service call even with both saved switches enabled and no Wi-Fi names")
        check(try wiredOnly.autoJoinEnabled && wiredOnly.powerOnWhenDisconnected && Data(contentsOf: wiredOnlyURL) == wiredOnlyBytes,
              "empty-list presentation preserves both enabled switches and their original preference bytes")
        wiredOnly.updateContext(allowRecovery: true, hasPhysicalConnection: false, sleeping: false, allowedNetworks: [])
        check(!wiredOnly.hasExistingConnection && wiredOnly.status == .noKnownNetworks && wiredOnly.setupIssue == .noKnownNetworks,
              "without a connection or Wi-Fi candidates, the status explains that no names are saved instead of asking for a selection")
        check(try Data(contentsOf: wiredOnlyURL) == wiredOnlyBytes,
              "a missing connection changes presentation without rewriting optional Wi-Fi preferences")

        // Remembered Wi-Fi is eligible by default; explicit exclusions persist without plaintext SSIDs.
        let url = directory.appendingPathComponent("wifi-recovery.json")
        check(try WiFiRecoveryPreferences.read(url) == WiFiRecoveryPreferences(), "missing preferences use disabled defaults")
        let controller = WiFiRecoveryController(preferencesURL: url, service: PreviewWiFiRecoveryService(), startAutomatically: false, requestsSystemPermission: false)
        check(!controller.autoJoinEnabled && !controller.powerOnWhenDisconnected, "both controller switches default off")
        controller.updateContext(allowRecovery: true, hasPhysicalConnection: false, sleeping: false, allowedNetworks: [first, second])
        check(controller.selectedNetworkIDs == [first.id, second.id], "all app-remembered Wi-Fi is eligible without duplicate selection")
        controller.setNetworkSelected(second.id, selected: false)
        controller.setAutoJoinEnabled(true)
        controller.setPowerOnWhenDisconnected(true)
        check(controller.selectedNetworkIDs == [first.id], "explicit opt-out removes only the chosen remembered Wi-Fi")
        let saved = try WiFiRecoveryPreferences.read(url)
        check(saved.autoJoinEnabled && saved.powerOnWhenDisconnected && saved.excludedNetworkIDs == [second.id] && saved.selections.isEmpty, "settings round trip atomically")
        let bytes = try Data(contentsOf: url)
        check(!String(decoding: bytes, as: UTF8.self).contains(first.ssid), "preferences do not duplicate plaintext SSID")
        let attributes = try FileManager.default.attributesOfItem(atPath: url.path)
        check((attributes[.posixPermissions] as? NSNumber)?.intValue == 0o600, "preferences mode is 0600")
        let renamed = WiFiRecoveryNetwork(id: first.id, ssid: first.ssid, displayName: "新显示名")
        controller.updateContext(allowRecovery: true, hasPhysicalConnection: false, sleeping: false, allowedNetworks: [renamed, second])
        check(controller.selectedNetworkIDs == [first.id], "display rename preserves explicit selection")
        let changedSSID = WiFiRecoveryNetwork(id: first.id, ssid: "Different SSID", displayName: first.displayName)
        controller.setNetworkSelected(first.id, selected: false)
        controller.updateContext(allowRecovery: true, hasPhysicalConnection: false, sleeping: false, allowedNetworks: [changedSSID, second])
        check(controller.selectedNetworkIDs.isEmpty, "same record ID with changed SSID cannot erase an explicit opt-out")
        check(try WiFiRecoveryPreferences.read(url).excludedNetworkIDs == [first.id, second.id], "SSID changes preserve stored exclusions")
        controller.setNetworkSelected(second.id, selected: true)
        controller.setAutoJoinEnabled(false)
        check(controller.selectedNetworkIDs == [second.id] && controller.powerOnWhenDisconnected, "master off retains choices and independent power preference")
        controller.updateContext(allowRecovery: true, hasPhysicalConnection: false, sleeping: false, allowedNetworks: [])
        check(controller.selectedNetworkIDs.isEmpty, "forgetting memory removes selection")

        controller.updateContext(allowRecovery: true, hasPhysicalConnection: false, sleeping: false, allowedNetworks: [changedSSID, second])
        check(controller.selectedNetworkIDs == [second.id], "temporary missing name metadata cannot erase an opt-out")
        let reloaded = WiFiRecoveryController(preferencesURL: url, service: PreviewWiFiRecoveryService(),
            startAutomatically: false, requestsSystemPermission: false)
        reloaded.updateContext(allowRecovery: true, hasPhysicalConnection: false, sleeping: false, allowedNetworks: [changedSSID, second])
        check(reloaded.selectedNetworkIDs == [second.id], "per-network exclusions survive restart")
        reloaded.setNetworkSelected(first.id, selected: true)
        check(reloaded.selectedNetworkIDs == [first.id, second.id], "a remembered Wi-Fi can be explicitly allowed again")

        let mergeURL = directory.appendingPathComponent("merge-exclusions.json")
        let merge = WiFiRecoveryController(preferencesURL: mergeURL, service: PreviewWiFiRecoveryService(),
            startAutomatically: false, requestsSystemPermission: false)
        merge.updateContext(allowRecovery: true, hasPhysicalConnection: true, sleeping: false, allowedNetworks: [first, second])
        check(merge.preserveExclusionsBeforeMerging([]) && merge.preserveExclusionsBeforeMerging([first.id]),
              "A refresh without consolidation has no preference side effect")
        check(merge.preserveExclusionsBeforeMerging([first.id, second.id]) && merge.selectedNetworkIDs == [first.id, second.id]
              && !FileManager.default.fileExists(atPath: mergeURL.path),
              "Consolidation without opt-outs does not write or change selection")
        merge.setNetworkSelected(second.id, selected: false)
        check(merge.preserveExclusionsBeforeMerging([first.id, second.id]) && merge.selectedNetworkIDs.isEmpty,
              "One excluded duplicate prevents both possible retained identities from automatic finding")
        check(Set(try WiFiRecoveryPreferences.read(mergeURL).excludedNetworkIDs ?? []) == [first.id, second.id],
              "The entire consolidation group's opt-out is durable before memory changes")

        // Decode old schema 1 files without the new optional exclusion field.
        for hadChoice in [false, true] {
            let legacyURL = directory.appendingPathComponent("legacy-\(hadChoice).json")
            let legacyChoices: [[String: String]] = hadChoice ? [["id": first.id, "ssidDigest": first.ssidDigest]] : []
            let oldData = try JSONSerialization.data(withJSONObject: ["schemaVersion": 1, "autoJoinEnabled": true,
                "powerOnWhenDisconnected": false, "selections": legacyChoices])
            try oldData.write(to: legacyURL)
            check(try WiFiRecoveryPreferences.read(legacyURL).excludedNetworkIDs == nil,
                  "old preferences decode without the optional exclusion field \(hadChoice)")
            let legacy = WiFiRecoveryController(preferencesURL: legacyURL, service: PreviewWiFiRecoveryService(),
                startAutomatically: false, requestsSystemPermission: false)
            legacy.updateContext(allowRecovery: true, hasPhysicalConnection: false, sleeping: false, allowedNetworks: [])
            check(try Data(contentsOf: legacyURL) == oldData,
                  "empty metadata does not prematurely consume a legacy allowlist \(hadChoice)")
            legacy.updateContext(allowRecovery: true, hasPhysicalConnection: false, sleeping: false, allowedNetworks: [first, second])
            check(legacy.selectedNetworkIDs == (hadChoice ? [first.id] : [first.id, second.id]),
                  "old explicit range is preserved while an old empty list adopts remembered targets \(hadChoice)")
            let migrated = try WiFiRecoveryPreferences.read(legacyURL)
            check(migrated.excludedNetworkIDs == (hadChoice ? [second.id] : []) && migrated.selections.isEmpty,
                  "legacy migration persists exclusions and retires the old allowlist \(hadChoice)")
        }
        let readOnlyURL = directory.appendingPathComponent("legacy-read-only.json")
        let readOnlyBytes = try JSONSerialization.data(withJSONObject: ["schemaVersion": 1, "autoJoinEnabled": true,
            "powerOnWhenDisconnected": true, "selections": []])
        try readOnlyBytes.write(to: readOnlyURL)
        let readOnlyService = ControlledService()
        let readOnlyModel = WiFiRecoveryController(preferencesURL: readOnlyURL, service: readOnlyService,
            startAutomatically: true, requestsSystemPermission: false, preferencesReadOnly: true)
        readOnlyModel.updateContext(allowRecovery: false, hasPhysicalConnection: false, sleeping: false, allowedNetworks: [first, second])
        check(try Data(contentsOf: readOnlyURL) == readOnlyBytes,
              "read-only legacy setup displays remembered targets without writing migration bytes")
        check(readOnlyModel.autoJoinEnabled && readOnlyModel.powerOnWhenDisconnected
              && readOnlyModel.selectedNetworkIDs == [first.id, second.id],
              "read-only preferences preserve saved switch values and present migrated targets in memory")
        readOnlyModel.setAutoJoinEnabled(false)
        readOnlyModel.setPowerOnWhenDisconnected(false)
        readOnlyModel.setNetworkSelected(first.id, selected: false)
        check(!readOnlyModel.preserveExclusionsBeforeMerging([first.id, second.id]),
              "Read-only controller refuses a memory consolidation preference transaction")
        readOnlyModel.requestWiFiNamePermission()
        check(readOnlyModel.autoJoinEnabled && readOnlyModel.powerOnWhenDisconnected
              && readOnlyModel.selectedNetworkIDs == [first.id, second.id],
              "read-only mutator calls cannot change saved or visible recovery choices")
        check(readOnlyModel.permissionStatus == .notRequested,
              "read-only permission button does not request or simulate authorization")
        // Even a caller supplying an allowed recovery context cannot bypass this storage mode.
        readOnlyModel.updateContext(allowRecovery: true, hasPhysicalConnection: false, sleeping: false, allowedNetworks: [first, second])
        await readOnlyModel.refreshOnce()
        try? await Task.sleep(for: .milliseconds(20))
        check(await readOnlyService.count == 0, "read-only controller cannot enter a radio service despite enabled saved switches")
        check(try Data(contentsOf: readOnlyURL) == readOnlyBytes,
              "read-only update, permission, preference and refresh paths leave the original bytes unchanged")

        let mismatchURL = directory.appendingPathComponent("legacy-mismatched-ssid.json")
        var mismatch = WiFiRecoveryPreferences(); mismatch.excludedNetworkIDs = nil
        mismatch.selections = [.init(id: first.id, ssidDigest: first.ssidDigest)]
        try mismatch.write(mismatchURL)
        let mismatchModel = WiFiRecoveryController(preferencesURL: mismatchURL, service: PreviewWiFiRecoveryService(),
            startAutomatically: false, requestsSystemPermission: false)
        mismatchModel.updateContext(allowRecovery: true, hasPhysicalConnection: false, sleeping: false, allowedNetworks: [changedSSID])
        check(mismatchModel.selectedNetworkIDs.isEmpty, "legacy explicit selection does not migrate across a changed SSID")

        for excluded in [[" "], ["valid", "valid"], ["line\nbreak"], (0..<65).map { "record-\($0)" }] {
            var invalid = WiFiRecoveryPreferences(); invalid.excludedNetworkIDs = excluded
            var rejected = false
            do { try invalid.validate() } catch { rejected = true }
            check(rejected, "exclusion metadata rejects noncanonical, duplicate or unbounded IDs")
        }
        let fullExclusionsURL = directory.appendingPathComponent("full-exclusions.json")
        var fullExclusions = WiFiRecoveryPreferences(); fullExclusions.autoJoinEnabled = true
        fullExclusions.excludedNetworkIDs = (0..<64).map { "excluded-\($0)" }
        try fullExclusions.write(fullExclusionsURL)
        let fullModel = WiFiRecoveryController(preferencesURL: fullExclusionsURL, service: PreviewWiFiRecoveryService(),
            startAutomatically: false, requestsSystemPermission: false)
        fullModel.updateContext(allowRecovery: true, hasPhysicalConnection: false, sleeping: false, allowedNetworks: [first])
        fullModel.setNetworkSelected(first.id, selected: false)
        check(!fullModel.autoJoinEnabled && fullModel.errorMessage != nil,
              "full exclusion storage stops recovery instead of dropping any earlier opt-out")
        check(try WiFiRecoveryPreferences.read(fullExclusionsURL).excludedNetworkIDs?.count == 64,
              "exclusion history stays bounded when its safety limit is reached")
        check(!fullModel.preserveExclusionsBeforeMerging(["excluded-0", first.id]) && !fullModel.autoJoinEnabled,
              "Exclusion capacity exhaustion stops consolidation without dropping older choices")

        for (index, data) in [Data("broken".utf8), Data("{\"schemaVersion\":2,\"autoJoinEnabled\":true,\"powerOnWhenDisconnected\":true,\"selections\":[]}".utf8), Data(repeating: 1, count: 16_385)].enumerated() {
            let badURL = directory.appendingPathComponent("bad-\(index).json")
            try data.write(to: badURL)
            let broken = WiFiRecoveryController(preferencesURL: badURL, service: PreviewWiFiRecoveryService(), startAutomatically: false, requestsSystemPermission: false)
            broken.updateContext(allowRecovery: true, hasPhysicalConnection: false, sleeping: false, allowedNetworks: [first])
            broken.setAutoJoinEnabled(true)
            check(broken.preferencesUnreadable && !broken.autoJoinEnabled && !broken.powerOnWhenDisconnected, "corrupt or unknown preferences disable actions \(index)")
            check(broken.setupIssue == .unreadable, "unreadable preferences remain visible in setup \(index)")
            check(try Data(contentsOf: badURL) == data, "unreadable original remains intact \(index)")
        }

        let externallyChangedURL = directory.appendingPathComponent("externally-changed.json")
        let externallyChanged = WiFiRecoveryController(preferencesURL: externallyChangedURL,
            service: PreviewWiFiRecoveryService(), startAutomatically: false, requestsSystemPermission: false)
        externallyChanged.updateContext(allowRecovery: true, hasPhysicalConnection: false, sleeping: false, allowedNetworks: [first])
        externallyChanged.setNetworkSelected(first.id, selected: true)
        let unexpectedBytes = Data("external invalid schema".utf8)
        try unexpectedBytes.write(to: externallyChangedURL)
        externallyChanged.setAutoJoinEnabled(true)
        check(externallyChanged.preferencesUnreadable && !externallyChanged.autoJoinEnabled,
              "file corrupted after launch disables recovery before a later save")
        check(try Data(contentsOf: externallyChangedURL) == unexpectedBytes, "later save preserves externally damaged file")
        var simulatedCallbacks = 0
        externallyChanged.onWiFiNamePermissionChanged = { simulatedCallbacks += 1 }
        externallyChanged.requestWiFiNamePermission()
        check(externallyChanged.permissionStatus == .authorized && simulatedCallbacks == 1,
              "preview permission button simulates state without creating a location manager")

        // Revocation must still take effect when the local file becomes unwritable.
        let blockedFolder = directory.appendingPathComponent("blocked")
        let blockedURL = blockedFolder.appendingPathComponent("wifi-recovery.json")
        let blocked = WiFiRecoveryController(preferencesURL: blockedURL, service: PreviewWiFiRecoveryService(), startAutomatically: false, requestsSystemPermission: false)
        blocked.updateContext(allowRecovery: true, hasPhysicalConnection: false, sleeping: false, allowedNetworks: [first])
        blocked.setNetworkSelected(first.id, selected: true); blocked.setAutoJoinEnabled(true)
        let backupFolder = directory.appendingPathComponent("blocked-preserved")
        try FileManager.default.moveItem(at: blockedFolder, to: backupFolder)
        try Data("not a directory".utf8).write(to: blockedFolder)
        blocked.setAutoJoinEnabled(false)
        check(!blocked.autoJoinEnabled && blocked.errorMessage != nil, "disable remains effective after disk write failure")
        blocked.setNetworkSelected(first.id, selected: false)
        check(blocked.selectedNetworkIDs.isEmpty, "network revocation remains effective after disk write failure")
        blocked.setAutoJoinEnabled(true)
        check(!blocked.autoJoinEnabled, "failed persistence cannot enable recovery")
        check(try WiFiRecoveryPreferences.read(backupFolder.appendingPathComponent("wifi-recovery.json")).autoJoinEnabled,
              "write failure does not corrupt previously saved data")
        blocked.updateContext(allowRecovery: true, hasPhysicalConnection: false, sleeping: false, allowedNetworks: [first, second])
        check(!blocked.preserveExclusionsBeforeMerging([first.id, second.id]) && blocked.selectedNetworkIDs.isEmpty,
              "Failed consolidation preference save still excludes every group member in memory")

        // Controller cancellation reaches the exact token the real serial service checks after its scan.
        let scanGate = BlockingScan()
        let slowRadio = FakeRadio(); slowRadio.results = [open]
        slowRadio.onScan = { scanGate.block() }
        let integrated = WiFiRecoveryController(preferencesURL: nil,
            service: CoreWLANRecoveryService(radio: slowRadio), startAutomatically: false, requestsSystemPermission: false)
        integrated.updateContext(allowRecovery: true, hasPhysicalConnection: false, sleeping: false, allowedNetworks: [first])
        integrated.requestWiFiNamePermission()
        integrated.setNetworkSelected(first.id, selected: true); integrated.setAutoJoinEnabled(true)
        let integratedTask = Task { await integrated.refreshOnce() }
        var sawScan = false
        for _ in 0..<1000 {
            if scanGate.started { sawScan = true; break }
            try? await Task.sleep(for: .milliseconds(1))
        }
        check(sawScan, "real service begins its blocking scan on the serial queue")
        integrated.setAutoJoinEnabled(false)
        scanGate.resume(); await integratedTask.value
        check(slowRadio.joins == 0 && integrated.status == .disabled,
              "turning off during real service scan prevents later association and success publication")

        // Real controller operations with a deferred in-memory service.
        func configured(_ service: ControlledService, automatic: Bool = false,
                        delays: [TimeInterval] = [15], clock: ManualClock? = nil,
                        sleeper: ManualRetrySleeper? = nil) -> WiFiRecoveryController {
            let model = WiFiRecoveryController(preferencesURL: nil, service: service, startAutomatically: automatic,
                requestsSystemPermission: false, retryDelays: delays, protectedRetryDelays: delays, now: { clock?.now() ?? Date() },
                retrySleep: { seconds in
                    if let sleeper { try await sleeper.sleep(seconds) }
                    else { try await Task.sleep(for: .seconds(seconds)) }
                })
            model.updateContext(allowRecovery: true, hasPhysicalConnection: false, sleeping: false, allowedNetworks: [first])
            model.requestWiFiNamePermission()
            model.setNetworkSelected(first.id, selected: true)
            model.setAutoJoinEnabled(true)
            return model
        }
        func waitForCall(_ service: ControlledService, count: Int = 1) async {
            for _ in 0..<1000 {
                if await service.count >= count { return }
                try? await Task.sleep(for: .milliseconds(1))
            }
            fatalError("service did not start")
        }
        for reason in 0..<6 {
            let service = ControlledService(); let model = configured(service)
            let task = Task { await model.refreshOnce() }
            await waitForCall(service)
            check(await service.latestValid, "operation starts with valid generation \(reason)")
            switch reason {
            case 0: model.setAutoJoinEnabled(false)
            case 1: model.setNetworkSelected(first.id, selected: false)
            case 2: model.updateContext(allowRecovery: false, hasPhysicalConnection: false, sleeping: false, allowedNetworks: [first])
            case 3: model.updateContext(allowRecovery: true, hasPhysicalConnection: true, sleeping: false, allowedNetworks: [first])
            case 4: model.updateContext(allowRecovery: true, hasPhysicalConnection: false, sleeping: true, allowedNetworks: [first])
            default: model.updateContext(allowRecovery: true, hasPhysicalConnection: false, sleeping: false, allowedNetworks: [changedSSID])
            }
            check(!(await service.latestValid), "off, revoke, hold, connection, sleep or SSID change invalidates in-flight operation \(reason)")
            await model.refreshOnce()
            check(await service.count == 1, "pending operation cannot overlap with another refresh \(reason)")
            await service.release(); await task.value
            check(model.status != .joined, "late result cannot publish success after invalidation \(reason)")
        }
        let powerService = ControlledService(); let powerModel = configured(powerService)
        powerModel.setPowerOnWhenDisconnected(true)
        let powerTask = Task { await powerModel.refreshOnce() }; await waitForCall(powerService)
        check(await powerService.latestRequest?.allowPowerOn == true, "independent power choice reaches service")
        powerModel.setPowerOnWhenDisconnected(false)
        check(!(await powerService.latestValid), "power toggle off invalidates pending power or join")
        await powerService.release(.poweredOn); await powerTask.value

        let blockingService = ControlledService(); let blockingModel = configured(blockingService)
        for flags in [(false, false, false), (true, true, false), (true, false, true)] {
            blockingModel.updateContext(allowRecovery: flags.0, hasPhysicalConnection: flags.1, sleeping: flags.2, allowedNetworks: [first])
            await blockingModel.refreshOnce()
        }
        check(await blockingService.count == 0, "hold, existing connection and sleep all prevent service entry")
        let emptyService = ControlledService()
        let emptyModel = WiFiRecoveryController(preferencesURL: nil, service: emptyService, startAutomatically: false, requestsSystemPermission: false)
        emptyModel.updateContext(allowRecovery: true, hasPhysicalConnection: false, sleeping: false, allowedNetworks: [])
        emptyModel.setPowerOnWhenDisconnected(true); await emptyModel.refreshOnce()
        check(await emptyService.count == 0, "power permission alone never runs recovery")
        emptyModel.setAutoJoinEnabled(true)
        let emptyTask = Task { await emptyModel.refreshOnce() }
        await waitForCall(emptyService)
        let emptyRequest = await emptyService.latestRequest
        check(emptyRequest?.networks.isEmpty == true && emptyModel.selectedNetworkIDs.isEmpty,
              "power-only recovery never invents an unremembered target")
        check(emptyRequest?.allowPowerOn == true, "independent power permission reaches a service without selected targets")
        await emptyService.release(.permissionRequired); await emptyTask.value

        // The same live controller becomes ready without restarting or a manual refresh.
        let setupService = ControlledService()
        let setupModel = WiFiRecoveryController(preferencesURL: nil, service: setupService,
            startAutomatically: true, requestsSystemPermission: false, retryDelays: [2])
        setupModel.updateContext(allowRecovery: true, hasPhysicalConnection: true, sleeping: false, allowedNetworks: [first])
        setupModel.setAutoJoinEnabled(true); setupModel.setPowerOnWhenDisconnected(true)
        try? await Task.sleep(for: .milliseconds(20))
        check(await setupService.count == 0 && setupModel.status == .connected && setupModel.setupIssue == .permissionRequired,
              "an existing connection is reported accurately while optional permission remains incomplete and no service starts")
        setupModel.requestWiFiNamePermission()
        check(await setupService.count == 0 && setupModel.status == .connected && setupModel.selectedNetworkIDs == [first.id],
              "permission completes remembered-network setup without a duplicate target-selection step")
        check(setupModel.setupIssue == nil && setupModel.status == .connected,
              "default remembered targets are ready while an existing connection is preserved")
        setupModel.updateContext(allowRecovery: true, hasPhysicalConnection: false, sleeping: false, allowedNetworks: [first])
        await waitForCall(setupService)
        let scheduledRequest = await setupService.latestRequest
        check(scheduledRequest?.networks == [first] && scheduledRequest?.allowPowerOn == true,
              "disconnect after live setup automatically starts the selected target and power policy")
        setupModel.setAutoJoinEnabled(false); await setupService.release()

        let permissionService = ControlledService()
        let permissionModel = WiFiRecoveryController(preferencesURL: nil, service: permissionService,
            startAutomatically: true, requestsSystemPermission: false, retryDelays: [2])
        permissionModel.updateContext(allowRecovery: true, hasPhysicalConnection: false, sleeping: false, allowedNetworks: [first])
        permissionModel.setNetworkSelected(first.id, selected: true); permissionModel.setAutoJoinEnabled(true)
        try? await Task.sleep(for: .milliseconds(20))
        check(await permissionService.count == 0 && permissionModel.status == .permissionRequired,
              "persisted selected targets still require permission before service entry")
        var permissionChanges = 0
        permissionModel.onWiFiNamePermissionChanged = { permissionChanges += 1 }
        permissionModel.requestWiFiNamePermission()
        await waitForCall(permissionService)
        check(permissionChanges == 1, "a real permission transition notifies the name refresh observer")
        permissionModel.applyPermissionStatus(.authorized)
        permissionModel.updateContext(allowRecovery: true, hasPhysicalConnection: false, sleeping: false, allowedNetworks: [renamed])
        permissionModel.setNetworkSelected(first.id, selected: true)
        check(await permissionService.latestValid && permissionModel.status == .searching && permissionChanges == 1,
              "unchanged permission callbacks, display rename and repeated selection do not cancel an active search")
        permissionModel.applyPermissionStatus(.denied)
        check(!(await permissionService.latestValid) && permissionModel.status == .permissionRequired,
              "revoked permission immediately invalidates a pending search")
        await permissionService.release(.joined)
        try? await Task.sleep(for: .milliseconds(20))
        check(await permissionService.count == 1 && permissionModel.status == .permissionRequired,
              "late success after permission revocation neither publishes nor schedules")
        permissionModel.requestWiFiNamePermission()
        await waitForCall(permissionService, count: 2)
        check(await permissionService.latestValid, "re-granting permission starts promptly without a restart")
        permissionModel.setAutoJoinEnabled(false); await permissionService.release()

        // Master enable uses the app-remembered list directly, with no second selection ceremony.
        for excludeFirst in [false, true] {
            let radio = FakeRadio()
            radio.results = [open, .init(id: UUID(), ssid: second.ssid, signal: -30, isOpen: true)]
            let service = ObservedRadioService(radio)
            let model = WiFiRecoveryController(preferencesURL: nil, service: service,
                startAutomatically: true, requestsSystemPermission: false, retryDelays: [2])
            model.updateContext(allowRecovery: true, hasPhysicalConnection: false, sleeping: false, allowedNetworks: [first, second])
            if excludeFirst { model.setNetworkSelected(first.id, selected: false) }
            model.requestWiFiNamePermission()
            model.setAutoJoinEnabled(true)
            for _ in 0..<1000 {
                if await service.results.count >= 1 { break }
                try? await Task.sleep(for: .milliseconds(1))
            }
            model.setAutoJoinEnabled(false)
            check(await service.results.first == .joined,
                  "master enable automatically associates an eligible remembered network \(excludeFirst)")
            check(radio.scanned == [excludeFirst ? second.ssid : first.ssid] && radio.joins == 1,
                  "automatic association honors optional per-network exclusions \(excludeFirst)")
        }

        // Automatic power recovery reaches the real serialized service with no permission or target.
        let bareRadio = FakeRadio(); bareRadio.permitted = false; bareRadio.powered = false
        let bareService = ObservedRadioService(bareRadio)
        let bareModel = WiFiRecoveryController(preferencesURL: nil, service: bareService,
            startAutomatically: true, requestsSystemPermission: false, retryDelays: [0.02, 2])
        bareModel.updateContext(allowRecovery: true, hasPhysicalConnection: false, sleeping: false, allowedNetworks: [])
        bareModel.setPowerOnWhenDisconnected(true); bareModel.setAutoJoinEnabled(true)
        for _ in 0..<1000 {
            if await bareService.results.count >= 3 { break }
            try? await Task.sleep(for: .milliseconds(1))
        }
        bareModel.setAutoJoinEnabled(false)
        let bareResults = await bareService.results
        check(bareResults.prefix(3) == [.poweredOn, .permissionRequired, .permissionRequired],
              "automatic controller powers on then promptly checks incomplete setup without escalating scan backoff")
        check(bareRadio.powers == 1 && bareRadio.scanned.isEmpty && bareRadio.joins == 0,
              "automatic bare setup performs one permitted power action and zero scans or associations")
        check(bareModel.permissionStatus == .notRequested && bareModel.setupIssue == .permissionRequired,
              "power recovery never grants or requests name permission")

        // Revoke while the serial service is inspecting power, before it may enter setPowerOn.
        for reason in 0..<5 {
            let powerGate = BlockingScan()
            let radio = FakeRadio(); radio.powered = false; radio.permitted = false
            radio.onPowerState = { powerGate.block() }
            let model = WiFiRecoveryController(preferencesURL: nil, service: CoreWLANRecoveryService(radio: radio),
                startAutomatically: false, requestsSystemPermission: false)
            model.updateContext(allowRecovery: true, hasPhysicalConnection: false, sleeping: false, allowedNetworks: [])
            model.setPowerOnWhenDisconnected(true); model.setAutoJoinEnabled(true)
            let task = Task { await model.refreshOnce() }
            var reachedPower = false
            for _ in 0..<1000 {
                if powerGate.started { reachedPower = true; break }
                try? await Task.sleep(for: .milliseconds(1))
            }
            check(reachedPower, "power-only recovery reaches its pre-action check \(reason)")
            switch reason {
            case 0: model.setAutoJoinEnabled(false)
            case 1: model.setPowerOnWhenDisconnected(false)
            case 2: model.updateContext(allowRecovery: true, hasPhysicalConnection: false, sleeping: true, allowedNetworks: [])
            case 3: model.updateContext(allowRecovery: true, hasPhysicalConnection: true, sleeping: false, allowedNetworks: [])
            default: model.updateContext(allowRecovery: false, hasPhysicalConnection: false, sleeping: false, allowedNetworks: [])
            }
            powerGate.resume(); await task.value
            check(radio.powers == 0 && radio.scanned.isEmpty && radio.joins == 0,
                  "master, power permission, sleep, physical connection or pause revokes pending power-on \(reason)")
        }

        // Barriers wait for work to enter a known state; retry time is entirely virtual.
        func waitUntil(_ title: String, _ condition: @MainActor () async -> Bool) async {
            for _ in 0..<1000 {
                if await condition() { return }
                try? await Task.sleep(for: .milliseconds(1))
            }
            fatalError("barrier did not complete: \(title)")
        }
        func nextTicket(_ sleeper: ManualRetrySleeper, after previous: UUID? = nil) async -> ManualRetrySleeper.Ticket {
            await waitUntil("retry registration") {
                await sleeper.tickets().contains { $0.id != previous }
            }
            return await sleeper.tickets().first { $0.id != previous }!
        }

        // A pre-cancelled wait cannot leak an unresumable continuation.
        let cancelledClock = ManualClock(); let cancelledSleeper = ManualRetrySleeper(cancelledClock)
        let cancelledWait = Task { try await cancelledSleeper.sleep(120) }
        cancelledWait.cancel()
        do { try await cancelledWait.value; check(false, "pre-cancelled retry throws") }
        catch { check(error is CancellationError, "pre-cancelled retry throws") }
        check(await cancelledSleeper.tickets().isEmpty, "pre-registration cancellation leaves no pending retry")
        await cancelledSleeper.cancelAll()

        // A real context transition starts promptly without inheriting an old deadline.
        for reason in 0..<5 {
            let clock = ManualClock(); let sleeper = ManualRetrySleeper(clock)
            let service = ControlledService()
            let model = configured(service, automatic: true, delays: [120, 240], clock: clock, sleeper: sleeper)
            await waitForCall(service)
            await service.release(.notFound)
            let ticket = await nextTicket(sleeper)
            let date = clock.now()
            check(ticket.deadline == date.addingTimeInterval(120) && model.status == .waiting,
                  "failed attempt registers its full virtual backoff \(reason)")
            switch reason {
            case 0:
                model.updateContext(allowRecovery: true, hasPhysicalConnection: true, sleeping: false, allowedNetworks: [first])
                model.updateContext(allowRecovery: true, hasPhysicalConnection: false, sleeping: false, allowedNetworks: [first])
            case 1:
                model.updateContext(allowRecovery: false, hasPhysicalConnection: false, sleeping: false, allowedNetworks: [first])
                model.updateContext(allowRecovery: true, hasPhysicalConnection: false, sleeping: false, allowedNetworks: [first])
            case 2:
                model.updateContext(allowRecovery: true, hasPhysicalConnection: false, sleeping: true, allowedNetworks: [first])
                model.updateContext(allowRecovery: true, hasPhysicalConnection: false, sleeping: false, allowedNetworks: [first])
            case 3:
                model.setAutoJoinEnabled(false); model.setAutoJoinEnabled(true)
            default:
                model.updateContext(allowRecovery: true, hasPhysicalConnection: false, sleeping: false, allowedNetworks: [first, second])
                model.setNetworkSelected(second.id, selected: true)
            }
            await waitForCall(service, count: 2)
            let isValid = await service.latestValid
            check(clock.now() == date && isValid,
                  "new disconnection, resume, wake, enable or target immediately starts a valid attempt \(reason)")
            check(await sleeper.cancelledAndEmpty(ticket.id),
                  "actual context change cancels the previous retry ticket \(reason)")
            check(await service.cancelledOnEntry == [false, false], "replacement attempt never runs in the cancelled task \(reason)")
            model.setAutoJoinEnabled(false); await service.release(); await sleeper.cancelAll()
        }

        let unchangedClock = ManualClock(); let unchangedSleeper = ManualRetrySleeper(unchangedClock)
        let unchangedService = ControlledService()
        let unchangedModel = configured(unchangedService, automatic: true, delays: [120, 240],
                                        clock: unchangedClock, sleeper: unchangedSleeper)
        await waitForCall(unchangedService)
        let started = unchangedClock.now()
        unchangedClock.advance(300) // The fake service is still pending.
        await unchangedService.release(.notFound)
        let ticket = await nextTicket(unchangedSleeper)
        check(ticket.deadline == unchangedClock.now().addingTimeInterval(120)
              && ticket.deadline != started.addingTimeInterval(120),
              "retry backoff begins when the service completes, not when it starts")
        for reason in 0..<4 {
            switch reason {
            case 0:
                unchangedModel.updateContext(allowRecovery: true, hasPhysicalConnection: false, sleeping: false, allowedNetworks: [first])
            case 1:
                unchangedModel.updateContext(allowRecovery: true, hasPhysicalConnection: false, sleeping: false, allowedNetworks: [renamed])
            case 2: unchangedModel.applyPermissionStatus(.authorized)
            default: unchangedModel.setNetworkSelected(first.id, selected: true)
            }
            check(await unchangedSleeper.tickets() == [ticket],
                  "same context, display rename, unchanged permission or selection preserves ticket ID and absolute deadline \(reason)")
            let historyCount = await unchangedSleeper.history().count
            let callCount = await unchangedService.count
            check(historyCount == 1 && callCount == 1,
                  "no-op neither replaces its retry nor calls the service \(reason)")
        }
        unchangedClock.set(ticket.deadline.addingTimeInterval(-1))
        check(await unchangedSleeper.releaseDue() == 0, "retry is not released before its exact deadline")
        await unchangedModel.refreshOnce()
        let beforeDeadlineCount = await unchangedService.count
        check(await unchangedSleeper.tickets() == [ticket] && beforeDeadlineCount == 1,
              "manual refresh cannot bypass an outstanding retry before its deadline")
        unchangedClock.set(ticket.deadline)
        check(await unchangedSleeper.releaseDue() == 1, "the original retry is released exactly at its deadline")
        await waitForCall(unchangedService, count: 2)
        check(await unchangedService.validCalls(2),
              "the preserved deadline starts exactly one valid retry")
        let originalTaskStates = await unchangedService.cancelledOnEntry
        check(await unchangedSleeper.history() == [ticket] && originalTaskStates == [false, false],
              "harmless updates never cancelled or replaced the original scheduled operation")
        unchangedClock.advance(37)
        let completed = unchangedClock.now()
        await unchangedService.release(.notFound)
        let secondTicket = await nextTicket(unchangedSleeper, after: ticket.id)
        check(secondTicket.deadline == completed.addingTimeInterval(240),
              "the next backoff advances only after the retry service completes")
        check(await unchangedService.count == 2, "a completed retry never creates a tight scan loop")
        unchangedModel.setAutoJoinEnabled(false)
        await waitUntil("disabled retry cancellation") {
            await unchangedSleeper.cancelledAndEmpty(secondTicket.id)
        }
        unchangedClock.set(secondTicket.deadline.addingTimeInterval(100))
        check(await unchangedSleeper.releaseDue() == 0, "master off removes its future retry even after its former deadline")
        await unchangedModel.refreshOnce()
        check(await unchangedService.count == 2, "master off prevents future scheduled scans")
        await unchangedSleeper.cancelAll()

        // Each blocking condition cancels a registered wait without a scan after its old deadline.
        for reason in 0..<5 {
            let clock = ManualClock(); let sleeper = ManualRetrySleeper(clock)
            let service = ControlledService()
            let model = configured(service, automatic: true, delays: [120], clock: clock, sleeper: sleeper)
            await waitForCall(service); await service.release(.notFound)
            let pending = await nextTicket(sleeper)
            switch reason {
            case 0: model.setAutoJoinEnabled(false)
            case 1: model.updateContext(allowRecovery: false, hasPhysicalConnection: false, sleeping: false, allowedNetworks: [first])
            case 2: model.updateContext(allowRecovery: true, hasPhysicalConnection: false, sleeping: true, allowedNetworks: [first])
            case 3: model.updateContext(allowRecovery: true, hasPhysicalConnection: true, sleeping: false, allowedNetworks: [first])
            default: model.applyPermissionStatus(.denied)
            }
            await waitUntil("blocked retry cancellation \(reason)") {
                await sleeper.cancelledAndEmpty(pending.id)
            }
            check(await sleeper.tickets().isEmpty, "off, pause, sleep, connection or revoked permission cancels its wait \(reason)")
            clock.set(pending.deadline.addingTimeInterval(100))
            check(await sleeper.releaseDue() == 0, "cancelled wait has no due continuation \(reason)")
            await model.refreshOnce()
            check(await service.count == 1, "blocked context cannot retry after the former deadline \(reason)")
            model.setAutoJoinEnabled(false); await sleeper.cancelAll()
        }

        // An intentionally uncooperative sleeper must not let the cancelled task enter the service.
        let lateClock = ManualClock(); let lateSleeper = ManualRetrySleeper(lateClock, ignoresCancellation: true)
        let lateService = ControlledService()
        let lateModel = configured(lateService, automatic: true, delays: [120], clock: lateClock, sleeper: lateSleeper)
        await waitForCall(lateService); await lateService.release(.notFound)
        let lateTicket = await nextTicket(lateSleeper)
        lateModel.setAutoJoinEnabled(false); lateModel.setAutoJoinEnabled(true)
        await waitUntil("late cancellation observed") { await lateSleeper.cancellationRequested(lateTicket.id) }
        let beforeLateWakeCount = await lateService.count
        check(await lateSleeper.tickets() == [lateTicket] && beforeLateWakeCount == 1,
              "a sleeper that ignores cancellation holds the old task without starting overlapping recovery")
        lateClock.set(lateTicket.deadline)
        check(await lateSleeper.releaseDue() == 1, "the cancelled sleeper can deliberately return late")
        await waitForCall(lateService, count: 2)
        check(await lateService.validCalls(2),
              "late wake skips the cancelled task and starts only a fresh authorized operation")
        let afterLateWakeCount = await lateService.count
        check(await lateSleeper.history() == [lateTicket] && afterLateWakeCount == 2,
              "late completion produces one replacement operation without duplicate service entry")
        lateModel.setAutoJoinEnabled(false); await lateService.release(); await lateSleeper.cancelAll()
        do {
            let clock = ManualClock(); let sleeper = ManualRetrySleeper(clock)
            let service = ControlledService()
            let model = WiFiRecoveryController(preferencesURL: nil, service: service,
                startAutomatically: true, requestsSystemPermission: false,
                now: { clock.now() }, retrySleep: { try await sleeper.sleep($0) })
            model.applyPermissionStatus(.authorized)
            model.updateContext(allowRecovery: true, hasPhysicalConnection: false, sleeping: false, allowedNetworks: [first])
            model.setAutoJoinEnabled(true)
            await waitForCall(service)
            await service.release(.notFound)
            let retry = await nextTicket(sleeper)
            check(retry.deadline == clock.now().addingTimeInterval(2),
                  "A temporarily missing remembered Wi-Fi retries after two seconds with production defaults")
            model.setAutoJoinEnabled(false); await sleeper.cancelAll()
        }
        func productionModel(_ service: ControlledService, _ clock: ManualClock,
                             _ sleeper: ManualRetrySleeper) -> WiFiRecoveryController {
            let model = WiFiRecoveryController(preferencesURL: nil, service: service,
                startAutomatically: true, requestsSystemPermission: false,
                now: { clock.now() }, retrySleep: { try await sleeper.sleep($0) })
            model.applyPermissionStatus(.authorized)
            model.updateContext(allowRecovery: true, hasPhysicalConnection: false, sleeping: false, allowedNetworks: [first])
            model.setAutoJoinEnabled(true)
            return model
        }
        do {
            let clock = ManualClock(); let sleeper = ManualRetrySleeper(clock); let service = ControlledService()
            let model = productionModel(service, clock, sleeper)
            await waitForCall(service)
            for (index, delay) in [2.0, 5, 10, 20, 30, 60, 120, 300, 300].enumerated() {
                await service.release(index.isMultiple(of: 2) ? .notFound : .failed)
                let retry = await nextTicket(sleeper)
                check(retry.deadline == clock.now().addingTimeInterval(delay),
                      "Transient target failures use the bounded production retry step \(index)")
                clock.set(retry.deadline.addingTimeInterval(-0.001))
                check(await sleeper.releaseDue() == 0, "Transient retry never runs before its deadline \(index)")
                clock.set(retry.deadline); _ = await sleeper.releaseDue()
                await waitForCall(service, count: index + 2)
            }
            model.setAutoJoinEnabled(false); await service.release(); await sleeper.cancelAll()
        }
        for protected in [WiFiRecoveryResult.permissionRequired, .unavailable, .systemConnectionRequired, .connectionUnclear, .wifiOff] {
            let clock = ManualClock(); let sleeper = ManualRetrySleeper(clock); let service = ControlledService()
            let model = productionModel(service, clock, sleeper)
            await waitForCall(service); await service.release(protected)
            let retry = await nextTicket(sleeper)
            check(retry.deadline == clock.now().addingTimeInterval(15),
                  "Permission, platform and unclear-link failures retain conservative retry: \(protected)")
            model.setAutoJoinEnabled(false); await sleeper.cancelAll()
        }
        for progress in [WiFiRecoveryResult.poweredOn, .joined, .alreadyConnected] {
            let clock = ManualClock(); let sleeper = ManualRetrySleeper(clock); let service = ControlledService()
            let model = productionModel(service, clock, sleeper)
            await waitForCall(service); await service.release(.notFound)
            let firstRetry = await nextTicket(sleeper)
            clock.set(firstRetry.deadline); _ = await sleeper.releaseDue(); await waitForCall(service, count: 2)
            await service.release(progress)
            let confirmation = await nextTicket(sleeper)
            check(confirmation.deadline == clock.now().addingTimeInterval(2),
                  "Radio progress permits prompt link observation: \(progress)")
            clock.set(confirmation.deadline); _ = await sleeper.releaseDue(); await waitForCall(service, count: 3)
            await service.release(.notFound)
            let freshRetry = await nextTicket(sleeper)
            check(freshRetry.deadline == clock.now().addingTimeInterval(2),
                  "Radio progress clears an older failure escalation: \(progress)")
            model.setAutoJoinEnabled(false); await sleeper.cancelAll()
        }
        do {
            let clock = ManualClock(); let sleeper = ManualRetrySleeper(clock); let service = ControlledService()
            let model = configured(service, automatic: true, delays: [120], clock: clock, sleeper: sleeper)
            await waitForCall(service)
            model.observeRadioPower(false)
            await service.release(.notFound)
            let oldRetry = await nextTicket(sleeper)
            model.observeRadioPower(true)
            await waitForCall(service, count: 2)
            check(await sleeper.cancelledAndEmpty(oldRetry.id),
                  "Opening Wi-Fi revokes a stale long wait and starts a fresh attempt")
            let activeToken = await service.latestValid
            model.observeRadioPower(true); model.observeRadioPower(nil)
            let remainsValid = await service.latestValid
            let unchangedCount = await service.count
            check(activeToken && remainsValid && unchangedCount == 2,
                  "Repeated or unknown power observations preserve the running scan and never overlap it")
            await service.release(.notFound)
            let retry = await nextTicket(sleeper)
            model.observeRadioPower(true)
            check(await sleeper.tickets() == [retry], "Identical powered-on polls preserve the exact retry ticket")
            check(!model.diagnosticSummary.contains(first.ssid) && !model.diagnosticSummary.contains(first.id)
                  && model.diagnosticSummary.contains("attempts=2"),
                  "Find-network diagnostics expose bounded state and counters without network names or identifiers")
            model.setAutoJoinEnabled(false); await sleeper.cancelAll()
        }
        do {
            let radio = FakeRadio(); radio.results = [open]
            let gate = BlockingScan(); radio.onScan = { gate.block() }
            let model = WiFiRecoveryController(preferencesURL: nil, service: CoreWLANRecoveryService(radio: radio),
                startAutomatically: false, requestsSystemPermission: false)
            model.applyPermissionStatus(.authorized)
            model.updateContext(allowRecovery: true, hasPhysicalConnection: false, sleeping: false, allowedNetworks: [first])
            model.setAutoJoinEnabled(true); model.observeRadioPower(true)
            let pending = Task { await model.refreshOnce() }
            await waitUntil("power-revocation scan") { gate.started }
            radio.powered = false; model.observeRadioPower(false)
            gate.resume(); await pending.value
            check(radio.joins == 0 && radio.powers == 0,
                  "Powering off during a native-service scan revokes association without overriding power consent")
        }
        for blocked in 0..<6 {
            let clock = ManualClock(); let sleeper = ManualRetrySleeper(clock); let service = ControlledService()
            let model = configured(service, automatic: true, delays: [120], clock: clock, sleeper: sleeper)
            await waitForCall(service); await service.release(.notFound)
            let retry = await nextTicket(sleeper)
            switch blocked {
            case 0: model.setAutoJoinEnabled(false)
            case 1: model.updateContext(allowRecovery: false, hasPhysicalConnection: false, sleeping: false, allowedNetworks: [first])
            case 2: model.updateContext(allowRecovery: true, hasPhysicalConnection: true, sleeping: false, allowedNetworks: [first])
            case 3: model.updateContext(allowRecovery: true, hasPhysicalConnection: false, sleeping: true, allowedNetworks: [first])
            case 4: model.setNetworkSelected(first.id, selected: false)
            default: model.applyPermissionStatus(.denied)
            }
            model.observeRadioPower(false); model.observeRadioPower(true)
            await waitUntil("blocked power retry cancellation") { await sleeper.cancelledAndEmpty(retry.id) }
            clock.advance(1000)
            _ = await sleeper.releaseDue()
            check(await service.count == 1, "Radio signals cannot bypass an existing stop condition \(blocked)")
            model.setAutoJoinEnabled(false); await sleeper.cancelAll()
        }
        do {
            let service = ControlledService()
            let model = WiFiRecoveryController(preferencesURL: nil, service: service,
                startAutomatically: false, requestsSystemPermission: false, preferencesReadOnly: true)
            model.applyPermissionStatus(.authorized)
            model.updateContext(allowRecovery: true, hasPhysicalConnection: false, sleeping: false, allowedNetworks: [first])
            model.observeRadioPower(false); model.observeRadioPower(true)
            await model.refreshOnce()
            check(await service.count == 0, "Read-only radio observations never enter recovery or mutate preferences")
        }
        check(await PreviewWiFiRecoveryService().recover(request, authorization: .init()) == .notFound, "preview implementation completes without system services")
        print("Wi-Fi recovery: \(passed) assertions passed; fake radio only, no real network or credentials.")
    }
}
#endif
