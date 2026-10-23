#if os(macOS)
import Foundation
import Darwin
import Security
import ServiceManagement

enum AppUninstallStep: String, CaseIterable {
    case validation, loginItem, credentials, supportData, preferences, application
}

enum AppUninstallStepState: Equatable {
    case notRequested, pending, completed, failed(String)
}

struct AppUninstallReport {
    var steps: [AppUninstallStep: AppUninstallStepState]
    var trashedApplicationURL: URL?
    var completed: Bool {
        trashedApplicationURL != nil && steps[.validation] == .completed
            && steps[.loginItem] == .completed && steps[.application] == .completed
            && [.credentials, .supportData, .preferences].allSatisfy {
                steps[$0] == .completed || steps[$0] == .notRequested
            }
    }
    var failedStep: AppUninstallStep? {
        AppUninstallStep.allCases.first { if case .failed = steps[$0] { return true }; return false }
    }
    var failureMessage: String? {
        guard let failedStep, case .failed(let message) = steps[failedStep] else { return nil }
        return message
    }
}

struct AppUninstallFileIdentity: Equatable, Sendable {
    let device: UInt64
    let inode: UInt64
}

struct AppUninstallCandidate: Sendable {
    let applicationURL: URL
    let resolvedURL: URL
    let executableURL: URL
    let bundleIdentifier: String
    let identity: AppUninstallFileIdentity
    let volumeReadOnly: Bool
    let parentWritable: Bool
}

/// Dependencies are explicit so tests cannot accidentally reach the live account,
/// login item, application, or preference domain.
@MainActor struct AppUninstallEnvironment {
    var executionAllowed: () -> Bool
    var applicationDirectories: [URL]
    var applicationSupportURL: URL
    var preferenceDomain: String
    var inspectApplication: () async throws -> AppUninstallCandidate
    var unregisterLoginItem: () async throws -> Void
    var deleteCredentials: () async throws -> Void
    var removeSupportData: (URL) throws -> Void
    var removePreferences: (String) throws -> Void
    var trashApplication: (URL, AppUninstallFileIdentity) throws -> URL
}

struct AppUninstallPlan {
    let applicationURL: URL
    fileprivate let identity: AppUninstallFileIdentity
    fileprivate let serviceID: UUID
}

enum AppUninstallError: LocalizedError {
    case isolated, invalidIdentity, unsafeLocation, readOnlyLocation, permissionDenied
    case applicationChanged, invalidPlan, invalidDataLocation, operationInProgress, cancelled
    case system(String, Int)

    var errorDescription: String? {
        let key: String
        switch self {
        case .isolated: key = "isolated"
        case .invalidIdentity: key = "invalidIdentity"
        case .unsafeLocation: key = "unsafeLocation"
        case .readOnlyLocation: key = "readOnlyLocation"
        case .permissionDenied: key = "permissionDenied"
        case .applicationChanged: key = "applicationChanged"
        case .invalidPlan: key = "invalidPlan"
        case .invalidDataLocation: key = "invalidDataLocation"
        case .operationInProgress: key = "operationInProgress"
        case .cancelled: key = "cancelled"
        case .system(let messageKey, let code):
            return SharedCopy.f("uninstall.mac.error.withCode", ["message": SharedCopy.t(messageKey), "code": String(code)])
        }
        return SharedCopy.t("uninstall.mac.error." + key)
    }
}

/// The caller must stop and drain all writers before execute. A cleanup failure
/// leaves the application installed. A trash error may have an uncertain result;
/// never infer that the application stayed in place from completed == false.
/// Every retry performs a fresh validation without restoring data or login items.
@MainActor final class AppUninstallService {
    nonisolated static let bundleIdentifier = "local.SZUConnect.mac"
    private let environment: AppUninstallEnvironment
    private let serviceID = UUID()
    private var executing = false

    init(environment: AppUninstallEnvironment) { self.environment = environment }

