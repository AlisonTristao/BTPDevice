// The recovery's request authentication (src/update/OtaAuth.h). The golden
// values come from Python's hmac/hashlib with the exact layout of
// docs/PLANO.md 4.14, the same one tools/btp_ota.py builds:
//
//   key   = bytes(range(16))
//   nonce = bytes(range(0xa0, 0xb0))
//   image = b"firmware image bytes" * 100
//   hmac.new(key, b"btp-ota-1" + nonce + b"U" + sha256(image), sha256)
//   hmac.new(key, b"btp-ota-1" + nonce + b"B" + bytes(32), sha256)

#include <cstring>
#include <string>

#include "check.h"
#include "security/Sha256.h"
#include "update/OtaAuth.h"

namespace {

using namespace btpdev::ota;

constexpr char kImageSha[] = "3f5a0b3dfe39f2714a5b2c004faa882dd9a768296bb1bca2d47435894341d11f";
constexpr char kUpdateMac[] = "6ef143d06ec211459855a0c6588c7ef795799e78d6d002265e82dbf38c89fc8e";
constexpr char kRebootMac[] = "800ea9f1afc17066dbd1d5d53cafe0353f0221b71887ed2968575926adeedf21";

struct Fixture {
    std::uint8_t key[16];
    std::uint8_t nonce[kNonceSize];
    std::uint8_t sha[kHashSize];
    Fixture() {
        for (std::size_t i = 0U; i < sizeof(key); ++i) key[i] = static_cast<std::uint8_t>(i);
        for (std::size_t i = 0U; i < sizeof(nonce); ++i) nonce[i] = static_cast<std::uint8_t>(0xA0U + i);
        std::string image;
        for (int i = 0; i < 100; ++i) image += "firmware image bytes";
        btpdev::Sha256::hash(image.data(), image.size(), sha);
    }
};

void golden_update_and_reboot() {
    const Fixture f;
    char text[2U * kMacSize + 1U];
    to_hex(f.sha, sizeof(f.sha), text);
    CHECK_STR_EQ(text, kImageSha);

    std::uint8_t mac[kMacSize];
    compute_mac(f.key, sizeof(f.key), f.nonce, Action::Update, f.sha, mac);
    to_hex(mac, sizeof(mac), text);
    CHECK_STR_EQ(text, kUpdateMac);

    const std::uint8_t zeros[kHashSize] = {};
    compute_mac(f.key, sizeof(f.key), f.nonce, Action::Reboot, zeros, mac);
    to_hex(mac, sizeof(mac), text);
    CHECK_STR_EQ(text, kRebootMac);
}

void check_mac_rejects_any_change() {
    const Fixture f;
    std::uint8_t mac[kMacSize];
    CHECK(from_hex(kUpdateMac, mac, sizeof(mac)));
    CHECK(check_mac(f.key, sizeof(f.key), f.nonce, Action::Update, f.sha, mac));
    // Same MAC, other action: refused (an update grant is not a reboot grant).
    CHECK(!check_mac(f.key, sizeof(f.key), f.nonce, Action::Reboot, f.sha, mac));

    std::uint8_t other_nonce[kNonceSize];
    std::memcpy(other_nonce, f.nonce, sizeof(other_nonce));
    other_nonce[15] ^= 1U;
    CHECK(!check_mac(f.key, sizeof(f.key), other_nonce, Action::Update, f.sha, mac));

    std::uint8_t other_sha[kHashSize];
    std::memcpy(other_sha, f.sha, sizeof(other_sha));
    other_sha[0] ^= 0x80U;
    CHECK(!check_mac(f.key, sizeof(f.key), f.nonce, Action::Update, other_sha, mac));

    std::uint8_t other_key[16];
    std::memcpy(other_key, f.key, sizeof(other_key));
    other_key[3] ^= 1U;
    CHECK(!check_mac(other_key, sizeof(other_key), f.nonce, Action::Update, f.sha, mac));
}

void hex_parsing_is_strict() {
    std::uint8_t out[4];
    CHECK(from_hex("00ff7Aa9", out, sizeof(out)));
    CHECK(out[0] == 0x00U && out[1] == 0xFFU && out[2] == 0x7AU && out[3] == 0xA9U);
    CHECK(!from_hex("00ff7a", out, sizeof(out)));     // short
    CHECK(!from_hex("00ff7aa9b", out, sizeof(out)));  // long
    CHECK(!from_hex("00ff7ag9", out, sizeof(out)));   // not hex
    CHECK(!from_hex(nullptr, out, sizeof(out)));
}

}  // namespace

int main() {
    golden_update_and_reboot();
    check_mac_rejects_any_change();
    hex_parsing_is_strict();
    return test_result("test_ota_auth");
}
