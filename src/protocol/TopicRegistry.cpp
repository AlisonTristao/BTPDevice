#include "protocol/TopicRegistry.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>
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

TopicDecl& Topics::stream(const char* name, std::uint16_t max_samples, const char* unit,
                          std::uint8_t queue_blocks) {
    topics_.push_back(TopicDecl(name, OnEvent));
    TopicDecl& decl = topics_.back();
    decl.is_stream_ = true;
    decl.stream_blocks_ = queue_blocks;
    decl.u32("seq").u32("rate", "Hz").i16("samples", unit);
    decl.fields_.back().max_count = max_samples;
    return decl;
}

// ---- TopicRegistry ------------------------------------------------------------

namespace {

constexpr const char* kTag = "btpdev.topic";

bool empty_name(const char* name) { return name == nullptr || name[0] == '\0'; }

void put_le16(std::uint8_t* out, std::uint16_t value) noexcept {
    out[0] = static_cast<std::uint8_t>(value);
    out[1] = static_cast<std::uint8_t>(value >> 8);
}

void put_le32(std::uint8_t* out, std::uint32_t value) noexcept {
    for (int i = 0; i < 4; ++i) out[i] = static_cast<std::uint8_t>(value >> (8 * i));
}

void put_le64(std::uint8_t* out, std::uint64_t value) noexcept {
    for (int i = 0; i < 8; ++i) out[i] = static_cast<std::uint8_t>(value >> (8 * i));
}

std::uint16_t get_le16(const std::uint8_t* in) noexcept {
    return static_cast<std::uint16_t>(in[0] | (in[1] << 8));
}

std::uint64_t get_le64(const std::uint8_t* in) noexcept {
    std::uint64_t value = 0U;
    for (int i = 7; i >= 0; --i) value = (value << 8) | in[i];
    return value;
}

// begin() runs on the caller's task -- app_main's 3.5 KB stack by default. A
// TopicBuilder buffers 32 FieldRecords on the stack, and the compiler would
// fold every catalogue helper into install()'s frame: each one gets its own
// frame instead, released before the next.
#if defined(__GNUC__)
#define BTPDEV_NOINLINE __attribute__((noinline))
#else
#define BTPDEV_NOINLINE
#endif

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

BTPDEV_NOINLINE btp::MessageError add_field_topic(btp::Catalog& catalog, std::uint16_t id, const char* name,
                                                  bool is_text, std::uint32_t rate_millihz,
                                                  const std::vector<FieldDecl>& fields) {
    btp::TopicBuilder builder =
        catalog.topic(id, TopicDecl::kSchemaVersion, name,
                      is_text ? btp::TelemetryEncoding::Utf8 : btp::TelemetryEncoding::PackedLe,
                      /*subscribable=*/true, rate_millihz);
    for (const FieldDecl& field : fields) add_field(builder, field);
    builder.default_rate(rate_millihz);
    return builder.end();
}

BTPDEV_NOINLINE btp::MessageError add_stream_topic(btp::Catalog& catalog, std::uint16_t id, const char* name,
                                                   const FieldDecl& seq, const FieldDecl& rate,
                                                   const FieldDecl& samples) {
    btp::FieldRecord fields[3] = {btp::u32(seq.name), btp::u32(rate.name, 1.0, rate.unit),
                                  btp::i16(samples.name, 1.0, samples.unit)};
    fields[2].flags = btp::kFieldVariableCount;
    fields[2].element_count = 0U;
    fields[2].max_element_count = samples.max_count;
    fields[2].min_value = samples.min_value;
    fields[2].max_value = samples.max_value;
    return catalog.add_topic(id, TopicDecl::kSchemaVersion, btp::TelemetryEncoding::PackedLe,
                             /*subscribable=*/true, /*max_rate_millihz=*/0U, name, fields, 3U);
}

}  // namespace

