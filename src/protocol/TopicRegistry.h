#ifndef BTPDEV_TOPIC_REGISTRY_H
#define BTPDEV_TOPIC_REGISTRY_H

// Turns the application's topic declarations (btpdev::Topics) into the served
// catalogue and the node's publish registry, and publishes what the node's
// own registry cannot: text topics, on-demand (event) publishes and streams.
//
// A stream (Topics::stream()) is a single-producer / single-consumer ring of
// fixed slots allocated by install(): write_stream() fills the next free slot
// with the block already encoded as its PACKED_LE payload, and
// publish_pending() sends slots in order, leaving the front one in place when
// the link refuses it (a full send queue) so it goes out on a later pass.
//
// install() runs once in begin(), before the manifest is served, and checks
// every rule a declaration can break -- the error names the topic. After
// that the registry is only used from the communication context.
//
// Portable (btp::Node and btp::Catalog are host code too), covered by
// test/host/test_topics.cpp, which decodes the published samples.

#include <btp/node.hpp>

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <memory>
#include <vector>

#include <btpdev/error.h>
#include <btpdev/topics.h>

namespace btpdev {

class TopicRegistry {
public:
    /// Largest text sample: 2-octet schema_version + the text.
    static constexpr std::size_t kMaxTextPayload = Text::kCapacity + 2U;
    /// Topics that can be flagged by request_publish() (bit mask).
    static constexpr std::size_t kMaxEventTopics = 32U;
    /// A stream sample's payload without its samples: schema_version (u16),
    /// seq (u32), rate (u32), sample count (u16).
    static constexpr std::size_t kStreamHead = 12U;
    /// Largest logical payload a stream block may use by default: one Serial
    /// frame (btp::kSerialTransport, 4096 - 40).
    static constexpr std::size_t kDefaultStreamPayload = 4056U;

    struct StreamStats {
        const char* name;
        std::uint32_t sent;     ///< blocks handed to the link
        std::uint32_t dropped;  ///< write_stream() refused: queue full
        std::uint32_t queued;   ///< waiting now
    };

    /// Takes the declarations over (`topics` is left empty), validates them
    /// and adds them to `catalog` / the node's publish registry.
    /// `max_topics` is the memory preset's catalogue capacity;
    /// `max_stream_payload` the largest block a stream may send (one frame of
    /// the link, and no more than the peer accepts).
    esp_err_t install(Topics& topics, btp::Node& node, btp::Catalog& catalog,
                      std::size_t max_topics, ErrorInfo& error,
                      std::size_t max_stream_payload = kDefaultStreamPayload) noexcept;

    /// Text topics that are due for their subscribers, and the topics asked
    /// for by request_publish() (only if someone is subscribed). Every pass.
    void publish_pending(btp::Node& node, std::uint64_t now_ms) noexcept;

    /// Any task: publish this topic on the next pass (an event), if anybody is
    /// subscribed. False: no topic by that name.
    bool request_publish(const char* name) noexcept;

    /// Any task: whether the topic had a subscriber at the last pass. False
    /// for an unknown name (or past the first kMaxEventTopics).
    bool subscribed(const char* name) const noexcept;

    /// ONE producer task per stream: queues a block of samples for the
    /// stream topic `name`. False, and nothing queued, when nobody is
    /// subscribed, the queue is full (counted as dropped), the name is not a
    /// stream or `count` is 0 or over its max_samples. The block's seq
    /// advances on every call, so the receiver sees any gap.
    bool write_stream(const char* name, const std::int16_t* samples, std::size_t count,
                      std::uint32_t rate_hz, std::uint64_t timestamp_us) noexcept;

    std::size_t stream_count() const noexcept { return streams_.size(); }
    /// Any task: counters of the i-th declared stream.
    StreamStats stream_stats(std::size_t i) const noexcept;

    std::size_t size() const noexcept { return topics_.topics_.size(); }
    /// Topic id of a declared topic, 0 when unknown.
    std::uint16_t id_of(const char* name) const noexcept;

private:
    // The slot storage comes from std::malloc: with exceptions off (ESP-IDF's
    // default) a failed new (std::nothrow) aborts instead of returning null,
    // and a stream's slots are the one large allocation begin() makes.
    struct FreeDeleter {
        void operator()(std::uint8_t* p) const noexcept { std::free(p); }
    };
    struct Stream {
        std::size_t topic_index = 0U;
        std::uint16_t topic_id = 0U;
        std::uint16_t max_samples = 0U;
        std::size_t slot_bytes = 0U;  // kSlotHead + kStreamHead + 2 * max_samples
        std::size_t slots = 0U;
        std::unique_ptr<std::uint8_t, FreeDeleter> storage;
        std::atomic<std::uint32_t> head{0U};  // next slot to fill (producer), free-running
        std::atomic<std::uint32_t> tail{0U};  // next slot to send (comms task)
        std::atomic<std::uint32_t> seq{0U};
        std::atomic<std::uint32_t> sent{0U};
        std::atomic<std::uint32_t> dropped{0U};
    };
    /// In front of each slot's payload: timestamp_us (u64) and payload size (u16).
    static constexpr std::size_t kSlotHead = 10U;

    static void fill_fields(void* context, btp::NamedSampleWriter& writer);
    bool publish_one(btp::Node& node, std::size_t index, std::uint64_t now_ms) noexcept;
    esp_err_t add_stream(const TopicDecl& topic, std::size_t index, btp::Catalog& catalog,
                         std::size_t max_stream_payload, ErrorInfo& error) noexcept;
    void send_streams(btp::Node& node, btp::SubscriptionTable& subscriptions) noexcept;

    std::vector<std::unique_ptr<Stream>> streams_;
    Topics topics_;
    std::atomic<std::uint32_t> requested_{0U};
    std::atomic<std::uint32_t> subscribed_{0U};   // written by publish_pending()
    int index_of(const char* name) const noexcept;
    Text text_;
    std::uint8_t text_payload_[kMaxTextPayload] = {};
};

}  // namespace btpdev

#endif  // BTPDEV_TOPIC_REGISTRY_H
