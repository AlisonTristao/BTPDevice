#ifndef BTPDEV_FRAME_QUEUE_H
#define BTPDEV_FRAME_QUEUE_H

// The send queue every link uses: a byte ring over caller-owned storage
// (allocated once, in begin() -- CONTRIBUTING.md "Memória"), holding whole
// frames back to back, each behind a 2-octet length prefix.
//
// Replaces bally_OS's per-link queues, which allocated a heap copy of every
// frame (new[] in send(), delete[] once written). Same two behaviors kept:
//
//   * PRIORITY ADMISSION (bally_OS T23): the last `reserve` bytes are for
//     Normal frames only. A Telemetry frame is refused once it would eat into
//     them, so a burst of telemetry can never keep a COMMAND_RESULT or a
//     TERMINAL_OUT from being queued right after it. Telemetry is never
//     retried by its producer, so the reserve is never eaten by a retry storm.
//   * STREAM DRAIN: a link reads the front of the queue as contiguous chunks
//     (front() / consume()), so a socket write or a BLE notification can take
//     part of a frame and come back for the rest, and a BLE notification can
//     pack the tail of one frame with the head of the next. A frame is never
//     interleaved with another one.
//
// Not thread-safe: a link guards it with its own mutex. Portable (no ESP-IDF),
// covered by test/host/test_frame_queue.cpp.

#include <cstddef>
#include <cstdint>

namespace btpdev {

enum class Priority : std::uint8_t {
    Normal = 0,     ///< replies, terminal, session/catalog traffic
    Telemetry = 1,  ///< TELEMETRY and LOG: first to be refused
};

class FrameQueue {
public:
    /// Largest frame one entry can hold (the length prefix is 16-bit).
    static constexpr std::size_t kMaxFrameSize = 0xFFFFU;
    /// Bytes each entry costs on top of its payload.
    static constexpr std::size_t kEntryOverhead = 2U;

    FrameQueue() noexcept = default;
    FrameQueue(const FrameQueue&) = delete;
    FrameQueue& operator=(const FrameQueue&) = delete;

    /// Points the queue at `storage` (not owned) and empties it. `reserve` is
    /// how many bytes at the top only Normal frames may use; it is clamped to
    /// `capacity`. A null storage or zero capacity leaves the queue unusable
    /// (every push() fails).
    void bind(std::uint8_t* storage, std::size_t capacity, std::size_t reserve) noexcept;

    /// Copies one whole frame in, or nothing. False: empty or oversized
    /// frame, not bound, no room, or refused by the priority admission.
    bool push(const std::uint8_t* data, std::size_t size, Priority priority) noexcept;

    /// The next contiguous bytes of the front frame that are still to be
    /// sent -- possibly only part of it, when the frame wraps around the end
    /// of the storage. False when the queue is empty.
    bool front(const std::uint8_t** data, std::size_t* size) const noexcept;

    /// Marks `size` bytes of the front frame as sent (at most what front()
    /// returned). The frame is removed once all of it has been consumed.
    void consume(std::size_t size) noexcept;

    /// Datagram links: copies the whole front frame (even when it wraps around
    /// the end of the storage) into `out`, without removing it. Returns its
    /// size, or 0 when the queue is empty, a partial consume() already
    /// happened, or `capacity` is too small.
    std::size_t copy_front(std::uint8_t* out, std::size_t capacity) const noexcept;

    /// Removes the whole front frame, however much of it was consumed.
    void pop_front() noexcept;

    /// Drops everything (a connection went away: nothing queued for the old
    /// peer may reach the next one).
    void clear() noexcept;

    bool empty() const noexcept { return frames_ == 0U; }
    std::size_t frames() const noexcept { return frames_; }
    std::size_t used_bytes() const noexcept { return used_; }
    std::size_t capacity() const noexcept { return capacity_; }

    /// Bytes still to send of the front frame (0 when empty).
    std::size_t front_remaining() const noexcept;

    /// Frames refused so far, per reason (never reset by clear()).
    struct Stats {
        std::uint32_t pushed = 0U;
        std::uint32_t refused_full = 0U;       ///< any priority, no room at all
        std::uint32_t refused_reserve = 0U;    ///< Telemetry kept out of the reserve
        std::uint32_t refused_invalid = 0U;    ///< empty, oversized or unbound
    };
    const Stats& stats() const noexcept { return stats_; }

private:
    void write_bytes(std::size_t at, const std::uint8_t* data, std::size_t size) noexcept;
    std::uint8_t byte_at(std::size_t at) const noexcept;

    std::uint8_t* storage_ = nullptr;
    std::size_t capacity_ = 0U;
    std::size_t reserve_ = 0U;
    std::size_t head_ = 0U;          // start of the front entry (its length prefix)
    std::size_t used_ = 0U;          // bytes in use, prefixes included
    std::size_t frames_ = 0U;
    std::size_t front_sent_ = 0U;    // payload bytes of the front frame already consumed
    Stats stats_{};
};

}  // namespace btpdev

#endif  // BTPDEV_FRAME_QUEUE_H
