// BTPDevice recovery firmware (docs/PLANO.md D19-D26, section 4.14).
//
// Lives in the `ota_0` partition and runs only when the application pointed
// the otadata at it (`btp -recovery`, the crash-loop detector, a factory
// reset). It joins the Wi-Fi the application mirrored into NVS, answers as
// <name>.local on port 80 with the protocol of TraceView's OTA tab, writes an
// authenticated image into the `factory` partition and boots it.
//
// Kept deliberately small: no BTP session, no file system, no Bluetooth.

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include <esp_app_desc.h>
#include <esp_event.h>
#include <esp_http_server.h>
#include <esp_image_format.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_random.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <mdns.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <sdkconfig.h>

#include "security/KeyStore.h"
#include "security/Sha256.h"
#include "transport/WifiNetworks.h"
#include "update/OtaAuth.h"
#include "update/RecoveryContract.h"

namespace rec = btpdev::recovery;
namespace ota = btpdev::ota;

namespace {

const char* version() { return esp_app_get_description()->version; }

constexpr const char* TAG = "recovery";

#ifdef CONFIG_BTPDEV_RECOVERY_INSECURE
constexpr bool kInsecure = true;
#else
constexpr bool kInsecure = false;
#endif

constexpr std::int64_t kReturnTimeoutUs =
    static_cast<std::int64_t>(CONFIG_BTPDEV_RECOVERY_TIMEOUT_MIN) * 60LL * 1000000LL;
constexpr std::size_t kChunk = 4096U;
constexpr std::size_t kEraseBlock = 64U * 1024U;

constexpr EventBits_t kGotIp = BIT0;
constexpr EventBits_t kDisconnected = BIT1;

// ---------------------------------------------------------------------------
// State. Written at boot and by the HTTP task; `lock` covers the nonce, the
// app description and the busy flag.
// ---------------------------------------------------------------------------

struct State {
    char name[btpdev::recovery::WifiBlob::kSsid] = "btpdev";
    char host[64] = "btpdev";
    rec::Reason reason = rec::Reason::None;
    bool nvs_ok = false;
    bool has_key = false;
    std::uint8_t key_e[btpdev::KeyStore::kKeyLength] = {};
    rec::WifiBlob wifi = {};
    const esp_partition_t* app = nullptr;
    bool app_valid = false;
    esp_app_desc_t app_desc = {};
    std::uint8_t nonce[ota::kNonceSize] = {};
    bool busy = false;
    std::int64_t last_activity_us = 0;
    char ip[16] = "";
    SemaphoreHandle_t lock = nullptr;
    EventGroupHandle_t events = nullptr;
};

State g;

class Lock {
public:
    Lock() { xSemaphoreTake(g.lock, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(g.lock); }
};

void new_nonce() { esp_fill_random(g.nonce, sizeof(g.nonce)); }

void restart_later(std::uint32_t ms) {
    esp_timer_create_args_t args{};
    args.callback = [](void*) { esp_restart(); };
    args.name = "rec_restart";
    esp_timer_handle_t timer = nullptr;
    if (esp_timer_create(&args, &timer) != ESP_OK || esp_timer_start_once(timer, ms * 1000ULL) != ESP_OK) {
        esp_restart();
    }
}

// ---------------------------------------------------------------------------
// NVS: the contract with the application (RecoveryContract.h)
// ---------------------------------------------------------------------------

void set_reason(rec::Reason reason) {
    g.reason = reason;
    if (!g.nvs_ok) return;
    nvs_handle_t h = 0;
    if (nvs_open(rec::kNamespace, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, rec::kKeyReason, static_cast<std::uint8_t>(reason));
    nvs_commit(h);
    nvs_close(h);
}

void load_contract() {
    nvs_handle_t h = 0;
    if (nvs_open(rec::kNamespace, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nothing from the application in NVS (namespace \"%s\"): no name, no Wi-Fi", rec::kNamespace);
        return;
    }
    std::uint8_t abi = 0U;
    nvs_get_u8(h, rec::kKeyAbi, &abi);
    if (abi > rec::kAbi) {
        ESP_LOGW(TAG, "the application wrote contract v%u, this recovery knows v%u: update the recovery",
                 static_cast<unsigned>(abi), static_cast<unsigned>(rec::kAbi));
    }
    std::size_t size = sizeof(g.name);
    nvs_get_str(h, rec::kKeyName, g.name, &size);
    std::uint8_t reason = 0U;
    if (nvs_get_u8(h, rec::kKeyReason, &reason) == ESP_OK && reason <= 4U) g.reason = static_cast<rec::Reason>(reason);
    size = sizeof(g.wifi);
    if (nvs_get_blob(h, rec::kKeyWifi, &g.wifi, &size) != ESP_OK || size != sizeof(g.wifi) ||
        g.wifi.count > rec::WifiBlob::kMaxNetworks) {
        std::memset(&g.wifi, 0, sizeof(g.wifi));
    }
    nvs_close(h);
    if (!btpdev::mdns_host_from_name(g.name, g.host, sizeof(g.host))) std::snprintf(g.host, sizeof(g.host), "btpdev");
}

void load_key() {
    nvs_handle_t h = 0;
    if (nvs_open(rec::kKeyNamespace, NVS_READONLY, &h) != ESP_OK) return;
    std::uint8_t image[btpdev::KeyStore::kFileSize];
    std::size_t size = sizeof(image);
    const esp_err_t err = nvs_get_blob(h, rec::kKeyBlob, image, &size);
    nvs_close(h);
    if (err != ESP_OK) return;
    btpdev::KeyStore store;
    if (size == sizeof(image) && store.load(image, size)) {
        std::memcpy(g.key_e, store.key_e(), sizeof(g.key_e));
        g.has_key = true;
    } else {
        ESP_LOGE(TAG, "the key in NVS is damaged: uploads refused (provision it again)");
    }
    btpdev::secure_wipe(image, sizeof(image));
    store.unload();
}

// ---------------------------------------------------------------------------
// The application partition
// ---------------------------------------------------------------------------

void check_app() {
    esp_app_desc_t desc{};
    bool valid = false;
    if (g.app != nullptr) {
        esp_partition_pos_t pos = {g.app->address, g.app->size};
        esp_image_metadata_t meta{};
        valid = esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta) == ESP_OK &&
                esp_ota_get_partition_description(g.app, &desc) == ESP_OK;
    }
    Lock lock;
    g.app_valid = valid;
    g.app_desc = desc;
}

void factory_reset(bool all) {
    ESP_LOGW(TAG, "factory reset%s: erasing the application%s", all ? " (all)" : "",
             all ? ", the file system and NVS" : " and the file system");
    if (g.app != nullptr) esp_partition_erase_range(g.app, 0, g.app->size);
    const esp_partition_t* storage =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, rec::kStorageLabel);
    if (storage != nullptr) esp_partition_erase_range(storage, 0, storage->size);
    if (all) {
        nvs_flash_deinit();
        nvs_flash_erase();
        g.nvs_ok = nvs_flash_init() == ESP_OK;
        g.has_key = false;
        btpdev::secure_wipe(g.key_e, sizeof(g.key_e));
        std::memset(&g.wifi, 0, sizeof(g.wifi));
        ESP_LOGW(TAG, "NVS erased: no key and no Wi-Fi any more. Flash a firmware over USB.");
    }
    // Done once: an empty app partition already keeps us here.
    set_reason(rec::Reason::None);
    g.reason = all ? rec::Reason::FactoryResetAll : rec::Reason::FactoryReset;
}

bool boot_app() {
    if (esp_ota_set_boot_partition(g.app) != ESP_OK) return false;
    set_reason(rec::Reason::None);
    return true;
}

// ---------------------------------------------------------------------------
// Wi-Fi station and mDNS
// ---------------------------------------------------------------------------

void on_event(void*, esp_event_base_t base, std::int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        g.ip[0] = '\0';
        xEventGroupSetBits(g.events, kDisconnected);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const auto* event = static_cast<ip_event_got_ip_t*>(data);
        std::snprintf(g.ip, sizeof(g.ip), IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(g.events, kGotIp);
    }
}

void start_wifi() {
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_t* netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(netif, g.host);
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&init);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_event, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_event, nullptr);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();

