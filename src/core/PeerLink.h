#ifndef BTPDEV_PEER_LINK_H
#define BTPDEV_PEER_LINK_H

// Link 1 of the device's BTP node: the peer link (include/btpdev/peer.h).
//
// It is the NodeLink of that link (send() to the application's transport,
// seal() / open() with the peer key) and owns everything the link needs:
// the reassembly slot, the receive and send queues in front of the node, the
// key and the counters.
//
// Threads:
//   * any task      -> push_rx() / push_tx() / set_password(): copy and wake.
//   * comms task    -> drain(): the only one that touches the node, seals,
//                      opens and calls the transport.

#include <btp/node.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <btpdev/error.h>
#include <btpdev/peer.h>

#include "core/FrameQueue.h"
#include "core/Memory.h"
#include "transport/LinkMutex.h"

namespace btpdev {

class PeerLink final : public btp::NodeLink {
public:
    static constexpr std::uint8_t kLinkIndex = 1U;
    static constexpr std::size_t kKeySize = 16U;   // AES-128-GCM

    using WakeFn = void (*)(void* context);

    explicit PeerLink(const PeerConfig& config) noexcept;
    ~PeerLink() override = default;
    PeerLink(const PeerLink&) = delete;
    PeerLink& operator=(const PeerLink&) = delete;

    /// After the node's begin(): buffers, the saved key, attach as link 1.
    esp_err_t start(btp::Node& node, WakeFn wake, void* wake_context, ErrorInfo& error) noexcept;

    // Any task.
    bool push_rx(const std::uint8_t* data, std::size_t size) noexcept;
    bool push_tx(std::uint16_t channel, const std::uint8_t* data, std::size_t size) noexcept;
    esp_err_t set_password(const char* password) noexcept;
    bool has_key() const noexcept { return key_loaded_.load(); }
    std::size_t max_message() const noexcept { return max_message_; }
    PeerStats stats() const noexcept;
    std::size_t max_datagram() const noexcept { return max_datagram_; }

    // Comms task: opens what arrived, sends what was queued.
    void drain(btp::Node& node, std::uint64_t now_ms) noexcept;

    // ---- btp::NodeLink (the node calls these on the comms task) ----------
    bool send(const std::uint8_t* frame, std::size_t frame_size) override;
    bool has_seal() const noexcept override { return true; }
    bool seal(const btp::Header& header, std::uint16_t payload_size, const std::uint8_t* plaintext,
              std::uint8_t* out) override;
    bool has_open() const noexcept override { return true; }
    bool open(const btp::Header& header, std::uint16_t sealed_size, const std::uint8_t* sealed,
              std::uint8_t* out_plaintext) override;

private:
    static constexpr std::size_t kSlots = 2U;
    static constexpr std::size_t kSlotBytes = 512U;
    static constexpr std::size_t kQueueBytes = 2048U;
    static constexpr std::uint64_t kReassemblyTimeoutMs = 30000U;   // a radio is slow
    static constexpr std::uint32_t kIterations = 4096U;

    static bool seal_fn(void* context, const btp::Header& header, std::uint16_t payload_size,
                        const std::uint8_t* plaintext, std::uint8_t* out) noexcept;
    void apply_pending_key() noexcept;
    void load_saved_key() noexcept;
    void event(PeerEvent e) noexcept;

    PeerConfig config_;
    std::size_t max_datagram_ = 0U;
    std::size_t max_message_ = 0U;
    WakeFn wake_ = nullptr;
    void* wake_context_ = nullptr;

    // Reassembly storage for the node's link 1.
    btp::ReassemblySlot slot_array_[kSlots];
    std::uint8_t slot_bytes_[kSlots][kSlotBytes];
    btp::ReassemblyStorage storage_[kSlots];
    std::uint8_t rx_buffer_[kSlotBytes];
    std::uint8_t open_buffer_[kSlotBytes];
    btp::NodeLinkSlot slot_;

    // The key the comms task seals and opens with, and the one set_password()
    // hands over to it.
    std::uint8_t key_[kKeySize] = {};
    std::uint8_t pending_key_[kKeySize] = {};
    std::atomic<bool> key_loaded_{false};
    std::atomic<int> key_change_{0};   // 0 none, 1 new key in pending_key_, -1 erase
    LinkMutex key_mutex_;

    // Queues in front of the node (entries: rx = datagram; tx = channel u16 LE + data).
    LinkMutex rx_mutex_;
    LinkMutex tx_mutex_;
    Buffer rx_storage_;
    Buffer tx_storage_;
    FrameQueue rx_queue_;
    FrameQueue tx_queue_;
    Buffer chunk_;

    std::atomic<std::uint32_t> sent_{0}, send_refused_{0}, received_{0}, auth_failed_{0}, cleartext_{0},
        malformed_{0}, queue_full_{0};
    std::uint32_t refused_type_ = 0U;   // open() refused a type the peer link does not carry
};

}  // namespace btpdev

#endif  // BTPDEV_PEER_LINK_H
