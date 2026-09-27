#ifndef BTPDEV_LINK_MUTEX_H
#define BTPDEV_LINK_MUTEX_H

// The FreeRTOS mutex each link keeps around its FrameQueue (the queue itself
// is not thread-safe: send() runs on any task, draining on the driver's).

#include <cstdint>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace btpdev {

/// pdMS_TO_TICKS that never rounds a non-zero time down to 0. With the
/// default CONFIG_FREERTOS_HZ=100 a tick is 10 ms: pdMS_TO_TICKS(1) is 0, and
/// a "wait 1 ms" becomes "do not wait" -- a task that loops on it never
/// sleeps and starves the idle task (task watchdog).
inline TickType_t ms_to_ticks(std::uint32_t ms) noexcept {
    const TickType_t ticks = pdMS_TO_TICKS(ms);
    return (ms != 0U && ticks == 0U) ? 1U : ticks;
}

class LinkMutex {
public:
    LinkMutex() noexcept = default;
    ~LinkMutex() {
        if (handle_ != nullptr) vSemaphoreDelete(handle_);
    }
    LinkMutex(const LinkMutex&) = delete;
    LinkMutex& operator=(const LinkMutex&) = delete;

    bool create() noexcept {
        if (handle_ == nullptr) handle_ = xSemaphoreCreateMutex();
        return handle_ != nullptr;
    }
    bool lock(TickType_t timeout = portMAX_DELAY) noexcept {
        return handle_ != nullptr && xSemaphoreTake(handle_, timeout) == pdTRUE;
    }
    void unlock() noexcept {
        if (handle_ != nullptr) xSemaphoreGive(handle_);
    }

private:
    SemaphoreHandle_t handle_ = nullptr;
};

/// Scoped lock; check locked() when a timeout was given.
class LinkLock {
public:
    explicit LinkLock(LinkMutex& mutex, TickType_t timeout = portMAX_DELAY) noexcept
        : mutex_(mutex), locked_(mutex.lock(timeout)) {}
    ~LinkLock() {
        if (locked_) mutex_.unlock();
    }
    LinkLock(const LinkLock&) = delete;
    LinkLock& operator=(const LinkLock&) = delete;
    bool locked() const noexcept { return locked_; }

private:
    LinkMutex& mutex_;
    bool locked_;
};

}  // namespace btpdev

#endif  // BTPDEV_LINK_MUTEX_H
