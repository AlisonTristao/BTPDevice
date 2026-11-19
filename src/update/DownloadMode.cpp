#include "update/DownloadMode.h"

#include <sdkconfig.h>

#include <esp_log.h>
#include <esp_rom_sys.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <hal/wdt_hal.h>
#include <soc/rtc.h>
#include <soc/soc.h>

#if CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32C2 || CONFIG_IDF_TARGET_ESP32C3
#include <soc/rtc_cntl_reg.h>
#define BTPDEV_FORCE_DOWNLOAD_RTC_CNTL 1
#elif CONFIG_IDF_TARGET_ESP32C6 || CONFIG_IDF_TARGET_ESP32C61 || CONFIG_IDF_TARGET_ESP32H2
#include <soc/lp_aon_reg.h>
#define BTPDEV_FORCE_DOWNLOAD_LP_AON 1
#endif

#include "core/LogTags.h"

namespace btpdev {

namespace {

constexpr const char* kTag = tag::kRecovery;
// Time to start the flash before the board gives up and boots normally.
constexpr std::uint32_t kDownloadTimeoutS = kDownloadModeTimeoutS;

void restart_into_download(void*) {
#if BTPDEV_FORCE_DOWNLOAD_RTC_CNTL
#if CONFIG_IDF_TARGET_ESP32S3
    // TinyUSB may hold the USB PHY (it lives in the RTC domain, which a
    // software reset keeps): hand it back to USB-Serial-JTAG, the port the
    // ROM downloads through on the S3.
    CLEAR_PERI_REG_MASK(RTC_CNTL_USB_CONF_REG,
                        RTC_CNTL_SW_HW_USB_PHY_SEL | RTC_CNTL_SW_USB_PHY_SEL | RTC_CNTL_USB_PAD_ENABLE);
#endif
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
#elif BTPDEV_FORCE_DOWNLOAD_LP_AON
    SET_PERI_REG_MASK(LP_AON_SYS_CFG_REG, LP_AON_FORCE_DOWNLOAD_BOOT);
#endif
    // The ROM waits for esptool forever. The RTC watchdog keeps counting in
    // there (esptool disables it when it connects), and an RTC reset also
    // clears the force-download flag: with no flash, the board boots normally
    // after the timeout. Not esp_restart(): it re-arms this watchdog itself
    // with ~2 s, which would end the download mode before anyone connects.
    wdt_hal_context_t rtc_wdt;
    wdt_hal_init(&rtc_wdt, WDT_RWDT, 0, false);
    wdt_hal_write_protect_disable(&rtc_wdt);
    wdt_hal_config_stage(&rtc_wdt, WDT_STAGE0,
                         static_cast<std::uint32_t>(kDownloadTimeoutS * rtc_clk_slow_freq_get_hz()),
                         WDT_STAGE_ACTION_RESET_RTC);
    wdt_hal_enable(&rtc_wdt);
    wdt_hal_write_protect_enable(&rtc_wdt);
    esp_rom_software_reset_system();
}

}  // namespace

bool download_mode_supported() noexcept {
#if BTPDEV_FORCE_DOWNLOAD_RTC_CNTL || BTPDEV_FORCE_DOWNLOAD_LP_AON
    return true;
#else
    return false;
#endif
}

esp_err_t enter_download_mode(std::uint32_t delay_ms) noexcept {
    if (!download_mode_supported()) return ESP_ERR_NOT_SUPPORTED;
    ESP_LOGW(kTag, "restarting into the ROM download mode: flash over USB within %u s", static_cast<unsigned>(kDownloadTimeoutS));
    esp_timer_create_args_t args{};
    args.callback = &restart_into_download;
    args.name = "btpdev_dl";
    esp_timer_handle_t timer = nullptr;
    if (esp_timer_create(&args, &timer) != ESP_OK || esp_timer_start_once(timer, delay_ms * 1000ULL) != ESP_OK) {
        restart_into_download(nullptr);
    }
    return ESP_OK;
}

}  // namespace btpdev
