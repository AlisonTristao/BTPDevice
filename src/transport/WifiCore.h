#ifndef TRACEKIT_WIFI_CORE_H
#define TRACEKIT_WIFI_CORE_H

// Brings the Wi-Fi driver up in station mode, once, for whichever link needs
// the radio (TCP connects the station to an AP; ESP-NOW only needs it
// started). Idempotent: a second call returns ESP_OK without touching
// anything.
//
// It does NOT erase NVS when nvs_flash_init() fails: the device key lives in
// NVS (docs/PLANO.md 4.9), and wiping it to make Wi-Fi calibration happy
// would silently de-provision the device. A failing NVS only costs the PHY
// calibration cache, so it is a warning.

#include <tracekit/config.h>
#include <tracekit/error.h>

namespace tracekit {

/// Before the first wifi_core_start(): how many buffers the driver reserves.
void wifi_core_set_buffers(WifiBuffers buffers) noexcept;
esp_err_t wifi_core_start(ErrorInfo& error) noexcept;
bool wifi_core_started() noexcept;

}  // namespace tracekit

#endif  // TRACEKIT_WIFI_CORE_H
