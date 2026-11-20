#include "transport/WifiCore.h"

#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <nvs_flash.h>

#include <sdkconfig.h>

#include "core/LogTags.h"
#include "core/Memory.h"
#include "core/TaskSpawn.h"

namespace tracekit {

namespace {
WifiBuffers g_buffers = WifiBuffers::Lean;
}  // namespace

void wifi_core_set_buffers(WifiBuffers buffers) noexcept { g_buffers = buffers; }
namespace {

bool g_started = false;

}  // namespace

bool wifi_core_started() noexcept { return g_started; }

esp_err_t wifi_core_start(ErrorInfo& error) noexcept {
    if (g_started) return ESP_OK;
    constexpr const char* kTag = tag::kWifi;

    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "nvs_flash_init() failed (%s): Wi-Fi works, without its calibration cache",
                 esp_err_to_name(err));
    }

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return fail(error, TRACEKIT_ERR_LINK_START, kTag, "esp_netif_init() failed (%s)",
                    esp_err_to_name(err));
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {  // the app may already have one
        return fail(error, TRACEKIT_ERR_LINK_START, kTag,
                    "esp_event_loop_create_default() failed (%s)", esp_err_to_name(err));
    }
    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == nullptr &&
        esp_netif_create_default_wifi_sta() == nullptr) {
        return fail(error, TRACEKIT_ERR_LINK_START, kTag, "could not create the Wi-Fi station netif");
    }

#if CONFIG_SPIRAM && !CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP
    // The Wi-Fi and lwIP buffers are the largest heap user of a TCP device,
    // and only the project's sdkconfig can send them to PSRAM.
    if (psram_policy() != Psram::Off && psram_available()) {
        ESP_LOGI(kTag, "this board has PSRAM: CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y moves the Wi-Fi and lwIP "
                       "buffers there and frees tens of KB of internal RAM");
    }
#endif
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    if (g_buffers == WifiBuffers::Lean) {
        // Reserved at init, in internal RAM, whether used or not. With PSRAM
        // on, the IDF's defaults grow to 16 receive + 16 static transmit
        // buffers (~51 KB); BTP never needs that many in flight.
        constexpr int kStaticRx = 6;
        if (config.static_rx_buf_num > kStaticRx) config.static_rx_buf_num = kStaticRx;
        if (config.rx_ba_win > config.static_rx_buf_num) config.rx_ba_win = config.static_rx_buf_num;
        config.tx_buf_type = 1;  // dynamic: taken while sending, given back after
        if (config.dynamic_tx_buf_num == 0) config.dynamic_tx_buf_num = 32;  // 0 when the sdkconfig chose static
    }
    err = esp_wifi_init(&config);
    if (err != ESP_OK) {
        return fail(error, TRACEKIT_ERR_LINK_START, kTag, "esp_wifi_init() failed (%s)",
                    esp_err_to_name(err));
    }
    // Credentials come from the networks file on every boot; nothing to keep
    // in the driver's own NVS namespace.
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err != ESP_OK) {
        return fail(error, TRACEKIT_ERR_LINK_START, kTag, "could not start Wi-Fi in station mode (%s)",
                    esp_err_to_name(err));
    }
    // Power save adds tens of milliseconds of latency to every packet the
    // device receives -- the terminal and command replies would feel it.
    esp_wifi_set_ps(WIFI_PS_NONE);
    g_started = true;
    return ESP_OK;
}

}  // namespace tracekit