esp_err_t TopicRegistry::install(Topics& topics, btp::Node& node, btp::Catalog& catalog,
                                 std::size_t max_topics, ErrorInfo& error,
                                 std::size_t max_stream_payload) noexcept {
    topics_.topics_ = std::move(topics.topics_);
    topics.topics_.clear();
    streams_.clear();
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
        if (t.is_stream_) {
            const esp_err_t err = add_stream(t, i, catalog, max_stream_payload, error);
            if (err != ESP_OK) return err;
            continue;
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

        const btp::MessageError added =
            add_field_topic(catalog, t.id_, t.name_, t.is_text_, t.rate_.millihz, t.fields_);
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

esp_err_t TopicRegistry::add_stream(const TopicDecl& t, std::size_t index, btp::Catalog& catalog,
                                    std::size_t max_stream_payload, ErrorInfo& error) noexcept {
    // Topics::stream() declared seq, rate and samples; anything chained after
    // it would not be in the payload write_stream() builds.
    if (t.fields_.size() != 3U || t.fill_) {
        return fail(error, BTPDEV_ERR_BAD_TOPIC, kTag,
                    "stream \"%s\": its fields are fixed (seq, rate, samples), chain only .range()",
                    t.name_);
    }
    const FieldDecl& samples = t.fields_[2];
    if (samples.max_count == 0U || t.stream_blocks_ == 0U) {
        return fail(error, BTPDEV_ERR_BAD_TOPIC, kTag, "stream \"%s\": max_samples and queue_blocks must be > 0",
                    t.name_);
    }
    const std::size_t payload = kStreamHead + 2U * samples.max_count;
    if (payload > max_stream_payload) {
        return fail(error, BTPDEV_ERR_BAD_TOPIC, kTag,
                    "stream \"%s\": %u samples need %u bytes, one frame of this link holds %u: use at "
                    "most %u samples per block",
                    t.name_, static_cast<unsigned>(samples.max_count), static_cast<unsigned>(payload),
                    static_cast<unsigned>(max_stream_payload),
                    static_cast<unsigned>((max_stream_payload - kStreamHead) / 2U));
    }

    const btp::MessageError added =
        add_stream_topic(catalog, t.id_, t.name_, t.fields_[0], t.fields_[1], samples);
    if (added != btp::MessageError::Ok) {
        return fail(error, BTPDEV_ERR_BAD_TOPIC, kTag,
                    "stream \"%s\" does not fit the catalogue (%s): too many fields or names for the "
                    "memory preset",
                    t.name_, btp::message_error_string(added));
    }

    std::unique_ptr<Stream> stream(new (std::nothrow) Stream);
    if (stream == nullptr) {
        return fail(error, BTPDEV_ERR_MEMORY_BUDGET, kTag, "no memory for stream \"%s\"", t.name_);
    }
    stream->topic_index = index;
    stream->topic_id = t.id_;
    stream->max_samples = samples.max_count;
    stream->slot_bytes = kSlotHead + payload;
    stream->slots = t.stream_blocks_;
    const std::size_t bytes = stream->slot_bytes * stream->slots;
    stream->storage.reset(static_cast<std::uint8_t*>(std::malloc(bytes)));
    if (stream->storage == nullptr) {
        return fail(error, BTPDEV_ERR_MEMORY_BUDGET, kTag,
                    "no memory for stream \"%s\" (%u bytes in one block): use fewer queue_blocks or "
                    "max_samples",
                    t.name_, static_cast<unsigned>(bytes));
    }
    streams_.push_back(std::move(stream));
    return ESP_OK;
}

bool TopicRegistry::write_stream(const char* name, const std::int16_t* samples, std::size_t count,
                                 std::uint32_t rate_hz, std::uint64_t timestamp_us) noexcept {
    if (name == nullptr) return false;
    Stream* stream = nullptr;
    for (const auto& s : streams_) {
        if (std::strcmp(topics_.topics_[s->topic_index].name_, name) == 0) {
            stream = s.get();
            break;
        }
    }
    if (stream == nullptr) return false;
    const std::uint32_t seq = stream->seq.fetch_add(1U, std::memory_order_relaxed);
    if (samples == nullptr || count == 0U || count > stream->max_samples) return false;
    if (stream->topic_index >= kMaxEventTopics ||
        (subscribed_.load(std::memory_order_relaxed) & (1U << stream->topic_index)) == 0U) {
        return false;
    }
    const std::uint32_t head = stream->head.load(std::memory_order_relaxed);
    if (head - stream->tail.load(std::memory_order_acquire) >= stream->slots) {
        stream->dropped.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }

    // The slot holds the block already encoded as its PACKED_LE payload, so
    // the communication task sends it as is.
    std::uint8_t* slot = stream->storage.get() + (head % stream->slots) * stream->slot_bytes;
    const std::size_t payload = kStreamHead + 2U * count;
    put_le64(slot, timestamp_us);
    put_le16(slot + 8, static_cast<std::uint16_t>(payload));
    std::uint8_t* out = slot + kSlotHead;
    put_le16(out, TopicDecl::kSchemaVersion);
    put_le32(out + 2, seq);
    put_le32(out + 6, rate_hz);
    put_le16(out + 10, static_cast<std::uint16_t>(count));
    out += kStreamHead;
    for (std::size_t i = 0U; i < count; ++i) put_le16(out + 2U * i, static_cast<std::uint16_t>(samples[i]));
    stream->head.store(head + 1U, std::memory_order_release);
    return true;
}

TopicRegistry::StreamStats TopicRegistry::stream_stats(std::size_t i) const noexcept {
    StreamStats stats{"", 0U, 0U, 0U};
    if (i >= streams_.size()) return stats;
    const Stream& s = *streams_[i];
    stats.name = topics_.topics_[s.topic_index].name_;
    stats.sent = s.sent.load(std::memory_order_relaxed);
    stats.dropped = s.dropped.load(std::memory_order_relaxed);
    stats.queued = s.head.load(std::memory_order_acquire) - s.tail.load(std::memory_order_acquire);
    return stats;
}

void TopicRegistry::send_streams(btp::Node& node, btp::SubscriptionTable& subscriptions) noexcept {
    for (const auto& s : streams_) {
        Stream& stream = *s;
        if (subscriptions.subscriber_count(stream.topic_id) == 0U) {
            // Nobody is listening any more: what is queued is for nobody.
            stream.tail.store(stream.head.load(std::memory_order_acquire), std::memory_order_release);
            continue;
        }
        for (;;) {
            const std::uint32_t tail = stream.tail.load(std::memory_order_relaxed);
            if (tail == stream.head.load(std::memory_order_acquire)) break;
            const std::uint8_t* slot = stream.storage.get() + (tail % stream.slots) * stream.slot_bytes;
            // Refused (the link's send queue is full): the block stays at the
            // front and goes out on a later pass, in order.
            if (!node.send(btp::MessageType::Telemetry, stream.topic_id, slot + kSlotHead, get_le16(slot + 8),
                           get_le64(slot))) {
                break;
            }
            stream.sent.fetch_add(1U, std::memory_order_relaxed);
            stream.tail.store(tail + 1U, std::memory_order_release);
        }
    }
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
    if (i < 0 || topics_.topics_[static_cast<std::size_t>(i)].is_stream_) return false;
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

    send_streams(node, *subscriptions);

    const std::uint32_t requested = requested_.exchange(0U, std::memory_order_relaxed);
    for (std::size_t i = 0U; requested != 0U && i < list.size() && i < kMaxEventTopics; ++i) {
        if ((requested & (1U << i)) == 0U) continue;
        if (subscriptions->subscriber_count(list[i].id_) == 0U) continue;  // nobody listening
        publish_one(node, i, now_ms);
    }
}

}  // namespace btpdev
