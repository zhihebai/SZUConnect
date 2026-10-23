import Foundation

@main
struct ConnectionHealthTests {
    static func main() throws {
        setbuf(stdout, nil)
        var passed = 0
        func check(_ condition: @autoclosure () -> Bool, _ name: String) {
            guard condition() else { fatalError("FAIL: \(name)") }
            passed += 1
            print("PASS: \(name)")
        }
        let now = Date(timeIntervalSinceReferenceDate: 1_000_000)
        func report(_ apple: ConnectivityEndpointState, _ microsoft: ConnectivityEndpointState,
                    appleTime: Double = 0.1, microsoftTime: Double = 0.2) -> ConnectivityReport {
            ConnectivityReport(appleResult: .init(state: apple, elapsed: appleTime),
                               microsoftResult: .init(state: microsoft, elapsed: microsoftTime))
        }
        func sample(date: Date = now, latency: Double = 0.1) -> ConnectionSample {
            ConnectionSample(report: report(.success, .notCompleted, appleTime: latency),
                             interface: "Wi-Fi en0", date: date)
        }
        // Decode untrusted persisted data entirely in memory so restore must validate it.
        func restoredSample(id: UUID = UUID(), date: Date = now, online: Bool = true,
                            latency: Any = 100.0, apple: String = "success", microsoft: String = "network",
                            interface: String = "Wi-Fi en0") throws -> ConnectionSample {
            let data = try JSONSerialization.data(withJSONObject: [
                "id": id.uuidString, "date": date.timeIntervalSinceReferenceDate, "online": online,
                "latencyMilliseconds": latency, "appleState": apple, "microsoftState": microsoft,
                "interface": interface
            ])
            let decoder = JSONDecoder()
            decoder.nonConformingFloatDecodingStrategy = .convertFromString(
                positiveInfinity: "Infinity", negativeInfinity: "-Infinity", nan: "NaN")
            return try decoder.decode(ConnectionSample.self, from: data)
        }

        check(MonitoringMode.migrated(raw: nil, legacyAutoReconnect: false) == .manual,
              "Fresh migration defaults to manual checks")
        check(MonitoringMode.migrated(raw: nil, legacyAutoReconnect: true) == .recover,
              "Previously enabled automatic recovery is preserved")
        check(MonitoringMode.migrated(raw: "observe", legacyAutoReconnect: true) == .observe,
              "Explicit read-only preference takes precedence over legacy automatic flag")
        check(MonitoringMode.migrated(raw: "manual", legacyAutoReconnect: true) == .manual,
              "Explicit manual preference cannot be silently re-enabled by legacy flag")
        check(MonitoringMode.migrated(raw: "recover", legacyAutoReconnect: false) == .recover,
              "Explicit recovery preference takes precedence over legacy false flag")
        check(MonitoringMode.migrated(raw: "unknown-future-mode", legacyAutoReconnect: false) == .manual,
              "Unknown preference never enables new recovery permission")
        check(MonitoringMode.migrated(raw: "", legacyAutoReconnect: true) == .recover,
              "Invalid persisted mode falls back to the user's legacy intent")
        check(MonitoringMode.allCases.map(\.title) == ["手动检测", "定时检测", "自动重连"],
              "Monitoring choices have stable user-facing titles")
        check(MonitoringMode.allCases.allSatisfy { $0.id == $0.rawValue && !$0.detail.isEmpty },
              "Every monitoring choice has stable identity and explanatory text")
        for mode in MonitoringMode.allCases {
            let decoded = try JSONDecoder().decode(MonitoringMode.self, from: JSONEncoder().encode(mode))
            check(decoded == mode, "\(mode.rawValue) preference survives Codable round trip")
        }

        let fastest = ConnectionSample(report: report(.success, .success, appleTime: 0.5, microsoftTime: 0.12),
                                       interface: "Wi-Fi en0", date: now)
        check(fastest.online && fastest.latencyMilliseconds == 120,
              "Successful sample records the fastest successful endpoint in milliseconds")
        check(fastest.appleState == "success" && fastest.microsoftState == "success",
              "Only typed endpoint raw states become persisted outcome labels")
        let oneSuccess = ConnectionSample(report: report(.cancelled, .success, appleTime: 0.001, microsoftTime: 0.3),
                                          interface: "有线 en5", date: now)
        check(oneSuccess.online && oneSuccess.latencyMilliseconds == 300,
              "A fast cancelled endpoint cannot supply successful latency")
        for state in [ConnectivityEndpointState.timeout, .dns, .tls, .redirect, .unexpectedResponse,
                      .network, .cancelled, .notCompleted] {
            let failed = ConnectionSample(report: report(state, .cancelled), interface: "Wi-Fi en0", date: now)
            check(!failed.online && failed.latencyMilliseconds == nil,
                  "\(state.rawValue) and cancelled results never count as success or successful latency")
        }
        for invalid in [Double.nan, .infinity, -.infinity, -0.1, Double.greatestFiniteMagnitude] {
            let filtered = ConnectionSample(report: report(.success, .notCompleted, appleTime: invalid),
                                            interface: "Wi-Fi en0", date: now)
            check(filtered.online && filtered.latencyMilliseconds == nil,
                  "Invalid or overflowing elapsed time is discarded without rewriting typed success")
        }
        let invalidAndValid = ConnectionSample(report: report(.success, .success, appleTime: .nan, microsoftTime: 0.15),
                                              interface: "Wi-Fi en0", date: now)
        check(invalidAndValid.latencyMilliseconds == 150, "Invalid successful elapsed time cannot hide another valid measurement")
        check(sample(latency: 0).latencyMilliseconds == 0, "Zero elapsed time is a valid measurement")

        let privateInterface = ConnectionSample(report: report(.success, .network),
            interface: "Campus-private-SSID · 192.0.2.1 · https://private.example/?account=secret", date: now)
        check(privateInterface.interface == "未知接口", "Arbitrary SSID, IP and URL labels are not retained")
        let multiInterface = ConnectionSample(report: report(.success, .network),
            interface: " Wi-Fi en0 · 有线 en5 · Wi-Fi en0 · Campus-private-SSID ", date: now)
        check(multiInterface.interface == "Wi-Fi en0 · 有线 en5", "Coarse typed interface labels are normalized and deduplicated")
        let encodedInterface = String(decoding: try JSONEncoder().encode(privateInterface), as: UTF8.self)
        check(!encodedInterface.contains("private.example") && !encodedInterface.contains("192.0.2.1")
              && !encodedInterface.contains("secret"), "Encoded sample has no rejected private interface metadata")

        var health = ConnectionHealth()
        check(health.samples.isEmpty && health.successfulCount == 0 && health.medianLatencyMilliseconds == nil,
              "Empty history has no fabricated success or latency")
        check(health.successSummary == "0/0 次通过", "Empty summary explicitly describes sample counts")
        health.restore([sample(date: now.addingTimeInterval(-86_400)),
                        sample(date: now.addingTimeInterval(-86_400.01)),
                        sample(date: now.addingTimeInterval(60)),
                        sample(date: now.addingTimeInterval(60.01))], now: now)
        check(health.samples.count == 2, "Retention includes exact 24-hour and 60-second boundaries only")
        check(health.samples.first?.date == now.addingTimeInterval(-86_400)
              && health.samples.last?.date == now.addingTimeInterval(60),
              "Accepted boundary samples are chronologically sorted")
        health.restore([sample(date: now.addingTimeInterval(-30)), sample(date: now),
                        sample(date: now.addingTimeInterval(-10))], now: now)
        check(health.samples.map(\.date) == [-30.0, -10.0, 0.0].map { now.addingTimeInterval($0) },
              "Out-of-order restored samples display oldest to newest")
        let many = (0..<500).map { sample(date: now.addingTimeInterval(-Double($0))) }
        health.restore(many, now: now)
        check(health.samples.count == 360, "Large restore retains at most 360 observations")
        check(health.samples.first?.date == now.addingTimeInterval(-359) && health.samples.last?.date == now,
              "Sample cap retains the newest observations rather than input order")
        let newestSample = sample(date: now.addingTimeInterval(1))
        health.append(newestSample, now: now)
        check(health.samples.count == 360 && health.samples.last?.id == newestSample.id
              && health.samples.first?.date == now.addingTimeInterval(-358),
              "Appending a new observation evicts exactly the oldest at capacity")
        health.restore([sample()], now: now)
        health.append(sample(date: now.addingTimeInterval(86_401)), now: now.addingTimeInterval(86_401))
        check(health.samples.count == 1 && health.samples.first?.date == now.addingTimeInterval(86_401),
              "Appending prunes observations that expired while monitoring was paused")

        let sharedID = UUID()
        let older = try restoredSample(id: sharedID, date: now.addingTimeInterval(-10), latency: 200)
        let newer = try restoredSample(id: sharedID, date: now, latency: 100)
        health.restore([newer, older, newer], now: now)
        check(health.samples.count == 1 && health.samples[0].date == now && health.samples[0].latencyMilliseconds == 100,
              "Duplicate IDs retain only the newest record independent of input order")
        health.append(newer, now: now)
        check(health.samples.count == 1, "Replaying an already persisted observation is idempotent")
        let tiedA = try restoredSample(id: UUID(uuidString: "00000000-0000-0000-0000-000000000001")!)
        let tiedB = try restoredSample(id: UUID(uuidString: "00000000-0000-0000-0000-000000000002")!)
        health.restore([tiedB, tiedA], now: now)
        check(health.samples.map(\.id) == [tiedA.id, tiedB.id], "Equal-date ordering is deterministic across restore")

        let corrupt = try [restoredSample(latency: "NaN"), restoredSample(latency: "Infinity"),
                           restoredSample(latency: "-Infinity"), restoredSample(latency: -1),
                           restoredSample(apple: "https://private.example/response")]
        health.restore(corrupt + [sample()], now: now)
        check(health.samples.count == 1, "Corrupt latency and untyped persisted states are dropped")
        let inconsistentFailure = try restoredSample(online: true, latency: 2, apple: "cancelled", microsoft: "timeout")
        let inconsistentSuccess = try restoredSample(online: false, latency: 40, apple: "success")
        health.restore([inconsistentFailure, inconsistentSuccess], now: now)
        check(health.successfulCount == 1, "Restore derives online status from typed states, never from an inconsistent flag")
        check(health.samples.first(where: { $0.id == inconsistentFailure.id })?.latencyMilliseconds == nil,
              "Restored failed observation cannot contribute a fabricated successful latency")
        check(health.medianLatencyMilliseconds == 40 && health.successSummary == "1/2 次通过",
              "Health summaries count real successful samples without implying continuous uptime")
        let restoredPrivate = try restoredSample(interface: "private-SSID · 192.0.2.1 · Wi-Fi en0")
        health.restore([restoredPrivate], now: now)
        check(health.samples[0].interface == "Wi-Fi en0", "Restore removes legacy private metadata from interface labels")
        health.restore([sample(date: Date(timeIntervalSinceReferenceDate: .infinity)), sample()], now: now)
        check(health.samples.count == 1, "Nonfinite observation dates are excluded")
        health.restore([sample()], now: Date(timeIntervalSinceReferenceDate: .nan))
        check(health.samples.isEmpty, "An invalid reference clock cannot admit unbounded history")

        health.restore([sample(latency: 0.01), sample(latency: 0.09), sample(latency: 0.03)], now: now)
        check(health.medianLatencyMilliseconds == 30, "Odd-sized latency history reports the median rather than mean")
        health.append(sample(latency: 0.05), now: now)
        check(health.medianLatencyMilliseconds == 40, "Even-sized latency history averages the two central observations")
        let noLatency = ConnectionSample(report: report(.success, .network, appleTime: .nan), interface: "Wi-Fi en0", date: now)
        health.append(noLatency, now: now)
        health.append(ConnectionSample(report: report(.network, .cancelled), interface: "有线 en5", date: now), now: now)
        check(health.successfulCount == 5 && health.samples.count == 6 && health.medianLatencyMilliseconds == 40,
              "Missing latency and failures stay in sample counts but not latency median")
        health.restore([try restoredSample(latency: Double.greatestFiniteMagnitude),
                        try restoredSample(latency: Double.greatestFiniteMagnitude)], now: now)
        check(health.medianLatencyMilliseconds == Double.greatestFiniteMagnitude,
              "Even median avoids overflow for valid large finite values")

        let restartNow = Date()
        var beforeRestart = ConnectionHealth()
        beforeRestart.restore([sample(date: restartNow.addingTimeInterval(-10), latency: 0.04),
                               sample(date: restartNow, latency: 0.08)], now: restartNow)
        let persisted = try JSONEncoder().encode(beforeRestart)
        let afterRestart = try JSONDecoder().decode(ConnectionHealth.self, from: persisted)
        check(afterRestart.samples.map(\.id) == beforeRestart.samples.map(\.id)
              && afterRestart.samples.map(\.date) == beforeRestart.samples.map(\.date),
              "Codable restart preserves observation identities and chronology")
        check(afterRestart.successSummary == "2/2 次通过" && afterRestart.medianLatencyMilliseconds == 60,
              "Restarted health statistics match stored observations")
        let stalePayload = try JSONSerialization.data(withJSONObject: ["samples": [
            ["id": UUID().uuidString, "date": restartNow.addingTimeInterval(-86_500).timeIntervalSinceReferenceDate,
             "online": true, "latencyMilliseconds": 100, "appleState": "success", "microsoftState": "network", "interface": "Wi-Fi en0"]
        ]])
        let staleHistory = try JSONDecoder().decode(ConnectionHealth.self, from: stalePayload)
        check(staleHistory.samples.isEmpty, "Codable decoding immediately prunes expired history before display")

        check(beforeRestart.snapshot(now: restartNow.addingTimeInterval(86_501)).samples.isEmpty,
              "Read snapshots expire old samples even without new probes or app restart")
        check(beforeRestart.samples.count == 2,
              "Reading a current window does not mutate the stored evidence")
        // Fast read snapshots must preserve the validated restore contract across clock changes.
        for offset in [-3600.0, -60, 0, 86400, 86501] {
            let shifted = restartNow.addingTimeInterval(offset)
            var reference = ConnectionHealth()
            reference.restore(afterRestart.samples, now: shifted)
            let fast = afterRestart.snapshot(now: shifted)
            check(fast.samples.map(\.id) == reference.samples.map(\.id)
                  && fast.samples.map(\.interface) == reference.samples.map(\.interface)
                  && fast.successSummary == reference.successSummary,
                  "Optimized snapshot preserves validated history at clock offset \(offset)")
        }
        check(afterRestart.snapshot(now: Date(timeIntervalSinceReferenceDate: .nan)).samples.isEmpty,
              "Fast snapshot rejects an invalid clock without leaking unbounded history")

        // Incremental writes must remain byte-for-byte equivalent to the untrusted restore
        // path, including duplicate replacement, equal-date ordering and clock movement.
        let canonicalEncoder = JSONEncoder()
        canonicalEncoder.outputFormatting = [.sortedKeys]
        var incremental = ConnectionHealth()
        incremental.restore((0..<400).map { sample(date: now.addingTimeInterval(-Double($0))) }, now: now)
        let duplicateIDs = (0..<37).map { _ in UUID() }
        for index in 0..<600 {
            let offset = [-86_501.0, -60, 0, 30, 61, 300][index % 6]
            let input = try restoredSample(id: duplicateIDs[index % duplicateIDs.count],
                date: now.addingTimeInterval(offset), online: index.isMultiple(of: 2),
                latency: index.isMultiple(of: 17) ? "NaN" as Any : Double(index),
                apple: index.isMultiple(of: 19) ? "invalid-state" : (index.isMultiple(of: 3) ? "timeout" : "success"),
                microsoft: "network", interface: index.isMultiple(of: 7) ? "private-SSID · Wi-Fi en0" : "有线 en5")
            let clockOffset = index < 400 ? 0.0 : [-86_400.0, -61, 0, 86_400, 86_501][index % 5]
            let clock = now.addingTimeInterval(clockOffset)
            var reference = ConnectionHealth()
            reference.restore(incremental.samples + [input], now: clock)
            incremental.append(input, now: clock)
            let actualData = try canonicalEncoder.encode(incremental)
            let referenceData = try canonicalEncoder.encode(reference)
            check(actualData == referenceData,
                  "Incremental append matches full validated replay at step \(index)")
        }
        incremental.restore([sample()], now: now)
        var invalidClockReference = ConnectionHealth()
        invalidClockReference.restore(incremental.samples + [sample()], now: Date(timeIntervalSinceReferenceDate: .nan))
        incremental.append(sample(), now: Date(timeIntervalSinceReferenceDate: .nan))
        let invalidClockData = try canonicalEncoder.encode(invalidClockReference)
        let incrementalClockData = try canonicalEncoder.encode(incremental)
        check(incrementalClockData == invalidClockData,
              "Incremental append clears history for a nonfinite reference clock exactly like restore")

        for invalid in ["{}", "{\"samples\":null}"] {
            do {
                _ = try JSONDecoder().decode(ConnectionHealth.self, from: Data(invalid.utf8))
                check(false, "Malformed storage must throw")
            } catch { check(true, "Missing or null sample array cannot silently erase a damaged file") }
        }

        // Presentation metadata and summary values derived from recorded samples.
        check(MonitoringMode.allCases.map(\.title) == ["手动检测", "定时检测", "自动重连"],
              "Monitoring titles stay stable for existing users")
        check(MonitoringMode.allCases.allSatisfy { !$0.symbol.isEmpty && !$0.caption.isEmpty && !$0.detail.isEmpty },
              "Every monitoring choice has a symbol, a one-line caption and a longer explanation")
        var rated = ConnectionHealth()
        check(rated.successRate == nil, "An empty history has no fabricated pass rate")
        rated.restore([sample(), ConnectionSample(report: report(.network, .timeout), interface: "Wi-Fi en0", date: now)],
                      now: now)
        check(rated.successRate == 0.5, "Pass rate counts recorded samples only")
        let captive = ConnectionSample(report: report(.tls, .redirect), interface: "Wi-Fi en0", date: now)
        check(captive.diagnosis == .portal && sample().diagnosis == .online,
              "Stored samples derive a plain-language diagnosis without changing their format")
        let legacy = try restoredSample(online: false, apple: "timeout", microsoft: "retired-state")
        check(legacy.diagnosis == .timeout, "Unknown stored states are ignored rather than guessed")

        let completePartial = ConnectivityReport(appleResult: .init(state: .success, elapsed: 0.1),
            microsoftResult: .init(state: .timeout, elapsed: 1), mode: .complete)
        let completeSample = ConnectionSample(report: completePartial, interface: "Wi-Fi en0", date: now)
        let roundTrip = try JSONDecoder().decode(ConnectionSample.self, from: JSONEncoder().encode(completeSample))
        check(roundTrip.probeMode == "complete" && roundTrip.online && roundTrip.isPartialCompleteResult,
              "History round-trips complete mode without losing partial connectivity semantics")
        let oldSample = try restoredSample()
        check(oldSample.probeMode == nil && oldSample.online,
              "Pre-mode history still decodes without erasing user records")
        var modeHistory = ConnectionHealth()
        modeHistory.restore([roundTrip], now: now)
        check(modeHistory.samples.first?.probeMode == "complete", "Normalization preserves a known probe mode")

        check(completeSample == roundTrip,
              "An unchanged sample remains equal after a Codable round trip")
        let sampleEncoder = JSONEncoder(); sampleEncoder.outputFormatting = [.sortedKeys]
        let sampleBytes = try sampleEncoder.encode(completeSample)
        let sampleObject = try JSONSerialization.jsonObject(with: sampleBytes) as! [String: Any]
        let changedFields: [(String, Any)] = [
            ("id", UUID().uuidString), ("date", now.addingTimeInterval(1).timeIntervalSinceReferenceDate),
            ("online", false), ("latencyMilliseconds", 999.0), ("appleState", "timeout"),
            ("microsoftState", "success"), ("interface", "有线 en5"), ("probeMode", "quick"),
            ("websiteTotal", 3), ("websitePassed", 2), ("appleMilliseconds", 101.0),
            ("microsoftMilliseconds", 1001.0), ("completeResult", "partial")
        ]
        for (key, value) in changedFields {
            var changed = sampleObject; changed[key] = value
            if key == "websiteTotal" { changed["websitePassed"] = 2 }
            if key == "websitePassed" { changed["websiteTotal"] = 3 }
            let decoded = try JSONDecoder().decode(ConnectionSample.self, from: JSONSerialization.data(withJSONObject: changed))
            check(decoded != completeSample, "Changing sample \(key) invalidates the records value input")
        }
        let roundTripBytes = try sampleEncoder.encode(roundTrip)
        check(roundTripBytes == sampleBytes,
              "Value equality preserves sample Codable bytes and optional fields")

        var collectedContext = DiagnosticSampleContext(operation: "automatic", monitoringMode: "recover",
            area: "teaching", startedAt: now.addingTimeInterval(-2),
            environment: .init(connection: .multiple, systemProxy: .off, tun: .detected, refreshing: false))
        let historicalPartial = ConnectionSample(report: completePartial, interface: "Wi-Fi en0 · 有线 en5", date: now,
            websiteTotal: 12, websitePassed: 11, context: collectedContext, completeResult: "partial")
        check(historicalPartial.appleMilliseconds == 100 && historicalPartial.microsoftMilliseconds == 1000,
              "Per-endpoint evidence retains actual success and failure elapsed time")
        check(historicalPartial.online && historicalPartial.isHistoricalFailure && historicalPartial.latencyMilliseconds == 100,
              "Complete partial failure remains visible without changing base success or successful latency")
        collectedContext.area = "dormitory"
        collectedContext.environment = .unknown
        check(historicalPartial.context?.area == "teaching" && historicalPartial.context?.environment.connection == .multiple
              && historicalPartial.context?.startedAt == now.addingTimeInterval(-2),
              "Stored context is a value snapshot unaffected by later area or environment changes")
        let historyRoundTrip = try JSONDecoder().decode(ConnectionSample.self, from: JSONEncoder().encode(historicalPartial))
        check(historyRoundTrip == historicalPartial, "New optional failure evidence round-trips without losing its original context")
        let anonymous = historicalPartial.anonymizedForExport
        check(anonymous.interface == "Wi-Fi · 有线" && anonymous.context == historicalPartial.context
              && anonymous.appleMilliseconds == 100 && anonymous.microsoftMilliseconds == 1000 && anonymous.completeResult == "partial",
              "Export removes interface identifiers while retaining reviewed timing and context evidence")
        let anonymousBytes = String(decoding: try JSONEncoder().encode(anonymous), as: UTF8.self)
        check(!anonymousBytes.contains("en0") && !anonymousBytes.contains("en5") && !anonymousBytes.contains("192.0.2"),
              "Historical export contains no raw interface or address fields")
        check(oldSample.appleMilliseconds == nil && oldSample.microsoftMilliseconds == nil
              && oldSample.context == nil && oldSample.completeResult == nil,
              "Old stored records do not fabricate elapsed times, context or complete verdicts")

        let quickSkipped = ConnectionSample(report: report(.success, .notCompleted, microsoftTime: 0),
            interface: "Wi-Fi en0", date: now, completeResult: "basicPassed")
        check(quickSkipped.appleMilliseconds == 100 && quickSkipped.microsoftMilliseconds == nil && !quickSkipped.isHistoricalFailure,
              "An unawaited quick-check endpoint has no zero duration and does not become failure evidence")
        let unperformed = ConnectionSample(report: report(.notCompleted, .notCompleted),
            interface: "Wi-Fi en0", date: now, completeResult: "unavailable")
        check(unperformed.appleMilliseconds == nil && unperformed.microsoftMilliseconds == nil && !unperformed.isHistoricalFailure,
              "A skipped-only observation does not fabricate endpoint failure or duration")
        let deferredSites = ConnectionSample(report: ConnectivityReport(
            appleResult: .init(state: .notCompleted, elapsed: 0), microsoftResult: .init(state: .notCompleted, elapsed: 0), mode: .complete),
            interface: "Wi-Fi en0", date: now, websiteTotal: 3, websitePassed: 0, completeResult: "unavailable")
        check(!deferredSites.isHistoricalFailure,
              "Selected-site counts cannot turn a wholly unperformed complete round into a fabricated failure")
        let cancelled = ConnectionSample(report: report(.timeout, .cancelled), interface: "Wi-Fi en0", date: now)
        check(cancelled.appleMilliseconds == 100 && cancelled.microsoftMilliseconds == nil && !cancelled.isHistoricalFailure,
              "A cancelled round retains completed timing but does not enter recent failures")
        let explicitlyCancelled = ConnectionSample(report: completePartial, interface: "Wi-Fi en0", date: now, completeResult: "cancelled")
        check(!explicitlyCancelled.isHistoricalFailure, "An explicit whole-round cancellation excludes otherwise failed endpoint results")
        let allBasePassed = ConnectivityReport(appleResult: .init(state: .success, elapsed: 0.1),
            microsoftResult: .init(state: .success, elapsed: 0.2), mode: .complete)
        for count in [5, 12] {
            let selectedFailure = ConnectionSample(report: allBasePassed, interface: "Wi-Fi en0", date: now,
                websiteTotal: count, websitePassed: count - 1, completeResult: "partial")
            check(selectedFailure.online && selectedFailure.isHistoricalFailure && selectedFailure.websiteTotal == count,
                  "A complete check preserves selected-site failure with \(count) saved sites even when base connectivity passed")
        }
        let fullyPassed = ConnectionSample(report: allBasePassed, interface: "Wi-Fi en0", date: now,
            websiteTotal: 12, websitePassed: 12, completeResult: "allPassed")
        check(!fullyPassed.isHistoricalFailure, "A successful complete check never overwrites or impersonates a historical failure")
        for state in [ConnectivityEndpointState.timeout, .dns, .tls, .redirect, .unexpectedResponse, .network] {
            let failure = ConnectionSample(report: report(state, .notCompleted, appleTime: 0.123),
                interface: "有线 en5", date: now)
            check(failure.appleMilliseconds == 123 && failure.microsoftMilliseconds == nil && failure.isHistoricalFailure,
                  "\(state.rawValue) preserves actual failed elapsed time without inventing the unawaited endpoint")
        }
        for elapsed in [Double.nan, .infinity, -.infinity, -0.1, 120.001, Double.greatestFiniteMagnitude] {
            let invalid = ConnectionSample(report: report(.timeout, .success, appleTime: elapsed, microsoftTime: elapsed),
                interface: "有线 en5", date: now)
            check(invalid.appleMilliseconds == nil && invalid.microsoftMilliseconds == nil,
                  "Invalid or over-two-minute per-item timing is discarded independently of result state")
        }
        let timingBoundary = ConnectionSample(report: report(.timeout, .success, appleTime: 120, microsoftTime: 0),
            interface: "有线 en5", date: now)
        check(timingBoundary.appleMilliseconds == 120_000 && timingBoundary.microsoftMilliseconds == 0,
              "Actual zero and two-minute boundary durations remain valid completed measurements")

        let historicalObject = try JSONSerialization.jsonObject(with: JSONEncoder().encode(historicalPartial)) as! [String: Any]
        func alteredHistory(_ changes: [String: Any]) throws -> ConnectionSample {
            var object = historicalObject
            for (key, value) in changes { object[key] = value }
            let decoder = JSONDecoder()
            decoder.nonConformingFloatDecodingStrategy = .convertFromString(
                positiveInfinity: "Infinity", negativeInfinity: "-Infinity", nan: "NaN")
            return try decoder.decode(ConnectionSample.self, from: JSONSerialization.data(withJSONObject: object))
        }
        for invalid in ["NaN" as Any, "Infinity", "-Infinity", -1, 120_001, "private-SSID", ["raw": "private-text"]] {
            let decoded = try alteredHistory(["appleMilliseconds": invalid, "microsoftMilliseconds": invalid])
            check(decoded.appleMilliseconds == nil && decoded.microsoftMilliseconds == nil && decoded.context != nil,
                  "Invalid optional duration fields are dropped without erasing the valid historical observation")
        }
        for invalid in ["private-SSID" as Any, 42, ["unexpected": "value"]] {
            let decoded = try alteredHistory(["completeResult": invalid])
            check(decoded.completeResult == nil && decoded.appleMilliseconds == 100,
                  "Unknown or malformed optional complete result does not enter persisted evidence")
        }
        for changes in [["websiteTotal": 13, "websitePassed": 12] as [String: Any],
                        ["websiteTotal": -1, "websitePassed": 0], ["websiteTotal": 12, "websitePassed": 13],
                        ["websiteTotal": "private-value", "websitePassed": 1]] {
            let decoded = try alteredHistory(changes)
            check(decoded.websiteTotal == nil && decoded.websitePassed == nil && decoded.context != nil,
                  "Invalid or malformed optional selected-site counts cannot erase valid historical context")
        }
        let contextObject = historicalObject["context"] as! [String: Any]
        for (key, value) in [("phase", "raw-request" as Any), ("operation", "arbitrary"),
                             ("monitoringMode", "arbitrary"), ("area", "arbitrary"),
                             ("startedAt", now.addingTimeInterval(1).timeIntervalSinceReferenceDate)] {
            var changed = contextObject; changed[key] = value
            let decoded = try alteredHistory(["context": changed])
            check(decoded.context == nil && decoded.appleMilliseconds == 100 && decoded.isHistoricalFailure,
                  "Invalid \(key) context is discarded without rewriting observed failure results")
        }
        for key in ["connection", "systemProxy", "tun"] {
            var changed = contextObject
            var environment = changed["environment"] as! [String: Any]
            environment[key] = "private-value"; changed["environment"] = environment
            let decoded = try alteredHistory(["context": changed])
            check(decoded.context == nil && decoded.microsoftMilliseconds == 1000,
                  "Unknown \(key) environment enum cannot leak into restored historical context")
        }
        let malformedContext = try alteredHistory(["context": "private-text"])
        check(malformedContext.context == nil, "Malformed optional context does not reject the entire old-compatible sample")
        for invalidStart in ["NaN" as Any, "Infinity", -3_155_760_001.0] {
            var changed = contextObject; changed["startedAt"] = invalidStart
            let decoded = try alteredHistory(["context": changed])
            check(decoded.context == nil && decoded.isHistoricalFailure,
                  "Nonfinite or out-of-range context clocks do not replace actual failure evidence")
        }
        var historicalWindow = ConnectionHealth()
        historicalWindow.restore([historicalPartial, fullyPassed], now: now)
        check(historicalWindow.samples.filter(\.isHistoricalFailure).count == 1 && historicalWindow.successfulCount == 2,
              "A later success preserves the earlier failure while base success statistics retain their original meaning")
        check(historicalWindow.snapshot(now: now.addingTimeInterval(86_400)).samples.count == 2
              && historicalWindow.snapshot(now: now.addingTimeInterval(86_400.01)).samples.isEmpty,
              "Optional diagnostic evidence follows the existing exact 24-hour retention window")
        check(historicalWindow.samples.first(where: { $0.id == historicalPartial.id })?.context?.area == "teaching",
              "Reading or expiring a snapshot cannot mutate the original failure context")
        let liveDate = Date()
        let liveContext = DiagnosticSampleContext(operation: "manual", monitoringMode: "observe", area: "automatic",
            startedAt: liveDate.addingTimeInterval(-2), environment: .unknown)
        let persistedFailure = ConnectionSample(report: completePartial, interface: "有线 en5", date: liveDate,
            websiteTotal: 5, websitePassed: 4, context: liveContext, completeResult: "partial")
        var beforeEvidenceRestart = ConnectionHealth()
        beforeEvidenceRestart.restore([persistedFailure], now: liveDate)
        let afterEvidenceRestart = try JSONDecoder().decode(ConnectionHealth.self, from: JSONEncoder().encode(beforeEvidenceRestart))
        check(afterEvidenceRestart.samples == [persistedFailure] && afterEvidenceRestart.samples[0].isHistoricalFailure,
              "Restarted bounded history preserves original failed timing and anonymous context without current-environment backfill")

        print("\(passed) pure connection-health assertions passed. No network, app, Keychain or file I/O was used.")
    }
}
