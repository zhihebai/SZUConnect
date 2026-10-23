#if os(macOS)
import Foundation
import CryptoKit
import Security
import Darwin

protocol ProxyAssistantKeyVault {
    func load() throws -> Data?
    func create(_ key: Data) throws
    func remove() throws
}
struct ProxyAssistantKeychainVault: ProxyAssistantKeyVault {
    private var query: [String: Any] { [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: "local.SZUConnect.mac.proxy-backup", kSecAttrAccount as String: "aes-gcm-v1"] }
    func load() throws -> Data? {
        var values = query; values[kSecReturnData as String] = true; values[kSecMatchLimit as String] = kSecMatchLimitOne
        var item: CFTypeRef?
        let status = SecItemCopyMatching(values as CFDictionary, &item)
        if status == errSecItemNotFound { return nil }
        guard status == errSecSuccess, let data = item as? Data, data.count == 32 else { throw ProxyAssistantFailure("backupCorrupt") }
        return data
    }
    func create(_ key: Data) throws {
        var values = query; values[kSecValueData as String] = key
        values[kSecAttrAccessible as String] = kSecAttrAccessibleWhenUnlockedThisDeviceOnly
        guard SecItemAdd(values as CFDictionary, nil) == errSecSuccess else { throw ProxyAssistantFailure("backupFailed") }
    }
    func remove() throws {
        let status = SecItemDelete(query as CFDictionary)
        guard status == errSecSuccess || status == errSecItemNotFound else { throw ProxyAssistantFailure("cleanupPending") }
    }
}

struct ProxyAssistantFileIdentity: Codable, Equatable {
    let device: Int32
    let inode: UInt64
    let owner: UInt32
    let mode: UInt16
    init(_ value: stat) { device = value.st_dev; inode = value.st_ino; owner = value.st_uid; mode = value.st_mode }
}
struct ProxyAssistantPreview {
    let url: URL
    let identity: ProxyAssistantFileIdentity
    let original: Data
    let patch: ProxyAssistantPatch
}

