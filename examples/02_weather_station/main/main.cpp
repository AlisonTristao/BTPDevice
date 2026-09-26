// 02_weather_station -- simulated sensors over Wi-Fi.
//
//   1. Put your network in btp/btp_device.h (.wifi) and flash.
//   2. In TraceView, add "estacao-quintal" (TCP, automatic): it is found as
//      estacao-quintal.local.
//   3. Plot "weather"; open the Text Board on "status"; `weather -now`.

#include <BtpDevice.h>
#include <cmath>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "btp/btp_device.h"
#include "btp/btp_storage.h"
#include "sensors.h"

float temperature = 20.0f;
float humidity = 50.0f;
float pressure = 1013.0f;
uint32_t readings = 0U;

void app_commands(btpdev::Commands& c);
void app_topics(btpdev::Topics& t);

static BtpDevice device(device_config, storage_config);

extern "C" void app_main() {
    if (device.begin(app_commands, app_topics) != ESP_OK) return;

    // "Read the sensors" every 100 ms: a slow cycle, like a day.
    for (uint32_t tick = 0;; ++tick) {
        const float phase = tick * 0.01f;
        temperature = 22.0f + 6.0f * std::sin(phase);
        humidity = 55.0f - 15.0f * std::sin(phase);
        pressure = 1013.0f + 2.0f * std::cos(phase * 0.3f);
        ++readings;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
