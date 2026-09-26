// What TraceView can plot.
#include <BtpDevice.h>
#include <esp_timer.h>

void app_topics(btpdev::Topics& t) {
    t.topic("uptime", btpdev::Hz(2)).f64("seconds", "s").fill([](btpdev::Sample& s) {
        s.put("seconds", esp_timer_get_time() / 1e6);
    });
}
