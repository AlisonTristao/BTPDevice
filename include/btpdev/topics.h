#ifndef BTPDEV_TOPICS_H
#define BTPDEV_TOPICS_H

// Telemetry topics: what TraceView can plot. Declared once, in the
// application's btp_topics.cpp, and published by the library only while
// someone is subscribed, at the rate they asked for (up to the declared one).
//
//   void app_topics(btpdev::Topics& t) {
//       // fields bound to variables: nothing else to write
//       t.topic("weather", btpdev::Hz(1))
//        .f32("temperature", &temperature, "°C", -40, 85)
//        .f32("humidity", &humidity, "%", 0, 100);
//
//       // or a function that fills a consistent snapshot
//       t.topic("power", btpdev::Hz(0.2))
//        .f32("battery_v", "V")
//        .f32("solar_ma", "mA")
//        .fill([](btpdev::Sample& s) {
//            s.put("battery_v", read_battery());
//            s.put("solar_ma", read_solar());
//        });
//
//       // free text (TraceView's Text Board)
//       t.text("status", btpdev::Hz(0.5), [](btpdev::Text& out) {
//           out.printf("uptime %lus", uptime_s());
//       });
//   }
//
// Topic ids are given in declaration order from kFirstTopicId, field ids and
// order in declaration order. Names must be unique (topics per device, fields
// per topic). Every mistake is reported by begin() (BTPDEV_ERR_BAD_TOPIC)
// with the topic's name.
//
// A variable bound with a pointer is read on the communication task when a
// sample is due; a 4-byte aligned float/int is read atomically on the ESP32,
// but several fields that must be consistent with each other belong in a
// .fill() function.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <type_traits>
#include <vector>

namespace btpdev {

/// A publish rate, in millihertz. Hz(10), Hz(0.2), OnEvent.
struct Rate {
    std::uint32_t millihz;
};
constexpr Rate Hz(double hz) noexcept {
    return Rate{hz <= 0.0 ? 0U : static_cast<std::uint32_t>(hz * 1000.0 + 0.5)};
}
/// Not periodic: only sent by BtpDevice::publish() (still only to subscribers).
constexpr Rate OnEvent{0U};

enum class FieldType : std::uint8_t { F32, F64, I8, I16, I32, U8, U16, U32, Bool };

/// Handed to a .fill() function: put each field by name, in declaration order.
class Sample {
public:
    template <typename T>
    typename std::enable_if<std::is_arithmetic<T>::value && !std::is_same<T, bool>::value, Sample&>::type
    put(const char* name, T value) noexcept {
        return put_number(name, static_cast<double>(value));
    }
    Sample& put(const char* name, bool value) noexcept { return put_bool(name, value); }
    /// A field with no value this time (only for fields declared nullable).
    Sample& put_null(const char* name) noexcept;

    /// false once a put() failed (wrong name/order/type): the sample is dropped.
    bool ok() const noexcept { return ok_; }

    explicit Sample(void* writer) noexcept : writer_(writer) {}

private:
    Sample& put_number(const char* name, double value) noexcept;
    Sample& put_bool(const char* name, bool value) noexcept;
    void* writer_;  // btp::NamedSampleWriter
    bool ok_ = true;
};

/// Handed to a text topic's function: the UTF-8 text to publish.
class Text {
public:
    static constexpr std::size_t kCapacity = 1024U;
    Text& printf(const char* format, ...) noexcept
#if defined(__GNUC__)
        __attribute__((format(printf, 2, 3)))
#endif
        ;
    Text& append(const char* text) noexcept;
    std::size_t size() const noexcept { return size_; }
    const char* data() const noexcept { return buffer_; }
    void clear() noexcept {
        size_ = 0U;
        buffer_[0] = '\0';
    }

private:
    char buffer_[kCapacity + 1U] = {};
    std::size_t size_ = 0U;
};

using FillFn = std::function<void(Sample&)>;
using TextFn = std::function<void(Text&)>;

struct FieldDecl {
    const char* name;
    FieldType type;
    const void* variable;   // nullptr: filled by the topic's .fill()
    const char* unit;
    double min_value;       // NAN: no bound
    double max_value;
    bool nullable;
};

class Topics;

/// Returned by Topics::topic(): chain the fields.
class TopicDecl {
public:
    static constexpr std::uint16_t kSchemaVersion = 1U;

