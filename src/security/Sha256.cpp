#include "security/Sha256.h"

#include <cstring>

namespace tracekit {
namespace {

constexpr std::uint32_t kRoundConstants[64] = {
    0x428A2F98U, 0x71374491U, 0xB5C0FBCFU, 0xE9B5DBA5U, 0x3956C25BU, 0x59F111F1U, 0x923F82A4U, 0xAB1C5ED5U,
    0xD807AA98U, 0x12835B01U, 0x243185BEU, 0x550C7DC3U, 0x72BE5D74U, 0x80DEB1FEU, 0x9BDC06A7U, 0xC19BF174U,
    0xE49B69C1U, 0xEFBE4786U, 0x0FC19DC6U, 0x240CA1CCU, 0x2DE92C6FU, 0x4A7484AAU, 0x5CB0A9DCU, 0x76F988DAU,
    0x983E5152U, 0xA831C66DU, 0xB00327C8U, 0xBF597FC7U, 0xC6E00BF3U, 0xD5A79147U, 0x06CA6351U, 0x14292967U,
    0x27B70A85U, 0x2E1B2138U, 0x4D2C6DFCU, 0x53380D13U, 0x650A7354U, 0x766A0ABBU, 0x81C2C92EU, 0x92722C85U,
    0xA2BFE8A1U, 0xA81A664BU, 0xC24B8B70U, 0xC76C51A3U, 0xD192E819U, 0xD6990624U, 0xF40E3585U, 0x106AA070U,
    0x19A4C116U, 0x1E376C08U, 0x2748774CU, 0x34B0BCB5U, 0x391C0CB3U, 0x4ED8AA4AU, 0x5B9CCA4FU, 0x682E6FF3U,
    0x748F82EEU, 0x78A5636FU, 0x84C87814U, 0x8CC70208U, 0x90BEFFFAU, 0xA4506CEBU, 0xBEF9A3F7U, 0xC67178F2U,
};

constexpr std::uint32_t kInitialState[8] = {0x6A09E667U, 0xBB67AE85U, 0x3C6EF372U, 0xA54FF53AU,
                                            0x510E527FU, 0x9B05688CU, 0x1F83D9ABU, 0x5BE0CD19U};

std::uint32_t rotate_right(std::uint32_t value, unsigned bits) noexcept {
    return (value >> bits) | (value << (32U - bits));
}

}  // namespace

void secure_wipe(void* data, std::size_t size) noexcept {
    volatile std::uint8_t* target = static_cast<std::uint8_t*>(data);
    for (std::size_t i = 0U; i < size; ++i) target[i] = 0U;
}

bool equal_bytes(const std::uint8_t* left, const std::uint8_t* right, std::size_t size) noexcept {
    std::uint8_t diff = 0U;
    for (std::size_t i = 0U; i < size; ++i) diff = static_cast<std::uint8_t>(diff | (left[i] ^ right[i]));
    return diff == 0U;
}

void Sha256::reset() noexcept {
    std::memcpy(state_, kInitialState, sizeof(state_));
    std::memset(block_, 0, sizeof(block_));
    pending_ = 0U;
    total_ = 0U;
}

void Sha256::wipe() noexcept {
    secure_wipe(state_, sizeof(state_));
    secure_wipe(block_, sizeof(block_));
    pending_ = 0U;
    total_ = 0U;
}

void Sha256::compress() noexcept {
    std::uint32_t w[64];
    for (std::size_t i = 0U; i < 16U; ++i) {
        w[i] = (static_cast<std::uint32_t>(block_[i * 4U]) << 24U) |
               (static_cast<std::uint32_t>(block_[i * 4U + 1U]) << 16U) |
               (static_cast<std::uint32_t>(block_[i * 4U + 2U]) << 8U) | static_cast<std::uint32_t>(block_[i * 4U + 3U]);
    }
    for (std::size_t i = 16U; i < 64U; ++i) {
        const std::uint32_t s0 = rotate_right(w[i - 15U], 7U) ^ rotate_right(w[i - 15U], 18U) ^ (w[i - 15U] >> 3U);
        const std::uint32_t s1 = rotate_right(w[i - 2U], 17U) ^ rotate_right(w[i - 2U], 19U) ^ (w[i - 2U] >> 10U);
        w[i] = w[i - 16U] + s0 + w[i - 7U] + s1;
    }

    std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
    for (std::size_t i = 0U; i < 64U; ++i) {
        const std::uint32_t sigma1 = rotate_right(e, 6U) ^ rotate_right(e, 11U) ^ rotate_right(e, 25U);
        const std::uint32_t choose = (e & f) ^ (~e & g);
        const std::uint32_t t1 = h + sigma1 + choose + kRoundConstants[i] + w[i];
        const std::uint32_t sigma0 = rotate_right(a, 2U) ^ rotate_right(a, 13U) ^ rotate_right(a, 22U);
        const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = sigma0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
    secure_wipe(w, sizeof(w));
}

void Sha256::update(const void* data, std::size_t size) noexcept {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    total_ += size;
    while (size != 0U) {
        // Whole blocks are copied in one go: hashing a firmware image byte by
        // byte would make the recovery's upload noticeably slower.
        const std::size_t take = size < kBlockSize - pending_ ? size : kBlockSize - pending_;
        std::memcpy(block_ + pending_, bytes, take);
        pending_ += take;
        bytes += take;
        size -= take;
        if (pending_ == kBlockSize) {
            compress();
            pending_ = 0U;
        }
    }
}

void Sha256::finish(std::uint8_t* digest) noexcept {
    const std::uint64_t bits = total_ * 8U;
    block_[pending_++] = 0x80U;
    if (pending_ > kBlockSize - 8U) {
        std::memset(block_ + pending_, 0, kBlockSize - pending_);
        compress();
        pending_ = 0U;
    }
    std::memset(block_ + pending_, 0, kBlockSize - 8U - pending_);
    for (std::size_t i = 0U; i < 8U; ++i) block_[kBlockSize - 8U + i] = static_cast<std::uint8_t>(bits >> (56U - (8U * i)));
    compress();
    for (std::size_t i = 0U; i < 8U; ++i) {
        digest[i * 4U] = static_cast<std::uint8_t>(state_[i] >> 24U);
        digest[i * 4U + 1U] = static_cast<std::uint8_t>(state_[i] >> 16U);
        digest[i * 4U + 2U] = static_cast<std::uint8_t>(state_[i] >> 8U);
        digest[i * 4U + 3U] = static_cast<std::uint8_t>(state_[i]);
    }
    wipe();
}

void Sha256::hash(const void* data, std::size_t size, std::uint8_t* digest) noexcept {
    Sha256 sha;
    sha.update(data, size);
    sha.finish(digest);
}

void hmac_sha256(const std::uint8_t* key, std::size_t key_size, const std::uint8_t* message, std::size_t message_size,
                 std::uint8_t* digest) noexcept {
    std::uint8_t padded_key[Sha256::kBlockSize] = {};
    if (key_size > Sha256::kBlockSize) {
        Sha256::hash(key, key_size, padded_key);
    } else if (key_size != 0U) {
        std::memcpy(padded_key, key, key_size);
    }

    std::uint8_t pad[Sha256::kBlockSize];
    for (std::size_t i = 0U; i < Sha256::kBlockSize; ++i) pad[i] = static_cast<std::uint8_t>(padded_key[i] ^ 0x36U);
    std::uint8_t inner_digest[Sha256::kDigestSize];
    {
        Sha256 inner;
        inner.update(pad, sizeof(pad));
        inner.update(message, message_size);
        inner.finish(inner_digest);
    }
    for (std::size_t i = 0U; i < Sha256::kBlockSize; ++i) pad[i] = static_cast<std::uint8_t>(padded_key[i] ^ 0x5CU);
    {
        Sha256 outer;
        outer.update(pad, sizeof(pad));
        outer.update(inner_digest, sizeof(inner_digest));
        outer.finish(digest);
    }

    // Everything above is derived from a key; none of it stays on the stack.
    secure_wipe(padded_key, sizeof(padded_key));
    secure_wipe(pad, sizeof(pad));
    secure_wipe(inner_digest, sizeof(inner_digest));
}

}  // namespace tracekit
