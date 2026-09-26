// What TraceView's terminal can call: switch the relay and read it back.
#include <BtpDevice.h>

#include "relay.h"

extern BtpDevice device;  // main.cpp

static uint8_t relay_set(uint8_t on) {
    if (on > 1U) {
        btpdev::printf("use 0 or 1\n");
        return RESULT_ERROR;
    }
    relay_write(on != 0U);
    btpdev::printf("relay %s\n", on ? "on" : "off");
    device.publish("relay");  // the plot sees the change at once
    // Also in TraceView's log panel, with the time it happened.
    btpdev::log(btpdev::LogLevel::Info, "relay switched %s", on ? "on" : "off");
    return RESULT_OK;
}

static uint8_t relay_get() {
    btpdev::printf("relay is %s (%lu switches)\n", relay_state() ? "on" : "off",
                   static_cast<unsigned long>(relay_switches()));
    return RESULT_OK;
}

void app_commands(btpdev::Commands& c) {
    c.module("relay", "Garden pump relay")
        .add("set", relay_set, "relay -set <0|1>")
        .add("get", relay_get, "relay -get");
}