    TopicDecl& f32(const char* name, const float* variable, const char* unit = "",
                   double min = NAN, double max = NAN) noexcept {
        return field(name, FieldType::F32, variable, unit, min, max);
    }
    TopicDecl& f32(const char* name, const char* unit = "", double min = NAN, double max = NAN) noexcept {
        return field(name, FieldType::F32, nullptr, unit, min, max);
    }
    TopicDecl& f64(const char* name, const double* variable, const char* unit = "",
                   double min = NAN, double max = NAN) noexcept {
        return field(name, FieldType::F64, variable, unit, min, max);
    }
    TopicDecl& f64(const char* name, const char* unit = "", double min = NAN, double max = NAN) noexcept {
        return field(name, FieldType::F64, nullptr, unit, min, max);
    }
    TopicDecl& i32(const char* name, const std::int32_t* variable, const char* unit = "") noexcept {
        return field(name, FieldType::I32, variable, unit, NAN, NAN);
    }
    TopicDecl& i32(const char* name, const char* unit = "") noexcept {
        return field(name, FieldType::I32, nullptr, unit, NAN, NAN);
    }
    TopicDecl& u32(const char* name, const std::uint32_t* variable, const char* unit = "") noexcept {
        return field(name, FieldType::U32, variable, unit, NAN, NAN);
    }
    TopicDecl& u32(const char* name, const char* unit = "") noexcept {
        return field(name, FieldType::U32, nullptr, unit, NAN, NAN);
    }
    TopicDecl& i16(const char* name, const std::int16_t* variable, const char* unit = "") noexcept {
        return field(name, FieldType::I16, variable, unit, NAN, NAN);
    }
    TopicDecl& u16(const char* name, const std::uint16_t* variable, const char* unit = "") noexcept {
        return field(name, FieldType::U16, variable, unit, NAN, NAN);
    }
    TopicDecl& i8(const char* name, const std::int8_t* variable, const char* unit = "") noexcept {
        return field(name, FieldType::I8, variable, unit, NAN, NAN);
    }
    TopicDecl& u8(const char* name, const std::uint8_t* variable, const char* unit = "") noexcept {
        return field(name, FieldType::U8, variable, unit, NAN, NAN);
    }
    TopicDecl& boolean(const char* name, const bool* variable) noexcept {
        return field(name, FieldType::Bool, variable, "", NAN, NAN);
    }
    TopicDecl& boolean(const char* name) noexcept {
        return field(name, FieldType::Bool, nullptr, "", NAN, NAN);
    }
    /// Marks the field just added as nullable (Sample::put_null()).
    TopicDecl& nullable() noexcept;
    /// Fills every sample (instead of reading bound variables).
    TopicDecl& fill(FillFn function) noexcept {
        fill_ = std::move(function);
        return *this;
    }
    /// Pins the topic id (default: kFirstTopicId + declaration index).
    TopicDecl& id(std::uint16_t topic_id) noexcept {
        id_ = topic_id;
        return *this;
    }

    const char* name() const noexcept { return name_; }

private:
    friend class Topics;
    friend class TopicRegistry;
    TopicDecl(const char* name, Rate rate) : name_(name), rate_(rate) {}
    TopicDecl& field(const char* name, FieldType type, const void* variable, const char* unit,
                     double min, double max) noexcept {
        fields_.push_back(FieldDecl{name, type, variable, unit != nullptr ? unit : "", min, max, false});
        return *this;
    }

    const char* name_;
    Rate rate_;
    std::uint16_t id_ = 0U;  // 0 = automatic
    std::vector<FieldDecl> fields_;
    FillFn fill_;
    TextFn text_;
    bool is_text_ = false;
};

/// What the application's topics function receives.
class Topics {
public:
    static constexpr std::uint16_t kFirstTopicId = 0x0101U;

    TopicDecl& topic(const char* name, Rate rate);
    /// A topic whose sample is free UTF-8 text (no fields).
    TopicDecl& text(const char* name, Rate rate, TextFn function);

    std::size_t size() const noexcept { return topics_.size(); }

private:
    friend class TopicRegistry;
    // A deque would keep references stable; with a vector the returned
    // reference is only valid until the next topic() -- which is how the
    // chained style uses it.
    std::vector<TopicDecl> topics_;
};

}  // namespace btpdev

#endif  // BTPDEV_TOPICS_H
