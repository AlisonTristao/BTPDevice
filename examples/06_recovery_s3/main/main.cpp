// 06_recovery_s3 -- an ESP32-S3 DevKitC-1 with nothing wired: the chip's own
// temperature and the on-board RGB LED, over Wi-Fi (TCP), updated over Wi-Fi
// through the recovery firmware (docs/PLANO.md 4.14).
//
//   1. data/bally.key (tools/provision_key.py) and data/wifi.txt
//      ("ssid,password"), then `pio run -t uploadfs`.
//   2. `pio run -t upload`: the app goes to `factory`, the recovery to `ota_0`.
//   3. TraceView: add "btpdev-s3-demo" (TCP, btpdev-s3-demo.local, password E).
//      Plot "chip"; terminal: `led -rgb 0, 30, 0`, `chip -temp`.
//      Settings, kept across reboots: `cfg -list`, `cfg -set led_pin, 38`,
//      `cfg -set sample_ms, 1000`, `cfg -set power_save, on` (then `btp -power`).
//   4. Update over Wi-Fi: `btp -recovery`, then
//      python tools/btp_ota.py btpdev-s3-demo.local .pio/build/esp32-s3/firmware.bin
//   5. Crash loop: `demo -crash` three times in a row lands in the recovery.

#include <BtpDevice.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "chip_temp.h"
#include "rgb_led.h"

class S3Demo : public btpdev::App {
public:
    btpdev::DeviceConfig config() const override {
        return {
            .name = "btpdev-s3-demo",
            .link = btpdev::Link::Tcp,         // networks from data/wifi.txt
            .memory = btpdev::Memory::Small,
            .debug_log = true,                 // boot steps on the serial console
            .recovery = {.crash_loop = 3},     // three crashes in a row -> recovery
        };
    }

    // Loaded from NVS before commands() and topics(); `cfg -set` changes them.
    void settings(btpdev::Settings& s) override {
        s.u8("led_pin", &led_pin_, rgb_led::kDefaultPin)
            .range(0, 48)
            .help("48: DevKitC-1 v1.0, 38: v1.1")
            .on_change([this] {
                if (led_started_) rgb_led::set_pin(led_pin_);
            });
        s.u32("sample_ms", &sample_ms_, 500U).range(100, 60000).unit("ms").help("temperature sampling period");
        s.boolean("power_save", &power_save_, false)
            .help("Wi-Fi modem sleep + light sleep when idle")
            .on_change([this] { apply_power(); });
    }

    void commands(btpdev::Commands& c) override {
        c.module("led", "On-board RGB LED")
            .add("rgb", [this](uint8_t r, uint8_t g, uint8_t b) { return rgb(r, g, b); }, "led -rgb <r>, <g>, <b>")
            .add("off", [this]() { return rgb(0, 0, 0); }, "led -off");
        c.module("chip", "ESP32-S3 die")
            .add("temp", [this]() {
                btpdev::printf("%.1f C\n", static_cast<double>(temperature_));
                return static_cast<uint8_t>(RESULT_OK);
            }, "chip -temp");
        c.module("demo", "Recovery tests")
            .add("crash", []() -> uint8_t {
                btpdev::printf("crashing on purpose\n");
                vTaskDelay(pdMS_TO_TICKS(200));  // let the line leave
                std::abort();
            }, "demo -crash (three in a row enter the recovery)");
    }

    void topics(btpdev::Topics& t) override {
        chip_ = t.topic("chip", btpdev::Hz(2))
                    .f32("temperature", &temperature_, "°C", 20.f, 80.f)
                    .u8("r", &r_)
                    .u8("g", &g_)
                    .u8("b", &b_);
    }

    void apply_power() {
        device().set_power_mode(power_save_ ? btpdev::PowerMode::Saving : btpdev::PowerMode::Performance);
    }

    void start_hardware() {
        apply_power();
        chip_temp::begin();
        led_started_ = rgb_led::begin(led_pin_);
        rgb_led::set(0, 0, 16);  // dim blue: the application is up
    }

    /// The application's own loop: read the sensor every sample_ms.
    void run() {
        while (true) {
            temperature_ = chip_temp::read();
            vTaskDelay(pdMS_TO_TICKS(sample_ms_));
        }
    }

private:
    uint8_t rgb(uint8_t r, uint8_t g, uint8_t b) {
        if (!rgb_led::set(r, g, b)) return RESULT_ERROR;
        r_ = r;
        g_ = g;
        b_ = b;
        device().publish(chip_);  // the plot follows the command at once
        return RESULT_OK;
    }

    std::uint8_t led_pin_ = rgb_led::kDefaultPin;
    std::uint32_t sample_ms_ = 500U;
    bool led_started_ = false;
    bool power_save_ = false;
    float temperature_ = NAN;
    uint8_t r_ = 0, g_ = 0, b_ = 0;
    btpdev::Topic chip_;
};

S3Demo app;
BtpDevice device(app);

extern "C" void app_main() {
    if (device.begin() != ESP_OK) return;  // the cause is already in the log
    app.start_hardware();
    app.run();
}
