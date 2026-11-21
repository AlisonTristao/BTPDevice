// TraceKit recovery firmware (docs/PLANO.md D19-D26, section 4.14).
//
// Lives in the `ota_0` partition and runs only when the application pointed
// the otadata at it (`btp -recovery`, the crash-loop detector, a factory
// reset). It joins the Wi-Fi the application mirrored into NVS, answers as
// <name>.local on port 80 with the protocol of TraceView's OTA tab, writes an
// authenticated image into the `factory` partition and boots it. When no known
// network answers it also raises its own ("btp-<name>", http://192.168.4.1/)
// with a captive portal; the page at / signs uploads in the browser.
//
// Kept deliberately small: no BTP session, no file system, no Bluetooth.

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
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
#include <lwip/sockets.h>
#include <mdns.h>
#include <miniz.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <sdkconfig.h>

#ifdef CONFIG_TRACEKIT_RECOVERY_AP
#include <dhcpserver/dhcpserver.h>
#endif

#include "security/KeyStore.h"
#include "security/Sha256.h"
#include "transport/WifiNetworks.h"
#include "update/OtaAuth.h"
#include "update/RecoveryContract.h"

namespace rec = tracekit::recovery;
namespace ota = tracekit::ota;

namespace {

const char* version() { return esp_app_get_description()->version; }

constexpr const char* TAG = "recovery";

#ifdef CONFIG_TRACEKIT_RECOVERY_INSECURE
constexpr bool kInsecure = true;
#else
constexpr bool kInsecure = false;
#endif

constexpr std::int64_t kReturnTimeoutUs =
    static_cast<std::int64_t>(CONFIG_TRACEKIT_RECOVERY_TIMEOUT_MIN) * 60LL * 1000000LL;
constexpr std::size_t kChunk = 4096U;
constexpr std::size_t kEraseBlock = 64U * 1024U;

// The page at / (page.html, embedded by main/CMakeLists.txt).
extern const char page_start[] asm("_binary_page_html_start");
extern const char page_end[] asm("_binary_page_html_end");

constexpr EventBits_t kGotIp = BIT0;
constexpr EventBits_t kDisconnected = BIT1;

// ---------------------------------------------------------------------------
// State. Written at boot and by the HTTP task; `lock` covers the nonce, the
// app description and the busy flag.
// ---------------------------------------------------------------------------

struct State {
    char name[tracekit::recovery::WifiBlob::kSsid] = "tracekit";
    char host[64] = "tracekit";
    rec::Reason reason = rec::Reason::None;
    bool nvs_ok = false;
    bool has_key = false;
    std::uint8_t key_e[tracekit::KeyStore::kKeyLength] = {};
    rec::WifiBlob wifi = {};
    const esp_partition_t* app = nullptr;
    bool app_valid = false;
    esp_app_desc_t app_desc = {};
    std::uint8_t nonce[ota::kNonceSize] = {};
    bool busy = false;
    std::int64_t last_activity_us = 0;
    char ip[16] = "";
    char ap_ssid[33] = "";
    esp_netif_t* ap = nullptr;
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
    if (!tracekit::mdns_host_from_name(g.name, g.host, sizeof(g.host))) std::snprintf(g.host, sizeof(g.host), "tracekit");
}

void load_key() {
    nvs_handle_t h = 0;
    if (nvs_open(rec::kKeyNamespace, NVS_READONLY, &h) != ESP_OK) return;
    std::uint8_t image[tracekit::KeyStore::kFileSize];
    std::size_t size = sizeof(image);
    const esp_err_t err = nvs_get_blob(h, rec::kKeyBlob, image, &size);
    nvs_close(h);
    if (err != ESP_OK) return;
    tracekit::KeyStore store;
    if (size == sizeof(image) && store.load(image, size)) {
        std::memcpy(g.key_e, store.key_e(), sizeof(g.key_e));
        g.has_key = true;
    } else {
        ESP_LOGE(TAG, "the key in NVS is damaged: uploads refused (provision it again)");
    }
    tracekit::secure_wipe(image, sizeof(image));
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
        tracekit::secure_wipe(g.key_e, sizeof(g.key_e));
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
#ifdef CONFIG_TRACEKIT_RECOVERY_AP
    g.ap = esp_netif_create_default_wifi_ap();
#endif
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

// ---------------------------------------------------------------------------
// Own network (CONFIG_TRACEKIT_RECOVERY_AP) and its captive-portal DNS
// ---------------------------------------------------------------------------

#ifdef CONFIG_TRACEKIT_RECOVERY_AP
// Answers every A query with the access point's address, so a phone that
// joins the network finds "no internet" behind a portal and opens the page.
void dns_task(void*) {
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (sock < 0 || bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ESP_LOGW(TAG, "captive DNS did not start: open http://192.168.4.1/ by hand");
        if (sock >= 0) close(sock);
        vTaskDelete(nullptr);
        return;
    }
    esp_netif_ip_info_t info{};
    esp_netif_get_ip_info(g.ap, &info);
    static std::uint8_t packet[512];
    while (true) {
        sockaddr_in from{};
        socklen_t from_len = sizeof(from);
        const int n = recvfrom(sock, packet, sizeof(packet) - 16, 0, reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n < 12 || (packet[2] & 0x80U) != 0U) continue;  // too short, or not a query
        // One question: walk its name to find where it ends.
        int end = 12;
        while (end < n && packet[end] != 0U) end += packet[end] + 1;
        end += 5;  // the root label, then QTYPE and QCLASS
        if (end > n || packet[4] != 0U || packet[5] != 1U) continue;
        const bool type_a = packet[end - 4] == 0U && packet[end - 3] == 1U;
        packet[2] = 0x84U;  // response, authoritative, opcode 0
        packet[3] = 0x00U;  // no error
        packet[6] = 0U;
        packet[7] = type_a ? 1U : 0U;  // answers
        std::memset(packet + 8, 0, 4);  // no authority, no additional records
        int len = end;
        if (type_a) {
            const std::uint8_t answer[12] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4};
            std::memcpy(packet + len, answer, sizeof(answer));
            std::memcpy(packet + len + 12, &info.ip.addr, 4);  // already in network order
            len += 16;
        }
        sendto(sock, packet, len, 0, reinterpret_cast<sockaddr*>(&from), from_len);
    }
}

void start_ap() {
    if (g.ap_ssid[0] != '\0') return;
    std::snprintf(g.ap_ssid, sizeof(g.ap_ssid), "btp-%.28s", g.host);
    wifi_config_t config{};
    const std::size_t ssid_len = std::strlen(g.ap_ssid);
    std::memcpy(config.ap.ssid, g.ap_ssid, ssid_len);
    config.ap.ssid_len = static_cast<std::uint8_t>(ssid_len);
    config.ap.channel = 1;
    config.ap.max_connection = 2;
    const char* password = CONFIG_TRACEKIT_RECOVERY_AP_PASSWORD;
    const std::size_t password_len = std::strlen(password);
    if (password_len >= 8U && password_len < sizeof(config.ap.password)) {
        std::memcpy(config.ap.password, password, password_len);
        config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        if (password_len != 0U) {
            ESP_LOGW(TAG, "CONFIG_TRACEKIT_RECOVERY_AP_PASSWORD needs 8 to 63 characters: network left open");
        }
        config.ap.authmode = WIFI_AUTH_OPEN;
    }

    // Offer ourselves as the DNS server, for the captive portal.
    esp_netif_ip_info_t info{};
    esp_netif_get_ip_info(g.ap, &info);
    esp_netif_dhcps_stop(g.ap);
    esp_netif_dns_info_t dns{};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = info.ip.addr;
    std::uint8_t offer = OFFER_DNS;
    esp_netif_dhcps_option(g.ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof(offer));
    esp_netif_set_dns_info(g.ap, ESP_NETIF_DNS_MAIN, &dns);
    esp_netif_dhcps_start(g.ap);

    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &config);
    xTaskCreate(&dns_task, "rec_dns", 3072, nullptr, 4, nullptr);
    ESP_LOGW(TAG, "own network \"%s\" (%s): join it and open http://" IPSTR "/", g.ap_ssid,
             config.ap.authmode == WIFI_AUTH_OPEN ? "open" : "WPA2", IP2STR(&info.ip));
}

