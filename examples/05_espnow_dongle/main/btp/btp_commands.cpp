// What TraceView's terminal can call.
#include <BtpDevice.h>
#include <esp_timer.h>

static uint8_t uptime() {
    btpdev::printf("up %llu s\n", static_cast<unsigned long long>(esp_timer_get_time() / 1000000LL));
    return RESULT_OK;
}

void app_commands(btpdev::Commands& c) {
    c.module("demo", "Example commands").add("uptime", uptime, "demo -uptime");
}
