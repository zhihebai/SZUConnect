#!/usr/bin/env python3
"""Reference for the user-initiated website check address rules.

Matches Swift WebsiteAddress.normalize and C++ szu::website::normalize. Results:
  {"ok": "<normalized absolute URL>"}  or  {"error": "<kind>"}
kinds: empty, tooLong, unsupportedScheme, sensitiveComponents, invalid.
"""
import re


def normalize(text: str):
    trimmed = text.strip()
    if not trimmed:
        return {"error": "empty"}
    if len(trimmed.encode("utf-8")) > 2048:
        return {"error": "tooLong"}
    if any(ord(c) < 0x20 or ord(c) == 0x7F or 0x80 <= ord(c) <= 0x9F for c in trimmed) or "\\" in trimmed:
        return {"error": "invalid"}
    if "://" in trimmed:
        value = trimmed
    elif trimmed.startswith("//"):
        value = "https:" + trimmed
    else:
        if ":" in trimmed and not trimmed.startswith("["):
            suffix = trimmed.split(":", 1)[1].split("/", 1)[0]
            if not suffix or not suffix.isdigit():
                return {"error": "unsupportedScheme"}
        value = "https://" + trimmed
    match = re.match(r"^([A-Za-z][A-Za-z0-9+.-]*)://(.*)$", value, re.S)
    if not match:
        return {"error": "unsupportedScheme"}
    scheme = match.group(1).lower()
    if scheme not in ("http", "https"):
        return {"error": "unsupportedScheme"}
    rest = match.group(2)
    # authority ends at the first '/', '?' or '#'
    end = len(rest)
    for mark in "/?#":
        index = rest.find(mark)
        if index != -1:
            end = min(end, index)
    authority, tail = rest[:end], rest[end:]
    if " " in value:
        return {"error": "unsupportedScheme"}  # URLComponents(string:) refuses unescaped spaces
    if "@" in authority:
        return {"error": "sensitiveComponents"}
    if "?" in tail or "#" in tail:
        return {"error": "sensitiveComponents"}
    host, port = authority, None
    if authority.startswith("["):
        return {"error": "invalid"}  # IPv6 literals are outside the shared contract
    if ":" in authority:
        host, port_text = authority.rsplit(":", 1)
        if not port_text.isdigit():
            return {"error": "invalid"}
        port = int(port_text)
    if not host or "%" in host or any(c.isspace() for c in host):
        return {"error": "invalid"}
    if any(ord(c) > 0x7E for c in host):
        return {"error": "invalid"}
    if not re.match(r"^[A-Za-z0-9.-]+$", host):
        return {"error": "invalid"}
    if port is not None and not 1 <= port <= 65535:
        return {"error": "invalid"}
    normalized = f"{scheme}://{host.lower()}" + (f":{port}" if port is not None else "") + tail
    return {"ok": normalized}


def verify_cases(data):
    failures = []
    for case in data["cases"]:
        got = normalize(case["input"])
        want = {"ok": case["ok"]} if "ok" in case else {"error": case["error"]}
        if got != want:
            failures.append(f"{case['id']}: {got} != {want}")
    return failures, len(data["cases"])
