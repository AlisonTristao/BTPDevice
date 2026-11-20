#include "update/Recovery.h"

#include <cstdio>
#include <cstring>

#include <esp_attr.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <nvs.h>

#include "core/LogTags.h"

namespace tracekit {
namespace {

constexpr const char* kTag = tag::kRecovery;
constexpr std::uint32_t kMagic = 0xB7D0C0A5U;

// Survives a panic or a watchdog reset, not a power cycle: exactly the resets
// a crash loop is made of. The magic tells garbage after power-on apart.
RTC_NOINIT_ATTR std::uint32_t s_magic;
RTC_NOINIT_ATTR std::uint32_t s_crashes;
// Set right before a reset the library causes on purpose through a watchdog
// (btp -bootloader's timeout): the next boot does not count it as a crash.
RTC_NOINIT_ATTR std::uint32_t s_expected;
constexpr std::uint32_t kExpectedMagic = 0xB7D0E5E7U;

bool abnormal(esp_reset_reason_t reason) noexcept {
    switch (reason) {
        case ESP_RST_PANIC:
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:
            return true;
        default:
            return false;
    }
}

const char* reset_name(esp_reset_reason_t reason) noexcept {
    switch (reason) {
        case ESP_RST_POWERON: return "power-on";
        case ESP_RST_EXT: return "external pin";
        case ESP_RST_SW: return "software";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "interrupt watchdog";
        case ESP_RST_TASK_WDT: return "task watchdog";
        case ESP_RST_WDT: return "watchdog";
        case ESP_RST_DEEPSLEEP: return "deep sleep";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_USB: return "USB";
        default: return "other";
    }
}

void restart_after(std::uint32_t delay_ms) noexcept {
    if (delay_ms == 0U) esp_restart();
    esp_timer_create_args_t args{};
    args.callback = [](void*) { esp_restart(); };
    args.name = "tracekit_recovery";
    esp_timer_handle_t timer = nullptr;
    if (esp_timer_create(&args, &timer) != ESP_OK ||
        esp_timer_start_once(timer, static_cast<std::uint64_t>(delay_ms) * 1000U) != ESP_OK) {
        esp_restart();
    }
}

}  // namespace

void Recovery::expect_reset() noexcept { s_expected = kExpectedMagic; }

std::uint32_t Recovery::crash_count() const noexcept { return s_magic == kMagic ? s_crashes : 0U; }

void Recovery::find_partitions() noexcept {
    app_ = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
    recovery_ = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
    const esp_partition_t* running = esp_ota_get_running_partition();
    available_ = false;
    if (recovery_ == nullptr || app_ == nullptr) {
        std::snprintf(why_, sizeof(why_), "no recovery partition (use partitions/tracekit_<size>.csv)");
        return;
    }
    if (running != app_) {
        std::snprintf(why_, sizeof(why_), "the application is not running from the factory partition");
        return;
    }
    esp_app_desc_t desc{};
    if (esp_ota_get_partition_description(recovery_, &desc) != ESP_OK) {
        std::snprintf(why_, sizeof(why_), "the recovery partition is empty (flash it: tools/pio_recovery.py)");
        return;
    }
    std::snprintf(recovery_version_, sizeof(recovery_version_), "%s", desc.version);
    available_ = true;
    why_[0] = '\0';
}

void Recovery::boot(const RecoveryConfig& config) noexcept {
    config_ = config;
    find_partitions();
    if (!config_.enabled) return;
    if (recovery_ == nullptr) {
        ESP_LOGI(kTag, "no recovery partition: updates over Wi-Fi are off (partitions/tracekit_<size>.csv adds one)");
        return;
    }
    if (!available_) {
        ESP_LOGW(kTag, "%s", why_);
        return;
    }

    const esp_reset_reason_t reason = esp_reset_reason();
    if (s_magic != kMagic) {
        s_magic = kMagic;
        s_crashes = 0U;
    }
    const bool expected = s_expected == kExpectedMagic;
    s_expected = 0U;
    if (!expected) s_crashes = abnormal(reason) ? s_crashes + 1U : 0U;
    if (s_crashes != 0U) {
        ESP_LOGW(kTag, "reset by %s: %lu abnormal reset(s) in a row (recovery at %u)", reset_name(reason),
                 static_cast<unsigned long>(s_crashes), static_cast<unsigned>(config_.crash_loop));
    }
    if (config_.crash_loop != 0U && s_crashes >= config_.crash_loop) {
        ESP_LOGE(kTag, "crash loop: %lu resets in a row before the application became stable. "
                       "Entering the recovery to wait for a new firmware.",
                 static_cast<unsigned long>(s_crashes));
        s_crashes = 0U;
        enter(recovery::Reason::CrashLoop, 0U);
        // Only if the recovery could not be selected: carry on and hope.
        ESP_LOGE(kTag, "could not switch to the recovery");
    }
}

void Recovery::arm_stable_timer() noexcept {
    if (!available_ || !config_.enabled || crash_count() == 0U) return;
    esp_timer_create_args_t args{};
    args.callback = [](void*) {
        s_crashes = 0U;
        ESP_LOGI(tag::kRecovery, "stable: crash count cleared");
    };
    args.name = "tracekit_stable";
    esp_timer_handle_t timer = nullptr;
    if (esp_timer_create(&args, &timer) == ESP_OK) {
        esp_timer_start_once(timer, static_cast<std::uint64_t>(config_.stable_ms) * 1000U);
    }
}

void Recovery::mirror(const char* name, const WifiNetwork* networks, std::size_t count) noexcept {
    if (recovery_ == nullptr || !config_.enabled) return;
    recovery::WifiBlob blob{};
    std::memset(&blob, 0, sizeof(blob));
    for (std::size_t i = 0U; i < count && blob.count < recovery::WifiBlob::kMaxNetworks; ++i) {
        std::snprintf(blob.networks[blob.count].ssid, recovery::WifiBlob::kSsid, "%s", networks[i].ssid);
        std::snprintf(blob.networks[blob.count].password, recovery::WifiBlob::kPassword, "%s", networks[i].password);
        ++blob.count;
    }

    nvs_handle_t h = 0;
    if (nvs_open(recovery::kNamespace, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(kTag, "NVS namespace \"%s\" could not be opened: the recovery will not know the Wi-Fi",
                 recovery::kNamespace);
        return;
    }
    bool changed = false;
    std::uint8_t abi = 0U;
    if (nvs_get_u8(h, recovery::kKeyAbi, &abi) != ESP_OK || abi != recovery::kAbi) {
        changed |= nvs_set_u8(h, recovery::kKeyAbi, recovery::kAbi) == ESP_OK;
    }
    char saved_name[recovery::WifiBlob::kSsid] = "";
    std::size_t size = sizeof(saved_name);
    if (nvs_get_str(h, recovery::kKeyName, saved_name, &size) != ESP_OK || std::strcmp(saved_name, name) != 0) {
        changed |= nvs_set_str(h, recovery::kKeyName, name) == ESP_OK;
    }
    // Only when the application knows some network: a Serial-only build must
    // not wipe what an earlier TCP build left for the recovery.
    if (blob.count != 0U) {
        static recovery::WifiBlob saved;
        size = sizeof(saved);
        if (nvs_get_blob(h, recovery::kKeyWifi, &saved, &size) != ESP_OK || size != sizeof(saved) ||
            std::memcmp(&saved, &blob, sizeof(blob)) != 0) {
            changed |= nvs_set_blob(h, recovery::kKeyWifi, &blob, sizeof(blob)) == ESP_OK;
        }
        std::memset(&saved, 0, sizeof(saved));
    }
    if (changed) {
        nvs_commit(h);
        ESP_LOGI(kTag, "recovery settings saved (name, %u network(s))", static_cast<unsigned>(blob.count));
    }
    nvs_close(h);
    std::memset(&blob, 0, sizeof(blob));  // passwords
}

esp_err_t Recovery::enter(recovery::Reason reason, std::uint32_t delay_ms) noexcept {
    if (!available_) return ESP_ERR_NOT_SUPPORTED;
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(recovery::kNamespace, NVS_READWRITE, &h);
    if (err == ESP_OK) err = nvs_set_u8(h, recovery::kKeyReason, static_cast<std::uint8_t>(reason));
    if (err == ESP_OK) err = nvs_commit(h);
    if (h != 0) nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "could not record the reason in NVS: %s", esp_err_to_name(err));
        return err;
    }
    // Validates the recovery image before touching the otadata.
    err = esp_ota_set_boot_partition(recovery_);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "the recovery image did not validate: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGW(kTag, "restarting into the recovery (%s)", recovery::reason_name(reason));
    restart_after(delay_ms);
    return ESP_OK;
}

