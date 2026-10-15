#if os(macOS)
import Foundation
import Darwin

enum AppStartupSignals {
    static func configure() {
        // Install once before any model or network worker starts. With curl's
        // CURLOPT_NOSIGNAL, broken connections must return EPIPE, not kill the app.
        signal(SIGPIPE, SIG_IGN)
    }
}

/// An open descriptor owns the lock. The file is never unlinked, including on exit.
final class SingleInstanceLock {
    private let descriptor: Int32
    private init(descriptor: Int32) { self.descriptor = descriptor }
    deinit { close(descriptor) }

    static func acquire(bundleIdentifier: String, cacheDirectory: URL) throws -> SingleInstanceLock? {
        guard !bundleIdentifier.isEmpty, bundleIdentifier.utf8.count <= 200,
              bundleIdentifier.utf8.allSatisfy({ (48...57).contains($0) || (65...90).contains($0)
                  || (97...122).contains($0) || [45, 46, 95].contains($0) }) else {
            throw POSIXError(.EINVAL)
        }
        let directory = open(cacheDirectory.path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
        guard directory >= 0 else { throw lastError() }
        defer { close(directory) }
        var directoryInfo = stat()
        guard fstat(directory, &directoryInfo) == 0 else { throw lastError() }
        guard directoryInfo.st_uid == geteuid(), directoryInfo.st_mode & 0o022 == 0 else {
            throw POSIXError(.EACCES)
        }
        let fd = openat(directory, bundleIdentifier + ".instance.lock",
                        O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0o600)
        guard fd >= 0 else { throw lastError() }
        var retained = false
        defer { if !retained { close(fd) } }
        var info = stat()
        guard fstat(fd, &info) == 0 else { throw lastError() }
        guard info.st_mode & S_IFMT == S_IFREG, info.st_uid == geteuid(), info.st_nlink == 1 else {
            throw POSIXError(.EACCES)
        }
        guard fchmod(fd, 0o600) == 0 else { throw lastError() }
        guard flock(fd, LOCK_EX | LOCK_NB) == 0 else {
            if errno == EWOULDBLOCK { return nil }
            throw lastError()
        }
        retained = true
        return SingleInstanceLock(descriptor: fd)
    }

    private static func lastError() -> POSIXError { POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
}
#endif
