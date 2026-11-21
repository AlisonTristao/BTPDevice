#ifndef TRACEKIT_RESTART_H
#define TRACEKIT_RESTART_H

// esp_restart() a little later, so a command's reply can leave first.
//
// From a task of its own, never from an esp_timer callback: esp_restart()
// runs the shutdown handlers in the calling task, and one of them is
// esp_wifi_stop(), which waits on the Wi-Fi driver -- whose own timers are
// dispatched by that same esp_timer task. Called from there it could wait on
// itself: the restart never happened, the esp_timer task stayed blocked, Wi-Fi
// limped on (lost pings, missed mDNS, dropped sessions) and every later shell
// command came back BUSY. `btp -recovery` had already pointed the otadata at
// the recovery, so the next reset landed there.

#include <cstdint>

#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace tracekit {

inline void restart_later(std::uint32_t delay_ms) noexcept {
    struct Task {
        static void run(void* arg) {
            const auto ms = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(arg));
            vTaskDelay(pdMS_TO_TICKS(ms) + 1U);  // +1: never 0 ticks
            esp_restart();
        }
    };
    if (delay_ms == 0U) esp_restart();
    // Above the application's tasks so a busy loop cannot starve it; 4 KB is
    // what the shutdown handlers (Wi-Fi stop) need.
    if (xTaskCreate(&Task::run, "tk_restart", 4096, reinterpret_cast<void*>(static_cast<std::uintptr_t>(delay_ms)),
                    tskIDLE_PRIORITY + 10U, nullptr) != pdPASS) {
        esp_restart();
    }
}

}  // namespace tracekit

#endif  // TRACEKIT_RESTART_H
