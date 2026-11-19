// What TraceView's terminal can call.
#include <BtpDevice.h>

#include "../slow_job.h"

static uint8_t hello(const char* name) {
    btpdev::printf("hello, %s!\n", name);
    return RESULT_OK;
}

void app_commands(btpdev::Commands& c) {
    c.module("demo", "Example commands")
        .add("hello", hello, "demo -hello <name>")
        // slow_job() stays shell-agnostic: only this line connects its
        // progress callback to TraceView's terminal.
        .add("progress", [](int steps) { return slow_job(steps, btpdev::progress, (void*)"job"); },
             "demo -progress <steps>: a job with a progress bar");
}
