#ifndef BTPDEV_TIME_TEXT_H
#define BTPDEV_TIME_TEXT_H

// Time as text, for `btp -time_set` and `btp -time` (docs/PLANO.md D29).
// Portable: no time zone database here. A text without an offset is local
// time, and only the device knows its zone: parse_time_text() says so and the
// caller converts with mktime() under the device's TZ.
//
// Accepted:
//   2026-11-19T14:00:00-03:00    an offset (also +0530, -03, Z)
//   2026-11-19 14:00             seconds optional, space or T
//   2026-11-19T14:00:00          no offset: local time
//   1763571600                   seconds since 1970 UTC (what a program sends)

#include <cstddef>
#include <cstdint>

namespace btpdev {

/// Times before this are a clock that was never set (2025-01-01 00:00 UTC).
constexpr std::int64_t kMinValidEpoch = 1735689600;

struct ParsedTime {
    bool is_epoch = false;     ///< the text was a plain number
    bool has_offset = false;   ///< an offset or Z was given
    std::int64_t epoch = 0;    ///< UTC seconds, when is_epoch or has_offset
    // The fields as written (local time when !has_offset).
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
};

bool parse_time_text(const char* text, ParsedTime* out) noexcept;

/// Days from 1970-01-01 to a proleptic Gregorian date.
std::int64_t days_from_civil(int year, int month, int day) noexcept;

/// "2026-11-19T17:00:00Z".
void format_utc(std::int64_t epoch, char* out, std::size_t size) noexcept;

}  // namespace btpdev

#endif  // BTPDEV_TIME_TEXT_H
