// What TraceView can plot.
#include <TraceKit.h>
#include <esp_timer.h>

void app_topics(tracekit::Topics& t) {
    t.topic("uptime", tracekit::Hz(2)).f64("seconds", "s").fill([](tracekit::Sample& s) {
        s.put("seconds", esp_timer_get_time() / 1e6);
    });
}
