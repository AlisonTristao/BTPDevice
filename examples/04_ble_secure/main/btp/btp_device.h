// Who the device is and how it talks to TraceView.
#pragma once
#include <btpdev/config.h>

inline const btpdev::DeviceConfig device_config = {
    .name = "sensor-ble",
    .link = btpdev::Link::Ble,   // needs the device key (data/bally.key)
};
