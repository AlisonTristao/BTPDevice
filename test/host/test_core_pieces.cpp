// Identity, runtime error reporting and shell-line validation.

#include "core/Identity.h"
#include "core/RuntimeErrors.h"
#include "protocol/ShellText.h"

#include <cstring>
#include <vector>

#include "check.h"

namespace {

void identity_matches_bally_os() {
    const std::uint8_t mac[6] = {0xDC, 0xDA, 0x0C, 0x30, 0xAA, 0x5C};
    CHECK(btpdev::source_id_from_mac(mac) == 0x0C30AA5CU);
    const std::uint8_t zero_tail[6] = {0x11, 0x22, 0, 0, 0, 0};
    CHECK(btpdev::source_id_from_mac(zero_tail) == 1U);  // 0 is reserved
    CHECK(btpdev::source_id_from_mac(nullptr) == 0U);

    std::uint8_t uuid[16];
    btpdev::uuid_from_mac(mac, uuid);
    CHECK(std::memcmp(uuid, mac, 6) == 0);
    CHECK(uuid[6] == 0xB0 && uuid[15] == 0xB9);

    CHECK(btpdev::next_boot_id(0U) == 1U);
    CHECK(btpdev::next_boot_id(41U) == 42U);
    CHECK(btpdev::next_boot_id(0xFFFFFFFFU) == 1U);
}

struct Reported {
    btpdev::RuntimeError kind;
    std::uint32_t count;
    bool first;
};

std::vector<Reported> report(btpdev::RuntimeErrors& errors, std::uint32_t now) {
    std::vector<Reported> out;
    errors.report(now, 1000U, [&](btpdev::RuntimeError kind, std::uint32_t count, bool first) {
        out.push_back({kind, count, first});
    });
    return out;
}

void runtime_errors_report_first_then_summarize() {
    btpdev::RuntimeErrors errors;
    CHECK(report(errors, 0U).empty());

    errors.note(btpdev::RuntimeError::TxRefused);
    auto got = report(errors, 10U);
    CHECK(got.size() == 1U);
    CHECK(got[0].kind == btpdev::RuntimeError::TxRefused && got[0].count == 1U && got[0].first);

    // 37 more within the next second: nothing until the period has passed.
    errors.note(btpdev::RuntimeError::TxRefused, 37U);
    CHECK(report(errors, 500U).empty());
    got = report(errors, 1010U);
    CHECK(got.size() == 1U);
    CHECK(got[0].count == 37U && !got[0].first);

    // Quiet: nothing to say even after the period.
    CHECK(report(errors, 5000U).empty());
    CHECK(errors.total(btpdev::RuntimeError::TxRefused) == 38U);

    // Another kind is independent: its first occurrence is immediate.
    errors.note(btpdev::RuntimeError::RxOverflow, 3U);
    got = report(errors, 5001U);
    CHECK(got.size() == 1U && got[0].kind == btpdev::RuntimeError::RxOverflow && got[0].first);
}

void every_runtime_error_has_a_name() {
    for (std::size_t i = 0; i < btpdev::RuntimeErrors::kKinds; ++i) {
        const char* name = btpdev::runtime_error_name(static_cast<btpdev::RuntimeError>(i));
        CHECK(name != nullptr && std::strcmp(name, "?") != 0);
    }
}

btpdev::ShellTextError copy(const char* text, char* out, std::size_t capacity) {
    const btp::ByteView view{reinterpret_cast<const std::uint8_t*>(text), std::strlen(text)};
    return btpdev::copy_shell_line(view, out, capacity);
}

void shell_lines_are_one_line_of_text() {
    char out[600];
    CHECK(copy("relay -set 1", out, sizeof(out)) == btpdev::ShellTextError::Ok);
    CHECK_STR_EQ(out, "relay -set 1");
    CHECK(copy("mensagem -set ol\xc3\xa1\tmundo", out, sizeof(out)) == btpdev::ShellTextError::Ok);
    CHECK(copy("a\nb", out, sizeof(out)) == btpdev::ShellTextError::Control);
    CHECK(copy("a\rb", out, sizeof(out)) == btpdev::ShellTextError::Control);
    CHECK(copy("a\x7f", out, sizeof(out)) == btpdev::ShellTextError::Control);
    CHECK(copy("", out, sizeof(out)) == btpdev::ShellTextError::Empty);
    CHECK(copy("abc", out, 3U) == btpdev::ShellTextError::OutputTooSmall);

    std::vector<char> long_line(btpdev::kMaxShellLine + 2U, 'x');
    long_line.back() = '\0';
    CHECK(copy(long_line.data(), out, sizeof(out)) == btpdev::ShellTextError::TooLong);

    const std::uint8_t with_nul[] = {'a', 0, 'b'};
    CHECK(btpdev::copy_shell_line(btp::ByteView{with_nul, 3U}, out, sizeof(out)) ==
          btpdev::ShellTextError::Control);
}

}  // namespace

int main() {
    identity_matches_bally_os();
    runtime_errors_report_first_then_summarize();
    every_runtime_error_has_a_name();
    shell_lines_are_one_line_of_text();
    return test_result("test_core_pieces");
}
