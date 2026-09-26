// What TraceView can plot. The fields are bound to the variables: the
// library reads them whenever a subscriber is due for a sample.
#include <BtpDevice.h>
#include <esp_timer.h>

#include "sensors.h"

void app_topics(btpdev::Topics& t) {
    t.topic("weather", btpdev::Hz(1))
        .f32("temperature", &temperature, "°C", -40, 85)
        .f32("humidity", &humidity, "%", 0, 100)
        .f32("pressure", &pressure, "hPa")
        .u32("readings", &readings);

    t.text("status", btpdev::Hz(0.2), [](btpdev::Text& out) {
        out.printf("up %llu s, %lu readings",
                   static_cast<unsigned long long>(esp_timer_get_time() / 1000000LL),
                   static_cast<unsigned long>(readings));
    });
}
