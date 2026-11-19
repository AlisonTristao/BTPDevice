#ifndef BTPDEV_UART_SERIAL_LINK_H
#define BTPDEV_UART_SERIAL_LINK_H

// Serial link over a UART: the board's USB-serial bridge (CP210x, CH340...)
// on chips without native USB (ESP32, ESP32-C3...), or on any chip when the
// bridge port is the one plugged in. Same wire format as UsbSerialLink
// (0x00 || COBS || 0x00, framed by the core), so TraceView treats both alike.
//
// A UART has no line state the device can see, so a "connection" opens with
// the first bytes received (TraceView sends HELLO as soon as the port opens)
// and closes only through disconnect_peer() -- the core calls it when the
// session watchdog expires. A new HELLO on an open connection already restarts
// the session in the core, so reopening the port in TraceView just works.
//
// Threads: one task of its own receives (uart event queue) and drains the
// send queue. send() only copies into the FrameQueue and wakes that task.
//
// UART0 is usually also the ESP_LOG console. With UartSerialConfig::
// mute_console the link swaps the log vprintf for one that discards while it
// runs, so no log line lands in the middle of a frame.

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "core/FrameQueue.h"
#include "core/Memory.h"
#include "transport/Link.h"
#include "transport/LinkMutex.h"

namespace btpdev {

struct UartSerialConfig {
    int port = 0;
    std::uint32_t baud = 921600U;
    int tx_pin = -1;   ///< -1: keep the port's current pin
    int rx_pin = -1;
    bool mute_console = true;
    std::size_t send_queue_bytes = 16384U;
};

class UartSerialLink final : public LinkDriver {
public:
    explicit UartSerialLink(const UartSerialConfig& config) noexcept : config_(config) {}
    ~UartSerialLink() override;

    const char* name() const noexcept override { return "Serial"; }
    Kind kind() const noexcept override { return Kind::Stream; }
    esp_err_t start(const LinkEvents& events, ErrorInfo& error) noexcept override;
    void stop() noexcept override;
    bool send(const std::uint8_t* data, std::size_t size, Priority priority) noexcept override;
    bool connected() const noexcept override { return connected_.load(); }
    void disconnect_peer() noexcept override;
    LinkStats stats() const noexcept override;
    void* task_handle() const noexcept override { return task_; }
    std::uint32_t task_stack_bytes() const noexcept override;

private:
    static void task_entry(void* arg) noexcept;
    void run() noexcept;
    void receive() noexcept;
    void drain() noexcept;
    void wake() noexcept;
    void open_session() noexcept;
    void close_session() noexcept;
    void release() noexcept;

    UartSerialConfig config_;
    LinkEvents events_{};
    LinkMutex mutex_;
    Buffer queue_storage_;
    FrameQueue queue_;   // guarded by mutex_
    QueueHandle_t uart_events_ = nullptr;   // owned by the UART driver
    TaskHandle_t task_ = nullptr;
    bool driver_installed_ = false;
    bool console_muted_ = false;
    std::atomic<bool> running_{false};
    std::atomic<bool> task_done_{true};
    std::atomic<bool> connected_{false};
    std::atomic<std::uint32_t> connections_{0U};
    std::atomic<std::uint32_t> bytes_in_{0U};
    std::atomic<std::uint32_t> bytes_out_{0U};
    std::atomic<std::uint32_t> send_refused_{0U};

    std::uint8_t rx_buffer_[256] = {};   // link task only
    std::uint8_t tx_chunk_[256] = {};    // link task only
};

}  // namespace btpdev

#endif  // BTPDEV_UART_SERIAL_LINK_H
