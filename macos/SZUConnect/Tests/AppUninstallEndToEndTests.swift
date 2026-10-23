import AppKit
import SwiftUI
import Darwin

/// Runs from its own disposable bundle. Only injected fake credentials, login
/// items and settings are used; the real Trash operation moves this test bundle.
@MainActor private final class EndToEndFixture: ObservableObject, AppUninstallModel {
    let root: URL
    let receiptURL: URL
    let scenario: String
    let removeData: Bool
    let applicationURL: URL
    let applications: URL
    let supportParent: URL
    let support: URL
    private let originalResourceURL: URL
    var canUninstallApplication = true
    @Published var showingAbout = false
    @Published var showingConfirmation = false
    @Published private(set) var maintaining = false
    private(set) var mutations: [String] = []
    private(set) var inspections = 0
    private(set) var maintenanceCalls = 0
    private(set) var maintenanceTransitions = 0
    private(set) var acknowledged: (Bool, Bool)?
    private(set) var nestedPresentationCount = 0
    private(set) var childBusyStateRendered = false
    private(set) var finished = false
    private(set) var busyAtFinish = true
    private(set) var completedAtFinish = false
    private(set) var cancellationReleasedUI = false
    private(set) var cancelledPreflightMadeNoChanges = false
    private(set) var latePreflightMadeNoChanges = false
    private(set) var failureReleasedUI = false
    private(set) var failureKeptOwnApplication = false
    private(set) var closedBothSheetsAfterFailure = false
    private(set) var closedBothSheetsAfterCancellation = false
    private(set) var systemTrashCalls = 0
    private(set) var systemTrashSuccesses = 0
    private(set) var returnedTrashURL: URL?
    private(set) var postMoveResourceLookupAttempted = false
    private(set) var postMoveResourceMissing = false
    private var pendingInspection: CheckedContinuation<Void, Never>?
    private var suspendNextInspection = false
    private var failNextTrash = false
    private var credentials = ["own": "offline-fixture-credential", "other": "keep-unrelated-fixture"]
    private var preferences = ["alwaysShowDockIcon": true]
    private let lifecycle = AppUninstallLifecycle()
    lazy var controller: AppUninstallController = AppUninstallController(service: makeService(), prepareUIForMaintenance: { [weak self] in
        precondition(self?.maintaining == true, "Only a stopped fake model may prepare its UI")
    }, finish: { [weak self] in
        guard let self else { return }
        busyAtFinish = controller.running || controller.preparing
        completedAtFinish = controller.report?.completed == true
        // Match the shipping failure trigger: the running bundle's old resource
        // path is now missing while its nested SwiftUI sheets are still attached.
        postMoveResourceLookupAttempted = true
        let lookedUp = Bundle.main.url(forResource: "OfflineResource", withExtension: "txt")
        let readable = lookedUp.flatMap { try? Data(contentsOf: $0) }
        postMoveResourceMissing = readable == nil && !FileManager.default.fileExists(atPath: originalResourceURL.path)
        precondition(postMoveResourceMissing, "The actual running bundle must leave its original resource path before termination")
        finished = true
        try! writeReceipt(terminated: false)
        lifecycle.finish()
    })

    init(root: URL, receiptURL: URL, scenario: String) throws {
        self.root = root.resolvingSymlinksInPath()
        self.receiptURL = receiptURL
        self.scenario = scenario
        removeData = scenario.hasSuffix("clear")
        precondition(self.root.path.hasPrefix("/private/tmp/") || self.root.path.hasPrefix("/tmp/"), "Private temporary fixture root required")
        precondition(self.root.pathComponents.contains { $0.hasPrefix("szu-uninstall-e2e.") }, "Recognizable test root required")
        applications = self.root.appendingPathComponent("Applications", isDirectory: true)
        applicationURL = Bundle.main.bundleURL.resolvingSymlinksInPath().standardizedFileURL
        precondition(applicationURL.deletingLastPathComponent() == applications,
                     "The running test bundle, not a sibling dummy, must be the uninstall target")
        precondition(applicationURL.lastPathComponent.hasPrefix("OfflineUninstall-"), "Only a named disposable test application may move")
        precondition(applicationURL.pathExtension == "app" && applicationURL.resolvingSymlinksInPath() == applicationURL,
                     "The test application must be a real temporary bundle, not a symlink")
        guard let resource = Bundle.main.url(forResource: "OfflineResource", withExtension: "txt"),
              (try? String(contentsOf: resource, encoding: .utf8)) == "offline self-trash resource\n" else {
            throw CocoaError(.fileReadNoSuchFile)
        }
        originalResourceURL = resource
        supportParent = self.root.appendingPathComponent("Application Support", isDirectory: true)
        support = supportParent.appendingPathComponent("SZUConnect", isDirectory: true)
        try FileManager.default.createDirectory(at: support, withIntermediateDirectories: true)
        for name in ["events.json", "network-memory.json", "wifi-recovery.json", "websites.json"] {
            try Data("offline fake user data".utf8).write(to: support.appendingPathComponent(name))
        }
    }

