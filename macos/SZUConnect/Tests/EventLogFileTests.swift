import Foundation

@main struct EventLogFileTests {
    static func main() throws {
        var passed = 0
        func check(_ value: Bool, _ name: String) {
            guard value else { fatalError("FAIL: \(name)") }
            passed += 1; print("PASS: \(name)")
        }
        let folder = FileManager.default.temporaryDirectory.appendingPathComponent("szu-fixture-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: folder) }
        let url = folder.appendingPathComponent("events.json")
        let events = (0..<301).map { ConnectionEvent(date: Date(), message: "fixture \($0)") }
        try JSONEncoder().encode(events).write(to: url)
        let loaded = try EventLogFile.read(from: url)
        check(loaded.count == 300 && loaded[0].id == events[0].id, "Existing event schema and newest entries stay compatible")
        let oversized = Data(repeating: 32, count: EventLogFile.maximumBytes + 8)
        try oversized.write(to: url)
        do { _ = try EventLogFile.read(from: url); fatalError("Oversized log accepted") }
        catch { check((error as NSError).code == CocoaError.fileReadTooLarge.rawValue, "Oversized input is rejected by a bounded read") }
        check((try Data(contentsOf: url)) == oversized, "Failed load never rewrites original bytes")
        let first = try EventLogFile.quarantine(url, date: Date(timeIntervalSince1970: 100))
        check(!FileManager.default.fileExists(atPath: url.path), "Recovery moves the unreadable original out of the active log path")
        check((try Data(contentsOf: first)) == oversized, "Recovery backup retains every original byte")
        try Data("broken".utf8).write(to: url)
        let second = try EventLogFile.quarantine(url, date: Date(timeIntervalSince1970: 100))
        check(first != second && FileManager.default.fileExists(atPath: first.path), "Same-second recoveries never collide or replace prior backup")
        for malformed in [Data("broken".utf8), try JSONEncoder().encode([ConnectionEvent(date: Date(), message: String(repeating: "a", count: 8193))]),
                          try JSONEncoder().encode([ConnectionEvent(date: Date(timeIntervalSinceReferenceDate: 1e100), message: "fixture")])] {
            try malformed.write(to: url)
            do { _ = try EventLogFile.read(from: url); fatalError("Malformed log accepted") }
            catch { check(true, "Damaged payload is rejected before rendering") }
            check((try Data(contentsOf: url)) == malformed, "Rejected event data remains available for quarantine")
        }
        check(RecordMessageClassifier.isFailure("fixture-old-failure with details", legacyMarkers: ["fixture-old-failure"]),
              "Older consumer failure markers remain recognized without rewriting stored messages")
        for key in ["portalError.rejected", "portalError.tls", "portalError.timeout", "event.offlineAfterOnline"] {
            check(RecordMessageClassifier.isFailure(SharedCopy.t(key), legacyMarkers: []),
                  "The current shared failure text appears in the failures-only list")
        }
        check(!RecordMessageClassifier.isFailure(SharedCopy.t("portalError.authenticationUncertain"), legacyMarkers: []),
              "An unknown submission result is not mislabeled as a confirmed failure")
        check(!RecordMessageClassifier.isFailure(SharedCopy.t("event.online"), legacyMarkers: []),
              "A successful detection is not selected by the failures-only filter")
        let previousFailures = [
            "外网检测这次没通过，稍后会再测。",
            "暂时连不上校园登录服务。确认已经连上校园网；开着代理或 VPN 的话，检查一下校园地址规则。",
            "校园登录服务没有及时回复。先检测一下连接，看看是不是已经能上网。",
            "找不到校园登录地址。检查一下网络、DNS 和代理规则。",
            "校园登录地址要求跳转，登录已停止。检查一下当前网络，或者改用学校官方登录页。",
            "校园登录服务暂时处理不了请求。",
            "校园登录服务回复异常，已经停止处理。过一会儿再试。",
            "校园登录页面的信息有误，登录已停止。重新检测一次，或者改用学校官方登录页。",
            "服务器证书没验证通过，登录已停止。检查电脑时间和代理设置。",
            "登录没有通过。核对一下账号、密码和登录区域。",
            "准备登录数据时出了问题，请重试。",
            "没能确认校园登录地址，已经停止连接。检查一下当前网络。",
            "校园服务没有允许继续登录，过一会儿再试。",
            "登录没有完成：fixture-old-reason"
        ]
        let historical = previousFailures.map { ConnectionEvent(date: Date(), message: $0) }
        try JSONEncoder().encode(historical).write(to: url)
        let historicalBytes = try Data(contentsOf: url)
        let decodedHistorical = try EventLogFile.read(from: url)
        let afterClassification = try Data(contentsOf: url)
        check(decodedHistorical.map(\.message) == previousFailures && afterClassification == historicalBytes,
              "Shared wording changes preserve every original historical message and saved byte")
        for event in decodedHistorical {
            check(RecordMessageClassifier.isFailure(event.message, legacyMarkers: []),
                  "Previous reviewed failure wording remains visible after loading a saved event log")
        }
        let uncertainBodies = [
            "登录已经提交，结果还没确认。先做一次完整检测。",
            "登录请求已发出，但暂未收到结果。请先做一次完整检测。",
            "暂时无法确认登录结果，请先做一次完整检测。",
            SharedCopy.t("portalError.authenticationUncertain")
        ]
        let legacyMarkers = ["登录没有完成", "登录未完成", "手动检测未通过", "拒绝了登录", "外网检测暂未通过",
            "网络路径暂不可用", "认证服务暂时连不上", "登录被拒绝", "网络连接暂不可用", "校园网登录服务暂时连不上", "登录未通过"]
        for body in uncertainBodies {
            for prefix in ["", "登录没有完成：", "登录未完成："] {
                check(!RecordMessageClassifier.isFailure(prefix + body, legacyMarkers: legacyMarkers),
                      "Every historical unknown-result body outranks historical and current failure prefixes")
            }
        }
        let recoveryMessages = [
            "Apple、Microsoft 两项基础检测均通过，已解除登录结果不明这一项暂停。",
            SharedCopy.t("event.uncertainSettled")
        ]
        for message in recoveryMessages {
            check(!RecordMessageClassifier.isFailure(message, legacyMarkers: legacyMarkers),
                  "Historical and current uncertain-result recovery remain outside the failures-only list")
        }
        let mixedMessages = [previousFailures[0], uncertainBodies[0], recoveryMessages[0],
            "登录未完成：" + uncertainBodies[1], SharedCopy.t("portalError.rejected"), recoveryMessages[1]]
        let mixedEvents = mixedMessages.enumerated().map { index, message in
            ConnectionEvent(date: Date(timeIntervalSince1970: 1_800_000_000 - Double(index)), message: message)
        }
        try JSONEncoder().encode(mixedEvents).write(to: url)
        let mixedBytes = try Data(contentsOf: url)
        let loadedMixed = try EventLogFile.read(from: url)
        let failures = loadedMixed.filter { RecordMessageClassifier.isFailure($0.message, legacyMarkers: legacyMarkers) }
        let afterMixedBytes = try Data(contentsOf: url)
        check(failures.map(\.id) == [mixedEvents[0].id, mixedEvents[4].id],
              "A mixed saved log filters only confirmed failures while retaining their original order and IDs")
        check(loadedMixed.map(\.message) == mixedMessages && loadedMixed.map(\.id) == mixedEvents.map(\.id)
              && afterMixedBytes == mixedBytes,
              "Classifying mixed historical logs never rewrites messages, identifiers, order or original file bytes")
        let referenceEvent = mixedEvents[0]
        let equalEvent = ConnectionEvent(id: referenceEvent.id, date: referenceEvent.date, message: referenceEvent.message)
        check(referenceEvent == equalEvent && [referenceEvent] == [equalEvent],
              "Unchanged event value inputs compare equal without changing their stored representation")
        check(referenceEvent != ConnectionEvent(date: referenceEvent.date, message: referenceEvent.message),
              "A new event identifier invalidates the records value input")
        check(referenceEvent != ConnectionEvent(id: referenceEvent.id, date: referenceEvent.date.addingTimeInterval(1), message: referenceEvent.message),
              "An event time change invalidates the records value input")
        check(referenceEvent != ConnectionEvent(id: referenceEvent.id, date: referenceEvent.date, message: referenceEvent.message + " fixture"),
              "An event text change invalidates the records value input")
        let eventEncoder = JSONEncoder(); eventEncoder.outputFormatting = [.sortedKeys]
        check(try eventEncoder.encode(referenceEvent) == eventEncoder.encode(equalEvent),
              "Value equality adds no fields or changes to event Codable output")
        let authentication = DiagnosticAuthentication(area: "teaching", stage: "submission", path: "/cgi-bin/srun_portal",
            httpStatus: 200, acID: "12", result: "uncertain", code: "conflictingStatus", serverCode: "unknown", saved: true)
        check(authentication.valid, "Reviewed typed authentication identifiers are valid")
        let authenticationEvent = ConnectionEvent(date: Date(), message: "anonymous authentication", authentication: authentication)
        try JSONEncoder().encode([authenticationEvent]).write(to: url)
        let restoredAuthentication = try EventLogFile.read(from: url)
        check(restoredAuthentication.first?.authentication == authentication, "Typed authentication survives save and restore")
        check(restoredAuthentication.first?.context == nil && restoredAuthentication.first?.kind == nil,
              "Authentication details do not invent a failure context")
        let encodedAuthentication = try eventEncoder.encode(authenticationEvent)
        let exportedAuthentication = String(decoding: encodedAuthentication, as: UTF8.self)
        check(!exportedAuthentication.contains("account") && !exportedAuthentication.contains("password")
              && !exportedAuthentication.contains("clientIP") && !exportedAuthentication.contains("challenge="),
              "Typed authentication contains no credentials or request material")
        for change: (inout DiagnosticAuthentication) -> Void in [
            { $0.area = "automatic" }, { $0.stage = "raw stage" }, { $0.path = "https://example.invalid/?secret=fake" },
            { $0.httpStatus = 99 }, { $0.httpStatus = 600 }, { $0.acID = "0" }, { $0.acID = "012" },
            { $0.acID = "123456789" }, { $0.acID = "192.0.2.1" }, { $0.result = "raw server message" },
            { $0.code = "raw error text" }, { $0.serverCode = "arbitrary server text" },
            { $0.stage = "homepage" }, { $0.area = "dormitory" }
        ] {
            var invalid = authentication; change(&invalid)
            check(!invalid.valid, "Unknown or inconsistent authentication identifiers are rejected")
            let invalidEvent = ConnectionEvent(date: Date(), message: "retained event", authentication: invalid)
            check(invalidEvent.authentication == nil, "Invalid optional authentication never reaches export")
        }
        var rawAuthentication = try JSONSerialization.jsonObject(with: encodedAuthentication) as! [String: Any]
        var malformedAuthentication = rawAuthentication["authentication"] as! [String: Any]
        malformedAuthentication["result"] = "raw unknown result"; rawAuthentication["authentication"] = malformedAuthentication
        let decodedInvalid = try JSONDecoder().decode(ConnectionEvent.self, from: JSONSerialization.data(withJSONObject: rawAuthentication))
        check(decodedInvalid.authentication == nil && decodedInvalid.message == authenticationEvent.message,
              "Invalid authentication in an old event is discarded without losing the existing event")
        let eventTime = Date()
        let failure = DiagnosticFailureContext(kind: "connection", result: "operationFailed", startedAt: eventTime,
            collectedAt: eventTime, operation: "manual", monitoringMode: "recover", area: "teaching", environment: .unknown,
            status: "uncertain", campusVerification: .notChecked,
            recovery: DiagnosticRecovery(state: .paused, readOnly: false, paused: true, networkAvailable: false,
                networkRemembered: false, sessionTrusted: false, holdReasons: [.uncertain], cooldownRemainingSeconds: 90))
        let combined = ConnectionEvent(date: eventTime, message: "anonymous combined event", context: failure, authentication: authentication)
        let decodedCombined = try JSONDecoder().decode(ConnectionEvent.self, from: eventEncoder.encode(combined))
        check(decodedCombined.context == failure && decodedCombined.authentication == authentication && decodedCombined.kind == "failure",
              "Independent failure and authentication contexts may coexist without changing either")
        check(RecordMessageClassifier.isFailure(decodedCombined, legacyMarkers: []),
              "Structured failure events remain visible in failures-only even when their short message is not a legacy failure phrase")
        check(!RecordMessageClassifier.isFailure(authenticationEvent, legacyMarkers: []),
              "An authentication-only uncertain result does not invent a confirmed failure context")
        for (stage, path) in [("homepage", "/srun_portal_pc"), ("challenge", "/cgi-bin/get_challenge")] {
            var value = authentication; value.stage = stage; value.path = path
            check(value.valid, "Reviewed SRun stages retain their exact bounded path")
        }
        var dormitoryAuthentication = authentication
        dormitoryAuthentication.area = "dormitory"; dormitoryAuthentication.path = "/eportal/portal/login"
        dormitoryAuthentication.acID = nil; dormitoryAuthentication.httpStatus = nil; dormitoryAuthentication.serverCode = nil
        check(dormitoryAuthentication.valid, "Dormitory submissions allow unavailable optional metadata")
        print("\(passed) disposable-file event-store assertions passed. No formal user data or network was accessed.")
    }
}
