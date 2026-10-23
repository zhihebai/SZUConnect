#!/usr/bin/env python3
"""Reference for the network-environment layer shown to users.

1. Proxy summary (read-only).  Input is a platform-neutral snapshot; macOS
   builds it from CFNetworkCopySystemProxySettings, Windows from
   WinHttpGetIEProxyConfigForCurrentUser (see windowsRaw cases).
2. Campus bypass: checks whether proxy exceptions cover the campus endpoints.
   Applies to browsers/apps that follow the system proxy; campus authentication
   requests never use the system proxy.
3. Compatibility diagnosis: turns the bounded, credential-free comparison of
   "未采用系统代理" vs "遵循系统代理" into one verdict plus actionable advice.
"""
import fnmatch
import ipaddress

CAMPUS_HOSTS = ("net.szu.edu.cn", "172.30.255.42")


def pattern_matches(pattern: str, host: str) -> bool:
    p = pattern.strip().lower()
    h = host.lower()
    if not p:
        return False
    if p == "<local>":
        return "." not in h and ":" not in h
    if "/" in p:
        base, _, bits = p.partition("/")
        parts = base.split(".")
        if not bits.isdigit() or not 1 <= len(parts) <= 4 or not all(x.isdigit() and int(x) <= 255 for x in parts):
            return False
        parts += ["0"] * (4 - len(parts))
        try:
            network = ipaddress.ip_network(".".join(parts) + "/" + bits, strict=False)
            return ipaddress.ip_address(h) in network
        except ValueError:
            return False
    if p.startswith("*."):
        return h.endswith(p[1:])
    if p.startswith("."):
        return h == p[1:] or h.endswith(p)
    if "*" in p:
        return fnmatch.fnmatchcase(h, p)
    return h == p


def summarize_proxy(snapshot):
    if not snapshot.get("readable", False):
        return {"state": "unknown", "kinds": [], "campusBypass": "unknown"}
    kinds = [k for k in ("http", "https", "socks", "pac", "autoDiscovery") if snapshot.get(k)]
    if not kinds:
        return {"state": "off", "kinds": [], "campusBypass": "notApplicable"}
    static = [k for k in kinds if k in ("http", "https", "socks")]
    if not static:
        return {"state": "on", "kinds": kinds, "campusBypass": "unknown"}
    exceptions = snapshot.get("exceptions", [])
    covered = [h for h in CAMPUS_HOSTS if any(pattern_matches(p, h) for p in exceptions)]
    bypass = "all" if len(covered) == len(CAMPUS_HOSTS) else ("partial" if covered else "none")
    return {"state": "on", "kinds": kinds, "campusBypass": bypass}


def parse_windows(raw):
    """WinHttpGetIEProxyConfigForCurrentUser fields -> neutral snapshot."""
    if not raw.get("readable", True):
        return {"readable": False}
    snap = {"readable": True, "http": False, "https": False, "socks": False,
            "pac": bool(raw.get("autoConfigUrl")), "autoDiscovery": bool(raw.get("autoDetect")), "exceptions": []}
    proxy = (raw.get("proxy") or "").strip()
    if proxy:
        entries = [e.strip() for e in proxy.replace(" ", ";").split(";") if e.strip()]
        for entry in entries:
            if "=" in entry:
                scheme, _, target = entry.partition("=")
                scheme = scheme.strip().lower()
                if not target.strip():
                    continue
                if scheme in ("http", "https", "socks"):
                    snap[scheme] = True
            else:
                snap["http"] = True
                snap["https"] = True
    bypass = raw.get("bypass") or ""
    snap["exceptions"] = [e.strip() for e in bypass.replace(" ", ";").split(";") if e.strip()]
    return snap


REACHED = "reachable"


def diagnose(inp):
    cd, cs, pd, ps = inp["campusDirect"], inp.get("campusSystem", "notRun"), inp["publicDirect"], inp.get("publicSystem", "notRun")
    tunnel, proxy = inp.get("tunnel", "unknown"), inp.get("proxy", "unknown")
    if "cancelled" in (cd, pd, cs, ps):
        return {"verdict": "cancelled", "recommendations": [], "offerRules": False}
    if cd == "tls":
        return {"verdict": "campusTLS", "recommendations": ["checkClock", "checkInterception"], "offerRules": False}
    if cd == REACHED and pd == REACHED:
        return {"verdict": "allReachable", "recommendations": ["nothingToFix"], "offerRules": False}
    if cd == REACHED:
        if ps == REACHED:
            return {"verdict": "publicOnlyViaProxy", "recommendations": ["loginFirst", "proxyMayHideLogin"], "offerRules": False}
        return {"verdict": "loginLikelyNeeded", "recommendations": ["loginFirst"], "offerRules": False}
    if tunnel == "detected":
        return {"verdict": "campusBlockedByTunnel", "recommendations": ["addDirectRules", "tunBypassCampus", "retryAfterRules"], "offerRules": True}
    if cs == REACHED:
        return {"verdict": "campusOnlyViaProxy", "recommendations": ["addDirectRules", "checkRouting"], "offerRules": True}
    if proxy == "on":
        return {"verdict": "campusUnreachableWithProxy", "recommendations": ["confirmCampusNetwork", "addDirectRules"], "offerRules": True}
    recs = ["confirmCampusNetwork"]
    verdict = "notOnCampusNetwork" if pd == REACHED else "campusUnreachable"
    if verdict == "campusUnreachable":
        recs.append("checkCable")
    if tunnel == "unknown":
        recs.append("tunnelStateUnknown")
    return {"verdict": verdict, "recommendations": recs, "offerRules": False}


def verify_environment_cases(data):
    failures, total = [], 0
    for case in data["matcherCases"]:
        total += 1
        if pattern_matches(case["pattern"], case["host"]) != case["matches"]:
            failures.append(f"matcher {case['pattern']} vs {case['host']}")
    for case in data["summaryCases"]:
        total += 1
        got = summarize_proxy(case["snapshot"])
        if got != case["expected"]:
            failures.append(f"summary {case['id']}: {got} != {case['expected']}")
    for case in data["windowsRawCases"]:
        total += 1
        got = summarize_proxy(parse_windows(case["raw"]))
        if got != case["expected"]:
            failures.append(f"windowsRaw {case['id']}: {got} != {case['expected']}")
    return failures, total


def verify_diagnosis_cases(data):
    failures = []
    for case in data["cases"]:
        got = diagnose(case["input"])
        if got != case["expected"]:
            failures.append(f"{case['id']}: {got} != {case['expected']}")
    return failures, len(data["cases"])