std::size_t ap_clients() {
    wifi_sta_list_t list{};
    return esp_wifi_ap_get_sta_list(&list) == ESP_OK ? static_cast<std::size_t>(list.num) : 0U;
}
#endif

// Tries the known networks in order until one gives an address; then waits
// for a disconnection and starts over. When a whole round fails (or there is
// no network at all) the own network comes up; while someone is connected to
// it the station stops retrying, because a retry hops channels and would drop
// them. Runs forever on its own task.
void wifi_task(void*) {
    if (g.wifi.count == 0U) {
#ifdef CONFIG_TRACEKIT_RECOVERY_AP
        ESP_LOGW(TAG, "no Wi-Fi network known (the application never mirrored one)");
        start_ap();
#else
        ESP_LOGE(TAG, "no Wi-Fi network known: the application never mirrored one (Link::Tcp, wifi.txt). "
                      "Waiting for a USB flash.");
#endif
        vTaskDelete(nullptr);
        return;
    }
    while (true) {
        bool was_connected = false;
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
            was_connected = true;
            break;
        }
        if (was_connected) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
#ifdef CONFIG_TRACEKIT_RECOVERY_AP
        start_ap();
        // Retry the known networks every 30 s, but not while someone uses ours.
        for (int i = 0; i < 30 || ap_clients() > 0U; ++i) vTaskDelay(pdMS_TO_TICKS(1000));
#else
        vTaskDelay(pdMS_TO_TICKS(1000));
#endif
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
                  ",\"chip\":\"%s\",\"chip_id\":%d,\"ap\":\"%s\",\"encodings\":[\"deflate\"]}",
                  ready ? "true" : "false", firmware, version(), name, nonce, auth_mode(), rec::reason_name(g.reason),
                  app_valid ? "true" : "false", project, app_version, g.app != nullptr ? g.app->size : 0U,
                  CONFIG_IDF_TARGET, CONFIG_IDF_FIRMWARE_CHIP_ID, g.ap_ssid);
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
                           "once, or build the recovery with CONFIG_TRACEKIT_RECOVERY_INSECURE for a bench";
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

