// The actuator: a relay on a GPIO (the DevKitC's LED pin by default, so it
// is visible without wiring anything).
#pragma once
#include <cstdint>

void relay_init();
void relay_write(bool on);
bool relay_state();
uint32_t relay_switches();
