#ifndef BTPDEV_SHELL_TEXT_H
#define BTPDEV_SHELL_TEXT_H

// The shell action carried by COMMAND_REQUEST (bally ecosystem convention,
// shared with TraceView's CommandClient and bally_dongle):
//
//   action_id 1, version 1   parameters = one TinyShell command line, UTF-8
//   action_id 2, version 1   ping: no side effect, answered at once (RTT)
//
// Portable, covered by test/host/test_shell_text.cpp.

#include <btp/codec.hpp>

#include <cstddef>
#include <cstdint>

namespace btpdev {

constexpr std::uint16_t kShellActionId = 0x0001U;
constexpr std::uint16_t kShellActionVersion = 0x0001U;
constexpr std::uint16_t kPingActionId = 0x0002U;
constexpr std::uint16_t kPingActionVersion = 0x0001U;
/// Longest command line accepted (TinyShell's own default line limit).
constexpr std::size_t kMaxShellLine = 512U;

enum class ShellTextError : std::uint8_t { Ok, Empty, TooLong, Control, OutputTooSmall };

/// Copies a COMMAND_REQUEST shell line into `out` (NUL-terminated). One line,
/// never a batch: NUL, CR, LF, DEL and other control bytes (tab allowed) are
/// refused. UTF-8 bytes >= 0x80 pass through untouched.
ShellTextError copy_shell_line(btp::ByteView parameters, char* out, std::size_t capacity) noexcept;

const char* shell_text_error_message(ShellTextError error) noexcept;

}  // namespace btpdev

#endif  // BTPDEV_SHELL_TEXT_H
