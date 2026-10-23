#!/usr/bin/env python3
"""Independent Python reference for the SRun wire format used by SZUConnect.

This is a verification tool only. It is never shipped in either app and never
talks to a network. It re-implements, from the behaviour of Sources/SrunCore.cpp
(itself adapted from ATMLuck/AutoSZUWeb src/srun.cpp), the XEncode + custom
Base64, HMAC-MD5 and SHA-1 pieces so that shared golden vectors can be produced
and cross-checked by the Swift and C++ implementations.
"""
import hashlib
import hmac
import json
import struct

ALPHABET = "LVoJPiCN2R8G90yg+hmFHuacZ1OWMnrsSTXkYpUq/3dlbfKwv6xztjI7DeBE45QA"
MASK = 0xFFFFFFFF


def _words(data: bytes, append_length: bool):
    out = []
    for i in range(0, len(data), 4):
        chunk = data[i:i + 4]
        value = 0
        for j, byte in enumerate(chunk):
            value |= byte << (8 * j)
        out.append(value)
    if append_length:
        out.append(len(data) & MASK)
    return out


def xencode(data: bytes, key: bytes) -> bytes:
    if not data:
        return b""
    v = _words(data, True)
    k = _words(key, False)
    k = (k + [0, 0, 0, 0])[:4]
    n = len(v) - 1
    z = v[n]
    total = 0
    rounds = 6 + 52 // len(v)
    while rounds:
        total = (total + 0x9E3779B9) & MASK
        e = (total >> 2) & 3
        for p in range(n + 1):
            y = v[0 if p == n else p + 1]
            mixed = ((z >> 5) ^ ((y << 2) & MASK)) & MASK
            mixed = (mixed + (((y >> 3) ^ ((z << 4) & MASK)) ^ (total ^ y))) & MASK
            mixed = (mixed + (k[(p & 3) ^ e] ^ z)) & MASK
            v[p] = (v[p] + mixed) & MASK
            z = v[p]
        rounds -= 1
    return b"".join(struct.pack("<I", w) for w in v)


def custom_base64(raw: bytes) -> str:
    out = []
    for i in range(0, len(raw), 3):
        bits = raw[i] << 16
        if i + 1 < len(raw):
            bits |= raw[i + 1] << 8
        if i + 2 < len(raw):
            bits |= raw[i + 2]
        out.append(ALPHABET[(bits >> 18) & 63])
        out.append(ALPHABET[(bits >> 12) & 63])
        out.append(ALPHABET[(bits >> 6) & 63] if i + 1 < len(raw) else "=")
        out.append(ALPHABET[bits & 63] if i + 2 < len(raw) else "=")
    return "".join(out)


def xencode_base64(data: bytes, key: bytes) -> str:
    return custom_base64(xencode(data, key))


def hmac_md5_hex(data: bytes, key: bytes) -> str:
    return hmac.new(key, data, hashlib.md5).hexdigest()


def sha1_hex(data: bytes) -> str:
    return hashlib.sha1(data).hexdigest()


def json_quote(value: str) -> str:
    # Mirrors JSONSerialization(.fragmentsAllowed, .withoutEscapingSlashes):
    # non-ASCII is emitted raw (UTF-8), control characters escaped.
    return json.dumps(value, ensure_ascii=False).replace("\\/", "/")


def payload(account: str, password: str, challenge: str, ip: str, ac_id: str):
    info_json = ("{\"username\":" + json_quote(account) + ",\"password\":" + json_quote(password)
                 + ",\"ip\":" + json_quote(ip) + ",\"acid\":" + json_quote(ac_id)
                 + ",\"enc_ver\":\"srun_bx1\"}")
    key = challenge.encode("utf-8")
    info = "{SRBX1}" + xencode_base64(info_json.encode("utf-8"), key)
    digest = hmac_md5_hex(password.encode("utf-8"), key)
    checksum_source = "".join(challenge + part for part in [account, digest, ac_id, ip, "200", "1", info])
    checksum = sha1_hex(checksum_source.encode("utf-8"))
    return {"infoJSON": info_json, "info": info, "hash": digest, "checksum": checksum}



# ---------------------------------------------------------------------------
# Rejection classification shared by Swift PortalWire and C++ szu::wire.
# Conservative: only well-known SRun /
# DrCOM wording maps to a specific kind; everything else is "unspecified".
# The kind decides how long a hold lasts, never whether a password is resent.
# ---------------------------------------------------------------------------
REJECTION_FIELDS = ("error_msg", "ploy_msg", "error", "res", "suc_msg", "msg", "message")
PASSWORD_MARKERS = ("e2553", "password is error", "password error", "ldap auth error",
                    "密码错误", "密码不正确", "账号或密码错误", "用户名或密码错误")
ACCOUNT_MARKERS = ("e2531", "user not found", "userid error", "no such user", "用户不存在",
                   "账号不存在", "账户不存在")
AREA_MARKERS = ("e2833", "dhcp table", "not in the dhcp", "不在认证范围", "ip地址不在", "ip 地址不在",
                "不允许在此区域", "nas_ip_error", "ac_id_error")


