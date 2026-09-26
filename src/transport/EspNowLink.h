#ifndef BTPDEV_ESPNOW_LINK_H
#define BTPDEV_ESPNOW_LINK_H

// ESP-NOW link: one BTP frame per datagram, to and from the bally_dongle
// (which relays to TraceView). Ported from bally_OS's ROBOT ESP-NOW code and
// TxScheduler, simplified to two FrameQueues:
//
//   * Normal frames (replies, terminal, status, manifest fragments) go to the
//     dongle as unicast, ONE in flight at a time, waiting for the driver's
//     delivery callback (or kDeliveryTimeoutMs) before the next -- that makes
//     the delivery callback unambiguous and keeps a multi-fragment manifest in
//     order.
//   * Telemetry is fire-and-forget to the broadcast address (a newer sample
//     supersedes a lost one; BTP's sequence numbers expose the gaps), sent
//     back to back while the driver accepts them.
//
// Received datagrams are accepted from the configured peer only (the dongle).
// Frames are at most btp::kEspNowMaxFrameSize (250) octets: the core encodes
// with the ESP-NOW transport profile, so nothing is fragmented here.
//
// Housekeeping (sending the next frame, delivery timeouts) happens in poll()
// and after each send(); esp_now_send() is never called from the driver's
// own callbacks.

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "core/FrameQueue.h"
#include "core/Memory.h"
#include "transport/Link.h"
#include "transport/LinkMutex.h"

namespace btpdev {

struct EspNowLinkConfig {
    std::uint8_t peer_mac[6] = {};
    std::uint8_t channel = 1U;
    std::size_t normal_queue_bytes = 8192U;
    std::size_t telemetry_queue_bytes = 4096U;
};

class EspNowLink final : public Link {
public:
    static constexpr std::size_t kMaxFrameSize = 250U;   // btp::kEspNowMaxFrameSize
    static constexpr std::uint32_t kDeliveryTimeoutMs = 250U;
    static constexpr std::size_t kTelemetryBurst = 8U;   // per poll()/send()

    explicit EspNowLink(const EspNowLinkConfig& config) noexcept : config_(config) {}
    ~EspNowLink() override;

    const char* name() const noexcept override { return "ESP-NOW"; }
    Kind kind() const noexcept override { return Kind::Datagram; }
    esp_err_t start(const LinkEvents& events, ErrorInfo& error) noexcept override;
    void stop() noexcept override;
    bool send(const std::uint8_t* data, std::size_t size, Priority priority) noexcept override;
    bool connected() const noexcept override { return running_.load(); }
    void poll(std::uint32_t now_ms) noexcept override;
    LinkStats stats() const noexcept override;

    struct DeliveryStats {
        std::uint32_t delivered = 0U;
        std::uint32_t failed = 0U;
        std::uint32_t timeouts = 0U;
        std::uint32_t rejected_source = 0U;  ///< datagrams from someone else
        std::uint32_t telemetry_sent = 0U;
    };
    DeliveryStats delivery_stats() const noexcept;

    // Driver callbacks (file-scope trampolines in the .cpp).
    void on_receive(const std::uint8_t* src_mac, const std::uint8_t* data, int size) noexcept;
    void on_sent(bool delivered) noexcept;

private:
    void pump(std::uint32_t now_ms) noexcept;

    EspNowLinkConfig config_;
    LinkEvents events_{};
    LinkMutex mutex_;
    Buffer normal_storage_;
    Buffer telemetry_storage_;
    FrameQueue normal_;      // guarded by mutex_
    FrameQueue telemetry_;   // guarded by mutex_
    std::atomic<bool> running_{false};
    std::atomic<bool> in_flight_{false};
    std::uint32_t in_flight_since_ms_ = 0U;   // pump() only
    std::uint8_t scratch_[kMaxFrameSize] = {};  // pump() only, under mutex_

    std::atomic<std::uint32_t> bytes_in_{0U};
    std::atomic<std::uint32_t> bytes_out_{0U};
    std::atomic<std::uint32_t> send_refused_{0U};
    std::atomic<std::uint32_t> delivered_{0U};
    std::atomic<std::uint32_t> failed_{0U};
    std::atomic<std::uint32_t> timeouts_{0U};
    std::atomic<std::uint32_t> rejected_source_{0U};
    std::atomic<std::uint32_t> telemetry_sent_{0U};
};

}  // namespace btpdev

#endif  // BTPDEV_ESPNOW_LINK_H
