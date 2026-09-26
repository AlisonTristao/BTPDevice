#include <BtpDevice.h>

// Smallest possible user of the public API: if this stops compiling, the
// README example stopped compiling too.
static const btpdev::DeviceConfig device_config = {
    .name = "build-smoke",
    .link = btpdev::Link::Serial,
};
static const btpdev::StorageConfig storage_config = {};

static BtpDevice device(device_config, storage_config);

extern "C" void app_main() {
    device.begin();
}
