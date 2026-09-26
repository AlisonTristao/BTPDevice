// Who the device is and how it talks to TraceView.
#pragma once
#include <btpdev/config.h>

inline const btpdev::DeviceConfig device_config = {
    .name = "estacao-quintal",         // found by TraceView as estacao-quintal.local
    .link = btpdev::Link::Tcp,
    .memory = btpdev::Memory::Small,
    .wifi = {.ssid = "minha-rede", .password = "minha-senha"},
    .source_info = {{"location", "Location", "backyard"}},
};
