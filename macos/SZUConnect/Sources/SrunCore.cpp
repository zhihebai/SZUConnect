// Protocol compatibility adaptation: https://github.com/ATMLuck/AutoSZUWeb
#include "SrunCore.h"
#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonHMAC.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {
std::vector<uint32_t> words(const unsigned char *data, size_t length, bool appendLength) {
    std::vector<uint32_t> result;
    for (size_t i = 0; i < length; i += 4) {
        uint32_t value = 0;
        for (size_t j = 0; j < 4 && i + j < length; ++j)
            value |= uint32_t(data[i + j]) << (j * 8);
        result.push_back(value);
    }
    if (appendLength) result.push_back(static_cast<uint32_t>(length));
    return result;
}
std::string encode(const unsigned char *data, size_t length, const unsigned char *key, size_t keyLength) {
    if (!length) return {};
    auto values = words(data, length, true);
    auto keys = words(key, keyLength, false);
    keys.resize(4); // The protocol uses the first 128 key bits.
    const size_t n = values.size() - 1;
    uint32_t z = values[n], total = 0;
    for (size_t rounds = 6 + 52 / values.size(); rounds; --rounds) {
        total += 0x9E3779B9u;
        const uint32_t e = (total >> 2) & 3;
        for (size_t p = 0; p <= n; ++p) {
            const uint32_t y = values[p == n ? 0 : p + 1];
            uint32_t mixed = (z >> 5) ^ (y << 2);
            mixed += ((y >> 3) ^ (z << 4)) ^ (total ^ y);
            mixed += keys[(p & 3) ^ e] ^ z;
            values[p] += mixed;
            z = values[p];
        }
    }
    std::string bytes;
    for (uint32_t value : values)
        for (unsigned shift = 0; shift < 32; shift += 8)
            bytes.push_back(static_cast<char>((value >> shift) & 255));
    static const char alphabet[] = "LVoJPiCN2R8G90yg+hmFHuacZ1OWMnrsSTXkYpUq/3dlbfKwv6xztjI7DeBE45QA";
    std::string output;
    for (size_t i = 0; i < bytes.size(); i += 3) {
        uint32_t bits = uint32_t(static_cast<unsigned char>(bytes[i])) << 16;
        if (i + 1 < bytes.size()) bits |= uint32_t(static_cast<unsigned char>(bytes[i + 1])) << 8;
        if (i + 2 < bytes.size()) bits |= static_cast<unsigned char>(bytes[i + 2]);
        output += alphabet[(bits >> 18) & 63];
        output += alphabet[(bits >> 12) & 63];
        output += i + 1 < bytes.size() ? alphabet[(bits >> 6) & 63] : '=';
        output += i + 2 < bytes.size() ? alphabet[bits & 63] : '=';
    }
    return output;
}
char *copy(const std::string &value) {
    auto *result = static_cast<char *>(std::malloc(value.size() + 1));
    if (result) std::memcpy(result, value.c_str(), value.size() + 1);
    return result;
}
char *hex(const unsigned char *bytes, size_t length) {
    static const char digits[] = "0123456789abcdef";
    std::string value;
    for (size_t i = 0; i < length; ++i) {
        value += digits[bytes[i] >> 4];
        value += digits[bytes[i] & 15];
    }
    return copy(value);
}
}
extern "C" char *szu_xencode_base64(const unsigned char *bytes, size_t length,
                                    const unsigned char *key, size_t key_length) {
    if ((!bytes && length) || (!key && key_length) || length > 65536 || key_length > 4096) return nullptr;
    try { return copy(encode(bytes, length, key, key_length)); } catch (...) { return nullptr; }
}
extern "C" char *szu_hmac_md5(const unsigned char *bytes, size_t length,
                             const unsigned char *key, size_t key_length) {
    if ((!bytes && length) || (!key && key_length)) return nullptr;
    unsigned char digest[CC_MD5_DIGEST_LENGTH];
    CCHmac(kCCHmacAlgMD5, key, key_length, bytes, length, digest);
    try { return hex(digest, sizeof digest); } catch (...) { return nullptr; }
}
extern "C" char *szu_sha1(const unsigned char *bytes, size_t length) {
    if ((!bytes && length) || length > UINT32_MAX) return nullptr;
    unsigned char digest[CC_SHA1_DIGEST_LENGTH];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CC_SHA1(bytes, static_cast<CC_LONG>(length), digest); // Required by SRun wire protocol.
#pragma clang diagnostic pop
    try { return hex(digest, sizeof digest); } catch (...) { return nullptr; }
}
extern "C" void szu_release(char *value) { std::free(value); }
