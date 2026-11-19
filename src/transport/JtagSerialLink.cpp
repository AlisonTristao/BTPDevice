#include "transport/JtagSerialLink.h"

#include <sdkconfig.h>

#include <algorithm>
#include <cstdarg>
#include <cstring>

#include <esp_log.h>
#include <soc/soc_caps.h>

#include "core/LogTags.h"

#if CONFIG_BTPDEV_LINK_SERIAL && SOC_USB_SERIAL_JTAG_SUPPORTED
#include <driver/usb_serial_jtag.h>
#define BTPDEV_HAS_JTAG_SERIAL 1
#endif

namespace btpdev {

namespace {
constexpr std::uint32_t kTaskStackBytes = 3072U;
}  // namespace

#if BTPDEV_HAS_JTAG_SERIAL

namespace {

constexpr const char* kTag = tag::kSerial;
constexpr std::uint32_t kRxRingBytes = 2048U;
constexpr std::uint32_t kTxRingBytes = 4096U;
constexpr UBaseType_t kTaskPriority = 9U;   // just below the comms task (10)

// The console prints to this port (as the main or the secondary console):
// ESP_LOG must stop while the link runs, or a log line lands mid-frame.
constexpr bool kConsoleOnJtag =
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG || CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG
    true;
#else
    false;
#endif

int discard_log(const char* /*format*/, va_list /*args*/) { return 0; }
vprintf_like_t g_saved_vprintf = nullptr;

}  // namespace

bool jtag_serial_host_present() noexcept { return usb_serial_jtag_is_connected(); }

JtagSerialLink::~JtagSerialLink() { stop(); }

esp_err_t JtagSerialLink::start(const LinkEvents& events, ErrorInfo& error) noexcept {
    if (running_.load()) return ESP_OK;
    if (events.on_receive == nullptr) {
        return fail(error, ESP_ERR_INVALID_ARG, kTag, "Serial link started without on_receive");
    }
    if (usb_serial_jtag_is_driver_installed()) {
        return fail(error, BTPDEV_ERR_LINK_START, kTag,
                    "the USB-Serial-JTAG driver is already installed by someone else");
    }
    if (!mutex_.create()) {
        return fail(error, ESP_ERR_NO_MEM, kTag, "no memory for the Serial link mutex");
    }
    if (!queue_storage_.allocate(config_.send_queue_bytes)) {
        return fail(error, BTPDEV_ERR_MEMORY_BUDGET, kTag, "no memory for the Serial send queue (%u bytes)",
                    static_cast<unsigned>(config_.send_queue_bytes));
    }
    queue_.bind(queue_storage_.data(), queue_storage_.size(), queue_storage_.size() / 4U);
    events_ = events;

    if (kConsoleOnJtag) {
        ESP_LOGW(kTag, "the USB-Serial-JTAG port becomes the BTP link: ESP_LOG is muted from now on "
                       "(logs reach TraceView through btpdev::log())");
        vTaskDelay(ms_to_ticks(20));  // let that line leave through the console first
        g_saved_vprintf = esp_log_set_vprintf(&discard_log);
        console_muted_ = true;
    }

    usb_serial_jtag_driver_config_t driver = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    driver.rx_buffer_size = kRxRingBytes;
    driver.tx_buffer_size = kTxRingBytes;
    const esp_err_t err = usb_serial_jtag_driver_install(&driver);
    if (err != ESP_OK) {
        release();
        return fail(error, BTPDEV_ERR_LINK_START, kTag, "usb_serial_jtag_driver_install failed (%s)",
                    esp_err_to_name(err));
    }
    driver_installed_ = true;
    // Whatever is already in the RX ring (a host typing before we listened)
    // is not the start of a frame.
    while (usb_serial_jtag_read_bytes(rx_buffer_, sizeof(rx_buffer_), 0) > 0) {
    }

    running_.store(true);
    task_done_.store(false);
    if (xTaskCreatePinnedToCore(&JtagSerialLink::task_entry, "btpdev_usj", kTaskStackBytes, this, kTaskPriority,
                                &task_, 0) != pdPASS) {
        running_.store(false);
        task_done_.store(true);
        task_ = nullptr;
        release();
        return fail(error, ESP_ERR_NO_MEM, kTag, "no memory for the Serial link task");
    }
    if (!console_muted_) ESP_LOGI(kTag, "USB-Serial-JTAG ready");
    return ESP_OK;
}

void JtagSerialLink::stop() noexcept {
    if (!running_.exchange(false)) return;
    if (task_ != nullptr) xTaskNotifyGive(task_);
    for (int i = 0; i < 100 && !task_done_.load(); ++i) vTaskDelay(ms_to_ticks(10));
    task_ = nullptr;
    if (connected_.load()) close_session();
    release();
}

void JtagSerialLink::release() noexcept {
    if (driver_installed_) {
        usb_serial_jtag_driver_uninstall();
        driver_installed_ = false;
    }
    if (console_muted_) {
        esp_log_set_vprintf(g_saved_vprintf != nullptr ? g_saved_vprintf : &vprintf);
        g_saved_vprintf = nullptr;
        console_muted_ = false;
    }
    {
        LinkLock lock(mutex_);
        queue_.bind(nullptr, 0U, 0U);
    }
    queue_storage_.reset();
}

void JtagSerialLink::task_entry(void* arg) noexcept {
    static_cast<JtagSerialLink*>(arg)->run();
    vTaskDelete(nullptr);
}

void JtagSerialLink::run() noexcept {
    while (running_.load()) {
        // The driver has no receive event: wait one tick for a send() to
        // notify us, then poll the RX ring. Worst-case receive latency is one
        // FreeRTOS tick.
        ulTaskNotifyTake(pdTRUE, 1);
        receive();
        // Cable pulled (no start-of-frame packets): the session is over.
        if (connected_.load() && !usb_serial_jtag_is_connected()) close_session();
        drain();
    }
    task_done_.store(true);
}

void JtagSerialLink::receive() noexcept {
    for (;;) {
        const int got = usb_serial_jtag_read_bytes(rx_buffer_, sizeof(rx_buffer_), 0);
        if (got <= 0) return;
        // No line state visible: the first bytes from a host open the session.
        if (!connected_.load()) open_session();
        bytes_in_.fetch_add(static_cast<std::uint32_t>(got), std::memory_order_relaxed);
        events_.on_receive(events_.context, rx_buffer_, static_cast<std::size_t>(got));
    }
}

void JtagSerialLink::drain() noexcept {
    while (connected_.load() && running_.load()) {
        std::size_t size = 0U;
        {
            // Copy a chunk out and write it after unlocking, so a slow host
            // never holds send() on the comms task. Only this task drains, so
            // frames leave in queue order and are never interleaved.
            LinkLock lock(mutex_, ms_to_ticks(10));
            if (!lock.locked()) return;
            const std::uint8_t* chunk = nullptr;
            if (!queue_.front(&chunk, &size)) return;
            size = std::min(size, sizeof(tx_chunk_));
            std::memcpy(tx_chunk_, chunk, size);
            queue_.consume(size);
        }
        // A host that stopped reading (port closed, cable still in) fills the
        // ring: give up on this chunk after a while rather than block forever.
        // The core's stream decoder on the other side resynchronizes on the
        // next 0x00 delimiter.
        std::size_t sent = 0U;
        for (int tries = 0; sent < size && tries < 10 && running_.load(); ++tries) {
            const int wrote = usb_serial_jtag_write_bytes(tx_chunk_ + sent, size - sent, ms_to_ticks(20));
            if (wrote > 0) sent += static_cast<std::size_t>(wrote);
        }
        bytes_out_.fetch_add(static_cast<std::uint32_t>(sent), std::memory_order_relaxed);
    }
}

bool JtagSerialLink::send(const std::uint8_t* data, std::size_t size, Priority priority) noexcept {
    bool queued = false;
    if (running_.load() && connected_.load()) {
        LinkLock lock(mutex_);
        queued = lock.locked() && queue_.push(data, size, priority);
    }
    if (!queued) {
        send_refused_.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    TaskHandle_t task = task_;
    if (task != nullptr) xTaskNotifyGive(task);
    return true;
}

void JtagSerialLink::disconnect_peer() noexcept {
    if (connected_.load()) close_session();
}

void JtagSerialLink::open_session() noexcept {
    {
        LinkLock lock(mutex_);
        queue_.clear();
    }
    connected_.store(true);
    connections_.fetch_add(1U, std::memory_order_relaxed);
    if (events_.on_connect != nullptr) events_.on_connect(events_.context);
}

void JtagSerialLink::close_session() noexcept {
    if (!connected_.exchange(false)) return;
    {
        LinkLock lock(mutex_);
        queue_.clear();
    }
    if (events_.on_disconnect != nullptr) events_.on_disconnect(events_.context);
}

#else  // no USB-Serial-JTAG on this chip, or the Serial link is off

bool jtag_serial_host_present() noexcept { return false; }

JtagSerialLink::~JtagSerialLink() = default;

esp_err_t JtagSerialLink::start(const LinkEvents& events, ErrorInfo& error) noexcept {
    (void)events;
#if !CONFIG_BTPDEV_LINK_SERIAL
    return fail(error, BTPDEV_ERR_LINK_UNSUPPORTED, tag::kSerial,
                "the Serial link is disabled in menuconfig (BTPDevice > Links)");
#else
    return fail(error, BTPDEV_ERR_LINK_UNSUPPORTED, tag::kSerial,
                "this chip has no USB-Serial-JTAG port (SerialPort::UsbJtag): use SerialPort::Uart");
#endif
}

void JtagSerialLink::stop() noexcept {}
void JtagSerialLink::release() noexcept {}
void JtagSerialLink::task_entry(void*) noexcept {}
void JtagSerialLink::run() noexcept {}
void JtagSerialLink::receive() noexcept {}
void JtagSerialLink::drain() noexcept {}
bool JtagSerialLink::send(const std::uint8_t*, std::size_t, Priority) noexcept { return false; }
void JtagSerialLink::disconnect_peer() noexcept {}
void JtagSerialLink::open_session() noexcept {}
void JtagSerialLink::close_session() noexcept {}

#endif

LinkStats JtagSerialLink::stats() const noexcept {
    LinkStats s;
    s.connections = connections_.load(std::memory_order_relaxed);
    s.bytes_in = bytes_in_.load(std::memory_order_relaxed);
    s.bytes_out = bytes_out_.load(std::memory_order_relaxed);
    s.send_refused = send_refused_.load(std::memory_order_relaxed);
    return s;
}

std::uint32_t JtagSerialLink::task_stack_bytes() const noexcept { return task_ != nullptr ? kTaskStackBytes : 0U; }

}  // namespace btpdev
