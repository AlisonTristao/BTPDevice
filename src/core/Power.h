#ifndef BTPDEV_POWER_H
#define BTPDEV_POWER_H

// PowerMode at run time (docs/PLANO.md D30): Wi-Fi modem sleep, the esp_pm
// configuration (automatic light sleep, CPU frequency) and the lock that
// keeps a Serial link awake.

#include <cstddef>
#include <cstdint>

#include <btpdev/config.h>

namespace btpdev {

class Power {
public:
    /// `wifi`: the link uses the Wi-Fi radio. `serial`: the link is a USB or
    /// UART port, which loses bytes in light sleep.
    void begin(const PowerConfig& config, bool wifi, bool serial, std::uint32_t period_ms) noexcept;
    /// Switches mode (any task). Returns the mode now in effect.
    PowerMode set(PowerMode mode) noexcept;
    PowerMode mode() const noexcept { return mode_; }
    /// The communication task's pause between passes in this mode.
    std::uint32_t idle_ms(std::uint32_t period_ms) const noexcept;
    /// Report for `btp -power`.
    void describe(char* out, std::size_t size) const noexcept;

private:
    PowerConfig config_{};
    PowerMode mode_ = PowerMode::Performance;
    bool wifi_ = false;
    bool serial_ = false;
    std::uint32_t period_ms_ = 1U;  // DeviceConfig::comms.period_ms
    bool light_sleep_ = false;  ///< esp_pm accepted light sleep
    void* serial_lock_ = nullptr;
};

const char* power_mode_name(PowerMode mode) noexcept;

}  // namespace btpdev

#endif  // BTPDEV_POWER_H
