#!/usr/bin/env python3
"""Reference for the first-screen status presenter shared by both apps.

Maps engine facts to connectivity status, available actions and automatic-login state.
Output values are copy keys (shared/copy.zh-CN.json),
tone tokens (shared/design-tokens.json) and action identifiers; each platform
renders them with its own controls.  Implemented by Swift StatusPresenter and
C++ szu::status::present; both are tested against shared/status-fixtures.json.
"""
BUSY = {"checking", "discovering", "loggingIn", "verifying"}
AUTH_ACTIONS = {"login", "resume", "trust", "forceLogin"}


def _can_login(inp):
    return inp["hasDraft"] and not inp["readOnly"] and inp["mode"] != "observe"


def _offline_actions(inp):
    if inp["readOnly"] or inp["mode"] == "observe":
        return "check", None
    if inp["hasDraft"]:
        return "login", "check"
    return "account", "check"


def present(inp):
    s = inp["status"]
    tone, primary, secondary = "neutral", None, None
    headline, detail = f"status.{s}.headline", f"status.{s}.detail"
    if s == "noNetwork":
        tone, secondary = "warning", "network"
    elif s == "sleeping":
        tone = "neutral"
    elif s in BUSY:
        tone, primary = "busy", "cancel"
    elif s == "online":
        tone, primary = "success", "check"
        if inp["probeMode"] == "complete":
            detail = "status.online.complete" if inp["passedCount"] >= 2 else "status.online.partial"
        else:
            detail = "status.online.quick"
        if inp.get("offerForcedLogin"):
            secondary = "forceLogin"
    elif s == "accepted":
        tone, primary = "info", "check"
    elif s == "rejected":
        tone, primary = "danger", "account"
        secondary = "login" if _can_login(inp) else None
    elif s == "uncertain":
        tone, primary = "warning", "check"
    elif s == "security":
        tone, primary, secondary = "danger", "network", "check"
    elif s == "unavailable":
        tone = "warning"
        primary, secondary = _offline_actions(inp)
    elif s == "unconfirmed":
        tone, primary, secondary = "warning", "network", "check"
    elif s == "needsTrust":
        tone, primary, secondary = "warning", "trust", "check"
    elif s == "paused":
        tone, primary = "warning", "resume"
        secondary = "login" if _can_login(inp) else None
        detail = f"hold.{inp['hold']}.detail" if inp.get("hold") else "status.paused.detail"
    elif s == "coolingDown":
        tone, primary = "info", "check"
    elif s == "confirming":
        tone = "info"
        primary, secondary = _offline_actions(inp)
    elif s == "needsAccount":
        tone, primary, secondary = "warning", "account", "check"
    elif s == "offline":
        tone = "warning"
        diagnosis = inp.get("diagnosis")
        if diagnosis == "portal":
            headline = "status.portal.headline"
        detail = f"diagnosis.{diagnosis}.detail" if diagnosis else "status.offline.detail"
        primary, secondary = _offline_actions(inp)
    elif s == "cancelled":
        tone, primary = "neutral", "check"
    elif s == "idle":
        tone, primary = "neutral", "check"
        secondary = "login" if _can_login(inp) else None
    else:
        raise ValueError(s)
    if inp["readOnly"] or inp["mode"] == "observe":
        if primary in AUTH_ACTIONS:
            primary = "check" if s != "noNetwork" and s != "sleeping" else None
        if secondary in AUTH_ACTIONS or secondary == primary:
            secondary = None
    if inp["readOnly"]:
        chip, chip_tone = "auto.readOnly.chip", "neutral"
    elif inp["mode"] != "recover":
        chip, chip_tone = f"auto.{inp['mode']}.chip", "neutral"
    else:
        state = inp["autoLogin"]
        chip = f"auto.{state}.chip"
        chip_tone = {"needsAccount": "warning", "paused": "danger", "needsTrust": "warning",
                     "coolingDown": "info", "limited": "warning", "ready": "success",
                     "awaitingProof": "warning"}[state]
    banner = None
    if inp["readOnly"]:
        banner = {"key": "banner.readOnly", "tone": "info", "action": None}
    elif inp["mode"] == "recover" and inp["autoLogin"] == "paused":
        hold = inp.get("hold") or "credentials"
        banner = {"key": f"hold.{hold}", "action": "resume",
                  "tone": {"security": "danger", "credentials": "danger", "uncertain": "warning"}.get(hold, "neutral")}
    elif inp["mode"] == "recover" and inp["autoLogin"] == "needsTrust":
        banner = {"key": "banner.needsTrust", "tone": "warning", "action": "trust"}
    elif inp["mode"] == "recover" and inp["autoLogin"] == "limited":
        banner = {"key": "banner.limited", "tone": "warning", "action": "guard"}
    elif inp["mode"] == "recover" and inp["autoLogin"] == "awaitingProof":
        banner = {"key": "banner.awaitingProof", "tone": "info", "action": None}
    return {"tone": tone, "headline": headline, "detail": detail, "primary": primary, "secondary": secondary,
            "chip": chip, "chipTone": chip_tone, "banner": banner}


def verify_cases(data):
    failures = []
    for case in data["cases"]:
        got = present(case["input"])
        if got != case["expected"]:
            failures.append(f"{case['id']}: {got} != {case['expected']}")
    return failures, len(data["cases"])
