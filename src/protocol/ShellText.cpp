#include "protocol/ShellText.h"

#include <cstring>

namespace btpdev {

ShellTextError copy_shell_line(btp::ByteView parameters, char* out, std::size_t capacity) noexcept {
    if (parameters.data == nullptr || parameters.size == 0U) return ShellTextError::Empty;
    if (parameters.size > kMaxShellLine) return ShellTextError::TooLong;
    if (out == nullptr || capacity <= parameters.size) return ShellTextError::OutputTooSmall;
    for (std::size_t i = 0U; i < parameters.size; ++i) {
        const std::uint8_t byte = parameters.data[i];
        if (byte == 0x7FU || (byte < 0x20U && byte != '\t')) return ShellTextError::Control;
    }
    std::memcpy(out, parameters.data, parameters.size);
    out[parameters.size] = '\0';
    return ShellTextError::Ok;
}

const char* shell_text_error_message(ShellTextError error) noexcept {
    switch (error) {
        case ShellTextError::Ok: return "ok";
        case ShellTextError::Empty: return "empty command";
        case ShellTextError::TooLong: return "command longer than 512 bytes";
        case ShellTextError::Control: return "command contains control characters (one line only)";
        case ShellTextError::OutputTooSmall: return "internal: command buffer too small";
    }
    return "?";
}

}  // namespace btpdev