    mdns_init();
    mdns_hostname_set(g.host);
    mdns_instance_name_set(g.name);
    mdns_txt_item_t txt[] = {{"mode", "recovery"}, {"name", g.name}};
    mdns_service_add(nullptr, "_http", "_tcp", rec::kHttpPort, txt, 2);
}

// Tries the known networks in order until one gives an address; then waits
// for a disconnection and starts over. Runs forever on its own task.
void wifi_task(void*) {
    if (g.wifi.count == 0U) {
        ESP_LOGE(TAG, "no Wi-Fi network known: the application never mirrored one (Link::Tcp, wifi.txt). "
                      "Waiting for a USB flash.");
        vTaskDelete(nullptr);
        return;
    }
    while (true) {
        for (std::size_t i = 0U; i < g.wifi.count; ++i) {
            wifi_config_t config{};
            std::snprintf(reinterpret_cast<char*>(config.sta.ssid), sizeof(config.sta.ssid), "%s",
                          g.wifi.networks[i].ssid);
            std::snprintf(reinterpret_cast<char*>(config.sta.password), sizeof(config.sta.password), "%s",
                          g.wifi.networks[i].password);
            config.sta.threshold.authmode = WIFI_AUTH_OPEN;
            esp_wifi_disconnect();
            xEventGroupClearBits(g.events, kGotIp | kDisconnected);
            esp_wifi_set_config(WIFI_IF_STA, &config);
            ESP_LOGI(TAG, "connecting to \"%s\"", g.wifi.networks[i].ssid);
            esp_wifi_connect();
            const EventBits_t bits = xEventGroupWaitBits(g.events, kGotIp, pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
            if ((bits & kGotIp) == 0U) {
                ESP_LOGW(TAG, "\"%s\": no address in 15 s", g.wifi.networks[i].ssid);
                continue;
            }
            ESP_LOGI(TAG, "ready: http://%s.local/ (%s), upload with tools/btp_ota.py or TraceView's OTA tab",
                     g.host, g.ip);
            xEventGroupClearBits(g.events, kDisconnected);
            xEventGroupWaitBits(g.events, kDisconnected, pdTRUE, pdFALSE, portMAX_DELAY);
            ESP_LOGW(TAG, "Wi-Fi lost, reconnecting");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ---------------------------------------------------------------------------
// HTTP (docs/PLANO.md 4.14)
// ---------------------------------------------------------------------------

const char* auth_mode() {
    if (g.has_key) return "hmac-sha256";
    return kInsecure ? "none" : "unavailable";
}

// JSON string body: the name is printable ASCII (checked by the library),
// but escape quotes and backslashes anyway.
void json_escape(const char* in, char* out, std::size_t size) {
    std::size_t o = 0U;
    for (std::size_t i = 0U; in[i] != '\0' && o + 2U < size; ++i) {
        const char c = in[i];
        if (c == '"' || c == '\\') out[o++] = '\\';
        out[o++] = (c >= 0x20 && c < 0x7F) ? c : '?';
    }
    out[o] = '\0';
}

esp_err_t reply(httpd_req_t* req, const char* status, const char* text) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, text);
}

esp_err_t handle_status(httpd_req_t* req) {
    char name[64], project[64], app_version[64], firmware[256], nonce[2U * ota::kNonceSize + 1U];
    bool app_valid, busy;
    {
        Lock lock;
        app_valid = g.app_valid;
        busy = g.busy;
        json_escape(g.name, name, sizeof(name));
        json_escape(app_valid ? g.app_desc.project_name : "", project, sizeof(project));
        json_escape(app_valid ? g.app_desc.version : "", app_version, sizeof(app_version));
        ota::to_hex(g.nonce, sizeof(g.nonce), nonce);
    }
    if (app_valid) {
        std::snprintf(firmware, sizeof(firmware), "recovery %s (app: %s %s)", version(), project, app_version);
    } else {
        std::snprintf(firmware, sizeof(firmware), "recovery %s (no app)", version());
    }
    const bool ready = !busy && (g.has_key || kInsecure);
    char body[1024];
    std::snprintf(body, sizeof(body),
                  "{\"ota_ready\":%s,\"firmware\":\"%s\",\"mode\":\"recovery\",\"recovery\":\"%s\","
                  "\"name\":\"%s\",\"nonce\":\"%s\",\"auth\":\"%s\",\"reason\":\"%s\","
                  "\"app\":{\"valid\":%s,\"project\":\"%s\",\"version\":\"%s\"},\"max_size\":%" PRIu32
                  ",\"chip\":\"%s\"}",
                  ready ? "true" : "false", firmware, version(), name, nonce, auth_mode(), rec::reason_name(g.reason),
                  app_valid ? "true" : "false", project, app_version, g.app != nullptr ? g.app->size : 0U,
                  CONFIG_IDF_TARGET);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

// Checks X-OTA-Auth against the current nonce and rotates the nonce whatever
// the outcome. `sha` is the image hash the client committed to.
// Returns nullptr when allowed, or the reason to refuse.
const char* authorize(httpd_req_t* req, ota::Action action, const std::uint8_t* sha) {
    if (!g.has_key) {
        return kInsecure ? nullptr
                         : "no device key in NVS: provision it (tools/provision_key.py) and boot the application "
                           "once, or build the recovery with CONFIG_BTPDEV_RECOVERY_INSECURE for a bench";
    }
    char text[2U * ota::kMacSize + 1U] = "";
    std::uint8_t mac[ota::kMacSize];
    const bool parsed = httpd_req_get_hdr_value_str(req, "X-OTA-Auth", text, sizeof(text)) == ESP_OK &&
                        ota::from_hex(text, mac, sizeof(mac));
    Lock lock;
    const bool ok = parsed && ota::check_mac(g.key_e, sizeof(g.key_e), g.nonce, action, sha, mac);
    new_nonce();
    if (!parsed) return "missing X-OTA-Auth (this device needs HMAC authentication: use tools/btp_ota.py)";
    return ok ? nullptr : "authentication failed (wrong password E, or a stale nonce: GET /status again)";
}

std::uint16_t read_u16_le(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }

esp_err_t handle_update(httpd_req_t* req) {
    // The hash the client commits to, before anything is erased.
    std::uint8_t claimed[ota::kHashSize] = {};
    bool has_claim = false;
    {
        char text[2U * ota::kHashSize + 1U] = "";
        if (httpd_req_get_hdr_value_str(req, "X-OTA-SHA256", text, sizeof(text)) == ESP_OK) {
            has_claim = ota::from_hex(text, claimed, sizeof(claimed));
            if (!has_claim) return reply(req, "400 Bad Request", "X-OTA-SHA256 is not 64 hex digits");
        }
    }
    if (g.has_key && !has_claim) {
        return reply(req, "400 Bad Request", "missing X-OTA-SHA256 (use tools/btp_ota.py)");
    }
    if (const char* refused = authorize(req, ota::Action::Update, claimed)) {
        ESP_LOGW(TAG, "upload refused: %s", refused);
        return reply(req, "401 Unauthorized", refused);
    }
    if (g.app == nullptr) return reply(req, "500 Internal Server Error", "no factory partition");
    const std::size_t total = req->content_len;
    if (total == 0U) return reply(req, "400 Bad Request", "empty body");
    if (total > g.app->size) return reply(req, "413 Payload Too Large", "image larger than the app partition");
    {
        Lock lock;
        if (g.busy) return reply(req, "409 Conflict", "another upload is running");
        g.busy = true;
        g.last_activity_us = esp_timer_get_time();
    }

    static std::uint8_t buffer[kChunk];
    btpdev::Sha256 sha;
    std::size_t received = 0U;
    std::size_t erased = 0U;
    unsigned last_tenth = 0U;
    const char* failure = nullptr;
    const char* status = "500 Internal Server Error";
    int timeouts = 0;

    while (received < total) {
        const std::size_t want = total - received < sizeof(buffer) ? total - received : sizeof(buffer);
        const int got = httpd_req_recv(req, reinterpret_cast<char*>(buffer), want);
        if (got == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) continue;
        if (got <= 0) {
            failure = "connection lost during the upload";
            break;
        }
        timeouts = 0;
        if (received == 0U) {
            // The image header, checked before the first erase: magic and chip.
            if (got < 24 || buffer[0] != 0xE9U) {
                failure = "not an ESP application image (magic 0xE9 missing)";
                status = "400 Bad Request";
                break;
            }
            const std::uint16_t chip = read_u16_le(buffer + 12);
            if (chip != CONFIG_IDF_FIRMWARE_CHIP_ID) {
                failure = "image built for another chip";
                status = "400 Bad Request";
                ESP_LOGE(TAG, "chip id %u in the image, %u here", static_cast<unsigned>(chip),
                         static_cast<unsigned>(CONFIG_IDF_FIRMWARE_CHIP_ID));
                break;
            }
            ESP_LOGW(TAG, "upload of %u bytes: erasing the application", static_cast<unsigned>(total));
            Lock lock;
            g.app_valid = false;
        }
        const std::size_t end = received + static_cast<std::size_t>(got);
        if (end > erased) {
            std::size_t to = (end + kEraseBlock - 1U) / kEraseBlock * kEraseBlock;
            if (to > g.app->size) to = g.app->size;
            if (esp_partition_erase_range(g.app, erased, to - erased) != ESP_OK) {
                failure = "flash erase failed";
                break;
            }
            erased = to;
        }
        if (esp_partition_write(g.app, received, buffer, static_cast<std::size_t>(got)) != ESP_OK) {
            failure = "flash write failed";
            break;
        }
        sha.update(buffer, static_cast<std::size_t>(got));
        received = end;
        const unsigned tenth = static_cast<unsigned>(received * 10U / total);
        if (tenth != last_tenth) {
            last_tenth = tenth;
            ESP_LOGI(TAG, "%u%%", tenth * 10U);
        }
    }

    if (failure == nullptr) {
        std::uint8_t digest[ota::kHashSize];
        sha.finish(digest);
        if (has_claim && !btpdev::equal_bytes(digest, claimed, sizeof(digest))) {
            failure = "SHA-256 of the received image does not match X-OTA-SHA256";
            status = "400 Bad Request";
        } else if (esp_ota_set_boot_partition(g.app) != ESP_OK) {
            // Validates the whole image (segments, checksum, chip, hash).
            failure = "the image did not validate (corrupt or incompatible)";
            status = "400 Bad Request";
        }
    }
    check_app();
    {
        Lock lock;
        g.busy = false;
        g.last_activity_us = esp_timer_get_time();
    }
    if (failure != nullptr) {
        ESP_LOGE(TAG, "upload failed: %s. Staying in recovery.", failure);
        return reply(req, status, failure);
    }
    set_reason(rec::Reason::None);
    char text[160];
    std::snprintf(text, sizeof(text), "OK. Rebooting into %s %s.", g.app_desc.project_name, g.app_desc.version);
    ESP_LOGI(TAG, "%s", text);
    reply(req, "200 OK", text);
    restart_later(1000U);
    return ESP_OK;
}

esp_err_t handle_reboot(httpd_req_t* req) {
    const std::uint8_t zeros[ota::kHashSize] = {};
    if (const char* refused = authorize(req, ota::Action::Reboot, zeros)) return reply(req, "401 Unauthorized", refused);
    bool valid;
    {
        Lock lock;
        valid = g.app_valid && !g.busy;
    }
    if (!valid || !boot_app()) return reply(req, "409 Conflict", "no valid application to boot: upload one");
    reply(req, "200 OK", "OK. Rebooting into the application.");
    restart_later(500U);
    return ESP_OK;
}

esp_err_t handle_root(httpd_req_t* req) {
    char text[256];
    std::snprintf(text, sizeof(text),
                  "BTPDevice recovery %s on \"%s\"\n\nGET  /status\nPOST /update  (tools/btp_ota.py %s.local "
                  "firmware.bin)\nPOST /reboot\n",
                  version(), g.name, g.host);
    return reply(req, "200 OK", text);
}

void start_http() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = rec::kHttpPort;
    config.stack_size = 8192;
    config.recv_wait_timeout = 10;
    config.max_uri_handlers = 4;
    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server did not start");
        return;
    }
    const httpd_uri_t uris[] = {
        {"/status", HTTP_GET, handle_status, nullptr},
        {"/update", HTTP_POST, handle_update, nullptr},
        {"/reboot", HTTP_POST, handle_reboot, nullptr},
        {"/", HTTP_GET, handle_root, nullptr},
    };
    for (const httpd_uri_t& uri : uris) httpd_register_uri_handler(server, &uri);
}

}  // namespace

