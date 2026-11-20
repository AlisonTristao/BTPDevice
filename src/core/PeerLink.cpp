#include "core/PeerLink.h"

#include <cstring>

#include <esp_log.h>
#include <esp_timer.h>
#include <nvs.h>

#include <btp/aead.hpp>

#include "core/LogTags.h"
#include "core/RuntimeErrors.h"
#include "security/KeyStore.h"

namespace tracekit {

namespace {

constexpr const char* kTag = "tracekit.peer";
constexpr const char* kNamespace = "btpdev";  // the old name, kept: devices in the field have data there
constexpr const char* kNvsKey = "peer_key";
constexpr const char* kSalt = "btpdev-peer-v1";  // the old name, kept: changing it unpairs every peer
constexpr std::size_t kChannelBytes = 2U;

btp::ReassemblyStorage* wire(btp::ReassemblyStorage* s, std::uint8_t (*bytes)[512], std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        s[i].data = bytes[i];
        s[i].capacity = 512U;
    }
    return s;
}

void wipe(std::uint8_t* p, std::size_t n) noexcept {
    volatile std::uint8_t* v = p;
    for (std::size_t i = 0; i < n; ++i) v[i] = 0U;
}

}  // namespace

const char* peer_event_name(PeerEvent event) noexcept {
    switch (event) {
        case PeerEvent::AuthFailed: return "auth failed";
        case PeerEvent::Cleartext: return "cleartext refused";
        case PeerEvent::Malformed: return "malformed";
        case PeerEvent::NoKey: return "no peer password";
    }
    return "?";
}

PeerLink::PeerLink(const PeerConfig& config) noexcept
    : config_(config),
      slot_array_(),
      slot_bytes_(),
      storage_(),
      rx_buffer_(),
      open_buffer_(),
      slot_(slot_array_, wire(storage_, slot_bytes_, kSlots), kSlots, kReassemblyTimeoutMs, btp::TransportLimits{},
            rx_buffer_, kSlotBytes, open_buffer_, kSlotBytes) {
    static_assert(kSlotBytes == 512U, "wire() assumes 512-byte slots");
}

esp_err_t PeerLink::start(btp::Node& node, WakeFn wake, void* wake_context, ErrorInfo& error) noexcept {
    wake_ = wake;
    wake_context_ = wake_context;
    max_datagram_ = config_.transport->max_datagram();
    if (max_datagram_ <= kPeerOverhead) {
        return fail(error, ESP_ERR_INVALID_ARG, kTag, "peer transport datagram (%u bytes) too small for BTP (> %u)",
                    static_cast<unsigned>(max_datagram_), static_cast<unsigned>(kPeerOverhead));
    }
    if (max_datagram_ > btp::kSerialMaxFrameSize) max_datagram_ = btp::kSerialMaxFrameSize;
    max_message_ = max_datagram_ - kPeerOverhead;
    if (max_message_ > kSlotBytes - btp::kEndpointAeadTagSize) max_message_ = kSlotBytes - btp::kEndpointAeadTagSize;
    transport = btp::TransportLimits{max_datagram_, true};

    if (!rx_mutex_.create() || !tx_mutex_.create() || !key_mutex_.create() || !rx_storage_.allocate(kQueueBytes) ||
        !tx_storage_.allocate(kQueueBytes) || !chunk_.allocate(max_datagram_ + kChannelBytes + 8U)) {
        return fail(error, ESP_ERR_NO_MEM, kTag, "no memory for the peer link queues");
    }
    rx_queue_.bind(rx_storage_.data(), rx_storage_.size(), 0U);
    tx_queue_.bind(tx_storage_.data(), tx_storage_.size(), 0U);
    load_saved_key();
    if (!node.attach_link(kLinkIndex, *this, slot_, kReassemblyTimeoutMs, nullptr)) {
        return fail(error, ESP_FAIL, kTag, "btp::Node::attach_link(1) failed (transport limits)");
    }
    ESP_LOGI(kTag, "peer link up: datagram %u bytes, message up to %u bytes, key %s",
             static_cast<unsigned>(max_datagram_), static_cast<unsigned>(max_message_),
             key_loaded_.load() ? "loaded" : "NOT SET (btp -peer_password <text>)");
    return ESP_OK;
}

