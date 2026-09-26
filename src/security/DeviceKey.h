#ifndef BTPDEV_DEVICE_KEY_H
#define BTPDEV_DEVICE_KEY_H

// The device's two keys (E for TraceView <-> device, L for dongle <-> device;
// include/btpdev/bally_channels.h), kept in NVS (docs/PLANO.md 4.9, D14).
//
// Provisioning: tools/provision_key.py writes the 96-byte key file; the user
// copies it to the device's file system (LittleFS or SD root). At boot -- or
// on `btp -key_import` -- the file is validated (KeyStore: format, CRC, the
// verify tag of each key), stored in NVS, and renamed to "<name>.imported"
// so it is not imported again. A rejected file is left in place and the
// error names the field that is wrong ("verify_e": wrong password E).
//
// SECURITY RULE (from bally_OS, kept): the keys are never printed, logged or
// traced, not even partially. Diagnostics show verify_e / verify_l, which are
// public by construction.

#include <btpdev/error.h>
#include <btpdev/filesystem.h>

#include <cstddef>
#include <cstdint>

#include "security/KeyStore.h"

namespace btpdev {

class DeviceKey {
public:
    /// Loads the key saved in NVS, if any. Quiet when there is none.
    void load_saved() noexcept;

    /// Imports `name` from `fs` if the file exists: ESP_OK when imported,
    /// ESP_ERR_NOT_FOUND when there is no file, BTPDEV_ERR_BAD_KEY (logged,
    /// with the field) when it is invalid, BTPDEV_ERR_STORAGE when NVS failed.
    esp_err_t import_file(FileSystem& fs, const char* name, ErrorInfo& error) noexcept;

    /// Erases the key from NVS and RAM.
    esp_err_t forget() noexcept;

    bool loaded() const noexcept { return store_.loaded(); }
    const std::uint8_t* key_e() const noexcept { return store_.key_e(); }
    const std::uint8_t* key_l() const noexcept { return store_.key_l(); }

    /// "verify_e 1a2b3c4d5e6f7a8b, verify_l ..." (public tags only), or "no key".
    void describe(char* out, std::size_t capacity) const noexcept;

private:
    KeyStore store_;
};

}  // namespace btpdev

#endif  // BTPDEV_DEVICE_KEY_H
