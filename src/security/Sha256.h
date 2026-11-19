#ifndef BTPDEV_SHA256_H
#define BTPDEV_SHA256_H

// SHA-256 (FIPS 180-4) and HMAC-SHA256 (RFC 2104), self-contained and
// allocation-free. Shared by the KeyStore (verify tags, PBKDF2) and the
// recovery's OTA authentication (src/update/OtaAuth.h), which also streams a
// whole firmware image through Sha256::update().
//
// Why not mbedtls, which does ship with the SDK: on ESP-IDF 6.0.1 mbedtls is
// 4.x / TF-PSA-Crypto, where the legacy digest API became private.
// <mbedtls/md.h> still exists, but every mbedtls_md_hmac* declaration in it
// sits inside "#if defined(MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS)", which only
// mbedtls's own translation units define, and <mbedtls/sha256.h> moved to
// <mbedtls/private/sha256.h>. The supported replacement is PSA
// (psa_mac_compute), whose headers need MBEDTLS_CONFIG_FILE and
// TF_PSA_CRYPTO_USER_CONFIG_FILE on the command line to configure themselves
// the same way the linked library was built -- and PlatformIO compiles lib/*
// outside the IDF component graph, without those defines. Depending on that
// would be depending on a mismatch that only shows up at run time.
//
// This code needs no configuration and also builds on the host, which is
// what lets test/host exercise it. Its correctness is not taken on faith:
// test_sha256.cpp checks the FIPS 180-2 vectors and RFC 4231 cases 1, 2 and 7
// (7 covers a longer-than-block key and a multi-block message), and the
// golden vector in test_keystore was produced by Python's hmac/hashlib.

#include <cstddef>
#include <cstdint>

namespace btpdev {

class Sha256 {
public:
    static constexpr std::size_t kDigestSize = 32U;
    static constexpr std::size_t kBlockSize = 64U;

    Sha256() noexcept { reset(); }
    ~Sha256() { wipe(); }
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    void reset() noexcept;
    void update(const void* data, std::size_t size) noexcept;
    /// Writes kDigestSize octets. The object must be reset() before reuse.
    void finish(std::uint8_t* digest) noexcept;

    /// One-shot.
    static void hash(const void* data, std::size_t size, std::uint8_t* digest) noexcept;

private:
    void compress() noexcept;
    void wipe() noexcept;

    std::uint32_t state_[8] = {};
    std::uint8_t block_[kBlockSize] = {};
    std::size_t pending_ = 0U;
    std::uint64_t total_ = 0U;  // message length in octets
};

/// HMAC-SHA256 of one contiguous message. Keys longer than the block size
/// are hashed first, per RFC 2104. Writes Sha256::kDigestSize octets;
/// `digest` may alias `message`.
void hmac_sha256(const std::uint8_t* key, std::size_t key_size, const std::uint8_t* message,
                 std::size_t message_size, std::uint8_t* digest) noexcept;

/// Fixed-time compare: the time taken does not depend on where the first
/// difference is. Use it for anything secret (a MAC, a tag).
bool equal_bytes(const std::uint8_t* left, const std::uint8_t* right, std::size_t size) noexcept;

/// Overwrite through a volatile pointer so the compiler cannot drop the store
/// as dead on a buffer that is about to go out of scope or be reused.
void secure_wipe(void* data, std::size_t size) noexcept;

}  // namespace btpdev

#endif  // BTPDEV_SHA256_H
