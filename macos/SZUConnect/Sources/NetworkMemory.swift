import Foundation
import Darwin
import CryptoKit

// Network memory records revocable permission to recover a selected campus network.
//
// Stored: keyed hashes (HMAC-SHA256 with a per-install random salt, first 128 bits) of stable network
// features. This store never contains SSID, client IP, raw feature values or credentials.
// Local display names/SSID live separately in NetworkLabels and never establish login authority.
// A remembered network is only an eligibility: every recovery still needs a fresh proof of the campus service
// (memoryVerdict) and never overrides holds, cooldown or the one-submission rule.
// Mirrors windows/SZUConnect/core/src/netmemory.cpp and tools/reference/memory_reference.py; verified against
// shared/fixtures/network-memory-cases.json (Tests/NetworkMemoryTests.swift).

struct MemoryRecord: Equatable, Identifiable, Sendable {
    var id: String
    var created: TimeInterval
    var verified: TimeInterval
    var area: String                    // dormitory | teaching | "" (area confirmed when remembered)
    var proofTeaching: Bool
    var proofDormitory: Bool
    var features: [String: String]      // label -> 32 lower-case hex characters
}

struct NetworkMemory: Sendable {
    static let limit = 8
    static let expiry: TimeInterval = 180 * 86_400
    static let touchInterval: TimeInterval = 3_600
    static let labels = ["gw", "gwip", "net", "dhcp", "dns", "sfx"]

    private(set) var salt: String
    private(set) var records: [MemoryRecord] = []

    struct Match: Equatable, Sendable {
        var level: MatchLevel = .none
        var record = ""
        var area = ""
    }

    init(salt: String) { self.salt = salt }
    var hasSalt: Bool { salt.count >= 16 }
    var count: Int { records.count }

    static func randomHex(bytes: Int = 16) -> String {
        var generator = SystemRandomNumberGenerator()
        return (0..<bytes).map { _ in String(format: "%02x", UInt8.random(in: 0...255, using: &generator)) }.joined()
    }

    // MARK: hashing and comparison

    func hashed(_ descriptor: NetworkDescriptor) -> [String: String] {
        var out: [String: String] = [:]
        let key = SymmetricKey(data: Data(salt.utf8))
        for label in Self.labels {
            guard let value = descriptor.raw[label], !value.isEmpty else { continue }
            let code = HMAC<SHA256>.authenticationCode(for: Data((label + "\u{1F}" + value).utf8), using: key)
            out[label] = code.prefix(16).map { String(format: "%02x", $0) }.joined()
        }
        return out
    }

    /// Same address or subnet but different gateway hardware is a conflict (another network borrowing it);
    /// a different gateway with nothing else in common is simply another network.
    static func compare(stored s: [String: String], current c: [String: String]) -> MatchLevel {
        func both(_ key: String) -> Bool { s[key] != nil && c[key] != nil }
        func eq(_ key: String) -> Bool { both(key) && s[key] == c[key] }
        func ne(_ key: String) -> Bool { both(key) && s[key] != c[key] }
        if ne("gw") { return (eq("gwip") || eq("net")) ? .conflict : .none }
        if eq("gw") { return (eq("net") || eq("gwip") || eq("dhcp")) ? .strong : .medium }
        if eq("gwip") && eq("net") { return ne("dhcp") ? .weak : .medium }
        if eq("gwip") || eq("net") { return .weak }
        return .none
    }

    private static func rank(_ level: MatchLevel) -> Int {
        switch level { case .strong: 3; case .medium: 2; case .weak: 1; default: 0 }
    }

    func match(_ descriptor: NetworkDescriptor, now: Date, exclude: Set<String> = [], requireDormitoryProof: Bool = false) -> Match {
        guard hasSalt, !descriptor.raw.isEmpty else { return Match() }
        let current = hashed(descriptor)
        var best = Match()
        var conflict = false
        for record in records {
            if exclude.contains(record.id) || record.verified + Self.expiry < now.timeIntervalSince1970 { continue }
            if requireDormitoryProof && !(record.proofTeaching && record.proofDormitory) { continue }
            let level = Self.compare(stored: record.features, current: current)
            if level == .conflict { conflict = true; continue }
            if Self.rank(level) > Self.rank(best.level) { best = Match(level: level, record: record.id, area: record.area) }
        }
        if Self.rank(best.level) < 2 && conflict { return Match(level: .conflict) }
        return best
    }

