#include "core/ConfigCheck.h"

#include "check.h"

using btpdev::DeviceConfig;
using btpdev::Link;
using btpdev::StorageConfig;

namespace {

DeviceConfig valid_device() {
    DeviceConfig config{};
    config.name = "weather-station";
    return config;
}

esp_err_t check(const DeviceConfig& config, btpdev::ErrorInfo& error) {
    return btpdev::check_device_config(config, error);
}

void the_defaults_plus_a_name_are_valid() {
    btpdev::ErrorInfo error;
    CHECK(check(valid_device(), error) == ESP_OK);
    CHECK(error.ok());
    CHECK(btpdev::check_storage_config(StorageConfig{}, error) == ESP_OK);
}

void designated_initializers_compile_in_declaration_order() {
    // The shape users write in btp_device.h; also documents the field order.
    const DeviceConfig config = {
        .name = "estacao-quintal",
        .link = Link::FromSettings,
        .fallback = Link::Tcp,
        .memory = btpdev::Memory::Small,
        .threading = btpdev::Threading::Auto,
        .comms = {.core = 0, .priority = 10, .stack_bytes = 6144, .period_ms = 1},
        .wifi = {.file = "wifi.txt"},
        .tcp = {.port = 44300},
        .espnow = {.peer_mac = {1, 2, 3, 4, 5, 6}, .channel = 6},
        .source_info = {{"hw", "Hardware", "rev B"}},
    };
    btpdev::ErrorInfo error;
    CHECK(check(config, error) == ESP_OK);
}

void the_name_is_required() {
    btpdev::ErrorInfo error;
    DeviceConfig config = valid_device();
    config.name = nullptr;
    CHECK(check(config, error) == BTPDEV_ERR_CONFIG);
    CHECK_CONTAINS(error.message, "name");
    CHECK_STR_EQ(error.module, "btpdev");

    config.name = "";
    CHECK(check(config, error) == BTPDEV_ERR_CONFIG);
}

void the_name_fits_a_ble_advertisement() {
    btpdev::ErrorInfo error;
    DeviceConfig config = valid_device();
    config.name = "12345678901234567890123456789";  // 29, the limit
    CHECK(check(config, error) == ESP_OK);
    config.name = "123456789012345678901234567890";  // 30
    CHECK(check(config, error) == BTPDEV_ERR_CONFIG);
    CHECK_CONTAINS(error.message, "max is 29");
}

void the_name_is_printable_ascii() {
    btpdev::ErrorInfo error;
    DeviceConfig config = valid_device();
    config.name = "esta\xc3\xa7\xc3\xa3o";  // "estação" in UTF-8
    CHECK(check(config, error) == BTPDEV_ERR_CONFIG);
    config.name = "tab\there";
    CHECK(check(config, error) == BTPDEV_ERR_CONFIG);
    config.name = "Robo 2 (bancada)";
    CHECK(check(config, error) == ESP_OK);
}

void the_fallback_must_be_a_concrete_link() {
    btpdev::ErrorInfo error;
    DeviceConfig config = valid_device();
    config.fallback = Link::FromSettings;
    CHECK(check(config, error) == BTPDEV_ERR_CONFIG);
    CHECK_CONTAINS(error.message, "fallback");
}

void task_and_network_ranges() {
    btpdev::ErrorInfo error;
    DeviceConfig config = valid_device();
    config.comms.core = 2;
    CHECK(check(config, error) == BTPDEV_ERR_CONFIG);
    config.comms.core = -1;
    CHECK(check(config, error) == ESP_OK);

    config = valid_device();
    CHECK(config.shell.core == -1 && config.shell.priority == 0U);  // the defaults
    config.shell.core = 2;
    CHECK(check(config, error) == BTPDEV_ERR_CONFIG);
    CHECK_CONTAINS(error.message, "shell.core");
    config.shell.core = 0;
    config.shell.stack_bytes = 2048U;
    CHECK(check(config, error) == BTPDEV_ERR_CONFIG);
    config.shell.stack_bytes = 6144U;
    CHECK(check(config, error) == ESP_OK);

    config = valid_device();
    config.tcp.port = 0;
    CHECK(check(config, error) == BTPDEV_ERR_CONFIG);

    config = valid_device();
    config.espnow.channel = 0;
    CHECK(check(config, error) == BTPDEV_ERR_CONFIG);
    config.espnow.channel = 15;
    CHECK(check(config, error) == BTPDEV_ERR_CONFIG);
}

void source_info_is_filled_from_the_start() {
    btpdev::ErrorInfo error;
    DeviceConfig config = valid_device();
    config.source_info[1] = {"hw", "Hardware", "rev B"};  // [0] left empty
    CHECK(check(config, error) == BTPDEV_ERR_CONFIG);
    CHECK_CONTAINS(error.message, "source_info[1]");

    config = valid_device();
    config.source_info[0] = {"hw", nullptr, nullptr};  // no value
    CHECK(check(config, error) == BTPDEV_ERR_CONFIG);

    config.source_info[0] = {"hw", nullptr, "rev B"};  // label is optional
    CHECK(check(config, error) == ESP_OK);
}

void storage_rules() {
    btpdev::ErrorInfo error;
    StorageConfig config{};
    config.fs.partition = "";  // no LittleFS: allowed
    CHECK(btpdev::check_storage_config(config, error) == ESP_OK);
    config.fs.partition = "storage";
    config.fs.mount = nullptr;
    CHECK(btpdev::check_storage_config(config, error) == BTPDEV_ERR_CONFIG);
    CHECK_STR_EQ(error.module, "btpdev.fs");

    config = StorageConfig{};
    config.fs.mount = "fs";
    CHECK(btpdev::check_storage_config(config, error) == BTPDEV_ERR_CONFIG);

    config = StorageConfig{};
    config.key_file = "";
    CHECK(btpdev::check_storage_config(config, error) == BTPDEV_ERR_CONFIG);
    config.key = btpdev::KeySource::None;
    CHECK(btpdev::check_storage_config(config, error) == ESP_OK);

    config = StorageConfig{};
    config.sd.enabled = true;  // SPI without pins
    CHECK(btpdev::check_storage_config(config, error) == BTPDEV_ERR_CONFIG);
    CHECK_CONTAINS(error.message, "pins");
    config.sd = {.enabled = true, .mode = btpdev::SdMode::Spi,
                 .cs = 10, .mosi = 11, .miso = 13, .sck = 12};
    CHECK(btpdev::check_storage_config(config, error) == ESP_OK);
    config.sd.mount = "/fs";  // same as the LittleFS mount
    CHECK(btpdev::check_storage_config(config, error) == BTPDEV_ERR_CONFIG);

    config = StorageConfig{};
    config.fs.partition = "";
    config.app_volume = "/sdcard";  // mounted by the application
    CHECK(btpdev::check_storage_config(config, error) == ESP_OK);
    config.app_volume = "sdcard";
    CHECK(btpdev::check_storage_config(config, error) == BTPDEV_ERR_CONFIG);
    CHECK_CONTAINS(error.message, "app_volume");
    config = StorageConfig{};
    config.app_volume = "/fs";  // the library's own LittleFS mount
    CHECK(btpdev::check_storage_config(config, error) == BTPDEV_ERR_CONFIG);
}

void link_names() {
    CHECK_STR_EQ(btpdev::link_name(Link::Serial), "Serial");
    CHECK_STR_EQ(btpdev::link_name(Link::EspNow), "ESP-NOW");
}

}  // namespace

int main() {
    the_defaults_plus_a_name_are_valid();
    designated_initializers_compile_in_declaration_order();
    the_name_is_required();
    the_name_fits_a_ble_advertisement();
    the_name_is_printable_ascii();
    the_fallback_must_be_a_concrete_link();
    task_and_network_ranges();
    source_info_is_filled_from_the_start();
    storage_rules();
    link_names();
    return test_result("test_config_check");
}
