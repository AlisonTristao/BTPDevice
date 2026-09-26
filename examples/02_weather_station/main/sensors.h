// The application's own state: the library only reads it.
#pragma once
#include <cstdint>

extern float temperature;   // degrees C
extern float humidity;      // %
extern float pressure;      // hPa
extern uint32_t readings;   // how many readings so far
