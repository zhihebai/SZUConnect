import Foundation
import Darwin

@MainActor private final class UninstallFixture {
    let root: URL
    let applications: URL
    let app: URL
    let supportParent: URL
    let support: URL
    let trash: URL
    var allowed = true
    var bundleIdentifier = AppUninstallService.bundleIdentifier
    var candidateURL: URL?
    var resolvedURL: URL?
    var executableURL: URL?
    var volumeReadOnly = false
    var parentWritable = true
    var identityOverride: AppUninstallFileIdentity?
    var fail: AppUninstallStep?
    var calls: [AppUninstallStep] = []
    var registered = true
    var credentials = ["own": "offline-placeholder", "other": "leave-this-alone"]
    var preferences = ["alwaysShowDockIcon": true, "launchAtLogin": true]
    var afterUnregister: (() -> Void)?
    var inspections = 0

    init() throws {
        root = FileManager.default.temporaryDirectory.resolvingSymlinksInPath()
            .appendingPathComponent("szu-uninstall-fixture-" + UUID().uuidString, isDirectory: true)
        applications = root.appendingPathComponent("Applications", isDirectory: true)
        app = applications.appendingPathComponent("OfflineFixture.app", isDirectory: true)
        supportParent = root.appendingPathComponent("Application Support", isDirectory: true)
        support = supportParent.appendingPathComponent("SZUConnect", isDirectory: true)
        trash = root.appendingPathComponent("FixtureTrash", isDirectory: true)
        for url in [app.appendingPathComponent("Contents/MacOS", isDirectory: true), support, trash] {
            try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true,
                attributes: [.posixPermissions: 0o700])
        }
        try Data("not-an-executable; offline fixture".utf8).write(to: app.appendingPathComponent("Contents/MacOS/Fixture"))
        for file in ["events.json", "health.json", "network-memory.json", "network-labels.json", "websites.json", "wifi-recovery.json", "recovery.json"] {
            try Data("offline fixture".utf8).write(to: support.appendingPathComponent(file))
        }
    }
    func clean() { try? FileManager.default.removeItem(at: root) }
    func record(_ step: AppUninstallStep) throws {
        calls.append(step)
        if fail == step { throw AppUninstallError.system("离线模拟失败", 7) }
    }
    func environment() -> AppUninstallEnvironment {
        AppUninstallEnvironment(executionAllowed: { self.allowed }, applicationDirectories: [applications],
            applicationSupportURL: support, preferenceDomain: "local.SZUConnect.offline-uninstall",
            inspectApplication: {
                self.inspections += 1
                var info = stat()
                guard lstat(self.app.path, &info) == 0 else { throw POSIXError(.ENOENT) }
                let url = self.candidateURL ?? self.app
                return AppUninstallCandidate(applicationURL: url, resolvedURL: self.resolvedURL ?? url,
                    executableURL: self.executableURL ?? url.appendingPathComponent("Contents/MacOS/Fixture"),
                    bundleIdentifier: self.bundleIdentifier,
                    identity: self.identityOverride ?? AppUninstallFileIdentity(device: UInt64(info.st_dev), inode: UInt64(info.st_ino)),
                    volumeReadOnly: self.volumeReadOnly, parentWritable: self.parentWritable)
            }, unregisterLoginItem: {
                try self.record(.loginItem); self.registered = false; self.afterUnregister?()
            }, deleteCredentials: {
                try self.record(.credentials); self.credentials.removeValue(forKey: "own")
            }, removeSupportData: { url in
                try self.record(.supportData)
                try AppUninstallDataRemoval.removeOwnedDirectory(url, expectedParent: self.supportParent)
            }, removePreferences: { _ in
                try self.record(.preferences); self.preferences.removeAll()
            }, trashApplication: { url, _ in
                try self.record(.application)
                let target = self.trash.appendingPathComponent(url.lastPathComponent)
                try FileManager.default.moveItem(at: url, to: target)
                return target
            })
    }
}

