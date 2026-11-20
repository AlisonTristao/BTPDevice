// Persistent settings (include/tracekit/settings.h, src/protocol/SettingsRegistry):
// declaration checks, loading saved values, text <-> value, reset, and what
// happens when a declaration changes under a saved value.

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "check.h"
#include "protocol/SettingsRegistry.h"

namespace {

using tracekit::Settings;
using tracekit::SettingsRegistry;
using Result = SettingsRegistry::Result;

class MapStore : public tracekit::SettingsStore {
public:
    bool load(const char* key, std::uint8_t* data, std::size_t* size) override {
        auto it = blobs.find(key);
        if (it == blobs.end() || it->second.size() > *size) return false;
        std::memcpy(data, it->second.data(), it->second.size());
        *size = it->second.size();
        return true;
    }
    bool save(const char* key, const std::uint8_t* data, std::size_t size) override {
        ++saves;
        if (refuse) return false;
        blobs[key] = std::vector<std::uint8_t>(data, data + size);
        return true;
    }
    bool erase(const char* key) override { return blobs.erase(key) != 0U; }

    std::map<std::string, std::vector<std::uint8_t>> blobs;
    int saves = 0;
    bool refuse = false;
};

struct App {
    float setpoint = -1.0f;
    std::uint32_t period = 0U;
    std::uint8_t pin = 0U;
    bool alarm = false;
    std::int32_t offset = 0;
    char site[16] = "x";
    int changes = 0;

