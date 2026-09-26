#ifndef BTPDEV_MOUNT_H
#define BTPDEV_MOUNT_H

// Mounting the two storage backends into ESP-IDF's VFS (docs/PLANO.md 4.10).
// After a successful mount the FileSystem is attached to the mount point and
// knows how to report the volume's usage.
//
//   LittleFS  a flash partition (StorageConfig::fs.partition), formatted on
//             first use when fs.format_if_empty is set. Survives power loss
//             mid-write, which is why it is the default (D5).
//   SD card   SPI (cs, mosi, miso, sck) or SDMMC 1-bit (sck = CLK,
//             mosi = CMD, miso = D0), FAT. Never formatted by the library:
//             a card may hold someone's data.

#include <btpdev/config.h>
#include <btpdev/error.h>
#include <btpdev/filesystem.h>

namespace btpdev {

esp_err_t mount_littlefs(const FsConfig& config, FileSystem& fs, ErrorInfo& error) noexcept;
esp_err_t mount_sd(const SdConfig& config, FileSystem& fs, ErrorInfo& error) noexcept;
/// A volume the application mounted (StorageConfig::app_volume): only attached,
/// with the usage of a FAT volume when it is one.
void attach_app_volume(const char* root, FileSystem& fs) noexcept;

}  // namespace btpdev

#endif  // BTPDEV_MOUNT_H
