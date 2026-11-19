#include "security/KeyStore.h"

#include "security/Sha256.h"

#include <cstring>

// btp::crc32 is CRC-32/ISO-HDLC (poly 0xEDB88320 reflected, 0xFFFFFFFF seed,
// final inversion) -- the same routine and the same convention the frame
// envelope already uses, and the one tools/provision_key.py was checked
// against. Reused instead of adding a second CRC to the firmware.
#include <btp/codec.hpp>


namespace btpdev {
namespace {

// Field offsets of the layout documented in KeyStore.h. Named rather than
// inlined so the parser reads like the table.
constexpr std::size_t kOffMagic = 0U;
constexpr std::size_t kOffFormat = 8U;
constexpr std::size_t kOffCipher = 9U;
constexpr std::size_t kOffKdf = 10U;
constexpr std::size_t kOffIters = 12U;
constexpr std::size_t kOffSalt = 16U;
constexpr std::size_t kOffKeyE = 32U;
constexpr std::size_t kOffKeyL = 48U;
constexpr std::size_t kOffVerifyE = 64U;
constexpr std::size_t kOffVerifyL = 72U;
constexpr std::size_t kOffCrc = 92U;

constexpr std::uint8_t kMagic[8] = {'B', 'T', 'P', 'K', 'E', 'Y', 0x00, 0x00};

// ASCII, no NUL terminator: each label is hashed as 13 octets. Distinct per
// channel so one tag can never stand in for the other.
constexpr char kVerifyLabelE[] = "bally-canal-b";
constexpr char kVerifyLabelL[] = "bally-canal-c";

std::uint32_t read_u32_le(const std::uint8_t* data) noexcept {
    return static_cast<std::uint32_t>(data[0]) |
           (static_cast<std::uint32_t>(data[1]) << 8U) |
           (static_cast<std::uint32_t>(data[2]) << 16U) |
           (static_cast<std::uint32_t>(data[3]) << 24U);
}

// True when HMAC-SHA256(key, label) starts with the expected tag.
bool verify_tag_matches(const std::uint8_t* key, std::size_t key_size,
                        const char* label, std::size_t label_size,
                        const std::uint8_t* expected,
                        std::size_t expected_size) noexcept {
    std::uint8_t digest[Sha256::kDigestSize] = {};
    hmac_sha256(key, key_size,
                reinterpret_cast<const std::uint8_t*>(label), label_size,
                digest);
    const bool matches = equal_bytes(digest, expected, expected_size);
    secure_wipe(digest, sizeof(digest));
    return matches;
}

}  // namespace

void derive_password_key(const char* password, std::size_t password_size, const char* salt,
                         std::uint32_t iterations, std::uint8_t* out, std::size_t out_size) noexcept {
    if (out == nullptr || out_size == 0U) return;
    if (out_size > Sha256::kDigestSize) out_size = Sha256::kDigestSize;
    const auto* key = reinterpret_cast<const std::uint8_t*>(password != nullptr ? password : "");
    // U1 = HMAC(P, S || INT(1)); Ui = HMAC(P, U(i-1)); T = U1 ^ U2 ^ ... ^ Uc.
    std::uint8_t first[64] = {};
    const std::size_t salt_size = salt != nullptr ? std::strlen(salt) : 0U;
    const std::size_t head = salt_size < sizeof(first) - 4U ? salt_size : sizeof(first) - 4U;
    if (head != 0U) std::memcpy(first, salt, head);
    first[head + 3U] = 1U;
    std::uint8_t u[Sha256::kDigestSize] = {};
    std::uint8_t t[Sha256::kDigestSize] = {};
    hmac_sha256(key, password_size, first, head + 4U, u);
    std::memcpy(t, u, sizeof(t));
    for (std::uint32_t i = 1U; i < iterations; ++i) {
        hmac_sha256(key, password_size, u, sizeof(u), u);
        for (std::size_t j = 0U; j < sizeof(t); ++j) t[j] = static_cast<std::uint8_t>(t[j] ^ u[j]);
    }
    std::memcpy(out, t, out_size);
    secure_wipe(u, sizeof(u));
    secure_wipe(t, sizeof(t));
}

KeyStore::Error KeyStore::parse(const std::uint8_t* data, std::size_t size,
                                Contents* out) noexcept {
    if (data == nullptr || size != kFileSize) return Error::Size;

    for (std::size_t i = 0U; i < sizeof(kMagic); ++i) {
        if (data[kOffMagic + i] != kMagic[i]) return Error::Magic;
    }
    if (data[kOffFormat] != kFormatTwoKeys) return Error::Format;
    if (data[kOffCipher] != kCipherAes128Gcm) return Error::Cipher;
    if (data[kOffKdf] != kKdfPbkdf2HmacSha256) return Error::Kdf;

    // CRC before the key material: a corrupted file must be reported as
    // corrupted, not as a wrong password.
    if (btp::crc32(data, kOffCrc) != read_u32_le(data + kOffCrc)) {
        return Error::Crc32;
    }

    // Neither the salt nor the iteration count is validated here, on purpose.
    // The robot never runs PBKDF2 -- both fields are carried for the bench and
    // for whoever has to reproduce the derivation elsewhere, so a value the
    // robot does not use must not be able to reject a good card. The CRC above
    // already covers them against corruption, and
    // tools/provision_key.py --verify is what checks them against the
    // project's contract.

    // Each key is checked against its own label, so the report says which one
    // is wrong: that is the entire reason the two tags exist.
    if (!verify_tag_matches(data + kOffKeyE, kKeyLength, kVerifyLabelE,
                            sizeof(kVerifyLabelE) - 1U, data + kOffVerifyE,
                            kVerifyLength)) {
        return Error::VerifyE;
    }
    if (!verify_tag_matches(data + kOffKeyL, kKeyLength, kVerifyLabelL,
                            sizeof(kVerifyLabelL) - 1U, data + kOffVerifyL,
                            kVerifyLength)) {
        return Error::VerifyL;
    }

    if (out != nullptr) {
        out->iterations = read_u32_le(data + kOffIters);
        std::memcpy(out->salt, data + kOffSalt, kSaltLength);
        std::memcpy(out->key_e, data + kOffKeyE, kKeyLength);
        std::memcpy(out->key_l, data + kOffKeyL, kKeyLength);
        std::memcpy(out->verify_e, data + kOffVerifyE, kVerifyLength);
        std::memcpy(out->verify_l, data + kOffVerifyL, kVerifyLength);
    }
    return Error::Ok;
}

const char* KeyStore::error_field(Error error) noexcept {
    switch (error) {
        case Error::Ok:       return "ok";
        case Error::Size:     return "size";
        case Error::Magic:    return "magic";
        case Error::Format:   return "format";
        case Error::Cipher:   return "cipher";
        case Error::Kdf:      return "kdf";
        case Error::Crc32:    return "crc32";
        case Error::VerifyE:  return "verify_e";
        case Error::VerifyL:  return "verify_l";
        case Error::File:     return "file";
    }
    return "unknown";
}

bool KeyStore::load(const std::uint8_t* data, std::size_t size) noexcept {
    Contents parsed{};
    const Error error = parse(data, size, &parsed);
    if (error != Error::Ok) {
        // A failed reload must not leave the previous keys reachable: the
        // caller is about to report an error, not to keep running with key
        // material it can no longer account for.
        unload();
        last_error_ = error;
        return false;
    }

    contents_ = parsed;
    secure_wipe(parsed.key_e, sizeof(parsed.key_e));
    secure_wipe(parsed.key_l, sizeof(parsed.key_l));
    loaded_ = true;
    last_error_ = Error::Ok;
    return true;
}

void KeyStore::unload() noexcept {
    secure_wipe(contents_.key_e, sizeof(contents_.key_e));
    secure_wipe(contents_.key_l, sizeof(contents_.key_l));
    contents_ = Contents{};
    loaded_ = false;
}

const std::uint8_t* KeyStore::key_e() const noexcept {
    return loaded_ ? contents_.key_e : nullptr;
}

const std::uint8_t* KeyStore::key_l() const noexcept {
    return loaded_ ? contents_.key_l : nullptr;
}

const std::uint8_t* KeyStore::verify_e() const noexcept {
    return loaded_ ? contents_.verify_e : nullptr;
}

const std::uint8_t* KeyStore::verify_l() const noexcept {
    return loaded_ ? contents_.verify_l : nullptr;
}

}  // namespace btpdev
