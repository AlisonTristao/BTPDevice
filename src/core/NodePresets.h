#ifndef BTPDEV_NODE_PRESETS_H
#define BTPDEV_NODE_PRESETS_H

// The btp::StaticNode behind each Memory preset (docs/PLANO.md 4.11, D18).
// Close to BTP's own NodeSize Low/Medium/High, with three changes a device
// talking to TraceView needs:
//
//   * CatalogSourceInfo > 0: the manifest's source_info block carries the
//     device name TraceView shows on the card. BTP's SizedNode leaves it at 0.
//   * ScratchBytes (the MANIFEST_DATA scratch) sized for the whole catalogue:
//     bally_OS needed 1900 bytes for 5 topics / 23 fields; BTP's Medium has 512.
//   * CommandBytes fits a 512-byte shell line (+20-byte request prefix) and
//     its COMMAND_RESULT (at most kMaxResultMessage of output) in the dedup
//     cache.
//
// Sizes (sizeof on the host, 64-bit; less on the ESP32) are checked by
// test/host/test_node_presets.cpp and recorded in docs/PLANO.md.

#include <btp/node.hpp>

#include <cstddef>

#include <btpdev/config.h>

namespace btpdev {

/// Longest shell output returned in a COMMAND_RESULT message (the terminal
/// shows the full output; a remote COMMAND caller gets this much).
constexpr std::size_t kMaxResultMessage = 160U;

template <Memory M>
struct NodePreset;

// One preset = the StaticNode dimensions plus the two byte queues of the
// library (the link's send queue and the receive queue in front of the node).
// The HELLO advertises Slots / SlotBytes / MaxSubscriptions / MaxCommands, so
// they are spelled out, not hidden in the template arguments.
#define BTPDEV_DEFINE_PRESET(MEM, SLOTS, SLOT_BYTES, SEAL, SCRATCH, TOPICS, FIELDS, STRINGS, SUBS,                              COMMANDS, COMMAND_BYTES, SOURCE_INFO, LINK_QUEUE, RX_QUEUE)              template <>                                                                                      struct NodePreset<MEM> {                                                                             static constexpr std::size_t kSlots = SLOTS;                                                     static constexpr std::size_t kSlotBytes = SLOT_BYTES;                                            static constexpr std::size_t kMaxSubscriptions = SUBS;                                           static constexpr std::size_t kMaxCommands = COMMANDS;                                            static constexpr std::size_t kCatalogTopics = TOPICS;                                            static constexpr std::size_t kCatalogFields = FIELDS;                                            static constexpr std::size_t kSourceInfo = SOURCE_INFO;                                          static constexpr std::size_t kLinkQueueBytes = LINK_QUEUE;                                       static constexpr std::size_t kRxQueueBytes = RX_QUEUE;                                           using Node = btp::StaticNode<SLOTS, SLOT_BYTES, SEAL, SCRATCH, TOPICS, FIELDS, STRINGS,                                       SUBS, COMMANDS, COMMAND_BYTES, SOURCE_INFO>;                    }

// SlotBytes >= 600 everywhere: a COMMAND_REQUEST with a full 512-byte shell
// line is 532 bytes of logical payload.
//                   memory          slots slot  seal  scratch topics fields strings subs cmds cmdB srcinfo linkQ  rxQ
BTPDEV_DEFINE_PRESET(Memory::Small,  2U,   600U, 1024U, 1024U, 4U,    16U,   512U,   4U,  2U,  768U,  8U,  8192U, 4096U);
BTPDEV_DEFINE_PRESET(Memory::Medium, 4U,   600U, 2048U, 2048U, 8U,    48U,   1536U,  8U,  4U,  768U, 12U, 16384U, 8192U);
BTPDEV_DEFINE_PRESET(Memory::Large,  6U,  1024U, 4096U, 4096U, 16U,   128U,  3072U, 16U,  8U, 1024U, 16U, 32768U, 16384U);

#undef BTPDEV_DEFINE_PRESET

/// A preset's numbers, at runtime.
struct PresetSizes {
    std::size_t node_bytes;
    std::size_t node_align;
    std::size_t slots;
    std::size_t slot_bytes;
    std::size_t max_subscriptions;
    std::size_t max_commands;
    std::size_t catalog_topics;
    std::size_t source_info;
    std::size_t link_queue_bytes;
    std::size_t rx_queue_bytes;
};

template <Memory M>
constexpr PresetSizes preset_sizes_of() noexcept {
    using P = NodePreset<M>;
    return {sizeof(typename P::Node), alignof(typename P::Node), P::kSlots, P::kSlotBytes,
            P::kMaxSubscriptions, P::kMaxCommands, P::kCatalogTopics, P::kSourceInfo, P::kLinkQueueBytes,
            P::kRxQueueBytes};
}

inline PresetSizes preset_sizes(Memory memory) noexcept {
    switch (memory) {
        case Memory::Small: return preset_sizes_of<Memory::Small>();
        case Memory::Large: return preset_sizes_of<Memory::Large>();
        case Memory::Medium:
        default: return preset_sizes_of<Memory::Medium>();
    }
}

}  // namespace btpdev

#endif  // BTPDEV_NODE_PRESETS_H
