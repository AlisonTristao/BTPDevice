#ifndef TRACEKIT_OTA_AUTH_H
#define TRACEKIT_OTA_AUTH_H

// Authentication of the recovery's HTTP requests (docs/PLANO.md D21, 4.14).
//
//   message    = "btp-ota-1" (9 ASCII octets) || nonce (16) || action (1) || sha256 (32)
//   X-OTA-Auth = hex(HMAC-SHA256(key_E, message))
//
// The nonce comes from GET /status and changes after every attempt, so a
// captured request is good once at most. The password never travels: the
// client derives key E from it (tools/provision_key.py's PBKDF2), the device
// already holds key E. tools/btp_ota.py implements the client side and
// test/host/test_ota_auth.cpp pins both ends to the same golden vector.
//
// Portable: the recovery firmware and the host tests compile it.

#include <cstddef>
#include <cstdint>

namespace tracekit::ota {

constexpr char kLabel[] = "btp-ota-1";
constexpr std::size_t kLabelSize = sizeof(kLabel) - 1U;  // no NUL
constexpr std::size_t kNonceSize = 16U;
constexpr std::size_t kHashSize = 32U;
constexpr std::size_t kMacSize = 32U;
constexpr std::size_t kMessageSize = kLabelSize + kNonceSize + 1U + kHashSize;

enum class Action : char {
    Update = 'U',  ///< POST /update: `image_sha256` is the body's SHA-256
    Reboot = 'B',  ///< POST /reboot: `image_sha256` is all zeros
};

/// HMAC-SHA256(key, message) into `mac` (kMacSize octets).
void compute_mac(const std::uint8_t* key, std::size_t key_size, const std::uint8_t* nonce, Action action,
                 const std::uint8_t* image_sha256, std::uint8_t* mac) noexcept;

/// Recomputes the MAC and compares it in fixed time.
bool check_mac(const std::uint8_t* key, std::size_t key_size, const std::uint8_t* nonce, Action action,
               const std::uint8_t* image_sha256, const std::uint8_t* mac) noexcept;

/// Lowercase hex, NUL-terminated: `out` needs 2 * size + 1 octets.
void to_hex(const std::uint8_t* data, std::size_t size, char* out) noexcept;

/// Exactly 2 * size hex digits (either case), nothing else. False otherwise.
bool from_hex(const char* text, std::uint8_t* out, std::size_t size) noexcept;

}  // namespace tracekit::ota

#endif  // TRACEKIT_OTA_AUTH_H
