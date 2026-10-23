// SRun wire format: XEncode + custom Base64, HMAC-MD5 and SHA-1.
// XEncode is adapted from ATMLuck/AutoSZUWeb src/srun.cpp (via macOS Sources/SrunCore.cpp);
// the digests are an independent portable implementation constrained by
// shared/fixtures/protocol-golden.json. Upstream attribution is not a license grant.
#pragma once
#include <string>
#include <string_view>

#include "szu/types.hpp"

namespace szu::srun {

std::string xencode_base64(std::string_view data, std::string_view key);
std::string md5_hex(std::string_view data);
std::string sha1_hex(std::string_view data);
std::string hmac_md5_hex(std::string_view data, std::string_view key);

struct Payload {
    std::string info_json;
    std::string info;
    std::string hash;
    std::string checksum;
};

Payload payload(const Credentials& credentials, std::string_view challenge, std::string_view ip, std::string_view ac_id);

}  // namespace szu::srun
