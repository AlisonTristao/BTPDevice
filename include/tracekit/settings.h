#ifndef TRACEKIT_SETTINGS_H
#define TRACEKIT_SETTINGS_H

// Persistent settings: values the application needs to adjust in the field
// (a setpoint, a period, a pin) without recompiling. Declared once, bound to
// the application's own variables, kept in NVS (docs/PLANO.md D28):
//
//   void settings(tracekit::Settings& s) override {
//       s.f32("setpoint", &setpoint_, 25.0f).range(0, 100).unit("°C");
//       s.u32("period_ms", &period_ms_, 500).range(100, 60000);
//       s.u8("led_pin", &pin_, 48).reboot();          // used once, at boot
//       s.text("site", site_, sizeof(site_), "bench");
//       s.boolean("alarm", &alarm_, true)
//        .on_change([this] { update_alarm(); });     // react right away
//   }
//
// begin() loads the saved values into the variables BEFORE commands() and
// topics() run, so the application already sees them there. The terminal
// gets a `cfg` module:
//
//   cfg -list                      every setting: value, default, range
//   cfg -get setpoint
//   cfg -set setpoint, 30          checked against type and range, then saved
//   cfg -reset setpoint            back to the default (cfg -reset all)
//
// From the code, TraceKit::set_setting("setpoint", "30") does what
// `cfg -set` does, and save_setting("setpoint") stores a value the
// application changed in the variable itself.
//
// Names are NVS keys: 1..15 printable ASCII characters, unique. A value saved
// with another type (the declaration changed) or out of the current range is
// replaced by the default, with a warning. `btp -factory_reset yes` keeps the
// settings; `all` erases them.
//
// Variables up to 4 bytes are written whole, so another task reading them
// sees the old or the new value, never a mix. A text setting is copied into
// its buffer while the shell task runs the command: read it from your own
// task only if a torn read for a moment does not matter, or copy it in
// on_change().

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace tracekit {

enum class SettingType : std::uint8_t { Bool, U8, U16, U32, I32, F32, Text };

class SettingDecl {
public:
    /// Inclusive limits for a number (ignored for bool and text). The
    /// default must be inside them.
    SettingDecl& range(double min, double max) noexcept {
        min_ = min;
        max_ = max;
        return *this;
    }
    SettingDecl& unit(const char* unit) noexcept {
        unit_ = unit != nullptr ? unit : "";
        return *this;
    }
    /// One line shown by `cfg -list`.
    SettingDecl& help(const char* text) noexcept {
        help_ = text != nullptr ? text : "";
        return *this;
    }
    /// Read only at boot: `cfg -set` saves it and says a restart is needed.
    SettingDecl& reboot() noexcept {
        reboot_ = true;
        return *this;
    }
    /// Called after `cfg -set`, `cfg -reset` or set_setting() changed the
    /// variable (on the task that changed it: the shell task for commands).
    SettingDecl& on_change(std::function<void()> callback) {
        on_change_ = std::move(callback);
        return *this;
    }

private:
    friend class Settings;
    friend class SettingsRegistry;

    const char* name_ = nullptr;
    SettingType type_ = SettingType::U32;
    void* variable_ = nullptr;
    std::size_t text_size_ = 0U;      ///< Text: the buffer's size, NUL included
    double default_number_ = 0.0;     ///< every type but Text
    const char* default_text_ = "";   ///< Text
    double min_ = NAN;
    double max_ = NAN;
    const char* unit_ = "";
    const char* help_ = "";
    bool reboot_ = false;
    std::function<void()> on_change_;
};

class Settings {
public:
    SettingDecl& boolean(const char* name, bool* variable, bool fallback) {
        return add(name, SettingType::Bool, variable, fallback ? 1.0 : 0.0);
    }
    SettingDecl& u8(const char* name, std::uint8_t* variable, std::uint8_t fallback) {
        return add(name, SettingType::U8, variable, fallback);
    }
    SettingDecl& u16(const char* name, std::uint16_t* variable, std::uint16_t fallback) {
        return add(name, SettingType::U16, variable, fallback);
    }
    SettingDecl& u32(const char* name, std::uint32_t* variable, std::uint32_t fallback) {
        return add(name, SettingType::U32, variable, fallback);
    }
    SettingDecl& i32(const char* name, std::int32_t* variable, std::int32_t fallback) {
        return add(name, SettingType::I32, variable, fallback);
    }
    SettingDecl& f32(const char* name, float* variable, float fallback) {
        return add(name, SettingType::F32, variable, fallback);
    }
    /// `buffer` holds up to size - 1 characters (at most kMaxText).
    SettingDecl& text(const char* name, char* buffer, std::size_t size, const char* fallback) {
        SettingDecl& d = add(name, SettingType::Text, buffer, 0.0);
        d.text_size_ = size;
        d.default_text_ = fallback != nullptr ? fallback : "";
        return d;
    }

    static constexpr std::size_t kMaxName = 15U;  ///< an NVS key
    static constexpr std::size_t kMaxText = 64U;
    static constexpr std::size_t kMaxSettings = 32U;

private:
    friend class SettingsRegistry;

    SettingDecl& add(const char* name, SettingType type, void* variable, double fallback) {
        list_.emplace_back();
        SettingDecl& d = list_.back();
        d.name_ = name;
        d.type_ = type;
        d.variable_ = variable;
        d.default_number_ = fallback;
        return d;
    }

    std::vector<SettingDecl> list_;
};

}  // namespace tracekit

#endif  // TRACEKIT_SETTINGS_H
