#ifndef BTPDEV_LINK_H
#define BTPDEV_LINK_H

// What every link driver (Serial, TCP, BLE, ESP-NOW) looks like to the core
// (btpdev::Link, in config.h, is the user-facing choice of which one to use). Four
// real implementations, so a small interface is justified (CONTRIBUTING.md
// section 2): the core picks one per boot and never needs to know which.
//
// A link is BTP-unaware. It moves bytes and reports connections; the core
// owns framing (COBS for Stream links) and the protocol.
//
// Contexts:
//   * start(), stop(): the task that runs begin() (or the test app).
//   * send(): any task; it copies the bytes into the link's FrameQueue and
//     never blocks on the peer.
//   * poll(): the core's communication task, every pass (Wi-Fi state machine,
//     ESP-NOW retries); a link that needs no housekeeping ignores it.
//   * LinkEvents callbacks: the link's OWN driver task (TCP task, NimBLE host,
//     TinyUSB, Wi-Fi). They must only copy and return -- never log, block or
//     call into btp::Node (CONTRIBUTING.md "Concorrência").

#include <btpdev/error.h>

#include <cstddef>
#include <cstdint>

#include "core/FrameQueue.h"

namespace btpdev {

struct LinkEvents {
    void* context = nullptr;
    /// Received bytes: a chunk of the byte stream (Stream) or one whole
    /// datagram (Datagram). Borrowed for the duration of the call only.
    void (*on_receive)(void* context, const std::uint8_t* data, std::size_t size) noexcept = nullptr;
    /// A peer connected (TCP client accepted, BLE central subscribed, USB host
    /// raised DTR). Not used by ESP-NOW, which has no connection.
    void (*on_connect)(void* context) noexcept = nullptr;
    void (*on_disconnect)(void* context) noexcept = nullptr;
};

/// Counters every link keeps (read from any task; approximate under load).
struct LinkStats {
    std::uint32_t connections = 0U;
    std::uint32_t bytes_in = 0U;
    std::uint32_t bytes_out = 0U;
    std::uint32_t send_refused = 0U;   ///< send() returned false
};

class LinkDriver {
public:
    enum class Kind : std::uint8_t {
        Stream,    ///< byte pipe: frames travel as 0x00 || COBS || 0x00
        Datagram,  ///< one frame per datagram (ESP-NOW)
    };

    virtual ~LinkDriver() = default;

    /// "Serial", "TCP", "BLE" or "ESP-NOW" (for logs).
    virtual const char* name() const noexcept = 0;
    virtual Kind kind() const noexcept = 0;

    /// Brings the link up. On failure returns the code also recorded in
    /// `error`, leaves nothing running and may be retried.
    virtual esp_err_t start(const LinkEvents& events, ErrorInfo& error) noexcept = 0;
    virtual void stop() noexcept = 0;

    /// Queues one unit for the peer: an already COBS-framed frame (Stream) or
    /// one encoded frame (Datagram). False when there is no peer, the unit is
    /// too big, or the priority admission refused it; nothing is queued then.
    virtual bool send(const std::uint8_t* data, std::size_t size, Priority priority) noexcept = 0;

    /// A peer is there and can receive (for TCP: a client; for BLE: a central
    /// subscribed to notifications; for ESP-NOW: always true once started).
    virtual bool connected() const noexcept = 0;

    virtual void poll(std::uint32_t now_ms) noexcept { (void)now_ms; }

    /// Drops the current peer (the session watchdog expired: the peer is gone
    /// but the transport did not notice -- a half-open TCP socket, a BLE
    /// central that vanished). A link that cannot do this ignores it.
    virtual void disconnect_peer() noexcept {}

    virtual LinkStats stats() const noexcept = 0;
};

}  // namespace btpdev

#endif  // BTPDEV_LINK_H
