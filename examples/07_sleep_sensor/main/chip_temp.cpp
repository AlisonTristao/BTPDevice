#include "chip_temp.h"

#include <cmath>

#include <driver/temperature_sensor.h>
#include <esp_log.h>

namespace chip_temp {
namespace {

temperature_sensor_handle_t s_sensor = nullptr;

}  // namespace

bool begin() {
    // The range picks the sensor's internal offset; -10..80 covers a desk.
    temperature_sensor_config_t config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&config, &s_sensor) != ESP_OK || temperature_sensor_enable(s_sensor) != ESP_OK) {
        ESP_LOGE("chip_temp", "the temperature sensor did not start");
        s_sensor = nullptr;
        return false;
    }
    return true;
}

float read() {
    float celsius = NAN;
    if (s_sensor == nullptr || temperature_sensor_get_celsius(s_sensor, &celsius) != ESP_OK) return NAN;
    return celsius;
}

}  // namespace chip_temp
