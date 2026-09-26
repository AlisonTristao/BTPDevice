// 01_minimal -- a device TraceView finds by name, with a terminal and a topic.
//
//   1. Flash it and plug the board's native USB port into the PC.
//   2. In TraceView, add a device named "btpdev-minimal" (Serial, automatic,
//      no key).
//   3. Open its terminal: `help`, `btp -status`, `demo -hello Ana`.
//   4. Plot the "uptime" topic.

#include <BtpDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "btp/btp_device.h"
#include "btp/btp_storage.h"

void app_commands(btpdev::Commands& c);  // btp/btp_commands.cpp
void app_topics(btpdev::Topics& t);      // btp/btp_topics.cpp

static BtpDevice device(device_config, storage_config);

extern "C" void app_main() {
    if (device.begin(app_commands, app_topics) != ESP_OK) {
        return;  // the reason is already in the log
    }
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));  // the application's own work goes here
    }
}
