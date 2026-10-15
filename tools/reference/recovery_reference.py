#!/usr/bin/env python3
"""Executable decision-level reference for SZUConnect authentication recovery.

Uses scripted fake transport responses to model one recovery round: connectivity,
admission, campus discovery, area selection, at most one credential submission
and bounded verification. No network requests are made.

shared/fixtures/auto-recovery-scenarios.json drives this model and the Swift/C++ tests.
legacy=False checks the shared contract; legacy=True supplies a comparison model
with global holds. Regression cases must distinguish the models; preserved
protections must pass under both.
"""
from dataclasses import dataclass, field
from typing import Optional

COOLDOWN = 90.0
PRE_SUBMISSION = {"unavailable", "challengeRefused", "security"}
PRIORITY = {"userPaused": 0, "security": 1, "uncertain": 2, "credentials": 3}


@dataclass
class Hold:
    reason: str               # credentials | security | uncertain | userPaused
    area: Optional[str]       # teaching | dormitory | None (all areas)
    generation: Optional[int] # saved-credential generation or None (any)
    epoch: Optional[int]      # network epoch or None (survives path changes)
    origin: str               # explicitArea | candidate | global | user
    rejection: Optional[str] = None

    def applies(self, area, generation, epoch):
        return ((self.area is None or self.area == area)
                and (self.generation is None or self.generation == generation)
                and (self.epoch is None or self.epoch == epoch))

    def summary(self):
        scope = "network" if self.epoch is not None else "sticky"
        return {"reason": self.reason, "area": self.area, "scope": scope}


@dataclass
class Policy:
    holds: list = field(default_factory=list)
    candidate_counts: dict = field(default_factory=dict)
    not_before: Optional[float] = None
    consecutive_failures: int = 0
    legacy_block: Optional[str] = None  # legacy mode only

    # ----- legacy helpers -------------------------------------------------
    def legacy_blocked(self):
        return self.legacy_block is not None

    # ----- scoped holds ---------------------------------------------------
    def blocking(self, area, generation, epoch):
        found = [h for h in self.holds if h.applies(area, generation, epoch)]
        return sorted(found, key=lambda h: PRIORITY[h.reason])[0] if found else None

    def global_hold(self, generation, epoch):
        found = [h for h in self.holds if h.area is None and h.applies("teaching", generation, epoch)]
        return sorted(found, key=lambda h: PRIORITY[h.reason])[0] if found else None

    def add(self, hold):
        self.holds = [h for h in self.holds if not (h.reason == hold.reason and h.area == hold.area
                                                   and h.generation == hold.generation and h.epoch == hold.epoch)]
        self.holds.append(hold)

    def record_failure(self, kind_choice, area, generation, epoch, outcome):
        """Returns the created hold (or None). outcome is a scripted token."""
        if outcome in ("unavailable", "challengeRefused", "cancelled"):
            return None
        if outcome == "security":
            h = Hold("security", None, None, None, "global")
        elif outcome == "uncertain":
            h = Hold("uncertain", None, None, None, "global")
        elif outcome == "invalidCredentials":
            if generation is None:
                return None
            h = Hold("credentials", None, generation, None, "explicitArea", "invalidCredentials")
        elif outcome.startswith("rejected:"):
            kind = outcome.split(":", 1)[1]
            if generation is None:
                return None  # an unsaved draft never speaks for the saved account
            if kind_choice == "automatic" and kind != "password":
                key = (area, generation)
                self.candidate_counts[key] = self.candidate_counts.get(key, 0) + 1
                # An area mismatch describes the network, never the credentials: it must not outlive the network.
                sticky = kind != "areaMismatch" and self.candidate_counts[key] >= 2
                h = Hold("credentials", area, generation, None if sticky else epoch, "candidate", kind)
            else:
                h = Hold("credentials", area, generation, None,
                         "candidate" if kind_choice == "automatic" else "explicitArea", kind)
        else:
            raise ValueError(outcome)
        self.add(h)
        return h

    def record_success(self, area, generation, epoch):
        if generation is None:
            return []
        released, kept = [], []
        for h in self.holds:
            same_generation = h.generation == generation
            release = (
                (same_generation and h.area == area and h.reason != "userPaused")
                or (h.area is None and h.generation is None and h.reason in ("security", "uncertain"))
                or (same_generation and h.origin == "candidate" and h.epoch == epoch)
                or (same_generation and h.area is None and h.reason == "credentials")
            )
            (released if release else kept).append(h)
        self.holds = kept
        for h in released:
            if h.area:
                self.candidate_counts.pop((h.area, generation), None)
        self.candidate_counts.pop((area, generation), None)
        return released

    def settle_uncertain(self):
        before = len(self.holds)
        self.holds = [h for h in self.holds if h.reason != "uncertain"]
        return before != len(self.holds)

    def resume(self):
        self.holds = []
        self.candidate_counts = {}
        self.consecutive_failures = 0

    def credentials_changed(self, new_generation):
        self.holds = [h for h in self.holds if h.generation is None or h.generation == new_generation]
        self.candidate_counts = {k: v for k, v in self.candidate_counts.items() if k[1] == new_generation}

    def path_changed(self, new_epoch):
        self.holds = [h for h in self.holds if h.epoch is None or h.epoch == new_epoch]


