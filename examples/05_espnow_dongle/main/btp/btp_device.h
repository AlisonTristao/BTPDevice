// Who the device is and how it talks to TraceView.
#pragma once
#include <tracekit/config.h>

inline const tracekit::DeviceConfig device_config = {
    .name = "sensor-radio",
    .link = tracekit::Link::EspNow,
    // The dongle's MAC and the channel it listens on (its `btp -status` /
    // settings show both).
    .espnow = {.peer_mac = {0xDC, 0xDA, 0x0C, 0x30, 0xAA, 0x5C}, .channel = 1},
};
