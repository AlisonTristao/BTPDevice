#include "relay.h"

#include <driver/gpio.h>

namespace {
constexpr gpio_num_t kRelayPin = GPIO_NUM_2;
bool g_on = false;
uint32_t g_switches = 0U;
}  // namespace

void relay_init() {
    gpio_reset_pin(kRelayPin);
    gpio_set_direction(kRelayPin, GPIO_MODE_OUTPUT);
    gpio_set_level(kRelayPin, 0);
}

void relay_write(bool on) {
    if (on != g_on) ++g_switches;
    g_on = on;
    gpio_set_level(kRelayPin, on ? 1 : 0);
}

bool relay_state() { return g_on; }
uint32_t relay_switches() { return g_switches; }
