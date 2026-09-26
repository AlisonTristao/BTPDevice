#ifndef BTPDEV_WIFI_STATION_H
#define BTPDEV_WIFI_STATION_H

// Connects the station to one of the known networks and keeps it connected.
// The decisions are WifiLogic's (portable, host-tested); this class only
// carries them out with esp_wifi and turns driver events into WifiLogic
// inputs. Event handlers run on the event loop task and only set flags;
// everything else happens in poll(), on the caller's task.
//
// Logging: every attempt that fails is a warning; after
// kFailuresBeforeError failures in a row an error says which networks were
// tried and why the last one failed, then once more every
// kFailuresBeforeError failures (not on every retry).

#include <btpdev/error.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <esp_event.h>

#include "transport/WifiLogic.h"
#include "transport/WifiNetworks.h"

namespace btpdev {

class WifiStation {
public:
    static constexpr std::uint32_t kFailuresBeforeError = 5U;
    static constexpr std::size_t kMaxScanResults = 20U;

    WifiStation() noexcept = default;
    ~WifiStation();
    WifiStation(const WifiStation&) = delete;
    WifiStation& operator=(const WifiStation&) = delete;

    /// `networks` must outlive the station. Starts the Wi-Fi driver if needed.
    /// BTPDEV_ERR_CONFIG when `count` is 0 (nothing to connect to).
    esp_err_t start(const WifiNetwork* networks, std::size_t count, ErrorInfo& error) noexcept;
    void stop() noexcept;

    /// Drives the state machine. Call every pass of the communication task.
    void poll(std::uint32_t now_ms) noexcept;

    bool has_ip() const noexcept { return state_ == WifiLogic::State::Connected; }
    WifiLogic::State state() const noexcept { return state_; }
    /// Dotted IP address while connected, "" otherwise.
    const char* ip() const noexcept { return ip_; }
    /// SSID of the network in use (valid while connected).
    const char* ssid() const noexcept;

private:
    static void on_event(void* arg, esp_event_base_t base, std::int32_t id, void* data);
    void apply(const WifiLogic::Action& action, std::uint32_t now_ms) noexcept;
    void handle_scan_done(std::uint32_t now_ms) noexcept;
    void report_failure() noexcept;

    const WifiNetwork* networks_ = nullptr;
    std::size_t count_ = 0U;
    WifiLogic logic_;
    WifiLogic::State state_ = WifiLogic::State::Idle;  // copy for other tasks
    bool running_ = false;
    esp_event_handler_instance_t wifi_handler_ = nullptr;
    esp_event_handler_instance_t ip_handler_ = nullptr;

    // Set by the event handlers, consumed by poll().
    std::atomic<bool> scan_done_{false};
    std::atomic<bool> got_ip_{false};
    std::atomic<bool> disconnected_{false};
    std::atomic<std::uint8_t> disconnect_reason_{0U};
    std::atomic<std::uint32_t> ip_address_{0U};

    char ip_[16] = {};
    WifiLogic::Visible visible_[kMaxScanResults] = {};
};

}  // namespace btpdev

#endif  // BTPDEV_WIFI_STATION_H