// ---- any task ---------------------------------------------------------------

bool PeerLink::push_rx(const std::uint8_t* data, std::size_t size) noexcept {
    if (data == nullptr || size == 0U || size > max_datagram_) return false;
    bool queued = false;
    {
        LinkLock lock(rx_mutex_, ms_to_ticks(5));
        if (lock.locked()) queued = rx_queue_.push(data, size, Priority::Normal);
    }
    if (!queued) queue_full_.fetch_add(1U);
    if (wake_ != nullptr) wake_(wake_context_);
    return queued;
}

bool PeerLink::push_tx(std::uint16_t channel, const std::uint8_t* data, std::size_t size) noexcept {
    if (data == nullptr || size == 0U || size > max_message_) return false;
    if (!key_loaded_.load() && key_change_.load() != 1) {
        send_refused_.fetch_add(1U);
        return false;
    }
    const std::uint8_t head[kChannelBytes] = {static_cast<std::uint8_t>(channel & 0xFFU),
                                              static_cast<std::uint8_t>(channel >> 8)};
    bool queued = false;
    {
        LinkLock lock(tx_mutex_, ms_to_ticks(5));
        if (lock.locked()) queued = tx_queue_.push(head, sizeof head, data, size, Priority::Normal);
    }
    if (!queued) queue_full_.fetch_add(1U);
    if (wake_ != nullptr) wake_(wake_context_);
    return queued;
}

esp_err_t PeerLink::set_password(const char* password) noexcept {
    const bool erase = password == nullptr || password[0] == '\0';
    std::uint8_t key[kKeySize] = {};
    if (!erase) derive_password_key(password, std::strlen(password), kSalt, kIterations, key, sizeof key);

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = erase ? nvs_erase_key(handle, kNvsKey) : nvs_set_blob(handle, kNvsKey, key, sizeof key);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    {
        LinkLock lock(key_mutex_);
        std::memcpy(pending_key_, key, sizeof key);
        key_change_.store(erase ? -1 : 1);
    }
    wipe(key, sizeof key);
    if (wake_ != nullptr) wake_(wake_context_);
    return err;
}

PeerStats PeerLink::stats() const noexcept {
    PeerStats s;
    s.sent = sent_.load();
    s.send_refused = send_refused_.load();
    s.received = received_.load();
    s.auth_failed = auth_failed_.load();
    s.cleartext = cleartext_.load();
    s.malformed = malformed_.load();
    s.queue_full = queue_full_.load();
    return s;
}

void PeerLink::load_saved_key() noexcept {
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) return;
    std::size_t size = sizeof key_;
    if (nvs_get_blob(handle, kNvsKey, key_, &size) == ESP_OK && size == sizeof key_) key_loaded_.store(true);
    nvs_close(handle);
}

// ---- comms task -------------------------------------------------------------

void PeerLink::apply_pending_key() noexcept {
    const int change = key_change_.load();
    if (change == 0) return;
    LinkLock lock(key_mutex_);
    if (key_change_.load() == 1) {
        std::memcpy(key_, pending_key_, sizeof key_);
        key_loaded_.store(true);
    } else {
        wipe(key_, sizeof key_);
        key_loaded_.store(false);
    }
    wipe(pending_key_, sizeof pending_key_);
    key_change_.store(0);
}

void PeerLink::event(PeerEvent e) noexcept {
    if (config_.on_event != nullptr) config_.on_event(e, config_.context);
}

