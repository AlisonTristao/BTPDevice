// 07_sleep_sensor -- a battery sensor: wake, measure, log, publish, sleep
// (docs/PLANO.md D31). An ESP32-S3 DevKitC-1 with nothing wired: the chip's
// own temperature.
//
// Every cycle is a new boot:
//   1. begin(): Wi-Fi and the TCP link come up (and SNTP sets the clock the
//      first time; the RTC keeps it through deep sleep).
//   2. One reading goes to /fs/log.csv with the date ("time,celsius"), and to
//      the "sensor" topic for TraceView.
//   3. The device stays up `awake_ms` (so TraceView can connect and read),
//      then sleeps `period_s`.
//
// Settings (`cfg -set period_s, 300`, ...) survive the sleep. To reach the
// device for longer -- a session, `btp -recovery` -- send `btp -awake 10`
// during an awake window: sleep_for() refuses for 10 minutes.
//
//   data/bally.key + data/wifi.txt, then `pio run -t uploadfs` and `-t upload`.
//   Terminal: `btp -sleep` (why it woke, how many wakes), `btp -fs`,
//   `log -tail` (the last lines of the log).

#include <BtpDevice.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "chip_temp.h"

class SleepSensor : public btpdev::App {
public:
    btpdev::DeviceConfig config() const override {
        return {
            .name = "btpdev-sleep",
            .link = btpdev::Link::Tcp,
            .memory = btpdev::Memory::Small,
            .debug_log = true,
        };
    }

    void settings(btpdev::Settings& s) override {
        s.u32("period_s", &period_s_, 60U).range(10, 86400).unit("s").help("deep sleep between readings");
        s.u32("awake_ms", &awake_ms_, 8000U).range(1000, 120000).unit("ms").help("up after each reading");
    }

    void commands(btpdev::Commands& c) override {
        c.module("log", "The readings logged on the device")
            .add("tail", [this]() { return tail(); }, "log -tail (last 10 lines)")
            .add("clear", []() -> uint8_t {
                btpdev::FileSystem* fs = btpdev_fs();
                return fs != nullptr && fs->remove(kLogFile) ? RESULT_OK : RESULT_ERROR;
            }, "log -clear");
    }

    void topics(btpdev::Topics& t) override {
        sensor_ = t.topic("sensor", btpdev::Hz(1))
                      .f32("temperature", &temperature_, "°C", 20.f, 80.f)
                      .u32("wakes", &wakes_);
    }

    void cycle() {
        temperature_ = chip_temp::read();
        wakes_ = device().wake_count();
        log_reading();
        device().publish(sensor_);
        vTaskDelay(pdMS_TO_TICKS(awake_ms_));
        if (!device().sleep_for(static_cast<std::uint64_t>(period_s_) * 1000ULL)) {
            // Kept awake (btp -awake): measure on the same period, without sleeping.
            vTaskDelay(pdMS_TO_TICKS(static_cast<std::uint64_t>(period_s_) * 1000ULL));
        }
    }

private:
    static constexpr const char* kLogFile = "log.csv";

    static btpdev::FileSystem* btpdev_fs();

    void log_reading() {
        btpdev::FileSystem* fs = btpdev_fs();
        if (fs == nullptr) return;
        char line[64];
        if (device().time_valid()) {
            const std::time_t now = device().now();
            std::tm local{};
            localtime_r(&now, &local);
            char when[32];
            std::strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%S%z", &local);
            std::snprintf(line, sizeof(line), "%s,%.2f\n", when, static_cast<double>(temperature_));
        } else {
            // No clock yet (first boot without internet): the wake number instead.
            std::snprintf(line, sizeof(line), "wake-%lu,%.2f\n", static_cast<unsigned long>(wakes_),
                          static_cast<double>(temperature_));
        }
        fs->append(kLogFile, line, std::strlen(line));
    }

    uint8_t tail() {
        btpdev::FileSystem* fs = btpdev_fs();
        static char buffer[2048];
        std::size_t size = 0U;
        if (fs == nullptr || !fs->read(kLogFile, buffer, sizeof(buffer) - 1U, &size)) {
            btpdev::printf("no log yet\n");
            return RESULT_OK;
        }
        buffer[size] = '\0';
        // The last 10 lines.
        int lines = 0;
        std::size_t start = size;
        while (start > 0U) {
            if (buffer[start - 1U] == '\n' && start != size && ++lines == 10) break;
            --start;
        }
        btpdev::printf("%s", buffer + start);
        return RESULT_OK;
    }

    std::uint32_t period_s_ = 60U;
    std::uint32_t awake_ms_ = 8000U;
    float temperature_ = NAN;
    std::uint32_t wakes_ = 0U;
    btpdev::Topic sensor_;
};

SleepSensor app;
BtpDevice device(app);

btpdev::FileSystem* SleepSensor::btpdev_fs() { return ::device.fs(); }

extern "C" void app_main() {
    if (device.begin() != ESP_OK) return;  // the cause is already in the log
    chip_temp::begin();
    for (;;) app.cycle();
}
