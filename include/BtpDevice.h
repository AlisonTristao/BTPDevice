#ifndef BTP_DEVICE_H
#define BTP_DEVICE_H

// BtpDevice -- the one object an application needs to talk to TraceView over
// BTP. See README.md for the target API and docs/PLANO.md for the plan.
//
// Status: Phase 1 skeleton. begin() validates the configuration and then
// reports BTPDEV_ERR_NOT_IMPLEMENTED; links, topics and commands arrive in
// Phases 2-4.

#include <btpdev/config.h>
#include <btpdev/error.h>

class BtpDevice {
public:
    /// Both configs are copied; they do not need to outlive the device.
    BtpDevice(const btpdev::DeviceConfig& device,
              const btpdev::StorageConfig& storage) noexcept;

    BtpDevice(const BtpDevice&) = delete;
    BtpDevice& operator=(const BtpDevice&) = delete;

    /// Brings the device up (storage, key, link, tasks). Returns ESP_OK, or
    /// the failure also recorded in last_error() and printed with ESP_LOGE.
    /// Call once, from app_main() or any task.
    esp_err_t begin() noexcept;

    /// The most recent failure (code == ESP_OK when there was none).
    const btpdev::ErrorInfo& last_error() const noexcept { return error_; }

    const btpdev::DeviceConfig& device_config() const noexcept { return device_; }
    const btpdev::StorageConfig& storage_config() const noexcept { return storage_; }

private:
    btpdev::DeviceConfig device_;
    btpdev::StorageConfig storage_;
    btpdev::ErrorInfo error_;
    bool started_ = false;
};

#endif  // BTP_DEVICE_H