    func prepareForAppMaintenance(removeData: Bool) async -> Bool {
        maintenanceCalls += 1
        precondition(removeData == self.removeData, "Cleanup choice must reach maintenance unchanged")
        if !maintaining { maintenanceTransitions += 1 }
        maintaining = true
        return true
    }

    func acknowledgeUninstallCleanup(loginItemRemoved: Bool, credentialsRemoved: Bool) {
        acknowledged = (loginItemRemoved, credentialsRemoved)
    }

    func recordChildBusyState() { childBusyStateRendered = true }

    private func makeService() -> AppUninstallService {
        AppUninstallService(environment: AppUninstallEnvironment(executionAllowed: { true }, applicationDirectories: [applications],
            applicationSupportURL: support, preferenceDomain: "local.SZUConnect.offline-e2e",
            inspectApplication: { [self] in
                inspections += 1
                if suspendNextInspection {
                    suspendNextInspection = false
                    await withCheckedContinuation { pendingInspection = $0 }
                }
                let identity = try AppUninstallTrash.identity(applicationURL)
                return AppUninstallCandidate(applicationURL: applicationURL, resolvedURL: applicationURL,
                    executableURL: Bundle.main.executableURL!.resolvingSymlinksInPath(), bundleIdentifier: AppUninstallService.bundleIdentifier,
                    identity: identity, volumeReadOnly: false, parentWritable: true)
            }, unregisterLoginItem: { [self] in
                precondition(maintaining, "Maintenance must finish before any mutation")
                mutations.append("login-item")
                // Render the real disabled busy state while a fake system request waits.
                try await Task.sleep(for: .milliseconds(50))
            }, deleteCredentials: { [self] in
                mutations.append("credentials"); credentials.removeValue(forKey: "own")
            }, removeSupportData: { [self] url in
                mutations.append("support")
                try AppUninstallDataRemoval.removeOwnedDirectory(url, expectedParent: supportParent)
            }, removePreferences: { [self] _ in
                mutations.append("preferences"); preferences.removeAll()
            }, trashApplication: { [self] url, identity in
                if failNextTrash {
                    failNextTrash = false
                    mutations.append("application-failed")
                    throw CocoaError(.fileWriteNoPermission)
                }
                precondition(url == Bundle.main.bundleURL.resolvingSymlinksInPath() && url == applicationURL,
                             "Only this process's disposable bundle may be moved")
                mutations.append("application")
                return try AppUninstallTrash.move(url, expectedIdentity: identity, moveToTrash: { item in
                    systemTrashCalls += 1
                    var result: NSURL?
                    try FileManager.default.trashItem(at: item, resultingItemURL: &result)
                    systemTrashSuccesses += 1
                    returnedTrashURL = result as URL?
                    return returnedTrashURL
                })
            }))
    }

    private func waitUntil(_ condition: () -> Bool, _ message: String) async {
        let deadline = ContinuousClock.now.advanced(by: .seconds(5))
        while !condition() {
            precondition(.now < deadline, message)
            try? await Task.sleep(for: .milliseconds(5))
        }
    }

    private func presentBothSheets() async {
        showingAbout = true
        await waitUntil({ NSApp.windows.contains { $0.attachedSheet?.attachedSheet != nil } },
                        "Two real SwiftUI sheets must attach before confirmation")
        nestedPresentationCount += 1
        precondition(NSApp.windows.allSatisfy { $0.alphaValue == 0 }, "Fixture windows must remain transparent")
    }

    private func closeBothSheets() async {
        showingConfirmation = false
        await waitUntil({ !NSApp.windows.contains { $0.attachedSheet?.attachedSheet != nil } },
                        "Confirmation must close after cancellation or failure")
        showingAbout = false
        await waitUntil({ NSApp.windows.allSatisfy { $0.attachedSheet == nil && $0.sheetParent == nil } },
                        "About must close after cancellation or failure")
    }