// Writes the application image into the app partition as it arrives, from
// the body itself or from the inflater: the header is checked before the
// first erase, the flash is erased ahead of the writes, and the SHA-256 is
// taken over exactly what was written. write() returns false once it has
// failed; `failure`/`status` say why.
struct ImageWriter {
    explicit ImageWriter(std::size_t expected) : expected(expected) {}

    bool write(const std::uint8_t* data, std::size_t size) {
        if (failure != nullptr) return false;
        if (!checked) {
            // Collected until there is a whole header, which may come in
            // pieces (the inflater's first output can be short).
            const std::size_t take = size < sizeof(head) - head_size ? size : sizeof(head) - head_size;
            std::memcpy(head + head_size, data, take);
            head_size += take;
            data += take;
            size -= take;
            if (head_size < sizeof(head)) return true;
            if (!check_header() || !store(head, sizeof(head))) return false;
            checked = true;
        }
        return size == 0U || store(data, size);
    }

    tracekit::Sha256 sha;
    std::size_t written = 0U;
    const char* failure = nullptr;
    const char* status = "500 Internal Server Error";

private:
    bool fail(const char* why, const char* http_status) {
        failure = why;
        status = http_status;
        return false;
    }

    bool check_header() {
        if (head[0] != 0xE9U) return fail("not an ESP application image (magic 0xE9 missing)", "400 Bad Request");
        const std::uint16_t chip = read_u16_le(head + 12);
        if (chip != CONFIG_IDF_FIRMWARE_CHIP_ID) {
            ESP_LOGE(TAG, "chip id %u in the image, %u here", static_cast<unsigned>(chip),
                     static_cast<unsigned>(CONFIG_IDF_FIRMWARE_CHIP_ID));
            return fail("image built for another chip", "400 Bad Request");
        }
        ESP_LOGW(TAG, "upload of %u bytes: erasing the application", static_cast<unsigned>(expected));
        Lock lock;
        g.app_valid = false;
        return true;
    }

