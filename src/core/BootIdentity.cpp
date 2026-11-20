#include <btpdev/identity.h>

#include <esp_log.h>
#include <esp_mac.h>
#include <esp_attr.h>
#include <esp_random.h>
#include <esp_system.h>
#include <nvs.h>
#include <nvs_flash.h>

#include "core/Identity.h"
#include "core/LogTags.h"

namespace btpdev {
namespace {

constexpr const char* kTag = tag::kCore;
constexpr const char* kNvsNamespace = "btpdev";
constexpr std::uint32_t kRtcMagic = 0xB7D0B007U;

// The boot_id block reserved by the last cold boot (Identity.h, D31): kept
// through deep sleep, garbage after a power-up (the magic tells).
RTC_NOINIT_ATTR std::uint32_t s_rtc_magic;
RTC_NOINIT_ATTR std::uint32_t s_rtc_next;
RTC_NOINIT_ATTR std::uint32_t s_rtc_last;

Identity make_identity() noexcept {
    Identity id;
    std::uint8_t mac[6] = {};
    esp_efuse_mac_get_default(mac);
    id.source_id = source_id_from_mac(mac);
    uuid_from_mac(mac, id.peer_uuid);

    // The boot counter. Never erase NVS to get it: the device key lives there
    // (docs/PLANO.md 4.9). Without NVS the boot_id is random, which still
    // tells peers this is a new boot.
    esp_err_t err = nvs_flash_init();
    nvs_handle_t handle = 0;
    if (err == ESP_OK) err = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    std::uint32_t previous = 0U;
    if (err == ESP_OK) {
        const esp_err_t get = nvs_get_u32(handle, "boot", &previous);
        if (get != ESP_OK && get != ESP_ERR_NVS_NOT_FOUND) err = get;
    }
    if (err == ESP_OK) {
        const bool woke = esp_reset_reason() == ESP_RST_DEEPSLEEP && s_rtc_magic == kRtcMagic;
        const BootIdPlan plan = plan_boot_id(woke, s_rtc_next, s_rtc_last, previous);
        id.boot_id = plan.id;
        if (plan.save) {
            err = nvs_set_u32(handle, "boot", plan.saved);  // the block's end, not this id
            if (err == ESP_OK) err = nvs_commit(handle);
        }
        s_rtc_next = plan.rtc_next;
        s_rtc_last = plan.rtc_last;
        s_rtc_magic = kRtcMagic;
    }
    if (handle != 0) nvs_close(handle);
    if (err != ESP_OK) {
        id.boot_id = esp_random() | 1U;
        ESP_LOGW(kTag,
                 "NVS unavailable (%s): boot counter not saved, using a random boot id. "
                 "If NVS is full or from an older layout, erase it (idf.py erase-flash) and "
                 "provision the key again",
                 esp_err_to_name(err));
    }
    return id;
}

}  // namespace

const Identity& identity() noexcept {
    // Function-local static: made once, thread-safe (C++11 magic statics).
    static const Identity id = make_identity();
    return id;
}

}  // namespace btpdev