def rejection_kind(obj) -> str:
    texts = []
    if isinstance(obj, dict):
        for key in REJECTION_FIELDS:
            value = obj.get(key)
            if isinstance(value, str) and value:
                texts.append(value.lower())
    joined = " | ".join(texts)
    # Password wording wins: a combined "账号或密码错误" must keep the stricter hold.
    if any(marker in joined for marker in PASSWORD_MARKERS):
        return "password"
    if any(marker in joined for marker in ACCOUNT_MARKERS):
        return "account"
    if any(marker in joined for marker in AREA_MARKERS):
        return "areaMismatch"
    return "unspecified"


def verify_rejections(data):
    failures = []
    cases = data["cases"]
    for case in cases:
        got = rejection_kind(case["response"])
        if got != case["kind"]:
            failures.append(f"{case['id']}: {got} != {case['kind']}")
    return failures, len(cases)


# ---------------------------------------------------------------------------
# Golden vectors shared by Swift tests and C++ tests/cross-checks.
# ---------------------------------------------------------------------------
def build_golden():
    challenge = "0123456789abcdef0123456789abcdef"
    xencode_cases = []
    inputs = [
        ("empty", "", "k"),
        ("one-byte", "a", "0123456789abcdef"),
        ("three-bytes", "abc", "0123456789abcdef"),
        ("four-bytes", "abcd", "0123456789abcdef"),
        ("five-bytes", "abcde", "0123456789abcdef"),
        ("short-key", "hello srun", "k"),
        ("long-key", "hello srun", "k" * 40),
        ("utf8-cjk", "深圳大学 校园网", challenge),
        ("json-like", "{\"username\":\"student\",\"password\":\"p@ss\"}", challenge),
        ("sixteen", "0123456789abcdef", challenge),
        ("sixty-five", "x" * 65, challenge),
    ]
    for name, data, key in inputs:
        xencode_cases.append({"id": name, "data": data, "key": key,
                              "expected": xencode_base64(data.encode("utf-8"), key.encode("utf-8"))})
    hmac_cases = [{"id": n, "data": d, "key": k, "expected": hmac_md5_hex(d.encode(), k.encode())}
                  for n, d, k in (("basic", "pass+&word", challenge), ("empty-data", "", challenge),
                                  ("cjk", "密码🙂", challenge))]
    sha_cases = [{"id": n, "data": d, "expected": sha1_hex(d.encode())}
                 for n, d in (("empty", ""), ("abc", "abc"), ("cjk", "深大随连"))]
    payload_cases = []
    for name, account, password, ch, ip, acid in (
        ("baseline", "student", "pass+&word", challenge, "10.2.3.4", "1"),
        ("quote-and-slash", "2023123456", "a\"b\\c/d", challenge, "172.29.1.2", "12"),
        ("unicode-password", "fixture-A", "密码 with space", challenge, "10.0.0.8", "3"),
    ):
        result = payload(account, password, ch, ip, acid)
        payload_cases.append({"id": name, "account": account, "password": password, "challenge": ch,
                              "ip": ip, "acID": acid, **result})
    return {
        "schemaVersion": 1,
        "description": "SRun wire-format golden vectors produced by tools/reference/srun_reference.py. "
                       "Consumed by macOS Tests/ProtocolOfflineTests.swift + SharedContractTests.swift, "
                       "the Linux g++ cross-check of Sources/SrunCore.cpp, and Windows SZUConnect.Tests. "
                       "Only printable input is covered; control characters are rejected by neither side and "
                       "are intentionally not part of the contract.",
        "alphabet": ALPHABET,
        "xencodeBase64": xencode_cases,
        "hmacMD5": hmac_cases,
        "sha1": sha_cases,
        "payload": payload_cases,
    }


def verify_golden(data):
    failures = []
    total = 0
    for case in data["xencodeBase64"]:
        total += 1
        got = xencode_base64(case["data"].encode("utf-8"), case["key"].encode("utf-8"))
        if got != case["expected"]:
            failures.append(f"xencode {case['id']}")
    for case in data["hmacMD5"]:
        total += 1
        if hmac_md5_hex(case["data"].encode(), case["key"].encode()) != case["expected"]:
            failures.append(f"hmac {case['id']}")
    for case in data["sha1"]:
        total += 1
        if sha1_hex(case["data"].encode()) != case["expected"]:
            failures.append(f"sha1 {case['id']}")
    for case in data["payload"]:
        total += 1
        got = payload(case["account"], case["password"], case["challenge"], case["ip"], case["acID"])
        for key in ("infoJSON", "info", "hash", "checksum"):
            if got[key] != case[key]:
                failures.append(f"payload {case['id']} {key}")
    return failures, total


if __name__ == "__main__":
    import pathlib
    import sys
    if "--write-golden" in sys.argv:
        target = pathlib.Path(__file__).resolve().parents[2] / "shared/fixtures/protocol-golden.json"
        target.write_text(json.dumps(build_golden(), ensure_ascii=False, indent=2) + "\n", "utf-8")
        print("wrote", target)
    else:
        result = payload("student", "pass+&word", "0123456789abcdef0123456789abcdef", "10.2.3.4", "1")
        print(json.dumps(result, ensure_ascii=False, indent=2))
