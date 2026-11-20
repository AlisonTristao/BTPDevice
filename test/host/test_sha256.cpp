// SHA-256 and HMAC-SHA256 (src/security/Sha256.h) against published vectors:
// FIPS 180-2 appendix B and RFC 4231 test cases 1, 2 and 7.

#include <cstring>
#include <string>

#include "check.h"
#include "security/Sha256.h"
#include "update/OtaAuth.h"

namespace {

std::string hex(const std::uint8_t* data, std::size_t size) {
    char out[2U * 64U + 1U];
    tracekit::ota::to_hex(data, size, out);
    return out;
}

std::string sha(const std::string& text) {
    std::uint8_t digest[tracekit::Sha256::kDigestSize];
    tracekit::Sha256::hash(text.data(), text.size(), digest);
    return hex(digest, sizeof(digest));
}

std::string hmac(const std::string& key, const std::string& message) {
    std::uint8_t digest[tracekit::Sha256::kDigestSize];
    tracekit::hmac_sha256(reinterpret_cast<const std::uint8_t*>(key.data()), key.size(),
                        reinterpret_cast<const std::uint8_t*>(message.data()), message.size(), digest);
    return hex(digest, sizeof(digest));
}

void fips_vectors() {
    CHECK(sha("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

void streaming_matches_one_shot_whatever_the_chunking() {
    // One million 'a' (FIPS 180-2 B.3), fed in uneven chunks the way the
    // recovery feeds an HTTP body.
    tracekit::Sha256 sha256;
    const std::string chunk(997U, 'a');
    std::size_t left = 1000000U;
    while (left != 0U) {
        const std::size_t take = left < chunk.size() ? left : chunk.size();
        sha256.update(chunk.data(), take);
        left -= take;
    }
    std::uint8_t digest[tracekit::Sha256::kDigestSize];
    sha256.finish(digest);
    CHECK(hex(digest, sizeof(digest)) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

    // Lengths around the padding boundary (55, 56, 63, 64 octets).
    for (std::size_t n : {55U, 56U, 63U, 64U, 65U}) {
        const std::string text(n, 'x');
        tracekit::Sha256 a;
        for (char c : text) a.update(&c, 1U);
        std::uint8_t one[32], two[32];
        a.finish(one);
        tracekit::Sha256::hash(text.data(), text.size(), two);
        CHECK(std::memcmp(one, two, 32U) == 0);
    }
}

void rfc4231_vectors() {
    // Case 1
    CHECK(hmac(std::string(20U, '\x0b'), "Hi There") ==
          "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    // Case 2
    CHECK(hmac("Jefe", "what do ya want for nothing?") ==
          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    // Case 7: 131-octet key (hashed first), multi-block message
    CHECK(hmac(std::string(131U, '\xaa'),
               "This is a test using a larger than block-size key and a larger than block-size data. The key needs "
               "to be hashed before being used by the HMAC algorithm.") ==
          "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2");
}

}  // namespace

int main() {
    fips_vectors();
    streaming_matches_one_shot_whatever_the_chunking();
    rfc4231_vectors();
    return test_result("test_sha256");
}
