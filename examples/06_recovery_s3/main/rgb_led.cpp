#include "rgb_led.h"

#include <esp_log.h>
#include <led_strip.h>

namespace rgb_led {
namespace {

constexpr const char* kTag = "rgb_led";

led_strip_handle_t s_strip = nullptr;
int s_pin = kDefaultPin;
std::uint8_t s_rgb[3] = {};

bool start(int gpio) {
    if (s_strip != nullptr) {
        led_strip_clear(s_strip);
        led_strip_del(s_strip);
        s_strip = nullptr;
    }
    led_strip_config_t strip{};
    strip.strip_gpio_num = gpio;
    strip.max_leds = 1;
    strip.led_model = LED_MODEL_WS2812;
    strip.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    led_strip_rmt_config_t rmt{};
    rmt.clk_src = RMT_CLK_SRC_DEFAULT;
    rmt.resolution_hz = 10U * 1000U * 1000U;
    const esp_err_t err = led_strip_new_rmt_device(&strip, &rmt, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "LED on GPIO%d did not start: %s", gpio, esp_err_to_name(err));
        s_strip = nullptr;
        return false;
    }
    s_pin = gpio;
    return set(s_rgb[0], s_rgb[1], s_rgb[2]);
}

}  // namespace

bool begin(int gpio) {
    ESP_LOGI(kTag, "RGB LED on GPIO%d (`cfg -set led_pin, 38` for a DevKitC-1 v1.1)", gpio);
    return start(gpio);
}

bool set_pin(int gpio) { return start(gpio); }

int pin() { return s_pin; }

bool set(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    s_rgb[0] = r;
    s_rgb[1] = g;
    s_rgb[2] = b;
    if (s_strip == nullptr) return false;
    if (r == 0U && g == 0U && b == 0U) return led_strip_clear(s_strip) == ESP_OK;
    return led_strip_set_pixel(s_strip, 0, r, g, b) == ESP_OK && led_strip_refresh(s_strip) == ESP_OK;
}

std::uint8_t red() { return s_rgb[0]; }
std::uint8_t green() { return s_rgb[1]; }
std::uint8_t blue() { return s_rgb[2]; }

}  // namespace rgb_led
