import Foundation
import Darwin

@main struct SingleInstanceTests {
    static func main() throws {
        let args = CommandLine.arguments
        if args.contains("--broken-pipe") {
            alarm(15)
            signal(SIGPIPE, SIG_DFL)
            if !args.contains("--unguarded") { AppStartupSignals.configure() }
            var descriptors: [Int32] = [0, 0]
            guard pipe(&descriptors) == 0 else { exit(5) }
            close(descriptors[0])
            let bytes: [UInt8] = [1]
            let written = bytes.withUnsafeBytes { Darwin.write(descriptors[1], $0.baseAddress, $0.count) }
            let writeError = errno
            close(descriptors[1])
            exit(written == -1 && writeError == EPIPE ? 0 : 6)
        }
        if args.count > 1, args[1] == "--after-exec" {
            alarm(15)
            FileHandle.standardOutput.write(Data("ready\n".utf8))
            _ = FileHandle.standardInput.readData(ofLength: 1)
            exit(0)
        }
        if args.count > 1, args[1] == "--child" {
            alarm(15)
            do {
                guard let lock = try SingleInstanceLock.acquire(bundleIdentifier: args[3],
                        cacheDirectory: URL(fileURLWithPath: args[2])) else { exit(2) }
                if args.contains("--exec-held") {
                    var replacement = [strdup(args[0]), strdup("--after-exec"), nil]
                    withExtendedLifetime(lock) { _ = execv(args[0], &replacement) }
                    exit(4)
                }
                if args.contains("--hold") {
                    FileHandle.standardOutput.write(Data("ready\n".utf8))
                    withExtendedLifetime(lock) { _ = FileHandle.standardInput.readData(ofLength: 1) }
                } else { withExtendedLifetime(lock) {} }
                exit(0)
            } catch { exit(3) }
        }
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent("szu-instance-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true,
                                               attributes: [.posixPermissions: 0o700])
        defer { try? FileManager.default.removeItem(at: directory) }
        var checks = 0
        var failures: [String] = []
        func expect(_ value: @autoclosure () throws -> Bool, _ name: String) {
            checks += 1
            if (try? value()) != true { failures.append(name) }
        }
        for guarded in [false, true] {
            let process = Process()
            process.executableURL = URL(fileURLWithPath: args[0])
            process.arguments = ["--broken-pipe"] + (guarded ? [] : ["--unguarded"])
            process.standardOutput = FileHandle.nullDevice
            process.standardError = FileHandle.nullDevice
            try process.run()
            process.waitUntilExit()
            expect(guarded ? process.terminationReason == .exit && process.terminationStatus == 0
                : process.terminationReason == .uncaughtSignal && process.terminationStatus == SIGPIPE,
                guarded ? "startup guard preserves EPIPE without terminating the app"
                    : "default SIGPIPE disposition reproduces the process exit")
        }
        func child(_ key: String, _ options: [String] = [], at cache: URL? = nil) throws -> (Process, Pipe, Pipe) {
            let process = Process(), input = Pipe(), output = Pipe()
            process.executableURL = URL(fileURLWithPath: args[0])
            process.arguments = ["--child", (cache ?? directory).path, key] + options
            process.standardInput = input
            process.standardOutput = output
            process.standardError = FileHandle.nullDevice
            try process.run()
            input.fileHandleForReading.closeFile()
            output.fileHandleForWriting.closeFile()
            return (process, input, output)
        }
        func result(_ key: String, _ options: [String] = [], at cache: URL? = nil) throws -> Int32 {
            let (process, _, _) = try child(key, options, at: cache)
            process.waitUntilExit()
            return process.terminationStatus
        }
        func owner(_ key: String, _ options: [String] = []) throws -> (Process, Pipe) {
            let (process, input, output) = try child(key, options + ["--hold"])
            expect(output.fileHandleForReading.readData(ofLength: 6) == Data("ready\n".utf8), "owner acquires lock")
            return (process, input)
        }
        let key = "local.test.formal"
        let (first, firstInput) = try owner(key)
        expect(try result(key) == 2, "second process cannot acquire same bundle ID")
        expect(try result(key, ["--observe-only"]) == 2, "observe-only cannot bypass the formal lock")
        expect(try result("local.test.preview") == 0, "independent preview bundle ID stays isolated")
        let lockFile = directory.appendingPathComponent(key + ".instance.lock")
        let attrs = try FileManager.default.attributesOfItem(atPath: lockFile.path)
        expect((attrs[.posixPermissions] as? NSNumber)?.intValue == 0o600, "lock file is private 0600")
        firstInput.fileHandleForWriting.write(Data([1]))
        first.waitUntilExit()
        expect(first.terminationStatus == 0, "owner exits normally")
        expect(FileManager.default.fileExists(atPath: lockFile.path), "normal exit does not unlink lock")
        expect(try result(key) == 0, "lock reacquired after owner exit without removing file")
        let (observer, observerInput) = try owner(key, ["--observe-only"])
        expect(try result(key) == 2, "an observe-only owner also excludes normal launch")
        observer.terminate()
        observer.waitUntilExit()
        withExtendedLifetime(observerInput) {}
        expect(try result(key) == 0, "terminated owner leaves no stale lock")

        let (replacement, replacementInput) = try owner(key, ["--exec-held"])
        expect(try result(key) == 0, "CLOEXEC releases lock when owner replaces its executable")
        replacementInput.fileHandleForWriting.write(Data([1]))
        replacement.waitUntilExit()
        expect(replacement.terminationStatus == 0, "replacement child exits cleanly")

        let target = directory.appendingPathComponent("unchanged.txt")
        try Data("preserve".utf8).write(to: target)
        let symlink = directory.appendingPathComponent("local.test.symlink.instance.lock")
        try FileManager.default.createSymbolicLink(at: symlink, withDestinationURL: target)
        expect(try result("local.test.symlink") == 3, "symlink lock is rejected")
        expect(try String(contentsOf: target, encoding: .utf8) == "preserve", "symlink target is untouched")
        let linked = directory.appendingPathComponent("local.test.hardlink.instance.lock")
        try FileManager.default.linkItem(at: target, to: linked)
        expect(try result("local.test.hardlink") == 3, "multiply linked file is rejected")
        let linkedDirectory = directory.appendingPathComponent("cache-link")
        try FileManager.default.createSymbolicLink(at: linkedDirectory, withDestinationURL: directory)
        expect(try result(key, at: linkedDirectory) == 3, "symlink cache directory is rejected")
        expect(try result("../../escape") == 3, "unsafe bundle identifier cannot escape cache")
        let fifo = directory.appendingPathComponent("local.test.fifo.instance.lock")
        expect(mkfifo(fifo.path, 0o600) == 0, "temporary FIFO fixture created")
        expect(try result("local.test.fifo") == 3, "nonregular file rejected without blocking")
        let unsafeDirectory = directory.appendingPathComponent("unsafe-cache")
        try FileManager.default.createDirectory(at: unsafeDirectory, withIntermediateDirectories: false)
        expect(chmod(unsafeDirectory.path, 0o777) == 0, "temporary unsafe cache fixture created")
        expect(try result(key, at: unsafeDirectory) == 3, "shared writable cache rejected")

        var localOwner: SingleInstanceLock? = try SingleInstanceLock.acquire(bundleIdentifier: key, cacheDirectory: directory)
        expect(localOwner != nil, "current process acquires after child cleanup")
        expect(try result(key) == 2, "current process descriptor excludes another process")
        localOwner = nil
        expect(try result(key) == 0, "deinit releases descriptor")
        if failures.isEmpty { print("PASS SingleInstanceTests: \(checks) checks") }
        else { failures.forEach { print("FAIL: \($0)") }; exit(1) }
    }
}