def plan(kind, choice, discovery, trusted, policy, generation, epoch):
    """Decision for one round. Returns (decision, areas).

    decision: submit | unconfirmed | needsTrust | paused
    """
    teaching_ok = discovery.get("teaching") == "confirmed"
    dorm_ok = discovery.get("dormitory") == "confirmed"
    manual = kind == "manual"

    def blocked(area):
        return None if manual else policy.blocking(area, generation, epoch)

    if choice == "teaching":
        if not manual and not teaching_ok:
            return "unconfirmed", []
        return ("paused", []) if blocked("teaching") else ("submit", ["teaching"])
    if choice == "dormitory":
        if not manual and not (teaching_ok and dorm_ok):
            return "unconfirmed", []
        if blocked("dormitory"):
            return "paused", []
        if not manual and not trusted:
            return "needsTrust", []
        return "submit", ["dormitory"]
    # automatic: identify first, then one submission.
    if not teaching_ok:
        return "unconfirmed", []
    if dorm_ok:
        if not blocked("dormitory"):
            if manual or trusted:
                return "submit", ["dormitory"]
            return "needsTrust", []
        if not blocked("teaching"):
            return "submit", ["teaching"]
        return "paused", []
    if not blocked("teaching"):
        return "submit", ["teaching"]
    return "paused", []


class Fake:
    def __init__(self):
        self.probes = []
        self.default_probe = "offline"
        self.discovery = [{"teaching": "unconfirmed", "dormitory": "notChecked"}]
        self.attempts = {}
        self.submissions = []
        self.attempt_log = []
        self.discoveries = 0

    def probe(self):
        return self.probes.pop(0) if self.probes else self.default_probe

    def discover(self, area):
        self.discoveries += 1
        value = self.discovery[0] if len(self.discovery) == 1 else self.discovery.pop(0)
        if area == "teaching":
            return {"teaching": value.get("teaching"), "dormitory": "notChecked"}
        return dict(value)

    def attempt(self, area, account):
        outcome = self.attempts.get(area, "unavailable")
        self.attempt_log.append([area, outcome])
        if outcome not in PRE_SUBMISSION:
            self.submissions.append([area, account])
        return outcome


