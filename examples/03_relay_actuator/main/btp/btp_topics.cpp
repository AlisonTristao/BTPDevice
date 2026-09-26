// The relay state: once per second for the plot, and at once on every change
// (device.publish("relay") in btp_commands.cpp).
#include <BtpDevice.h>

#include "relay.h"

void app_topics(btpdev::Topics& t) {
    t.topic("relay", btpdev::Hz(1))
        .boolean("on")
        .u32("switches")
        .fill([](btpdev::Sample& s) {
            s.put("on", relay_state());
            s.put("switches", relay_switches());
        });
}
