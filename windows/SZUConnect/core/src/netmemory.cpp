#include "szu/netmemory.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "szu/json.hpp"

namespace szu {
namespace {
constexpr std::uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
inline std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

std::array<std::uint8_t, 32> sha256_raw(std::string_view data) {
    std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::string msg(data);
    const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8;
    msg.push_back(static_cast<char>(0x80));
    while (msg.size() % 64 != 56) msg.push_back('\0');
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<char>((bits >> (i * 8)) & 0xff));
    for (std::size_t off = 0; off < msg.size(); off += 64) {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            const auto* p = reinterpret_cast<const unsigned char*>(msg.data() + off + i * 4);
            w[i] = (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t t1 = hh + S1 + ch + kK[i] + w[i];
            const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const std::uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = S0 + mj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    std::array<std::uint8_t, 32> out{};
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 4; ++j) out[static_cast<std::size_t>(i * 4 + j)] = static_cast<std::uint8_t>((h[i] >> (24 - j * 8)) & 0xff);
    return out;
}
std::string hex(const std::uint8_t* p, std::size_t n) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (std::size_t i = 0; i < n; ++i) { out.push_back(digits[p[i] >> 4]); out.push_back(digits[p[i] & 15]); }
    return out;
}
bool is_hex(std::string_view s, std::size_t min_len, std::size_t max_len) {
    if (s.size() < min_len || s.size() > max_len) return false;
    return std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
const char* const kLabels[] = {"gw", "gwip", "net", "dhcp", "dns", "sfx"};
}  // namespace

std::string sha256_hex(std::string_view data) {
    const auto d = sha256_raw(data);
    return hex(d.data(), d.size());
}

std::string hmac_sha256_hex(std::string_view key, std::string_view data) {
    std::string k(key);
    if (k.size() > 64) { const auto d = sha256_raw(k); k.assign(reinterpret_cast<const char*>(d.data()), d.size()); }
    k.resize(64, '\0');
    std::string inner(64, '\0'), outer(64, '\0');
    for (std::size_t i = 0; i < 64; ++i) { inner[i] = static_cast<char>(k[i] ^ 0x36); outer[i] = static_cast<char>(k[i] ^ 0x5c); }
    const auto in = sha256_raw(inner + std::string(data));
    const auto out = sha256_raw(outer + std::string(reinterpret_cast<const char*>(in.data()), in.size()));
    return hex(out.data(), out.size());
}

std::string_view to_string(MatchLevel value) {
    switch (value) {
        case MatchLevel::none: return "none";
        case MatchLevel::weak: return "weak";
        case MatchLevel::medium: return "medium";
        case MatchLevel::strong: return "strong";
        case MatchLevel::conflict: return "conflict";
    }
    return "none";
}
std::optional<MatchLevel> match_level_from(std::string_view text) {
    for (MatchLevel l : {MatchLevel::none, MatchLevel::weak, MatchLevel::medium, MatchLevel::strong, MatchLevel::conflict})
        if (to_string(l) == text) return l;
    return std::nullopt;
}
std::string_view to_string(MemoryVerdict value) {
    switch (value) {
        case MemoryVerdict::notRemembered: return "notRemembered";
        case MemoryVerdict::conflict: return "conflict";
        case MemoryVerdict::awaitingProof: return "awaitingProof";
        case MemoryVerdict::authorized: return "authorized";
    }
    return "notRemembered";
}

MemoryVerdict memory_verdict(MatchLevel level, const CampusDiscovery& d, bool ambiguous) {
    if (level == MatchLevel::conflict) return MemoryVerdict::conflict;
    if (level != MatchLevel::medium && level != MatchLevel::strong) return MemoryVerdict::notRemembered;
    const bool teaching = d.teaching == Discovery::confirmed, dorm = d.dormitory == Discovery::confirmed;
    const bool both_needed = level == MatchLevel::medium || ambiguous;
    const bool ok = both_needed ? (teaching && dorm) : dorm;
    return ok ? MemoryVerdict::authorized : MemoryVerdict::awaitingProof;
}

bool NetworkDescriptor::reliable() const {
    const auto has = [&](const char* k) { const auto it = raw.find(k); return it != raw.end() && !it->second.empty(); };
    return has("gw") || (has("gwip") && has("net"));
}

std::map<std::string, std::string> NetworkMemory::hashed(const NetworkDescriptor& d) const {
    std::map<std::string, std::string> out;
    for (const char* label : kLabels) {
        const auto it = d.raw.find(label);
        if (it == d.raw.end() || it->second.empty()) continue;
        out[label] = hmac_sha256_hex(salt_, std::string(label) + '\x1f' + it->second).substr(0, 32);
    }
    return out;
}

MatchLevel NetworkMemory::compare(const std::map<std::string, std::string>& s, const std::map<std::string, std::string>& c) {
    const auto both = [&](const char* k) { return s.count(k) && c.count(k); };
    const auto eq = [&](const char* k) { return both(k) && s.at(k) == c.at(k); };
    const auto ne = [&](const char* k) { return both(k) && s.at(k) != c.at(k); };
    // Same address or subnet but different gateway hardware: another network borrowing it. Without any
    // address evidence in common a different gateway is just a different network, not a conflict.
    if (ne("gw")) return (eq("gwip") || eq("net")) ? MatchLevel::conflict : MatchLevel::none;
    if (eq("gw")) return (eq("net") || eq("gwip") || eq("dhcp")) ? MatchLevel::strong : MatchLevel::medium;
    if (eq("gwip") && eq("net")) return ne("dhcp") ? MatchLevel::weak : MatchLevel::medium;
    if (eq("gwip") || eq("net")) return MatchLevel::weak;
    return MatchLevel::none;
}

NetworkMemory::Match NetworkMemory::match(const NetworkDescriptor& d, double now, const std::set<std::string>* exclude,
                                          bool require_dormitory_proof) const {
    Match best;
    if (!has_salt() || d.empty()) return best;
    const auto current = hashed(d);
    bool conflict = false;
    int best_rank = 0;
    const auto rank = [](MatchLevel l) { return l == MatchLevel::strong ? 3 : l == MatchLevel::medium ? 2 : l == MatchLevel::weak ? 1 : 0; };
    for (const auto& r : records_) {
        if (exclude && exclude->count(r.id)) continue;
        if (r.verified + kExpirySeconds < now) continue;
        if (require_dormitory_proof && !(r.proof_teaching && r.proof_dormitory)) continue;
        const MatchLevel level = compare(r.features, current);
        if (level == MatchLevel::conflict) { conflict = true; continue; }
        if (rank(level) > best_rank) { best_rank = rank(level); best = {level, r.id, r.area}; }
    }
    if (best_rank < 2 && conflict) return {MatchLevel::conflict, {}, {}};
    return best;
}

NetworkMemory::Match NetworkMemory::match_any(const std::vector<NetworkDescriptor>& ds, double now, const std::set<std::string>* exclude,
                                              bool require_dormitory_proof) const {
    Match best;
    bool conflict = false;
    const auto rank = [](MatchLevel l) { return l == MatchLevel::strong ? 3 : l == MatchLevel::medium ? 2 : l == MatchLevel::weak ? 1 : 0; };
    for (const auto& d : ds) {
        const Match m = match(d, now, exclude, require_dormitory_proof);
        if (m.level == MatchLevel::conflict) { conflict = true; continue; }
        if (rank(m.level) > rank(best.level)) best = m;
    }
    if (rank(best.level) < 2 && conflict) return {MatchLevel::conflict, {}, {}};
    return best;
}

std::vector<std::string> NetworkMemory::matching_ids(const std::vector<NetworkDescriptor>& ds, double now) const {
    std::vector<std::string> ids;
    for (const auto& d : ds) {
        if (!has_salt() || d.empty()) continue;
        const auto current = hashed(d);
        for (const auto& r : records_) {
            if (r.verified + kExpirySeconds < now) continue;
            const MatchLevel l = compare(r.features, current);
            if ((l == MatchLevel::medium || l == MatchLevel::strong) && std::find(ids.begin(), ids.end(), r.id) == ids.end()) ids.push_back(r.id);
        }
    }
    return ids;
}

std::vector<std::string> NetworkMemory::equivalent_ids(const std::string& id) const {
    const auto selected = std::find_if(records_.begin(), records_.end(), [&](const MemoryRecord& r) { return r.id == id; });
    if (selected == records_.end()) return {};
    std::vector<std::string> ids;
    for (const auto& r : records_) {
        const auto gateway = selected->features.find("gw"), other = r.features.find("gw");
        if (r.id == id || r.features == selected->features ||
            (gateway != selected->features.end() && other != r.features.end() && gateway->second == other->second))
            ids.push_back(r.id);
    }
    return ids;
}

std::vector<std::string> NetworkMemory::merge_candidates(const NetworkDescriptor& descriptor, double now) const {
    if (!has_salt() || !descriptor.reliable()) return {};
    const auto current = hashed(descriptor);
    std::vector<std::string> ids;
    std::set<std::string> gateways;
    for (const auto& r : records_) {
        if (r.verified + kExpirySeconds < now) continue;
        const auto level = compare(r.features, current);
        if (level != MatchLevel::medium && level != MatchLevel::strong) continue;
        ids.push_back(r.id);
        if (const auto gateway = r.features.find("gw"); gateway != r.features.end()) gateways.insert(gateway->second);
    }
    return ids.size() > 1 && gateways.size() <= 1 ? ids : std::vector<std::string>{};
}

void NetworkMemory::prune(double now) {
    records_.erase(std::remove_if(records_.begin(), records_.end(), [&](const MemoryRecord& r) { return r.verified + kExpirySeconds < now; }), records_.end());
    while (records_.size() > kLimit) {
        const auto oldest = std::min_element(records_.begin(), records_.end(), [](const MemoryRecord& a, const MemoryRecord& b) { return a.verified < b.verified; });
        records_.erase(oldest);
    }
}

std::optional<std::string> NetworkMemory::remember(const NetworkDescriptor& d, const std::string& area, bool pt, bool pd, double now,
                                                   const std::string& new_id) {
    if (!has_salt() || !d.reliable()) return std::nullopt;
    prune(now);
    const auto current = hashed(d);
    std::size_t best = records_.size();
    int best_rank = 0;
    std::vector<std::string> candidates;
    std::set<std::string> gateways;
    for (std::size_t i = 0; i < records_.size(); ++i) {
        const auto& r = records_[i];
        const MatchLevel level = compare(r.features, current);
        const int rank = level == MatchLevel::strong ? 2 : level == MatchLevel::medium ? 1 : 0;
        if (!rank) continue;
        candidates.push_back(r.id);
        if (const auto gateway = r.features.find("gw"); gateway != r.features.end()) gateways.insert(gateway->second);
        if (rank > best_rank || (rank == best_rank && r.features.count("gw") && !records_[best].features.count("gw"))) {
            best = i;
            best_rank = rank;
        }
    }
    if (best != records_.size()) {
        auto& r = records_[best];
        auto refreshed = current;
        for (const char* key : {"gw", "gwip", "net"})
            if (!refreshed.count(key))
                if (const auto prior = r.features.find(key); prior != r.features.end()) refreshed[key] = prior->second;
        r.features = std::move(refreshed);
        r.verified = now;
        if (!area.empty()) r.area = area;
        // Proofs belong to this verification, not to any record being replaced.
        r.proof_teaching = pt;
        r.proof_dormitory = pd;
        const std::string id = r.id;
        // A temporarily missing gateway can match two different networks. Keep both in that case.
        if (gateways.size() <= 1)
            records_.erase(std::remove_if(records_.begin(), records_.end(), [&](const MemoryRecord& other) {
                return other.id != id && std::find(candidates.begin(), candidates.end(), other.id) != candidates.end();
            }), records_.end());
        return id;
    }
    if (!is_hex(new_id, 8, 32)) return std::nullopt;
    records_.push_back(MemoryRecord{new_id, now, now, area, pt, pd, current});
    prune(now);
    return new_id;
}

bool NetworkMemory::touch(const std::string& id, double now, const std::string& area) {
    for (auto& r : records_) {
        if (r.id != id) continue;
        if (now - r.verified < kTouchInterval && (area.empty() || r.area == area)) return false;
        r.verified = std::max(r.verified, now);
        if (!area.empty()) r.area = area;
        return true;
    }
    return false;
}

bool NetworkMemory::prefer_authenticated_area(const std::string& id, const std::string& area) {
    if (area != "teaching" && area != "dormitory") return false;
    for (auto& record : records_) {
        if (record.id != id || record.area == area) continue;
        record.area = area;
        return true;
    }
    return false;
}

bool NetworkMemory::forget(const std::string& id) {
    const auto it = std::remove_if(records_.begin(), records_.end(), [&](const MemoryRecord& r) { return r.id == id; });
    const bool removed = it != records_.end();
    records_.erase(it, records_.end());
    return removed;
}

std::string NetworkMemory::encode() const {
    json::Value root = json::Value::object(), list = json::Value::array();
    root.set("schema", json::Value::number(1));
    root.set("salt", json::Value::string(salt_));
    for (const auto& r : records_) {
        json::Value item = json::Value::object(), f = json::Value::object();
        item.set("id", json::Value::string(r.id));
        item.set("created", json::Value::number(std::floor(r.created)));
        item.set("verified", json::Value::number(std::floor(r.verified)));
        if (!r.area.empty()) item.set("area", json::Value::string(r.area));
        item.set("proofTeaching", json::Value::boolean(r.proof_teaching));
        item.set("proofDormitory", json::Value::boolean(r.proof_dormitory));
        for (const char* label : kLabels)
            if (const auto it = r.features.find(label); it != r.features.end()) f.set(label, json::Value::string(it->second));
        item.set("features", std::move(f));
        list.push(std::move(item));
    }
    root.set("records", std::move(list));
    return json::serialize(root);
}

std::optional<NetworkMemory> NetworkMemory::decode(std::string_view text, double now) {
    const auto value = json::parse(text, {.max_depth = 8, .max_bytes = 65536});
    if (!value || !value->is_object() || value->number_at("schema") != 1) return std::nullopt;
    const auto salt = value->string_at("salt");
    const auto* entries = value->find("records");
    if (!salt || !is_hex(*salt, 16, 64) || !entries || !entries->is_array() || entries->size() > kLimit) return std::nullopt;
    NetworkMemory memory(*salt);
    for (const auto& e : entries->items()) {
        if (!e.is_object()) return std::nullopt;
        MemoryRecord r;
        const auto id = e.string_at("id");
        const auto created = e.number_at("created"), verified = e.number_at("verified");
        const auto pt = e.bool_at("proofTeaching"), pd = e.bool_at("proofDormitory");
        const auto* f = e.find("features");
        if (!id || !is_hex(*id, 8, 32) || !created || !verified || !std::isfinite(*created) || !std::isfinite(*verified) || *created < 0 ||
            *verified < 0 || !pt || !pd || !f || !f->is_object())
            return std::nullopt;
        if (const auto area = e.string_at("area")) {
            if (*area != "dormitory" && *area != "teaching") return std::nullopt;
            r.area = *area;
        }
        for (const char* label : kLabels) {
            if (const auto v = f->string_at(label)) {
                if (!is_hex(*v, 32, 32)) return std::nullopt;
                r.features[label] = *v;
            }
        }
        r.id = *id; r.created = *created; r.verified = std::min(*verified, now + 3600.0);  // a future clock cannot extend trust
        r.proof_teaching = *pt; r.proof_dormitory = *pd;
        if (memory.records_.end() != std::find_if(memory.records_.begin(), memory.records_.end(), [&](const MemoryRecord& o) { return o.id == r.id; }))
            return std::nullopt;
        memory.records_.push_back(std::move(r));
    }
    memory.prune(now);
    return memory;
}

}  // namespace szu