class Model:
    """Decision-level model for the Swift AppModel and C++ ConnectionEngine."""

    def __init__(self, initial, legacy=False):
        self.legacy = legacy
        self.fake = Fake()
        self.policy = Policy()
        self.time = 0.0
        self.choice = initial.get("area", "automatic")
        self.mode = initial.get("mode", "recover")
        self.saved = tuple(initial["saved"]) if initial.get("saved") else None
        self.draft = tuple(initial.get("draft", initial.get("saved") or ("", "")))
        self.generation = 1 if self.saved else 0
        self.trusted = bool(initial.get("trusted", False))
        self.epoch = 1
        self.online = False
        self.last_area = None
        self.last_discovery = None
        self.status = "idle"

    # ---- helpers ---------------------------------------------------------
    def saved_generation_for(self, creds):
        return self.generation if self.saved and creds == self.saved else None

    def paused(self):
        if self.legacy:
            return self.policy.legacy_blocked()
        if self.mode != "recover" or not self.saved:
            return False
        gen = self.generation
        if self.policy.global_hold(gen, self.epoch):
            return True
        if self.choice == "automatic":
            return (self.policy.blocking("teaching", gen, self.epoch) is not None
                    and self.policy.blocking("dormitory", gen, self.epoch) is not None)
        return self.policy.blocking(self.choice, gen, self.epoch) is not None

    def auto_login_state(self):
        if self.mode != "recover":
            return "off"
        if not self.saved:
            return "needsAccount"
        if self.paused():
            return "paused"
        if not self.legacy and not self.trusted and (
                self.choice == "dormitory" or (self.choice == "automatic" and self.last_discovery is not None
                                               and self.last_discovery.get("dormitory") == "confirmed")):
            return "needsTrust"
        if not self.online and self.policy.not_before is not None and self.policy.not_before > self.time:
            return "coolingDown"
        if not self.legacy and self.choice == "automatic" and any(
                h.area is not None for h in self.policy.holds if h.generation == self.generation):
            return "limited"
        return "ready"

    def set_online(self):
        self.online = True
        self.policy.consecutive_failures = 0
        if self.legacy:
            if self.policy.legacy_block == "uncertain":
                self.policy.legacy_block = None
        else:
            self.policy.settle_uncertain()

    # ---- one round -------------------------------------------------------
    def round(self, kind):
        credentials = self.saved if kind == "automatic" else self.draft
        if kind == "automatic" and self.mode == "manual":
            return
        report = self.fake.probe()
        if report == "online":
            self.set_online()
            self.status = "online"
            return
        self.online = False
        if kind == "automatic":
            self.policy.consecutive_failures = min(2, self.policy.consecutive_failures + 1)
        may_auth = self.mode == "recover" if kind == "automatic" else self.mode != "observe"
        if not may_auth:
            self.status = "offline"
            return
        if not credentials or not credentials[0]:
            self.status = "needsAccount"
            return
        gen = self.saved_generation_for(credentials)
        if kind == "automatic":
            if self.legacy:
                if self.policy.legacy_blocked():
                    self.status = "paused"
                    return
            elif self.paused():
                # Paused for the chosen area(s): keep read-only checks, skip discovery.
                self.status = "paused"
                return
            if self.policy.not_before is not None and self.policy.not_before > self.time:
                self.status = "coolingDown"
                return
            if self.policy.consecutive_failures < 2:
                self.status = "confirming"
                return
        if self.legacy:
            self._legacy_submit(kind, credentials, gen)
        else:
            self._submit(kind, credentials, gen)

    def _verify(self):
        for _ in range(3):
            if self.fake.probe() == "online":
                self.set_online()
                return True
        return False

    def _succeeded(self, kind, area, gen):
        self.last_area = area
        if self.legacy:
            if gen is not None:
                self.policy.legacy_block = None
        else:
            self.policy.record_success(area, gen, self.epoch)
        self.policy.not_before = self.time + COOLDOWN
        self.policy.consecutive_failures = 0
        if kind == "manual":
            # AppModel.authorizeAfterManualSuccess: the driver always has a known identity.
            self.trusted = True
        self.status = "online" if self._verify() else "accepted"

    def _submit(self, kind, credentials, gen):
        needs_discovery = kind == "automatic" or self.choice == "automatic"
        discovery = self.fake.discover(self.choice) if needs_discovery else {
            "teaching": "confirmed", "dormitory": "confirmed"}
        if needs_discovery:
            self.last_discovery = dict(discovery)
        decision, areas = plan(kind, self.choice, discovery, self.trusted, self.policy, gen, self.epoch)
        if decision != "submit":
            self.status = decision
            return
        area = areas[0]
        self.policy.not_before = self.time + COOLDOWN
        outcome = self.fake.attempt(area, credentials[0])
        if (outcome in ("unavailable", "challengeRefused") and area == "teaching"
                and self.choice == "automatic"):
            again = self.fake.discover("dormitory")
            if again.get("teaching") == "confirmed" and again.get("dormitory") == "confirmed":
                if kind == "manual" or self.trusted:
                    blocked = None if kind == "manual" else self.policy.blocking("dormitory", gen, self.epoch)
                    if not blocked:
                        area = "dormitory"
                        outcome = self.fake.attempt(area, credentials[0])
        if outcome == "success":
            self._succeeded(kind, area, gen)
            return
        self.policy.record_failure(self.choice if self.choice == "automatic" else "pinned", area, gen,
                                   self.epoch, outcome)
        self.status = "failed:" + outcome

    def _legacy_submit(self, kind, credentials, gen):
        # Comparison model: global holds and teaching-first manual selection.
        if kind == "automatic":
            d = self.fake.discover(self.choice)
            teaching_ok = d.get("teaching") == "confirmed"
            dorm_ok = d.get("dormitory") == "confirmed"
            if not teaching_ok:
                confirmed = None
            elif self.choice == "teaching":
                confirmed = "teaching"
            elif dorm_ok:
                confirmed = "dormitory"
            else:
                confirmed = "teaching" if self.choice == "automatic" else None
            if confirmed is None or (confirmed == "dormitory" and not self.trusted):
                self.status = "unconfirmed" if confirmed is None else "needsTrust"
                return
            self.policy.not_before = self.time + COOLDOWN
            outcome = self.fake.attempt(confirmed, credentials[0])
            area = confirmed
        else:
            self.policy.not_before = self.time + COOLDOWN
            if self.choice != "automatic":
                area = self.choice
                outcome = self.fake.attempt(area, credentials[0])
            else:
                area = "teaching"
                outcome = self.fake.attempt(area, credentials[0])
                # PortalWire.shouldFallback: only pre-submission availability failures.
                if outcome == "unavailable":
                    d = self.fake.discover("dormitory")
                    if d.get("teaching") == "confirmed" and d.get("dormitory") == "confirmed":
                        area = "dormitory"
                        outcome = self.fake.attempt(area, credentials[0])
        if outcome == "success":
            self._succeeded(kind, area, gen)
            return
        mapped = {"challengeRefused": "credentials", "invalidCredentials": "credentials",
                  "security": "security", "uncertain": "uncertain"}
        if outcome.startswith("rejected:"):
            self.policy.legacy_block = "credentials"
        elif outcome in mapped:
            self.policy.legacy_block = mapped[outcome]
        self.status = "failed:" + outcome

    # ---- user and system events -----------------------------------------
    def apply(self, step):
        action = step["do"]
        if action == "configure":
            if "probes" in step:
                self.fake.probes.extend(step["probes"])
            if "defaultProbe" in step:
                self.fake.default_probe = step["defaultProbe"]
            if "discovery" in step:
                value = step["discovery"]
                self.fake.discovery = [dict(v) for v in value] if isinstance(value, list) else [dict(value)]
            if "attempts" in step:
                self.fake.attempts = dict(step["attempts"])
        elif action == "automaticRound":
            for _ in range(step.get("count", 1)):
                self.round("automatic")
        elif action == "manualConnect":
            self.round("manual")
        elif action == "advance":
            self.time += step["seconds"]
        elif action == "trust":
            self.trusted = True
        elif action == "revokeTrust":
            self.trusted = False
        elif action in ("pathChange", "sleepWake"):
            self.epoch += 1
            self.trusted = False
            self.last_discovery = None
            self.online = False
            self.policy.consecutive_failures = 0
            if not self.legacy:
                self.policy.path_changed(self.epoch)
        elif action == "saveCredentials":
            creds = (step["account"], step["password"])
            self.draft = creds
            if creds != self.saved:
                self.saved = creds
                self.generation += 1
                if not self.legacy:
                    self.policy.credentials_changed(self.generation)
        elif action == "editDraft":
            self.draft = (step["account"], step["password"])
        elif action == "revertDraft":
            self.draft = self.saved
        elif action == "resume":
            if self.legacy:
                self.policy.legacy_block = None
            else:
                self.policy.resume()
        elif action == "pause":
            if not self.legacy:
                self.policy.add(Hold("userPaused", None, None, None, "user"))
        elif action == "setMode":
            self.mode = step["mode"]
        elif action == "setArea":
            self.choice = step["area"]
        elif action == "expect":
            pass
        else:
            raise ValueError("unknown step " + action)

    def observe(self):
        holds = sorted((h.summary() for h in self.policy.holds),
                       key=lambda s: (s["reason"], s["area"] or "", s["scope"]))
        if self.legacy and self.policy.legacy_block:
            holds = [{"reason": self.policy.legacy_block, "area": None, "scope": "sticky"}]
        return {
            "submissions": [list(s) for s in self.fake.submissions],
            "attempts": [list(a) for a in self.fake.attempt_log],
            "paused": self.paused(),
            "mode": self.mode,
            "online": self.online,
            "holds": holds,
            "autoLoginState": self.auto_login_state(),
            "lastSuccessArea": self.last_area,
            "discoveries": self.fake.discoveries,
        }


def check_expectation(observed, expected):
    problems = []
    for key, value in expected.items():
        if key == "holdCount":
            if len(observed["holds"]) != value:
                problems.append(f"holdCount {len(observed['holds'])} != {value}")
            continue
        if key == "submissionCount":
            if len(observed["submissions"]) != value:
                problems.append(f"submissionCount {len(observed['submissions'])} != {value}")
            continue
        if key == "holds":
            want = sorted(value, key=lambda s: (s["reason"], s.get("area") or "", s["scope"]))
            got = observed["holds"]
            if want != got:
                problems.append(f"holds {got} != {want}")
            continue
        if observed.get(key) != value:
            problems.append(f"{key} {observed.get(key)!r} != {value!r}")
    return problems


def run_scenario(scenario, legacy=False):
    model = Model(scenario["initial"], legacy=legacy)
    failures = []
    for index, step in enumerate(scenario["steps"]):
        model.apply(step)
        if step["do"] == "expect":
            for problem in check_expectation(model.observe(), step["expect"]):
                failures.append(f"step {index}: {problem}")
    return failures
