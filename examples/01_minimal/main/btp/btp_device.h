// Who the device is and how it talks to TraceView.
#pragma once
#include <tracekit/config.h>

inline const tracekit::DeviceConfig device_config = {
    .name = "tracekit-minimal",          // what TraceView shows and looks for
    .link = tracekit::Link::Serial,      // the native USB port
};
