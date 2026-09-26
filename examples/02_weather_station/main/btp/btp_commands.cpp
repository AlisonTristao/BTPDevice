// What TraceView's terminal can call.
#include <BtpDevice.h>

#include "sensors.h"

static uint8_t sample_now() {
    btpdev::printf("T=%.1f C  H=%.0f %%  P=%.1f hPa\n", temperature, humidity, pressure);
    return RESULT_OK;
}

void app_commands(btpdev::Commands& c) {
    c.module("weather", "Weather station").add("now", sample_now, "weather -now");
}
