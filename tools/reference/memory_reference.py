#!/usr/bin/env python3
"""Independent reference for network memory and the readiness checklist.

    python3 tools/reference/memory_reference.py [source-root]

Implements: keyed feature hashing, descriptor comparison, the fresh-proof verdict, CampusPlan with a
remembered-network match, and account / network / protection readiness. Swift and C++
tests use the same shared/fixtures/network-memory-cases.json and readiness-cases.json.
"""
import hashlib, hmac, json, pathlib, sys

RANK = {"strong": 3, "medium": 2, "weak": 1, "none": 0}


def feature_hash(salt, label, raw):
    return hmac.new(salt.encode(), (label + "\x1f" + raw).encode(), hashlib.sha256).hexdigest()[:32]


def compare(stored, current):
    both = lambda k: k in stored and k in current
    eq = lambda k: both(k) and stored[k] == current[k]
    ne = lambda k: both(k) and stored[k] != current[k]
    if ne("gw"):
        return "conflict" if (eq("gwip") or eq("net")) else "none"
    if eq("gw"):
        return "strong" if (eq("net") or eq("gwip") or eq("dhcp")) else "medium"
    if eq("gwip") and eq("net"):
        return "weak" if ne("dhcp") else "medium"
    if eq("gwip") or eq("net"):
        return "weak"
    return "none"


def verdict(level, teaching, dormitory, ambiguous):
    if level == "conflict":
        return "conflict"
    if level not in ("medium", "strong"):
        return "notRemembered"
    both_needed = level == "medium" or ambiguous
    ok = (teaching == "confirmed" and dormitory == "confirmed") if both_needed else dormitory == "confirmed"
    return "authorized" if ok else "awaitingProof"


def plan(kind, choice, teaching, dormitory, trusted, memory, ambiguous, blocked):
    manual = kind == "manual"
    t_ok, d_ok = teaching == "confirmed", dormitory == "confirmed"
    remembered = verdict(memory, teaching, dormitory, ambiguous) == "authorized"
    is_blocked = lambda area: (not manual) and area in blocked
    if choice == "teaching":
        if not manual and not t_ok:
            return "unconfirmed", None
        return ("paused", None) if is_blocked("teaching") else ("submit", "teaching")
    if choice == "dormitory":
        if not manual and not ((t_ok and d_ok) or remembered):
            return "unconfirmed", None
        if is_blocked("dormitory"):
            return "paused", None
        if not manual and not (trusted or remembered):
            return "needsTrust", "dormitory"
        return "submit", "dormitory"
    if not t_ok:
        if remembered:
            return ("paused", None) if is_blocked("dormitory") else ("submit", "dormitory")
        return "unconfirmed", None
    if d_ok:
        if not is_blocked("dormitory"):
            return ("submit", "dormitory") if (manual or trusted or remembered) else ("needsTrust", "dormitory")
        if not is_blocked("teaching"):
            return "submit", "teaching"
        return "paused", None
    return ("paused", None) if is_blocked("teaching") else ("submit", "teaching")


def readiness(i):
    if i["mode"] != "recover" or i["readOnly"]:
        return {"title": "ready.title.off", "tone": "neutral", "items": []}
    a = i["autoLogin"]
    if not i["hasSaved"]:
        account = ("ready.account.missing", "warning")
    elif i["unsavedChanges"]:
        account = ("ready.account.draft", "warning")
    else:
        account = ("ready.account.saved", "success")
    if i["hasSaved"] and not i.get("networkIdentified", True):
        network = ("ready.network.identifying", "warning")
    elif a == "needsTrust":
        network = ("ready.network.conflict", "danger") if i["memoryConflict"] else ("ready.network.needsTrust", "warning")
    elif a == "awaitingProof":
        network = ("ready.network.awaiting", "warning")
    elif i["sessionTrusted"] and not i["remembered"]:
        network = ("ready.network.trustedCombination" if i.get("sessionOnlyNetwork", False) else "ready.network.trusted", "success" if i.get("campusVerification") == "confirmed" else "info")
    elif i.get("sessionOnlyNetwork", False):
        network = ("ready.network.sessionAvailable", "info")
    elif i["remembered"]:
        network = ("ready.network.remembered", "success" if i.get("campusVerification") == "confirmed" else "info")
    else:
        network = ("ready.network.notNeeded", "neutral")
    if a == "paused":
        protection = ("ready.protection.paused", "danger" if i.get("hold") in ("credentials", "security") else "warning")
    elif a == "coolingDown":
        protection = ("ready.protection.cooldown", "info")
    elif a == "limited":
        protection = ("ready.protection.limited", "warning")
    else:
        protection = ("ready.protection.ok", "success")
    items = [{"kind": k, "key": v[0], "tone": v[1]} for k, v in (("account", account), ("network", network), ("protection", protection))]
    if a == "paused":
        return {"title": "ready.title.paused", "tone": protection[1], "items": items}
    tones = [x["tone"] for x in items]
    if "danger" in tones or "warning" in tones:
        return {"title": "ready.title.attention", "tone": "danger" if "danger" in tones else "warning", "items": items}
    if i.get("campusVerification") != "confirmed":
        return {"title": "ready.title.campusPending", "tone": "info", "items": items}
    return {"title": "ready.title.ready", "tone": "success", "items": items}


def verify(root):
    problems = 0
    total = 0
    mem = json.loads((root / "shared/fixtures/network-memory-cases.json").read_text("utf-8"))
    salt = mem["salt"]
    hashed = lambda raw: {k: feature_hash(salt, k, v) for k, v in raw.items() if k in ("gw", "gwip", "net", "dhcp", "dns", "sfx")}
    for c in mem["match"]:
        total += 1
        got = compare(hashed(c["stored"]), hashed(c["current"]))
        if got != c["expected"]:
            problems += 1; print(f"  MISMATCH match {c['id']}: {got} != {c['expected']}")
    for c in mem["verdict"]:
        total += 1
        got = verdict(c["level"], c["teaching"], c["dormitory"], c["ambiguous"])
        if got != c["expected"]:
            problems += 1; print(f"  MISMATCH verdict {c['id']}: {got} != {c['expected']}")
    for c in mem["plan"]:
        total += 1
        got = plan(c["kind"], c["choice"], c["discovery"]["teaching"], c["discovery"]["dormitory"], c["trusted"], c["memory"], c["ambiguous"], c["blocked"])
        want = (c["expected"]["decision"], c["expected"]["target"])
        if got != want:
            problems += 1; print(f"  MISMATCH plan {c['id']}: {got} != {want}")
    for c in mem["hash"]:
        total += 1
        if feature_hash(salt, c["label"], c["raw"]) != c["expected"]:
            problems += 1; print(f"  MISMATCH hash {c['label']}")
    for c in json.loads((root / "shared/fixtures/readiness-cases.json").read_text("utf-8"))["cases"]:
        total += 1
        if readiness(c["input"]) != c["expected"]:
            problems += 1; print(f"  MISMATCH readiness {c['id']}: {readiness(c['input'])}")
    print(f"[network-memory + readiness] {total - problems}/{total} cases agree")
    return total, problems


if __name__ == "__main__":
    root = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[2]
    _, bad = verify(root)
    sys.exit(1 if bad else 0)
