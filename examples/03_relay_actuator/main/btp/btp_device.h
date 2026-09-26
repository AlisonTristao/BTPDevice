// Who the device is and how it talks to TraceView.
#pragma once
#include <btpdev/config.h>

inline const btpdev::DeviceConfig device_config = {
    .name = "bomba-jardim",
    .link = btpdev::Link::Serial,
    .memory = btpdev::Memory::Small,
};
