import Foundation

@main
struct ConnectionHealthWebsiteAggregationTests {
    private static var assertions = 0

    private static func check(_ condition: Bool, _ description: String) {
        guard condition else { fatalError("FAIL: \(description)") }
        assertions += 1
        print("PASS: \(description)")
    }

    private static func report(_ apple: ConnectivityEndpointState,
                               _ microsoft: ConnectivityEndpointState,
                               mode: ConnectivityProbeMode = .complete) -> ConnectivityReport {
        ConnectivityReport(appleResult: .init(state: apple, elapsed: 0.02),
                           microsoftResult: .init(state: microsoft, elapsed: 0.03), mode: mode)
    }

    private static func sample(_ apple: ConnectivityEndpointState,
                               _ microsoft: ConnectivityEndpointState,
                               total: Int? = nil, passed: Int? = nil,
                               date: Date = Date()) -> ConnectionSample {
        ConnectionSample(report: report(apple, microsoft), interface: "Wi-Fi en0", date: date,
                        websiteTotal: total, websitePassed: passed)
    }

    private static func rawSample(id: UUID = UUID(), date: Date,
                                  apple: String, microsoft: String,
                                  includeNewFields: Bool = true,
                                  total: Int = 2, passed: Int = 1,
                                  extra: [String: Any] = [:]) -> [String: Any] {
        var object: [String: Any] = [
            "id": id.uuidString,
            "date": date.timeIntervalSinceReferenceDate,
            "online": apple == "success" || microsoft == "success",
            "latencyMilliseconds": 20.0,
            "appleState": apple,
            "microsoftState": microsoft,
            "interface": "Wi-Fi en0"
        ]
        if includeNewFields {
            object["probeMode"] = ConnectivityProbeMode.complete.rawValue
            object["websiteTotal"] = total
            object["websitePassed"] = passed
        }
        for (key, value) in extra { object[key] = value }
        return object
    }

    private static func rawHistory(_ sample: [String: Any]) throws -> Data {
        try JSONSerialization.data(withJSONObject: ["samples": [sample]])
    }

