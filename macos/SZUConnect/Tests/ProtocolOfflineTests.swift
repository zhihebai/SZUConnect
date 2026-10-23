import Foundation

@main
struct ProtocolOfflineTests {
    static func main() throws {
        var passed = 0
        func check(_ condition: @autoclosure () -> Bool, _ name: String) {
            guard condition() else { fatalError("FAIL: \(name)") }
            passed += 1
            print("PASS: \(name)")
        }
        check(!PortalWire.dormitorySuccess(["result": 0, "msg": "AC认证失败"]),
              "An explicit ePortal failure cannot be overridden by legacy result zero")

        for value: [String: Any] in [
            ["error": "denied", "st": 1], ["error": "ok", "st": 0],
            ["error": "ok", "res": "failed"], ["res": "ok", "st": false]
        ] { check(!PortalWire.teachingSuccess(value), "Conflicting SRun status fields never imply success") }
        check(PortalWire.teachingSuccess(["error": "", "st": true]), "Empty optional error retains the upstream boolean status contract")
        for message in ["认证失败", "用户名错误", "未认证成功", "正在等待认证成功", "稍后认证成功", "AC authentication failed", "invalid password", "waiting for login"] {
            check(!PortalWire.dormitorySuccess(["result": 0, "msg": message]),
                  "Failure or pending wording overrides result zero")
        }
        check(!PortalWire.dormitorySuccess(["result": 1, "msg": "请确认认证成功"]),
              "An arbitrary success suffix is not a positive acknowledgement")
        check(PortalWire.dormitorySuccess(["result": 1, "msg": "Portal协议认证成功！"]),
              "DrCOM result one with known success wording remains supported")
        func parse(_ text: String) throws -> [String: Any] { try PortalWire.object(from: Data(text.utf8)) }
        let payload = try PortalWire.payload(credentials: Credentials(account: "student", password: "pass+&word"),
                                             challenge: "0123456789abcdef0123456789abcdef", ip: "10.2.3.4", acID: "1")
        // Golden values independently generated with Python struct/base64/hmac/hashlib.
        check(payload.info == "{SRBX1}hIv/DExCB4eKvW9Wraa1oNYV1tQ46tgselGVxjee4cNQSomKI1JXP+bNsnF1/869dYzg8jy5J6yyt0fgY3Xojj3daok6ruiYbrve5PNdLC9UxdNQOZUt+TeuYPIuqGr6j++TBS==", "SRun XEncode and custom Base64 golden payload")
        check(payload.hash == "88cc168aaf538f35a446bf5101fb6ce4", "HMAC-MD5 golden payload")
        check(payload.checksum == "7e5547e0de24a1c287076e4a94fb7d959dd1ddc8", "SHA1 checksum golden payload")
        let plain = try parse(#"{"error":"ok"}"#)
        check(PortalWire.teachingSuccess(plain), "Teaching plain JSON success")
        let jsonp = try parse(#"_({"st":true});"#)
        check(PortalWire.teachingSuccess(jsonp), "Teaching JSONP boolean success")
        let dorm = try parse(#"jsonpReturn({"result":0})"#)
        check(PortalWire.dormitorySuccess(dorm), "Dormitory result zero success")
        let existing = try parse(#"{"result":1,"msg":"认证成功"}"#)
        check(PortalWire.dormitorySuccess(existing), "Dormitory repeated-login success phrase")
        check(!PortalWire.dormitorySuccess(["msg": "未认证成功"]), "Reject negated success message")
        check(!PortalWire.dormitorySuccess(["result": false]), "Reject boolean false as result zero")
        check(!PortalWire.dormitorySuccess([:]), "Missing result does not succeed")
        check(!PortalWire.teachingSuccess(["st": 2]), "Teaching rejected status")
        for object in [[:], ["error": ""], ["error": "ok", "res": "failed"],
                       ["st": 2], ["error": 1], ["error": "pending"]] as [[String: Any]] {
            if case .uncertain = PortalWire.teachingVerdict(object) { check(true, "Ambiguous teaching acknowledgement stays uncertain") }
            else { check(false, "Ambiguous teaching acknowledgement stays uncertain") }
        }
        for field in ["suc_msg", "error_msg"] {
            let object: [String: Any] = ["error": "ok", "res": "ok", "st": 1, field: "ip_already_online_error"]
            if case .uncertain(.existingSession) = PortalWire.teachingVerdict(object) {
                check(true, "Existing IP session overrides all positive credential fields")
            } else { check(false, "Existing IP session overrides all positive credential fields") }
            check(!PortalWire.teachingSuccess(object), "Legacy success helper cannot bypass the existing-session verdict")
        }
        if case .rejected = PortalWire.teachingVerdict(["error": "login_error"]) {
            check(true, "Explicit teaching refusal remains rejected")
        } else { check(false, "Explicit teaching refusal remains rejected") }
        if case .accepted = PortalWire.teachingVerdict(["st": true]) {
            check(true, "Legacy boolean positive status remains accepted")
        } else { check(false, "Legacy boolean positive status remains accepted") }
        if case .rejected = PortalWire.teachingVerdict(["st": false]) {
            check(true, "Legacy boolean negative status remains rejected")
        } else { check(false, "Legacy boolean negative status remains rejected") }
        for code in [AuthDiagnostic.ServerCode.e2553, .e2531, .e2833, .nasIpError, .acIdError, .alreadyOnline] {
            check(PortalWire.serverCode(["error_msg": "code=\(code.rawValue.lowercased())"]) == code,
                  "Known server token is normalized to a fixed diagnostic code")
        }
        check(PortalWire.serverCode(["error": "ok"]) == nil,
              "Positive status does not invent a server error code")
        check(PortalWire.serverCode(["error_msg": "E2553evil account=student password=secret ip=10.2.3.4"]) == .unknown,
              "Unknown server text becomes a fixed marker without copying input")
        for unsafe in ["0", "0000", "6\n", "6x", "student", "password=secret", "10.2.3.4", "https://example.invalid/"] {
            check(AuthDiagnostic(stage: .homepage, path: .homepage, acID: unsafe, code: .invalidStatus).acID == nil,
                  "Diagnostic controller accepts no credentials, addresses or malformed digits")
        }
        check(AuthDiagnostic(stage: .homepage, path: .homepage, acID: "0006", code: .accepted).acID == "6",
              "Diagnostic controller normalizes valid positive ASCII digits")
        for object in [[:], ["result": 1], ["result": 2, "msg": "认证成功"],
                       ["result": false, "msg": "认证成功"], ["result": 0, "msg": "认证失败"],
                       ["result": 0, "msg": "等待认证"]] as [[String: Any]] {
            if case .uncertain = PortalWire.dormitoryVerdict(object) { check(true, "Ambiguous dormitory acknowledgement stays uncertain") }
            else { check(false, "Ambiguous dormitory acknowledgement stays uncertain") }
        }
        if case .rejected = PortalWire.dormitoryVerdict(["result": 1, "msg": "认证失败"]) {
            check(true, "Explicit dormitory refusal remains rejected")
        } else { check(false, "Explicit dormitory refusal remains rejected") }
        for invalid in ["evil({\"result\":0})", "_({\"result\":0});alert(1)", "<html>login</html>", "[]"] {
            check((try? parse(invalid)) == nil, "Reject non-protocol JSONP or payload")
        }
        for (html, expected) in [
            ("location='/srun?ac_id=27'", "27"),
            ("var ac_id = 18;", "18"),
            ("var acid   : \"6\";", "6"),
            (#"{"acid":6,"ac_id":"6"}"#, "6"),
            (#"<input type="hidden" name="acid" value="6">"#, "6"),
            (#"{"ac_id":8}"#, "8"),
            (#"{"ac_id":"18"}"#, "18"),
            (#"<input type="hidden" name="ac_id" value="27">"#, "27"),
            (#"<input value='18' name='ac_id' type='hidden'>"#, "18"),
            ("ac_id=18&other=1; var ac_id = 18;", "18"),
            ("ac_id=99999999", "99999999"),
            ("data-ac_id=18; ac_id=8", "8")
        ] {
            let value = try PortalWire.acID(in: Data(html.utf8))
            check(value == expected, "Controller discovery accepts supported forms and ignores prefixed field names")
        }
        for html in [
            "<html></html>",
            "other_ac_id=99; xac_id=33; ac_id_suffix=22",
            "data-ac_id=18; x-ac_id=27; data-acid=6; xacid=7",
            "ac_id=18&ac_id=27", "acid:6; ac_id=1", #"{"acid":6,"ac_id":1}"#,
            #"{"ac_id":18}<input name='acid' value='27'>"#,
            "ac_id=0", "ac_id=-1", "ac_id=100000000", "ac_id=18evil",
            "acid:0", "acid:-1", "acid:100000000", "acid:6evil", "acid:",
            "ac_id=1.5", "ac_id=", #"{"ac_id":false}"#,
            #"<input name="ac_id" value="">"#
        ] {
            do {
                _ = try PortalWire.acID(in: Data(html.utf8))
                fatalError("FAIL: explicit invalid or conflicting controller values must stop discovery")
            } catch PortalError.protocolConflict {
                check(true, "Invalid or conflicting controller values cannot silently select a fallback")
            }
        }
        do {
            _ = try PortalWire.acID(in: Data(repeating: 65, count: 1_048_577))
            fatalError("FAIL: oversized controller page must be rejected")
        } catch PortalError.responseTooLarge {
            check(true, "Controller discovery is bounded before parsing")
        }
        do {
            _ = try PortalWire.acID(in: Data([0xff, 0xfe]))
            fatalError("FAIL: invalid text must not become the default controller")
        } catch PortalError.response {
            check(true, "Invalid controller page encoding is not a missing field")
        }

        let challengeToken = "0123456789abcdef0123456789abcdef"
        for status: [String: Any] in [["error": "ok"], ["res": "ok"], ["error": "ok", "res": "ok"]] {
            var object = status
            object["challenge"] = challengeToken
            object["client_ip"] = "10.2.3.4"
            let challenge = try PortalWire.challenge(from: object)
            check(challenge.challenge == challengeToken && challenge.ip == "10.2.3.4",
                  "Challenge accepts explicit compatible success fields without altering payload values")
        }
        for status: [String: Any] in [
            ["error": "ok", "res": "failed"], ["error": "denied", "res": "ok"],
            ["error": 0, "res": "ok"], ["error": "ok", "res": true],
            ["error": "", "res": "ok"], [:]
        ] {
            var object = status
            object["challenge"] = challengeToken
            object["client_ip"] = "10.2.3.4"
            do {
                _ = try PortalWire.challenge(from: object)
                fatalError("FAIL: invalid challenge status must be rejected before authentication")
            } catch PortalError.protocolConflict {
                check(true, "Contradictory, mistyped or missing challenge status cannot authorize submission")
            }
        }
        for invalid: [String: Any] in [
            ["error": "ok", "client_ip": "10.2.3.4"],
            ["error": "ok", "challenge": "", "client_ip": "10.2.3.4"],
            ["error": "ok", "challenge": String(repeating: "x", count: 4097), "client_ip": "10.2.3.4"],
            ["error": "ok", "challenge": challengeToken, "client_ip": ""],
            ["error": "ok", "challenge": challengeToken, "client_ip": String(repeating: "1", count: 65)]
        ] {
            check((try? PortalWire.challenge(from: invalid)) == nil,
                  "Challenge token and client address retain their required bounded values")
        }
        for address in ["https://net.szu.edu.cn/cgi-bin/srun_portal", "http://172.30.255.42:801/eportal/portal/login",
                        "https://www.apple.com/library/test/success.html"] {
            check(PortalWire.isAllowed(URL(string: address)!), "Allow fixed endpoint")
        }
        for address in ["http://net.szu.edu.cn/cgi-bin/srun_portal", "https://net.szu.edu.cn.evil.test/cgi-bin/srun_portal",
                        "http://172.30.255.42:80/eportal/portal/login", "https://user@net.szu.edu.cn/cgi-bin/srun_portal",
                        "https://www.apple.com/library/test/success.html?password=secret", "https://net.szu.edu.cn/unknown"] {
            check(!PortalWire.isAllowed(URL(string: address)!), "Reject unapproved origin, route, or credentials")
        }
        let special = "p+&= ?#%中文"
        let encoded = try PortalWire.makeURL(base: "http://172.30.255.42:801", path: "/eportal/portal/login",
                                             parameters: [URLQueryItem(name: "user_password", value: special)])
        let decoded = URLComponents(url: encoded, resolvingAgainstBaseURL: false)!.queryItems!
        check(decoded.count == 1 && decoded[0].value == special, "Credential special characters round-trip without query injection")
        check(encoded.absoluteString.contains("%2B") && encoded.absoluteString.contains("%26") && encoded.absoluteString.contains("%23"), "Form parsers receive encoded plus, ampersand and fragment")
        check(LoginAttempt(error: PortalError.network).submissionNotStarted, "Availability failure can use alternate campus area")
        check(LoginAttempt(error: PortalError.timeout).submissionNotStarted, "Pre-submit timeout allows verified alternate discovery")
        check(LoginAttempt(error: PortalError.dns).submissionNotStarted, "Pre-submit DNS failure allows verified alternate discovery")
        check(LoginAttempt(error: PortalError.response).submissionNotStarted, "Unrecognized portal can use alternate campus area")
        for error in [PortalError.rejected, .tls, .cryptography, .invalidCredentials, .forbiddenTarget, .authenticationUncertain, .redirect, .responseTooLarge, .protocolConflict] {
            check(!LoginAttempt(error: error).submissionNotStarted, "Rejected, TLS or local security failure cannot downgrade")
        }
        check(!LoginAttempt(error: CancellationError()).submissionNotStarted, "Cancellation cannot trigger alternate authentication")
        check(PortalWire.isTLSError(URLError(.serverCertificateUntrusted)), "Certificate error classified as TLS failure")
        check(PortalWire.isTLSError(URLError(.secureConnectionFailed)), "TLS handshake failure cannot downgrade")
        check(!PortalWire.isTLSError(URLError(.timedOut)), "Ordinary timeout remains availability failure")
        let expected = Data("<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>\n".utf8)
        check(PortalWire.isConnectivitySuccess(expected, status: 200), "Exact HTTPS connectivity payload")
        check(!PortalWire.isConnectivitySuccess(expected, status: 302), "Redirect cannot indicate online")
        check(!PortalWire.isConnectivitySuccess(Data("Success".utf8), status: 200), "Partial page cannot indicate online")
        check(!PortalWire.isConnectivitySuccess(Data("<html>Please login</html>".utf8), status: 200), "Captive page cannot indicate online")
        check(ConnectivityReport(apple: true, microsoft: false).online, "Apple alone establishes connectivity")
        check(ConnectivityReport(apple: false, microsoft: true).online, "Microsoft alone establishes connectivity")
        check(!ConnectivityReport(apple: false, microsoft: false).online, "Only failure of both endpoints indicates offline")
        let cancelledReport = ConnectivityReport(appleResult: .init(state: .success, elapsed: 0.1), microsoftResult: .init(state: .cancelled, elapsed: 0.1))
        check(cancelledReport.summary.contains("已取消") && !cancelledReport.summary.contains("未通过"), "Early cancellation is not labelled endpoint failure")
        check(PortalWire.isMicrosoftConnectivitySuccess(Data("Microsoft Connect Test".utf8), status: 200), "Microsoft exact success payload")
        check(!PortalWire.isMicrosoftConnectivitySuccess(Data("Microsoft Connect Test".utf8), status: 302), "Microsoft redirect rejected")
        check(!PortalWire.isMicrosoftConnectivitySuccess(Data("<html>Microsoft Connect Test</html>".utf8), status: 200), "Microsoft partial payload rejected")
        for address in ["http://www.msftconnecttest.com/connecttest.txt", "http://172.30.255.42:801/"] {
            check(PortalWire.isAllowed(URL(string: address)!), "Allow fixed credential-free probe endpoint")
        }
        for address in ["http://www.msftconnecttest.com/connecttest.txt?username=x", "http://www.msftconnecttest.com/other", "http://www.msftconnecttest.com.evil.test/connecttest.txt", "http://172.30.255.42:801/?password=x", "https://net.szu.edu.cn/?username=x"] {
            check(!PortalWire.isAllowed(URL(string: address)!), "Probe allowlist rejects alternate route, host or query")
        }
        let teachingURL = URL(string: "https://net.szu.edu.cn/")!
        let dormURL = URL(string: "http://172.30.255.42:801/")!
        check(PortalWire.isCampusHomepage(Data("深圳大学 srun login".utf8), status: 200, url: teachingURL), "Exact teaching origin with campus and portal markers accepted")
        check(PortalWire.isCampusHomepage(Data("深圳大学 drcom 登录".utf8), status: 200, url: dormURL), "Dorm portal requires both campus and DrCOM markers")
        for html in ["<html>OK</html>", "深圳大学", "drcom login", "srun login", "hotel login"] {
            check(!PortalWire.isCampusHomepage(Data(html.utf8), status: 200, url: teachingURL), "Generic or incomplete portal identity rejected")
            check(!PortalWire.isCampusHomepage(Data(html.utf8), status: 200, url: dormURL), "Private IP alone never confirms campus identity")
        }
        check(!PortalWire.isCampusHomepage(Data("深圳大学 srun login".utf8), status: 302, url: teachingURL), "Portal redirect is not automatic authentication evidence")
        check(!PortalWire.isCampusHomepage(Data("深圳大学 srun login".utf8), status: 200, url: URL(string: "https://net.szu.edu.cn.evil.test/")!), "Lookalike campus host rejected")
        let verifiedTeachingURL = URL(string: "https://net.szu.edu.cn/srun_portal_pc")!
        let verifiedDormURL = URL(string: "http://172.30.255.42:801/eportal")!
        let eportal = Data("<html><title>EPortal</title></html>".utf8)
        check(PortalWire.isCampusHomepage(Data("深圳大学 srun login".utf8), status: 200, url: verifiedTeachingURL), "Verified fixed SRun page establishes campus identity")
        check(!PortalWire.isCampusHomepage(eportal, status: 200, url: verifiedTeachingURL), "EPortal alone cannot establish campus identity")
        check(PortalWire.isDormitoryHomepage(eportal, status: 200, url: verifiedDormURL, campusConfirmed: true), "Dormitory area accepted only with separate HTTPS campus identity")
        check(!PortalWire.isDormitoryHomepage(eportal, status: 200, url: verifiedDormURL, campusConfirmed: false), "Standalone EPortal cannot authorize dormitory authentication")
        check(!PortalWire.isDormitoryHomepage(eportal, status: 302, url: verifiedDormURL, campusConfirmed: true), "Dormitory redirect rejected even with campus identity")
        check(!PortalWire.isDormitoryHomepage(Data("EPortal".utf8), status: 200, url: verifiedDormURL, campusConfirmed: true), "Dormitory requires exact title marker")
        check(!PortalWire.isDormitoryHomepage(eportal, status: 200, url: URL(string: "http://172.30.255.43:801/eportal")!, campusConfirmed: true), "Different private host cannot establish dormitory area")
        for address in ["https://net.szu.edu.cn/srun_portal_pc?username=x", "http://172.30.255.42:801/eportal?username=x", "https://net.szu.edu.cn/index_1.html"] {
            check(!PortalWire.isAllowed(URL(string: address)!), "New discovery endpoints remain query-free and fixed")
        }

        // Dormitory success parsing accepts supported variants and rejects negation.
        for phrase in ["认证成功", "认证成功！", "认证成功!", "Portal协议认证成功！", " 认证成功。 "] {
            check(PortalWire.dormitorySuccess(["result": 1, "msg": phrase]), "Known ePortal success wording accepted: \(phrase)")
        }
        for phrase in ["未认证成功", "认证成功失败", "认证失败", "非本网段认证成功", "此账号无权限认证成功", "账号不存在", "",
                       "认证成功了吗？不", String(repeating: "长", count: 30) + "认证成功"] {
            check(!PortalWire.dormitorySuccess(["result": 1, "msg": phrase]), "Negated, failed or unusual wording rejected: \(phrase)")
        }

        // Only availability failures may repeat a credential-free read.
        for error in [PortalError.timeout, .network, .dns] {
            check(PortalWire.isTransient(error), "Transient availability failure: \(error)")
        }
        for error in [PortalError.tls, .redirect, .rejected, .httpStatus, .response, .responseTooLarge,
                      .authenticationUncertain, .invalidCredentials, .forbiddenTarget, .cryptography, .protocolConflict] {
            check(!PortalWire.isTransient(error), "Security, protocol and credential failures are never retried: \(error)")
        }

        // Probe diagnosis is explanatory and never grants authentication permission.
        func diagnosis(_ apple: ConnectivityEndpointState, _ microsoft: ConnectivityEndpointState) -> ConnectivityDiagnosis {
            ConnectivityReport(appleResult: ConnectivityEndpointResult(state: apple, elapsed: 0),
                               microsoftResult: ConnectivityEndpointResult(state: microsoft, elapsed: 0)).diagnosis
        }
        check(diagnosis(.success, .notCompleted) == .online, "One passing endpoint reads as online")
        check(diagnosis(.tls, .redirect) == .portal, "A captive redirect outranks the HTTPS failure it causes")
        check(diagnosis(.unexpectedResponse, .timeout) == .response, "Unexpected content is not proof of a captive login page")
        check(diagnosis(.tls, .timeout) == .security, "Certificate failure without a redirect reads as interception")
        check(diagnosis(.dns, .timeout) == .dns, "Resolution failure is named before a timeout")
        check(diagnosis(.timeout, .network) == .timeout, "Timeout outranks a generic network failure")
        check(diagnosis(.network, .network) == .unreachable, "Plain network failure reads as unreachable")
        check(diagnosis(.cancelled, .notCompleted) == .incomplete, "Cancelled rounds claim nothing")
        for value in [ConnectivityDiagnosis.online, .portal, .response, .security, .dns, .timeout, .unreachable, .incomplete] {
            check(!value.title.isEmpty && value.explanation.hasSuffix("。"), "Diagnosis \(value.rawValue) has a plain title and sentence")
        }
        check(CampusArea.allCases.allSatisfy { !$0.symbol.isEmpty && !$0.explanation.isEmpty }, "Every campus area explains itself")
        print("\(passed) offline protocol assertions passed. No URLSession instance or network request was created.")
    }
}
