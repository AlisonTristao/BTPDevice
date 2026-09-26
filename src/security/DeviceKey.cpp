#include "security/DeviceKey.h"

#include <cstdio>
#include <cstring>

#include <esp_log.h>
#include <nvs.h>

#include "core/LogTags.h"

namespace btpdev {
namespace {

constexpr const char* kTag = tag::kKey;
constexpr const char* kNamespace = "btpdev";
constexpr const char* kBlob = "keyfile";

// The buffers below hold both keys in the clear: overwrite them before they
// go out of scope (through volatile, so the store is not optimized away).
void wipe(std::uint8_t* data, std::size_t size) noexcept {
    volatile std::uint8_t* target = data;
    for (std::size_t i = 0U; i < size; ++i) target[i] = 0U;
}

void hex(const std::uint8_t* data, std::size_t size, char* out) noexcept {
    static constexpr char kDigits[] = "0123456789abcdef";
    for (std::size_t i = 0U; i < size; ++i) {
        out[i * 2U] = kDigits[data[i] >> 4];
        out[i * 2U + 1U] = kDigits[data[i] & 0x0FU];
    }
    out[size * 2U] = '\0';
}

}  // namespace

void DeviceKey::load_saved() noexcept {
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) return;
    std::uint8_t image[KeyStore::kFileSize] = {};
    std::size_t size = sizeof(image);
    const esp_err_t err = nvs_get_blob(handle, kBlob, image, &size);
    nvs_close(handle);
    if (err == ESP_OK) {
        if (store_.load(image, size)) {
            ESP_LOGI(kTag, "key loaded from NVS");
        } else {
            // Written by us after validation: only NVS corruption gets here.
            ESP_LOGE(kTag, "the key saved in NVS is damaged (%s): provision it again",
                     KeyStore::error_field(store_.last_error()));
        }
    }
    wipe(image, sizeof(image));
}

esp_err_t DeviceKey::import_file(FileSystem& fs, const char* name, ErrorInfo& error) noexcept {
    if (!fs.mounted() || name == nullptr || !fs.exists(name)) return ESP_ERR_NOT_FOUND;

    // One byte more than the format: a longer file is refused by size.
    std::uint8_t image[KeyStore::kFileSize + 1U] = {};
    std::size_t size = 0U;
    if (!fs.read(name, image, sizeof(image), &size)) size = sizeof(image);  // too big
    const KeyStore::Error parsed = KeyStore::parse(image, size);
    if (parsed != KeyStore::Error::Ok) {
        wipe(image, sizeof(image));
        const char* hint = parsed == KeyStore::Error::VerifyE   ? "wrong password E"
                           : parsed == KeyStore::Error::VerifyL ? "wrong password L"
                           : parsed == KeyStore::Error::Crc32   ? "the file is corrupted"
                                                                : "not a key file from tools/provision_key.py";
        return fail(error, BTPDEV_ERR_BAD_KEY, kTag, "%s%s rejected (%s: %s); the key in use did not change",
                    fs.root(), name, KeyStore::error_field(parsed), hint);
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (err == ESP_OK) err = nvs_set_blob(handle, kBlob, image, KeyStore::kFileSize);
    if (err == ESP_OK) err = nvs_commit(handle);
    if (handle != 0) nvs_close(handle);
    if (err != ESP_OK) {
        wipe(image, sizeof(image));
        return fail(error, BTPDEV_ERR_STORAGE, kTag, "could not save the key in NVS (%s)", esp_err_to_name(err));
    }
    store_.load(image, KeyStore::kFileSize);
    wipe(image, sizeof(image));

    // Imported once: the renamed file stays as a record, never read again.
    char imported[FileSystem::kMaxPath];
    std::snprintf(imported, sizeof(imported), "%s.imported", name);
    if (!fs.rename(name, imported)) {
        ESP_LOGW(kTag, "key imported, but %s could not be renamed: delete it", name);
    }
    char tags[64];
    describe(tags, sizeof(tags));
    ESP_LOGI(kTag, "key imported from %s/%s (%s)", fs.root(), name, tags);
    return ESP_OK;
}

esp_err_t DeviceKey::forget() noexcept {
    store_.unload();
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_erase_key(handle, kBlob);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    return err;
}

void DeviceKey::describe(char* out, std::size_t capacity) const noexcept {
    if (out == nullptr || capacity == 0U) return;
    if (!store_.loaded()) {
        std::snprintf(out, capacity, "no key");
        return;
    }
    char e[KeyStore::kVerifyLength * 2U + 1U];
    char l[KeyStore::kVerifyLength * 2U + 1U];
    hex(store_.verify_e(), KeyStore::kVerifyLength, e);
    hex(store_.verify_l(), KeyStore::kVerifyLength, l);
    std::snprintf(out, capacity, "verify_e %s, verify_l %s", e, l);
}

}  // namespace btpdev
