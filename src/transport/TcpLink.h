#ifndef BTPDEV_TCP_LINK_H
#define BTPDEV_TCP_LINK_H

// TCP link: a Wi-Fi station (WifiStation) plus a BTP control server on one
// port, found by TraceView as <name>.local over mDNS. Ported from bally_OS
// lib/TcpBtpServer, with a FrameQueue instead of a heap copy per frame and
// with the Wi-Fi connection it used to borrow from OTAUpdater folded in.
//
// One ACTIVE client (the control session) plus, briefly, one PENDING second
// connection: BTP says the server accepts it at the socket level and rejects
// it at HELLO with HELLO_RESULT BUSY (session-and-terminal.md section 3.4).
// Recognising HELLO needs BTP, so the pending bytes go to a PendingHandler
// the core installs; without one -- or when it never answers -- the pending
// socket is closed after kPendingHelloDeadlineMs, silently.
//
// The server task only moves bytes: select() with a short timeout, recv()
// into a buffer owned by the link, non-blocking send() of whatever the queue
// holds. A partial write keeps the rest of the frame at the head.

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "core/FrameQueue.h"
#include "core/Memory.h"
#include "transport/Link.h"
#include "transport/LinkMutex.h"
#include "transport/WifiStation.h"

namespace btpdev {

struct TcpLinkConfig {
    /// Device name: mDNS instance name, and the hostname through
    /// mdns_host_from_name(). Must stay valid while the link runs.
    const char* device_name = nullptr;
    std::uint16_t port = 44300U;
    std::size_t send_queue_bytes = 16384U;
    /// Known networks (parsed networks file). Must stay valid while the link runs.
    const WifiNetwork* networks = nullptr;
    std::size_t network_count = 0U;
    std::uint32_t task_stack_bytes = 6144U;
    std::uint32_t task_priority = 5U;
    int task_core = 0;
};

class TcpLink final : public LinkDriver {
public:
    static constexpr std::uint32_t kPendingHelloDeadlineMs = 2000U;
    // One recv() chunk; it has to fit the core's receive queue whole (the
    // Small preset has 4 KB), so a little over one TCP segment.
    static constexpr std::size_t kReceiveBufferSize = 1536U;
    static constexpr std::size_t kPendingReceiveBufferSize = 512U;

    /// Bytes from the pending (second, not yet rejected) connection. Runs on
    /// the server task; may call send_pending() / close_pending().
    struct PendingHandler {
        void* context = nullptr;
        void (*on_receive)(void* context, const std::uint8_t* data, std::size_t size) noexcept = nullptr;
    };

    explicit TcpLink(const TcpLinkConfig& config) noexcept : config_(config) {}
    ~TcpLink() override;

    const char* name() const noexcept override { return "TCP"; }
    Kind kind() const noexcept override { return Kind::Stream; }
    esp_err_t start(const LinkEvents& events, ErrorInfo& error) noexcept override;
    void stop() noexcept override;
    bool send(const std::uint8_t* data, std::size_t size, Priority priority) noexcept override;
    bool connected() const noexcept override { return client_fd_.load() >= 0; }
    void poll(std::uint32_t now_ms) noexcept override;
    void disconnect_peer() noexcept override { close_client(); }
    LinkStats stats() const noexcept override;

    void set_pending_handler(const PendingHandler& handler) noexcept { pending_handler_ = handler; }
    /// One best-effort write to the pending connection (the BUSY reply).
    bool send_pending(const std::uint8_t* data, std::size_t size) noexcept;
    void close_pending() noexcept;
    /// Drops the active client without stopping the server.
    void close_client() noexcept;

    const WifiStation& wifi() const noexcept { return wifi_; }

private:
    static void task_entry(void* context) noexcept;
    void run() noexcept;
    esp_err_t open_listener(ErrorInfo& error) noexcept;
    void drain() noexcept;
    void start_mdns() noexcept;

    TcpLinkConfig config_;
    LinkEvents events_{};
    PendingHandler pending_handler_{};
    WifiStation wifi_;
    LinkMutex mutex_;
    Buffer queue_storage_;
    FrameQueue queue_;   // guarded by mutex_

    std::atomic<bool> running_{false};
    TaskHandle_t task_ = nullptr;
    int listen_fd_ = -1;
    std::atomic<int> client_fd_{-1};
    std::atomic<int> pending_fd_{-1};
    std::uint32_t pending_started_ms_ = 0U;
    bool mdns_started_ = false;

    std::atomic<std::uint32_t> connections_{0U};
    std::atomic<std::uint32_t> bytes_in_{0U};
    std::atomic<std::uint32_t> bytes_out_{0U};
    std::atomic<std::uint32_t> send_refused_{0U};

    // Owned by the link and only touched by run(): keeps them off the task
    // stack, which the receive callback chain also needs.
    std::uint8_t receive_buffer_[kReceiveBufferSize] = {};
    std::uint8_t pending_receive_buffer_[kPendingReceiveBufferSize] = {};
};

}  // namespace btpdev

#endif  // BTPDEV_TCP_LINK_H
