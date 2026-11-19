#ifndef BTPDEV_DOWNLOAD_MODE_H
#define BTPDEV_DOWNLOAD_MODE_H

// `btp -bootloader`: restart into the ROM's download mode, the state the BOOT
// button gives, so a USB flash (`pio run -t upload`, esptool) works without
// touching the board. Useful when esptool cannot reset the chip by itself:
// the native USB taken by TinyUSB (a CDC or Mass Storage app), or a board
// without the auto-reset circuit.
//
// With no flash, the board boots normally after kDownloadModeTimeoutS (the
// RTC watchdog, which esptool switches off when it connects). esptool also
// clears the flag when it resets after flashing.

#include <cstdint>

#include <esp_err.h>

namespace btpdev {

/// Seconds the ROM waits for esptool before the board boots normally again.
constexpr std::uint32_t kDownloadModeTimeoutS = 30U;

/// Whether this chip can force its download mode from software (S2, S3, C2,
/// C3, C6, C61, H2). Not the original ESP32: hold BOOT and press EN there.
bool download_mode_supported() noexcept;

/// Sets the force-download flag and restarts after `delay_ms` (so a
/// command's reply can leave first). ESP_ERR_NOT_SUPPORTED on other chips.
esp_err_t enter_download_mode(std::uint32_t delay_ms) noexcept;

}  // namespace btpdev

#endif  // BTPDEV_DOWNLOAD_MODE_H
