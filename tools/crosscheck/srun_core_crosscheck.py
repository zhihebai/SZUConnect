#!/usr/bin/env python3
"""Compile the macOS Sources/SrunCore.cpp on Linux (g++) against a tiny test-only
CommonCrypto shim that forwards to the system libcrypto, then compare every
shared golden vector (shared/fixtures/protocol-golden.json).

Checks the macOS C++ core against the shared Python reference. The shim is
test-only; native macOS builds use Apple's CommonCrypto.

Usage: python3 tools/crosscheck/srun_core_crosscheck.py
Requires: g++ and libcrypto.so.3 (OpenSSL 3 runtime; headers NOT required).
"""
import json
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
CORE = ROOT / "macos/SZUConnect/Sources/SrunCore.cpp"
GOLDEN = json.loads((ROOT / "shared/fixtures/protocol-golden.json").read_text("utf-8"))

DIGEST_SHIM = r'''
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef uint32_t CC_LONG;
#define CC_MD5_DIGEST_LENGTH 16
#define CC_SHA1_DIGEST_LENGTH 20
extern "C" unsigned char *SHA1(const unsigned char *d, size_t n, unsigned char *md);
static inline unsigned char *CC_SHA1(const void *data, CC_LONG len, unsigned char *md) {
    return SHA1(static_cast<const unsigned char *>(data), len, md);
}
'''
HMAC_SHIM = r'''
#pragma once
#include <stddef.h>
typedef struct evp_md_st EVP_MD;
extern "C" const EVP_MD *EVP_md5(void);
extern "C" unsigned char *HMAC(const EVP_MD *evp_md, const void *key, int key_len,
                               const unsigned char *d, size_t n, unsigned char *md, unsigned int *md_len);
enum { kCCHmacAlgMD5 = 1 };
static inline void CCHmac(int, const void *key, size_t keyLength, const void *data, size_t dataLength, void *macOut) {
    unsigned int length = 0;
    HMAC(EVP_md5(), key, static_cast<int>(keyLength), static_cast<const unsigned char *>(data), dataLength,
         static_cast<unsigned char *>(macOut), &length);
}
'''


def c_literal(text: str) -> str:
    data = text.encode("utf-8")
    return '"' + "".join(f"\\x{b:02x}" for b in data) + '"', len(data)


def main():
    lines = ['#include "SrunCore.h"', "#include <cstdio>", "#include <cstring>", "int main() { int failures = 0; int total = 0;"]
    for case in GOLDEN["xencodeBase64"]:
        data, dlen = c_literal(case["data"])
        key, klen = c_literal(case["key"])
        expected, _ = c_literal(case["expected"])
        lines.append(f'{{ char *v = szu_xencode_base64((const unsigned char*){data}, {dlen}, (const unsigned char*){key}, {klen}); total++;'
                     f' if (!v || std::strcmp(v, {expected}) != 0) {{ std::printf("FAIL xencode {case["id"]}: %s\\n", v ? v : "(null)"); failures++; }}'
                     f' else std::printf("PASS xencode {case["id"]}\\n"); szu_release(v); }}')
    for case in GOLDEN["hmacMD5"]:
        data, dlen = c_literal(case["data"])
        key, klen = c_literal(case["key"])
        expected, _ = c_literal(case["expected"])
        lines.append(f'{{ char *v = szu_hmac_md5((const unsigned char*){data}, {dlen}, (const unsigned char*){key}, {klen}); total++;'
                     f' if (!v || std::strcmp(v, {expected}) != 0) {{ std::printf("FAIL hmac {case["id"]}\\n"); failures++; }}'
                     f' else std::printf("PASS hmac {case["id"]}\\n"); szu_release(v); }}')
    for case in GOLDEN["sha1"]:
        data, dlen = c_literal(case["data"])
        expected, _ = c_literal(case["expected"])
        lines.append(f'{{ char *v = szu_sha1((const unsigned char*){data}, {dlen}); total++;'
                     f' if (!v || std::strcmp(v, {expected}) != 0) {{ std::printf("FAIL sha1 {case["id"]}\\n"); failures++; }}'
                     f' else std::printf("PASS sha1 {case["id"]}\\n"); szu_release(v); }}')
    for case in GOLDEN["payload"]:
        data, dlen = c_literal(case["infoJSON"])
        key, klen = c_literal(case["challenge"])
        expected, _ = c_literal(case["info"][len("{SRBX1}"):])
        lines.append(f'{{ char *v = szu_xencode_base64((const unsigned char*){data}, {dlen}, (const unsigned char*){key}, {klen}); total++;'
                     f' if (!v || std::strcmp(v, {expected}) != 0) {{ std::printf("FAIL payload-info {case["id"]}\\n"); failures++; }}'
                     f' else std::printf("PASS payload-info {case["id"]}\\n"); szu_release(v); }}')
    lines.append('std::printf("SrunCore.cpp cross-check: %d/%d vectors agree\\n", total - failures, total); return failures ? 1 : 0; }')
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        (tmp / "CommonCrypto").mkdir()
        (tmp / "CommonCrypto/CommonDigest.h").write_text(DIGEST_SHIM)
        (tmp / "CommonCrypto/CommonHMAC.h").write_text(HMAC_SHIM)
        (tmp / "main.cpp").write_text("\n".join(lines))
        libcrypto = next((p for p in ("/usr/lib/x86_64-linux-gnu/libcrypto.so.3", "/usr/lib/aarch64-linux-gnu/libcrypto.so.3",
                                      "/usr/lib64/libcrypto.so.3") if pathlib.Path(p).exists()), None)
        if libcrypto is None:
            print("NOT_RUN: libcrypto.so.3 not found")
            return 2
        command = ["g++", "-std=c++17", "-Wall", "-Wno-unknown-pragmas", "-I", str(tmp), "-I", str(CORE.parent),
                   str(CORE), str(tmp / "main.cpp"), libcrypto, "-o", str(tmp / "crosscheck")]
        print("$", " ".join(command))
        subprocess.run(command, check=True)
        result = subprocess.run([str(tmp / "crosscheck")])
        return result.returncode


if __name__ == "__main__":
    sys.exit(main())