    func prepare() async throws -> AppUninstallPlan {
        try requireExecutionAllowed()
        let candidate = try await validatedCandidate()
        return AppUninstallPlan(applicationURL: candidate.applicationURL,
                                identity: candidate.identity, serviceID: serviceID)
    }

    func execute(_ plan: AppUninstallPlan, removeData: Bool) async -> AppUninstallReport {
        var report = AppUninstallReport(steps: Dictionary(uniqueKeysWithValues:
            AppUninstallStep.allCases.map { ($0, .pending) }))
        if !removeData {
            for step in [AppUninstallStep.credentials, .supportData, .preferences] { report.steps[step] = .notRequested }
        }
        var step = AppUninstallStep.validation
        do {
            guard !executing else { throw AppUninstallError.operationInProgress }
            guard plan.serviceID == serviceID else { throw AppUninstallError.invalidPlan }
            executing = true
            defer { executing = false }
            try await validate(plan)
            report.steps[.validation] = .completed
            step = .loginItem
            try await environment.unregisterLoginItem()
            report.steps[step] = .completed
            // Unregistration can suspend and present system UI. Check again before
            // deleting anything, and reject a mode or application change.
            step = .validation
            try await validate(plan)
            if removeData {
                step = .credentials
                try await environment.deleteCredentials()
                report.steps[step] = .completed
                step = .validation
                try await validate(plan)
                step = .supportData
                try environment.removeSupportData(environment.applicationSupportURL)
                report.steps[step] = .completed
                step = .preferences
                try environment.removePreferences(environment.preferenceDomain)
                report.steps[step] = .completed
            }
            step = .validation
            try await validate(plan)
            step = .application
            let trashed = try environment.trashApplication(plan.applicationURL, plan.identity)
            report.trashedApplicationURL = trashed
            report.steps[step] = .completed
        } catch {
            report.steps[step] = .failed(error.localizedDescription)
        }
        return report
    }

    private func requireExecutionAllowed() throws {
        guard !Task.isCancelled else { throw AppUninstallError.cancelled }
        guard environment.executionAllowed() else { throw AppUninstallError.isolated }
    }

    private func validate(_ plan: AppUninstallPlan) async throws {
        try requireExecutionAllowed()
        let current = try await validatedCandidate()
        guard current.applicationURL == plan.applicationURL, current.identity == plan.identity else {
            throw AppUninstallError.applicationChanged
        }
    }

    private func validatedCandidate() async throws -> AppUninstallCandidate {
        let candidate = try await environment.inspectApplication()
        try requireExecutionAllowed()
        let executableURL = candidate.executableURL
        let resolvedExecutableURL = await Task.detached(priority: .userInitiated) {
            executableURL.resolvingSymlinksInPath()
        }.value
        // Read-only native inspection can outlive cancellation. Its late result
        // never grants permission to unregister, delete data, or move the app.
        try requireExecutionAllowed()
        let url = candidate.applicationURL.standardizedFileURL
        let parent = url.deletingLastPathComponent()
        guard url.isFileURL, candidate.applicationURL == url,
              url.pathExtension.lowercased() == "app", candidate.resolvedURL == url,
              !url.pathComponents.contains("AppTranslocation"),
              !url.pathComponents.contains(".Trash"), !url.path.hasPrefix("/Volumes/"),
              environment.applicationDirectories.contains(where: { $0.standardizedFileURL == parent }) else {
            throw AppUninstallError.unsafeLocation
        }
        guard candidate.bundleIdentifier == Self.bundleIdentifier,
              candidate.executableURL.deletingLastPathComponent() == url.appendingPathComponent("Contents/MacOS", isDirectory: true),
              resolvedExecutableURL.deletingLastPathComponent() == url.appendingPathComponent("Contents/MacOS", isDirectory: true) else {
            throw AppUninstallError.invalidIdentity
        }
        guard !candidate.volumeReadOnly else { throw AppUninstallError.readOnlyLocation }
        guard candidate.parentWritable else { throw AppUninstallError.permissionDenied }
        return candidate
    }

