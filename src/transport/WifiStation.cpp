#include "transport/WifiStation.h"

#include <cstdio>
#include <cstring>

#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>

#include "core/LogTags.h"
#include "transport/WifiCore.h"

namespace btpdev {
namespace {

constexpr const char* kTag = tag::kWifi;

// Short hint for the common disconnect reasons; the number is always logged
// next to it.
const char* reason_hint(std::uint8_t reason) {
    switch (reason) {
        case WIFI_REASON_NO_AP_FOUND:
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
            return "AP not found";
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_AUTH_EXPIRE:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            return "authentication failed, check the password";
        case WIFI_REASON_ASSOC_FAIL:
        case WIFI_REASON_ASSOC_LEAVE:
        case WIFI_REASON_ASSOC_NOT_AUTHED:
            return "association rejected by the AP";
        case WIFI_REASON_BEACON_TIMEOUT:
            return "beacon timeout, weak signal";
        default:
            return "unspecified";
    }
}

}  // namespace

WifiStation::~WifiStation() { stop(); }

const char* WifiStation::ssid() const noexcept {
    return (networks_ != nullptr && logic_.current_network() < count_)
               ? networks_[logic_.current_network()].ssid
               : "";
}

esp_err_t WifiStation::start(const WifiNetwork* networks, std::size_t count,
                             ErrorInfo& error) noexcept {
    if (running_) return ESP_OK;
    if (networks == nullptr || count == 0U) {
        return fail(error, BTPDEV_ERR_CONFIG, kTag,
                    "no Wi-Fi network to connect to: add \"ssid,password\" lines to the "
                    "networks file (DeviceConfig::wifi.file)");
    }
    const esp_err_t err = wifi_core_start(error);
    if (err != ESP_OK) return err;

    if (esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiStation::on_event,
                                            this, &wifi_handler_) != ESP_OK ||
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiStation::on_event,
                                            this, &ip_handler_) != ESP_OK) {
        stop();
        return fail(error, BTPDEV_ERR_LINK_START, kTag, "could not register the Wi-Fi event handlers");
    }

    networks_ = networks;
    count_ = count;
    running_ = true;
    const std::uint32_t now = static_cast<std::uint32_t>(esp_log_timestamp());
    ESP_LOGI(kTag, "looking for %u known network(s)", static_cast<unsigned>(count));
    apply(logic_.begin(networks, count, now), now);
    state_ = logic_.state();
    return ESP_OK;
}

void WifiStation::stop() noexcept {
    if (wifi_handler_ != nullptr) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler_);
        wifi_handler_ = nullptr;
    }
    if (ip_handler_ != nullptr) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_handler_);
        ip_handler_ = nullptr;
    }
    if (running_) esp_wifi_disconnect();
    running_ = false;
    state_ = WifiLogic::State::Idle;
    ip_[0] = '\0';
}

void WifiStation::on_event(void* arg, esp_event_base_t base, std::int32_t id, void* data) {
    auto* self = static_cast<WifiStation*>(arg);
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_SCAN_DONE) {
            self->scan_done_.store(true, std::memory_order_release);
        } else if (id == WIFI_EVENT_STA_DISCONNECTED && data != nullptr) {
            const auto* event = static_cast<wifi_event_sta_disconnected_t*>(data);
            self->disconnect_reason_.store(event->reason, std::memory_order_relaxed);
            self->disconnected_.store(true, std::memory_order_release);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP && data != nullptr) {
        const auto* event = static_cast<ip_event_got_ip_t*>(data);
        self->ip_address_.store(event->ip_info.ip.addr, std::memory_order_relaxed);
        self->got_ip_.store(true, std::memory_order_release);
    }
}

void WifiStation::poll(std::uint32_t now_ms) noexcept {
    if (!running_) return;

    if (scan_done_.exchange(false, std::memory_order_acq_rel)) handle_scan_done(now_ms);

    if (disconnected_.exchange(false, std::memory_order_acq_rel)) {
        const WifiLogic::State before = logic_.state();
        const std::uint8_t reason = disconnect_reason_.load(std::memory_order_relaxed);
        apply(logic_.on_disconnected(now_ms), now_ms);
        if (before == WifiLogic::State::Connected) {
            ESP_LOGW(kTag, "lost \"%s\" (reason %u: %s), reconnecting", ssid(),
                     static_cast<unsigned>(reason), reason_hint(reason));
            ip_[0] = '\0';
        } else if (before == WifiLogic::State::Connecting) {
            ESP_LOGW(kTag, "\"%s\" refused the connection (reason %u: %s)", ssid(),
                     static_cast<unsigned>(reason), reason_hint(reason));
            report_failure();
        }
    }

    if (got_ip_.exchange(false, std::memory_order_acq_rel)) {
        const std::uint32_t address = ip_address_.load(std::memory_order_relaxed);
        esp_ip4_addr_t ip{};
        ip.addr = address;
        esp_ip4addr_ntoa(&ip, ip_, sizeof(ip_));
        logic_.on_got_ip(now_ms);
        if (logic_.state() == WifiLogic::State::Connected) {
            ESP_LOGI(kTag, "connected to \"%s\" as %s", ssid(), ip_);
        }
    }

    const WifiLogic::State before = logic_.state();
    apply(logic_.tick(now_ms), now_ms);
    if (before != WifiLogic::State::Backoff && logic_.state() == WifiLogic::State::Backoff) {
        ESP_LOGW(kTag, "%s, retrying in %u ms", wifi_failure_name(logic_.last_failure()),
                 static_cast<unsigned>(logic_.backoff_ms()));
        report_failure();
    }
    state_ = logic_.state();
}

