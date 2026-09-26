// What TraceView's terminal can call.
#include <BtpDevice.h>

static uint8_t hello(const char* name) {
    btpdev::printf("hello, %s!\n", name);
    return RESULT_OK;
}

void app_commands(btpdev::Commands& c) {
    c.module("demo", "Example commands").add("hello", hello, "demo -hello <name>");
}
