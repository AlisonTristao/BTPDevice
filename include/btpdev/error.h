#ifndef BTPDEV_ERROR_H
#define BTPDEV_ERROR_H

// Error codes and the "last error" record every BtpDevice operation reports
// through (docs/PLANO.md section 4.12).
//
// Codes are esp_err_t, the ESP-IDF convention, so they compose with the rest
// of an application: IDF codes pass through unchanged, and the BtpDevice-
// specific ones live in their own range (BTPDEV_ERR_BASE), which no ESP-IDF
// 6.0 component uses (0x7000 is HTTP, 0x9000 HTTPS OTA, 0xB000 HTTPD).
//
// This header also builds on the host (test/host): esp_err.h is ESP-IDF
// only, so a minimal stand-in with the same values is used there.

#include <cstddef>

#if defined(ESP_PLATFORM)
#include <esp_err.h>
#else
// Host stand-in for <esp_err.h>. Values are identical to ESP-IDF's, so a code
// computed by portable code means the same thing on both sides.
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_TIMEOUT 0x107
#endif

#define BTPDEV_ERR_BASE 0xA000
/// DeviceConfig / StorageConfig is invalid (empty name, bad link, ...).
#define BTPDEV_ERR_CONFIG (BTPDEV_ERR_BASE + 0x01)
/// The requested link is not compiled in (Kconfig) or not supported by this chip.
#define BTPDEV_ERR_LINK_UNSUPPORTED (BTPDEV_ERR_BASE + 0x02)
/// The link was compiled in but failed to start (driver, Wi-Fi, NimBLE...).
#define BTPDEV_ERR_LINK_START (BTPDEV_ERR_BASE + 0x03)
/// The memory preset does not fit in the free heap.
#define BTPDEV_ERR_MEMORY_BUDGET (BTPDEV_ERR_BASE + 0x04)
/// The file system could not be mounted (nor formatted, when allowed).
#define BTPDEV_ERR_STORAGE (BTPDEV_ERR_BASE + 0x05)
/// The link needs a key and none is provisioned.
#define BTPDEV_ERR_NO_KEY (BTPDEV_ERR_BASE + 0x06)
/// A key file was found but rejected (size, magic, CRC, verify tag...).
#define BTPDEV_ERR_BAD_KEY (BTPDEV_ERR_BASE + 0x07)
/// A topic declaration is invalid (duplicate name, bad field, pool too small).
#define BTPDEV_ERR_BAD_TOPIC (BTPDEV_ERR_BASE + 0x08)
/// A command declaration is invalid (duplicate name, table full).
#define BTPDEV_ERR_BAD_COMMAND (BTPDEV_ERR_BASE + 0x09)
/// begin() was called on a device that is already running.
#define BTPDEV_ERR_ALREADY_STARTED (BTPDEV_ERR_BASE + 0x0A)
/// Not implemented yet (only while the library is being built, see PLANO.md).
#define BTPDEV_ERR_NOT_IMPLEMENTED (BTPDEV_ERR_BASE + 0x0B)

namespace btpdev {

/// The most recent failure, as begin() and the other fallible calls leave it.
/// `module` is the log tag of the module that failed ("btpdev.key", ...), and
/// `message` says what happened, the consequence and how to fix it.
struct ErrorInfo {
    static constexpr std::size_t kMessageSize = 128U;

    esp_err_t code = ESP_OK;
    const char* module = "";
    char message[kMessageSize] = {};

    bool ok() const noexcept { return code == ESP_OK; }
};

/// Records a failure into `out` (printf-style message, truncated to fit) and
/// returns `code`, so a caller can write `return fail(err, CODE, tag, ...)`.
/// `module` must be a string literal (it is stored as a pointer).
esp_err_t fail(ErrorInfo& out, esp_err_t code, const char* module,
               const char* format, ...) noexcept
#if defined(__GNUC__)
    __attribute__((format(printf, 4, 5)))
#endif
    ;

/// Resets `out` to "no error".
void clear(ErrorInfo& out) noexcept;

/// Name of a BTPDEV_ERR_* code ("BTPDEV_ERR_NO_KEY"). For any other code, the
/// ESP-IDF name on the target (esp_err_to_name) or a generic one on the host.
/// Never returns nullptr.
const char* err_name(esp_err_t code) noexcept;

}  // namespace btpdev

#endif  // BTPDEV_ERROR_H
