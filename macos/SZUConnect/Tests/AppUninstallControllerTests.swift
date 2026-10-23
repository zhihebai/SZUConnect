import Foundation

@MainActor final class ModelFixture: AppUninstallModel {
    var canUninstallApplication = true
    var maintenanceCalls = 0
    var acknowledged: (Bool, Bool)?
    func prepareForAppMaintenance(removeData: Bool) async -> Bool { maintenanceCalls += 1; return true }
    func acknowledgeUninstallCleanup(loginItemRemoved: Bool, credentialsRemoved: Bool) { acknowledged = (loginItemRemoved, credentialsRemoved) }
}

@MainActor final class ServiceFixture {
    var inspections = 0
    var mutations: [String] = []
    var pauseFirstInspection = false
    var gate: CheckedContinuation<Void, Never>?
    var failTrash = false
    var allowed = true
    func makeService() -> AppUninstallService {
        let apps = URL(fileURLWithPath: "/private/tmp/SZUConnect-controller-virtual/Applications", isDirectory: true)
        let app = apps.appendingPathComponent("Fixture.app", isDirectory: true)
        return AppUninstallService(environment: AppUninstallEnvironment(executionAllowed: { self.allowed }, applicationDirectories: [apps],
            applicationSupportURL: apps, preferenceDomain: "offline-fixture",
            inspectApplication: {
                self.inspections += 1
                if self.pauseFirstInspection && self.inspections == 1 { await withCheckedContinuation { self.gate = $0 } }
                return AppUninstallCandidate(applicationURL: app, resolvedURL: app,
                    executableURL: app.appendingPathComponent("Contents/MacOS/Fixture"), bundleIdentifier: AppUninstallService.bundleIdentifier,
                    identity: AppUninstallFileIdentity(device: 1, inode: 2), volumeReadOnly: false, parentWritable: true)
            }, unregisterLoginItem: { self.mutations.append("login") }, deleteCredentials: { self.mutations.append("credentials") },
            removeSupportData: { _ in self.mutations.append("support") }, removePreferences: { _ in self.mutations.append("preferences") },
            trashApplication: { _, _ in
                self.mutations.append("trash")
                if self.failTrash { throw CocoaError(.fileWriteNoPermission) }
                return URL(fileURLWithPath: "/private/tmp/SZUConnect-controller-virtual/.Trash/Fixture.app")
            }))
    }
}

@main struct AppUninstallControllerTests {
    @MainActor static func main() async {
        var assertions = 0
        func expect(_ condition: Bool, _ message: String) { assertions += 1; if !condition { fatalError(message) } }
        do {
            let fixture = ServiceFixture(), model = ModelFixture()
            let controller = AppUninstallController(service: fixture.makeService(), prepareUIForMaintenance: {}, finish: {})
            controller.configure(model: model)
            controller.cancelPreparation()
            expect(fixture.inspections == 0 && fixture.mutations.isEmpty && model.maintenanceCalls == 0, "Opening and cancelling must do no preflight or mutation")
            fixture.pauseFirstInspection = true
            let task = Task { @MainActor in await controller.confirm(model: model, removeData: true) }
            while fixture.gate == nil { await Task.yield() }
            expect(controller.preparing && !controller.running, "Slow preflight must allow cancellation")
            var heartbeat = false
            let ticker = Task { @MainActor in heartbeat = true }
            await ticker.value
            expect(heartbeat, "Main actor must remain responsive during preflight")
            controller.cancelPreparation()
            expect(!controller.preparing && !controller.running, "Cancellation clears only pending preflight")
            fixture.gate?.resume(); fixture.gate = nil
            await task.value
            expect(fixture.mutations.isEmpty && model.maintenanceCalls == 0 && controller.report == nil, "Late preflight must not enter maintenance or mutate")
        }
        for removeData in [false, true] {
            let fixture = ServiceFixture(), model = ModelFixture()
            var finished = 0, preparedUI = 0
            var controller: AppUninstallController!
            controller = AppUninstallController(service: fixture.makeService(), prepareUIForMaintenance: { preparedUI += 1 }, finish: {
                expect(!controller.running && !controller.preparing, "Success must clear busy state before termination")
                expect(controller.report?.completed == true && model.maintenanceCalls == 1 && model.acknowledged?.0 == true, "Terminate only after all requested work and acknowledgement")
                finished += 1
            })
            await controller.confirm(model: model, removeData: removeData)
            expect(finished == 1 && preparedUI == 1 && controller.report?.completed == true, "Successful uninstall finishes once")
            expect(fixture.mutations == (removeData ? ["login", "credentials", "support", "preferences", "trash"] : ["login", "trash"]), "Keep/clean choice must match exact mutations")
            expect(model.acknowledged?.1 == removeData, "RAM credentials acknowledgement matches cleanup choice")
            expect(fixture.inspections == (removeData ? 5 : 4), "Exactly one confirmation preflight; no duplicate opening check")
        }
        do {
            let fixture = ServiceFixture(), model = ModelFixture(); fixture.failTrash = true
            var finished = 0
            let controller = AppUninstallController(service: fixture.makeService(), prepareUIForMaintenance: {}, finish: { finished += 1 })
            await controller.confirm(model: model, removeData: true)
            expect(finished == 0 && controller.report?.completed == false && controller.errorMessage != nil, "Failed trash must show a result and never claim success")
            expect(!controller.running && !controller.preparing, "Failure must release UI busy state so sheets can close")
            fixture.failTrash = false
            await controller.confirm(model: model, removeData: true)
            expect(finished == 1 && controller.report?.completed == true, "Retry remains possible")
        }
        do {
            let fixture = ServiceFixture(), model = ModelFixture(); model.canUninstallApplication = false
            let controller = AppUninstallController(service: fixture.makeService(), prepareUIForMaintenance: {}, finish: { fatalError("Read-only must not finish") })
            await controller.confirm(model: model, removeData: true)
            expect(fixture.inspections == 0 && fixture.mutations.isEmpty && model.maintenanceCalls == 0, "Read-only forbids any uninstall action")
        }
        print("App uninstall controller: \(assertions) assertions passed; fake model and injected services only.")
    }
}
