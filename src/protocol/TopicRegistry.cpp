#include "protocol/TopicRegistry.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <utility>

namespace btpdev {

// ---- Sample / Text / Topics (the public declaration types) -----------------

Sample& Sample::put_number(const char* name, double value) noexcept {
    if (ok_ && static_cast<btp::NamedSampleWriter*>(writer_)->put(name, value) != btp::MessageError::Ok) {
        ok_ = false;
    }
    return *this;
}

Sample& Sample::put_bool(const char* name, bool value) noexcept {
    if (ok_ && static_cast<btp::NamedSampleWriter*>(writer_)->put_bool(name, value) != btp::MessageError::Ok) {
        ok_ = false;
    }
    return *this;
}

Sample& Sample::put_null(const char* name) noexcept {
    if (ok_ && static_cast<btp::NamedSampleWriter*>(writer_)->put_null(name) != btp::MessageError::Ok) {
        ok_ = false;
    }
    return *this;
}

Text& Text::printf(const char* format, ...) noexcept {
    if (format == nullptr || size_ >= kCapacity) return *this;
    va_list args;
    va_start(args, format);
    const int written = std::vsnprintf(buffer_ + size_, kCapacity + 1U - size_, format, args);
    va_end(args);
    if (written > 0) {
        size_ += static_cast<std::size_t>(written);
        if (size_ > kCapacity) size_ = kCapacity;  // cut, still terminated
    }
    return *this;
}

Text& Text::append(const char* text) noexcept {
    if (text == nullptr) return *this;
    const std::size_t length = std::strlen(text);
    const std::size_t take = length < kCapacity - size_ ? length : kCapacity - size_;
    std::memcpy(buffer_ + size_, text, take);
    size_ += take;
    buffer_[size_] = '\0';
    return *this;
}

TopicDecl& TopicDecl::nullable() noexcept {
    if (!fields_.empty()) fields_.back().nullable = true;
    return *this;
}

TopicDecl& TopicDecl::range(double min, double max) noexcept {
    if (!fields_.empty()) {
        fields_.back().min_value = min;
        fields_.back().max_value = max;
    }
    return *this;
}

TopicDecl& Topics::topic(const char* name, Rate rate) {
    topics_.push_back(TopicDecl(name, rate));
    return topics_.back();
}

TopicDecl& Topics::text(const char* name, Rate rate, TextFn function) {
    topics_.push_back(TopicDecl(name, rate));
    TopicDecl& decl = topics_.back();
    decl.text_ = std::move(function);
    decl.is_text_ = true;
    return decl;
}

// ---- TopicRegistry ------------------------------------------------------------

namespace {

constexpr const char* kTag = "btpdev.topic";

bool empty_name(const char* name) { return name == nullptr || name[0] == '\0'; }

void add_field(btp::TopicBuilder& builder, const FieldDecl& field) {
    switch (field.type) {
        case FieldType::F32: builder.f32(field.name, field.unit, field.nullable); break;
        case FieldType::F64: builder.f64(field.name, field.unit, field.nullable); break;
        case FieldType::I8: builder.i8(field.name, 1.0, field.unit, field.nullable); break;
        case FieldType::I16: builder.i16(field.name, 1.0, field.unit, field.nullable); break;
        case FieldType::I32: builder.i32(field.name, 1.0, field.unit, field.nullable); break;
        case FieldType::U8: builder.u8(field.name, 1.0, field.unit, field.nullable); break;
        case FieldType::U16: builder.u16(field.name, 1.0, field.unit, field.nullable); break;
        case FieldType::U32: builder.u32(field.name, 1.0, field.unit, field.nullable); break;
        case FieldType::Bool: builder.boolean(field.name, field.nullable); break;
    }
    if (!std::isnan(field.min_value) || !std::isnan(field.max_value)) {
        builder.range(field.min_value, field.max_value);
    }
}

}  // namespace

esp_err_t TopicRegistry::install(Topics& topics, btp::Node& node, btp::Catalog& catalog,
                                 std::size_t max_topics, ErrorInfo& error) noexcept {
    topics_.topics_ = std::move(topics.topics_);
    topics.topics_.clear();
    std::vector<TopicDecl>& list = topics_.topics_;

    if (list.size() > max_topics) {
        return fail(error, BTPDEV_ERR_BAD_TOPIC, kTag,
                    "%u topics declared, the memory preset holds %u: use a larger preset "
                    "(DeviceConfig::memory)",
                    static_cast<unsigned>(list.size()), static_cast<unsigned>(max_topics));
    }

    for (std::size_t i = 0U; i < list.size(); ++i) {
        TopicDecl& t = list[i];
        if (empty_name(t.name_)) {
            return fail(error, BTPDEV_ERR_BAD_TOPIC, kTag, "topic #%u has no name", static_cast<unsigned>(i + 1U));
        }
        if (t.id_ == 0U) t.id_ = static_cast<std::uint16_t>(Topics::kFirstTopicId + i);
        for (std::size_t j = 0U; j < i; ++j) {
            if (std::strcmp(list[j].name_, t.name_) == 0) {
                return fail(error, BTPDEV_ERR_BAD_TOPIC, kTag, "topic \"%s\" is declared twice", t.name_);
            }
            if (list[j].id_ == t.id_) {
                return fail(error, BTPDEV_ERR_BAD_TOPIC, kTag, "topics \"%s\" and \"%s\" have the same id 0x%04x",
                            list[j].name_, t.name_, static_cast<unsigned>(t.id_));
            }
        }
        if (t.is_text_) {
            if (!t.fields_.empty()) {
                return fail(error, BTPDEV_ERR_BAD_TOPIC, kTag, "text topic \"%s\" cannot have fields", t.name_);
            }
            if (!t.text_) {
                return fail(error, BTPDEV_ERR_BAD_TOPIC, kTag, "text topic \"%s\" has no text function", t.name_);
            }
        } else {
            if (t.fields_.empty()) {
                return fail(error, BTPDEV_ERR_BAD_TOPIC, kTag, "topic \"%s\" has no fields", t.name_);
            }
            for (std::size_t f = 0U; f < t.fields_.size(); ++f) {
                const FieldDecl& field = t.fields_[f];
                if (empty_name(field.name)) {
                    return fail(error, BTPDEV_ERR_BAD_TOPIC, kTag, "topic \"%s\": field #%u has no name",
                                t.name_, static_cast<unsigned>(f + 1U));
                }
                for (std::size_t g = 0U; g < f; ++g) {
                    if (std::strcmp(t.fields_[g].name, field.name) == 0) {
                        return fail(error, BTPDEV_ERR_BAD_TOPIC, kTag, "topic \"%s\": field \"%s\" is declared twice",
                                    t.name_, field.name);
                    }
                }
                if (!t.fill_ && field.variable == nullptr) {
                    return fail(error, BTPDEV_ERR_BAD_TOPIC, kTag,
                                "topic \"%s\": field \"%s\" has no variable and the topic has no .fill()",
                                t.name_, field.name);
                }
                if (t.fill_ && field.variable != nullptr) {
                    return fail(error, BTPDEV_ERR_BAD_TOPIC, kTag,
                                "topic \"%s\": field \"%s\" is bound to a variable but the topic uses "
                                ".fill(): use one or the other",
                                t.name_, field.name);
                }
            }
        }

        btp::TopicBuilder builder = catalog.topic(
            t.id_, TopicDecl::kSchemaVersion, t.name_,
            t.is_text_ ? btp::TelemetryEncoding::Utf8 : btp::TelemetryEncoding::PackedLe,
            /*subscribable=*/true, t.rate_.millihz);
        for (const FieldDecl& field : t.fields_) add_field(builder, field);
        builder.default_rate(t.rate_.millihz);
        const btp::MessageError added = builder.end();
        if (added != btp::MessageError::Ok) {
            return fail(error, BTPDEV_ERR_BAD_TOPIC, kTag,
                        "topic \"%s\" does not fit the catalogue (%s): too many fields or names for "
                        "the memory preset, or more than 32 fields in one topic",
                        t.name_, btp::message_error_string(added));
        }
        if (!t.is_text_ && !node.on_publish(t.id_, &TopicRegistry::fill_fields, &t)) {
            return fail(error, BTPDEV_ERR_BAD_TOPIC, kTag, "topic \"%s\": the publish registry is full", t.name_);
        }
    }
    return ESP_OK;
}

void TopicRegistry::fill_fields(void* context, btp::NamedSampleWriter& writer) {
    TopicDecl& topic = *static_cast<TopicDecl*>(context);
    Sample sample(&writer);
    if (topic.fill_) {
        topic.fill_(sample);
        return;
    }
    for (const FieldDecl& f : topic.fields_) {
        switch (f.type) {
            case FieldType::F32: sample.put(f.name, *static_cast<const float*>(f.variable)); break;
            case FieldType::F64: sample.put(f.name, *static_cast<const double*>(f.variable)); break;
            case FieldType::I8: sample.put(f.name, *static_cast<const std::int8_t*>(f.variable)); break;
            case FieldType::I16: sample.put(f.name, *static_cast<const std::int16_t*>(f.variable)); break;
            case FieldType::I32: sample.put(f.name, *static_cast<const std::int32_t*>(f.variable)); break;
            case FieldType::U8: sample.put(f.name, *static_cast<const std::uint8_t*>(f.variable)); break;
            case FieldType::U16: sample.put(f.name, *static_cast<const std::uint16_t*>(f.variable)); break;
            case FieldType::U32: sample.put(f.name, *static_cast<const std::uint32_t*>(f.variable)); break;
            case FieldType::Bool: sample.put(f.name, *static_cast<const bool*>(f.variable)); break;
        }
    }
}

std::uint16_t TopicRegistry::id_of(const char* name) const noexcept {
    if (name == nullptr) return 0U;
    for (const TopicDecl& t : topics_.topics_) {
        if (std::strcmp(t.name_, name) == 0) return t.id_;
    }
    return 0U;
}

int TopicRegistry::index_of(const char* name) const noexcept {
    if (name == nullptr) return -1;
    const std::vector<TopicDecl>& list = topics_.topics_;
    for (std::size_t i = 0U; i < list.size() && i < kMaxEventTopics; ++i) {
        if (std::strcmp(list[i].name_, name) == 0) return static_cast<int>(i);
    }
    return -1;
}

bool TopicRegistry::request_publish(const char* name) noexcept {
    const int i = index_of(name);
    if (i < 0) return false;
    requested_.fetch_or(1U << i, std::memory_order_relaxed);
    return true;
}

bool TopicRegistry::subscribed(const char* name) const noexcept {
    const int i = index_of(name);
    return i >= 0 && (subscribed_.load(std::memory_order_relaxed) & (1U << i)) != 0U;
}

bool TopicRegistry::publish_one(btp::Node& node, std::size_t index, std::uint64_t now_ms) noexcept {
    TopicDecl& t = topics_.topics_[index];
    const std::uint64_t timestamp_us = now_ms * 1000ULL;
    if (!t.is_text_) {
        return node.publish_named(t.id_, &TopicRegistry::fill_fields, &t, timestamp_us);
    }
    text_.clear();
    t.text_(text_);
    // Body-only UTF-8 topic: schema_version (u16 LE) then the text itself.
    text_payload_[0] = static_cast<std::uint8_t>(TopicDecl::kSchemaVersion & 0xFFU);
    text_payload_[1] = static_cast<std::uint8_t>(TopicDecl::kSchemaVersion >> 8);
    std::memcpy(text_payload_ + 2, text_.data(), text_.size());
    return node.send(btp::MessageType::Telemetry, t.id_, text_payload_, text_.size() + 2U, timestamp_us);
}

void TopicRegistry::publish_pending(btp::Node& node, std::uint64_t now_ms) noexcept {
    btp::SubscriptionTable* subscriptions = node.subscriptions();
    if (subscriptions == nullptr) return;
    std::vector<TopicDecl>& list = topics_.topics_;

    std::uint32_t subscribed = 0U;
    for (std::size_t i = 0U; i < list.size() && i < kMaxEventTopics; ++i) {
        if (subscriptions->subscriber_count(list[i].id_) != 0U) subscribed |= 1U << i;
    }
    subscribed_.store(subscribed, std::memory_order_relaxed);

    // Field topics that are due are the node's own job (on_publish); text
    // topics are not in its registry (no fields to write), so they are here.
    for (std::size_t i = 0U; i < list.size(); ++i) {
        if (!list[i].is_text_ || !subscriptions->due(list[i].id_, now_ms)) continue;
        if (publish_one(node, i, now_ms)) subscriptions->note_published(list[i].id_, now_ms);
    }

    const std::uint32_t requested = requested_.exchange(0U, std::memory_order_relaxed);
    for (std::size_t i = 0U; requested != 0U && i < list.size() && i < kMaxEventTopics; ++i) {
        if ((requested & (1U << i)) == 0U) continue;
        if (subscriptions->subscriber_count(list[i].id_) == 0U) continue;  // nobody listening
        publish_one(node, i, now_ms);
    }
}

}  // namespace btpdev
