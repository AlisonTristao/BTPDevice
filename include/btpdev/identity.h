#ifndef BTPDEV_IDENTITY_PUBLIC_H
#define BTPDEV_IDENTITY_PUBLIC_H

// The device's BTP identity, which every message carries:
//   source_id  from the MAC (bytes 2..5, big-endian), stable across boots
//   boot_id    a counter in NVS, one more at every boot
//   peer_uuid  the MAC plus a fixed suffix (the HELLO peer_uuid)
//
// begin() uses it. Call it earlier to stamp something with the same identity
// before the link is up -- log files written from the first line of app_main,
// for instance. The first call (whoever makes it) counts the boot; later calls
// return the same values.

#include <cstdint>

namespace btpdev {

struct Identity {
    std::uint32_t source_id = 0U;
    std::uint32_t boot_id = 0U;
    std::uint8_t peer_uuid[16] = {};
};

const Identity& identity() noexcept;

}  // namespace btpdev

#endif  // BTPDEV_IDENTITY_PUBLIC_H
