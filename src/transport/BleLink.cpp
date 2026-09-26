#include "transport/BleLink.h"

#include <sdkconfig.h>

#include <cstring>

#include "core/LogTags.h"

#if CONFIG_BTPDEV_LINK_BLE && CONFIG_BT_NIMBLE_ENABLED
#define BTPDEV_HAS_BLE 1
#else
#define BTPDEV_HAS_BLE 0
#endif

#if BTPDEV_HAS_BLE
#include <esp_log.h>

#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#endif

namespace btpdev {

BleLink::BleLink(const BleLinkConfig& config) noexcept : config_(config) {
    const char* name = config.device_name != nullptr && config.device_name[0] != '\0'
                           ? config.device_name
                           : "BTPDevice";
    std::size_t length = std::strlen(name);
    if (length > kMaxNameLength) {
        length = kMaxNameLength;
        // Never cut a UTF-8 sequence in half.
        while (length > 0U && (static_cast<unsigned char>(name[length]) & 0xC0U) == 0x80U) --length;
    }
    std::memcpy(device_name_, name, length);
    device_name_[length] = '\0';
}

#if BTPDEV_HAS_BLE

namespace {

constexpr const char* kTag = tag::kBle;
// Legacy advertising PDU / scan response ceiling, and one complete 128-bit
// service UUID AD structure (2-octet header + 16).
constexpr std::size_t kAdvPayloadMax = 31U;
constexpr std::size_t kUuid128AdSize = 18U;

// BTP v1 BLE service/characteristic UUIDs (fragmentation-and-transports.md
// section 8), the same three TraceView's BleTransport uses. BLE_UUID128_INIT
// wants little-endian wire order: each array is the UUID string's bytes
// reversed.
// 547a1aae-676e-4b68-8e20-bace26cd0726
const ble_uuid128_t kServiceUuid = BLE_UUID128_INIT(
    0x26, 0x07, 0xcd, 0x26, 0xce, 0xba, 0x20, 0x8e, 0x68, 0x4b, 0x6e, 0x67, 0xae, 0x1a, 0x7a, 0x54);
// f9160b78-c242-42f4-8f5e-88df2c51cbe6 (RX, write)
const ble_uuid128_t kRxUuid = BLE_UUID128_INIT(
    0xe6, 0xcb, 0x51, 0x2c, 0xdf, 0x88, 0x5e, 0x8f, 0xf4, 0x42, 0x42, 0xc2, 0x78, 0x0b, 0x16, 0xf9);
// 20f96ede-2f3b-4e02-b2cf-be6bb75dbe35 (TX, notify)
const ble_uuid128_t kTxUuid = BLE_UUID128_INIT(
    0x35, 0xbe, 0x5d, 0xb7, 0x6b, 0xbe, 0xcf, 0xb2, 0x02, 0x4e, 0x3b, 0x2f, 0xde, 0x6e, 0xf9, 0x20);

// One radio, one link: the GATT table and the host callbacks are global.
BleLink* g_link = nullptr;
std::uint16_t g_tx_handle = 0U;

int gatt_access(std::uint16_t conn_handle, std::uint16_t /*attr_handle*/,
                struct ble_gatt_access_ctxt* ctxt, void* /*arg*/) {
    if (g_link == nullptr || ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    const std::uint16_t length = OS_MBUF_PKTLEN(ctxt->om);
    if (length == 0U || length > BleLink::kMaxChunkSize) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    // The link's own buffer, not the stack: this runs on the NimBLE host
    // task, whose stack also carries everything on_receive() triggers.
    static std::uint8_t buffer[BleLink::kMaxChunkSize];
    std::uint16_t copied = 0U;
    if (ble_hs_mbuf_to_flat(ctxt->om, buffer, sizeof(buffer), &copied) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    return g_link->on_rx_write(conn_handle, buffer, copied);
}

int gap_event(struct ble_gap_event* event, void* /*arg*/) {
    return g_link != nullptr ? g_link->on_gap_event(event) : 0;
}

// NimBLE's tables are meant to be zero-filled past the fields a service
// uses (and the field list grows between IDF releases): listing every member
// here would break on the next NimBLE update.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
const struct ble_gatt_chr_def kCharacteristics[] = {
    {.uuid = &kRxUuid.u, .access_cb = &gatt_access, .flags = BLE_GATT_CHR_F_WRITE},
    {.uuid = &kTxUuid.u, .access_cb = &gatt_access, .flags = BLE_GATT_CHR_F_NOTIFY,
     .val_handle = &g_tx_handle},
    {0},
};

const struct ble_gatt_svc_def kServices[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &kServiceUuid.u, .characteristics = kCharacteristics},
    {0},
};
#pragma GCC diagnostic pop

void host_task(void* /*param*/) {
    nimble_port_run();  // returns once nimble_port_stop() is called
    nimble_port_freertos_deinit();
}

void on_sync_trampoline() {
    if (g_link != nullptr) g_link->on_sync();
}

void on_reset_trampoline(int reason) { ESP_LOGW(kTag, "NimBLE host reset (reason %d)", reason); }

}  // namespace

BleLink::~BleLink() { stop(); }

esp_err_t BleLink::start(const LinkEvents& events, ErrorInfo& error) noexcept {
    if (running_.load()) return ESP_OK;
    if (events.on_receive == nullptr) {
        return fail(error, ESP_ERR_INVALID_ARG, kTag, "BLE link started without on_receive");
    }
    if (g_link != nullptr && g_link != this) {
        return fail(error, ESP_ERR_INVALID_STATE, kTag, "another BLE link already owns the radio");
    }
    if (!mutex_.create()) return fail(error, ESP_ERR_NO_MEM, kTag, "no memory for the BLE link mutex");
    if (!queue_storage_.allocate(config_.send_queue_bytes)) {
        return fail(error, BTPDEV_ERR_MEMORY_BUDGET, kTag, "no memory for the BLE send queue (%u bytes)",
                    static_cast<unsigned>(config_.send_queue_bytes));
    }
    queue_.bind(queue_storage_.data(), queue_storage_.size(), queue_storage_.size() / 4U);
    events_ = events;
    g_link = this;

    const esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        g_link = nullptr;
        queue_storage_.reset();
        return fail(error, BTPDEV_ERR_LINK_START, kTag,
                    "nimble_port_init() failed (%s): usually not enough internal RAM for the "
                    "Bluetooth controller",
                    esp_err_to_name(err));
    }
    ble_hs_cfg.sync_cb = &on_sync_trampoline;
    ble_hs_cfg.reset_cb = &on_reset_trampoline;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 0;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(kServices);
    if (rc == 0) rc = ble_gatts_add_svcs(kServices);
    if (rc != 0) {
        nimble_port_deinit();
        g_link = nullptr;
        queue_storage_.reset();
        return fail(error, BTPDEV_ERR_LINK_START, kTag, "could not register the BTP GATT service (rc %d)", rc);
    }
    ble_svc_gap_device_name_set(device_name_);

