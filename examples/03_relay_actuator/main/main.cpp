// 03_relay_actuator -- commands that act on hardware.
//
//   1. Flash it and plug the board's native USB port into the PC.
//   2. In TraceView, add "bomba-jardim" (Serial, automatic, no key).
//   3. Terminal: `relay -set 1`, `relay -get`. Plot "relay".

#include <BtpDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "btp/btp_device.h"
#include "btp/btp_storage.h"
#include "relay.h"

void app_commands(btpdev::Commands& c);
void app_topics(btpdev::Topics& t);

BtpDevice device(device_config, storage_config);  // btp_commands.cpp publishes through it

extern "C" void app_main() {
    relay_init();
    if (device.begin(app_commands, app_topics) != ESP_OK) return;
    while (true) vTaskDelay(pdMS_TO_TICKS(1000));
}
