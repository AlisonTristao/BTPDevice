#ifndef TRACEKIT_SLEEP_H
#define TRACEKIT_SLEEP_H

// Deep sleep (docs/PLANO.md D31): a battery sensor that wakes, measures,
// sends and sleeps again.
//
//   extern "C" void app_main() {
//       device.begin();
//       measure_and_publish();
//       device.sleep_for(5 * 60 * 1000);   // does not return (unless kept awake)
//   }
//
// Waking up is a new boot: app_main runs from the top, begin() connects
// again. device.wake_reason() says why. The boot_id keeps growing without a
// flash write per wake, and the Wi-Fi channel is remembered for a fast join.
//
// A device that sleeps is hard to reach: `btp -awake <minutes>` (or
// SleepConfig::awake_gpio held at boot) makes sleep_for() refuse for that
// long, so there is time for `btp -recovery` or a long session.

#include <cstdint>

namespace tracekit {

enum class WakeReason : std::uint8_t {
    PowerOn,  ///< first boot after power-up (or a reset that is not a wake)
    Timer,    ///< sleep_for() / sleep_until() ran out
    Gpio,     ///< SleepConfig::wake_gpio
    Other,    ///< another wake source the application configured
};

const char* wake_reason_name(WakeReason reason) noexcept;

}  // namespace tracekit

#endif  // TRACEKIT_SLEEP_H