    static func native(executionAllowed: @escaping () -> Bool) -> AppUninstallService {
        let manager = FileManager.default
        let home = manager.homeDirectoryForCurrentUser
        let support = home.appendingPathComponent("Library/Application Support/SZUConnect", isDirectory: true)
        let environment = AppUninstallEnvironment(executionAllowed: {
                let arguments = ProcessInfo.processInfo.arguments
                return executionAllowed() && Bundle.main.bundleIdentifier == bundleIdentifier
                    && !arguments.contains("--observe-only") && !arguments.contains("--offline-preview")
                    && Bundle.main.object(forInfoDictionaryKey: "SZUOfflinePreview") as? Bool != true
            },
            applicationDirectories: [URL(fileURLWithPath: "/Applications", isDirectory: true), home.appendingPathComponent("Applications", isDirectory: true)],
            applicationSupportURL: support, preferenceDomain: bundleIdentifier,
            inspectApplication: {
                try await Task.detached(priority: .userInitiated) {
                    try AppUninstallNative.inspectCurrentApplication()
                }.value
            },
            unregisterLoginItem: {
                if SMAppService.mainApp.status != .notRegistered && SMAppService.mainApp.status != .notFound {
                    try await SMAppService.mainApp.unregister()
                }
                guard SMAppService.mainApp.status == .notRegistered || SMAppService.mainApp.status == .notFound else {
                    throw AppUninstallError.system("uninstall.mac.error.loginItem", Int(SMAppService.mainApp.status.rawValue))
                }
            }, deleteCredentials: {
                try await Task.detached(priority: .userInitiated) { try AppUninstallNative.deleteCredentials() }.value
            }, removeSupportData: { try AppUninstallDataRemoval.removeOwnedDirectory($0, expectedParent: home.appendingPathComponent("Library/Application Support", isDirectory: true)) },
            removePreferences: { domain in
                guard domain == bundleIdentifier else { throw AppUninstallError.invalidDataLocation }
                UserDefaults.standard.removePersistentDomain(forName: domain)
                guard UserDefaults.standard.synchronize(), UserDefaults.standard.persistentDomain(forName: domain)?.isEmpty != false else {
                    throw AppUninstallError.system("uninstall.mac.error.preferences", 1)
                }
            }, trashApplication: { url, identity in
                try AppUninstallTrash.move(url, expectedIdentity: identity)
            })
        return AppUninstallService(environment: environment)
    }
}

/// A successful system Trash call is authoritative. Reading the returned Trash
/// location may be denied by macOS privacy controls; it is not a success condition.
enum AppUninstallTrash {
    static func move(_ url: URL, expectedIdentity: AppUninstallFileIdentity,
                     moveToTrash: (URL) throws -> URL? = { item in
                         var result: NSURL?
                         try FileManager.default.trashItem(at: item, resultingItemURL: &result)
                         return result as URL?
                     }, sourceIdentity: (URL) throws -> AppUninstallFileIdentity = identity,
                     sourceIsAbsent: (URL) throws -> Bool = isAbsent) throws -> URL {
        guard try sourceIdentity(url) == expectedIdentity else { throw AppUninstallError.applicationChanged }
        let result = try moveToTrash(url)
        guard let result, result.isFileURL, result.standardizedFileURL != url.standardizedFileURL,
              result.host == nil || result.host == "" || result.host == "localhost",
              result.path.hasPrefix("/"),
              result.pathExtension.lowercased() == "app",
              result.pathComponents.contains(where: { $0 == ".Trash" || $0 == ".Trashes" }),
              try sourceIsAbsent(url) else {
            throw AppUninstallError.system("uninstall.mac.error.trash", 1)
        }
        return result
    }

