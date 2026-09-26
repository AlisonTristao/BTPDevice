#include "transport/WifiNetworks.h"

#include <cstring>

namespace btpdev {

WifiParseResult parse_wifi_networks(const char* text, std::size_t size,
                                    WifiNetwork* out, std::size_t max) noexcept {
    WifiParseResult result{};
    if (text == nullptr) return result;

    std::size_t pos = 0U;
    while (pos < size) {
        std::size_t end = pos;
        while (end < size && text[end] != '\n' && text[end] != '\r' && text[end] != '\0') {
            ++end;
        }
        const char* line = text + pos;
        const std::size_t length = end - pos;
        pos = end + 1U;
        if (end < size && text[end] == '\0') pos = size;  // stop at an embedded NUL

        if (length == 0U || line[0] == '#') continue;

        const char* comma = static_cast<const char*>(std::memchr(line, ',', length));
        const std::size_t ssid_length = comma != nullptr ? static_cast<std::size_t>(comma - line) : 0U;
        const std::size_t password_length =
            comma != nullptr ? length - ssid_length - 1U : 0U;
        if (comma == nullptr || ssid_length == 0U || ssid_length > WifiNetwork::kMaxSsid ||
            password_length > WifiNetwork::kMaxPassword || result.networks >= max ||
            out == nullptr) {
            ++result.skipped_lines;
            continue;
        }

        WifiNetwork& network = out[result.networks++];
        std::memcpy(network.ssid, line, ssid_length);
        network.ssid[ssid_length] = '\0';
        std::memcpy(network.password, comma + 1, password_length);
        network.password[password_length] = '\0';
    }
    return result;
}

bool mdns_host_from_name(const char* name, char* out, std::size_t capacity) noexcept {
    constexpr std::size_t kMaxHost = 63U;
    if (name == nullptr || out == nullptr || capacity == 0U) return false;

    // trimmed(): skip leading/trailing whitespace before mapping.
    std::size_t begin = 0U;
    std::size_t end = std::strlen(name);
    auto is_space = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
    };
    while (begin < end && is_space(name[begin])) ++begin;
    while (end > begin && is_space(name[end - 1U])) --end;

    std::size_t length = 0U;
    bool last_was_dash = true;  // no leading '-'
    for (std::size_t i = begin; i < end && length < kMaxHost; ++i) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            if (length + 1U >= capacity) return false;
            out[length++] = c;
            last_was_dash = false;
        } else if (!last_was_dash) {
            if (length + 1U >= capacity) return false;
            out[length++] = '-';
            last_was_dash = true;
        }
    }
    while (length > 0U && out[length - 1U] == '-') --length;
    out[length] = '\0';
    return length > 0U;
}

}  // namespace btpdev
