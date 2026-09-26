// Link echo: every valid BTP frame received on the link under test is sent
// straight back on it. The PC side is tools/link_echo_test.py, which checks
// the frames come back intact and measures round trips.
//
// Stream links (Serial, TCP, BLE): the bytes go through StreamDecoder (COBS +
// CRC), the decoded frame is re-encoded and re-framed -- exactly the path the
// core will use. ESP-NOW: the datagram is echoed as is to the peer.
//
// Link selection: menuconfig "Link echo", or the PlatformIO env
// (-DECHO_LINK_*_FLAG). Logs go to the UART console.

#include <sdkconfig.h>

#include <cstdio>
#include <cstring>

#include <btp/codec.hpp>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "core/Memory.h"
#include "core/StreamFraming.h"
#include "transport/BleLink.h"
#include "transport/EspNowLink.h"
#include "transport/TcpLink.h"
#include "transport/UsbSerialLink.h"

#if defined(ECHO_LINK_SERIAL_FLAG) || (!defined(ECHO_LINK_TCP_FLAG) && !defined(ECHO_LINK_BLE_FLAG) && \
                                       !defined(ECHO_LINK_ESPNOW_FLAG) && CONFIG_ECHO_LINK_SERIAL)
#define ECHO_SERIAL 1
#elif defined(ECHO_LINK_TCP_FLAG) || (!defined(ECHO_LINK_BLE_FLAG) && !defined(ECHO_LINK_ESPNOW_FLAG) && \
                                      CONFIG_ECHO_LINK_TCP)
#define ECHO_TCP 1
#elif defined(ECHO_LINK_BLE_FLAG) || (!defined(ECHO_LINK_ESPNOW_FLAG) && CONFIG_ECHO_LINK_BLE)
#define ECHO_BLE 1
#else
#define ECHO_ESPNOW 1
#endif

#if defined(ECHO_WIFI_SSID_FLAG)
#define ECHO_SSID ECHO_WIFI_SSID_FLAG
#define ECHO_PASSWORD ECHO_WIFI_PASSWORD_FLAG
#else
#define ECHO_SSID CONFIG_ECHO_WIFI_SSID
#define ECHO_PASSWORD CONFIG_ECHO_WIFI_PASSWORD
#endif

namespace {

constexpr const char* kTag = "link_echo";
constexpr const char* kName = CONFIG_ECHO_DEVICE_NAME;

btpdev::LinkDriver* g_link = nullptr;
btpdev::StreamDecoder g_decoder;
btpdev::Buffer g_decoder_storage;
std::uint8_t g_frame[btp::kSerialMaxFrameSize];
std::uint8_t g_wire[btp::kSerialMaxPacketSize];
std::uint32_t g_echoed = 0U;
std::uint32_t g_echo_refused = 0U;

void echo_frame(const btp::DecodedFrame& decoded) {
    btp::Frame frame{};
    frame.header = decoded.header;
    frame.payload = decoded.payload;
    std::size_t frame_size = 0U;
    std::size_t wire_size = 0U;
    if (btp::encode(frame, btp::kSerialTransport, g_frame, sizeof(g_frame), &frame_size) != btp::Error::Ok ||
        !btpdev::stream_encode(g_frame, frame_size, g_wire, sizeof(g_wire), &wire_size)) {
        return;
    }
    const btpdev::Priority priority = decoded.header.type == btp::MessageType::Telemetry
                                          ? btpdev::Priority::Telemetry
                                          : btpdev::Priority::Normal;
    if (g_link->send(g_wire, wire_size, priority)) {
        ++g_echoed;
    } else {
        ++g_echo_refused;
    }
}

void on_receive(void*, const std::uint8_t* data, std::size_t size) noexcept {
    if (g_link->kind() == btpdev::LinkDriver::Kind::Datagram) {
        if (g_link->send(data, size, btpdev::Priority::Normal)) {
            ++g_echoed;
        } else {
            ++g_echo_refused;
        }
        return;
    }
    g_decoder.feed(data, size, echo_frame);
}

void on_connect(void*) noexcept { g_decoder.reset(); }
void on_disconnect(void*) noexcept { g_decoder.reset(); }

#if ECHO_TCP
btpdev::WifiNetwork g_networks[1];
#endif

btpdev::LinkDriver* make_link() {
#if ECHO_SERIAL
    static btpdev::UsbSerialLink link({.product_name = kName});
    return &link;
#elif ECHO_TCP
    std::snprintf(g_networks[0].ssid, sizeof(g_networks[0].ssid), "%s", ECHO_SSID);
    std::snprintf(g_networks[0].password, sizeof(g_networks[0].password), "%s", ECHO_PASSWORD);
    static btpdev::TcpLink link({.device_name = kName,
                                 .networks = g_networks,
                                 .network_count = g_networks[0].ssid[0] != '\0' ? 1U : 0U});
    return &link;
#elif ECHO_BLE
    static btpdev::BleLink link({.device_name = kName});
    return &link;
#else
    btpdev::EspNowLinkConfig config{};
    unsigned mac[6] = {};
    std::sscanf(CONFIG_ECHO_ESPNOW_PEER, "%x:%x:%x:%x:%x:%x", &mac[0], &mac[1], &mac[2], &mac[3],
                &mac[4], &mac[5]);
    for (int i = 0; i < 6; ++i) config.peer_mac[i] = static_cast<std::uint8_t>(mac[i]);
    config.channel = CONFIG_ECHO_ESPNOW_CHANNEL;
    static btpdev::EspNowLink link(config);
    return &link;
#endif
}

}  // namespace

extern "C" void app_main() {
    g_link = make_link();
    if (!g_decoder_storage.allocate(btpdev::StreamDecoder::kStorageSize) ||
        !g_decoder.bind(g_decoder_storage.data(), g_decoder_storage.size())) {
        ESP_LOGE(kTag, "no memory for the stream decoder");
        return;
    }

    btpdev::LinkEvents events;
    events.on_receive = &on_receive;
    events.on_connect = &on_connect;
    events.on_disconnect = &on_disconnect;
    btpdev::ErrorInfo error;
    if (g_link->start(events, error) != ESP_OK) {
        ESP_LOGE(error.module, "%s [%s]", error.message, btpdev::err_name(error.code));
        return;
    }
    ESP_LOGI(kTag, "echoing on %s as \"%s\"", g_link->name(), kName);

    std::uint32_t next_report = 0U;
    for (;;) {
        const auto now = static_cast<std::uint32_t>(esp_timer_get_time() / 1000LL);
        g_link->poll(now);
        if (static_cast<std::int32_t>(now - next_report) >= 0) {
            next_report = now + 5000U;
            const btpdev::LinkStats s = g_link->stats();
            const btpdev::StreamDecoder::Stats d = g_decoder.stats();
            ESP_LOGI(kTag,
                     "%s: %s, conn %lu, in %lu B, out %lu B, frames %lu echoed %lu refused %lu, "
                     "cobs err %lu, crc err %lu",
                     g_link->name(), g_link->connected() ? "connected" : "waiting",
                     static_cast<unsigned long>(s.connections), static_cast<unsigned long>(s.bytes_in),
                     static_cast<unsigned long>(s.bytes_out), static_cast<unsigned long>(d.frames),
                     static_cast<unsigned long>(g_echoed), static_cast<unsigned long>(g_echo_refused),
                     static_cast<unsigned long>(d.cobs_errors), static_cast<unsigned long>(d.frame_errors));
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
