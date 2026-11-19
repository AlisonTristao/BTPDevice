// The DevKitC-1's on-board RGB LED (one WS2812, driven by the RMT).
//
// Board revisions put it on different pins: GPIO48 on v1.0 (and most
// clones), GPIO38 on v1.1. The pin is kept in NVS, so `led -pin 38` fixes a
// board without rebuilding.
#pragma once

#include <cstdint>

namespace rgb_led {

constexpr int kDefaultPin = 48;

/// Reads the saved pin (kDefaultPin when none) and starts the driver.
bool begin();
/// Moves the LED to `gpio` and saves it for the next boots.
bool set_pin(int gpio);
int pin();

/// 0..255 per channel. The WS2812 is very bright: 20-40 is plenty on a desk.
bool set(std::uint8_t r, std::uint8_t g, std::uint8_t b);
std::uint8_t red();
std::uint8_t green();
std::uint8_t blue();

}  // namespace rgb_led
