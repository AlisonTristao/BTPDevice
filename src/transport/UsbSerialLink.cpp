#include "transport/UsbSerialLink.h"

#include <sdkconfig.h>

#include "core/LogTags.h"

#if CONFIG_BTPDEV_LINK_SERIAL && CONFIG_TINYUSB_CDC_ENABLED
#define BTPDEV_HAS_USB_SERIAL 1
#else
#define BTPDEV_HAS_USB_SERIAL 0
#endif

#if BTPDEV_HAS_USB_SERIAL
#include <esp_log.h>
#include <esp_mac.h>

#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_default_config.h"
#include "tusb.h"
#endif

namespace btpdev {

#if BTPDEV_HAS_USB_SERIAL

namespace {

constexpr const char* kTag = tag::kSerial;
constexpr char kLanguageEnglishUs[] = {0x09, 0x04, 0x00};
constexpr char kHexDigits[] = "0123456789ABCDEF";

// esp_tinyusb's CDC callbacks are plain C function pointers without a user
// argument, and TinyUSB's tud_cdc_tx_complete_cb() has none either: one CDC
// port, one link, one file-local pointer.
UsbSerialLink* g_link = nullptr;

void on_cdc_rx(int /*itf*/, cdcacm_event_t* /*event*/) {
    if (g_link != nullptr) g_link->handle_rx();
}

void on_cdc_line_state(int /*itf*/, cdcacm_event_t* event) {
    if (g_link != nullptr && event != nullptr) {
        g_link->handle_line_state(event->line_state_changed_data.dtr);
    }
}

}  // namespace

}  // namespace btpdev

// TinyUSB's weak hook (cdc_device.c): the previous IN transfer finished, the
// CDC TX FIFO has room again -- keep draining from here, on the TinyUSB task.
// An application that defines this hook itself cannot use the Serial link.
extern "C" void tud_cdc_tx_complete_cb(uint8_t itf) {
    (void)itf;
    if (btpdev::g_link != nullptr) btpdev::g_link->drain();
}

namespace btpdev {

UsbSerialLink::~UsbSerialLink() { stop(); }

esp_err_t UsbSerialLink::start(const LinkEvents& events, ErrorInfo& error) noexcept {
    if (running_.load()) return ESP_OK;
    if (events.on_receive == nullptr) {
        return fail(error, ESP_ERR_INVALID_ARG, kTag, "Serial link started without on_receive");
    }
    if (g_link != nullptr && g_link != this) {
        return fail(error, ESP_ERR_INVALID_STATE, kTag,
                    "another Serial link already owns the USB CDC port");
    }
    if (!mutex_.create()) {
        return fail(error, ESP_ERR_NO_MEM, kTag, "no memory for the Serial link mutex");
    }
    if (!queue_storage_.allocate(config_.send_queue_bytes)) {
        return fail(error, BTPDEV_ERR_MEMORY_BUDGET, kTag,
                    "no memory for the Serial send queue (%u bytes)",
                    static_cast<unsigned>(config_.send_queue_bytes));
    }
    queue_.bind(queue_storage_.data(), queue_storage_.size(), queue_storage_.size() / 4U);
    events_ = events;

    if (config_.install_driver) {
        // A stable per-device serial number keeps Windows from reusing one
        // "123456" COM port record for every board.
        std::uint8_t mac[6] = {};
        esp_efuse_mac_get_default(mac);
        for (std::size_t i = 0; i < sizeof(mac); ++i) {
            serial_[i * 2U] = kHexDigits[mac[i] >> 4];
            serial_[i * 2U + 1U] = kHexDigits[mac[i] & 0x0FU];
        }
        serial_[12] = '\0';
        const char* product = (config_.product_name != nullptr && config_.product_name[0] != '\0')
                                  ? config_.product_name
                                  : CONFIG_TINYUSB_DESC_PRODUCT_STRING;
        strings_[0] = kLanguageEnglishUs;
        strings_[1] = CONFIG_TINYUSB_DESC_MANUFACTURER_STRING;
        strings_[2] = product;
        strings_[3] = serial_;
        // The CDC interface string is what Windows lists the port under, so it
        // carries the device name too (TraceView matches ports by it).
        strings_[4] = product;

        tinyusb_config_t usb = TINYUSB_DEFAULT_CONFIG();
        usb.descriptor.string = strings_;
        usb.descriptor.string_count = 5;
        const esp_err_t err = tinyusb_driver_install(&usb);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            queue_storage_.reset();
            return fail(error, BTPDEV_ERR_LINK_START, kTag,
                        "tinyusb_driver_install() failed (%s)", esp_err_to_name(err));
        }
        if (err == ESP_ERR_INVALID_STATE) {
            ESP_LOGW(kTag, "TinyUSB was already installed: the USB name is the application's");
        }
    }

