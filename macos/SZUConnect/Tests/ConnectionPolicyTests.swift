import Foundation

@main
struct ConnectionPolicyTests {
    static func main() {
        var passed = 0
        func check(_ condition: @autoclosure () -> Bool, _ name: String) {
            guard condition() else { fatalError("FAIL: \(name)") }
            passed += 1
            print("PASS: \(name)")
        }

        var protectedCooldown = ConnectionPolicy(foreground: true)
        let cooldownStart = Date(timeIntervalSince1970: 1_700_000_000)
        protectedCooldown.authenticationAttemptStarted(now: cooldownStart)
        protectedCooldown.suspendAuthentication(.uncertain)
        protectedCooldown.resumeAuthentication()
        check(protectedCooldown.authenticationNotBefore == cooldownStart.addingTimeInterval(90),
              "Resuming recovery never erases an active credential-submission cooldown")

        do {
            let wired = PhysicalAuthenticationScope(kind: "ethernet", interface: "en12")
            let wifi = PhysicalAuthenticationScope(kind: "wifi", interface: "en0")
            let later = cooldownStart.addingTimeInterval(10)
            var scoped = ConnectionPolicy(foreground: true)
            scoped.authenticationAttemptStarted(now: cooldownStart)
            check(scoped.cooldownRemaining(now: later, scope: wifi) == 80, "Unknown submission retains global cooldown after a physical switch")
            scoped.authenticationAccepted(now: cooldownStart, scope: wired)
            check(scoped.cooldownRemaining(now: later, scope: wifi) == 80, "Successful response alone cannot relax another interface's cooldown")
            scoped.verifyAcceptedScope(wifi, now: later)
            check(scoped.cooldownRemaining(now: later, scope: wifi) == 80, "Another interface cannot settle the original request")
            scoped.verifyAcceptedScope(wired, now: later)
            check(scoped.cooldownRemaining(now: later, scope: wifi) == nil, "Complete bound wired success permits fresh Wi-Fi verification")
            check(scoped.cooldownRemaining(now: later, scope: wired) == 80, "Same physical interface retains its original ninety-second deadline")
            let old = scoped.cooldownState
            scoped.authenticationAttemptStarted(now: later); scoped.restoreCooldownState(old)
            check(scoped.cooldownRemaining(now: later, scope: wifi) == nil && scoped.cooldownRemaining(now: later, scope: wired) == 80,
                  "Definitely-unsent preflight restores both original deadline and settled scope")
            scoped.authenticationAttemptStarted(now: later)
            scoped.authenticationAccepted(now: later, scope: wifi); scoped.verifyAcceptedScope(wifi, now: later)
            let again = later.addingTimeInterval(10)
            check(scoped.cooldownRemaining(now: again, scope: wired) == 70, "A to B to A retains A's earlier deadline")
            check(scoped.cooldownRemaining(now: again, scope: wifi) == 80, "B retains its independent ninety-second deadline")
            scoped.authenticationAttemptStarted(now: again)
            check(scoped.cooldownRemaining(now: again, scope: wired) == 90, "Cancelled or unknown new submission is globally protected")
            scoped.restoreRecovery(RecoveryJournal(generation: 1, holds: [], notBefore: again.addingTimeInterval(42), pendingSubmission: false))
            check(scoped.cooldownRemaining(now: again, scope: wifi) == 42, "Durable restoration cannot reuse RAM-only successful scope proof")
        }

        var policy = ConnectionPolicy(foreground: true)
        check(!policy.automaticEnabled, "Automatic reconnection defaults off")
        check(!policy.canRunAutomatic, "No automatic loop at startup")
        check(policy.begin(.manual) == nil, "No operation without a usable path")
        policy.available = true
        check(policy.begin(.automatic) == nil, "Usable path does not opt into automatic authentication")
        let first = policy.begin(.manual)!
        check(policy.begin(.manual) == nil, "Repeated clicks before task execution reserve only one operation")
        policy.automaticEnabled = true
        check(policy.begin(.automatic) == nil, "Automatic tick cannot overlap manual operation")
        policy.cancel(.automatic)
        check(policy.owns(first), "Disabling automatic reconnect preserves manual operation")
        check(policy.finish(first), "Owner can finish operation")
        check(policy.active == nil, "Completion releases admission")

        let automatic = policy.begin(.automatic)!
        check(policy.begin(.manual) == nil, "Manual operation cannot overlap automatic operation")
        policy.automaticEnabled = false
        policy.cancel(.automatic)
        check(!policy.owns(automatic), "Disabling automatic reconnect invalidates in-flight completion")
        let replacement = policy.begin(.manual)!
        check(!policy.finish(automatic), "Stale completion cannot release replacement operation")
        check(policy.owns(replacement), "Replacement stays busy after stale task cleanup")

        // A confirmed path replacement invalidates all work, even when the next path is usable.
        policy.resetForPathChange()
        policy.available = true
        check(!policy.owns(replacement), "Usable-to-usable path change invalidates old probe result")
        let newPath = policy.begin(.manual)!
        check(!policy.finish(replacement), "Old path task cleanup cannot affect new path operation")
        policy.resetForPathChange()
        policy.available = false
        check(!policy.owns(newPath), "Path loss invalidates active authentication")
        check(policy.begin(.manual) == nil, "Path loss blocks manual admission")
        policy.automaticEnabled = true
        check(!policy.canRunAutomatic, "Path loss blocks automatic loop")

        policy.available = true
        let beforeBackground = policy.begin(.automatic)!
        policy.foreground = false
        policy.cancel()
        check(!policy.owns(beforeBackground), "Background transition invalidates outstanding completion")
        check(!policy.canRunAutomatic, "iOS background suspends automatic loop")
        check(policy.begin(.manual) == nil, "Queued manual intent in background is rejected")
        check(policy.begin(.automatic) == nil, "Automatic admission in background is rejected")
        policy.foreground = true
        check(policy.canRunAutomatic, "Foreground resumes explicitly enabled automatic loop")
        let resumed = policy.begin(.automatic)!
        check(!policy.finish(beforeBackground), "Pre-background completion cannot clear resumed operation")
        check(policy.owns(resumed), "Resumed operation retains ownership")
        policy.finish(resumed)
        policy.automaticEnabled = false
        policy.foreground = false
        policy.foreground = true
        check(!policy.canRunAutomatic, "Foreground does not enable paused automatic reconnect")

        check(policy.retryDelay == 30, "Initial polling delay is 30 seconds")
        for delay in [60, 120, 240, 300, 300] {
            policy.failed()
            check(policy.retryDelay == delay, "Failure backoff reaches and stays at \(delay) seconds")
        }
        for _ in 0..<10_000 { policy.failed() }
        check(policy.retryDelay == 300 && policy.failures == 4, "Saturated failure counter cannot overflow over long runtimes")
        policy.resetFailures()
        check(policy.retryDelay == 30, "Success or explicit re-enable resets backoff")
        policy.foreground = true
        policy.automaticEnabled = true
        let guarded = policy.begin(.automatic)!
        policy.observeProbe(online: false)
        check(!policy.mayAuthenticateAutomatically(guarded, campusConfirmed: true), "One failed round cannot authenticate")
        policy.observeProbe(online: false)
        check(!policy.mayAuthenticateAutomatically(guarded, campusConfirmed: false), "Unknown campus cannot authenticate after two failures")
        check(policy.mayAuthenticateAutomatically(guarded, campusConfirmed: true), "Two failures and confirmed campus admit owned automatic operation")
        policy.observeProbe(online: true)
        check(policy.consecutiveProbeFailures == 0 && !policy.mayAuthenticateAutomatically(guarded, campusConfirmed: true), "Any successful probe resets authentication threshold")
        policy.observeProbe(online: false)
        policy.observeProbe(online: false)
        policy.cancel()
        check(!policy.mayAuthenticateAutomatically(guarded, campusConfirmed: true), "Cancelled operation can never authenticate despite earlier evidence")
        check(policy.consecutiveProbeFailures == 0, "Operation cancellation discards old probe failures")
        let untrusted = policy.begin(.automatic)!
        policy.observeProbe(online: false)
        policy.observeProbe(online: false)
        check(!policy.mayAuthenticateAutomatically(untrusted, campusConfirmed: true, requiresLocalAuthorization: true), "Reachable dormitory portal never authorizes an unknown local network")
        policy.finish(untrusted)
        policy.authorizeCurrentNetwork()
        check(policy.currentNetworkAuthorized && policy.active == nil, "Explicit network permission does not begin an operation or authentication")
        let trusted = policy.begin(.automatic)!
        check(policy.mayAuthenticateAutomatically(trusted, campusConfirmed: true, requiresLocalAuthorization: true), "Explicit path permission admits dormitory only with previous safeguards")
        policy.resetForPathChange()
        check(!policy.currentNetworkAuthorized, "Network change clears in-memory permission")
        let manualSuccess = policy.begin(.manual)!
        policy.authorizeAfterManualSuccess(manualSuccess)
        check(policy.currentNetworkAuthorized, "Owned successful manual authentication authorizes current path")
        policy.cancel(.automatic)
        check(policy.currentNetworkAuthorized, "Enabling automatic checks does not discard an explicitly granted path permission")
        policy.automaticEnabled = false
        policy.cancel(.automatic)
        check(policy.currentNetworkAuthorized, "Disabling automatic reconnection preserves granted network permission")
        policy.cancel()
        check(policy.currentNetworkAuthorized, "Cancelling any round preserves granted network permission")
        policy.revokeAuthorization()
        check(!policy.currentNetworkAuthorized, "Explicit revocation clears granted network permission")
        policy.authorizeAfterManualSuccess(manualSuccess)
        check(!policy.currentNetworkAuthorized, "Stale manual success cannot authorize a replacement path")
        var cadence = ConnectionPolicy(foreground: true)
        cadence.available = true
        cadence.automaticEnabled = true
        check(cadence.nextCheckDelay() == 30, "Fresh path begins with moderate detection interval")
        cadence.observeProbe(online: true)
        check(cadence.nextCheckDelay() == 30, "First successful round remains responsive")
        cadence.observeProbe(online: true)
        check(cadence.nextCheckDelay() == 45, "Two stable rounds reduce polling load")
        cadence.observeProbe(online: true)
        check(cadence.nextCheckDelay() == 60, "Three stable rounds use steady-state interval")
        check(cadence.nextCheckDelay(constrained: true) == 120, "Constrained networks use a longer healthy interval")
        cadence.observeProbe(online: false)
        cadence.failed()
        check(cadence.nextCheckDelay() == 5, "First failed round confirms quickly despite prior stability")
        cadence.observeProbe(online: false)
        cadence.failed()
        check(cadence.nextCheckDelay() == 120, "Second failed round uses bounded exponential backoff")
        cadence.observeProbe(online: true)
        check(cadence.nextCheckDelay() == 30 && cadence.failures == 0,
              "A successful probe clears failure backoff")

        let epoch = Date(timeIntervalSince1970: 1_000_000)
        for reason in [ConnectionPolicy.AuthenticationBlock.credentials, .security, .uncertain] {
            var blocked = ConnectionPolicy(foreground: true)
            blocked.available = true
            blocked.automaticEnabled = true
            blocked.suspendAuthentication(reason)
            blocked.observeProbe(online: false)
            blocked.observeProbe(online: false)
            let beforeToggle = blocked.begin(.automatic)!
            check(!blocked.mayAuthenticateAutomatically(beforeToggle, campusConfirmed: true),
                  "\(reason) block denies authentication despite all other admission evidence")
            blocked.automaticEnabled = false
            blocked.cancel(.automatic)
            blocked.automaticEnabled = true
            blocked.resetForPathChange()
            blocked.foreground = false
            blocked.cancel()
            blocked.foreground = true
            check(blocked.authenticationBlock == reason,
                  "\(reason) protection survives automatic toggle, path change and suspension")
            check(blocked.nextCheckDelay() == 60 && blocked.nextCheckDelay(constrained: true) == 120,
                  "\(reason) suspension continues read-only checks at reduced frequency")
            blocked.observeProbe(online: true)
            check(blocked.authenticationBlock == reason, "Online probe does not silently release \(reason) rejection")
            blocked.resumeAuthentication()
            check(blocked.authenticationBlock == nil && blocked.consecutiveProbeFailures == 0,
                  "Explicit resume releases \(reason) but requires fresh failure evidence")
        }

        var alternateAccount = ConnectionPolicy(foreground: true)
        alternateAccount.suspendAuthentication(.credentials)
        alternateAccount.authenticationAccepted(now: epoch, clearBlock: false)
        check(alternateAccount.authenticationBlock == .credentials,
              "Success with alternate unsaved credentials preserves saved-account rejection protection")
        alternateAccount.authenticationAccepted(now: epoch, clearBlock: true)
        check(alternateAccount.authenticationBlock == nil,
              "Success with the saved credentials can release their rejection protection")

        var propagation = ConnectionPolicy(foreground: true)
        propagation.available = true
        propagation.automaticEnabled = true
        let propagationOperation = propagation.begin(.automatic)!
        propagation.authenticationAccepted(now: epoch)
        check(propagation.consecutiveProbeFailures == 0, "Accepted authentication clears preceding probe evidence")
        propagation.observeProbe(online: false)
        propagation.observeProbe(online: false)
        check(!propagation.mayAuthenticateAutomatically(propagationOperation, campusConfirmed: true,
                                                        now: epoch.addingTimeInterval(89)),
              "Propagation grace period forbids repeat submission before 90 seconds")
        check(propagation.mayAuthenticateAutomatically(propagationOperation, campusConfirmed: true,
                                                       now: epoch.addingTimeInterval(90)),
              "Grace period expires precisely at the 90-second boundary")
        check(propagation.nextCheckDelay(now: epoch.addingTimeInterval(89)) == 15,
              "Accepted authentication uses brief read-only rechecks")
        propagation.resetForPathChange()
        check(propagation.authenticationNotBefore == epoch.addingTimeInterval(90),
              "Path changes preserve an already accepted authentication grace period")
        let afterPath = propagation.begin(.automatic)!
        propagation.observeProbe(online: false)
        propagation.observeProbe(online: false)
        check(!propagation.mayAuthenticateAutomatically(afterPath, campusConfirmed: true,
                                                        now: epoch.addingTimeInterval(89)),
              "New path cannot bypass active propagation grace period")
        propagation.resumeAuthentication()
        check(propagation.authenticationNotBefore == epoch.addingTimeInterval(90) && propagation.consecutiveProbeFailures == 0,
              "Explicit recovery keeps the submission grace period while discarding old evidence")

        var cancelledSubmission = ConnectionPolicy(foreground: true)
        cancelledSubmission.available = true
        cancelledSubmission.automaticEnabled = true
        cancelledSubmission.authenticationAttemptStarted(now: epoch)
        cancelledSubmission.cancel()
        cancelledSubmission.resetForPathChange()
        check(cancelledSubmission.authenticationNotBefore == epoch.addingTimeInterval(90),
              "An interrupted credential submission retains cooldown when server outcome is unknown")
        let afterCancellation = cancelledSubmission.begin(.automatic)!
        cancelledSubmission.observeProbe(online: false)
        cancelledSubmission.observeProbe(online: false)
        check(!cancelledSubmission.mayAuthenticateAutomatically(afterCancellation, campusConfirmed: true,
                                                               now: epoch.addingTimeInterval(10)),
              "Fresh probes cannot immediately repeat a cancelled submission")
        // Only verified connectivity settles an uncertain submission, without clearing other holds.
        for reason in [ConnectionPolicy.AuthenticationBlock.credentials, .security] {
            var kept = ConnectionPolicy(foreground: true)
            kept.suspendAuthentication(reason)
            let released = kept.settleUncertainAuthentication()
            check(!released && kept.authenticationBlock == reason, "Verified connectivity never releases a \(reason) block")
        }
        var settled = ConnectionPolicy(foreground: true)
        settled.authenticationAttemptStarted(now: epoch)
        settled.suspendAuthentication(.uncertain)
        let settledOnce = settled.settleUncertainAuthentication()
        check(settledOnce && settled.authenticationBlock == nil && settled.authenticationNotBefore == epoch.addingTimeInterval(90),
              "Verified connectivity settles an uncertain submission but keeps its cooldown")
        let settledTwice = settled.settleUncertainAuthentication()
        check(!settledTwice, "Settling is idempotent")
        print("\(passed) offline connection-policy assertions passed. No AppModel, URLSession, Keychain or NWPathMonitor was instantiated.")
    }
}
