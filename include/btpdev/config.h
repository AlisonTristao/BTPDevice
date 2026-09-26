#ifndef BTPDEV_CONFIG_H
#define BTPDEV_CONFIG_H

// The two configuration structs a BtpDevice is built from: DeviceConfig
// (identity, link, memory, network) and StorageConfig (file system, key).
// Users fill them in their btp_device.h / btp_storage.h with C++20
// designated initializers:
//
//   const btpdev::DeviceConfig device_config = {
//       .name = "weather-station",
//       .link = btpdev::Link::Tcp,
//   };
//
// Every field has a sensible default, so only what differs is written.
// Designated initializers must follow declaration order: fields below are
// ordered from "almost always set" to "rarely set" for that reason.

#include <cstddef>
#include <cstdint>

namespace btpdev {

/// How the device talks to TraceView. One link per boot (docs/PLANO.md D4).
enum class Link : std::uint8_t {
    Serial,        ///< USB CDC (or UART), 0x00 || COBS || 0x00 framing
    Tcp,           ///< Wi-Fi station + TCP server, found as <name>.local
    Ble,           ///< NimBLE peripheral with the BTP GATT service
    EspNow,        ///< ESP-NOW datagrams through the bally_dongle
    FromSettings,  ///< the link saved in NVS by set_link(); `fallback` if none
};

/// Memory budget preset (docs/PLANO.md section 4.11, D18).
enum class Memory : std::uint8_t {
    Small,   ///< a sensor with a few topics
    Medium,  ///< default
    Large,   ///< many topics / fields, big manifest
};

/// Who drives the communication loop.
enum class Threading : std::uint8_t {
    Auto,    ///< the library runs its own task; the user calls nothing
    Manual,  ///< the user calls BtpDevice::routine() from their own loop
};

/// The library's communication task.
struct TaskConfig {
    int core = 0;                        ///< 0 or 1; -1 = no affinity
    std::uint32_t priority = 10U;
    std::uint32_t stack_bytes = 6144U;
    std::uint32_t period_ms = 1U;        ///< pause between two routine() passes
};

struct WifiConfig {
    /// Networks file in the file system root, one "ssid,password" per line.
    const char* file = "wifi.txt";
};

struct TcpConfig {
    std::uint16_t port = 44300U;         ///< TraceView's default
};

struct EspNowConfig {
    std::uint8_t peer_mac[6] = {};       ///< the dongle's MAC
    std::uint8_t channel = 1U;           ///< Wi-Fi channel shared with the dongle
};

/// One extra entry of the manifest's source_info block (shown by TraceView on
/// the device card). `key` is a stable machine key, `label` a human label.
struct SourceInfo {
    const char* key = nullptr;
    const char* label = nullptr;
    const char* value = nullptr;
};

struct DeviceConfig {
    /// Longest name BLE can advertise (31-octet scan response minus the
    /// 2-octet AD header). The same name feeds mDNS, USB and the manifest.
    static constexpr std::size_t kMaxNameLength = 29U;
    /// source_info entries beyond the ones the library adds itself.
    static constexpr std::size_t kMaxSourceInfo = 8U;

    /// Required. 1..kMaxNameLength printable ASCII characters.
    const char* name = nullptr;
    Link link = Link::Serial;
    /// Used by Link::FromSettings when nothing is saved yet. Never FromSettings.
    Link fallback = Link::Serial;
    Memory memory = Memory::Medium;
    Threading threading = Threading::Auto;
    TaskConfig comms{};
    WifiConfig wifi{};
    TcpConfig tcp{};
    EspNowConfig espnow{};
    /// Unused entries keep key == nullptr.
    SourceInfo source_info[kMaxSourceInfo] = {};
};

/// Where the provisioned key lives once imported (docs/PLANO.md section 4.9).
enum class KeySource : std::uint8_t {
    Nvs,   ///< imported once from `key_file`, then read from NVS
    None,  ///< no key at all: links that need one fall back to Serial
};

/// LittleFS on a flash partition (docs/PLANO.md D5).
struct FsConfig {
    const char* partition = "storage";   ///< label in partitions.csv
    const char* mount = "/fs";
    bool format_if_empty = true;
};

enum class SdMode : std::uint8_t { Spi, Sdmmc };

/// Optional SD card, a second root for the same files.
struct SdConfig {
    bool enabled = false;
    SdMode mode = SdMode::Spi;
    int cs = -1;
    int mosi = -1;
    int miso = -1;
    int sck = -1;
    const char* mount = "/sd";
};

struct StorageConfig {
    FsConfig fs{};
    KeySource key = KeySource::Nvs;
    /// Key file looked for at boot (96 octets, tools/provision_key.py format).
    const char* key_file = "bally.key";
    SdConfig sd{};
};

}  // namespace btpdev

#endif  // BTPDEV_CONFIG_H