void PeerLink::drain(btp::Node& node, std::uint64_t now_ms) noexcept {
    apply_pending_key();

    // Received datagrams: open them on link 1.
    for (int budget = 0; budget < 8; ++budget) {
        std::size_t size = 0U;
        {
            LinkLock lock(rx_mutex_, ms_to_ticks(5));
            if (!lock.locked() || rx_queue_.empty()) break;
            size = rx_queue_.copy_front(chunk_.data(), chunk_.size());
            rx_queue_.pop_front();
        }
        if (size == 0U) continue;
        if (!key_loaded_.load()) {
            event(PeerEvent::NoKey);
            continue;
        }
        const btp::Node::Stats before = node.link_stats(kLinkIndex);
        const std::uint32_t refused_before = refused_type_;
        btp::ReceivedMessage message{};
        const btp::NodeRx rx = node.receive_on(kLinkIndex, chunk_.data(), size, now_ms, &message);
        if (rx == btp::NodeRx::Complete && message.header.type == btp::MessageType::Telemetry) {
            received_.fetch_add(1U);
            if (config_.on_message != nullptr) {
                PeerMessage m{};
                m.source_id = message.header.source_id;
                m.boot_id = message.header.boot_id;
                m.channel = message.header.object_id;
                m.data = message.payload.data;
                m.size = message.payload.size;
                config_.on_message(m, config_.context);
            }
            continue;
        }
        if (rx != btp::NodeRx::DroppedFrame) continue;   // a fragment, or something the node handled
        const btp::Node::Stats after = node.link_stats(kLinkIndex);
        const std::uint32_t open_failed = after.dropped_open_failed - before.dropped_open_failed;
        const std::uint32_t refused = refused_type_ - refused_before;
        if (open_failed > refused) {
            auth_failed_.fetch_add(1U);
            event(PeerEvent::AuthFailed);
        } else if (after.dropped_cleartext != before.dropped_cleartext) {
            cleartext_.fetch_add(1U);
            event(PeerEvent::Cleartext);
        } else if (refused == 0U) {
            malformed_.fetch_add(1U);
            event(PeerEvent::Malformed);
        }
    }

    // Queued messages: seal and hand them to the transport.
    for (int budget = 0; budget < 4; ++budget) {
        std::size_t size = 0U;
        {
            LinkLock lock(tx_mutex_, ms_to_ticks(5));
            if (!lock.locked() || tx_queue_.empty()) break;
            size = tx_queue_.copy_front(chunk_.data(), chunk_.size());
            tx_queue_.pop_front();
        }
        if (size <= kChannelBytes) continue;
        if (!key_loaded_.load()) {
            send_refused_.fetch_add(1U);
            event(PeerEvent::NoKey);
            continue;
        }
        const std::uint16_t channel = static_cast<std::uint16_t>(chunk_.data()[0] | (chunk_.data()[1] << 8));
        const bool ok = node.send_on(node.link_ref(kLinkIndex), btp::MessageType::Telemetry, channel,
                                     chunk_.data() + kChannelBytes, size - kChannelBytes,
                                     static_cast<std::uint64_t>(esp_timer_get_time()), &PeerLink::seal_fn, this);
        if (ok) sent_.fetch_add(1U);
        else send_refused_.fetch_add(1U);
    }
}

bool PeerLink::send(const std::uint8_t* frame, std::size_t frame_size) {
    return config_.transport != nullptr && config_.transport->send(frame, frame_size);
}

bool PeerLink::seal(const btp::Header& header, std::uint16_t payload_size, const std::uint8_t* plaintext,
                    std::uint8_t* out) {
    if (!key_loaded_.load()) return false;   // fail closed: never a cleartext fallback
    const btp::AeadKey key{key_, kKeySize};
    return btp::aead_seal(key, header, payload_size, plaintext, out) == btp::AeadError::Ok;
}

bool PeerLink::seal_fn(void* context, const btp::Header& header, std::uint16_t payload_size,
                       const std::uint8_t* plaintext, std::uint8_t* out) noexcept {
    return static_cast<PeerLink*>(context)->seal(header, payload_size, plaintext, out);
}

bool PeerLink::open(const btp::Header& header, std::uint16_t sealed_size, const std::uint8_t* sealed,
                    std::uint8_t* out_plaintext) {
    // The peer link carries application messages only: a command, terminal
    // line or control message from a peer is never run, whatever its key.
    if (header.type != btp::MessageType::Telemetry) {
        ++refused_type_;
        return false;
    }
    if (!key_loaded_.load()) return false;
    const btp::AeadKey key{key_, kKeySize};
    return btp::aead_open(key, header, sealed_size, sealed, out_plaintext) == btp::AeadError::Ok;
}

}  // namespace tracekit