    bool store(const std::uint8_t* data, std::size_t size) {
        // Never past the size announced (and checked against the partition):
        // a compressed body could otherwise inflate into anything.
        if (written + size > expected) return fail("the image is larger than announced", "400 Bad Request");
        const std::size_t end = written + size;
        if (end > erased) {
            std::size_t to = (end + kEraseBlock - 1U) / kEraseBlock * kEraseBlock;
            if (to > g.app->size) to = g.app->size;
            if (esp_partition_erase_range(g.app, erased, to - erased) != ESP_OK) return fail("flash erase failed", status);
            erased = to;
        }
        if (esp_partition_write(g.app, written, data, size) != ESP_OK) return fail("flash write failed", status);
        sha.update(data, size);
        written = end;
        const unsigned tenth = static_cast<unsigned>(written * 10U / expected);
        if (tenth != last_tenth) {
            last_tenth = tenth;
            ESP_LOGI(TAG, "%u%%", tenth * 10U);
        }
        return true;
    }

    std::size_t expected;
    std::size_t erased = 0U;
    std::uint8_t head[24] = {};
    std::size_t head_size = 0U;
    bool checked = false;
    unsigned last_tenth = 0U;
};

// The inflater (miniz's tinfl, in the chip's ROM) and its 32 KB dictionary,
// which doubles as the output ring. Static: ~43 KB the recovery has to spare,
// and only one upload runs at a time (g.busy).
tinfl_decompressor g_inflator;
std::uint8_t g_dict[TINFL_LZ_DICT_SIZE];

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

    // X-OTA-Encoding: deflate -- the body is a zlib stream (RFC 1950, what
    // HTTP calls "deflate") of the image, and X-OTA-Size its size once
    // inflated. The hash and the signature are over the inflated image, so
    // authentication does not change. Without the header the body is the
    // image itself, as before (the page, older clients).
    bool deflate = false;
    std::size_t image_size = total;
    {
        char text[32] = "";
        if (httpd_req_get_hdr_value_str(req, "X-OTA-Encoding", text, sizeof(text)) == ESP_OK) {
            if (std::strcmp(text, "deflate") != 0) {
                return reply(req, "415 Unsupported Media Type", "X-OTA-Encoding: only \"deflate\" is supported");
            }
            deflate = true;
            char size_text[16] = "";
            char* end = nullptr;
            unsigned long size = 0;
            if (httpd_req_get_hdr_value_str(req, "X-OTA-Size", size_text, sizeof(size_text)) == ESP_OK) {
                size = std::strtoul(size_text, &end, 10);
            }
            if (end == nullptr || *end != '\0' || size == 0U) {
                return reply(req, "400 Bad Request", "X-OTA-Encoding: deflate needs X-OTA-Size (the inflated size)");
            }
            image_size = size;
        }
    }
    if (image_size > g.app->size) return reply(req, "413 Payload Too Large", "image larger than the app partition");
    {
        Lock lock;
        if (g.busy) return reply(req, "409 Conflict", "another upload is running");
        g.busy = true;
        g.last_activity_us = esp_timer_get_time();
    }

    static std::uint8_t buffer[kChunk];
    ImageWriter writer(image_size);
    if (deflate) tinfl_init(&g_inflator);
    std::size_t dict_offset = 0U;
    tinfl_status inflated = TINFL_STATUS_NEEDS_MORE_INPUT;
    std::size_t received = 0U;
    const char* failure = nullptr;
    const char* status = "500 Internal Server Error";
    int timeouts = 0;

    while (received < total && failure == nullptr) {
        const std::size_t want = total - received < sizeof(buffer) ? total - received : sizeof(buffer);
        const int got = httpd_req_recv(req, reinterpret_cast<char*>(buffer), want);
        if (got == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) continue;
        if (got <= 0) {
            failure = "connection lost during the upload";
            break;
        }
        timeouts = 0;
        received += static_cast<std::size_t>(got);
        if (!deflate) {
            if (!writer.write(buffer, static_cast<std::size_t>(got))) break;
            continue;
        }
        // Inflate this chunk into the 32 KB dictionary ring, handing every
        // piece of output to the writer before tinfl reuses that space.
        const std::uint8_t* in = buffer;
        std::size_t in_left = static_cast<std::size_t>(got);
        const mz_uint32 flags = TINFL_FLAG_PARSE_ZLIB_HEADER | (received < total ? TINFL_FLAG_HAS_MORE_INPUT : 0U);
        for (;;) {
            std::size_t in_bytes = in_left;
            std::size_t out_bytes = sizeof(g_dict) - dict_offset;
            inflated = tinfl_decompress(&g_inflator, in, &in_bytes, g_dict, g_dict + dict_offset, &out_bytes, flags);
            in += in_bytes;
            in_left -= in_bytes;
            if (out_bytes > 0U && !writer.write(g_dict + dict_offset, out_bytes)) break;
            dict_offset = (dict_offset + out_bytes) & (sizeof(g_dict) - 1U);
            if (inflated < TINFL_STATUS_DONE) {
                failure = "the compressed image is corrupt (deflate stream)";
                status = "400 Bad Request";
                break;
            }
            // Only HAS_MORE_OUTPUT calls again with what is left of the
            // chunk; NEEDS_MORE_INPUT means it took all of it.
            if (inflated != TINFL_STATUS_HAS_MORE_OUTPUT) break;
        }
        if (writer.failure != nullptr) break;
        if (inflated == TINFL_STATUS_DONE && (received < total || in_left > 0U)) {
            failure = "data after the end of the compressed image";
            status = "400 Bad Request";
        }
    }
    if (failure == nullptr && writer.failure != nullptr) {
        failure = writer.failure;
        status = writer.status;
    }
    if (failure == nullptr && deflate && inflated != TINFL_STATUS_DONE) {
        failure = "the compressed image ended early";
        status = "400 Bad Request";
    }
    if (failure == nullptr && writer.written != image_size) {
        failure = deflate ? "the inflated image is not X-OTA-Size bytes" : "the image is too short";
        status = "400 Bad Request";
    }

    if (failure == nullptr) {
        std::uint8_t digest[ota::kHashSize];
        writer.sha.finish(digest);
        if (has_claim && !tracekit::equal_bytes(digest, claimed, sizeof(digest))) {
            failure = "SHA-256 of the received image does not match X-OTA-SHA256";
            status = "400 Bad Request";
        } else if (esp_ota_set_boot_partition(g.app) != ESP_OK) {
            // Validates the whole image (segments, checksum, chip, hash).
            failure = "the image did not validate (corrupt or incompatible)";
            status = "400 Bad Request";
        }
    }
    if (deflate) {
        ESP_LOGI(TAG, "%u bytes received for a %u byte image", static_cast<unsigned>(received),
                 static_cast<unsigned>(image_size));
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
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, page_start, page_end - page_start - 1);  // EMBED_TXTFILES adds a NUL
}

// Anything else goes to the page: this is what makes a phone's captive-portal
// probe (generate_204, hotspot-detect.html...) open it.
esp_err_t handle_not_found(httpd_req_t* req, httpd_err_code_t) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, nullptr, 0);
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
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, handle_not_found);
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
    ESP_LOGI(TAG, "TraceKit recovery %s on \"%s\" (%s), reason %s, key %s, app %s %s", version(), g.name,
             CONFIG_IDF_TARGET, rec::reason_name(g.reason), g.has_key ? "loaded" : (kInsecure ? "none (INSECURE)" : "none"),
             g.app_valid ? g.app_desc.project_name : "none", g.app_valid ? g.app_desc.version : "");
    if (kInsecure) ESP_LOGW(TAG, "CONFIG_TRACEKIT_RECOVERY_INSECURE: anyone on the network can flash this device");

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
            ESP_LOGI(TAG, "no upload in %d min: back to the application", CONFIG_TRACEKIT_RECOVERY_TIMEOUT_MIN);
            if (boot_app()) esp_restart();
            Lock lock;
            g.last_activity_us = esp_timer_get_time();
        }
    }
}
