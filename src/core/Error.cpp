#include <tracekit/error.h>

#include <cstdarg>
#include <cstdio>

namespace tracekit {

esp_err_t fail(ErrorInfo& out, esp_err_t code, const char* module,
               const char* format, ...) noexcept {
    out.code = code;
    out.module = module != nullptr ? module : "";
    if (format == nullptr) {
        out.message[0] = '\0';
        return code;
    }
    va_list args;
    va_start(args, format);
    // vsnprintf always NUL-terminates when the size is non-zero; a longer
    // message is cut, never overflowed.
    std::vsnprintf(out.message, sizeof(out.message), format, args);
    va_end(args);
    return code;
}

void clear(ErrorInfo& out) noexcept {
    out.code = ESP_OK;
    out.module = "";
    out.message[0] = '\0';
}

const char* err_name(esp_err_t code) noexcept {
    switch (code) {
        case TRACEKIT_ERR_CONFIG: return "TRACEKIT_ERR_CONFIG";
        case TRACEKIT_ERR_LINK_UNSUPPORTED: return "TRACEKIT_ERR_LINK_UNSUPPORTED";
        case TRACEKIT_ERR_LINK_START: return "TRACEKIT_ERR_LINK_START";
        case TRACEKIT_ERR_MEMORY_BUDGET: return "TRACEKIT_ERR_MEMORY_BUDGET";
        case TRACEKIT_ERR_STORAGE: return "TRACEKIT_ERR_STORAGE";
        case TRACEKIT_ERR_NO_KEY: return "TRACEKIT_ERR_NO_KEY";
        case TRACEKIT_ERR_BAD_KEY: return "TRACEKIT_ERR_BAD_KEY";
        case TRACEKIT_ERR_BAD_TOPIC: return "TRACEKIT_ERR_BAD_TOPIC";
        case TRACEKIT_ERR_BAD_COMMAND: return "TRACEKIT_ERR_BAD_COMMAND";
        case TRACEKIT_ERR_ALREADY_STARTED: return "TRACEKIT_ERR_ALREADY_STARTED";
        case TRACEKIT_ERR_NOT_IMPLEMENTED: return "TRACEKIT_ERR_NOT_IMPLEMENTED";
        default: break;
    }
#if defined(ESP_PLATFORM)
    return esp_err_to_name(code);
#else
    switch (code) {
        case ESP_OK: return "ESP_OK";
        case ESP_FAIL: return "ESP_FAIL";
        case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
        case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
        case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
        case ESP_ERR_INVALID_SIZE: return "ESP_ERR_INVALID_SIZE";
        case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
        case ESP_ERR_NOT_SUPPORTED: return "ESP_ERR_NOT_SUPPORTED";
        case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
        default: return "UNKNOWN_ERROR";
    }
#endif
}

}  // namespace tracekit
