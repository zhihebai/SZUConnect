// Protocol compatibility adaptation: https://github.com/ATMLuck/AutoSZUWeb (src/srun.cpp)
#include "szu/srun.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "szu/json.hpp"

namespace szu::srun {
namespace {

std::vector<std::uint32_t> words(std::string_view data, bool append_length) {
    std::vector<std::uint32_t> result;
    for (std::size_t i = 0; i < data.size(); i += 4) {
        std::uint32_t value = 0;
        for (std::size_t j = 0; j < 4 && i + j < data.size(); ++j)
            value |= static_cast<std::uint32_t>(static_cast<unsigned char>(data[i + j])) << (j * 8);
        result.push_back(value);
    }
    if (append_length) result.push_back(static_cast<std::uint32_t>(data.size()));
    return result;
}

std::string to_hex(const unsigned char* bytes, std::size_t length) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(length * 2);
    for (std::size_t i = 0; i < length; ++i) {
        out.push_back(digits[bytes[i] >> 4]);
        out.push_back(digits[bytes[i] & 15]);
    }
    return out;
}

std::uint32_t rotl(std::uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

std::array<unsigned char, 16> md5(std::string_view data) {
    static const std::uint32_t k[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int r[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                              5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                              4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                              6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    std::uint32_t h[4] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};
    std::vector<unsigned char> msg(data.begin(), data.end());
    const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8;
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    for (int i = 0; i < 8; ++i) msg.push_back(static_cast<unsigned char>(bits >> (8 * i)));
    for (std::size_t off = 0; off < msg.size(); off += 64) {
        std::uint32_t m[16];
        for (int i = 0; i < 16; ++i) {
            const unsigned char* p = &msg[off + static_cast<std::size_t>(i) * 4];
            m[i] = static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
                   (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; ++i) {
            std::uint32_t f;
            int g;
            if (i < 16) { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
            else { f = c ^ (b | ~d); g = (7 * i) % 16; }
            const std::uint32_t temp = d;
            d = c;
            c = b;
            b = b + rotl(a + f + k[i] + m[g], r[i]);
            a = temp;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    }
    std::array<unsigned char, 16> out{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) out[static_cast<std::size_t>(i * 4 + j)] = static_cast<unsigned char>(h[i] >> (8 * j));
    return out;
}

std::array<unsigned char, 20> sha1(std::string_view data) {
    std::uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    std::vector<unsigned char> msg(data.begin(), data.end());
    const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8;
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<unsigned char>(bits >> (8 * i)));
    for (std::size_t off = 0; off < msg.size(); off += 64) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            const unsigned char* p = &msg[off + static_cast<std::size_t>(i) * 4];
            w[i] = (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
                   (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
        }
        for (int i = 16; i < 80; ++i) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f, kk;
            if (i < 20) { f = (b & c) | (~b & d); kk = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d; kk = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); kk = 0x8F1BBCDCu; }
            else { f = b ^ c ^ d; kk = 0xCA62C1D6u; }
            const std::uint32_t temp = rotl(a, 5) + f + e + kk + w[i];
            e = d; d = c; c = rotl(b, 30); b = a; a = temp;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    std::array<unsigned char, 20> out{};
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 4; ++j) out[static_cast<std::size_t>(i * 4 + j)] = static_cast<unsigned char>(h[i] >> (24 - 8 * j));
    return out;
}

}  // namespace

std::string xencode_base64(std::string_view data, std::string_view key) {
    if (data.empty()) return {};
    auto values = words(data, true);
    auto keys = words(key, false);
    keys.resize(4);  // The protocol uses the first 128 key bits.
    const std::size_t n = values.size() - 1;
    std::uint32_t z = values[n], total = 0;
    for (std::size_t rounds = 6 + 52 / values.size(); rounds; --rounds) {
        total += 0x9E3779B9u;
        const std::uint32_t e = (total >> 2) & 3;
        for (std::size_t p = 0; p <= n; ++p) {
            const std::uint32_t y = values[p == n ? 0 : p + 1];
            std::uint32_t mixed = (z >> 5) ^ (y << 2);
            mixed += ((y >> 3) ^ (z << 4)) ^ (total ^ y);
            mixed += keys[(p & 3) ^ e] ^ z;
            values[p] += mixed;
            z = values[p];
        }
    }
    std::string bytes;
    for (const std::uint32_t value : values)
        for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<char>((value >> shift) & 255));
    static const char alphabet[] = "LVoJPiCN2R8G90yg+hmFHuacZ1OWMnrsSTXkYpUq/3dlbfKwv6xztjI7DeBE45QA";
    std::string output;
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        std::uint32_t chunk = static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i])) << 16;
        if (i + 1 < bytes.size()) chunk |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i + 1])) << 8;
        if (i + 2 < bytes.size()) chunk |= static_cast<unsigned char>(bytes[i + 2]);
        output += alphabet[(chunk >> 18) & 63];
        output += alphabet[(chunk >> 12) & 63];
        output += i + 1 < bytes.size() ? alphabet[(chunk >> 6) & 63] : '=';
        output += i + 2 < bytes.size() ? alphabet[chunk & 63] : '=';
    }
    return output;
}

std::string md5_hex(std::string_view data) {
    const auto d = md5(data);
    return to_hex(d.data(), d.size());
}

std::string sha1_hex(std::string_view data) {
    const auto d = sha1(data);
    return to_hex(d.data(), d.size());
}

std::string hmac_md5_hex(std::string_view data, std::string_view key) {
    std::array<unsigned char, 64> block{};
    if (key.size() > 64) {
        const auto hashed = md5(key);
        std::memcpy(block.data(), hashed.data(), hashed.size());
    } else if (!key.empty()) {
        std::memcpy(block.data(), key.data(), key.size());
    }
    std::string inner(64, '\0'), outer(64, '\0');
    for (std::size_t i = 0; i < 64; ++i) {
        inner[i] = static_cast<char>(block[i] ^ 0x36);
        outer[i] = static_cast<char>(block[i] ^ 0x5c);
    }
    inner.append(data.data(), data.size());
    const auto inner_digest = md5(inner);
    outer.append(reinterpret_cast<const char*>(inner_digest.data()), inner_digest.size());
    const auto result = md5(outer);
    return to_hex(result.data(), result.size());
}

Payload payload(const Credentials& credentials, std::string_view challenge, std::string_view ip, std::string_view ac_id) {
    Payload out;
    out.info_json = "{\"username\":" + json::quote(credentials.account) + ",\"password\":" + json::quote(credentials.password) +
                    ",\"ip\":" + json::quote(ip) + ",\"acid\":" + json::quote(ac_id) + ",\"enc_ver\":\"srun_bx1\"}";
    out.info = "{SRBX1}" + xencode_base64(out.info_json, challenge);
    out.hash = hmac_md5_hex(credentials.password, challenge);
    std::string source;
    const std::string_view parts[] = {credentials.account, out.hash, ac_id, ip, "200", "1", out.info};
    for (const std::string_view part : parts) {
        source.append(challenge.data(), challenge.size());
        source.append(part.data(), part.size());
    }
    out.checksum = sha1_hex(source);
    return out;
}

}  // namespace szu::srun
