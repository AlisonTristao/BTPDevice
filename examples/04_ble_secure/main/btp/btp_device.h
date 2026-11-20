// Who the device is and how it talks to TraceView.
#pragma once
#include <tracekit/config.h>

inline const tracekit::DeviceConfig device_config = {
    .name = "sensor-ble",
    .link = tracekit::Link::Ble,   // needs the device key (data/bally.key)
};