    func matchAny(_ descriptors: [NetworkDescriptor], now: Date, exclude: Set<String> = [], requireDormitoryProof: Bool = false) -> Match {
        var best = Match()
        var conflict = false
        for descriptor in descriptors {
            let found = match(descriptor, now: now, exclude: exclude, requireDormitoryProof: requireDormitoryProof)
            if found.level == .conflict { conflict = true; continue }
            if Self.rank(found.level) > Self.rank(best.level) { best = found }
        }
        if Self.rank(best.level) < 2 && conflict { return Match(level: .conflict) }
        return best
    }

    /// Records that represent the current network (medium or strong).
    func matchingIDs(_ descriptors: [NetworkDescriptor], now: Date) -> [String] {
        var ids: [String] = []
        guard hasSalt else { return ids }
        for descriptor in descriptors where !descriptor.raw.isEmpty {
            let current = hashed(descriptor)
            for record in records where record.verified + Self.expiry >= now.timeIntervalSince1970 {
                let level = Self.compare(stored: record.features, current: current)
                if (level == .medium || level == .strong) && !ids.contains(record.id) { ids.append(record.id) }
            }
        }
        return ids
    }

    /// Exact duplicates or the same known gateway. A missing gateway never bridges two records.
    func equivalentIDs(id: String) -> [String] {
        guard let selected = records.first(where: { $0.id == id }) else { return [] }
        return records.compactMap { record in
            if record.id == id || record.features == selected.features { return record.id }
            if let gateway = selected.features["gw"], record.features["gw"] == gateway {
                let level = Self.compare(stored: selected.features, current: record.features)
                if level == .medium || level == .strong { return record.id }
            }
            return nil
        }
    }

    func mergeCandidates(_ descriptor: NetworkDescriptor, now: Date) -> [String] {
        guard hasSalt, descriptor.isReliable else { return [] }
        let current = hashed(descriptor)
        let candidates = records.filter { record in
            guard record.verified + Self.expiry >= now.timeIntervalSince1970 else { return false }
            let level = Self.compare(stored: record.features, current: current)
            return level == .medium || level == .strong
        }
        guard candidates.count > 1, Set(candidates.compactMap { $0.features["gw"] }).count <= 1 else { return [] }
        return candidates.map(\.id)
    }

    // MARK: mutation

    private static func isHex(_ text: String, min: Int, max: Int) -> Bool {
        text.count >= min && text.count <= max && text.allSatisfy { ("0"..."9").contains($0) || ("a"..."f").contains($0) }
    }

    private mutating func prune(now: Date) {
        records.removeAll { $0.verified + Self.expiry < now.timeIntervalSince1970 }
        while records.count > Self.limit, let oldest = records.enumerated().min(by: { $0.element.verified < $1.element.verified })?.offset {
            records.remove(at: oldest)
        }
    }

    /// Adds or refreshes the record of a reliable descriptor; nil when it is not reliable.
    mutating func remember(_ descriptor: NetworkDescriptor, area: String, proofTeaching: Bool, proofDormitory: Bool,
                           now: Date, newID: String) -> String? {
        guard hasSalt, descriptor.isReliable else { return nil }
        prune(now: now)
        let current = hashed(descriptor)
        let candidates = records.indices.filter {
            let level = Self.compare(stored: records[$0].features, current: current)
            return level == .medium || level == .strong
        }
        if let index = candidates.reduce(nil as Int?, { best, candidate in
            guard let best else { return candidate }
            let candidateRank = Self.rank(Self.compare(stored: records[candidate].features, current: current))
            let bestRank = Self.rank(Self.compare(stored: records[best].features, current: current))
            if candidateRank != bestRank { return candidateRank > bestRank ? candidate : best }
            return records[best].features["gw"] == nil && records[candidate].features["gw"] != nil ? candidate : best
        }) {
            let id = records[index].id
            let gateways = Set(candidates.compactMap { records[$0].features["gw"] })
            var refreshed = current
            for label in ["gw", "gwip", "net"] where refreshed[label] == nil {
                refreshed[label] = records[index].features[label]
            }
            records[index].features = refreshed
            records[index].verified = now.timeIntervalSince1970
            if !area.isEmpty { records[index].area = area }
            records[index].proofTeaching = proofTeaching
            records[index].proofDormitory = proofDormitory
            if gateways.count <= 1 {
                let redundant = Set(candidates.filter { $0 != index }.map { records[$0].id })
                records.removeAll { redundant.contains($0.id) }
            }
            return id
        }
        guard Self.isHex(newID, min: 8, max: 32) else { return nil }
        records.append(MemoryRecord(id: newID, created: now.timeIntervalSince1970, verified: now.timeIntervalSince1970, area: area,
                                    proofTeaching: proofTeaching, proofDormitory: proofDormitory, features: current))
        prune(now: now)
        return newID
    }