    void declare(Settings& s) {
        s.f32("setpoint", &setpoint, 25.0f).range(0, 100).unit("C");
        s.u32("period_ms", &period, 500U).range(100, 60000);
        s.u8("led_pin", &pin, 48U).reboot();
        s.boolean("alarm", &alarm, true).on_change([this] { ++changes; });
        s.i32("offset", &offset, -5);
        s.text("site", site, sizeof(site), "bench");
    }
};

void defaults_then_saved_values() {
    MapStore store;
    tracekit::ErrorInfo error;
    {
        App app;
        Settings s;
        app.declare(s);
        SettingsRegistry reg;
        CHECK(reg.install(s, &store, error) == ESP_OK);
        CHECK(app.setpoint == 25.0f && app.period == 500U && app.pin == 48U && app.alarm && app.offset == -5);
        CHECK_STR_EQ(app.site, "bench");
        CHECK(store.blobs.empty());  // defaults are not written

        CHECK(reg.set("setpoint", "30.5") == Result::Ok);
        CHECK(app.setpoint == 30.5f);
        CHECK(reg.set("led_pin", "38") == Result::OkReboot);
        CHECK(reg.set("alarm", "off") == Result::Ok && !app.alarm && app.changes == 1);
        CHECK(reg.set("offset", " -12 ") == Result::Ok && app.offset == -12);
        CHECK(reg.set("site", "garden") == Result::Ok);
        CHECK_STR_EQ(app.site, "garden");
    }
    {
        App app;  // a reboot: new variables, same store
        Settings s;
        app.declare(s);
        SettingsRegistry reg;
        CHECK(reg.install(s, &store, error) == ESP_OK);
        CHECK(app.setpoint == 30.5f && app.pin == 38U && !app.alarm && app.offset == -12 && app.period == 500U);
        CHECK_STR_EQ(app.site, "garden");
        char text[32];
        CHECK(reg.get("setpoint", text, sizeof(text)));
        CHECK_STR_EQ(text, "30.5");
        CHECK(reg.get("alarm", text, sizeof(text)));
        CHECK_STR_EQ(text, "false");
    }
}

void bad_values_change_nothing() {
    MapStore store;
    tracekit::ErrorInfo error;
    App app;
    Settings s;
    app.declare(s);
    SettingsRegistry reg;
    CHECK(reg.install(s, &store, error) == ESP_OK);
    CHECK(reg.set("setpoint", "101") == Result::OutOfRange);
    CHECK(reg.set("setpoint", "abc") == Result::BadValue);
    CHECK(reg.set("setpoint", "") == Result::BadValue);
    CHECK(reg.set("period_ms", "99") == Result::OutOfRange);
    CHECK(reg.set("period_ms", "-1") == Result::OutOfRange);
    CHECK(reg.set("period_ms", "12x") == Result::BadValue);
    CHECK(reg.set("led_pin", "256") == Result::OutOfRange);  // the type's own limit
    CHECK(reg.set("alarm", "maybe") == Result::BadValue);
    CHECK(reg.set("site", "a name that is far too long") == Result::TooLong);
    CHECK(reg.set("nope", "1") == Result::UnknownName);
    CHECK(app.setpoint == 25.0f && app.period == 500U && app.pin == 48U && app.alarm);
    CHECK_STR_EQ(app.site, "bench");
    CHECK(store.saves == 0);

    char text[96];
    reg.explain(Result::OutOfRange, "period_ms", text, sizeof(text));
    CHECK_CONTAINS(text, "100..60000");
}

void reset_one_and_all() {
    MapStore store;
    tracekit::ErrorInfo error;
    App app;
    Settings s;
    app.declare(s);
    SettingsRegistry reg;
    CHECK(reg.install(s, &store, error) == ESP_OK);
    CHECK(reg.set("setpoint", "10") == Result::Ok);
    CHECK(reg.set("period_ms", "1000") == Result::Ok);
    CHECK(reg.reset("setpoint") == Result::Ok);
    CHECK(app.setpoint == 25.0f && app.period == 1000U);
    CHECK(store.blobs.count("setpoint") == 0U && store.blobs.count("period_ms") == 1U);
    CHECK(reg.reset("all") == Result::OkReboot);  // led_pin is a reboot setting
    CHECK(app.period == 500U && store.blobs.empty());
    CHECK(reg.reset("nope") == Result::UnknownName);
}

void changed_declarations_fall_back_to_the_default() {
    MapStore store;
    tracekit::ErrorInfo error;
    {
        App app;
        Settings s;
        app.declare(s);
        SettingsRegistry reg;
        CHECK(reg.install(s, &store, error) == ESP_OK);
        CHECK(reg.set("period_ms", "50000") == Result::Ok);
        CHECK(reg.set("setpoint", "80") == Result::Ok);
    }
    // The next firmware: period_ms is now a u16 and setpoint's range shrank.
    std::uint16_t period = 0U;
    float setpoint = 0.0f;
    Settings s;
    s.u16("period_ms", &period, 700U);
    s.f32("setpoint", &setpoint, 20.0f).range(0, 50);
    SettingsRegistry reg;
    std::vector<std::string> warnings;
    CHECK(reg.install(s, &store, error, [&](const char* w) { warnings.emplace_back(w); }) == ESP_OK);
    CHECK(period == 700U && setpoint == 20.0f);
    CHECK(warnings.size() == 2U);
    CHECK(store.blobs.empty());  // the stale values are gone
}

void declarations_are_checked() {
    tracekit::ErrorInfo error;
    float f = 0.0f;
    std::uint8_t u = 0U;
    char t[4];
    {
        Settings s;
        s.f32("", &f, 1.0f);
        SettingsRegistry reg;
        CHECK(reg.install(s, nullptr, error) == TRACEKIT_ERR_CONFIG);
    }
    {
        Settings s;
        s.f32("a_name_of_16_chr", &f, 1.0f);  // 16 > an NVS key's 15
        SettingsRegistry reg;
        CHECK(reg.install(s, nullptr, error) == TRACEKIT_ERR_CONFIG);
    }
    {
        Settings s;
        s.f32("x", &f, 1.0f);
        s.u8("x", &u, 1U);
        SettingsRegistry reg;
        CHECK(reg.install(s, nullptr, error) == TRACEKIT_ERR_CONFIG);
        CHECK_CONTAINS(error.message, "twice");
    }
    {
        Settings s;
        s.f32("x", &f, 200.0f).range(0, 100);
        SettingsRegistry reg;
        CHECK(reg.install(s, nullptr, error) == TRACEKIT_ERR_CONFIG);
        CHECK_CONTAINS(error.message, "default");
    }
    {
        Settings s;
        s.u8("x", &u, 1U).range(0, 300);
        SettingsRegistry reg;
        CHECK(reg.install(s, nullptr, error) == TRACEKIT_ERR_CONFIG);
    }
    {
        Settings s;
        s.text("x", t, sizeof(t), "toolong");
        SettingsRegistry reg;
        CHECK(reg.install(s, nullptr, error) == TRACEKIT_ERR_CONFIG);
    }
    {
        Settings s;
        s.u8("all", &u, 1U);
        SettingsRegistry reg;
        CHECK(reg.install(s, nullptr, error) == TRACEKIT_ERR_CONFIG);
    }
    {
        Settings s;
        s.u8("has space", &u, 1U);
        SettingsRegistry reg;
        CHECK(reg.install(s, nullptr, error) == TRACEKIT_ERR_CONFIG);
    }
}

void a_refusing_store_is_reported() {
    MapStore store;
    store.refuse = true;
    tracekit::ErrorInfo error;
    App app;
    Settings s;
    app.declare(s);
    SettingsRegistry reg;
    CHECK(reg.install(s, &store, error) == ESP_OK);
    CHECK(reg.set("setpoint", "40") == Result::NotSaved);
    CHECK(app.setpoint == 40.0f);  // still applied in memory
}

void list_lines() {
    App app;
    Settings s;
    app.declare(s);
    SettingsRegistry reg;
    tracekit::ErrorInfo error;
    CHECK(reg.install(s, nullptr, error) == ESP_OK);
    CHECK(reg.size() == 6U);
    char line[160];
    reg.describe(0, line, sizeof(line));
    CHECK_CONTAINS(line, "setpoint");
    CHECK_CONTAINS(line, "25 C");
    CHECK_CONTAINS(line, "0..100");
    reg.describe(2, line, sizeof(line));
    CHECK_CONTAINS(line, "reboot");
    reg.describe(5, line, sizeof(line));
    CHECK_CONTAINS(line, "\"bench\"");
}

}  // namespace

int main() {
    defaults_then_saved_values();
    bad_values_change_nothing();
    reset_one_and_all();
    changed_declarations_fall_back_to_the_default();
    declarations_are_checked();
    a_refusing_store_is_reported();
    list_lines();
    return test_result("test_settings");
}
