import Foundation
#if canImport(Darwin)
import Darwin
#else
import Glibc
#endif
#if os(macOS)
import CoreWLAN
import CoreLocation
import SystemConfiguration
#endif

/// Local presentation metadata only. It never participates in network identity, matching or trust.
enum NetworkLabelKind: String, Codable, Sendable { case wifi, ethernet, other }

struct NetworkLabel: Codable, Equatable, Sendable {
    var displayName: String
    var kind: NetworkLabelKind
    var ssid: String?
    var manualName: Bool
}

struct NetworkLabels: Equatable, Sendable {
    static let unnamed = "待命名的校园网络"
    static let wifiUnnamed = "校园 Wi-Fi（名称待补全）"
    static let ethernetUnnamed = "校园有线网络"
    static let maxNameLength = 80
    private(set) var entries: [String: NetworkLabel] = [:]

    static func cleanName(_ text: String) -> String {
        let visible = text.unicodeScalars.filter {
            !CharacterSet.controlCharacters.contains($0) && $0.properties.generalCategory != .format &&
                $0.value != 0x2028 && $0.value != 0x2029
        }
        return String(String(String.UnicodeScalarView(visible))
            .trimmingCharacters(in: .whitespacesAndNewlines).prefix(maxNameLength))
    }

    /// Preserve the exact SSID, including meaningful leading/trailing spaces. An unsafe or invalid
    /// string is unavailable; never turn a modified display string into a connection candidate.
    static func validSSID(_ text: String?) -> String? {
        guard let text, !text.isEmpty, text.utf8.count <= 32,
              !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty,
              text.unicodeScalars.allSatisfy({ !CharacterSet.controlCharacters.contains($0) &&
                  $0.properties.generalCategory != .format && $0.value != 0x2028 && $0.value != 0x2029 }) else { return nil }
        return text
    }

    private static func validID(_ id: String) -> Bool {
        (8...32).contains(id.count) && id.allSatisfy { ("a"..."f").contains($0) || ("0"..."9").contains($0) }
    }

    func displayName(for id: String) -> String { entries[id]?.displayName ?? Self.unnamed }

    /// Presentation only; equal names do not establish equal network identities.
    func sameNameIndex(for id: String, among ids: [String]) -> Int? {
        let name = displayName(for: id)
        let matching = ids.filter { displayName(for: $0) == name }.sorted()
        guard matching.count > 1, let index = matching.firstIndex(of: id) else { return nil }
        return index + 1
    }

    @discardableResult
    mutating func updateAutomatic(id: String, displayName: String, kind: NetworkLabelKind, ssid: String? = nil) -> Bool {
        guard Self.validID(id), entries[id] != nil || entries.count < 64 else { return false }
        let prior = entries[id]
        let observedSSID = Self.validSSID(ssid)
        let keepWiFi = prior?.ssid != nil && observedSSID == nil && kind != .wifi
        let name = Self.cleanName(displayName)
        let fallback = kind == .wifi ? Self.wifiUnnamed : kind == .ethernet ? Self.ethernetUnnamed : Self.unnamed
        var next = NetworkLabel(displayName: name.isEmpty ? fallback : name, kind: kind,
                                ssid: observedSSID ?? prior?.ssid, manualName: prior?.manualName ?? false)
        if prior?.manualName == true || keepWiFi { next.displayName = prior!.displayName }
        if keepWiFi { next.kind = prior!.kind }
        // A temporary loss of permission must not replace a previously known Wi-Fi name with a placeholder.
        if kind == .wifi, observedSSID == nil, prior?.ssid != nil, !next.manualName { next.displayName = prior!.displayName }
        guard next != prior else { return false }
        entries[id] = next
        return true
    }

    @discardableResult
    mutating func rename(id: String, to name: String) -> Bool {
        let name = Self.cleanName(name)
        guard Self.validID(id), !name.isEmpty, entries[id] != nil || entries.count < 64 else { return false }
        var next = entries[id] ?? NetworkLabel(displayName: name, kind: .other, ssid: nil, manualName: false)
        next.displayName = name
        next.manualName = true
        guard next != entries[id] else { return false }
        entries[id] = next
        return true
    }

    @discardableResult
    mutating func retain(ids: Set<String>) -> Bool {
        let count = entries.count
        entries = entries.filter { ids.contains($0.key) }
        return entries.count != count
    }

