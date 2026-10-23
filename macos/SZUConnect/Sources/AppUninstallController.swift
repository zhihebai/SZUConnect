#if os(macOS)
import Foundation
import Combine

@MainActor protocol AppUninstallModel: AnyObject {
    var canUninstallApplication: Bool { get }
    func prepareForAppMaintenance(removeData: Bool) async -> Bool
    func acknowledgeUninstallCleanup(loginItemRemoved: Bool, credentialsRemoved: Bool)
}

/// Opening or cancelling the confirmation never inspects or changes the installation.
/// A pending read-only preflight may finish after dismissal; only its current owner
/// can advance to maintenance or cleanup.
@MainActor final class AppUninstallController: ObservableObject {
    @Published private(set) var preparing = false
    @Published private(set) var running = false
    @Published private(set) var errorMessage: String?
    @Published private(set) var report: AppUninstallReport?
    private var service: AppUninstallService?
    private var operationID: UUID?
    private let prepareUIForMaintenance: () async -> Void
    private let finish: () -> Void

    init(service: AppUninstallService? = nil,
         prepareUIForMaintenance: @escaping () async -> Void,
         finish: @escaping () -> Void) {
        self.service = service
        self.prepareUIForMaintenance = prepareUIForMaintenance
        self.finish = finish
    }

    func configure(model: any AppUninstallModel) {
        if service == nil {
            service = .native(executionAllowed: { [weak model] in model?.canUninstallApplication == true })
        }
    }

    func cancelPreparation() {
        guard !running else { return }
        operationID = nil
        preparing = false
    }

    func confirm(model: any AppUninstallModel, removeData: Bool) async {
        guard !preparing, !running, model.canUninstallApplication else { return }
        configure(model: model)
        guard let service else { return }
        let id = UUID()
        operationID = id
        preparing = true
        errorMessage = nil
        report = nil
        defer {
            if operationID == id {
                operationID = nil
                preparing = false
                running = false
            }
        }
        let plan: AppUninstallPlan
        do { plan = try await service.prepare() }
        catch {
            if operationID == id { errorMessage = error.localizedDescription }
            return
        }
        guard operationID == id, !Task.isCancelled, model.canUninstallApplication else { return }
        preparing = false
        running = true
        guard await model.prepareForAppMaintenance(removeData: removeData) else {
            errorMessage = SharedCopy.t("uninstall.mac.maintenanceUnavailable")
            return
        }
        await prepareUIForMaintenance()
        let result = await service.execute(plan, removeData: removeData)
        report = result
        model.acknowledgeUninstallCleanup(loginItemRemoved: result.steps[.loginItem] == .completed,
                                         credentialsRemoved: result.steps[.credentials] == .completed)
        if result.completed {
            // Both nested sheets must be dismissible before AppKit handles termination.
            running = false
            operationID = nil
            finish()
        } else {
            errorMessage = result.failureMessage ?? SharedCopy.t("uninstall.failed")
        }
    }
}
#endif
