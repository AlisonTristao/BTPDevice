#include "core/StreamFraming.h"

#include <btp/codec.hpp>

#include <vector>

#include "check.h"

namespace {

// A valid BTP frame (header + payload + CRC) to push through the link layer.
std::vector<std::uint8_t> make_frame(std::uint32_t sequence, std::size_t payload_size) {
    std::vector<std::uint8_t> payload(payload_size);
    for (std::size_t i = 0; i < payload_size; ++i) {
        payload[i] = static_cast<std::uint8_t>(i);  // includes 0x00 bytes on purpose
    }
    btp::Header header{};
    header.type = btp::MessageType::Telemetry;
    header.source_id = 0x11223344U;
    header.boot_id = 1U;
    header.sequence = sequence;
    header.timestamp_us = 1000U;
    header.object_id = 0x0101U;
    header.fragment_count = 1U;

    btp::Frame input{};
    input.header = header;
    input.payload = btp::ByteView{payload.data(), payload.size()};
    std::vector<std::uint8_t> frame(btp::kSerialMaxFrameSize);
    std::size_t written = 0U;
    const btp::Error error = btp::encode(input, btp::kSerialTransport, frame.data(),
                                         frame.size(), &written);
    CHECK(error == btp::Error::Ok);
    frame.resize(written);
    return frame;
}

std::vector<std::uint8_t> framed(const std::vector<std::uint8_t>& frame) {
    std::vector<std::uint8_t> out(btpdev::stream_encoded_capacity(frame.size()));
    std::size_t written = 0U;
    CHECK(btpdev::stream_encode(frame.data(), frame.size(), out.data(), out.size(), &written));
    out.resize(written);
    return out;
}

struct Collector {
    std::vector<std::uint32_t> sequences;
    void operator()(const btp::DecodedFrame& frame) { sequences.push_back(frame.header.sequence); }
};

void encoded_frames_are_delimited_and_zero_free_inside() {
    const auto wire = framed(make_frame(1U, 40U));
    CHECK(wire.front() == 0x00U);
    CHECK(wire.back() == 0x00U);
    for (std::size_t i = 1; i + 1 < wire.size(); ++i) CHECK(wire[i] != 0x00U);
}

void encode_refuses_a_small_buffer() {
    const auto frame = make_frame(1U, 40U);
    std::vector<std::uint8_t> out(10);
    std::size_t written = 0U;
    CHECK(!btpdev::stream_encode(frame.data(), frame.size(), out.data(), out.size(), &written));
    CHECK(!btpdev::stream_encode(nullptr, 5U, out.data(), out.size(), &written));
}

void unbound_decoder_is_inert() {
    btpdev::StreamDecoder decoder;
    CHECK(!decoder.bound());
    std::vector<std::uint8_t> too_small(100);
    CHECK(!decoder.bind(too_small.data(), too_small.size()));
    const auto wire = framed(make_frame(1U, 8U));
    Collector got;
    decoder.feed(wire.data(), wire.size(), got);
    CHECK(got.sequences.empty());
}

void frames_survive_arbitrary_chunking() {
    std::vector<std::uint8_t> storage(btpdev::StreamDecoder::kStorageSize);
    btpdev::StreamDecoder decoder;
    CHECK(decoder.bind(storage.data(), storage.size()));

    std::vector<std::uint8_t> stream;
    for (std::uint32_t seq = 1; seq <= 5; ++seq) {
        const auto wire = framed(make_frame(seq, 30U * seq));
        stream.insert(stream.end(), wire.begin(), wire.end());
    }
    // 1, 7 and 64 bytes at a time: TCP segments, BLE writes, USB packets.
    for (std::size_t chunk : {std::size_t{1}, std::size_t{7}, std::size_t{64}}) {
        decoder.reset();
        Collector got;
        for (std::size_t at = 0; at < stream.size(); at += chunk) {
            const std::size_t n = at + chunk <= stream.size() ? chunk : stream.size() - at;
            decoder.feed(stream.data() + at, n, got);
        }
        CHECK((got.sequences == std::vector<std::uint32_t>{1, 2, 3, 4, 5}));
    }
}

void a_corrupt_frame_is_skipped_and_the_next_one_arrives() {
    std::vector<std::uint8_t> storage(btpdev::StreamDecoder::kStorageSize);
    btpdev::StreamDecoder decoder;
    decoder.bind(storage.data(), storage.size());

    auto bad = framed(make_frame(1U, 20U));
    bad[10] ^= 0x5AU;  // breaks the CRC (or the COBS block)
    const auto good = framed(make_frame(2U, 20U));
    std::vector<std::uint8_t> stream(bad);
    stream.insert(stream.end(), good.begin(), good.end());

    Collector got;
    decoder.feed(stream.data(), stream.size(), got);
    CHECK((got.sequences == std::vector<std::uint32_t>{2}));
    CHECK(decoder.stats().frames == 1U);
    CHECK(decoder.stats().cobs_errors + decoder.stats().frame_errors >= 1U);
}

void reset_discards_a_frame_left_half_way() {
    std::vector<std::uint8_t> storage(btpdev::StreamDecoder::kStorageSize);
    btpdev::StreamDecoder decoder;
    decoder.bind(storage.data(), storage.size());

    const auto first = framed(make_frame(1U, 50U));
    Collector got;
    // Previous connection: sends only half a frame, then goes away.
    decoder.feed(first.data(), first.size() / 2, got);
    decoder.reset();
    // New connection: its first frame must decode on its own.
    const auto second = framed(make_frame(2U, 50U));
    decoder.feed(second.data(), second.size(), got);
    CHECK((got.sequences == std::vector<std::uint32_t>{2}));
}

void the_largest_serial_frame_fits() {
    std::vector<std::uint8_t> storage(btpdev::StreamDecoder::kStorageSize);
    btpdev::StreamDecoder decoder;
    decoder.bind(storage.data(), storage.size());
    const auto frame = make_frame(9U, btp::kSerialMaxFrameSize - 40U);
    const auto wire = framed(frame);
    Collector got;
    decoder.feed(wire.data(), wire.size(), got);
    CHECK((got.sequences == std::vector<std::uint32_t>{9}));
}

}  // namespace

int main() {
    encoded_frames_are_delimited_and_zero_free_inside();
    encode_refuses_a_small_buffer();
    unbound_decoder_is_inert();
    frames_survive_arbitrary_chunking();
    a_corrupt_frame_is_skipped_and_the_next_one_arrives();
    reset_discards_a_frame_left_half_way();
    the_largest_serial_frame_fits();
    return test_result("test_stream_framing");
}
