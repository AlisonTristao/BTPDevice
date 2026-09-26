#include "transport/WifiLogic.h"
#include "transport/WifiNetworks.h"

#include <cstring>

#include "check.h"

using btpdev::WifiLogic;
using btpdev::WifiNetwork;

namespace {

// ---- networks file ----------------------------------------------------------

void parses_lines_comments_and_line_endings() {
    const char text[] =
        "# home first\n"
        "casa,senha123\r\n"
        "\n"
        "oficina,a,b,c\r"      // commas inside the password
        "visitantes,\n"        // open network
        ",semssid\n"           // no SSID: skipped
        "sem-virgula\n";       // no comma: skipped
    WifiNetwork out[8];
    const auto result = btpdev::parse_wifi_networks(text, sizeof(text) - 1U, out, 8U);
    CHECK(result.networks == 3U);
    CHECK(result.skipped_lines == 2U);
    CHECK_STR_EQ(out[0].ssid, "casa");
    CHECK_STR_EQ(out[0].password, "senha123");
    CHECK_STR_EQ(out[1].ssid, "oficina");
    CHECK_STR_EQ(out[1].password, "a,b,c");
    CHECK_STR_EQ(out[2].ssid, "visitantes");
    CHECK_STR_EQ(out[2].password, "");
}

void too_long_fields_and_overflow_are_skipped() {
    char text[256];
    std::snprintf(text, sizeof(text), "%s,x\nok1,p\nok2,p\nok3,p\n",
                  "123456789012345678901234567890123");  // 33-char SSID
    WifiNetwork out[2];
    const auto result = btpdev::parse_wifi_networks(text, std::strlen(text), out, 2U);
    CHECK(result.networks == 2U);
    CHECK(result.skipped_lines == 2U);  // long SSID + ok3 past `max`
    CHECK_STR_EQ(out[1].ssid, "ok2");
}

void stops_at_an_embedded_nul() {
    const char text[] = "a,1\n\0b,2\n";
    WifiNetwork out[4];
    const auto result = btpdev::parse_wifi_networks(text, sizeof(text) - 1U, out, 4U);
    CHECK(result.networks == 1U);
}

// ---- hostname (must match TraceView's mdnsHostFromName) -------------------

void hostname_matches_traceview() {
    char host[64];
    CHECK(btpdev::mdns_host_from_name("Robo 2 (bancada)", host, sizeof(host)));
    CHECK_STR_EQ(host, "robo-2-bancada");
    CHECK(btpdev::mdns_host_from_name("  BallyRobot  ", host, sizeof(host)));
    CHECK_STR_EQ(host, "ballyrobot");
    CHECK(btpdev::mdns_host_from_name("--estação__quintal--", host, sizeof(host)));
    CHECK_STR_EQ(host, "esta-o-quintal");
    CHECK(!btpdev::mdns_host_from_name("***", host, sizeof(host)));

    char long_name[100];
    std::memset(long_name, 'a', 70);
    long_name[70] = '\0';
    CHECK(btpdev::mdns_host_from_name(long_name, host, sizeof(host)));
    CHECK(std::strlen(host) == 63U);

    // A '-' landing exactly at the 63-char cut is trimmed, like left(63) + chop.
    std::memset(long_name, 'b', 62);
    long_name[62] = ' ';
    long_name[63] = 'c';
    long_name[64] = '\0';
    CHECK(btpdev::mdns_host_from_name(long_name, host, sizeof(host)));
    CHECK(std::strlen(host) == 62U);
}

// ---- reconnection state machine ----------------------------------------------

WifiNetwork networks[2];

void setup_networks() {
    std::snprintf(networks[0].ssid, sizeof(networks[0].ssid), "casa");
    std::snprintf(networks[1].ssid, sizeof(networks[1].ssid), "oficina");
}

WifiLogic::Visible ap(const char* ssid, std::uint8_t channel) {
    WifiLogic::Visible v{};
    std::snprintf(v.ssid, sizeof(v.ssid), "%s", ssid);
    v.channel = channel;
    v.bssid[5] = channel;
    return v;
}

void no_networks_stays_idle() {
    WifiLogic logic;
    const auto action = logic.begin(networks, 0U, 0U);
    CHECK(action.kind == WifiLogic::ActionKind::None);
    CHECK(logic.state() == WifiLogic::State::Idle);
}

void connects_to_the_first_known_network_in_file_order() {
    WifiLogic logic;
    auto action = logic.begin(networks, 2U, 0U);
    CHECK(action.kind == WifiLogic::ActionKind::StartScan);
    CHECK(logic.state() == WifiLogic::State::Scanning);

    // "oficina" is stronger and listed first by the scan, but "casa" comes
    // first in the file.
    const WifiLogic::Visible seen[] = {ap("vizinho", 1), ap("oficina", 6), ap("casa", 11)};
    action = logic.on_scan_done(seen, 3U, 100U);
    CHECK(action.kind == WifiLogic::ActionKind::Connect);
    CHECK(action.network == 0U);
    CHECK(action.channel == 11U);
    CHECK(action.bssid[5] == 11U);
    CHECK(logic.state() == WifiLogic::State::Connecting);

    logic.on_got_ip(500U);
    CHECK(logic.state() == WifiLogic::State::Connected);
    CHECK(logic.failures_in_a_row() == 0U);
}

void nothing_visible_backs_off_and_doubles() {
    WifiLogic::Timing timing;
    timing.backoff_min_ms = 1000U;
    timing.backoff_max_ms = 4000U;
    WifiLogic logic(timing);
    logic.begin(networks, 2U, 0U);

    const WifiLogic::Visible seen[] = {ap("vizinho", 1)};
    std::uint32_t now = 0U;
    const std::uint32_t expected[] = {1000U, 2000U, 4000U, 4000U};
    for (std::uint32_t wait : expected) {
        auto action = logic.on_scan_done(seen, 1U, now);
        CHECK(action.kind == WifiLogic::ActionKind::None);
        CHECK(logic.state() == WifiLogic::State::Backoff);
        CHECK(logic.last_failure() == WifiLogic::Failure::NothingVisible);
        CHECK(logic.backoff_ms() == wait);
        CHECK(logic.tick(now + wait - 1U).kind == WifiLogic::ActionKind::None);
        now += wait;
        CHECK(logic.tick(now).kind == WifiLogic::ActionKind::StartScan);
    }
    CHECK(logic.failures_in_a_row() == 4U);
}

void connect_timeout_disconnects_before_retrying() {
    WifiLogic::Timing timing;
    timing.connect_timeout_ms = 5000U;
    WifiLogic logic(timing);
    logic.begin(networks, 2U, 0U);
    const WifiLogic::Visible seen[] = {ap("casa", 3)};
    logic.on_scan_done(seen, 1U, 0U);
    CHECK(logic.tick(4999U).kind == WifiLogic::ActionKind::None);
    const auto action = logic.tick(5000U);
    CHECK(action.kind == WifiLogic::ActionKind::Disconnect);
    CHECK(logic.last_failure() == WifiLogic::Failure::ConnectTimeout);
    CHECK(logic.state() == WifiLogic::State::Backoff);
}

void scan_timeout_backs_off() {
    WifiLogic::Timing timing;
    timing.scan_timeout_ms = 3000U;
    WifiLogic logic(timing);
    logic.begin(networks, 1U, 0U);
    CHECK(logic.tick(3000U).kind == WifiLogic::ActionKind::None);
    CHECK(logic.last_failure() == WifiLogic::Failure::ScanTimeout);
    CHECK(logic.state() == WifiLogic::State::Backoff);
}

void a_drop_after_connecting_recovers_quickly() {
    WifiLogic::Timing timing;
    timing.backoff_min_ms = 500U;
    WifiLogic logic(timing);
    logic.begin(networks, 2U, 0U);
    const WifiLogic::Visible seen[] = {ap("casa", 3)};
    // Fail a few times first, so the backoff has grown.
    const WifiLogic::Visible none[] = {ap("x", 1)};
    logic.on_scan_done(none, 1U, 0U);
    logic.tick(500U);
    logic.on_scan_done(none, 1U, 500U);
    logic.tick(1500U);
    CHECK(logic.backoff_ms() == 1000U);

    logic.on_scan_done(seen, 1U, 1500U);
    logic.on_got_ip(2000U);
    CHECK(logic.state() == WifiLogic::State::Connected);

    // The AP reboots: the first retry waits only backoff_min_ms.
    logic.on_disconnected(60000U);
    CHECK(logic.state() == WifiLogic::State::Backoff);
    CHECK(logic.backoff_ms() == 500U);
    CHECK(logic.tick(60500U).kind == WifiLogic::ActionKind::StartScan);
}

void stale_events_are_ignored() {
    WifiLogic logic;
    logic.begin(networks, 1U, 0U);
    // A disconnect / connect error while scanning (left over from an old
    // attempt) must not count as a new failure.
    CHECK(logic.on_disconnected(10U).kind == WifiLogic::ActionKind::None);
    CHECK(logic.on_connect_error(10U).kind == WifiLogic::ActionKind::None);
    CHECK(logic.state() == WifiLogic::State::Scanning);
    CHECK(logic.failures_in_a_row() == 0U);
    // got IP while scanning: ignored too.
    logic.on_got_ip(20U);
    CHECK(logic.state() == WifiLogic::State::Scanning);
}

void timers_survive_the_32_bit_wraparound() {
    WifiLogic::Timing timing;
    timing.backoff_min_ms = 1000U;
    WifiLogic logic(timing);
    const std::uint32_t near_wrap = 0xFFFFFF00U;
    logic.begin(networks, 1U, near_wrap);
    const WifiLogic::Visible none[] = {ap("x", 1)};
    logic.on_scan_done(none, 1U, near_wrap);
    CHECK(logic.tick(near_wrap + 999U).kind == WifiLogic::ActionKind::None);
    CHECK(logic.tick(near_wrap + 1000U).kind == WifiLogic::ActionKind::StartScan);
}

}  // namespace

int main() {
    parses_lines_comments_and_line_endings();
    too_long_fields_and_overflow_are_skipped();
    stops_at_an_embedded_nul();
    hostname_matches_traceview();
    setup_networks();
    no_networks_stays_idle();
    connects_to_the_first_known_network_in_file_order();
    nothing_visible_backs_off_and_doubles();
    connect_timeout_disconnects_before_retrying();
    scan_timeout_backs_off();
    a_drop_after_connecting_recovers_quickly();
    stale_events_are_ignored();
    timers_survive_the_32_bit_wraparound();
    return test_result("test_wifi");
}
