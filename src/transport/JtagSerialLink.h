#ifndef TRACEKIT_JTAG_SERIAL_LINK_H
#define TRACEKIT_JTAG_SERIAL_LINK_H

// Serial link over the chip's USB-Serial-JTAG port (ESP32-S3, C3, C5, C6,
// H2, P4...): the USB port esptool flashes through and the console usually
// prints to. On a C3 or C6 board without a USB-serial bridge it is the only
// port there is. Same wire format as the other serial links (0x00 || COBS ||
// 0x00, framed by the core), so TraceView treats them alike.
//
// The peripheral shows no DTR to the firmware, so, like a UART, a
// "connection" opens with the first bytes received (TraceView sends HELLO as
// soon as the port opens) and closes through disconnect_peer() -- or when the
// USB host goes away (no start-of-frame packets: cable pulled).
//
// The console shares the port: while the link runs ESP_LOG is discarded (a
// log line in the middle of a frame would corrupt it), on every console.
// Plain printf() is not caught: do not use it while the link runs.
//
// Threads: one task of its own receives and drains the send queue. send()
// only copies into the FrameQueue and notifies that task.

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "core/FrameQueue.h"
#include "core/Memory.h"
#include "core/TaskSpawn.h"
#include "transport/Link.h"
#include "transport/LinkMutex.h"

namespace tracekit {

struct JtagSerialConfig {
    std::size_t send_queue_bytes = 16384U;
};

/// True when the chip has a USB-Serial-JTAG port and a USB host is talking
/// to it now (start-of-frame packets seen): SerialPort::Auto picks this port
/// then. Always false on chips without one.
bool jtag_serial_host_present() noexcept;

class JtagSerialLink final : public LinkDriver {
public:
    explicit JtagSerialLink(const JtagSerialConfig& config) noexcept : config_(config) {}
    ~JtagSerialLink() override;

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
    void open_session() noexcept;
    void close_session() noexcept;
    void release() noexcept;

    JtagSerialConfig config_;
    LinkEvents events_{};
    LinkMutex mutex_;
    Buffer queue_storage_;
    FrameQueue queue_;   // guarded by mutex_
    TaskHandle_t task_ = nullptr;
    TaskMemory task_memory_{};  // PSRAM stack (Psram::Stacks)
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

}  // namespace tracekit

#endif  // TRACEKIT_JTAG_SERIAL_LINK_H