    static func main() throws {
        setbuf(stdout, nil)
        let now = Date()

        // Old history predating selectable probes has no new optional keys.
        let oldPayload = try rawHistory(rawSample(date: now, apple: "success", microsoft: "network",
                                                   includeNewFields: false))
        let oldHistory = try JSONDecoder().decode(ConnectionHealth.self, from: oldPayload)
        check(oldHistory.samples.count == 1, "Legacy history without new optional keys still decodes")
        let oldSample = oldHistory.samples[0]
        check(oldSample.probeMode == nil && oldSample.websiteTotal == nil && oldSample.websitePassed == nil,
              "Missing probe and website fields migrate to nil")
        check(oldSample.online && oldSample.resultTitle == "基础检测通过",
              "Legacy connectivity outcomes retain their prior interpretation")

        // Site failures cannot erase fully passing base probes.
        let basePassedSitesPartial = sample(.success, .success, total: 2, passed: 1)
        check(basePassedSitesPartial.online && basePassedSitesPartial.allSelectedPassed == false,
              "A custom-site failure leaves the successful base state intact")
        check(basePassedSitesPartial.resultTitle == "完整检测 · 部分通过"
              && basePassedSitesPartial.isPartialCompleteResult,
              "Passing both base endpoints with one of two sites passing is partial")

        // A custom-site pass is useful evidence but cannot turn a failed base into online.
        let sitesPassedBaseFailed = sample(.timeout, .network, total: 2, passed: 1)
        check(!sitesPassedBaseFailed.online && sitesPassedBaseFailed.hasAnySelectedPass,
              "Site success remains separate from the failed base online state")
        check(sitesPassedBaseFailed.resultTitle == "完整检测 · 部分通过"
              && sitesPassedBaseFailed.isPartialCompleteResult,
              "Passing a selected site while both base probes fail is partial")

        let allPassed = sample(.success, .success, total: 3, passed: 3)
        check(allPassed.online && allPassed.allSelectedPassed
              && allPassed.resultTitle == "完整检测 · 全部通过"
              && !allPassed.isPartialCompleteResult,
              "Passing both base probes and every selected site is allPassed")

        let expandedPartial = sample(.success, .success, total: 12, passed: 11)
        check(expandedPartial.websiteTotal == 12 && expandedPartial.websitePassed == 11
              && expandedPartial.isPartialCompleteResult && !expandedPartial.allSelectedPassed,
              "Twelve-site aggregate retains its failing site instead of becoming base-only success")
        let expandedHistory = try JSONDecoder().decode(ConnectionHealth.self, from:
            rawHistory(rawSample(date: now, apple: "success", microsoft: "success", total: 12, passed: 11)))
        check(expandedHistory.samples.first?.websiteTotal == 12 && expandedHistory.samples.first?.websitePassed == 11
              && expandedHistory.samples.first?.isPartialCompleteResult == true,
              "Schema 1 history restores twelve-site totals without dropping partial evidence")

        // Both constructor and persisted-history restoration clamp malformed aggregates.
        for (total, passed, label) in [
            (13, 13, "too many configured websites"),
            (-1, 0, "negative website total"),
            (2, -1, "negative website pass count"),
            (2, 3, "passed count above total")
        ] {
            let constructed = sample(.success, .success, total: total, passed: passed)
            check(constructed.websiteTotal == nil && constructed.websitePassed == nil,
                  "Sample initializer drops \(label)")

            let payload = try rawHistory(rawSample(date: now, apple: "success", microsoft: "success",
                                                    total: total, passed: passed))
            let restored = try JSONDecoder().decode(ConnectionHealth.self, from: payload)
            check(restored.samples.count == 1 && restored.samples[0].websiteTotal == nil
                  && restored.samples[0].websitePassed == nil,
                  "Restoring persisted history drops \(label)")
        }
        let noWebsites = sample(.success, .success, total: 0, passed: 0)
        check(noWebsites.websiteTotal == 0 && noWebsites.websitePassed == 0,
              "A valid empty selected-site aggregate remains 0 of 0")

        // Even if an older or external JSON object carries website metadata, canonical history
        // retains only the aggregate counts and typed connection evidence.
        let privateHost = "private-customer-418.invalid"
        let privateURL = "https://\(privateHost)/tokenized-history-path-418"
        let inputWithUnknownSiteFields = try rawHistory(rawSample(
            date: now, apple: "success", microsoft: "network", total: 2, passed: 1,
            extra: ["websiteName": privateHost, "websiteURL": privateURL,
                    "customWebsites": [["name": privateHost, "url": privateURL]]]))
        let sanitizedHistory = try JSONDecoder().decode(ConnectionHealth.self, from: inputWithUnknownSiteFields)
        let canonicalData = try JSONEncoder().encode(sanitizedHistory)
        let canonicalText = String(decoding: canonicalData, as: UTF8.self)
        check(!canonicalText.contains(privateHost) && !canonicalText.contains(privateURL)
              && !canonicalText.contains("tokenized-history-path-418"),
              "Canonical history never retains a custom website name or URL")
        let canonicalObject = try JSONSerialization.jsonObject(with: canonicalData) as? [String: Any]
        let canonicalSamples = canonicalObject?["samples"] as? [[String: Any]]
        check(canonicalSamples?.first?["websiteTotal"] as? Int == 2
              && canonicalSamples?.first?["websitePassed"] as? Int == 1,
              "Canonical history keeps the website aggregate counts")
        let canonicalKeys = Set(canonicalSamples?.first?.keys ?? Dictionary<String, Any>().keys)
        check(!canonicalKeys.contains("websiteName") && !canonicalKeys.contains("websiteURL")
              && !canonicalKeys.contains("customWebsites"),
              "Canonical history schema contains no per-site metadata keys")

        print("\nConnectionHealth selected-site checks passed: \(assertions)")
    }
}
