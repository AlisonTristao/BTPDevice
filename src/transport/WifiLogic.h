#ifndef BTPDEV_WIFI_LOGIC_H
#define BTPDEV_WIFI_LOGIC_H

// The Wi-Fi station's decisions, separated from the ESP-IDF calls that carry
// them out (WifiStation.cpp) so every path -- including the ones that are
// painful to reproduce on a bench -- is covered by test/host/test_wifi.cpp.
//
//   Idle --begin()--> Scanning --scan done, a known network visible--> Connecting
//   Connecting --got IP--> Connected
//   Connecting --disconnected / timeout / connect error--> Backoff
//   Scanning --scan done, nothing known visible / scan timeout--> Backoff
//   Connected --disconnected--> Backoff          (bally_OS never recovered here)
//   Backoff --delay elapsed--> Scanning
//
// The backoff doubles on each failure, from `backoff_min_ms` up to
// `backoff_max_ms`, and resets once connected. A lost connection first waits
// only `backoff_min_ms`: an AP reboot or a brief drop should come back fast.
//
// Not thread-safe: one task feeds it (the event handlers only set flags).

#include <cstddef>
#include <cstdint>

#include "transport/WifiNetworks.h"

namespace btpdev {

class WifiLogic {
public:
    enum class State : std::uint8_t { Idle, Scanning, Connecting, Connected, Backoff };

    struct Timing {
        std::uint32_t scan_timeout_ms = 10000U;
        std::uint32_t connect_timeout_ms = 15000U;
        std::uint32_t backoff_min_ms = 1000U;
        std::uint32_t backoff_max_ms = 30000U;
    };

    /// One access point from a scan.
    struct Visible {
        char ssid[WifiNetwork::kMaxSsid + 1U] = {};
        std::uint8_t bssid[6] = {};
        std::uint8_t channel = 0U;
        std::int8_t rssi = 0;
    };

    enum class ActionKind : std::uint8_t {
        None,
        StartScan,   ///< start an async scan
        Connect,     ///< connect to networks[network] at channel/bssid
        Disconnect,  ///< drop a half-finished attempt before the next one
    };
    struct Action {
        ActionKind kind = ActionKind::None;
        std::size_t network = 0U;
        std::uint8_t channel = 0U;
        std::uint8_t bssid[6] = {};
    };

    /// Why the last attempt did not end Connected (for the log).
    enum class Failure : std::uint8_t { None, NothingVisible, ScanTimeout, ConnectTimeout, Disconnected, ConnectError };

    WifiLogic() noexcept = default;
    explicit WifiLogic(const Timing& timing) noexcept : timing_(timing) {}

    /// Starts looking for `networks` (the parsed networks file, not copied:
    /// must outlive this object). With no networks it stays Idle and returns
    /// None -- the caller reports the empty file.
    Action begin(const WifiNetwork* networks, std::size_t count, std::uint32_t now_ms) noexcept;

    /// Timeouts and backoff expiry. Call regularly (every pass of the task).
    Action tick(std::uint32_t now_ms) noexcept;

    /// A scan finished. Picks the first network, in file order, that is
    /// visible (the file order is the user's preference, not the RSSI).
    Action on_scan_done(const Visible* visible, std::size_t count, std::uint32_t now_ms) noexcept;

    void on_got_ip(std::uint32_t now_ms) noexcept;
    /// The station lost (or never completed) its association.
    Action on_disconnected(std::uint32_t now_ms) noexcept;
    /// esp_wifi_connect() itself refused to start.
    Action on_connect_error(std::uint32_t now_ms) noexcept;

    State state() const noexcept { return state_; }
    Failure last_failure() const noexcept { return last_failure_; }
    /// The network being tried / used (valid in Connecting and Connected).
    std::size_t current_network() const noexcept { return current_; }
    std::uint32_t failures_in_a_row() const noexcept { return failures_; }
    std::uint32_t backoff_ms() const noexcept { return backoff_ms_; }

private:
    Action fail(Failure failure, std::uint32_t now_ms, bool disconnect) noexcept;
    Action start_scan(std::uint32_t now_ms) noexcept;
    static bool elapsed(std::uint32_t now_ms, std::uint32_t deadline_ms) noexcept {
        return static_cast<std::int32_t>(now_ms - deadline_ms) >= 0;
    }

    Timing timing_{};
    const WifiNetwork* networks_ = nullptr;
    std::size_t count_ = 0U;
    State state_ = State::Idle;
    Failure last_failure_ = Failure::None;
    std::size_t current_ = 0U;
    std::uint32_t deadline_ms_ = 0U;
    std::uint32_t backoff_ms_ = 0U;
    std::uint32_t failures_ = 0U;
};

const char* wifi_state_name(WifiLogic::State state) noexcept;
const char* wifi_failure_name(WifiLogic::Failure failure) noexcept;

}  // namespace btpdev

#endif  // BTPDEV_WIFI_LOGIC_H
