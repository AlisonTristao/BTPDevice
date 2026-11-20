#ifndef TRACEKIT_CONFIG_CHECK_H
#define TRACEKIT_CONFIG_CHECK_H

// Validation of the user's configuration, done first thing in begin(), before
// anything is allocated or started. Portable (no ESP-IDF calls), so the host
// tests cover every rule.

#include <tracekit/config.h>
#include <tracekit/error.h>

namespace tracekit {

/// ESP_OK, or TRACEKIT_ERR_CONFIG with `out` saying which field and why.
esp_err_t check_device_config(const DeviceConfig& config, ErrorInfo& out) noexcept;

/// ESP_OK, or TRACEKIT_ERR_CONFIG with `out` saying which field and why.
esp_err_t check_storage_config(const StorageConfig& config, ErrorInfo& out) noexcept;

/// "Serial", "TCP", "BLE", "ESP-NOW" or "FromSettings". Never nullptr.
const char* link_name(Link link) noexcept;

}  // namespace tracekit

#endif  // TRACEKIT_CONFIG_CHECK_H
