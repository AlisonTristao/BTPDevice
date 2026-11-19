#include "update/OtaAuth.h"

#include <cstring>

#include "security/Sha256.h"

namespace btpdev::ota {

void compute_mac(const std::uint8_t* key, std::size_t key_size, const std::uint8_t* nonce, Action action,
                 const std::uint8_t* image_sha256, std::uint8_t* mac) noexcept {
    std::uint8_t message[kMessageSize];
    std::size_t at = 0U;
    std::memcpy(message + at, kLabel, kLabelSize);
    at += kLabelSize;
    std::memcpy(message + at, nonce, kNonceSize);
    at += kNonceSize;
    message[at++] = static_cast<std::uint8_t>(action);
    std::memcpy(message + at, image_sha256, kHashSize);
    hmac_sha256(key, key_size, message, sizeof(message), mac);
}

bool check_mac(const std::uint8_t* key, std::size_t key_size, const std::uint8_t* nonce, Action action,
               const std::uint8_t* image_sha256, const std::uint8_t* mac) noexcept {
    std::uint8_t expected[kMacSize];
    compute_mac(key, key_size, nonce, action, image_sha256, expected);
    const bool ok = equal_bytes(expected, mac, kMacSize);
    secure_wipe(expected, sizeof(expected));
    return ok;
}

void to_hex(const std::uint8_t* data, std::size_t size, char* out) noexcept {
    static constexpr char kDigits[] = "0123456789abcdef";
    for (std::size_t i = 0U; i < size; ++i) {
        out[i * 2U] = kDigits[data[i] >> 4U];
        out[i * 2U + 1U] = kDigits[data[i] & 0x0FU];
    }
    out[size * 2U] = '\0';
}

namespace {

int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

bool from_hex(const char* text, std::uint8_t* out, std::size_t size) noexcept {
    if (text == nullptr || std::strlen(text) != size * 2U) return false;
    for (std::size_t i = 0U; i < size; ++i) {
        const int high = hex_value(text[i * 2U]);
        const int low = hex_value(text[i * 2U + 1U]);
        if (high < 0 || low < 0) return false;
        out[i] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return true;
}

}  // namespace btpdev::ota
