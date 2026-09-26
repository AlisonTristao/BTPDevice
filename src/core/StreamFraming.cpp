#include "core/StreamFraming.h"

namespace btpdev {

std::size_t stream_encoded_capacity(std::size_t frame_size) noexcept {
    std::size_t encoded = 0U;
    if (btp::cobs_max_encoded_size(frame_size, &encoded) != btp::CobsError::Ok) {
        return 0U;
    }
    return encoded + 2U;  // leading and trailing 0x00
}

bool stream_encode(const std::uint8_t* frame, std::size_t frame_size,
                   std::uint8_t* out, std::size_t capacity,
                   std::size_t* written) noexcept {
    if (frame == nullptr || frame_size == 0U || out == nullptr || written == nullptr ||
        capacity < 3U) {
        return false;
    }
    std::size_t encoded = 0U;
    if (btp::cobs_encode(frame, frame_size, out + 1U, capacity - 2U, &encoded) !=
        btp::CobsError::Ok) {
        return false;
    }
    // The leading delimiter lets a receiver that joined mid-stream (or saw a
    // corrupt block) resynchronize on this frame instead of the next one.
    out[0] = 0x00U;
    out[encoded + 1U] = 0x00U;
    *written = encoded + 2U;
    return true;
}

StreamDecoder::~StreamDecoder() {
    if (decoder_ != nullptr) decoder_->~SerialDecoder();
}

bool StreamDecoder::bind(std::uint8_t* storage, std::size_t size) noexcept {
    if (decoder_ != nullptr) {
        decoder_->~SerialDecoder();
        decoder_ = nullptr;
    }
    if (storage == nullptr || size < kStorageSize) return false;
    decoder_ = new (decoder_storage_)
        btp::SerialDecoder(storage, btp::kSerialMaxCobsBlockSize,
                           storage + btp::kSerialMaxCobsBlockSize,
                           btp::kSerialMaxFrameSize);
    if (!decoder_->valid()) {
        decoder_->~SerialDecoder();
        decoder_ = nullptr;
        return false;
    }
    return true;
}

void StreamDecoder::reset() noexcept {
    if (decoder_ != nullptr) decoder_->reset();
}

}  // namespace btpdev