    /// Throttled refresh of the sliding expiry; true when something changed.
    mutating func touch(id: String, now: Date, area: String) -> Bool {
        guard let index = records.firstIndex(where: { $0.id == id }) else { return false }
        let time = now.timeIntervalSince1970
        if time - records[index].verified < Self.touchInterval && (area.isEmpty || records[index].area == area) { return false }
        records[index].verified = max(records[index].verified, time)
        if !area.isEmpty { records[index].area = area }
        return true
    }

    @discardableResult
    mutating func forget(id: String) -> Bool {
        let before = records.count
        records.removeAll { $0.id == id }
        return records.count != before
    }

    mutating func forgetAll() { records.removeAll() }

    // MARK: persistence (schema 1)

    private struct StoredRecord: Codable {
        var id: String
        var created: Double
        var verified: Double
        var area: String?
        var proofTeaching: Bool
        var proofDormitory: Bool
        var features: [String: String]
    }
    private struct Stored: Codable {
        var schema: Int
        var salt: String
        var records: [StoredRecord]
    }

    func encoded() -> Data {
        let stored = Stored(schema: 1, salt: salt, records: records.map {
            StoredRecord(id: $0.id, created: $0.created.rounded(.down), verified: $0.verified.rounded(.down),
                         area: $0.area.isEmpty ? nil : $0.area, proofTeaching: $0.proofTeaching,
                         proofDormitory: $0.proofDormitory, features: $0.features)
        })
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys]
        return (try? encoder.encode(stored)) ?? Data()
    }

    /// nil: corrupt or unknown schema. The caller keeps the file and runs without memory.
    static func decode(_ data: Data, now: Date) -> NetworkMemory? {
        guard data.count <= 65_536, let stored = try? JSONDecoder().decode(Stored.self, from: data), stored.schema == 1,
              isHex(stored.salt, min: 16, max: 64), stored.records.count <= limit else { return nil }
        var memory = NetworkMemory(salt: stored.salt)
        for item in stored.records {
            guard isHex(item.id, min: 8, max: 32), item.created.isFinite, item.verified.isFinite, item.created >= 0, item.verified >= 0,
                  !memory.records.contains(where: { $0.id == item.id }) else { return nil }
            if let area = item.area, area != "dormitory", area != "teaching" { return nil }
            for label in labels {
                if let value = item.features[label], !isHex(value, min: 32, max: 32) { return nil }
            }
            // A clock set into the future cannot extend trust.
            let verified = min(item.verified, now.timeIntervalSince1970 + 3_600)
            memory.records.append(MemoryRecord(id: item.id, created: item.created, verified: verified, area: item.area ?? "",
                                               proofTeaching: item.proofTeaching, proofDormitory: item.proofDormitory,
                                               features: item.features.filter { labels.contains($0.key) }))
        }
        memory.prune(now: now)
        return memory
    }
}

/// Atomic, owner-only file access; an unreadable file is never rewritten by the caller.
enum NetworkMemoryFile {
    static func read(_ url: URL) throws -> Data? {
        guard FileManager.default.fileExists(atPath: url.path) else { return nil }
        let handle = try FileHandle(forReadingFrom: url)
        defer { try? handle.close() }
        let data = try handle.read(upToCount: 65_537) ?? Data()
        guard data.count <= 65_536 else { throw CocoaError(.fileReadTooLarge) }
        return data
    }
    static func write(_ data: Data, to url: URL) throws {
        guard !data.isEmpty, data.count <= 65_536 else { throw CocoaError(.fileWriteUnknown) }
        let staged = url.deletingLastPathComponent().appendingPathComponent(".network-memory-\(UUID().uuidString).tmp")
        defer { try? FileManager.default.removeItem(at: staged) }
        try data.write(to: staged, options: [.atomic, .completeFileProtection])
        try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: staged.path)
        guard rename(staged.path, url.path) == 0 else { throw POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO) }
    }
}
