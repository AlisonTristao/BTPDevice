// A long job, the way a driver or a third-party library would write it: it
// knows nothing about BtpDevice or the shell. Progress goes to an optional
// plain C callback; btp/btp_commands.cpp plugs btpdev::progress into it.
#pragma once

#include <cstdint>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

using ProgressFn = void (*)(void* ctx, std::uint8_t percent);

inline std::uint8_t slow_job(int steps, ProgressFn on_progress = nullptr, void* ctx = nullptr) {
    if (steps <= 0) return 1;
    for (int i = 1; i <= steps; ++i) {
        vTaskDelay(pdMS_TO_TICKS(50));  // the real work
        if (on_progress != nullptr) on_progress(ctx, static_cast<std::uint8_t>(i * 100 / steps));
    }
    return 0;
}
