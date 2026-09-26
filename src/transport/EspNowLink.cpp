#include "transport/EspNowLink.h"

#include <sdkconfig.h>

#include <cstring>

#include "core/LogTags.h"

#if CONFIG_BTPDEV_LINK_ESPNOW
#include <esp_log.h>
#include <esp_now.h>
#include <esp_timer.h>
#include <esp_wifi.h>

#include "transport/WifiCore.h"
#endif

namespace btpdev {

#if CONFIG_BTPDEV_LINK_ESPNOW

namespace {

constexpr const char* kTag = tag::kEspNow;
constexpr std::uint8_t kBroadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// The ESP-NOW callbacks carry no user argument: one radio, one link.
EspNowLink* g_link = nullptr;

void on_recv_trampoline(const esp_now_recv_info_t* info, const std::uint8_t* data, int size) {
    if (g_link != nullptr && info != nullptr) g_link->on_receive(info->src_addr, data, size);
}

void on_send_trampoline(const wifi_tx_info_t* info, esp_now_send_status_t status) {
    // Only the unicast frame waits for this callback. A broadcast telemetry
    // frame completing meanwhile must not release it.
    if (g_link == nullptr || info == nullptr || info->des_addr == nullptr ||
        std::memcmp(info->des_addr, kBroadcastMac, 6) == 0) {
        return;
    }
    g_link->on_sent(status == ESP_NOW_SEND_SUCCESS);
}

std::uint32_t now_ms() { return static_cast<std::uint32_t>(esp_timer_get_time() / 1000LL); }

bool all_zero(const std::uint8_t* mac) {
    for (int i = 0; i < 6; ++i) {
        if (mac[i] != 0U) return false;
    }
    return true;
}

esp_err_t add_peer(const std::uint8_t* mac) {
    esp_now_peer_info_t peer{};
    std::memcpy(peer.peer_addr, mac, 6);
    peer.channel = 0;  // the current channel
    peer.encrypt = false;  // BTP seals its own payload (keys E / L)
    peer.ifidx = WIFI_IF_STA;
    esp_err_t err = esp_now_add_peer(&peer);
    if (err == ESP_ERR_ESPNOW_EXIST) err = ESP_OK;
    if (err == ESP_OK) {
        // A fixed, fast rate: bally_OS measured the margin for it.
        esp_now_rate_config_t rate{};
        rate.phymode = WIFI_PHY_MODE_HT20;
        rate.rate = WIFI_PHY_RATE_MCS5_SGI;
        esp_now_set_peer_rate_config(mac, &rate);
    }
    return err;
}

}  // namespace

EspNowLink::~EspNowLink() { stop(); }

esp_err_t EspNowLink::start(const LinkEvents& events, ErrorInfo& error) noexcept {
    if (running_.load()) return ESP_OK;
    if (events.on_receive == nullptr) {
        return fail(error, ESP_ERR_INVALID_ARG, kTag, "ESP-NOW link started without on_receive");
    }
    if (all_zero(config_.peer_mac)) {
        return fail(error, BTPDEV_ERR_CONFIG, kTag,
                    "DeviceConfig::espnow.peer_mac is not set: put the dongle's MAC there");
    }
    if (!mutex_.create()) return fail(error, ESP_ERR_NO_MEM, kTag, "no memory for the ESP-NOW mutex");
    if (!normal_storage_.allocate(config_.normal_queue_bytes) ||
        !telemetry_storage_.allocate(config_.telemetry_queue_bytes)) {
        normal_storage_.reset();
        return fail(error, BTPDEV_ERR_MEMORY_BUDGET, kTag, "no memory for the ESP-NOW queues (%u bytes)",
                    static_cast<unsigned>(config_.normal_queue_bytes + config_.telemetry_queue_bytes));
    }
    normal_.bind(normal_storage_.data(), normal_storage_.size(), 0U);
    telemetry_.bind(telemetry_storage_.data(), telemetry_storage_.size(), 0U);
    events_ = events;

    esp_err_t err = wifi_core_start(error);
    if (err != ESP_OK) {
        normal_storage_.reset();
        telemetry_storage_.reset();
        return err;
    }
    // Both ends must sit on the same channel; the station is not associated,
    // so nothing moves it afterwards.
    err = esp_wifi_set_channel(config_.channel, WIFI_SECOND_CHAN_NONE);
    if (err == ESP_OK) {
        err = esp_now_init();
        if (err == ESP_ERR_ESPNOW_EXIST) err = ESP_OK;
    }
    g_link = this;
    if (err == ESP_OK) err = esp_now_register_recv_cb(&on_recv_trampoline);
    if (err == ESP_OK) err = esp_now_register_send_cb(&on_send_trampoline);
    if (err == ESP_OK) err = add_peer(config_.peer_mac);
    if (err == ESP_OK) err = add_peer(kBroadcastMac);
    if (err != ESP_OK) {
        esp_now_deinit();
        g_link = nullptr;
        normal_storage_.reset();
        telemetry_storage_.reset();
        return fail(error, BTPDEV_ERR_LINK_START, kTag, "could not start ESP-NOW on channel %u (%s)",
                    static_cast<unsigned>(config_.channel), esp_err_to_name(err));
    }
    running_.store(true);
    ESP_LOGI(kTag, "ready on channel %u, peer %02x:%02x:%02x:%02x:%02x:%02x",
             static_cast<unsigned>(config_.channel), config_.peer_mac[0], config_.peer_mac[1],
             config_.peer_mac[2], config_.peer_mac[3], config_.peer_mac[4], config_.peer_mac[5]);
    return ESP_OK;
}

void EspNowLink::stop() noexcept {
    if (!running_.exchange(false)) return;
    esp_now_unregister_recv_cb();
    esp_now_unregister_send_cb();
    esp_now_deinit();
    g_link = nullptr;
    {
        LinkLock lock(mutex_);
        normal_.bind(nullptr, 0U, 0U);
        telemetry_.bind(nullptr, 0U, 0U);
    }
    normal_storage_.reset();
    telemetry_storage_.reset();
}

void EspNowLink::on_receive(const std::uint8_t* src_mac, const std::uint8_t* data, int size) noexcept {
    if (data == nullptr || size <= 0) return;
    if (std::memcmp(src_mac, config_.peer_mac, 6) != 0) {
        rejected_source_.fetch_add(1U, std::memory_order_relaxed);
        return;
    }
    bytes_in_.fetch_add(static_cast<std::uint32_t>(size), std::memory_order_relaxed);
    events_.on_receive(events_.context, data, static_cast<std::size_t>(size));
}

void EspNowLink::on_sent(bool delivered) noexcept {
    if (!in_flight_.exchange(false)) return;  // already timed out
    (delivered ? delivered_ : failed_).fetch_add(1U, std::memory_order_relaxed);
}

bool EspNowLink::send(const std::uint8_t* data, std::size_t size, Priority priority) noexcept {
    bool queued = false;
    if (running_.load() && size <= kMaxFrameSize) {
        LinkLock lock(mutex_, pdMS_TO_TICKS(20));
        queued = lock.locked() &&
                 (priority == Priority::Telemetry ? telemetry_ : normal_).push(data, size, priority);
    }
    if (!queued) {
        send_refused_.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    pump(now_ms());
    return true;
}

void EspNowLink::poll(std::uint32_t now) noexcept { pump(now); }

void EspNowLink::pump(std::uint32_t now) noexcept {
    if (!running_.load()) return;
    LinkLock lock(mutex_, pdMS_TO_TICKS(5));
    if (!lock.locked()) return;

    if (in_flight_.load() && now - in_flight_since_ms_ >= kDeliveryTimeoutMs) {
        // No callback in time: give up on that one and move on.
        if (in_flight_.exchange(false)) timeouts_.fetch_add(1U, std::memory_order_relaxed);
    }

    if (!in_flight_.load() && !normal_.empty()) {
        const std::size_t size = normal_.copy_front(scratch_, sizeof(scratch_));
        if (size == 0U) {
            normal_.pop_front();  // cannot happen with the size check in send()
        } else {
            in_flight_.store(true);
            in_flight_since_ms_ = now;
            if (esp_now_send(config_.peer_mac, scratch_, size) == ESP_OK) {
                bytes_out_.fetch_add(static_cast<std::uint32_t>(size), std::memory_order_relaxed);
                normal_.pop_front();
            } else {
                in_flight_.store(false);  // driver queue full: same frame next pass
            }
        }
    }

    for (std::size_t sent = 0U; sent < kTelemetryBurst && !telemetry_.empty(); ++sent) {
        const std::size_t size = telemetry_.copy_front(scratch_, sizeof(scratch_));
        if (size == 0U) {
            telemetry_.pop_front();
            continue;
        }
        if (esp_now_send(kBroadcastMac, scratch_, size) != ESP_OK) break;  // radio pushes back
        telemetry_.pop_front();
        bytes_out_.fetch_add(static_cast<std::uint32_t>(size), std::memory_order_relaxed);
        telemetry_sent_.fetch_add(1U, std::memory_order_relaxed);
    }
}

#else  // !CONFIG_BTPDEV_LINK_ESPNOW

EspNowLink::~EspNowLink() = default;
esp_err_t EspNowLink::start(const LinkEvents&, ErrorInfo& error) noexcept {
    return fail(error, BTPDEV_ERR_LINK_UNSUPPORTED, tag::kEspNow,
                "the ESP-NOW link is disabled in menuconfig (BTPDevice > Links)");
}
void EspNowLink::stop() noexcept {}
bool EspNowLink::send(const std::uint8_t*, std::size_t, Priority) noexcept { return false; }
void EspNowLink::poll(std::uint32_t) noexcept {}
void EspNowLink::on_receive(const std::uint8_t*, const std::uint8_t*, int) noexcept {}
void EspNowLink::on_sent(bool) noexcept {}
void EspNowLink::pump(std::uint32_t) noexcept {}

#endif  // CONFIG_BTPDEV_LINK_ESPNOW

LinkStats EspNowLink::stats() const noexcept {
    LinkStats s;
    s.connections = running_.load() ? 1U : 0U;
    s.bytes_in = bytes_in_.load(std::memory_order_relaxed);
    s.bytes_out = bytes_out_.load(std::memory_order_relaxed);
    s.send_refused = send_refused_.load(std::memory_order_relaxed);
    return s;
}

EspNowLink::DeliveryStats EspNowLink::delivery_stats() const noexcept {
    DeliveryStats s;
    s.delivered = delivered_.load(std::memory_order_relaxed);
    s.failed = failed_.load(std::memory_order_relaxed);
    s.timeouts = timeouts_.load(std::memory_order_relaxed);
    s.rejected_source = rejected_source_.load(std::memory_order_relaxed);
    s.telemetry_sent = telemetry_sent_.load(std::memory_order_relaxed);
    return s;
}

}  // namespace btpdev
