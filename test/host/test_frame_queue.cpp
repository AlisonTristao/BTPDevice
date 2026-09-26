#include "core/FrameQueue.h"

#include <cstring>
#include <vector>

#include "check.h"

using btpdev::FrameQueue;
using btpdev::Priority;

namespace {

std::vector<std::uint8_t> bytes(std::size_t size, std::uint8_t first) {
    std::vector<std::uint8_t> out(size);
    for (std::size_t i = 0; i < size; ++i) out[i] = static_cast<std::uint8_t>(first + i);
    return out;
}

// Drains the whole front frame through front()/consume(), `chunk` bytes at a
// time, as a socket or BLE notification would.
std::vector<std::uint8_t> drain_front(FrameQueue& queue, std::size_t chunk) {
    std::vector<std::uint8_t> out;
    std::size_t remaining = queue.front_remaining();
    while (remaining > 0U) {
        const std::uint8_t* data = nullptr;
        std::size_t size = 0U;
        if (!queue.front(&data, &size)) break;
        const std::size_t take = size < chunk ? size : chunk;
        out.insert(out.end(), data, data + take);
        queue.consume(take);
        remaining -= take;
    }
    return out;
}

void unbound_queue_refuses_everything() {
    FrameQueue queue;
    const std::uint8_t frame[3] = {1, 2, 3};
    CHECK(!queue.push(frame, sizeof(frame), Priority::Normal));
    CHECK(queue.stats().refused_invalid == 1U);
    const std::uint8_t* data = nullptr;
    std::size_t size = 0U;
    CHECK(!queue.front(&data, &size));
}

void frames_come_out_whole_and_in_order() {
    std::uint8_t storage[64];
    FrameQueue queue;
    queue.bind(storage, sizeof(storage), 0U);
    const auto a = bytes(10, 0x10);
    const auto b = bytes(5, 0x80);
    CHECK(queue.push(a.data(), a.size(), Priority::Normal));
    CHECK(queue.push(b.data(), b.size(), Priority::Telemetry));
    CHECK(queue.frames() == 2U);
    CHECK(queue.used_bytes() == 10U + 5U + 2U * FrameQueue::kEntryOverhead);
    CHECK(drain_front(queue, 1000U) == a);
    CHECK(drain_front(queue, 1000U) == b);
    CHECK(queue.empty());
    CHECK(queue.used_bytes() == 0U);
}

void partial_consume_keeps_the_frame_at_the_head() {
    std::uint8_t storage[64];
    FrameQueue queue;
    queue.bind(storage, sizeof(storage), 0U);
    const auto a = bytes(20, 1);
    const auto b = bytes(4, 100);
    queue.push(a.data(), a.size(), Priority::Normal);
    queue.push(b.data(), b.size(), Priority::Normal);
    CHECK(drain_front(queue, 3U) == a);  // 3 bytes per "write"
    CHECK(queue.frames() == 1U);
    CHECK(drain_front(queue, 3U) == b);
}

void frames_wrap_around_the_end_of_the_storage() {
    std::uint8_t storage[32];
    FrameQueue queue;
    queue.bind(storage, sizeof(storage), 0U);
    const auto filler = bytes(12, 0);
    for (int round = 0; round < 50; ++round) {
        // Two in, one out: the head keeps moving, so frames straddle the end.
        const auto a = bytes(9, static_cast<std::uint8_t>(round));
        CHECK(queue.push(a.data(), a.size(), Priority::Normal));
        CHECK(queue.push(filler.data(), filler.size(), Priority::Normal));
        CHECK(drain_front(queue, 1000U) == a);
        CHECK(drain_front(queue, 1000U) == filler);
        CHECK(queue.empty());
    }
    // And with a frame left behind each round, so wrapping happens mid-queue.
    const auto keep = bytes(7, 0x55);
    CHECK(queue.push(keep.data(), keep.size(), Priority::Normal));
    for (int round = 0; round < 50; ++round) {
        const auto a = bytes(11, static_cast<std::uint8_t>(round * 3));
        CHECK(queue.push(a.data(), a.size(), Priority::Normal));
        const auto first = drain_front(queue, 4U);
        CHECK(first.size() == (round == 0 ? 7U : 11U));
    }
}

void a_full_queue_refuses_any_priority() {
    std::uint8_t storage[20];
    FrameQueue queue;
    queue.bind(storage, sizeof(storage), 0U);
    const auto a = bytes(18, 0);  // 18 + 2 = exactly full
    CHECK(queue.push(a.data(), a.size(), Priority::Normal));
    const std::uint8_t one = 1;
    CHECK(!queue.push(&one, 1U, Priority::Normal));
    CHECK(queue.stats().refused_full == 1U);
}

void telemetry_is_kept_out_of_the_reserve() {
    std::uint8_t storage[100];
    FrameQueue queue;
    queue.bind(storage, sizeof(storage), 25U);  // top 25 bytes: Normal only
    const auto t = bytes(28, 0);                // 30 bytes per entry
    CHECK(queue.push(t.data(), t.size(), Priority::Telemetry));  // 30
    CHECK(queue.push(t.data(), t.size(), Priority::Telemetry));  // 60
    CHECK(!queue.push(t.data(), t.size(), Priority::Telemetry)); // 90 > 75
    CHECK(queue.stats().refused_reserve == 1U);
    // A reply still fits: that is what the reserve is for.
    CHECK(queue.push(t.data(), t.size(), Priority::Normal));     // 90
    CHECK(queue.frames() == 3U);
}

void oversized_and_empty_frames_are_invalid() {
    std::uint8_t storage[16];
    FrameQueue queue;
    queue.bind(storage, sizeof(storage), 0U);
    const auto big = bytes(15, 0);  // 15 + 2 > 16
    CHECK(!queue.push(big.data(), big.size(), Priority::Normal));
    CHECK(queue.stats().refused_full == 1U);
    CHECK(!queue.push(big.data(), 0U, Priority::Normal));
    CHECK(!queue.push(nullptr, 3U, Priority::Normal));
    CHECK(queue.stats().refused_invalid == 2U);
}

void clear_drops_a_half_sent_frame() {
    std::uint8_t storage[64];
    FrameQueue queue;
    queue.bind(storage, sizeof(storage), 0U);
    const auto a = bytes(20, 0);
    queue.push(a.data(), a.size(), Priority::Normal);
    queue.consume(5U);
    queue.clear();
    CHECK(queue.empty());
    CHECK(queue.front_remaining() == 0U);
    const auto b = bytes(3, 9);
    queue.push(b.data(), b.size(), Priority::Normal);
    CHECK(drain_front(queue, 100U) == b);
}

void large_frames_use_the_16_bit_length() {
    std::vector<std::uint8_t> storage(5000);
    FrameQueue queue;
    queue.bind(storage.data(), storage.size(), 0U);
    const auto a = bytes(4115, 7);  // a COBS-framed 4096-octet BTP frame
    CHECK(queue.push(a.data(), a.size(), Priority::Normal));
    CHECK(drain_front(queue, 244U) == a);  // BLE-sized chunks
}

}  // namespace

int main() {
    unbound_queue_refuses_everything();
    frames_come_out_whole_and_in_order();
    partial_consume_keeps_the_frame_at_the_head();
    frames_wrap_around_the_end_of_the_storage();
    a_full_queue_refuses_any_priority();
    telemetry_is_kept_out_of_the_reserve();
    oversized_and_empty_frames_are_invalid();
    clear_drops_a_half_sent_frame();
    large_frames_use_the_16_bit_length();
    return test_result("test_frame_queue");
}
