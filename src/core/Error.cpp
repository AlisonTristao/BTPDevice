#include <btpdev/error.h>

#include <cstdarg>
#include <cstdio>

namespace btpdev {

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
        case BTPDEV_ERR_CONFIG: return "BTPDEV_ERR_CONFIG";
        case BTPDEV_ERR_LINK_UNSUPPORTED: return "BTPDEV_ERR_LINK_UNSUPPORTED";
        case BTPDEV_ERR_LINK_START: return "BTPDEV_ERR_LINK_START";
        case BTPDEV_ERR_MEMORY_BUDGET: return "BTPDEV_ERR_MEMORY_BUDGET";
        case BTPDEV_ERR_STORAGE: return "BTPDEV_ERR_STORAGE";
        case BTPDEV_ERR_NO_KEY: return "BTPDEV_ERR_NO_KEY";
        case BTPDEV_ERR_BAD_KEY: return "BTPDEV_ERR_BAD_KEY";
        case BTPDEV_ERR_BAD_TOPIC: return "BTPDEV_ERR_BAD_TOPIC";
        case BTPDEV_ERR_BAD_COMMAND: return "BTPDEV_ERR_BAD_COMMAND";
        case BTPDEV_ERR_ALREADY_STARTED: return "BTPDEV_ERR_ALREADY_STARTED";
        case BTPDEV_ERR_NOT_IMPLEMENTED: return "BTPDEV_ERR_NOT_IMPLEMENTED";
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

}  // namespace btpdev
