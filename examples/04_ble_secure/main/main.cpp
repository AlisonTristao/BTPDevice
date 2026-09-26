// 04_ble_secure -- BLE, encrypted with the device key.
//
//   1. python tools/provision_key.py --password-e <E> --password-l <L>
//      and copy the resulting bally.key to data/ (next to this example).
//   2. pio run -t uploadfs   (data/ -> LittleFS), then pio run -t upload.
//   3. In TraceView, add "sensor-ble" (Bluetooth, automatic) with the same
//      password E. Plot "wave"; the terminal answers `demo -uptime`.
//   Without the key the device refuses BLE and falls back to Serial
//   (see the boot log and `btp -status`).

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
