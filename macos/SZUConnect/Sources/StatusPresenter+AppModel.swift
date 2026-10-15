import Foundation

// UI wording comes from the same resources as Windows. Probe data stays unchanged.
extension WebsiteProbeStrategy {
    var sharedDetail: String { SharedCopy.t("website.strategy.\(rawValue).detail") }
}

extension WebsiteProbeResult {
    private var sharedStateKey: String? {
        if state == .invalidInput { return nil }
        if state == .httpStatus && statusCode == 403 { return "forbidden" }
        return state.rawValue
    }

    var sharedTitle: String {
        guard let key = sharedStateKey else { return title }
        return SharedCopy.t("website.state.\(key).title")
    }

    var sharedDetail: String {
        guard let key = sharedStateKey else { return detail }
        let resource = "website.state.\(key).detail"
        let template = SharedCopy.t(resource)
        guard template.contains("{code}") else { return template }
        guard let code = statusCode else { return detail }
        return SharedCopy.f(resource, ["code": String(code)])
    }
}

// AppModel → shared status vocabulary (kept apart so contract tests need no AppModel).
extension AppModel {
    var connectionKindTitle: String {
        guard networkAvailable else { return SharedCopy.t("env.access.none") }
        let labels = interfaceSummary.components(separatedBy: " · ")
        let wifi = labels.filter { $0.hasPrefix("Wi-Fi") }.count
        let wired = labels.filter { $0.hasPrefix("有线") }.count
        let key = wifi + wired > 1 ? "multiple" : wifi == 1 ? "wifi" : wired == 1 ? "ethernet" : "unknown"
        return SharedCopy.t("env.access." + key)
    }

    /// shared vocabulary: off | needsAccount | paused | needsTrust | coolingDown | limited | ready | awaitingProof
    var autoLoginKey: String {
        guard monitoringMode == .recover else { return "off" }
        guard hasSavedCredentials else { return "needsAccount" }
        if pausedForAutomatic { return "paused" }
        let dormitorySeen = lastDiscovery?.dormitory == .confirmed
            || dormitoryMemoryLevel == .medium || dormitoryMemoryLevel == .strong
        if !currentNetworkAuthorized && (area == .dormitory || (area == .automatic && dormitorySeen)) {
            switch memoryVerdictNow {
            case .authorized: break
            case .awaitingProof:
                // An online remembered network needs no user action. Fresh proof is still
                // required by CampusPlan if a later outage actually needs a login.
                if isOnline && lastDiscovery == nil { break }
                return "awaitingProof"
            case .notRemembered, .conflict: return "needsTrust"
            }
        }
        if !isOnline && cooldownRemaining != nil { return "coolingDown" }
        if area == .automatic && holds.contains(where: { $0.area != nil }) { return "limited" }
        return "ready"
    }

    var readinessInput: ReadinessInput {
        ReadinessInput(mode: String(describing: monitoringMode), readOnly: sessionReadOnly, hasSaved: hasSavedCredentials,
                       unsavedChanges: hasSavedCredentials && hasUnsavedCredentialChanges, autoLogin: autoLoginKey, hold: pauseReasonKey,
                       sessionTrusted: currentNetworkAuthorized, remembered: networkRemembered, memoryConflict: memoryLevel == .conflict,
                       networkIdentified: physicalNetworkIdentified, sessionOnlyNetwork: sessionOnlyTrust,
                       campusVerification: campusVerificationState)
    }
    var readiness: Readiness { ReadinessPresenter.make(readinessInput) }

    /// Why a remembered network is not authorizing yet: "" | unconfirmed | tunnel | conflict.
    var proofWaitKey: String {
        switch autoLoginKey {
        case "awaitingProof": return tunnelActive ? "tunnel" : "unconfirmed"
        case "needsTrust": return memoryLevel == .conflict ? "conflict" : ""
        default: return ""
        }
    }

    var pauseReasonKey: String? {
        guard pausedForAutomatic else { return nil }
        return holds.min { $0.reason < $1.reason }?.reason.key
    }

    var presentationStatus: String {
        if busy && !isForegroundBusy && isOnline { return "online" }
        if engineStatus == "sleeping" { return "sleeping" }
        if !networkAvailable && !busy { return "noNetwork" }
        if busy && !StatusPresenter.busyStatuses.contains(engineStatus) { return "checking" }
        if !busy && StatusPresenter.busyStatuses.contains(engineStatus) { return isOnline ? "online" : "idle" }
        // A forced login can be rejected after this network's complete probe passed.
        // Keep the connectivity headline while the independent hold banner explains the rejection.
        if engineStatus == "rejected", isOnline, lastInternetCheck?.base.online == true { return "online" }
        if engineStatus == "idle" && isOnline { return "online" }
        return engineStatus
    }

