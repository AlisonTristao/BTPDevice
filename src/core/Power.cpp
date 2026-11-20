#include "core/Power.h"

#include <cstdio>
#include <cstring>

#include <esp_clk_tree.h>
#include <esp_log.h>
#include <esp_pm.h>
#include <esp_wifi.h>
#include <sdkconfig.h>

#include "core/LogTags.h"
#include "transport/WifiCore.h"

namespace btpdev {

namespace {
constexpr const char* kTag = tag::kCore;
}  // namespace

const char* power_mode_name(PowerMode mode) noexcept {
    return mode == PowerMode::Saving ? "saving" : "performance";
}

void Power::begin(const PowerConfig& config, bool wifi, bool serial, std::uint32_t period_ms) noexcept {
    config_ = config;
    period_ms_ = period_ms;
    wifi_ = wifi;
    serial_ = serial;
#if CONFIG_PM_ENABLE
    if (serial_ && serial_lock_ == nullptr) {
        esp_pm_lock_handle_t lock = nullptr;
        if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "btpdev_serial", &lock) == ESP_OK) {
            esp_pm_lock_acquire(lock);  // held for good: the port is always listening
            serial_lock_ = lock;
        }
    }
#endif
    set(config.mode);
}

PowerMode Power::set(PowerMode mode) noexcept {
    mode_ = mode;
    const bool saving = mode == PowerMode::Saving;
    if (wifi_ && wifi_core_started()) {
        // Modem sleep: the radio sleeps between the AP's beacons. MIN_MODEM
        // wakes for every DTIM beacon (~100-300 ms): connected, a bit slower.
        esp_wifi_set_ps(saving ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
    }
#if CONFIG_PM_ENABLE
    std::uint32_t xtal_hz = 40000000U;
    esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_XTAL, ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &xtal_hz);
    esp_pm_config_t pm{};
    pm.max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    pm.min_freq_mhz = saving ? static_cast<int>(xtal_hz / 1000000U) : CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
#if CONFIG_FREERTOS_USE_TICKLESS_IDLE
    pm.light_sleep_enable = saving;
#else
    pm.light_sleep_enable = false;
#endif
    const esp_err_t err = esp_pm_configure(&pm);
    light_sleep_ = err == ESP_OK && pm.light_sleep_enable && !serial_;
    if (err != ESP_OK) ESP_LOGW(kTag, "esp_pm_configure: %s", esp_err_to_name(err));
#if !CONFIG_FREERTOS_USE_TICKLESS_IDLE
    if (saving) {
        ESP_LOGW(kTag, "PowerMode::Saving without light sleep: add CONFIG_FREERTOS_USE_TICKLESS_IDLE=y to the sdkconfig");
    }
#endif
#else
    light_sleep_ = false;
    if (saving) {
        ESP_LOGW(kTag, "PowerMode::Saving: only Wi-Fi modem sleep. For light sleep and a slower idle CPU add "
                       "CONFIG_PM_ENABLE=y and CONFIG_FREERTOS_USE_TICKLESS_IDLE=y to the sdkconfig");
    }
#endif
    return mode_;
}

std::uint32_t Power::idle_ms(std::uint32_t period_ms) const noexcept {
    if (mode_ != PowerMode::Saving) return period_ms;
    return period_ms > config_.idle_ms ? period_ms : config_.idle_ms;
}

void Power::describe(char* out, std::size_t size) const noexcept {
    const bool saving = mode_ == PowerMode::Saving;
    int written = std::snprintf(
        out, size,
        "mode      %s\n"
        "wifi      %s\n"
        "light     %s\n"
        "pause     %lu ms between passes of the communication task\n",
        power_mode_name(mode_),
        !wifi_ ? "not used by this link" : (saving ? "modem sleep (wakes for beacons)" : "always listening"),
        light_sleep_ ? "automatic light sleep when idle"
        : serial_    ? "never (Serial link)"
#if CONFIG_PM_ENABLE && CONFIG_FREERTOS_USE_TICKLESS_IDLE
                     : "off",
#else
                     : "unavailable (CONFIG_PM_ENABLE + CONFIG_FREERTOS_USE_TICKLESS_IDLE)",
#endif
        static_cast<unsigned long>(idle_ms(period_ms_)));
#if CONFIG_PM_ENABLE && CONFIG_PM_PROFILING
    // The time spent in each mode since boot (CONFIG_PM_PROFILING).
    if (written > 0 && static_cast<std::size_t>(written) < size) {
        char dump[1024];
        FILE* f = fmemopen(dump, sizeof(dump) - 1U, "w");
        if (f != nullptr) {
            esp_pm_dump_locks(f);
            const long n = ftell(f);
            std::fclose(f);
            dump[n > 0 && n < static_cast<long>(sizeof(dump)) ? n : 0] = '\0';
            const char* stats = std::strstr(dump, "Mode stats");
            if (stats != nullptr) std::snprintf(out + written, size - static_cast<std::size_t>(written), "%s", stats);
        }
    }
#else
    (void)written;
#endif
}

}  // namespace btpdev
