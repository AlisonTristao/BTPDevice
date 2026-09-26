#include "core/FrameQueue.h"

#include <cstring>

namespace btpdev {

void FrameQueue::bind(std::uint8_t* storage, std::size_t capacity,
                      std::size_t reserve) noexcept {
    if (storage == nullptr) capacity = 0U;
    storage_ = storage;
    capacity_ = capacity;
    reserve_ = reserve > capacity ? capacity : reserve;
    clear();
}

void FrameQueue::clear() noexcept {
    head_ = 0U;
    used_ = 0U;
    frames_ = 0U;
    front_sent_ = 0U;
}

std::uint8_t FrameQueue::byte_at(std::size_t at) const noexcept {
    return storage_[at % capacity_];
}

void FrameQueue::write_bytes(std::size_t at, const std::uint8_t* data,
                             std::size_t size) noexcept {
    at %= capacity_;
    const std::size_t first = size < capacity_ - at ? size : capacity_ - at;
    std::memcpy(storage_ + at, data, first);
    if (first < size) std::memcpy(storage_, data + first, size - first);
}

bool FrameQueue::push(const std::uint8_t* data, std::size_t size,
                      Priority priority) noexcept {
    if (data == nullptr || size == 0U || size > kMaxFrameSize || capacity_ == 0U) {
        ++stats_.refused_invalid;
        return false;
    }
    const std::size_t needed = size + kEntryOverhead;
    if (needed > capacity_ - used_) {
        ++stats_.refused_full;
        return false;
    }
    if (priority == Priority::Telemetry && used_ + needed > capacity_ - reserve_) {
        ++stats_.refused_reserve;
        return false;
    }

    const std::uint8_t prefix[kEntryOverhead] = {
        static_cast<std::uint8_t>(size & 0xFFU),
        static_cast<std::uint8_t>((size >> 8) & 0xFFU),
    };
    const std::size_t tail = head_ + used_;
    write_bytes(tail, prefix, kEntryOverhead);
    write_bytes(tail + kEntryOverhead, data, size);
    used_ += needed;
    ++frames_;
    ++stats_.pushed;
    return true;
}

std::size_t FrameQueue::front_remaining() const noexcept {
    if (frames_ == 0U) return 0U;
    const std::size_t size = static_cast<std::size_t>(byte_at(head_)) |
                             (static_cast<std::size_t>(byte_at(head_ + 1U)) << 8);
    return size - front_sent_;
}

bool FrameQueue::front(const std::uint8_t** data, std::size_t* size) const noexcept {
    if (frames_ == 0U || data == nullptr || size == nullptr) return false;
    const std::size_t remaining = front_remaining();
    const std::size_t at = (head_ + kEntryOverhead + front_sent_) % capacity_;
    const std::size_t contiguous = capacity_ - at;
    *data = storage_ + at;
    *size = remaining < contiguous ? remaining : contiguous;
    return true;
}

void FrameQueue::consume(std::size_t size) noexcept {
    if (frames_ == 0U || size == 0U) return;
    const std::size_t remaining = front_remaining();
    if (size < remaining) {
        front_sent_ += size;
        return;
    }
    // The whole front frame is out: release its entry.
    const std::size_t entry = kEntryOverhead + front_sent_ + remaining;
    head_ = (head_ + entry) % capacity_;
    used_ -= entry;
    --frames_;
    front_sent_ = 0U;
    if (frames_ == 0U) head_ = 0U;  // keep the next frames contiguous when possible
}

}  // namespace btpdev
