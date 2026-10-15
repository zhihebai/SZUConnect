#!/usr/bin/env python3
"""Reference for 'is traffic captured by a VPN/TUN route?' evidence.

Shared reference for macOS TunnelRouteSnapshot.classify and C++ szu::routes::classify.
A route counts as tunnel evidence only when all hold:
  * route is usable (up, not reject/blackhole/cloned/llinfo/local/broadcast/
    multicast/condemned/dead);
  * its interface is live and is a tunnel (name utun/tun/tap/ppp/ipsec + digits
    on macOS; interface kind 'tunnel' on Windows);
  * destination is not loopback/link-local/multicast housekeeping;
  * scoped routes additionally need prefix > 0 and a public destination;
  * gateway-less (interface) routes need prefix <= 1 or a public destination.
Evidence describes routing only; it never proves a VPN handshake or reachability.
"""
import ipaddress
import re

EXCLUDED = {"reject", "blackhole", "cloned", "llinfo", "local", "broadcast", "multicast", "condemned", "dead"}
SPECIAL_V4 = [ipaddress.ip_network(n) for n in (
    "0.0.0.0/8", "10.0.0.0/8", "100.64.0.0/10", "127.0.0.0/8", "169.254.0.0/16", "172.16.0.0/12",
    "192.0.0.0/24", "192.0.2.0/24", "192.168.0.0/16", "198.18.0.0/15", "198.51.100.0/24",
    "203.0.113.0/24", "224.0.0.0/4", "240.0.0.0/4")]


def is_tunnel_name(name):
    return re.fullmatch(r"(utun|tun|tap|ppp|ipsec)[0-9]+", name) is not None


def non_local(route):
    net = ipaddress.ip_network(f"{route['destination']}/{route['prefix']}", strict=False)
    raw = ipaddress.ip_address(route["destination"]).packed
    prefix = route["prefix"]
    if route["family"] == 4:
        if prefix == 0:
            return all(b == 0 for b in raw)
        return not (prefix >= 8 and raw[0] in (0, 127)) and not (prefix >= 16 and raw[0] == 169 and raw[1] == 254) \
            and not (prefix >= 4 and raw[0] >= 224)
    if prefix == 0:
        return all(b == 0 for b in raw)
    return not (prefix >= 10 and raw[0] == 0xFE and raw[1] & 0xC0 == 0x80) and not (prefix >= 8 and raw[0] == 0xFF) \
        and not (prefix == 128 and all(b == 0 for b in raw[:-1]) and raw[-1] <= 1)


def public_evidence(route):
    raw = ipaddress.ip_address(route["destination"]).packed
    prefix = route["prefix"]
    if route["family"] == 4:
        dest = ipaddress.ip_network(f"{route['destination']}/{prefix}", strict=False)
        for special in SPECIAL_V4:
            if prefix >= special.prefixlen and dest.subnet_of(special):
                return False
        return True
    bits = min(prefix, 3)
    mask = 0 if bits == 0 else (0xFF << (8 - bits)) & 0xFF
    if raw[0] & mask != 0x20 & mask:
        return False
    documentation = ipaddress.ip_network("2001:db8::/32")
    dest = ipaddress.ip_network(f"{route['destination']}/{prefix}", strict=False)
    return not (prefix >= 32 and dest.subnet_of(documentation))


def is_evidence(route, interfaces, view):
    flags = set(route.get("flags", []))
    if "up" not in flags or flags & EXCLUDED:
        return False
    iface = interfaces.get(route["interface"])
    if not iface or not iface.get("up") or not iface.get("running", True):
        return False
    tunnel = is_tunnel_name(route["interface"]) if view == "mac" else iface.get("kind") == "tunnel"
    if not tunnel or not non_local(route):
        return False
    if "scoped" in flags:
        return route["prefix"] > 0 and public_evidence(route)
    if "gateway" not in flags:
        return route["prefix"] <= 1 or public_evidence(route)
    return True


def classify(case, view):
    count = sum(1 for r in case["routes"] if is_evidence(r, case["interfaces"], view))
    return {"tunnelEvidence": count > 0, "evidenceCount": count}


def verify_cases(data):
    failures = []
    total = 0
    for case in data["cases"]:
        for view in case.get("views", ["mac", "windows"]):
            total += 1
            got = classify(case, view)
            want = {"tunnelEvidence": case["tunnelEvidence"], "evidenceCount": case["evidenceCount"]}
            if got != want:
                failures.append(f"{case['id']}[{view}]: {got} != {want}")
    return failures, total