    running_.store(true);
    nimble_port_freertos_init(&host_task);
    ESP_LOGI(kTag, "advertising as \"%s\"", device_name_);
    return ESP_OK;
}

void BleLink::stop() noexcept {
    if (!running_.exchange(false)) return;
    const std::uint16_t handle = conn_handle_.load();
    if (handle != kNoConnection) ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM);
    ble_gap_adv_stop();
    nimble_port_stop();
    nimble_port_deinit();
    {
        LinkLock lock(mutex_);
        queue_.bind(nullptr, 0U, 0U);
    }
    queue_storage_.reset();
    g_link = nullptr;
}

bool BleLink::connected() const noexcept {
    return conn_handle_.load() != kNoConnection && subscribed_.load();
}

void BleLink::disconnect_peer() noexcept {
    const std::uint16_t handle = conn_handle_.load();
    if (handle != kNoConnection) ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM);
}

void BleLink::on_sync() noexcept {
    std::uint8_t address_type = 0U;
    ble_hs_id_infer_auto(0, &address_type);
    start_advertising();
}

void BleLink::start_advertising() noexcept {
    // A legacy PDU carries 31 bytes: flags + the 128-bit UUID + a name does
    // not fit, so the UUID goes in the advertisement (TraceView filters on
    // it) and the name in the scan response. The UUID is repeated in the scan
    // response when it fits beside the name: Qt on Windows builds a device
    // from whichever PDU it sees first.
    struct ble_hs_adv_fields fields{};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &kServiceUuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(kTag, "ble_gap_adv_set_fields() failed (rc %d): the device is not advertising", rc);
        return;
    }

    const std::size_t name_length = std::strlen(device_name_);
    struct ble_hs_adv_fields response{};
    if (2U + name_length + kUuid128AdSize <= kAdvPayloadMax) {
        response.uuids128 = &kServiceUuid;
        response.num_uuids128 = 1;
        response.uuids128_is_complete = 1;
    }
    response.name = reinterpret_cast<const std::uint8_t*>(device_name_);
    response.name_len = static_cast<std::uint8_t>(name_length);
    response.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&response);
    if (rc != 0) {
        ESP_LOGE(kTag, "ble_gap_adv_rsp_set_fields() failed (rc %d): the device is not advertising", rc);
        return;
    }

    struct ble_gap_adv_params params{};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    std::uint8_t own_address_type = 0U;
    rc = ble_hs_id_infer_auto(0, &own_address_type);
    if (rc == 0) {
        rc = ble_gap_adv_start(own_address_type, nullptr, BLE_HS_FOREVER, &params, &gap_event, nullptr);
    }
    if (rc != 0) ESP_LOGE(kTag, "could not start advertising (rc %d)", rc);
}

