// What TraceView can plot.
#include <TraceKit.h>
#include <esp_timer.h>

#include <cmath>

void app_topics(tracekit::Topics& t) {
    t.topic("wave", tracekit::Hz(20)).f32("sine").f32("cosine").fill([](tracekit::Sample& s) {
        const double seconds = esp_timer_get_time() / 1e6;
        s.put("sine", std::sin(seconds));
        s.put("cosine", std::cos(seconds));
    });
}
