// Who the device is and how it talks to TraceView.
#pragma once
#include <btpdev/config.h>

inline const btpdev::DeviceConfig device_config = {
    .name = "btpdev-minimal",          // what TraceView shows and looks for
    .link = btpdev::Link::Serial,      // the native USB port
};
