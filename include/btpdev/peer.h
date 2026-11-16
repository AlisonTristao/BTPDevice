#ifndef BTPDEV_PEER_H
#define BTPDEV_PEER_H

// The peer link: a second BTP link, device to device, over a transport the
// application brings (a radio modem, a LoRa module, a cable...).
//
// Link 0 stays TraceView's (Serial, TCP, BLE, ESP-NOW, as configured). The
// same BTP node gets link 1 for the peer:
//
//   * its own reassembly and size limit (the transport's datagram size);
//   * its own key, derived from a password every device of the group shares
//     (`btp -peer_password <text>`, kept in NVS). Every message is sealed
//     with AES-128-GCM; a message without the key's tag -- a different
//     password, a corrupted or forged frame -- is dropped and reported as
//     PeerEvent::AuthFailed. Without a password nothing is sent and
//     everything received is dropped (fail closed);
//   * no session and no telemetry subscriptions: the peer link carries the
//     application's own messages, by channel (BtpDevice::peer_send() /
//     PeerConfig::on_message).
//
// One message travels in one datagram (no fragmentation on the peer link):
// BtpDevice::max_peer_message() is the transport's datagram size minus the
// BTP header, CRC and AEAD tag (kPeerOverhead).
//
// Threads: PeerTransport::send(), on_message and on_event run on the
// library's communication task (keep them short, never block). The
// transport hands received datagrams to BtpDevice::peer_receive() from any
// task.

#include <cstddef>
#include <cstdint>

namespace btpdev {

/// Bytes BTP adds to one peer message: the frame header and CRC (40) and the
/// AES-GCM tag (16).
constexpr std::size_t kPeerOverhead = 56U;

/// The application's datagram transport for the peer link.
class PeerTransport {
public:
    virtual ~PeerTransport() = default;

    /// Largest datagram the transport carries, in bytes (at least
    /// kPeerOverhead + 1).
    virtual std::size_t max_datagram() const noexcept = 0;

    /// One encoded BTP frame for the peer. Called on the communication task:
    /// queue it and return; never wait for the air. False: refused (the
    /// message counts as not sent).
    virtual bool send(const std::uint8_t* frame, std::size_t size) noexcept = 0;
};

/// A message received on the peer link (authenticated and decrypted).
struct PeerMessage {
    std::uint32_t source_id;   ///< the sender's BTP source_id
    std::uint32_t boot_id;     ///< the sender's boot
    std::uint16_t channel;     ///< what the sender passed to peer_send()
    const std::uint8_t* data;  ///< valid during the callback only
    std::size_t size;
};

/// Something the peer link dropped (PeerConfig::on_event).
enum class PeerEvent : std::uint8_t {
    AuthFailed,  ///< sealed with another key (a different password), or altered on the way
    Cleartext,   ///< arrived unencrypted: refused
    Malformed,   ///< not a valid BTP frame
    NoKey,       ///< a message arrived (or one was to be sent) but no password is set
};

const char* peer_event_name(PeerEvent event) noexcept;

struct PeerStats {
    std::uint32_t sent = 0U;          ///< messages handed to the transport
    std::uint32_t send_refused = 0U;  ///< refused by the transport, or no key
    std::uint32_t received = 0U;      ///< messages delivered to on_message
    std::uint32_t auth_failed = 0U;   ///< dropped: wrong key / altered
    std::uint32_t cleartext = 0U;     ///< dropped: not encrypted
    std::uint32_t malformed = 0U;     ///< dropped: not a valid frame
    std::uint32_t queue_full = 0U;    ///< peer_send() / peer_receive() refused: queue full
};

/// DeviceConfig::peer. A null transport: no peer link.
struct PeerConfig {
    PeerTransport* transport = nullptr;
    void (*on_message)(const PeerMessage& message, void* context) = nullptr;
    void (*on_event)(PeerEvent event, void* context) = nullptr;
    void* context = nullptr;
};

}  // namespace btpdev

#endif  // BTPDEV_PEER_H
