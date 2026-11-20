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
    CHECK(tracekit::source_id_from_mac(mac) == 0x0C30AA5CU);
    const std::uint8_t zero_tail[6] = {0x11, 0x22, 0, 0, 0, 0};
    CHECK(tracekit::source_id_from_mac(zero_tail) == 1U);  // 0 is reserved
    CHECK(tracekit::source_id_from_mac(nullptr) == 0U);

    std::uint8_t uuid[16];
    tracekit::uuid_from_mac(mac, uuid);
    CHECK(std::memcmp(uuid, mac, 6) == 0);
    CHECK(uuid[6] == 0xB0 && uuid[15] == 0xB9);

    CHECK(tracekit::next_boot_id(0U) == 1U);
    CHECK(tracekit::next_boot_id(41U) == 42U);
    CHECK(tracekit::next_boot_id(0xFFFFFFFFU) == 1U);

    // Deep sleep: a block of ids per cold boot, the rest from RTC memory.
    tracekit::BootIdPlan p = tracekit::plan_boot_id(false, 0U, 0U, 41U);  // cold
    CHECK(p.id == 42U && p.save && p.saved == 42U + tracekit::kBootBlock - 1U);
    CHECK(p.rtc_next == 43U && p.rtc_last == p.saved);
    tracekit::BootIdPlan w = tracekit::plan_boot_id(true, p.rtc_next, p.rtc_last, p.saved);  // wake
    CHECK(w.id == 43U && !w.save && w.rtc_next == 44U);
    w = tracekit::plan_boot_id(true, p.rtc_last, p.rtc_last, p.saved);  // the block's last id
    CHECK(w.id == p.rtc_last && !w.save && w.rtc_next == 0U);
    w = tracekit::plan_boot_id(true, 0U, p.rtc_last, p.saved);  // used up: a new block
    CHECK(w.id == p.saved + 1U && w.save);
    w = tracekit::plan_boot_id(false, 50U, p.rtc_last, p.saved);  // power loss / reset: never reuse
    CHECK(w.id == p.saved + 1U && w.save);
    w = tracekit::plan_boot_id(false, 0U, 0U, 0xFFFFFFF0U);  // near the wrap: no block
    CHECK(w.id == 0xFFFFFFF1U && w.saved == w.id && w.rtc_next == 0U);
}

struct Reported {
    tracekit::RuntimeError kind;
    std::uint32_t count;
    bool first;
};

std::vector<Reported> report(tracekit::RuntimeErrors& errors, std::uint32_t now) {
    std::vector<Reported> out;
    errors.report(now, 1000U, [&](tracekit::RuntimeError kind, std::uint32_t count, bool first) {
        out.push_back({kind, count, first});
    });
    return out;
}

void runtime_errors_report_first_then_summarize() {
    tracekit::RuntimeErrors errors;
    CHECK(report(errors, 0U).empty());

    errors.note(tracekit::RuntimeError::TxRefused);
    auto got = report(errors, 10U);
    CHECK(got.size() == 1U);
    CHECK(got[0].kind == tracekit::RuntimeError::TxRefused && got[0].count == 1U && got[0].first);

    // 37 more within the next second: nothing until the period has passed.
    errors.note(tracekit::RuntimeError::TxRefused, 37U);
    CHECK(report(errors, 500U).empty());
    got = report(errors, 1010U);
    CHECK(got.size() == 1U);
    CHECK(got[0].count == 37U && !got[0].first);

    // Quiet: nothing to say even after the period.
    CHECK(report(errors, 5000U).empty());
    CHECK(errors.total(tracekit::RuntimeError::TxRefused) == 38U);

    // Another kind is independent: its first occurrence is immediate.
    errors.note(tracekit::RuntimeError::RxOverflow, 3U);
    got = report(errors, 5001U);
    CHECK(got.size() == 1U && got[0].kind == tracekit::RuntimeError::RxOverflow && got[0].first);
}

void every_runtime_error_has_a_name() {
    for (std::size_t i = 0; i < tracekit::RuntimeErrors::kKinds; ++i) {
        const char* name = tracekit::runtime_error_name(static_cast<tracekit::RuntimeError>(i));
        CHECK(name != nullptr && std::strcmp(name, "?") != 0);
    }
}

tracekit::ShellTextError copy(const char* text, char* out, std::size_t capacity) {
    const btp::ByteView view{reinterpret_cast<const std::uint8_t*>(text), std::strlen(text)};
    return tracekit::copy_shell_line(view, out, capacity);
}

void shell_lines_are_one_line_of_text() {
    char out[600];
    CHECK(copy("relay -set 1", out, sizeof(out)) == tracekit::ShellTextError::Ok);
    CHECK_STR_EQ(out, "relay -set 1");
    CHECK(copy("mensagem -set ol\xc3\xa1\tmundo", out, sizeof(out)) == tracekit::ShellTextError::Ok);
    CHECK(copy("a\nb", out, sizeof(out)) == tracekit::ShellTextError::Control);
    CHECK(copy("a\rb", out, sizeof(out)) == tracekit::ShellTextError::Control);
    CHECK(copy("a\x7f", out, sizeof(out)) == tracekit::ShellTextError::Control);
    CHECK(copy("", out, sizeof(out)) == tracekit::ShellTextError::Empty);
    CHECK(copy("abc", out, 3U) == tracekit::ShellTextError::OutputTooSmall);

    std::vector<char> long_line(tracekit::kMaxShellLine + 2U, 'x');
    long_line.back() = '\0';
    CHECK(copy(long_line.data(), out, sizeof(out)) == tracekit::ShellTextError::TooLong);

    const std::uint8_t with_nul[] = {'a', 0, 'b'};
    CHECK(tracekit::copy_shell_line(btp::ByteView{with_nul, 3U}, out, sizeof(out)) ==
          tracekit::ShellTextError::Control);
}

}  // namespace

int main() {
    identity_matches_bally_os();
    runtime_errors_report_first_then_summarize();
    every_runtime_error_has_a_name();
    shell_lines_are_one_line_of_text();
    return test_result("test_core_pieces");
}
