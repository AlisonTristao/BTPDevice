#ifndef BTPDEV_IDENTITY_H
#define BTPDEV_IDENTITY_H

// How a device derives its BTP identity. The same rules as bally_OS, so a
// device migrated to the library keeps the identity TraceView already knows
// (TraceView caches manifests by source_id and re-validates peer_uuid on every
// connection -- docs/PLANO.md "Fase 0: resultados").
//
//   source_id  the 4 low octets of the factory MAC, big-endian; never 0
//   peer_uuid  MAC (6 octets) + a fixed 10-octet suffix: stable per device
//   boot_id    a counter kept in NVS, +1 every boot, never 0 -- it is what
//              lets a peer tell a reboot from a retransmission (the command
//              dedup key is source_id + boot_id + sequence)
//
// Portable; the NVS part lives in BtpDevice.cpp.

#include <cstdint>

namespace btpdev {

std::uint32_t source_id_from_mac(const std::uint8_t mac[6]) noexcept;
void uuid_from_mac(const std::uint8_t mac[6], std::uint8_t uuid_out[16]) noexcept;
/// The boot_id after `previous` (0 = never booted): wraps past UINT32_MAX to 1.
std::uint32_t next_boot_id(std::uint32_t previous) noexcept;

/// boot_id across deep sleep (docs/PLANO.md D31). A cold boot takes the next
/// id after the one NVS holds and reserves a block of kBootBlock ids, saving
/// the block's last id to NVS (one flash write); a wake from deep sleep takes
/// the next id of that block from RTC memory without touching flash. A power
/// loss forgets RTC memory, and NVS already holds the block's end: no id is
/// ever reused (it is part of the AEAD nonce).
constexpr std::uint32_t kBootBlock = 64U;

struct BootIdPlan {
    std::uint32_t id = 0U;       ///< this boot's boot_id
    bool save = false;           ///< write `saved` to NVS
    std::uint32_t saved = 0U;    ///< the highest id this block may use
    std::uint32_t rtc_next = 0U; ///< what RTC memory holds for the next wake
    std::uint32_t rtc_last = 0U;
};

/// `woke`: this boot is a wake from deep sleep with RTC memory intact.
BootIdPlan plan_boot_id(bool woke, std::uint32_t rtc_next, std::uint32_t rtc_last,
                        std::uint32_t nvs_previous) noexcept;

}  // namespace btpdev

#endif  // BTPDEV_IDENTITY_H