int BleLink::on_gap_event(void* raw) noexcept {
    auto* event = static_cast<struct ble_gap_event*>(raw);
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status != 0) {
                start_advertising();  // a failed attempt: advertising stopped anyway
                return 0;
            }
            conn_handle_.store(event->connect.conn_handle);
            subscribed_.store(false);
            notify_in_flight_.store(false);
            {
                LinkLock lock(mutex_);
                queue_.clear();
            }
            {
                // 7.5-15 ms instead of the 30-50 ms centrals default to: every
                // notification waits for a connection event. A request only.
                struct ble_gap_upd_params update{};
                update.itvl_min = 6U;
                update.itvl_max = 12U;
                update.latency = 0U;
                update.supervision_timeout = 400U;
                ble_gap_update_params(event->connect.conn_handle, &update);
            }
            connections_.fetch_add(1U, std::memory_order_relaxed);
            if (events_.on_connect != nullptr) events_.on_connect(events_.context);
            return 0;

        case BLE_GAP_EVENT_DISCONNECT:
            conn_handle_.store(kNoConnection);
            subscribed_.store(false);
            notify_in_flight_.store(false);
            att_mtu_.store(23U);
            {
                LinkLock lock(mutex_);
                queue_.clear();
            }
            if (events_.on_disconnect != nullptr) events_.on_disconnect(events_.context);
            start_advertising();  // the whole second-session rejection is this line
            return 0;

        case BLE_GAP_EVENT_SUBSCRIBE:
            if (event->subscribe.attr_handle == g_tx_handle) {
                subscribed_.store(event->subscribe.cur_notify != 0);
            }
            return 0;

        case BLE_GAP_EVENT_MTU:
            att_mtu_.store(static_cast<std::uint16_t>(event->mtu.value));
            return 0;

        case BLE_GAP_EVENT_NOTIFY_TX:
            // The previous notification left the controller: send the next.
            notify_in_flight_.store(false);
            drain();
            return 0;

        default:
            return 0;
    }
}