void WifiStation::handle_scan_done(std::uint32_t now_ms) noexcept {
    std::uint16_t count = kMaxScanResults;
    wifi_ap_record_t records[kMaxScanResults];
    if (esp_wifi_scan_get_ap_records(&count, records) != ESP_OK) count = 0U;
    for (std::uint16_t i = 0U; i < count; ++i) {
        std::snprintf(visible_[i].ssid, sizeof(visible_[i].ssid), "%s",
                      reinterpret_cast<const char*>(records[i].ssid));
        std::memcpy(visible_[i].bssid, records[i].bssid, sizeof(visible_[i].bssid));
        visible_[i].channel = records[i].primary;
        visible_[i].rssi = records[i].rssi;
    }
    // Clears the driver's own copy even when it held more than we asked for.
    esp_wifi_clear_ap_list();

    const WifiLogic::Action action = logic_.on_scan_done(visible_, count, now_ms);
    if (action.kind == WifiLogic::ActionKind::None) {
        ESP_LOGW(kTag, "none of the %u known network(s) is among the %u visible, retrying in %u ms",
                 static_cast<unsigned>(count_), static_cast<unsigned>(count),
                 static_cast<unsigned>(logic_.backoff_ms()));
        report_failure();
    }
    apply(action, now_ms);
}

void WifiStation::apply(const WifiLogic::Action& action, std::uint32_t now_ms) noexcept {
    switch (action.kind) {
        case WifiLogic::ActionKind::None:
            break;
        case WifiLogic::ActionKind::StartScan: {
            wifi_scan_config_t scan{};
            const esp_err_t err = esp_wifi_scan_start(&scan, false);
            // A refused scan start is covered by WifiLogic's scan timeout.
            if (err != ESP_OK) ESP_LOGD(kTag, "esp_wifi_scan_start(): %s", esp_err_to_name(err));
            break;
        }
        case WifiLogic::ActionKind::Connect: {
            const WifiNetwork& network = networks_[action.network];
            wifi_config_t config{};
            // The driver's fields hold exactly 32 / 64 bytes with no room for
            // a terminator: snprintf() would cut the last character of a
            // 32-character SSID or a 64-character passphrase.
            std::memcpy(config.sta.ssid, network.ssid,
                        strnlen(network.ssid, sizeof(config.sta.ssid)));
            std::memcpy(config.sta.password, network.password,
                        strnlen(network.password, sizeof(config.sta.password)));
            config.sta.channel = action.channel;
            std::memcpy(config.sta.bssid, action.bssid, sizeof(config.sta.bssid));
            config.sta.bssid_set = true;
            ESP_LOGI(kTag, "connecting to \"%s\" (channel %u)", network.ssid,
                     static_cast<unsigned>(action.channel));
            if (esp_wifi_set_config(WIFI_IF_STA, &config) != ESP_OK ||
                esp_wifi_connect() != ESP_OK) {
                apply(logic_.on_connect_error(now_ms), now_ms);
                ESP_LOGW(kTag, "could not start connecting to \"%s\", retrying in %u ms",
                         network.ssid, static_cast<unsigned>(logic_.backoff_ms()));
                report_failure();
            }
            break;
        }
        case WifiLogic::ActionKind::Disconnect:
            esp_wifi_disconnect();
            break;
    }
}

void WifiStation::report_failure() noexcept {
    const std::uint32_t failures = logic_.failures_in_a_row();
    if (failures == 0U || failures % kFailuresBeforeError != 0U) return;
    ESP_LOGE(kTag,
             "%u attempts in a row without Wi-Fi (last: %s). Check that one of the %u "
             "network(s) in the networks file is in range and its password is right",
             static_cast<unsigned>(failures), wifi_failure_name(logic_.last_failure()),
             static_cast<unsigned>(count_));
}

}  // namespace btpdev
