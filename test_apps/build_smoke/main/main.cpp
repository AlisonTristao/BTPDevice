#include <TraceKit.h>

// Smallest possible user of the public API: if this stops compiling, the
// README example stopped compiling too.
static const tracekit::DeviceConfig device_config = {
    .name = "build-smoke",
    .link = tracekit::Link::Serial,
};
static const tracekit::StorageConfig storage_config = {};

static TraceKit device(device_config, storage_config);

extern "C" void app_main() {
    device.begin();
}
