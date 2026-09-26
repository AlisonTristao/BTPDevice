#include <BtpDevice.h>

#include <btp/version.hpp>
#include <btpdev/version.h>
#include <esp_log.h>

#include "core/ConfigCheck.h"
#include "core/LogTags.h"

// BTP 2.49 is the first release that is an ESP-IDF component on its own
// (docs/PLANO.md D15); 2.48 brought the automatic manifest revision and the
// manifest cache hooks the plan relies on.
static_assert(btp::kLibraryVersionMajor == 2U && btp::kLibraryVersionMinor >= 49U,
              "BTPDevice needs BTP 2.49.0 or newer (2.x)");

using btpdev::ErrorInfo;

namespace {

void apply_log_level() {
    const auto level = static_cast<esp_log_level_t>(CONFIG_BTPDEV_LOG_LEVEL);
    for (const char* tag : btpdev::tag::kAll) {
        esp_log_level_set(tag, level);
    }
}

// Every failure reaches the console the same way: the module's own tag, the
// actionable message, then the code's name for searching.
void log_failure(const ErrorInfo& error) {
    ESP_LOGE(error.module, "%s [%s]", error.message, btpdev::err_name(error.code));
}

}  // namespace

BtpDevice::BtpDevice(const btpdev::DeviceConfig& device,
                     const btpdev::StorageConfig& storage) noexcept
    : device_(device), storage_(storage) {}

esp_err_t BtpDevice::begin() noexcept {
    if (started_) {
        btpdev::fail(error_, BTPDEV_ERR_ALREADY_STARTED, btpdev::tag::kCore,
                     "begin() called twice: the device is already running");
        log_failure(error_);
        return error_.code;
    }
    apply_log_level();

    if (btpdev::check_device_config(device_, error_) != ESP_OK ||
        btpdev::check_storage_config(storage_, error_) != ESP_OK) {
        log_failure(error_);
        return error_.code;
    }
    ESP_LOGI(btpdev::tag::kCore, "BTPDevice %s: \"%s\", link %s",
             BTPDEV_VERSION_STRING, device_.name, btpdev::link_name(device_.link));

    // Phase 1 stops here: storage, key, links and tasks arrive in Phases 2-5.
    btpdev::fail(error_, BTPDEV_ERR_NOT_IMPLEMENTED, btpdev::tag::kCore,
                 "links are not implemented yet (docs/PLANO.md, Phases 2-3)");
    log_failure(error_);
    return error_.code;
}