    private struct Stored: Codable { var schema: Int; var labels: [String: NetworkLabel] }
    func encoded() -> Data {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys]
        return (try? encoder.encode(Stored(schema: 1, labels: entries))) ?? Data()
    }

    /// Unknown schema or malformed input is unavailable and must not be rewritten by the caller.
    static func decode(_ data: Data) -> NetworkLabels? {
        guard data.count <= 65_536, let stored = try? JSONDecoder().decode(Stored.self, from: data),
              stored.schema == 1, stored.labels.count <= 64 else { return nil }
        var result = NetworkLabels()
        for (id, value) in stored.labels {
            guard validID(id) else { return nil }
            let name = cleanName(value.displayName)
            guard !name.isEmpty else { return nil }
            result.entries[id] = NetworkLabel(displayName: name, kind: value.kind,
                                               ssid: validSSID(value.ssid), manualName: value.manualName)
        }
        return result
    }
}

/// This sidecar is deliberately separate from network-memory.json and all diagnostic exporters.
struct NetworkLabelsStore: Sendable {
    static let filename = "network-labels.json"
    let url: URL
    enum LoadResult: Sendable { case missing, loaded(NetworkLabels), unavailable }

    func load() -> LoadResult {
        let manager = FileManager.default
        guard manager.fileExists(atPath: url.path) else {
            // A dangling symlink is not an absent file that this store may replace.
            if (try? manager.destinationOfSymbolicLink(atPath: url.path)) != nil { return .unavailable }
            return .missing
        }
        guard let attributes = try? manager.attributesOfItem(atPath: url.path),
              attributes[.type] as? FileAttributeType == .typeRegular,
              let size = attributes[.size] as? NSNumber, size.intValue <= 65_536,
              let data = try? Data(contentsOf: url), let labels = NetworkLabels.decode(data) else { return .unavailable }
        return .loaded(labels)
    }

    @discardableResult
    func save(_ labels: NetworkLabels) -> Bool {
        // Recheck on each write so a corrupt/newer file appearing after launch is also preserved.
        if case .unavailable = load() { return false }
        let data = labels.encoded()
        guard !data.isEmpty, data.count <= 65_536 else { return false }
        let directory = url.deletingLastPathComponent()
        do { try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true,
                                                      attributes: [.posixPermissions: 0o700]) }
        catch { return false }
        let temporary = directory.appendingPathComponent(".network-labels-\(UUID().uuidString).tmp")
        let descriptor = open(temporary.path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, S_IRUSR | S_IWUSR)
        guard descriptor >= 0 else { return false }
        let file = FileHandle(fileDescriptor: descriptor, closeOnDealloc: true)
        defer { try? file.close(); try? FileManager.default.removeItem(at: temporary) }
        do { try file.write(contentsOf: data); try file.synchronize(); try file.close() }
        catch { return false }
        return rename(temporary.path, url.path) == 0
    }
}

struct NetworkObservedLabel: Equatable, Sendable {
    var displayName: String
    var kind: NetworkLabelKind
    var ssid: String?

    static func make(kind: NetworkLabelKind, serviceName: String? = nil, ssid: String? = nil) -> Self {
        let ssid = kind == .wifi ? NetworkLabels.validSSID(ssid) : nil
        let service = NetworkLabels.cleanName(serviceName ?? "")
        let name: String
        switch kind {
        case .wifi: name = ssid.map(NetworkLabels.cleanName) ?? NetworkLabels.wifiUnnamed
        case .ethernet: name = service.isEmpty ? NetworkLabels.ethernetUnnamed : service
        case .other: name = service.isEmpty ? NetworkLabels.unnamed : service
        }
        return Self(displayName: name, kind: kind, ssid: ssid)
    }

    /// Read-only metadata for an already-selected reliable physical interface. Does not ask for
    /// location permission, scan networks, connect, or alter any networking preference.
    static func capture(interface: String, kind: NetworkLabelKind, serviceID: String? = nil) -> Self {
        #if os(macOS)
        var serviceName: String?
        if kind != .wifi,
           let preferences = SCPreferencesCreate(nil, "local.SZUConnect.labels" as CFString, nil),
           let services = SCNetworkServiceCopyAll(preferences) as? [SCNetworkService] {
            for service in services {
                if let serviceID, SCNetworkServiceGetServiceID(service) as String? != serviceID { continue }
                guard let networkInterface = SCNetworkServiceGetInterface(service),
                      SCNetworkInterfaceGetBSDName(networkInterface) as String? == interface else { continue }
                serviceName = SCNetworkServiceGetName(service) as String?
                if serviceName != nil { break }
            }
        }
        var ssid: String?
        if kind == .wifi {
            // Create the permission reader on a thread with the main run loop; never request access.
            let readPermission = { CLLocationManager().authorizationStatus == .authorizedAlways }
            let permitted = Thread.isMainThread ? readPermission() : DispatchQueue.main.sync(execute: readPermission)
            if permitted {
                ssid = CWWiFiClient.shared().interface(withName: interface)?.ssid()
            }
        }
        return make(kind: kind, serviceName: serviceName, ssid: ssid)
        #else
        return make(kind: kind)
        #endif
    }
}
