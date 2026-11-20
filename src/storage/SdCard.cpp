// The SD card driver (tracekit::sd_card), in a file of its own: it is linked
// only when a project sets SdConfig::driver to it, so the FAT, SDMMC and SDSPI
// drivers (~70 KB of flash) stay out of every project without a card.

#include <cstring>

#include <tracekit/config.h>
#include <tracekit/error.h>
#include <tracekit/filesystem.h>
#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <sdkconfig.h>
#include <sdmmc_cmd.h>
#include <soc/soc_caps.h>

#include <driver/sdspi_host.h>
#include <driver/spi_common.h>
#if SOC_SDMMC_HOST_SUPPORTED
#include <driver/sdmmc_host.h>
#endif

#include "core/LogTags.h"

namespace tracekit {
namespace {

constexpr const char* kTag = tag::kFs;

char g_sd_mount[16] = {};  // outlives the mount: the usage callback reads it

bool sd_usage(void* context, std::uint64_t* total, std::uint64_t* used) {
    std::uint64_t t = 0U;
    std::uint64_t free_bytes = 0U;
    if (esp_vfs_fat_info(static_cast<const char*>(context), &t, &free_bytes) != ESP_OK) return false;
    *total = t;
    *used = t - free_bytes;
    return true;
}

}  // namespace

int sd_card(const SdConfig& config, FileSystem& fs, ErrorInfo& error) {
    std::strncpy(g_sd_mount, config.mount, sizeof(g_sd_mount) - 1U);
    esp_vfs_fat_mount_config_t mount{};
    mount.format_if_mount_failed = false;
    mount.max_files = 4;
    mount.allocation_unit_size = 16U * 1024U;
    sdmmc_card_t* card = nullptr;
    esp_err_t err = ESP_OK;

    if (config.mode == SdMode::Sdmmc) {
#if SOC_SDMMC_HOST_SUPPORTED
        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
        slot.width = 1;
#if SOC_SDMMC_USE_GPIO_MATRIX
        slot.clk = static_cast<gpio_num_t>(config.sck);
        slot.cmd = static_cast<gpio_num_t>(config.mosi);
        slot.d0 = static_cast<gpio_num_t>(config.miso);
#endif
        slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
        err = esp_vfs_fat_sdmmc_mount(config.mount, &host, &slot, &mount, &card);
#else
        return fail(error, TRACEKIT_ERR_STORAGE, kTag, "this chip has no SDMMC host: use SdMode::Spi");
#endif
    } else {
        sdmmc_host_t host = SDSPI_HOST_DEFAULT();
        spi_bus_config_t bus{};
        bus.mosi_io_num = config.mosi;
        bus.miso_io_num = config.miso;
        bus.sclk_io_num = config.sck;
        bus.quadwp_io_num = -1;
        bus.quadhd_io_num = -1;
        bus.max_transfer_sz = 4096;
        err = spi_bus_initialize(static_cast<spi_host_device_t>(host.slot), &bus, SDSPI_DEFAULT_DMA);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {  // INVALID_STATE: bus already up
            return fail(error, TRACEKIT_ERR_STORAGE, kTag, "SPI bus for the SD card failed (%s)",
                        esp_err_to_name(err));
        }
        sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
        slot.gpio_cs = static_cast<gpio_num_t>(config.cs);
        slot.host_id = static_cast<spi_host_device_t>(host.slot);
        err = esp_vfs_fat_sdspi_mount(config.mount, &host, &slot, &mount, &card);
    }
    if (err != ESP_OK) {
        return fail(error, TRACEKIT_ERR_STORAGE, kTag,
                    "no SD card at %s (%s): check the card, the pins and that it is FAT formatted",
                    config.mount, esp_err_to_name(err));
    }
    fs.attach(config.mount, &sd_usage, g_sd_mount);
    ESP_LOGI(kTag, "SD card at %s: %s, %llu MB", config.mount, card != nullptr ? card->cid.name : "?",
             card != nullptr
                 ? static_cast<unsigned long long>(card->csd.capacity) * card->csd.sector_size / (1024ULL * 1024ULL)
                 : 0ULL);
    return ESP_OK;
}

}  // namespace tracekit