@main struct AppUninstallTests {
    @MainActor static func main() async throws {
        var checks = 0
        var failures: [String] = []
        func expect(_ condition: @autoclosure () -> Bool, _ name: String) {
            checks += 1
            if !condition() { failures.append(name) }
        }
        func exists(_ url: URL) -> Bool { FileManager.default.fileExists(atPath: url.path) }
        func rejected(_ service: AppUninstallService) async -> Bool { do { _ = try await service.prepare(); return false } catch { return true } }

        do {
            let fixture = try UninstallFixture(); defer { fixture.clean() }
            let service = AppUninstallService(environment: fixture.environment())
            let plan = try await service.prepare()
            expect(fixture.calls.isEmpty && exists(fixture.app) && exists(fixture.support), "preflight only inspects; cancellation needs no rollback")
            let report = await service.execute(plan, removeData: false)
            expect(report.completed && report.trashedApplicationURL != nil, "retaining data moves fake application to private fixture trash")
            expect(!exists(fixture.app) && exists(report.trashedApplicationURL!), "trash result refers to moved application")
            expect(!fixture.registered && fixture.calls == [.loginItem, .application], "unregister runs before trash and retained data is not touched")
            expect(fixture.credentials.count == 2 && !fixture.preferences.isEmpty && exists(fixture.support), "default preserves credentials, Dock setting, network memory, URLs and records")
            expect(report.steps[.credentials] == .notRequested && report.failedStep == nil, "retention is explicitly reported")
        }
        do {
            let fixture = try UninstallFixture(); defer { fixture.clean() }
            let service = AppUninstallService(environment: fixture.environment())
            let report = await service.execute(try await service.prepare(), removeData: true)
            expect(report.completed && fixture.calls == [.loginItem, .credentials, .supportData, .preferences, .application], "complete cleanup follows maintenance sequence")
            expect(fixture.credentials == ["other": "leave-this-alone"], "mock credential deletion preserves unrelated item")
            expect(fixture.preferences.isEmpty && !exists(fixture.support), "full cleanup removes every app data fixture and Dock preferences")
        }
        for failing in [AppUninstallStep.loginItem, .credentials, .supportData, .preferences, .application] {
            let fixture = try UninstallFixture(); defer { fixture.clean() }
            fixture.fail = failing
            let service = AppUninstallService(environment: fixture.environment())
            let report = await service.execute(try await service.prepare(), removeData: true)
            expect(!report.completed && report.failedStep == failing && report.failureMessage != nil, "\(failing) failure reports actual failing stage")
            expect(exists(fixture.app) && report.trashedApplicationURL == nil, "\(failing) failure leaves application available for retry")
            if failing == .loginItem { expect(fixture.credentials.count == 2 && exists(fixture.support) && !fixture.preferences.isEmpty, "login item failure cannot clear data") }
            if failing == .credentials { expect(exists(fixture.support) && !fixture.preferences.isEmpty, "credential failure cannot clear remaining data") }
            fixture.fail = nil
            let retried = await service.execute(try await service.prepare(), removeData: true)
            expect(retried.completed, "\(failing) partial result is safely retryable")
        }
        for removeData in [false, true] {
            let fixture = try UninstallFixture(); defer { fixture.clean() }
            var environment = fixture.environment()
            environment.trashApplication = { url, _ in
                try fixture.record(.application)
                try FileManager.default.moveItem(at: url, to: fixture.trash.appendingPathComponent(url.lastPathComponent))
                // A filesystem API can move successfully and fail while checking
                // its result. The caller must not promise success or retention.
                throw AppUninstallError.system("uninstall.mac.error.trash", 1)
            }
            let service = AppUninstallService(environment: environment)
            let plan = try await service.prepare()
            let report = await service.execute(plan, removeData: removeData)
            expect(!report.completed && report.failedStep == .application && report.trashedApplicationURL == nil,
                   "trash verification failure cannot claim completion (cleanup=\(removeData))")
            expect(!exists(fixture.app) && exists(fixture.trash.appendingPathComponent(fixture.app.lastPathComponent)),
                   "failed result can coexist with application already moved (cleanup=\(removeData))")
            expect(report.failureMessage?.contains("仍保留") == false && report.failureMessage?.contains("访达") == true,
                   "uncertain trash message asks to inspect Finder without claiming retention (cleanup=\(removeData))")
            expect(report.steps[.loginItem] == .completed && !fixture.registered,
                   "uncertain move accurately retains completed login-item status (cleanup=\(removeData))")
            expect(report.steps[.credentials] == (removeData ? .completed : .notRequested),
                   "uncertain move accurately retains selected data status (cleanup=\(removeData))")
            let oldCalls = fixture.calls
            let wasRejected147 = await rejected(service)
            expect(wasRejected147, "a missing original app prevents fresh retry preflight (cleanup=\(removeData))")
            let retry = await service.execute(plan, removeData: removeData)
            expect(retry.failedStep == .validation && fixture.calls == oldCalls,
                   "reusing the old plan cannot mutate anything after a move (cleanup=\(removeData))")
        }
        do {
            let fixture = try UninstallFixture(); defer { fixture.clean() }
            let target = fixture.root.appendingPathComponent(".Trash/Fixture.app")
            let identity = try AppUninstallTrash.identity(fixture.app)
            var destinationReadAttempted = false
            let result = try AppUninstallTrash.move(fixture.app, expectedIdentity: identity,
                moveToTrash: { source in
                    try FileManager.default.createDirectory(at: target.deletingLastPathComponent(), withIntermediateDirectories: true)
                    try FileManager.default.moveItem(at: source, to: target)
                    return target
                }, sourceIdentity: { item in
                    if item == target { destinationReadAttempted = true; throw POSIXError(.EPERM) }
                    return try AppUninstallTrash.identity(item)
                })
            expect(result == target && !exists(fixture.app), "successful Trash API and absent source establish completed move")
            expect(!destinationReadAttempted, "a TCC-denied destination stat is never required after a successful move")
        }
        for suspicious in ["nil-result", "same-source", "non-file", "not-trash", "wrong-extension", "source-remains", "source-inaccessible", "changed-identity"] {
            let fixture = try UninstallFixture(); defer { fixture.clean() }
            let identity = try AppUninstallTrash.identity(fixture.app)
            var called = false, failed = false
            do {
                _ = try AppUninstallTrash.move(fixture.app, expectedIdentity: identity,
                    moveToTrash: { source in
                        called = true
                        switch suspicious {
                        case "nil-result": return nil
                        case "same-source": return source
                        case "non-file": return URL(string: "https://example.invalid/.Trash/Fixture.app")!
                        case "not-trash": return fixture.root.appendingPathComponent("other/Fixture.app")
                        case "wrong-extension": return fixture.root.appendingPathComponent(".Trash/other.txt")
                        default: return fixture.root.appendingPathComponent(".Trash/Fixture.app")
                        }
                    }, sourceIdentity: { _ in
                        suspicious == "changed-identity" ? .init(device: 0, inode: 0) : identity
                    }, sourceIsAbsent: { _ in
                        if suspicious == "source-inaccessible" { throw POSIXError(.EPERM) }
                        return suspicious != "source-remains"
                    })
            } catch { failed = true }
            expect(failed, "\(suspicious) Trash result fails closed")
            if suspicious == "changed-identity" { expect(!called, "changed source identity is rejected before moving") }
        }
        do {
            let fixture = try UninstallFixture(); defer { fixture.clean() }
            var environment = fixture.environment()
            let initialInspection = environment.inspectApplication
            let candidate = try await initialInspection()
            var pending: CheckedContinuation<AppUninstallCandidate, any Error>?
            environment.inspectApplication = {
                try await withCheckedThrowingContinuation { pending = $0 }
            }
            let service = AppUninstallService(environment: environment)
            let inspection = Task { @MainActor in try await service.prepare() }
            for _ in 0..<100 where pending == nil { await Task.yield() }
            var mainActorResponsive = false
            await Task { @MainActor in mainActorResponsive = true }.value
            expect(pending != nil && mainActorResponsive && fixture.calls.isEmpty,
                   "suspended read-only inspection permits UI work and performs no mutations")
            inspection.cancel()
            pending?.resume(returning: candidate)
            var rejectedLateResult = false
            do { _ = try await inspection.value } catch { rejectedLateResult = true }
            expect(rejectedLateResult && fixture.calls.isEmpty, "cancelled preparation discards a late inspection result")
        }
        do {
            let fixture = try UninstallFixture(); defer { fixture.clean() }
            var environment = fixture.environment()
            let initialInspection = environment.inspectApplication
            let candidate = try await initialInspection()
            var pending: CheckedContinuation<AppUninstallCandidate, any Error>?
            environment.inspectApplication = {
                try await withCheckedThrowingContinuation { pending = $0 }
            }
            let service = AppUninstallService(environment: environment)
            let inspection = Task { @MainActor in try await service.prepare() }
            for _ in 0..<100 where pending == nil { await Task.yield() }
            fixture.allowed = false
            pending?.resume(returning: candidate)
            var denied = false
            do { _ = try await inspection.value } catch { denied = true }
            expect(denied && fixture.calls.isEmpty, "mode change while inspection waits revokes its late result")
        }
        do {
            let fixture = try UninstallFixture(); defer { fixture.clean() }
            var environment = fixture.environment()
            let initialInspection = environment.inspectApplication
            let candidate = try await initialInspection()
            var suspendInspection = false
            var pending: CheckedContinuation<AppUninstallCandidate, any Error>?
            environment.inspectApplication = {
                if suspendInspection {
                    suspendInspection = false
                    return try await withCheckedThrowingContinuation { pending = $0 }
                }
                return try await initialInspection()
            }
            let service = AppUninstallService(environment: environment)
            let plan = try await service.prepare()
            suspendInspection = true
            let first = Task { @MainActor in await service.execute(plan, removeData: false) }
            for _ in 0..<100 where pending == nil { await Task.yield() }
            let second = await service.execute(plan, removeData: false)
            expect(second.failedStep == .validation && fixture.calls.isEmpty,
                   "an awaiting execution owns the service before its first suspension")
            pending?.resume(returning: candidate)
            let report = await first.value
            expect(report.completed && fixture.calls == [.loginItem, .application],
                   "concurrent confirmation cannot duplicate destructive steps")
        }
        for mode in ["observe-only", "preview", "offline"] {
            let fixture = try UninstallFixture(); defer { fixture.clean() }
            fixture.allowed = false
            let service = AppUninstallService(environment: fixture.environment())
            let wasRejected156 = await rejected(service)
            expect(wasRejected156 && fixture.inspections == 0 && fixture.calls.isEmpty, "\(mode) refuses before inspecting or mutating live dependencies")
        }
        do {
            let native = AppUninstallService.native(executionAllowed: { false })
            let wasRejected160 = await rejected(native)
            expect(wasRejected160, "native isolated factory cannot inspect credentials, login items or application")
        }
        for mutation in ["mode", "identity", "path"] {
            let fixture = try UninstallFixture(); defer { fixture.clean() }
            let service = AppUninstallService(environment: fixture.environment())
            let plan = try await service.prepare()
            if mutation == "mode" { fixture.allowed = false }
            else if mutation == "identity" { fixture.identityOverride = .init(device: 0, inode: 0) }
            else { fixture.candidateURL = fixture.root.appendingPathComponent("Downloads/Other.app") }
            let report = await service.execute(plan, removeData: true)
            expect(!report.completed && report.failedStep == .validation && fixture.calls.isEmpty, "\(mutation) change after confirmation prevents all mutation")
        }
        for mutation in ["mode", "identity"] {
            let fixture = try UninstallFixture(); defer { fixture.clean() }
            let service = AppUninstallService(environment: fixture.environment())
            fixture.afterUnregister = {
                if mutation == "mode" { fixture.allowed = false }
                else { fixture.identityOverride = .init(device: 0, inode: 0) }
            }
            let report = await service.execute(try await service.prepare(), removeData: true)
            expect(!report.completed && report.failedStep == .validation && fixture.calls == [.loginItem], "\(mutation) change during async unregister stops cleanup")
            expect(report.steps[.loginItem] == .completed && exists(fixture.app), "\(mutation) partial unregistration is accurately reported")
        }
        for kind in ["wrong-bundle", "wrong-executable", "symlink", "read-only", "no-permission", "downloads", "source", "backup", "volume", "translocation", "nested", "wrong-extension", "trash"] {
            let fixture = try UninstallFixture(); defer { fixture.clean() }
            switch kind {
            case "wrong-bundle": fixture.bundleIdentifier = "local.other-app"
            case "wrong-executable": fixture.executableURL = fixture.root.appendingPathComponent("external")
            case "symlink": fixture.resolvedURL = fixture.root.appendingPathComponent("External.app")
            case "read-only": fixture.volumeReadOnly = true
            case "no-permission": fixture.parentWritable = false
            case "downloads": fixture.candidateURL = fixture.root.appendingPathComponent("Downloads/Fixture.app")
            case "source": fixture.candidateURL = fixture.root.appendingPathComponent("source/build/Fixture.app")
            case "backup": fixture.candidateURL = fixture.root.appendingPathComponent("backups/Fixture.app")
            case "volume": fixture.candidateURL = URL(fileURLWithPath: "/Volumes/Fixture/Fixture.app")
            case "translocation": fixture.candidateURL = URL(fileURLWithPath: "/private/var/folders/AppTranslocation/Fixture.app")
            case "nested": fixture.candidateURL = fixture.app.appendingPathComponent("Contents/Other.app")
            case "wrong-extension": fixture.candidateURL = fixture.app.deletingPathExtension()
            default: fixture.candidateURL = fixture.root.appendingPathComponent(".Trash/Fixture.app")
            }
            let wasRejected200 = await rejected(AppUninstallService(environment: fixture.environment()))
            expect(wasRejected200 && fixture.calls.isEmpty, "\(kind) application cannot be uninstalled")
        }
        do {
            let fixture = try UninstallFixture(); defer { fixture.clean() }
            let first = AppUninstallService(environment: fixture.environment())
            let second = AppUninstallService(environment: fixture.environment())
            let report = await second.execute(try await first.prepare(), removeData: true)
            expect(report.failedStep == .validation && fixture.calls.isEmpty, "plans cannot be reused by a different service instance")
        }
        do {
            let fixture = try UninstallFixture(); defer { fixture.clean() }
            let executable = fixture.app.appendingPathComponent("Contents/MacOS/Fixture")
            try FileManager.default.removeItem(at: executable)
            let outside = fixture.root.appendingPathComponent("outside-executable")
            try Data("offline only".utf8).write(to: outside)
            try FileManager.default.createSymbolicLink(at: executable, withDestinationURL: outside)
            let wasRejected216 = await rejected(AppUninstallService(environment: fixture.environment()))
            expect(wasRejected216 && exists(outside), "executable symlink outside bundle is rejected")
        }
        do {
            let fixture = try UninstallFixture(); defer { fixture.clean() }
            let service = AppUninstallService(environment: fixture.environment())
            let plan = try await service.prepare()
            let task = Task { @MainActor in await service.execute(plan, removeData: true) }
            task.cancel()
            let report = await task.value
            expect(report.failedStep == .validation && fixture.calls.isEmpty, "task cancellation before execution cannot unregister or delete")
        }
        do {
            let fixture = try UninstallFixture(); defer { fixture.clean() }
            let external = fixture.root.appendingPathComponent("unrelated-data", isDirectory: true)
            try FileManager.default.createDirectory(at: external, withIntermediateDirectories: true)
            let protected = external.appendingPathComponent("must-stay.txt")
            try Data("keep".utf8).write(to: protected)
            try FileManager.default.createSymbolicLink(at: fixture.support.appendingPathComponent("external-link"), withDestinationURL: external)
            try FileManager.default.createSymbolicLink(at: fixture.support.appendingPathComponent("file-link"), withDestinationURL: protected)
            let nested = fixture.support.appendingPathComponent("nested/deep", isDirectory: true)
            try FileManager.default.createDirectory(at: nested, withIntermediateDirectories: true)
            try Data("remove".utf8).write(to: nested.appendingPathComponent("fixture"))
            try AppUninstallDataRemoval.removeOwnedDirectory(fixture.support, expectedParent: fixture.supportParent)
            expect(!exists(fixture.support) && exists(protected), "descriptor cleanup removes child links without following targets")
            expect((try? String(contentsOf: protected, encoding: .utf8)) == "keep", "unrelated bytes survive nested and symlink cleanup")
            try AppUninstallDataRemoval.removeOwnedDirectory(fixture.support, expectedParent: fixture.supportParent)
            expect(!exists(fixture.support), "missing support directory cleanup is idempotent")
        }
        for rootKind in ["symlink-root", "wrong-name", "outside-parent", "symlink-parent"] {
            let fixture = try UninstallFixture(); defer { fixture.clean() }
            let external = fixture.root.appendingPathComponent("unrelated-data", isDirectory: true)
            try FileManager.default.createDirectory(at: external, withIntermediateDirectories: true)
            let protected = external.appendingPathComponent("must-stay.txt")
            try Data("keep".utf8).write(to: protected)
            var target = fixture.support, parent = fixture.supportParent
            if rootKind == "symlink-root" {
                try FileManager.default.removeItem(at: target)
                try FileManager.default.createSymbolicLink(at: target, withDestinationURL: external)
            } else if rootKind == "wrong-name" { target = external }
            else if rootKind == "outside-parent" { target = fixture.root.appendingPathComponent("SZUConnect") }
            else {
                let linked = fixture.root.appendingPathComponent("linked-parent")
                try FileManager.default.createSymbolicLink(at: linked, withDestinationURL: fixture.supportParent)
                parent = linked; target = linked.appendingPathComponent("SZUConnect")
            }
            var failed = false
            do { try AppUninstallDataRemoval.removeOwnedDirectory(target, expectedParent: parent) } catch { failed = true }
            expect(failed && exists(protected), "\(rootKind) cleanup is rejected without reaching unrelated data")
        }
        do {
            let fixture = try UninstallFixture(); defer { fixture.clean() }
            let service = AppUninstallService(environment: fixture.environment())
            let domain = "local.SZUConnect.offline-uninstall.\(UUID().uuidString)"
            let preferences = UserDefaults(suiteName: domain)!
            defer { preferences.removePersistentDomain(forName: domain); _ = preferences.synchronize() }
            preferences.set(true, forKey: "alwaysShowDockIcon")
            var env = fixture.environment()
            env.preferenceDomain = domain
            env.removePreferences = { target in
                guard target == domain else { throw AppUninstallError.invalidDataLocation }
                preferences.removePersistentDomain(forName: target)
                guard preferences.synchronize() else { throw AppUninstallError.system("fixture sync", 1) }
            }
            let isolatedService = AppUninstallService(environment: env)
            let report = await isolatedService.execute(try await isolatedService.prepare(), removeData: true)
            expect(report.completed && preferences.persistentDomain(forName: domain)?.isEmpty != false, "isolated real UserDefaults domain can be cleared")
            _ = service
        }
        if !failures.isEmpty {
            for failure in failures { print("FAIL: \(failure)") }
            exit(1)
        }
        print("App uninstall: \(checks) offline assertions passed; temporary fake applications, mock login items and credentials only.")
    }
}
