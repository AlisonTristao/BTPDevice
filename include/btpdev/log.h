#ifndef BTPDEV_LOG_H
#define BTPDEV_LOG_H

// Log lines for TraceView's log panel (BTP LOG messages).
//
//   btpdev::log(btpdev::LogLevel::Warn, "battery at %u%%", percent);
//
// Any task may call it. The line goes out on the next pass of the comms task,
// sealed like every other message. Best effort, as BTP defines LOG: with no
// peer connected, or with the log queue full, the line is dropped (false, and
// counted in `btp -errors`). For the serial console use ESP_LOGx as usual;
// this is only the wire.

#include <cstddef>
#include <cstdint>

namespace btpdev {

/// Severity carried in the LOG object_id (TraceView's LogSeverity).
enum class LogLevel : std::uint8_t {
    Info = 1,
    Warn = 2,
    Error = 3,
    Debug = 4,
    Command = 5,  ///< a command and its output
};

/// Longest line sent; longer ones are cut.
constexpr std::uint16_t kMaxLogLine = 480U;

/// True when the line was queued.
bool log(LogLevel level, const char* format, ...) noexcept
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

/// A line the application kept and sends later (its own log buffer): it keeps
/// its original time, esp_timer_get_time() microseconds. `text` needs no NUL.
/// True when queued; false means "keep it and try again later".
bool log_record(LogLevel level, std::uint64_t timestamp_us, const char* text,
                std::size_t length) noexcept;

/// True on the library's shell task while a command runs: btpdev::printf()
/// there becomes part of the command's result.
bool in_command() noexcept;

}  // namespace btpdev

#endif  // BTPDEV_LOG_H
