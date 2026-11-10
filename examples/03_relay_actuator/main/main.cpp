// 03_relay_actuator -- commands that act on hardware, written as one class
// (btpdev::App, in garden_pump.h).
//
//   1. Flash it and plug the board's native USB port into the PC.
//   2. In TraceView, add "bomba-jardim" (Serial, automatic, no key).
//   3. Terminal: `relay -set 1`, `relay -get`. Plot "relay".

#include <BtpDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "garden_pump.h"
#include "relay.h"

GardenPump app;
BtpDevice device(app);  // reads the App in begin(): the order of these two does not matter

extern "C" void app_main() {
    relay_init();
    if (device.begin() != ESP_OK) return;  // the cause is already in the log
    while (true) vTaskDelay(pdMS_TO_TICKS(1000));
}
