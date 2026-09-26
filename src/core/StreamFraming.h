#ifndef BTPDEV_STREAM_FRAMING_H
#define BTPDEV_STREAM_FRAMING_H

// Link framing of the three stream links (Serial, TCP, BLE):
// 0x00 || COBS(frame) || 0x00 over a byte pipe with no message boundaries
// (BTP/docs/fragmentation-and-transports.md sections 8.2 and 9). btp::Node
// only takes whole frames, so the device owns this layer, on both sides.
//
// Built on BTP's own codec (btp::cobs_encode, btp::SerialDecoder); this file
// only adds caller-owned storage binding, per-connection reset and counters.
// Portable, covered by test/host/test_stream_framing.cpp.

#include <btp/stream.hpp>

#include <cstddef>
#include <cstdint>
#include <new>

namespace btpdev {

/// Worst-case size of 0x00 || COBS(frame) || 0x00 for a `frame_size`-octet
/// frame; 0 when frame_size is out of range.
std::size_t stream_encoded_capacity(std::size_t frame_size) noexcept;

/// Writes 0x00 || COBS(frame) || 0x00 into `out`. False on a null argument or
/// when `capacity` is too small (see stream_encoded_capacity()).
bool stream_encode(const std::uint8_t* frame, std::size_t frame_size,
                   std::uint8_t* out, std::size_t capacity,
                   std::size_t* written) noexcept;

/// Receive side: raw link bytes in, whole CRC-valid frames out. One instance
/// per link, driven by one task at a time.
class StreamDecoder {
public:
    /// Storage bind() needs: one COBS block plus one decoded frame, at the
    /// serial profile's ceiling (the one TraceView encodes TCP and BLE with,
    /// docs/PLANO.md "Fase 0: resultados").
    static constexpr std::size_t kStorageSize =
        btp::kSerialMaxCobsBlockSize + btp::kSerialMaxFrameSize;

    struct Stats {
        std::uint32_t frames = 0U;
        std::uint32_t cobs_errors = 0U;
        std::uint32_t frame_errors = 0U;   ///< bad header / CRC after COBS
        std::uint32_t overflows = 0U;      ///< block longer than the storage
    };

    StreamDecoder() noexcept = default;
    ~StreamDecoder();
    StreamDecoder(const StreamDecoder&) = delete;
    StreamDecoder& operator=(const StreamDecoder&) = delete;

    /// Uses `storage` (not owned, at least kStorageSize bytes). False when it
    /// is null or too small; feed() is then a no-op.
    bool bind(std::uint8_t* storage, std::size_t size) noexcept;

    /// Drops any partial frame. Call on every new connection: a new peer must
    /// never complete a frame the previous one started.
    void reset() noexcept;

    bool bound() const noexcept { return decoder_ != nullptr; }
    const Stats& stats() const noexcept { return stats_; }

    /// Feeds raw bytes; calls on_frame(const btp::DecodedFrame&) for each
    /// complete, valid frame, in order. The frame view is only valid during
    /// that call. Corrupt blocks are counted and skipped: the decoder
    /// resynchronizes on the next 0x00 by itself.
    template <typename OnFrame>
    void feed(const std::uint8_t* data, std::size_t size, OnFrame&& on_frame) noexcept {
        if (decoder_ == nullptr || data == nullptr) return;
        for (std::size_t i = 0U; i < size; ++i) {
            btp::DecodedFrame frame{};
            const btp::SerialDecodeResult result = decoder_->push(data[i], &frame);
            switch (result.event) {
                case btp::SerialDecodeEvent::Frame:
                    ++stats_.frames;
                    on_frame(static_cast<const btp::DecodedFrame&>(frame));
                    break;
                case btp::SerialDecodeEvent::CobsError: ++stats_.cobs_errors; break;
                case btp::SerialDecodeEvent::FrameError: ++stats_.frame_errors; break;
                case btp::SerialDecodeEvent::Overflow: ++stats_.overflows; break;
                default: break;
            }
        }
    }

private:
    alignas(btp::SerialDecoder) unsigned char decoder_storage_[sizeof(btp::SerialDecoder)] = {};
    btp::SerialDecoder* decoder_ = nullptr;
    Stats stats_{};
};

}  // namespace btpdev

#endif  // BTPDEV_STREAM_FRAMING_H
