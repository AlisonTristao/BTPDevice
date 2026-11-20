#include "transport/UartSerialLink.h"

#include <sdkconfig.h>

#include <algorithm>
#include <cstdarg>
#include <cstring>

#include <driver/uart.h>
#include <esp_log.h>

#include "core/LogTags.h"

namespace tracekit {

#if CONFIG_TRACEKIT_LINK_SERIAL

namespace {

constexpr const char* kTag = tag::kSerial;
constexpr int kRxRingBytes = 2048;   // > the hardware FIFO, as the driver requires
constexpr int kTxRingBytes = 4096;   // uart_write_bytes() returns once copied here
constexpr int kEventQueueLength = 20;
constexpr std::uint32_t kTaskStackBytes = 3072U;
constexpr UBaseType_t kTaskPriority = 9U;   // just below the comms task (10)
// Not a driver event: the value send() posts to wake the link task.
constexpr uart_event_type_t kWakeEvent = UART_EVENT_MAX;

bool is_console_port(int port) noexcept {
#if CONFIG_ESP_CONSOLE_UART && defined(CONFIG_ESP_CONSOLE_UART_NUM)
    return port == CONFIG_ESP_CONSOLE_UART_NUM;
#else
    (void)port;
    return false;
#endif
}

// While the link owns the console UART: a log line in the middle of a frame
// would corrupt it, so ESP_LOG output goes nowhere.
int discard_log(const char* /*format*/, va_list /*args*/) { return 0; }
vprintf_like_t g_saved_vprintf = nullptr;

}  // namespace

UartSerialLink::~UartSerialLink() { stop(); }

esp_err_t UartSerialLink::start(const LinkEvents& events, ErrorInfo& error) noexcept {
    if (running_.load()) return ESP_OK;
    if (events.on_receive == nullptr) {
        return fail(error, ESP_ERR_INVALID_ARG, kTag, "Serial link started without on_receive");
    }
    const auto port = static_cast<uart_port_t>(config_.port);
    if (config_.port < 0 || config_.port >= SOC_UART_HP_NUM) {
        return fail(error, TRACEKIT_ERR_LINK_START, kTag, "UART%d does not exist on this chip",
                    config_.port);
    }
    if (uart_is_driver_installed(port)) {
        return fail(error, TRACEKIT_ERR_LINK_START, kTag,
                    "UART%d already has a driver installed by someone else", config_.port);
    }
    if (!mutex_.create()) {
        return fail(error, ESP_ERR_NO_MEM, kTag, "no memory for the Serial link mutex");
    }
    if (!queue_storage_.allocate(config_.send_queue_bytes)) {
        return fail(error, TRACEKIT_ERR_MEMORY_BUDGET, kTag,
                    "no memory for the Serial send queue (%u bytes)",
                    static_cast<unsigned>(config_.send_queue_bytes));
    }
    queue_.bind(queue_storage_.data(), queue_storage_.size(), queue_storage_.size() / 4U);
    events_ = events;

    const bool console = is_console_port(config_.port);
    if (console && config_.mute_console) {
        // The last line the console shows, at its own baud rate, before the
        // port switches to the link's.
        ESP_LOGW(kTag, "UART%d becomes the BTP link at %u baud: ESP_LOG is muted on it from now on",
                 config_.port, static_cast<unsigned>(config_.baud));
    }

    esp_err_t err = uart_driver_install(port, kRxRingBytes, kTxRingBytes, kEventQueueLength,
                                        &uart_events_, 0);
    if (err != ESP_OK) {
        release();
        return fail(error, TRACEKIT_ERR_LINK_START, kTag, "uart_driver_install(UART%d) failed (%s)",
                    config_.port, esp_err_to_name(err));
    }
    driver_installed_ = true;
    if (console) uart_wait_tx_done(port, ms_to_ticks(50));  // the line above leaves at the old rate
    if (console && config_.mute_console) {
        g_saved_vprintf = esp_log_set_vprintf(&discard_log);
        console_muted_ = true;
    }

    uart_config_t line = {};
    line.baud_rate = static_cast<int>(config_.baud);
    line.data_bits = UART_DATA_8_BITS;
    line.parity = UART_PARITY_DISABLE;
    line.stop_bits = UART_STOP_BITS_1;
    line.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    line.source_clk = UART_SCLK_DEFAULT;
    err = uart_param_config(port, &line);
    if (err == ESP_OK) {
        err = uart_set_pin(port, config_.tx_pin < 0 ? UART_PIN_NO_CHANGE : config_.tx_pin,
                           config_.rx_pin < 0 ? UART_PIN_NO_CHANGE : config_.rx_pin,
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK) {
        release();
        return fail(error, TRACEKIT_ERR_LINK_START, kTag, "UART%d setup failed (%s): check baud and pins",
                    config_.port, esp_err_to_name(err));
    }
    // Whatever arrived before (boot noise, a host that was already talking)
    // is not the start of a frame.
    uart_flush_input(port);

    running_.store(true);
    task_done_.store(false);
    if (!spawn_task(&UartSerialLink::task_entry, "tracekit_uart", kTaskStackBytes, this, kTaskPriority, 0,
                    /*may_be_external=*/true, &task_, &task_memory_)) {
        running_.store(false);
        task_done_.store(true);
        task_ = nullptr;
        release();
        return fail(error, ESP_ERR_NO_MEM, kTag, "no memory for the Serial link task");
    }
    if (!console_muted_) {
        ESP_LOGI(kTag, "UART%d ready at %u baud", config_.port, static_cast<unsigned>(config_.baud));
    }
    return ESP_OK;
}

void UartSerialLink::stop() noexcept {
    if (!running_.exchange(false)) return;
    wake();
    // The task notices running_ within one event wait and deletes itself.
    for (int i = 0; i < 100 && !task_done_.load(); ++i) vTaskDelay(ms_to_ticks(10));
    release_task(task_, &task_memory_);
    task_ = nullptr;
    if (connected_.load()) close_session();
    release();
}

void UartSerialLink::release() noexcept {
    if (driver_installed_) {
        uart_driver_delete(static_cast<uart_port_t>(config_.port));
        driver_installed_ = false;
        uart_events_ = nullptr;
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

void UartSerialLink::task_entry(void* arg) noexcept {
    auto* self = static_cast<UartSerialLink*>(arg);
    self->run();
    finish_task(self->task_memory_);
}

void UartSerialLink::run() noexcept {
    const auto port = static_cast<uart_port_t>(config_.port);
    uart_event_t event = {};
    while (running_.load()) {
        if (xQueueReceive(uart_events_, &event, ms_to_ticks(50)) == pdTRUE) {
            switch (event.type) {
                case UART_DATA:
                    receive();
                    break;
                case UART_FIFO_OVF:
                case UART_BUFFER_FULL:
                    // Bytes were lost: drop the rest too. The core's stream
                    // decoder resynchronizes on the next 0x00 delimiter.
                    uart_flush_input(port);
                    xQueueReset(uart_events_);
                    break;
                default:  // kWakeEvent, break, frame and parity errors
                    break;
            }
        }
        drain();
    }
    task_done_.store(true);
}

void UartSerialLink::receive() noexcept {
    const auto port = static_cast<uart_port_t>(config_.port);
    for (;;) {
        const int got = uart_read_bytes(port, rx_buffer_, sizeof(rx_buffer_), 0);
        if (got <= 0) return;
        // No line state on a UART: the first bytes from a host open the session.
        if (!connected_.load()) open_session();
        bytes_in_.fetch_add(static_cast<std::uint32_t>(got), std::memory_order_relaxed);
        events_.on_receive(events_.context, rx_buffer_, static_cast<std::size_t>(got));
    }
}

void UartSerialLink::drain() noexcept {
    const auto port = static_cast<uart_port_t>(config_.port);
    while (connected_.load()) {
        std::size_t size = 0U;
        {
            // Copy a chunk out and write it after unlocking, so a slow UART
            // never holds send() on the comms task. Only this task drains, so
            // frames still leave in queue order and are never interleaved.
            LinkLock lock(mutex_, ms_to_ticks(10));
            if (!lock.locked()) return;
            const std::uint8_t* chunk = nullptr;
            if (!queue_.front(&chunk, &size)) return;
            size = std::min(size, sizeof(tx_chunk_));
            std::memcpy(tx_chunk_, chunk, size);
            queue_.consume(size);
        }
        // Blocks only while the TX ring is full: this task is the only writer.
        const int wrote = uart_write_bytes(port, tx_chunk_, size);
        if (wrote > 0) bytes_out_.fetch_add(static_cast<std::uint32_t>(wrote), std::memory_order_relaxed);
    }
}

void UartSerialLink::wake() noexcept {
    QueueHandle_t queue = uart_events_;
    if (queue == nullptr) return;
    uart_event_t event = {};
    event.type = kWakeEvent;
    xQueueSend(queue, &event, 0);  // full: the task is busy and drains anyway
}

bool UartSerialLink::send(const std::uint8_t* data, std::size_t size, Priority priority) noexcept {
    bool queued = false;
    if (running_.load() && connected_.load()) {
        LinkLock lock(mutex_);
        queued = lock.locked() && queue_.push(data, size, priority);
    }
    if (!queued) {
        send_refused_.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    wake();
    return true;
}

void UartSerialLink::disconnect_peer() noexcept {
    if (connected_.load()) close_session();
}

void UartSerialLink::open_session() noexcept {
    {
        LinkLock lock(mutex_);
        queue_.clear();
    }
    connected_.store(true);
    connections_.fetch_add(1U, std::memory_order_relaxed);
    if (events_.on_connect != nullptr) events_.on_connect(events_.context);
}

void UartSerialLink::close_session() noexcept {
    if (!connected_.exchange(false)) return;
    {
        LinkLock lock(mutex_);
        queue_.clear();
    }
    if (events_.on_disconnect != nullptr) events_.on_disconnect(events_.context);
}

#else  // !CONFIG_TRACEKIT_LINK_SERIAL

UartSerialLink::~UartSerialLink() = default;

esp_err_t UartSerialLink::start(const LinkEvents& events, ErrorInfo& error) noexcept {
    (void)events;
    return fail(error, TRACEKIT_ERR_LINK_UNSUPPORTED, tag::kSerial,
                "the Serial link is disabled in menuconfig (TraceKit > Links)");
}

void UartSerialLink::stop() noexcept {}
void UartSerialLink::release() noexcept {}
void UartSerialLink::task_entry(void*) noexcept {}
void UartSerialLink::run() noexcept {}
void UartSerialLink::receive() noexcept {}
void UartSerialLink::drain() noexcept {}
void UartSerialLink::wake() noexcept {}
bool UartSerialLink::send(const std::uint8_t*, std::size_t, Priority) noexcept { return false; }
void UartSerialLink::disconnect_peer() noexcept {}
void UartSerialLink::open_session() noexcept {}
void UartSerialLink::close_session() noexcept {}

#endif  // CONFIG_TRACEKIT_LINK_SERIAL

LinkStats UartSerialLink::stats() const noexcept {
    LinkStats s;
    s.connections = connections_.load(std::memory_order_relaxed);
    s.bytes_in = bytes_in_.load(std::memory_order_relaxed);
    s.bytes_out = bytes_out_.load(std::memory_order_relaxed);
    s.send_refused = send_refused_.load(std::memory_order_relaxed);
    return s;
}

std::uint32_t UartSerialLink::task_stack_bytes() const noexcept { return task_ != nullptr ? kTaskStackBytes : 0U; }

}  // namespace tracekit