    static func identity(_ url: URL) throws -> AppUninstallFileIdentity {
        var item = stat()
        guard lstat(url.path, &item) == 0, item.st_mode & S_IFMT == S_IFDIR else {
            throw AppUninstallError.applicationChanged
        }
        return AppUninstallFileIdentity(device: UInt64(item.st_dev), inode: UInt64(item.st_ino))
    }

    static func isAbsent(_ url: URL) throws -> Bool {
        var item = stat()
        if lstat(url.path, &item) == 0 { return false }
        if errno == ENOENT { return true }
        throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
    }
}

private enum AppUninstallNative {
    static func inspectCurrentApplication() throws -> AppUninstallCandidate {
        let bundle = Bundle.main
        let appURL = bundle.bundleURL.standardizedFileURL
        guard bundle.bundleIdentifier == AppUninstallService.bundleIdentifier,
              let executable = bundle.executableURL else { throw AppUninstallError.invalidIdentity }
        // Compare the on-disk application to the running process's designated
        // requirement. This preserves publisher identity without embedding a
        // person's signing identity or Team ID in public source.
        var running: SecCode?, runningStatic: SecStaticCode?, requirement: SecRequirement?, target: SecStaticCode?
        guard SecCodeCopySelf([], &running) == errSecSuccess, let running,
              SecCodeCheckValidity(running, SecCSFlags(rawValue: kSecCSStrictValidate).union(.noNetworkAccess), nil) == errSecSuccess,
              SecCodeCopyStaticCode(running, [], &runningStatic) == errSecSuccess, let runningStatic,
              SecCodeCopyDesignatedRequirement(runningStatic, [], &requirement) == errSecSuccess, let requirement,
              SecStaticCodeCreateWithPath(appURL as CFURL, [], &target) == errSecSuccess, let target else {
            throw AppUninstallError.invalidIdentity
        }
        var information: CFDictionary?
        guard SecCodeCopySigningInformation(runningStatic, SecCSFlags(rawValue: kSecCSSigningInformation), &information) == errSecSuccess,
              let signing = information as? [String: Any],
              let team = signing[kSecCodeInfoTeamIdentifier as String] as? String, !team.isEmpty,
              SecStaticCodeCheckValidity(target, SecCSFlags(rawValue: kSecCSCheckAllArchitectures | kSecCSStrictValidate).union(.noNetworkAccess), requirement) == errSecSuccess else {
            throw AppUninstallError.invalidIdentity
        }
        var item = stat()
        guard lstat(appURL.path, &item) == 0, item.st_mode & S_IFMT == S_IFDIR else { throw AppUninstallError.invalidIdentity }
        let volume = try appURL.resourceValues(forKeys: [.volumeIsReadOnlyKey])
        guard let volumeReadOnly = volume.volumeIsReadOnly else { throw AppUninstallError.readOnlyLocation }
        return AppUninstallCandidate(applicationURL: appURL, resolvedURL: appURL.resolvingSymlinksInPath(),
            executableURL: executable.standardizedFileURL, bundleIdentifier: bundle.bundleIdentifier ?? "",
            identity: AppUninstallFileIdentity(device: UInt64(item.st_dev), inode: UInt64(item.st_ino)),
            volumeReadOnly: volumeReadOnly, parentWritable: FileManager.default.isWritableFile(atPath: appURL.deletingLastPathComponent().path))
    }

    static func deleteCredentials() throws {
        // Full uninstall only. Ordinary account deletion retains the independent backup key.
        for (service, account) in [("local.SZUConnect.credentials", "campus-account"),
                                   ("local.SZUConnect.mac.proxy-backup", "aes-gcm-v1")] {
            let query: [String: Any] = [kSecClass as String: kSecClassGenericPassword,
                kSecAttrService as String: service, kSecAttrAccount as String: account]
            let status = SecItemDelete(query as CFDictionary)
            guard status == errSecSuccess || status == errSecItemNotFound else {
                throw AppUninstallError.system("uninstall.mac.error.credentials", Int(status))
            }
        }
    }
}