    /// Owned by the application delegate, not by a sheet task that is cancelled
    /// when that sheet closes during the cancellation/failure scenarios.
    func runScenario() async {
        await presentBothSheets()
        if scenario.hasPrefix("cancel-") {
            suspendNextInspection = true
            let confirmation = Task { @MainActor in await controller.confirm(model: self, removeData: removeData) }
            await waitUntil({ pendingInspection != nil }, "Pending read-only preflight must reach the cancellation gate")
            precondition(controller.preparing && !controller.running, "Read-only preparation must remain cancellable")
            controller.cancelPreparation()
            cancellationReleasedUI = !controller.preparing && !controller.running
            cancelledPreflightMadeNoChanges = mutations.isEmpty && maintenanceCalls == 0 && !maintaining
            await closeBothSheets()
            closedBothSheetsAfterCancellation = true
            pendingInspection?.resume(); pendingInspection = nil
            await confirmation.value
            latePreflightMadeNoChanges = mutations.isEmpty && maintenanceCalls == 0 && controller.report == nil
            precondition(cancellationReleasedUI && cancelledPreflightMadeNoChanges && latePreflightMadeNoChanges,
                         "Cancelled preflight and its late result must not perform maintenance or cleanup")
            await presentBothSheets()
        } else if scenario.hasPrefix("retry-") {
            failNextTrash = true
            await controller.confirm(model: self, removeData: removeData)
            failureReleasedUI = !controller.preparing && !controller.running
            failureKeptOwnApplication = FileManager.default.fileExists(atPath: applicationURL.path)
            precondition(controller.report?.failedStep == .application && controller.errorMessage != nil
                         && !finished && failureReleasedUI && failureKeptOwnApplication,
                         "A failed move must release the confirmation and keep the real running test bundle")
            await closeBothSheets()
            closedBothSheetsAfterFailure = true
            await presentBothSheets()
        }
        await controller.confirm(model: self, removeData: removeData)
        precondition(controller.report?.completed == true, controller.errorMessage ?? "Self-uninstall should succeed")
    }

    func writeReceipt(terminated: Bool) throws {
        let sourceAbsent = (try? AppUninstallTrash.isAbsent(applicationURL)) == true
        let values: [String: Any] = [
            "scenario": scenario, "removeData": removeData,
            "bundleIdentifier": Bundle.main.bundleIdentifier ?? "", "runningBundleAtStart": applicationURL.path,
            "nestedPresentationCount": nestedPresentationCount, "finished": finished, "terminated": terminated,
            "busyAtFinish": busyAtFinish, "childBusyStateRendered": childBusyStateRendered,
            "completedAtFinish": completedAtFinish, "maintenanceCalls": maintenanceCalls,
            "maintenanceTransitions": maintenanceTransitions,
            "acknowledgedLoginItem": acknowledged?.0 ?? false, "acknowledgedCredentials": acknowledged?.1 ?? false,
            "mutations": mutations, "inspections": inspections,
            "formalDependenciesConstructed": false, "nativeSignatureInspectionUsed": false,
            "allWindowsTransparent": NSApp.windows.allSatisfy { $0.alphaValue == 0 },
            "attachedSheetsAtTermination": NSApp.windows.filter { $0.attachedSheet != nil || $0.sheetParent != nil }.count,
            "selfBundleMoved": sourceAbsent && returnedTrashURL != nil,
            "sourceBundleAbsent": sourceAbsent,
            "systemTrashCalls": systemTrashCalls, "systemTrashSuccesses": systemTrashSuccesses,
            "resultURLPresent": returnedTrashURL != nil, "resultURL": returnedTrashURL?.path ?? "",
            "postMoveResourceLookupAttempted": postMoveResourceLookupAttempted,
            "postMoveResourceMissing": postMoveResourceMissing,
            "fakeSupportPresent": FileManager.default.fileExists(atPath: support.path),
            "ownFakeCredentialPresent": credentials["own"] != nil,
            "unrelatedFakeCredentialPreserved": credentials["other"] == "keep-unrelated-fixture",
            "fakePreferencesPresent": !preferences.isEmpty,
            "cancellationReleasedUI": cancellationReleasedUI,
            "cancelledPreflightMadeNoChanges": cancelledPreflightMadeNoChanges,
            "latePreflightMadeNoChanges": latePreflightMadeNoChanges,
            "closedBothSheetsAfterCancellation": closedBothSheetsAfterCancellation,
            "failureReleasedUI": failureReleasedUI, "failureKeptOwnApplication": failureKeptOwnApplication,
            "closedBothSheetsAfterFailure": closedBothSheetsAfterFailure,
        ]
        try JSONSerialization.data(withJSONObject: values, options: [.prettyPrinted, .sortedKeys]).write(to: receiptURL, options: .atomic)
    }
}

