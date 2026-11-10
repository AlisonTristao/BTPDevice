// The whole device in one class (btpdev::App): who it is, what the terminal
// can call and what TraceView can plot, with the state as members -- no
// globals, no `extern BtpDevice`.
#pragma once

#include <BtpDevice.h>

#include "relay.h"

class GardenPump : public btpdev::App {
public:
    btpdev::DeviceConfig config() const override {
        return {
            .name = "bomba-jardim",
            .link = btpdev::Link::Serial,
            .memory = btpdev::Memory::Small,
        };
    }

    void commands(btpdev::Commands& c) override {
        c.module("relay", "Garden pump relay")
            .add("set", [this](uint8_t on) { return set(on); }, "relay -set <0|1>")
            .add("get", [this]() { return get(); }, "relay -get");
    }

    // The relay state: once per second for the plot, and at once on every
    // change (device().publish() in set()).
    void topics(btpdev::Topics& t) override {
        relay_ = t.topic("relay", btpdev::Hz(1))
                     .boolean("on")
                     .u32("switches")
                     .fill([](btpdev::Sample& s) {
                         s.put("on", relay_state());
                         s.put("switches", relay_switches());
                     });
    }

private:
    uint8_t set(uint8_t on) {
        if (on > 1U) {
            btpdev::printf("use 0 or 1\n");
            return RESULT_ERROR;
        }
        relay_write(on != 0U);
        btpdev::printf("relay %s\n", on ? "on" : "off");
        device().publish(relay_);  // by handle: no name lookup, no typo possible
        // Also in TraceView's log panel, with the time it happened.
        btpdev::log(btpdev::LogLevel::Info, "relay switched %s", on ? "on" : "off");
        return RESULT_OK;
    }

    uint8_t get() {
        btpdev::printf("relay is %s (%lu switches)\n", relay_state() ? "on" : "off",
                       static_cast<unsigned long>(relay_switches()));
        return RESULT_OK;
    }

    btpdev::Topic relay_;
};
