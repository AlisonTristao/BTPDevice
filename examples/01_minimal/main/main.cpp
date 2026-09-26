// 01_minimal -- a device TraceView finds by name, with a terminal.
//
//   1. Flash it and plug the board's native USB port into the PC.
//   2. In TraceView, add a device named "btpdev-minimal" (Serial, automatic).
//   3. Open its terminal: `help`, `btp -status`, `demo -blink 3`.

#include <BtpDevice.h>
#include <TinyShell.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "btp/btp_device.h"
#include "btp/btp_storage.h"

static BtpDevice device(device_config, storage_config);

// A command reachable from the TraceView terminal: returns 0 on success.
static uint8_t demo_blink(uint8_t times) {
    ESP_LOGI("demo", "blink x%u", static_cast<unsigned>(times));
    return RESULT_OK;
}

extern "C" void app_main() {
    if (device.begin() != ESP_OK) {
        // The reason is already in the log; the app keeps running without BTP.
        return;
    }
    TinyShell* shell = device.shell();
    shell->create_module("demo", "Example commands");
    shell->add(demo_blink, "blink", "demo -blink <times>", "demo");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
