#ifndef BTPDEV_TOPIC_REGISTRY_H
#define BTPDEV_TOPIC_REGISTRY_H

// Turns the application's topic declarations (btpdev::Topics) into the served
// catalogue and the node's publish registry, and publishes what the node's
// own registry cannot: text topics and on-demand (event) publishes.
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
#include <cstdint>

#include <btpdev/error.h>
#include <btpdev/topics.h>

namespace btpdev {

class TopicRegistry {
public:
    /// Largest text sample: 2-octet schema_version + the text.
    static constexpr std::size_t kMaxTextPayload = Text::kCapacity + 2U;
    /// Topics that can be flagged by request_publish() (bit mask).
    static constexpr std::size_t kMaxEventTopics = 32U;

    /// Takes the declarations over (`topics` is left empty), validates them
    /// and adds them to `catalog` / the node's publish registry.
    /// `max_topics` is the memory preset's catalogue capacity.
    esp_err_t install(Topics& topics, btp::Node& node, btp::Catalog& catalog,
                      std::size_t max_topics, ErrorInfo& error) noexcept;

    /// Text topics that are due for their subscribers, and the topics asked
    /// for by request_publish() (only if someone is subscribed). Every pass.
    void publish_pending(btp::Node& node, std::uint64_t now_ms) noexcept;

    /// Any task: publish this topic on the next pass (an event), if anybody is
    /// subscribed. False: no topic by that name.
    bool request_publish(const char* name) noexcept;

    std::size_t size() const noexcept { return topics_.topics_.size(); }
    /// Topic id of a declared topic, 0 when unknown.
    std::uint16_t id_of(const char* name) const noexcept;

private:
    static void fill_fields(void* context, btp::NamedSampleWriter& writer);
    bool publish_one(btp::Node& node, std::size_t index, std::uint64_t now_ms) noexcept;

    Topics topics_;
    std::atomic<std::uint32_t> requested_{0U};
    Text text_;
    std::uint8_t text_payload_[kMaxTextPayload] = {};
};

}  // namespace btpdev

#endif  // BTPDEV_TOPIC_REGISTRY_H
