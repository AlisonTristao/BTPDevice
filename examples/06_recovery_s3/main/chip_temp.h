// The ESP32-S3's internal temperature sensor: the die temperature, a few
// degrees above the room and rising with load and Wi-Fi traffic. Good enough
// to plot something real without wiring anything.
#pragma once

namespace chip_temp {

bool begin();
/// Degrees Celsius, or NAN when the sensor is not running.
float read();

}  // namespace chip_temp