int BleLink::on_rx_write(std::uint16_t conn_handle, const std::uint8_t* data,
                         std::size_t size) noexcept {
    if (conn_handle != conn_handle_.load()) return 0;
    bytes_in_.fetch_add(static_cast<std::uint32_t>(size), std::memory_order_relaxed);
    events_.on_receive(events_.context, data, size);
    return 0;
}

bool BleLink::send(const std::uint8_t* data, std::size_t size, Priority priority) noexcept {
    bool queued = false;
    if (connected()) {
        LinkLock lock(mutex_);
        queued = lock.locked() && queue_.push(data, size, priority);
    }
    if (!queued) {
        send_refused_.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    drain();
    return true;
}

void BleLink::drain() noexcept {
    if (!connected()) return;
    if (notify_in_flight_.exchange(true)) return;  // NOTIFY_TX will call back

    std::size_t chunk_size = 0U;
    {
        LinkLock lock(mutex_);
        const std::uint16_t mtu = att_mtu_.load();
        std::size_t capacity = mtu > 3U ? mtu - 3U : 20U;
        if (capacity > sizeof(notify_scratch_)) capacity = sizeof(notify_scratch_);
        // Pack as many queued bytes as fit, across frame boundaries: one
        // notification per small telemetry frame is what let bally_OS's
        // queue back up.
        const std::uint8_t* piece = nullptr;
        std::size_t piece_size = 0U;
        while (chunk_size < capacity && queue_.front(&piece, &piece_size)) {
            const std::size_t take = piece_size < capacity - chunk_size ? piece_size : capacity - chunk_size;
            std::memcpy(notify_scratch_ + chunk_size, piece, take);
            queue_.consume(take);
            chunk_size += take;
        }
    }
    if (chunk_size == 0U) {
        notify_in_flight_.store(false);
        return;
    }
    struct os_mbuf* om = ble_hs_mbuf_from_flat(notify_scratch_, chunk_size);
    if (om == nullptr) {
        notify_in_flight_.store(false);
        return;
    }
    // ble_gatts_notify_custom() frees `om` on every path. No NOTIFY_TX follows
    // a synchronous failure, so clear the latch here in that case.
    if (ble_gatts_notify_custom(conn_handle_.load(), g_tx_handle, om) != 0) {
        notify_in_flight_.store(false);
    } else {
        bytes_out_.fetch_add(static_cast<std::uint32_t>(chunk_size), std::memory_order_relaxed);
    }
}

#else  // !BTPDEV_HAS_BLE

BleLink::~BleLink() = default;
esp_err_t BleLink::start(const LinkEvents&, ErrorInfo& error) noexcept {
#if !CONFIG_BTPDEV_LINK_BLE
    return fail(error, BTPDEV_ERR_LINK_UNSUPPORTED, tag::kBle,
                "the BLE link is disabled in menuconfig (BTPDevice > Links)");
#else
    return fail(error, BTPDEV_ERR_LINK_UNSUPPORTED, tag::kBle,
                "Bluetooth is off: enable Component config > Bluetooth with the NimBLE host");
#endif
}
void BleLink::stop() noexcept {}
bool BleLink::send(const std::uint8_t*, std::size_t, Priority) noexcept { return false; }
bool BleLink::connected() const noexcept { return false; }
void BleLink::disconnect_peer() noexcept {}
void BleLink::on_sync() noexcept {}
int BleLink::on_gap_event(void*) noexcept { return 0; }
int BleLink::on_rx_write(std::uint16_t, const std::uint8_t*, std::size_t) noexcept { return 0; }
void BleLink::drain() noexcept {}
void BleLink::start_advertising() noexcept {}

#endif  // BTPDEV_HAS_BLE

LinkStats BleLink::stats() const noexcept {
    LinkStats s;
    s.connections = connections_.load(std::memory_order_relaxed);
    s.bytes_in = bytes_in_.load(std::memory_order_relaxed);
    s.bytes_out = bytes_out_.load(std::memory_order_relaxed);
    s.send_refused = send_refused_.load(std::memory_order_relaxed);
    return s;
}

}  // namespace btpdev
