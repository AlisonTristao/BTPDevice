#include "core/Identity.h"

#include <cstring>

namespace tracekit {

std::uint32_t source_id_from_mac(const std::uint8_t mac[6]) noexcept {
    if (mac == nullptr) return 0U;
    std::uint32_t id = (static_cast<std::uint32_t>(mac[2]) << 24U) |
                       (static_cast<std::uint32_t>(mac[3]) << 16U) |
                       (static_cast<std::uint32_t>(mac[4]) << 8U) |
                       static_cast<std::uint32_t>(mac[5]);
    return id != 0U ? id : 1U;  // BTP reserves 0
}

void uuid_from_mac(const std::uint8_t mac[6], std::uint8_t uuid_out[16]) noexcept {
    // bally_OS's suffix, kept so a robot moved onto the library keeps its uuid.
    static constexpr std::uint8_t kSuffix[10] = {0xB0, 0xB1, 0xB2, 0xB3, 0xB4,
                                                 0xB5, 0xB6, 0xB7, 0xB8, 0xB9};
    if (uuid_out == nullptr) return;
    if (mac != nullptr) {
        std::memcpy(uuid_out, mac, 6U);
    } else {
        std::memset(uuid_out, 0, 6U);
    }
    std::memcpy(uuid_out + 6U, kSuffix, sizeof(kSuffix));
}

std::uint32_t next_boot_id(std::uint32_t previous) noexcept {
    return previous == 0xFFFFFFFFU ? 1U : previous + 1U;
}

BootIdPlan plan_boot_id(bool woke, std::uint32_t rtc_next, std::uint32_t rtc_last,
                        std::uint32_t nvs_previous) noexcept {
    BootIdPlan plan;
    if (woke && rtc_next != 0U && rtc_next <= rtc_last) {
        plan.id = rtc_next;
        plan.rtc_next = rtc_next < rtc_last ? rtc_next + 1U : 0U;  // 0: block used up
        plan.rtc_last = rtc_last;
        return plan;
    }
    plan.id = next_boot_id(nvs_previous);
    // Near the wrap no block: the next boot is a cold one again.
    plan.saved = plan.id <= 0xFFFFFFFFU - (kBootBlock - 1U) ? plan.id + (kBootBlock - 1U) : plan.id;
    plan.save = true;
    plan.rtc_next = plan.saved > plan.id ? plan.id + 1U : 0U;
    plan.rtc_last = plan.saved;
    return plan;
}

}  // namespace tracekit
