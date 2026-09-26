// 05_espnow_dongle -- ESP-NOW through the bally_dongle.
//
//   1. Provision the SAME key file the dongle has (password L is the
//      fleet's; password E is this device's) and copy it to data/.
//   2. Set the dongle's MAC and channel in btp/btp_device.h.
//   3. pio run -t uploadfs, then pio run -t upload.
//   4. TraceView sees "sensor-radio" through the dongle (hub).

#include <BtpDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "btp/btp_device.h"
#include "btp/btp_storage.h"

void app_commands(btpdev::Commands& c);
void app_topics(btpdev::Topics& t);

static BtpDevice device(device_config, storage_config);

extern "C" void app_main() {
    if (device.begin(app_commands, app_topics) != ESP_OK) return;
    while (true) vTaskDelay(pdMS_TO_TICKS(1000));
}
