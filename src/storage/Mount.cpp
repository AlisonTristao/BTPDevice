#include "storage/Mount.h"

#include <cstring>

#include <esp_littlefs.h>
#include <esp_log.h>
#include <sdkconfig.h>

#include "core/LogTags.h"

extern "C" esp_err_t esp_vfs_fat_info(const char* base_path, std::uint64_t* out_total_bytes,
                                     std::uint64_t* out_free_bytes) __attribute__((weak));

namespace tracekit {
namespace {

constexpr const char* kTag = tag::kFs;

// The partition label outlives the mount: the usage callback needs it.
char g_littlefs_label[17] = {};

bool littlefs_usage(void*, std::uint64_t* total, std::uint64_t* used) {
    std::size_t t = 0U;
    std::size_t u = 0U;
    if (esp_littlefs_info(g_littlefs_label, &t, &u) != ESP_OK) return false;
    *total = t;
    *used = u;
    return true;
}

// context: the mount point (outlives the attachment). Through a weak
// reference: an application volume is FAT only when the application linked
// FATFS itself, and a weak reference never pulls it into one that did not.
bool fat_usage(void* context, std::uint64_t* total, std::uint64_t* used) {
    if (esp_vfs_fat_info == nullptr) return false;
    std::uint64_t t = 0U;
    std::uint64_t free_bytes = 0U;
    if (esp_vfs_fat_info(static_cast<const char*>(context), &t, &free_bytes) != ESP_OK) return false;
    *total = t;
    *used = t - free_bytes;
    return true;
}

}  // namespace

esp_err_t mount_littlefs(const FsConfig& config, FileSystem& fs, ErrorInfo& error) noexcept {
    std::strncpy(g_littlefs_label, config.partition, sizeof(g_littlefs_label) - 1U);
    esp_vfs_littlefs_conf_t conf{};
    conf.base_path = config.mount;
    conf.partition_label = g_littlefs_label;
    conf.format_if_mount_failed = config.format_if_empty;
    conf.dont_mount = false;
    const esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err == ESP_ERR_NOT_FOUND) {
        return fail(error, TRACEKIT_ERR_STORAGE, kTag,
                    "no partition \"%s\" in the partition table: add a data/littlefs partition "
                    "(see examples/*/partitions.csv) or change StorageConfig::fs.partition",
                    config.partition);
    }
    if (err != ESP_OK) {
        return fail(error, TRACEKIT_ERR_STORAGE, kTag,
                    "could not mount LittleFS on \"%s\" (%s)%s", config.partition, esp_err_to_name(err),
                    config.format_if_empty ? "" : ": set StorageConfig::fs.format_if_empty to format it");
    }
    fs.attach(config.mount, &littlefs_usage, nullptr);
    std::uint64_t total = 0U;
    std::uint64_t used = 0U;
    fs.usage(&total, &used);
    ESP_LOGI(kTag, "LittleFS \"%s\" at %s: %lu of %lu KB used", config.partition, config.mount,
             static_cast<unsigned long>(used / 1024U), static_cast<unsigned long>(total / 1024U));
    return ESP_OK;
}

void attach_app_volume(const char* root, FileSystem& fs) noexcept {
    // Not ours to mount: when the application has it unmounted (USB Mass
    // Storage, card removed), reads simply fail and the key stays in NVS.
    fs.attach(root, &fat_usage, const_cast<char*>(root));
    ESP_LOGI(kTag, "application volume %s attached", root);
}

}  // namespace tracekit