void Recovery::describe(char* out, std::size_t size) const noexcept {
    const esp_partition_t* running = esp_ota_get_running_partition();
    int used = std::snprintf(out, size, "running   %s (0x%06lx)\n", running != nullptr ? running->label : "?",
                             running != nullptr ? static_cast<unsigned long>(running->address) : 0UL);
    if (used < 0 || static_cast<std::size_t>(used) >= size) return;
    if (recovery_ != nullptr) {
        used += std::snprintf(out + used, size - used, "recovery  %s (0x%06lx, %lu KB) %s%s\n", recovery_->label,
                              static_cast<unsigned long>(recovery_->address),
                              static_cast<unsigned long>(recovery_->size / 1024U),
                              available_ ? "version " : "", available_ ? recovery_version_ : why_);
    } else {
        used += std::snprintf(out + used, size - used, "recovery  none: %s\n", why_);
    }
    if (used < 0 || static_cast<std::size_t>(used) >= size) return;
    if (app_ != nullptr) {
        used += std::snprintf(out + used, size - used, "app slot  %lu KB\n", static_cast<unsigned long>(app_->size / 1024U));
    }
    if (used < 0 || static_cast<std::size_t>(used) >= size) return;
    std::snprintf(out + used, size - used, "reset     %s, crash count %lu of %u%s\n", reset_name(esp_reset_reason()),
                  static_cast<unsigned long>(crash_count()), static_cast<unsigned>(config_.crash_loop),
                  config_.crash_loop == 0U ? " (detector off)" : "");
}

}  // namespace tracekit
