#include "storage/NvsSettingsStore.h"

#include <nvs.h>

namespace tracekit {

bool NvsSettingsStore::load(const char* key, std::uint8_t* data, std::size_t* size) {
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) return false;  // nothing saved yet
    const esp_err_t err = nvs_get_blob(handle, key, data, size);
    nvs_close(handle);
    return err == ESP_OK;
}

bool NvsSettingsStore::save(const char* key, const std::uint8_t* data, std::size_t size) {
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(handle, key, data, size);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

bool NvsSettingsStore::erase(const char* key) {
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t err = nvs_erase_key(handle, key);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND;
}

}  // namespace tracekit
