// The presets compile, keep their order, and their sizes are printed for
// docs/PLANO.md (4.11).

#include "core/NodePresets.h"

#include <cstdio>

#include "check.h"

int main() {
    const auto small = btpdev::preset_sizes(btpdev::Memory::Small);
    const auto medium = btpdev::preset_sizes(btpdev::Memory::Medium);
    const auto large = btpdev::preset_sizes(btpdev::Memory::Large);
    std::printf("node bytes: small %zu, medium %zu, large %zu\n", small.node_bytes,
                medium.node_bytes, large.node_bytes);
    CHECK(small.node_bytes < medium.node_bytes);
    CHECK(medium.node_bytes < large.node_bytes);
    CHECK(small.link_queue_bytes <= medium.link_queue_bytes);
    // A COMMAND_REQUEST with a full shell line plus its result must fit the
    // dedup cache's per-command storage.
    CHECK(768U >= 20U + 512U + 30U + btpdev::kMaxResultMessage);
    return test_result("test_node_presets");
}