    var statusInput: StatusInput {
        var input = StatusInput()
        input.status = presentationStatus
        input.diagnosis = lastDiagnosis.map { String(describing: $0) }
        input.probeMode = (lastProbeMode ?? probeMode) == .complete ? "complete" : "quick"
        input.passedCount = lastProbePassedCount
        input.mode = String(describing: monitoringMode)
        input.readOnly = sessionReadOnly
        input.hasDraft = hasCompleteDraft
        input.autoLogin = autoLoginKey
        input.hold = pauseReasonKey
        input.offerForcedLogin = offerForcedLogin
        return input
    }

    var systemPathIsLeading: Bool {
        systemPathCanLead && systemConnectivity?.online == true && lastInternetCheck?.base.online != true
    }

    var systemPathIsChecking: Bool {
        systemPathCanLead && systemConnectivity == nil && systemConnectivityChecking && lastInternetCheck?.base.online != true
    }

    var displayedConnectivity: ConnectivityReport? {
        systemPathIsLeading ? systemConnectivity : lastInternetCheck?.base
    }

    var presentation: StatusPresentation {
        var value = StatusPresenter.present(statusInput)
        if autoLoginKey == "awaitingProof" && networkRemembered {
            value.chip = "auto.knownRecovery.chip"
            value.chipTone = "info"
        }
        if presentationStatus == "confirming" && prioritizingCampusRecovery {
            value.detail = "status.confirming.rapidDetail"
        }
        #if os(macOS)
        if presentationStatus == "unconfirmed" && tunnelActive {
            value.detail = "status.unconfirmed.physicalDetail"
        }
        #endif
        if !canTrustCurrentNetwork {
            if value.primary == "trust" { value.primary = "check"; value.secondary = "network" }
            if value.secondary == "trust" { value.secondary = nil }
            if presentationStatus == "needsTrust" {
                value.headline = "banner.networkUnidentified.title"
                value.detail = "banner.networkUnidentified.detail"
                value.tone = "warning"
            }
        }
        if value.banner?.key == "banner.needsTrust" {
            if !canTrustCurrentNetwork {
                value.banner = StatusBanner(key: "banner.networkUnidentified", tone: "warning", action: "network")
            } else if sessionOnlyTrust {
                value.banner = StatusBanner(key: "banner.sessionTrustOnly", tone: "warning", action: "trust")
            }
        }
        if engineStatus == "rejected", presentationStatus == "online", value.banner == nil {
            value.banner = StatusBanner(key: "banner.loginRejected", tone: "warning", action: "account")
        }
        if systemPathIsLeading || systemPathIsChecking {
            value.headline = systemPathIsLeading ? "env.systemOnline.title" : "env.systemChecking.title"
            value.detail = systemPathIsLeading ? "env.systemOnline.detail" : "env.systemChecking.detail"
            value.tone = systemPathIsLeading ? "success" : "busy"
            value.primary = systemPathIsLeading ? "check" : "cancel"
            value.secondary = "network"
            return value
        }
        guard !isForegroundBusy, ["online", "offline", "idle"].contains(presentationStatus) else { return value }
        if websiteConfigurationChanged {
            value.headline = "internet.pending.title"; value.detail = "internet.pending.detail"; value.tone = "info"
            return value
        }
        guard let report = lastInternetCheck else {
            if networkAvailable && !busy && !pausedForAutomatic && holds.isEmpty && cooldownRemaining == nil {
                value.headline = "internet.pending.title"; value.detail = "internet.pending.detail"; value.tone = "info"
                value.primary = "check"; value.secondary = "network"
            }
            return value
        }
        value.headline = "internet.\(report.verdict.rawValue).title"
        value.detail = "internet.\(report.verdict.rawValue).detail"
        switch report.verdict {
        case .allPassed, .basicPassed: value.tone = "success"
        case .partial: value.tone = "warning"
        case .unavailable: value.tone = "warning"
        case .cancelled: value.tone = "neutral"
        }
        if report.hasOnlyWebsiteIssues {
            value.headline = "internet.websiteIssues.title"
            value.detail = "internet.websiteIssues.detail"
            if report.hasRecoveryEvidence && canAuthenticate {
                value.primary = hasCompleteDraft ? "partialLogin" : "account"
                value.secondary = "check"
                value.detail = partialRecoveryAttempted ? "internet.partial.afterAttempt" : "internet.partial.canLogin"
            } else {
                value.primary = "check"
                value.secondary = canOfferForcedLogin ? "forceLogin" : "network"
            }
        } else if report.verdict == .partial && report.base.online {
            value.headline = "dashboard.partialWithBase"
            if canAuthenticate {
                value.primary = hasCompleteDraft ? "partialLogin" : "account"
                value.secondary = "check"
                value.detail = partialRecoveryAttempted ? "internet.partial.afterAttempt" : "internet.partial.canLogin"
            }
        }
        if report.base.mode == .quick && report.selectedCount > 0 { value.detail = "internet.quickSkips" }
        return value
    }
}