/// Set opacity before presentation, including each newly attached sheet.
private struct TransparentFixtureWindow: NSViewRepresentable {
    final class Anchor: NSView {
        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            window?.alphaValue = 0
            window?.animationBehavior = .none
        }
    }
    func makeNSView(context: Context) -> NSView { Anchor(frame: .zero) }
    func updateNSView(_ view: NSView, context: Context) { view.window?.alphaValue = 0 }
}

private struct FixtureDashboard: View {
    @ObservedObject var fixture: EndToEndFixture
    var body: some View {
        Text("Offline dashboard fixture").frame(width: 480, height: 300)
            .background(TransparentFixtureWindow())
            .sheet(isPresented: $fixture.showingAbout) { FixtureAbout(fixture: fixture) }
    }
}

private struct FixtureAbout: View {
    @ObservedObject var fixture: EndToEndFixture
    var body: some View {
        VStack {
            Text("Offline About fixture")
            Button("Close") { fixture.showingAbout = false }.keyboardShortcut(.cancelAction)
        }
        .frame(width: 440, height: 260)
        .background(TransparentFixtureWindow())
        .sheet(isPresented: $fixture.showingConfirmation) {
            FixtureConfirmation(fixture: fixture, controller: fixture.controller)
        }
        .onAppear { fixture.showingConfirmation = true }
    }
}

private struct FixtureConfirmation: View {
    @ObservedObject var fixture: EndToEndFixture
    @ObservedObject var controller: AppUninstallController
    var body: some View {
        VStack {
            Text("Offline uninstall confirmation fixture")
            Text(fixture.removeData ? "Clear fake data" : "Keep fake data")
            Button("Cancel") {
                controller.cancelPreparation()
                fixture.showingConfirmation = false
            }.keyboardShortcut(.cancelAction).disabled(controller.running)
        }
        .frame(width: 400, height: 220)
        .background(TransparentFixtureWindow())
        .interactiveDismissDisabled(controller.running)
        .onChange(of: controller.running) { _, running in
            if running { fixture.recordChildBusyState() }
        }
    }
}

@MainActor private final class FixtureApplicationDelegate: NSObject, NSApplicationDelegate {
    let fixture: EndToEndFixture
    private var window: NSWindow?
    private var scenarioTask: Task<Void, Never>?
    init(fixture: EndToEndFixture) { self.fixture = fixture }
    func applicationDidFinishLaunching(_ notification: Notification) {
        let window = NSWindow(contentRect: NSRect(x: -4000, y: -4000, width: 480, height: 300),
            styleMask: [.titled, .closable], backing: .buffered, defer: false)
        window.identifier = NSUserInterfaceItemIdentifier("offline.uninstall-e2e")
        window.isReleasedWhenClosed = false
        window.alphaValue = 0
        window.animationBehavior = .none
        window.contentView = NSHostingView(rootView: FixtureDashboard(fixture: fixture))
        self.window = window
        window.orderFront(nil)
        scenarioTask = Task { @MainActor in await fixture.runScenario() }
    }
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { false }
    func applicationWillTerminate(_ notification: Notification) {
        precondition(fixture.finished && !fixture.controller.running && !fixture.controller.preparing,
            "Successful completion must release UI state before real AppKit termination")
        try! fixture.writeReceipt(terminated: true)
    }
}

@main @MainActor
struct AppUninstallEndToEndTests {
    static func main() throws {
        setbuf(stdout, nil)
        alarm(18)
        let arguments = ProcessInfo.processInfo.arguments
        precondition(arguments.count == 4, "Expected scenario, private fixture root and receipt path")
        let scenario = arguments[1]
        precondition(["keep", "clear", "cancel-keep", "cancel-clear", "retry-keep", "retry-clear"].contains(scenario), "Unknown offline scenario")
        precondition(Bundle.main.bundleIdentifier == "local.SZUConnect.offline.uninstall-e2e", "Separate test bundle required")
        let fixture = try EndToEndFixture(root: URL(fileURLWithPath: arguments[2], isDirectory: true),
            receiptURL: URL(fileURLWithPath: arguments[3]), scenario: scenario)
        let application = NSApplication.shared
        application.setActivationPolicy(.accessory)
        let delegate = FixtureApplicationDelegate(fixture: fixture)
        application.delegate = delegate
        withExtendedLifetime(delegate) { application.run() }
        preconditionFailure("NSApplication.terminate must exit this disposable process")
    }
}
