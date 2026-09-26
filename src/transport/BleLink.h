#ifndef BTPDEV_BLE_LINK_H
#define BTPDEV_BLE_LINK_H

// BLE link: a NimBLE peripheral with the BTP GATT service (RX write, TX
// notify; BTP/docs/fragmentation-and-transports.md section 8). Ported from
// bally_OS lib/BleBtpServer, with a FrameQueue instead of a heap copy per
// frame.
//
// TX is a byte stream: each notification is filled from as many queued bytes
// as fit in ATT_MTU - 3, across frame boundaries, and the next one waits for
// BLE_GAP_EVENT_NOTIFY_TX (one notification in flight).
//
// Exclusivity is at the GAP level: advertising stops while a central is
// connected and resumes when it leaves, so a second central cannot even find
// the service. A central is "connected" for send() only once it subscribed to
// TX -- before that, notifications are dropped by the stack.
//
// No pairing/bonding: confidentiality and authenticity are BTP's own AEAD
// (key E), exactly like TCP (no TLS).
//
// Needs CONFIG_BT_NIMBLE_ENABLED; otherwise start() reports
// BTPDEV_ERR_LINK_UNSUPPORTED.

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "core/FrameQueue.h"
#include "core/Memory.h"
#include "transport/Link.h"
#include "transport/LinkMutex.h"

namespace btpdev {

struct BleLinkConfig {
    /// Advertised name (scan response) and GAP device name, at most
    /// kMaxNameLength bytes; longer is cut at a UTF-8 boundary.
    const char* device_name = nullptr;
    std::size_t send_queue_bytes = 8192U;
};

class BleLink final : public LinkDriver {
public:
    static constexpr std::size_t kMaxNameLength = 29U;
    static constexpr std::size_t kMaxChunkSize = 512U;  // >= BLE 5's max ATT_MTU - 3

    explicit BleLink(const BleLinkConfig& config) noexcept;
    ~BleLink() override;

    const char* name() const noexcept override { return "BLE"; }
    Kind kind() const noexcept override { return Kind::Stream; }
    esp_err_t start(const LinkEvents& events, ErrorInfo& error) noexcept override;
    void stop() noexcept override;
    bool send(const std::uint8_t* data, std::size_t size, Priority priority) noexcept override;
    bool connected() const noexcept override;
    void disconnect_peer() noexcept override;
    LinkStats stats() const noexcept override;

    // NimBLE glue (called by the file-scope handlers in the .cpp).
    void on_sync() noexcept;
    int on_gap_event(void* event) noexcept;
    int on_rx_write(std::uint16_t conn_handle, const std::uint8_t* data, std::size_t size) noexcept;
    void drain() noexcept;

private:
    static constexpr std::uint16_t kNoConnection = 0xFFFFU;

    void start_advertising() noexcept;

    BleLinkConfig config_;
    char device_name_[kMaxNameLength + 1U] = {};
    LinkEvents events_{};
    LinkMutex mutex_;
    Buffer queue_storage_;
    FrameQueue queue_;   // guarded by mutex_

    std::atomic<bool> running_{false};
    std::atomic<std::uint16_t> conn_handle_{kNoConnection};
    std::atomic<bool> subscribed_{false};
    std::atomic<bool> notify_in_flight_{false};
    std::atomic<std::uint16_t> att_mtu_{23U};

    std::atomic<std::uint32_t> connections_{0U};
    std::atomic<std::uint32_t> bytes_in_{0U};
    std::atomic<std::uint32_t> bytes_out_{0U};
    std::atomic<std::uint32_t> send_refused_{0U};

    std::uint8_t notify_scratch_[kMaxChunkSize] = {};
};

}  // namespace btpdev

#endif  // BTPDEV_BLE_LINK_H