extern "C" void app_main() {
    g.lock = xSemaphoreCreateMutex();
    g.events = xEventGroupCreate();
    new_nonce();

    const esp_err_t nvs = nvs_flash_init();
    g.nvs_ok = nvs == ESP_OK;
    if (!g.nvs_ok) {
        // Erasing it here would throw away the key: report and carry on.
        ESP_LOGE(TAG, "NVS unusable (%s): no key, no Wi-Fi", esp_err_to_name(nvs));
    } else {
        load_contract();
        load_key();
    }
    g.app = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);

    if (g.reason == rec::Reason::FactoryReset || g.reason == rec::Reason::FactoryResetAll) {
        factory_reset(g.reason == rec::Reason::FactoryResetAll);
    }
    check_app();
    ESP_LOGI(TAG, "BTPDevice recovery %s on \"%s\" (%s), reason %s, key %s, app %s %s", version(), g.name,
             CONFIG_IDF_TARGET, rec::reason_name(g.reason), g.has_key ? "loaded" : (kInsecure ? "none (INSECURE)" : "none"),
             g.app_valid ? g.app_desc.project_name : "none", g.app_valid ? g.app_desc.version : "");
    if (kInsecure) ESP_LOGW(TAG, "CONFIG_BTPDEV_RECOVERY_INSECURE: anyone on the network can flash this device");

    start_wifi();
    start_http();
    xTaskCreate(&wifi_task, "rec_wifi", 4096, nullptr, 5, nullptr);

    // Entered by command (or by hand): give the application back after the
    // timeout. A crash loop or a factory reset waits for a firmware (D25).
    g.last_activity_us = esp_timer_get_time();
    const bool returns = g.reason == rec::Reason::Command || g.reason == rec::Reason::None;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!returns) continue;
        bool due;
        {
            Lock lock;
            due = g.app_valid && !g.busy && esp_timer_get_time() - g.last_activity_us > kReturnTimeoutUs;
        }
        if (due) {
            ESP_LOGI(TAG, "no upload in %d min: back to the application", CONFIG_BTPDEV_RECOVERY_TIMEOUT_MIN);
            if (boot_app()) esp_restart();
            Lock lock;
            g.last_activity_us = esp_timer_get_time();
        }
    }
}
