#include "core/RuntimeErrors.h"

namespace btpdev {

const char* runtime_error_name(RuntimeError kind) noexcept {
    switch (kind) {
        case RuntimeError::RxOverflow: return "receive buffer full, link bytes dropped";
        case RuntimeError::TxRefused: return "frames refused by the link (no peer or send queue full)";
        case RuntimeError::StreamCorrupt: return "corrupt frames on the link (COBS/CRC)";
        case RuntimeError::ShellBusy: return "commands refused, the shell was busy";
        case RuntimeError::TerminalDropped: return "terminal input/output dropped";
        case RuntimeError::SessionTimeout: return "peer went silent, session closed";
        case RuntimeError::Count: break;
    }
    return "?";
}

}  // namespace btpdev
