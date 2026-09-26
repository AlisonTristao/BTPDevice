#include "core/ConfigCheck.h"
#include "core/LogTags.h"

#include <cstring>

namespace btpdev {
namespace {

constexpr const char* kTag = tag::kCore;

bool is_valid_link(Link link) noexcept {
    switch (link) {
        case Link::Serial:
        case Link::Tcp:
        case Link::Ble:
        case Link::EspNow:
        case Link::FromSettings:
            return true;
    }
    return false;
}

}  // namespace

const char* link_name(Link link) noexcept {
    switch (link) {
        case Link::Serial: return "Serial";
        case Link::Tcp: return "TCP";
        case Link::Ble: return "BLE";
        case Link::EspNow: return "ESP-NOW";
        case Link::FromSettings: return "FromSettings";
    }
    return "?";
}

esp_err_t check_device_config(const DeviceConfig& config, ErrorInfo& out) noexcept {
    if (config.name == nullptr || config.name[0] == '\0') {
        return fail(out, BTPDEV_ERR_CONFIG, kTag,
                    "device name is empty: set DeviceConfig::name (it is how "
                    "TraceView finds the device)");
    }
    const std::size_t name_length = std::strlen(config.name);
    if (name_length > DeviceConfig::kMaxNameLength) {
        return fail(out, BTPDEV_ERR_CONFIG, kTag,
                    "device name \"%s\" has %u characters, max is %u (BLE "
                    "advertising limit)",
                    config.name, static_cast<unsigned>(name_length),
                    static_cast<unsigned>(DeviceConfig::kMaxNameLength));
    }
    for (std::size_t i = 0; i < name_length; ++i) {
        const unsigned char c = static_cast<unsigned char>(config.name[i]);
        // Printable ASCII only: the name goes into a BLE scan response, a USB
        // string descriptor and (sanitized) an mDNS hostname.
        if (c < 0x20U || c > 0x7EU) {
            return fail(out, BTPDEV_ERR_CONFIG, kTag,
                        "device name has a non-printable or non-ASCII "
                        "character at position %u",
                        static_cast<unsigned>(i));
        }
    }
    if (!is_valid_link(config.link)) {
        return fail(out, BTPDEV_ERR_CONFIG, kTag, "DeviceConfig::link is not a valid Link");
    }
    if (!is_valid_link(config.fallback) || config.fallback == Link::FromSettings) {
        return fail(out, BTPDEV_ERR_CONFIG, kTag,
                    "DeviceConfig::fallback must be a concrete link (Serial, "
                    "Tcp, Ble or EspNow), not FromSettings");
    }
    if (config.comms.core < -1 || config.comms.core > 1) {
        return fail(out, BTPDEV_ERR_CONFIG, kTag,
                    "DeviceConfig::comms.core is %d: use 0, 1 or -1 (no affinity)",
                    config.comms.core);
    }
    if (config.tcp.port == 0U) {
        return fail(out, BTPDEV_ERR_CONFIG, kTag, "DeviceConfig::tcp.port is 0");
    }
    if (config.espnow.channel < 1U || config.espnow.channel > 14U) {
        return fail(out, BTPDEV_ERR_CONFIG, kTag,
                    "DeviceConfig::espnow.channel is %u: Wi-Fi channels are 1..14",
                    static_cast<unsigned>(config.espnow.channel));
    }
    // source_info: entries are used front to back; an entry needs key and
    // value (label may be empty), and a gap would silently hide the rest.
    bool ended = false;
    for (std::size_t i = 0; i < DeviceConfig::kMaxSourceInfo; ++i) {
        const SourceInfo& entry = config.source_info[i];
        if (entry.key == nullptr) {
            ended = true;
            continue;
        }
        if (ended) {
            return fail(out, BTPDEV_ERR_CONFIG, kTag,
                        "DeviceConfig::source_info[%u] comes after an empty "
                        "entry: fill the array from the start",
                        static_cast<unsigned>(i));
        }
        if (entry.key[0] == '\0' || entry.value == nullptr) {
            return fail(out, BTPDEV_ERR_CONFIG, kTag,
                        "DeviceConfig::source_info[%u] needs a key and a value",
                        static_cast<unsigned>(i));
        }
    }
    clear(out);
    return ESP_OK;
}

esp_err_t check_storage_config(const StorageConfig& config, ErrorInfo& out) noexcept {
    constexpr const char* kFsTag = tag::kFs;
    if (config.fs.partition == nullptr || config.fs.partition[0] == '\0') {
        return fail(out, BTPDEV_ERR_CONFIG, kFsTag,
                    "StorageConfig::fs.partition is empty: name the LittleFS "
                    "partition from partitions.csv");
    }
    if (config.fs.mount == nullptr || config.fs.mount[0] != '/') {
        return fail(out, BTPDEV_ERR_CONFIG, kFsTag,
                    "StorageConfig::fs.mount must be an absolute path such as \"/fs\"");
    }
    if (config.key == KeySource::Nvs &&
        (config.key_file == nullptr || config.key_file[0] == '\0')) {
        return fail(out, BTPDEV_ERR_CONFIG, tag::kKey,
                    "StorageConfig::key_file is empty while key = Nvs");
    }
    if (config.sd.enabled) {
        if (config.sd.mount == nullptr || config.sd.mount[0] != '/') {
            return fail(out, BTPDEV_ERR_CONFIG, kFsTag,
                        "StorageConfig::sd.mount must be an absolute path such as \"/sd\"");
        }
        if (std::strcmp(config.sd.mount, config.fs.mount) == 0) {
            return fail(out, BTPDEV_ERR_CONFIG, kFsTag,
                        "StorageConfig::sd.mount and fs.mount are both \"%s\"",
                        config.sd.mount);
        }
        if (config.sd.mode == SdMode::Spi &&
            (config.sd.cs < 0 || config.sd.mosi < 0 || config.sd.miso < 0 ||
             config.sd.sck < 0)) {
            return fail(out, BTPDEV_ERR_CONFIG, kFsTag,
                        "SD over SPI needs cs, mosi, miso and sck pins");
        }
    }
    clear(out);
    return ESP_OK;
}

}  // namespace btpdev