/// Descriptor-relative removal never follows a child symlink. The root must be
/// the exact application-owned directory beneath the supplied support parent.
enum AppUninstallDataRemoval {
    static func removeOwnedDirectory(_ url: URL, expectedParent: URL) throws {
        let target = url.standardizedFileURL, parent = expectedParent.standardizedFileURL
        guard target == url, target.lastPathComponent == "SZUConnect",
              target.deletingLastPathComponent() == parent,
              parent.resolvingSymlinksInPath() == parent else { throw AppUninstallError.invalidDataLocation }
        let parentFD = open(parent.path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        if parentFD < 0 {
            if errno == ENOENT { return }
            throw currentError()
        }
        defer { close(parentFD) }
        var info = stat()
        guard fstatat(parentFD, "SZUConnect", &info, AT_SYMLINK_NOFOLLOW) == 0 else {
            if errno == ENOENT { return }; throw currentError()
        }
        guard info.st_mode & S_IFMT == S_IFDIR, info.st_uid == geteuid() else { throw AppUninstallError.invalidDataLocation }
        let directory = openat(parentFD, "SZUConnect", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        guard directory >= 0 else { throw currentError() }
        defer { close(directory) }
        var opened = stat()
        guard fstat(directory, &opened) == 0, opened.st_dev == info.st_dev, opened.st_ino == info.st_ino else {
            throw AppUninstallError.invalidDataLocation
        }
        try removeChildren(directory, device: info.st_dev)
        var final = stat()
        guard fstatat(parentFD, "SZUConnect", &final, AT_SYMLINK_NOFOLLOW) == 0,
              final.st_dev == info.st_dev, final.st_ino == info.st_ino else { throw AppUninstallError.invalidDataLocation }
        guard unlinkat(parentFD, "SZUConnect", AT_REMOVEDIR) == 0 else { throw currentError() }
    }

    private static func removeChildren(_ directory: Int32, device: dev_t) throws {
        let copy = dup(directory)
        guard copy >= 0 else { throw currentError() }
        guard let stream = fdopendir(copy) else { close(copy); throw currentError() }
        defer { closedir(stream) }
        while true {
            errno = 0
            guard let entry = readdir(stream) else { if errno != 0 { throw currentError() }; break }
            let name = withUnsafePointer(to: &entry.pointee.d_name) {
                $0.withMemoryRebound(to: CChar.self, capacity: Int(entry.pointee.d_namlen) + 1) { String(cString: $0) }
            }
            if name == "." || name == ".." { continue }
            var info = stat()
            guard fstatat(directory, name, &info, AT_SYMLINK_NOFOLLOW) == 0 else { throw currentError() }
            // Do not descend into a mount point inside the data directory.
            guard info.st_dev == device else { throw AppUninstallError.invalidDataLocation }
            if info.st_mode & S_IFMT == S_IFDIR {
                let child = openat(directory, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
                guard child >= 0 else { throw currentError() }
                var opened = stat()
                guard fstat(child, &opened) == 0, opened.st_dev == info.st_dev, opened.st_ino == info.st_ino else {
                    close(child); throw AppUninstallError.invalidDataLocation
                }
                do { try removeChildren(child, device: device); close(child) } catch { close(child); throw error }
                var final = stat()
                guard fstatat(directory, name, &final, AT_SYMLINK_NOFOLLOW) == 0,
                      final.st_dev == info.st_dev, final.st_ino == info.st_ino else { throw AppUninstallError.invalidDataLocation }
                guard unlinkat(directory, name, AT_REMOVEDIR) == 0 else { throw currentError() }
            } else {
                guard unlinkat(directory, name, 0) == 0 else { throw currentError() }
            }
        }
    }
    private static func currentError() -> POSIXError { POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
}
#endif
