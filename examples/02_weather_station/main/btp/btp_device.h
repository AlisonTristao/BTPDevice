// Who the device is and how it talks to TraceView.
#pragma once
#include <tracekit/config.h>

inline const tracekit::DeviceConfig device_config = {
    .name = "estacao-quintal",         // found by TraceView as estacao-quintal.local
    .link = tracekit::Link::Tcp,
    .memory = tracekit::Memory::Small,
    .wifi = {.ssid = "minha-rede", .password = "minha-senha"},
    .source_info = {{"location", "Location", "backyard"}},
};