// All file mutations are coordinated, local, bounded, no-follow, and relative to a verified directory fd.
// Ciphertext contains the bookmark and original bytes. No node, path, subscription or config is logged.
final class ProxyAssistantFiles {
    private struct Backup: Codable {
        let version: Int
        let bookmark: Data
        let path: String
        let identity: ProxyAssistantFileIdentity
        let original: Data
        let savedHash: Data
    }
    private let directory: URL
    private let vault: any ProxyAssistantKeyVault
    private let readOnly: Bool
    private var backupURL: URL { directory.appendingPathComponent("proxy-direct-v1.aesgcm") }
    private var cleanupURL: URL { directory.appendingPathComponent("proxy-direct-v1.aesgcm.cleanup") }
    var hasBackup: Bool { FileManager.default.fileExists(atPath: backupURL.path) || FileManager.default.fileExists(atPath: cleanupURL.path) }
    init(directory: URL, vault: any ProxyAssistantKeyVault = ProxyAssistantKeychainVault(), readOnly: Bool = false) {
        self.directory = directory; self.vault = vault; self.readOnly = readOnly
    }
    static func defaultDirectory() -> URL {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0].appendingPathComponent("SZUConnect/ProxyAssistant", isDirectory: true)
    }
    func preview(_ url: URL, format: ProxyAssistantFormat) throws -> ProxyAssistantPreview {
        guard !readOnly else { throw ProxyAssistantFailure("unavailable") }
        return try coordinate(url, writing: false) { coordinated in
            let (bytes, identity) = try Self.read(coordinated)
            return ProxyAssistantPreview(url: coordinated, identity: identity, original: bytes, patch: try ProxyAssistantConfig.patch(bytes, format: format))
        }
    }
    func save(_ preview: ProxyAssistantPreview) throws {
        guard !readOnly else { throw ProxyAssistantFailure("unavailable") }
        guard !preview.patch.changes.isEmpty else { return }
        guard !hasBackup else { throw ProxyAssistantFailure("backupExists") }
        let started = preview.url.startAccessingSecurityScopedResource(); defer { if started { preview.url.stopAccessingSecurityScopedResource() } }
        try coordinate(preview.url, writing: true) { url in
            let (current, identity) = try Self.read(url)
            guard identity == preview.identity, current == preview.original else { throw ProxyAssistantFailure("changed") }
            let bookmark = try url.bookmarkData(options: [.withSecurityScope], includingResourceValuesForKeys: nil, relativeTo: nil)
            let payload = Backup(version: 1, bookmark: bookmark, path: url.path, identity: identity, original: current, savedHash: Data(SHA256.hash(data: preview.patch.bytes)))
            try writeBackup(payload)
            // Backup is durable before any target mutation. Failures retain it for recovery/retry.
            try Self.replace(url, expectedIdentity: identity, expectedBytes: current, bytes: preview.patch.bytes)
        }
    }
    func restore() throws {
        guard !readOnly else { throw ProxyAssistantFailure("unavailable") }
        guard !FileManager.default.fileExists(atPath: cleanupURL.path) else { throw ProxyAssistantFailure("keyCleanup") }
        let payload = try readBackup()
        var stale = false
        let url: URL
        do { url = try URL(resolvingBookmarkData: payload.bookmark, options: [.withSecurityScope, .withoutUI], relativeTo: nil, bookmarkDataIsStale: &stale) }
        catch { throw ProxyAssistantFailure("backupCorrupt") }
        // Our own atomic save replaces the inode, which legitimately makes the pre-save
        // bookmark stale. Never follow a moved bookmark to a different file; the original
        // location is separately opened no-follow and verified against the saved bytes.
        guard url.standardizedFileURL.path == URL(fileURLWithPath: payload.path).standardizedFileURL.path else {
            throw ProxyAssistantFailure("restoreConflict")
        }
        let started = url.startAccessingSecurityScopedResource(); defer { if started { url.stopAccessingSecurityScopedResource() } }
        let originalLocation = URL(fileURLWithPath: payload.path)
        try coordinate(originalLocation, writing: true) { coordinated in
            let (current, identity) = try Self.read(coordinated)
            // Atomic replacement changes inode; the post-save inode is not authoritative. The exact path,
            // owner/mode and our saved content hash must still match. A later edit is never overwritten.
            guard identity.device == payload.identity.device, identity.owner == payload.identity.owner,
                  identity.mode == payload.identity.mode else { throw ProxyAssistantFailure("restoreConflict") }
            if current == payload.original { return } // A failed save or previous successful restore: cleanup only.
            guard Data(SHA256.hash(data: current)) == payload.savedHash else { throw ProxyAssistantFailure("restoreConflict") }
            try Self.replace(coordinated, expectedIdentity: identity, expectedBytes: current, bytes: payload.original)
        }
        try discardBackup()
    }
    // Call only after a separate explicit confirmation. Never invoked by uninstall or deinit.
    func discardBackup() throws {
        guard !readOnly else { throw ProxyAssistantFailure("unavailable") }
        if FileManager.default.fileExists(atPath: cleanupURL.path) {
            guard !FileManager.default.fileExists(atPath: backupURL.path) else { throw ProxyAssistantFailure("cleanupPending") }
            try finishDiscard(); return
        }
        guard hasBackup else { throw ProxyAssistantFailure("backupMissing") }
        _ = try readBackup() // Do not silently remove an unrecognized or corrupted backup.
        let (_, identity) = try Self.read(backupURL, maximum: 2 * ProxyAssistantConfig.maximumBytes)
        let dir = try Self.openParent(backupURL); defer { close(dir) }
        var now = stat()
        guard fstatat(dir, backupURL.lastPathComponent, &now, AT_SYMLINK_NOFOLLOW) == 0, ProxyAssistantFileIdentity(now) == identity,
              renameatx_np(dir, backupURL.lastPathComponent, dir, cleanupURL.lastPathComponent, UInt32(RENAME_EXCL)) == 0,
              fsync(dir) == 0 else { throw ProxyAssistantFailure("cleanupPending") }
        // A dedicated pending name makes either cleanup failure retryable, including a
        // restart after the key was removed but before ciphertext could be unlinked.
        try finishDiscard()
    }
    private func finishDiscard() throws {
        let (_, identity) = try Self.read(cleanupURL, maximum: 2 * ProxyAssistantConfig.maximumBytes)
        do { try vault.remove() } catch { throw ProxyAssistantFailure("keyCleanup") }
        let dir = try Self.openParent(cleanupURL); defer { close(dir) }
        var now = stat()
        guard fstatat(dir, cleanupURL.lastPathComponent, &now, AT_SYMLINK_NOFOLLOW) == 0, ProxyAssistantFileIdentity(now) == identity,
              unlinkat(dir, cleanupURL.lastPathComponent, 0) == 0, fsync(dir) == 0 else { throw ProxyAssistantFailure("cleanupPending") }
    }
    private func writeBackup(_ payload: Backup) throws {
        do {
            let parent = try Self.openDirectory(directory, creating: true); defer { close(parent) }
            var info = stat(); guard fstat(parent, &info) == 0, info.st_uid == geteuid(), (info.st_mode & 0o077) == 0 else { throw ProxyAssistantFailure("backupFailed") }
            // An orphaned key is retained safely after an interrupted creation and can be reused.
            let keyData: Data
            if let existing = try vault.load() { keyData = existing } else {
                let key = SymmetricKey(size: .bits256); keyData = key.withUnsafeBytes { Data($0) }; try vault.create(keyData)
            }
            guard keyData.count == 32 else { throw ProxyAssistantFailure("backupCorrupt") }
            let sealed = try AES.GCM.seal(JSONEncoder().encode(payload), using: SymmetricKey(data: keyData), authenticating: Data("SZUConnect proxy backup v1".utf8))
            guard let bytes = sealed.combined, bytes.count <= 2 * ProxyAssistantConfig.maximumBytes else { throw ProxyAssistantFailure("backupFailed") }
            let fd = openat(parent, backupURL.lastPathComponent, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0o600)
            guard fd >= 0 else { throw ProxyAssistantFailure("backupFailed") }
            defer { close(fd) }
            do { try Self.write(bytes, fd: fd); guard fsync(fd) == 0, fsync(parent) == 0 else { throw ProxyAssistantFailure("backupFailed") } }
            catch { _ = unlinkat(parent, backupURL.lastPathComponent, 0); throw error }
        } catch let error as ProxyAssistantFailure { throw error }
        catch { throw ProxyAssistantFailure("backupFailed") }
    }
    private func readBackup() throws -> Backup {
        guard hasBackup else { throw ProxyAssistantFailure("backupMissing") }
        do {
            let (data, _) = try Self.read(backupURL, maximum: 2 * ProxyAssistantConfig.maximumBytes)
            guard let key = try vault.load() else { throw ProxyAssistantFailure("backupCorrupt") }
            let box = try AES.GCM.SealedBox(combined: data)
            let bytes = try AES.GCM.open(box, using: SymmetricKey(data: key), authenticating: Data("SZUConnect proxy backup v1".utf8))
            let payload = try JSONDecoder().decode(Backup.self, from: bytes)
            guard payload.version == 1, payload.original.count <= ProxyAssistantConfig.maximumBytes, payload.savedHash.count == 32 else { throw ProxyAssistantFailure("backupCorrupt") }
            return payload
        } catch { throw ProxyAssistantFailure("backupCorrupt") }
    }
    private func coordinate<T>(_ url: URL, writing: Bool, _ action: (URL) throws -> T) throws -> T {
        // Reject remote volumes and linked ancestors before asking a coordinator to
        // contact any file presenter. Mutation repeats these checks under coordination.
        let preflight = try Self.openParent(url); close(preflight)
        var error: NSError?, result: Result<T, Error>?
        let coordinator = NSFileCoordinator(filePresenter: nil)
        if writing { coordinator.coordinate(writingItemAt: url, options: .forReplacing, error: &error) { coordinated in result = Result { try action(coordinated) } } }
        else { coordinator.coordinate(readingItemAt: url, options: .withoutChanges, error: &error) { coordinated in result = Result { try action(coordinated) } } }
        guard error == nil, let result else { throw ProxyAssistantFailure("unavailable") }
        return try result.get()
    }
    private static func openParent(_ url: URL) throws -> Int32 {
        try openDirectory(url.deletingLastPathComponent(), creating: false)
    }
    private static func openDirectory(_ url: URL, creating: Bool) throws -> Int32 {
        guard url.isFileURL, url.host == nil || url.host == "", url.path.hasPrefix("/"), !url.pathComponents.contains(".."), !url.pathComponents.contains("."),
              !url.lastPathComponent.isEmpty, !url.path.unicodeScalars.contains(where: { $0.value < 32 || $0.value == 127 }) else { throw ProxyAssistantFailure("invalidPath") }
        var descriptor = open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW)
        guard descriptor >= 0 else { throw ProxyAssistantFailure("invalidPath") }
        for component in url.pathComponents.dropFirst() {
            var next = openat(descriptor, component, O_RDONLY | O_DIRECTORY | O_NOFOLLOW)
            if next < 0, errno == ENOENT, creating {
                guard mkdirat(descriptor, component, 0o700) == 0, fsync(descriptor) == 0 else {
                    close(descriptor); throw ProxyAssistantFailure("backupFailed")
                }
                next = openat(descriptor, component, O_RDONLY | O_DIRECTORY | O_NOFOLLOW)
            }
            close(descriptor); guard next >= 0 else { throw ProxyAssistantFailure("invalidPath") }; descriptor = next
        }
        var fs = statfs()
        guard fstatfs(descriptor, &fs) == 0, (fs.f_flags & UInt32(MNT_LOCAL)) != 0 else { close(descriptor); throw ProxyAssistantFailure("invalidPath") }
        return descriptor
    }
    private static func read(_ url: URL, maximum: Int = ProxyAssistantConfig.maximumBytes) throws -> (Data, ProxyAssistantFileIdentity) {
        let parent = try openParent(url); defer { close(parent) }
        let fd = openat(parent, url.lastPathComponent, O_RDONLY | O_NOFOLLOW | O_NONBLOCK); guard fd >= 0 else { throw ProxyAssistantFailure("readFailed") }; defer { close(fd) }
        return try read(fd, maximum: maximum)
    }
    private static func read(_ fd: Int32, maximum: Int) throws -> (Data, ProxyAssistantFileIdentity) {
        var before = stat(), after = stat()
        guard fstat(fd, &before) == 0, (before.st_mode & S_IFMT) == S_IFREG, before.st_nlink == 1, before.st_uid == geteuid(), (before.st_mode & 0o222) != 0, (before.st_mode & 0o7000) == 0 else { throw ProxyAssistantFailure("unsupportedFile") }
        guard before.st_size >= 0, before.st_size <= maximum else { throw ProxyAssistantFailure("tooLarge") }
        var bytes = Data(), buffer = [UInt8](repeating: 0, count: 8192)
        while true {
            let count = Darwin.read(fd, &buffer, buffer.count)
            if count < 0 { if errno == EINTR { continue }; throw ProxyAssistantFailure("readFailed") }
            if count == 0 { break }; bytes.append(contentsOf: buffer[0..<count])
            guard bytes.count <= maximum else { throw ProxyAssistantFailure("tooLarge") }
        }
        guard fstat(fd, &after) == 0, ProxyAssistantFileIdentity(before) == ProxyAssistantFileIdentity(after), before.st_size == after.st_size,
              before.st_nlink == after.st_nlink, before.st_mtimespec.tv_sec == after.st_mtimespec.tv_sec, before.st_mtimespec.tv_nsec == after.st_mtimespec.tv_nsec,
              before.st_ctimespec.tv_sec == after.st_ctimespec.tv_sec, before.st_ctimespec.tv_nsec == after.st_ctimespec.tv_nsec else { throw ProxyAssistantFailure("changed") }
        return (bytes, ProxyAssistantFileIdentity(after))
    }
    private static func replace(_ url: URL, expectedIdentity: ProxyAssistantFileIdentity, expectedBytes: Data, bytes: Data) throws {
        let parent = try openParent(url); defer { close(parent) }
        let name = ".szuconnect-" + UUID().uuidString
        let temporary = openat(parent, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0o600)
        guard temporary >= 0 else { throw ProxyAssistantFailure("writeFailed") }
        defer { close(temporary); _ = unlinkat(parent, name, 0) }
        try write(bytes, fd: temporary)
        guard fchmod(temporary, mode_t(expectedIdentity.mode & 0o777)) == 0, fsync(temporary) == 0 else { throw ProxyAssistantFailure("writeFailed") }
        let original = openat(parent, url.lastPathComponent, O_RDONLY | O_NOFOLLOW | O_NONBLOCK)
        guard original >= 0 else { throw ProxyAssistantFailure("changed") }; defer { close(original) }
        let (current, identity) = try read(original, maximum: ProxyAssistantConfig.maximumBytes)
        var entry = stat()
        guard identity == expectedIdentity, current == expectedBytes,
              fstatat(parent, url.lastPathComponent, &entry, AT_SYMLINK_NOFOLLOW) == 0, ProxyAssistantFileIdentity(entry) == identity else { throw ProxyAssistantFailure("changed") }
        // NSFileCoordinator excludes cooperating editors; final inode/content check excludes stale previews.
        guard renameat(parent, name, parent, url.lastPathComponent) == 0, fsync(parent) == 0 else { throw ProxyAssistantFailure("writeFailed") }
    }
    private static func write(_ data: Data, fd: Int32) throws {
        try data.withUnsafeBytes { raw in
            var offset = 0
            while offset < raw.count {
                let count = Darwin.write(fd, raw.baseAddress!.advanced(by: offset), raw.count - offset)
                if count < 0, errno == EINTR { continue }
                guard count > 0 else { throw ProxyAssistantFailure("writeFailed") }; offset += count
            }
        }
    }
}
#endif
