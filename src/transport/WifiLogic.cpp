#include "transport/WifiLogic.h"

#include <cstring>

namespace btpdev {

WifiLogic::Action WifiLogic::begin(const WifiNetwork* networks, std::size_t count,
                                   std::uint32_t now_ms) noexcept {
    networks_ = networks;
    count_ = networks != nullptr ? count : 0U;
    failures_ = 0U;
    backoff_ms_ = 0U;
    last_failure_ = Failure::None;
    if (count_ == 0U) {
        state_ = State::Idle;
        return Action{};
    }
    return start_scan(now_ms);
}

WifiLogic::Action WifiLogic::start_scan(std::uint32_t now_ms) noexcept {
    state_ = State::Scanning;
    deadline_ms_ = now_ms + timing_.scan_timeout_ms;
    Action action;
    action.kind = ActionKind::StartScan;
    return action;
}

WifiLogic::Action WifiLogic::fail(Failure failure, std::uint32_t now_ms,
                                  bool disconnect) noexcept {
    last_failure_ = failure;
    ++failures_;
    if (backoff_ms_ == 0U) {
        backoff_ms_ = timing_.backoff_min_ms;
    } else {
        backoff_ms_ = backoff_ms_ >= timing_.backoff_max_ms / 2U ? timing_.backoff_max_ms
                                                                 : backoff_ms_ * 2U;
    }
    state_ = State::Backoff;
    deadline_ms_ = now_ms + backoff_ms_;
    Action action;
    if (disconnect) action.kind = ActionKind::Disconnect;
    return action;
}

WifiLogic::Action WifiLogic::tick(std::uint32_t now_ms) noexcept {
    switch (state_) {
        case State::Scanning:
            if (elapsed(now_ms, deadline_ms_)) return fail(Failure::ScanTimeout, now_ms, false);
            break;
        case State::Connecting:
            // Giving up waiting for DHCP does not tear the association down:
            // disconnect, or the next connect stalls on "sta is connected".
            if (elapsed(now_ms, deadline_ms_)) return fail(Failure::ConnectTimeout, now_ms, true);
            break;
        case State::Backoff:
            if (elapsed(now_ms, deadline_ms_)) return start_scan(now_ms);
            break;
        case State::Idle:
        case State::Connected:
            break;
    }
    return Action{};
}

WifiLogic::Action WifiLogic::on_scan_done(const Visible* visible, std::size_t count,
                                          std::uint32_t now_ms) noexcept {
    if (state_ != State::Scanning) return Action{};
    for (std::size_t n = 0U; n < count_; ++n) {
        for (std::size_t v = 0U; visible != nullptr && v < count; ++v) {
            if (std::strncmp(visible[v].ssid, networks_[n].ssid, sizeof(visible[v].ssid)) != 0) {
                continue;
            }
            // Channel + BSSID from the scan let the driver go straight to
            // this AP instead of sweeping every channel again (a ~20 s stall
            // in bally_OS before it did this).
            current_ = n;
            state_ = State::Connecting;
            deadline_ms_ = now_ms + timing_.connect_timeout_ms;
            Action action;
            action.kind = ActionKind::Connect;
            action.network = n;
            action.channel = visible[v].channel;
            std::memcpy(action.bssid, visible[v].bssid, sizeof(action.bssid));
            return action;
        }
    }
    return fail(Failure::NothingVisible, now_ms, false);
}

void WifiLogic::on_got_ip(std::uint32_t now_ms) noexcept {
    (void)now_ms;
    if (state_ != State::Connecting && state_ != State::Connected) return;
    state_ = State::Connected;
    failures_ = 0U;
    backoff_ms_ = 0U;
    last_failure_ = Failure::None;
}

WifiLogic::Action WifiLogic::on_disconnected(std::uint32_t now_ms) noexcept {
    if (state_ == State::Connected) {
        // A drop after a working connection: retry soon, from a fresh scan
        // (the AP may have changed channel).
        backoff_ms_ = 0U;
        return fail(Failure::Disconnected, now_ms, false);
    }
    if (state_ == State::Connecting) return fail(Failure::Disconnected, now_ms, false);
    return Action{};  // a stale event after we already moved on
}

WifiLogic::Action WifiLogic::on_connect_error(std::uint32_t now_ms) noexcept {
    if (state_ != State::Connecting) return Action{};
    return fail(Failure::ConnectError, now_ms, true);
}

const char* wifi_state_name(WifiLogic::State state) noexcept {
    switch (state) {
        case WifiLogic::State::Idle: return "idle";
        case WifiLogic::State::Scanning: return "scanning";
        case WifiLogic::State::Connecting: return "connecting";
        case WifiLogic::State::Connected: return "connected";
        case WifiLogic::State::Backoff: return "waiting to retry";
    }
    return "?";
}

const char* wifi_failure_name(WifiLogic::Failure failure) noexcept {
    switch (failure) {
        case WifiLogic::Failure::None: return "none";
        case WifiLogic::Failure::NothingVisible: return "none of the known networks is visible";
        case WifiLogic::Failure::ScanTimeout: return "scan timed out";
        case WifiLogic::Failure::ConnectTimeout: return "no IP address in time (DHCP)";
        case WifiLogic::Failure::Disconnected: return "disconnected";
        case WifiLogic::Failure::ConnectError: return "esp_wifi_connect() failed";
    }
    return "?";
}

}  // namespace btpdev
