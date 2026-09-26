#ifndef BTPDEV_WIFI_NETWORKS_H
#define BTPDEV_WIFI_NETWORKS_H

// The pure parts of the Wi-Fi station: reading the networks file and turning
// the device name into its mDNS hostname. Portable, covered by
// test/host/test_wifi.cpp.

#include <cstddef>
#include <cstdint>

namespace btpdev {

struct WifiNetwork {
    static constexpr std::size_t kMaxSsid = 32U;      // 802.11 limit
    static constexpr std::size_t kMaxPassword = 64U;  // WPA2 passphrase limit

    char ssid[kMaxSsid + 1U] = {};
    char password[kMaxPassword + 1U] = {};
};

struct WifiParseResult {
    std::size_t networks = 0U;        ///< entries written to `out`
    std::size_t skipped_lines = 0U;   ///< malformed / too long / past `max`
};

/// Parses the networks file: one "ssid,password" per line (the first comma
/// splits, so a password may contain commas; an empty password is an open
/// network). Blank lines and lines starting with '#' are ignored. CR, LF and
/// CRLF all end a line. `text` need not be NUL-terminated.
WifiParseResult parse_wifi_networks(const char* text, std::size_t size,
                                    WifiNetwork* out, std::size_t max) noexcept;

/// The mDNS hostname TraceView looks for, derived from the device name with
/// TraceView's exact rule (lib/devices/device.cpp, mdnsHostFromName): trimmed,
/// lowercase, [a-z0-9] kept, every other run of characters becomes one '-',
/// no leading/trailing '-', at most 63 characters. "Robo 2 (bancada)" ->
/// "robo-2-bancada". `out` needs 64 bytes. False when nothing is left (the
/// name has no letter or digit) or `capacity` is too small.
bool mdns_host_from_name(const char* name, char* out, std::size_t capacity) noexcept;

}  // namespace btpdev

#endif  // BTPDEV_WIFI_NETWORKS_H
