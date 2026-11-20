#ifndef BTPDEV_TIME_H
#define BTPDEV_TIME_H

// Wall-clock time (docs/PLANO.md D29). The ESP32 has no battery-backed clock:
// the time survives a reboot and deep sleep (the RTC keeps counting) but not
// a power loss. It is set by SNTP when the TCP link reaches the internet,
// by `btp -time_set` in the terminal, or by BtpDevice::set_time().
//
//   if (device.time_valid()) {
//       std::time_t t = device.now();          // UTC seconds
//       std::tm local;
//       localtime_r(&t, &local);               // in DeviceConfig::time.tz
//   }

#include <cstdint>

namespace btpdev {

enum class TimeSource : std::uint8_t {
    None,         ///< never set since the last power-up: not valid
    Sntp,         ///< from an NTP server
    Manual,       ///< btp -time_set
    Application,  ///< BtpDevice::set_time() (an RTC chip, GPS...)
};

const char* time_source_name(TimeSource source) noexcept;

}  // namespace btpdev

#endif  // BTPDEV_TIME_H