    g_link = this;
    tinyusb_config_cdcacm_t cdc = {};
    cdc.cdc_port = TINYUSB_CDC_ACM_0;
    cdc.callback_rx = &on_cdc_rx;
    cdc.callback_line_state_changed = &on_cdc_line_state;
    const esp_err_t err = tinyusb_cdcacm_init(&cdc);
    if (err != ESP_OK) {
        g_link = nullptr;
        queue_storage_.reset();
        return fail(error, BTPDEV_ERR_LINK_START, kTag, "tinyusb_cdcacm_init() failed (%s)",
                    esp_err_to_name(err));
    }
    running_.store(true);
    ESP_LOGI(kTag, "USB CDC ready as \"%s\"", strings_[2] != nullptr ? strings_[2] : "?");
    return ESP_OK;
}

void UsbSerialLink::stop() noexcept {
    if (!running_.exchange(false)) return;
    if (connected_.load()) close_session();
    tinyusb_cdcacm_deinit(TINYUSB_CDC_ACM_0);
    if (g_link == this) g_link = nullptr;
    {
        LinkLock lock(mutex_);
        queue_.bind(nullptr, 0U, 0U);
    }
    queue_storage_.reset();
}

void UsbSerialLink::handle_line_state(bool dtr) noexcept {
    if (dtr) {
        // A host (re)opening the port is a new client even if the previous
        // one never dropped DTR (a crashed app, a replugged cable).
        if (connected_.load()) close_session();
        open_session();
    } else if (connected_.load()) {
        close_session();
    }
}

void UsbSerialLink::handle_rx() noexcept {
    for (;;) {
        std::size_t got = 0U;
        if (tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, rx_buffer_, sizeof(rx_buffer_), &got) != ESP_OK ||
            got == 0U) {
            return;
        }
        if (!connected_.load()) open_session();  // a host that never raises DTR
        bytes_in_.fetch_add(static_cast<std::uint32_t>(got), std::memory_order_relaxed);
        events_.on_receive(events_.context, rx_buffer_, got);
    }
}

void UsbSerialLink::open_session() noexcept {
    {
        LinkLock lock(mutex_);
        queue_.clear();
    }
    connected_.store(true);
    connections_.fetch_add(1U, std::memory_order_relaxed);
    if (events_.on_connect != nullptr) events_.on_connect(events_.context);
}

void UsbSerialLink::close_session() noexcept {
    connected_.store(false);
    {
        LinkLock lock(mutex_);
        queue_.clear();
    }
    // Whatever the old session left in TinyUSB's FIFO must not reach the next
    // host as the head of its byte stream.
    tud_cdc_n_write_clear(0);
    if (events_.on_disconnect != nullptr) events_.on_disconnect(events_.context);
}

bool UsbSerialLink::send(const std::uint8_t* data, std::size_t size, Priority priority) noexcept {
    bool queued = false;
    if (connected_.load() && tud_ready()) {
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

void UsbSerialLink::drain() noexcept {
    if (!connected_.load()) return;
    bool wrote = false;
    {
        // Only this function writes into the CDC FIFO, always under the mutex
        // and in queue order: a frame may be split across passes (FIFO full)
        // but never interleaved with another one.
        LinkLock lock(mutex_);
        if (!lock.locked()) return;
        const std::uint8_t* chunk = nullptr;
        std::size_t size = 0U;
        while (queue_.front(&chunk, &size)) {
            const std::size_t written = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, chunk, size);
            if (written == 0U) break;
            queue_.consume(written);
            bytes_out_.fetch_add(static_cast<std::uint32_t>(written), std::memory_order_relaxed);
            wrote = true;
            if (written < size) break;
        }
    }
    // Non-blocking: TinyUSB keeps transmitting on its own task, and
    // tud_cdc_tx_complete_cb() brings us back for whatever is left.
    if (wrote) tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
}

#else  // !BTPDEV_HAS_USB_SERIAL

UsbSerialLink::~UsbSerialLink() = default;

esp_err_t UsbSerialLink::start(const LinkEvents& events, ErrorInfo& error) noexcept {
    (void)events;
#if !CONFIG_BTPDEV_LINK_SERIAL
    return fail(error, BTPDEV_ERR_LINK_UNSUPPORTED, tag::kSerial,
                "the Serial link is disabled in menuconfig (BTPDevice > Links)");
#else
    return fail(error, BTPDEV_ERR_LINK_UNSUPPORTED, tag::kSerial,
                "USB CDC is not available: this chip has no USB OTG, or "
                "CONFIG_TINYUSB_CDC_ENABLED is off (menuconfig > TinyUSB Stack > CDC)");
#endif
}

void UsbSerialLink::stop() noexcept {}
bool UsbSerialLink::send(const std::uint8_t*, std::size_t, Priority) noexcept { return false; }
void UsbSerialLink::handle_line_state(bool) noexcept {}
void UsbSerialLink::handle_rx() noexcept {}
void UsbSerialLink::drain() noexcept {}
void UsbSerialLink::open_session() noexcept {}
void UsbSerialLink::close_session() noexcept {}

#endif  // BTPDEV_HAS_USB_SERIAL

LinkStats UsbSerialLink::stats() const noexcept {
    LinkStats s;
    s.connections = connections_.load(std::memory_order_relaxed);
    s.bytes_in = bytes_in_.load(std::memory_order_relaxed);
    s.bytes_out = bytes_out_.load(std::memory_order_relaxed);
    s.send_refused = send_refused_.load(std::memory_order_relaxed);
    return s;
}

}  // namespace btpdev
