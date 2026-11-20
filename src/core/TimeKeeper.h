#ifndef BTPDEV_TIME_KEEPER_H
#define BTPDEV_TIME_KEEPER_H

// The device's wall clock (docs/PLANO.md D29): time zone, SNTP, and whether
// the time can be trusted. Validity lives in RTC memory next to the clock
// itself: both survive a reboot and deep sleep, both are lost on power loss.

#include <cstddef>
#include <cstdint>
#include <ctime>

#include <btpdev/config.h>
#include <btpdev/time.h>

namespace btpdev {

class TimeKeeper {
public:
    /// Applies the zone (the one saved by `btp -tz`, else config.tz).
    void begin(const TimeConfig& config) noexcept;
    /// Once the TCP link is up: SNTP starts asking as soon as there is an IP.
    void start_sntp() noexcept;

    bool valid() const noexcept;
    TimeSource source() const noexcept;
    std::time_t now() const noexcept;
    std::int64_t now_us() const noexcept;

    /// Sets the clock to `epoch` (UTC seconds). False when it is before
    /// kMinValidEpoch (a clock that was never set).
    bool set(std::int64_t epoch, TimeSource source) noexcept;
    /// Parses a `btp -time_set` text (TimeText.h); false with `error` filled.
    bool set_text(const char* text, char* error, std::size_t size) noexcept;
    /// Changes the zone and keeps it in NVS. False: not a usable TZ string.
    bool set_zone(const char* tz) noexcept;
    const char* zone() const noexcept { return tz_; }

    /// Multi-line report for `btp -time`.
    void describe(char* out, std::size_t size) const noexcept;

private:
    TimeConfig config_{};
    char tz_[48] = "";
    bool sntp_started_ = false;
};

}  // namespace btpdev

#endif  // BTPDEV_TIME_KEEPER_H
