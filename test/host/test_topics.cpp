// Topic declarations -> catalogue -> published samples, through a real
// btp::StaticNode, decoded back with btp::SampleReader.

#include "protocol/TopicRegistry.h"

#include <btp/codec.hpp>
#include <btp/telemetry.hpp>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"

using btpdev::Topics;
using btpdev::TopicRegistry;

namespace {

constexpr std::uint32_t kSelf = 0x11223344U;
constexpr std::uint32_t kPeer = 0x55667788U;

struct CaptureConfig : btp::NodeConfig {
    std::vector<std::vector<std::uint8_t>> frames;
    bool refuse = false;  // a full link: every frame refused
    CaptureConfig() {
        source_id = kSelf;
        boot_id = 1U;
        transport = btp::kSerialTransport;
    }
    bool send(const std::uint8_t* frame, std::size_t size) override {
        if (refuse) return false;
        frames.emplace_back(frame, frame + size);
        return true;
    }
};

using TestNode = btp::StaticNode<2U, 600U, 1024U, 2048U, 8U, 48U, 1536U, 8U, 2U, 256U, 4U>;

struct Rig {
    CaptureConfig config;
    TestNode node{config};
    TopicRegistry registry;
    btpdev::ErrorInfo error;

    esp_err_t install(Topics& topics, std::size_t max_topics = 8U) {
        const esp_err_t err = registry.install(topics, node, node.catalog(), max_topics, error);
        if (err == ESP_OK) {
            node.serve_catalog(static_cast<std::uint8_t>(btp::Role::Producer));
            node.begin();
        }
        return err;
    }

    bool subscribe(std::uint16_t topic_id, std::uint32_t rate_millihz = 10000U) {
        btp::Header header{};
        header.type = btp::MessageType::Control;
        header.source_id = kPeer;
        header.boot_id = 1U;
        header.sequence = 1U;
        btp::Subscribe request{};
        request.target_source_id = kSelf;
        request.target_boot_id = 1U;
        request.topic_id = topic_id;
        request.requested_rate_millihz = rate_millihz;
        request.requested_lease_ms = 60000U;
        btp::SubscribeResult result{};
        node.subscriptions()->handle_subscribe(node.catalog(), header, request, 0U, &result);
        return result.status == static_cast<std::uint8_t>(btp::ResultStatus::Success);
    }

    // Values of the telemetry samples captured so far for `topic_id`.
    std::vector<std::vector<double>> samples(std::uint16_t topic_id) {
        std::vector<std::vector<double>> out;
        const btp::CatalogTopic* topic = node.catalog().topic(topic_id);
        if (topic == nullptr) return out;
        for (const auto& frame : config.frames) {
            btp::DecodedFrame decoded{};
            if (btp::decode(frame.data(), frame.size(), btp::kSerialTransport, &decoded) != btp::Error::Ok ||
                decoded.header.type != btp::MessageType::Telemetry || decoded.header.object_id != topic_id) {
                continue;
            }
            btp::SampleReader reader(decoded.payload.data, decoded.payload.size, topic->fields,
                                     topic->field_count, topic->encoding);
            std::vector<double> values;
            btp::SampleValue value{};
            while (reader.next(&value) == btp::SampleStep::Item) {
                values.push_back(value.is_null ? -9999.0 : value.f64(0));
            }
            if (reader.finish() == btp::MessageError::Ok) out.push_back(values);
        }
        return out;
    }

    // Every value of every field (arrays expanded), per captured sample.
    std::vector<std::vector<double>> all_values(std::uint16_t topic_id) {
        std::vector<std::vector<double>> out;
        const btp::CatalogTopic* topic = node.catalog().topic(topic_id);
        if (topic == nullptr) return out;
        for (const auto& frame : config.frames) {
            btp::DecodedFrame decoded{};
            if (btp::decode(frame.data(), frame.size(), btp::kSerialTransport, &decoded) != btp::Error::Ok ||
                decoded.header.type != btp::MessageType::Telemetry || decoded.header.object_id != topic_id) {
                continue;
            }
            btp::SampleReader reader(decoded.payload.data, decoded.payload.size, topic->fields,
                                     topic->field_count, topic->encoding);
            std::vector<double> values;
            btp::SampleValue value{};
            while (reader.next(&value) == btp::SampleStep::Item) {
                for (std::uint16_t i = 0U; i < value.count; ++i) values.push_back(value.f64(i));
            }
            if (reader.finish() == btp::MessageError::Ok) out.push_back(values);
        }
        return out;
    }

    std::vector<std::string> texts(std::uint16_t topic_id) {
        std::vector<std::string> out;
        for (const auto& frame : config.frames) {
            btp::DecodedFrame decoded{};
            if (btp::decode(frame.data(), frame.size(), btp::kSerialTransport, &decoded) != btp::Error::Ok ||
                decoded.header.object_id != topic_id || decoded.payload.size < 2U) {
                continue;
            }
            out.emplace_back(reinterpret_cast<const char*>(decoded.payload.data) + 2, decoded.payload.size - 2U);
        }
        return out;
    }
};

float temperature = 21.5f;
float humidity = 40.0f;
std::int32_t counter = 7;
bool pump_on = true;

void bound_variables_become_samples() {
    Rig rig;
    Topics topics;
    topics.topic("weather", btpdev::Hz(10))
        .f32("temperature", &temperature, "C", -40, 85)
        .f32("humidity", &humidity, "%")
        .i32("counter", &counter)
        .boolean("pump", &pump_on);
    CHECK(rig.install(topics) == ESP_OK);
    CHECK(rig.error.ok());

    const btp::CatalogTopic* topic = rig.node.catalog().topic(Topics::kFirstTopicId);
    CHECK(topic != nullptr);
    CHECK(topic != nullptr && topic->field_count == 4U);
    CHECK(topic != nullptr && topic->max_rate_millihz == 10000U);

    // Nobody subscribed: nothing goes out.
    rig.node.routine(1000U);
    CHECK(rig.samples(Topics::kFirstTopicId).empty());

    CHECK(rig.subscribe(Topics::kFirstTopicId));
    rig.node.routine(2000U);
    auto samples = rig.samples(Topics::kFirstTopicId);
    CHECK(samples.size() == 1U);
    if (samples.size() == 1U) {
        CHECK(samples[0].size() == 4U);
        CHECK(samples[0][0] == 21.5 && samples[0][1] == 40.0 && samples[0][2] == 7.0 && samples[0][3] == 1.0);
    }

    // The variable is read at publish time.
    temperature = 30.0f;
    rig.node.routine(3000U);
    samples = rig.samples(Topics::kFirstTopicId);
    CHECK(samples.size() == 2U && samples.back()[0] == 30.0);
}

void fill_functions_write_the_sample() {
    Rig rig;
    Topics topics;
    topics.topic("power", btpdev::Hz(5)).f32("volts", "V").f32("amps", "A").fill([](btpdev::Sample& s) {
        s.put("volts", 12.5).put("amps", 0.25f);
    });
    CHECK(rig.install(topics) == ESP_OK);
    CHECK(rig.subscribe(Topics::kFirstTopicId));
    rig.node.routine(1000U);
    const auto samples = rig.samples(Topics::kFirstTopicId);
    CHECK(samples.size() == 1U && samples[0].size() == 2U && samples[0][0] == 12.5 && samples[0][1] == 0.25);
}

void ranges_reach_the_catalogue_for_any_type() {
    Rig rig;
    Topics topics;
    topics.topic("motors", btpdev::Hz(5))
        .u16("current", "counts").range(0, 4095)
        .i8("pwm", "%").range(-100, 100)
        .u8("mode")
        .fill([](btpdev::Sample& s) { s.put("current", 1234).put("pwm", -50).put("mode", 3); });
    CHECK(rig.install(topics) == ESP_OK);
    const btp::CatalogTopic* topic = rig.node.catalog().topic(Topics::kFirstTopicId);
    CHECK(topic != nullptr && topic->field_count == 3U);
    if (topic != nullptr) {
        CHECK(rig.node.catalog().field_min(*topic, 0) == 0.0 && rig.node.catalog().field_max(*topic, 0) == 4095.0);
        CHECK(rig.node.catalog().field_min(*topic, 1) == -100.0 && rig.node.catalog().field_max(*topic, 1) == 100.0);
        CHECK(std::isnan(rig.node.catalog().field_min(*topic, 2)));
    }
    CHECK(rig.subscribe(Topics::kFirstTopicId));
    rig.node.routine(1000U);
    const auto samples = rig.samples(Topics::kFirstTopicId);
    CHECK(samples.size() == 1U && samples[0].size() == 3U && samples[0][0] == 1234.0 &&
          samples[0][1] == -50.0 && samples[0][2] == 3.0);
}

void a_wrong_fill_drops_the_sample() {
    Rig rig;
    Topics topics;
    topics.topic("power", btpdev::Hz(5)).f32("volts", "V").f32("amps", "A").fill([](btpdev::Sample& s) {
        s.put("amps", 1.0);  // out of order: the writer refuses, nothing is sent
    });
    CHECK(rig.install(topics) == ESP_OK);
    CHECK(rig.subscribe(Topics::kFirstTopicId));
    rig.node.routine(1000U);
    CHECK(rig.samples(Topics::kFirstTopicId).empty());
}

void text_topics_publish_utf8() {
    Rig rig;
    Topics topics;
    topics.text("status", btpdev::Hz(1), [](btpdev::Text& out) { out.printf("uptime %us", 42U); });
    CHECK(rig.install(topics) == ESP_OK);
    rig.registry.publish_pending(rig.node, 1000U);
    CHECK(rig.texts(Topics::kFirstTopicId).empty());  // not subscribed
    CHECK(rig.subscribe(Topics::kFirstTopicId));
    rig.registry.publish_pending(rig.node, 2000U);
    const auto texts = rig.texts(Topics::kFirstTopicId);
    CHECK(texts.size() == 1U && texts[0] == "uptime 42s");
}

void event_publish_only_reaches_subscribers() {
    Rig rig;
    Topics topics;
    topics.topic("alarm", btpdev::OnEvent).boolean("door", &pump_on);
    CHECK(rig.install(topics) == ESP_OK);
    CHECK(rig.registry.request_publish("alarm"));
    CHECK(!rig.registry.request_publish("nope"));
    rig.registry.publish_pending(rig.node, 10U);
    CHECK(rig.samples(Topics::kFirstTopicId).empty());  // nobody listening

    CHECK(!rig.registry.subscribed("alarm"));
    CHECK(rig.subscribe(Topics::kFirstTopicId, 1000U));
    CHECK(!rig.registry.subscribed("alarm"));  // seen on the next pass
    rig.registry.request_publish("alarm");
    rig.registry.publish_pending(rig.node, 20U);
    CHECK(rig.samples(Topics::kFirstTopicId).size() == 1U);
    CHECK(rig.registry.subscribed("alarm") && !rig.registry.subscribed("nope"));
    rig.registry.publish_pending(rig.node, 30U);  // the request was consumed
    CHECK(rig.samples(Topics::kFirstTopicId).size() == 1U);
}

void handles_reach_the_same_topics_as_names() {
    Rig rig;
    Topics topics;
    const btpdev::Topic temp = topics.topic("temp", btpdev::Hz(1)).f32("x", &temperature);
    btpdev::Topic alarm = topics.topic("alarm", btpdev::OnEvent).boolean("door", &pump_on);
    const btpdev::Topic audio = topics.stream("audio", 4U, "counts", 2U).handle();
    CHECK(temp.valid() && temp.index == 0U && alarm.index == 1U && audio.index == 2U);
    CHECK(rig.install(topics) == ESP_OK);
    CHECK(rig.registry.handle_of("alarm").index == alarm.index);
    CHECK(!rig.registry.handle_of("nope").valid());

    // An invalid handle, or one past the declarations, does nothing.
    CHECK(!rig.registry.request_publish(btpdev::Topic{}));
    CHECK(!rig.registry.request_publish(btpdev::Topic{7U}));
    CHECK(!rig.registry.subscribed(btpdev::Topic{7U}));
    const std::int16_t block[] = {1, 2};
    CHECK(!rig.registry.write_stream(temp, block, 2U, 8000U, 1U));  // not a stream
    CHECK(!rig.registry.request_publish(audio));                    // not an event topic

    const std::uint16_t alarm_id = static_cast<std::uint16_t>(Topics::kFirstTopicId + alarm.index);
    const std::uint16_t audio_id = static_cast<std::uint16_t>(Topics::kFirstTopicId + audio.index);
    CHECK(rig.subscribe(alarm_id, 1000U) && rig.subscribe(audio_id, 1000U));
    rig.registry.publish_pending(rig.node, 10U);  // sees the subscribers
    CHECK(rig.registry.subscribed(alarm) && !rig.registry.subscribed(temp));
    CHECK(rig.registry.request_publish(alarm));
    CHECK(rig.registry.write_stream(audio, block, 2U, 8000U, 20U));
    rig.registry.publish_pending(rig.node, 20U);
    CHECK(rig.samples(alarm_id).size() == 1U);
    const auto blocks = rig.all_values(audio_id);
    CHECK(blocks.size() == 1U && (blocks[0] == std::vector<double>{0, 8000, 1, 2}));  // seq 0
}

void ids_are_automatic_unless_pinned() {
    Rig rig;
    Topics topics;
    topics.topic("a", btpdev::Hz(1)).f32("x", &temperature);
    topics.topic("b", btpdev::Hz(1)).f32("x", &temperature).id(0x0200);
    topics.topic("c", btpdev::Hz(1)).f32("x", &temperature);
    CHECK(rig.install(topics) == ESP_OK);
    CHECK(rig.registry.id_of("a") == Topics::kFirstTopicId);
    CHECK(rig.registry.id_of("b") == 0x0200);
    CHECK(rig.registry.id_of("c") == Topics::kFirstTopicId + 2U);
    CHECK(rig.registry.id_of("zzz") == 0U);
}

esp_err_t install_error(Topics& topics, std::string& message, std::size_t max_topics = 8U) {
    Rig rig;
    const esp_err_t err = rig.install(topics, max_topics);
    message = rig.error.message;
    return err;
}

void mistakes_are_reported_by_name() {
    std::string message;
    {
        Topics t;
        t.topic("weather", btpdev::Hz(1)).f32("x", &temperature);
        t.topic("weather", btpdev::Hz(1)).f32("y", &temperature);
        CHECK(install_error(t, message) == BTPDEV_ERR_BAD_TOPIC);
        CHECK_CONTAINS(message.c_str(), "\"weather\" is declared twice");
    }
    {
        Topics t;
        t.topic("empty", btpdev::Hz(1));
        CHECK(install_error(t, message) == BTPDEV_ERR_BAD_TOPIC);
        CHECK_CONTAINS(message.c_str(), "no fields");
    }
    {
        Topics t;
        t.topic("w", btpdev::Hz(1)).f32("x", &temperature).f32("x", &humidity);
        CHECK(install_error(t, message) == BTPDEV_ERR_BAD_TOPIC);
        CHECK_CONTAINS(message.c_str(), "field \"x\" is declared twice");
    }
    {
        Topics t;
        t.topic("w", btpdev::Hz(1)).f32("x");  // no variable, no fill
        CHECK(install_error(t, message) == BTPDEV_ERR_BAD_TOPIC);
        CHECK_CONTAINS(message.c_str(), "no .fill()");
    }
    {
        Topics t;
        t.topic("w", btpdev::Hz(1)).f32("x", &temperature).fill([](btpdev::Sample&) {});
        CHECK(install_error(t, message) == BTPDEV_ERR_BAD_TOPIC);
        CHECK_CONTAINS(message.c_str(), "one or the other");
    }
    {
        Topics t;
        t.topic("a", btpdev::Hz(1)).f32("x", &temperature);
        t.topic("b", btpdev::Hz(1)).f32("x", &temperature);
        CHECK(install_error(t, message, 1U) == BTPDEV_ERR_BAD_TOPIC);
        CHECK_CONTAINS(message.c_str(), "larger preset");
    }
}

void streams_send_every_block_in_order() {
    Rig rig;
    Topics topics;
    topics.stream("audio", 4U, "counts", 2U).range(-2048, 2047);
    CHECK(rig.install(topics) == ESP_OK);
    const btp::CatalogTopic* topic = rig.node.catalog().topic(Topics::kFirstTopicId);
    CHECK(topic != nullptr && topic->field_count == 3U);
    if (topic != nullptr) {
        CHECK((topic->fields[2].flags & btp::kFieldVariableCount) != 0U);
        CHECK(topic->fields[2].max_element_count == 4U);
        CHECK(rig.node.catalog().field_min(*topic, 2) == -2048.0 && rig.node.catalog().field_max(*topic, 2) == 2047.0);
    }
    CHECK(!rig.registry.request_publish("audio"));  // not an event topic

    const std::int16_t a[] = {1, -2, 3};
    const std::int16_t b[] = {-2048, 2047, 0, 5};
    const std::int16_t c[] = {7};
    CHECK(!rig.registry.write_stream("audio", a, 3U, 8000U, 100U));  // nobody subscribed (seq 0)
    CHECK(rig.subscribe(Topics::kFirstTopicId, 1000U));
    rig.registry.publish_pending(rig.node, 10U);                      // sees the subscriber
    CHECK(rig.registry.write_stream("audio", a, 3U, 8000U, 200U));    // seq 1
    CHECK(rig.registry.write_stream("audio", b, 4U, 8000U, 300U));    // seq 2
    CHECK(!rig.registry.write_stream("audio", c, 1U, 8000U, 400U));   // queue of 2 full (seq 3)
    CHECK(!rig.registry.write_stream("audio", b, 5U, 8000U, 500U));   // over max_samples
    CHECK(!rig.registry.write_stream("nope", c, 1U, 8000U, 500U));
    CHECK(rig.registry.stream_stats(0).dropped == 1U && rig.registry.stream_stats(0).queued == 2U);

    // A refusing link keeps the blocks queued, in order.
    rig.config.refuse = true;
    rig.registry.publish_pending(rig.node, 20U);
    CHECK(rig.registry.stream_stats(0).queued == 2U && rig.registry.stream_stats(0).sent == 0U);
    rig.config.refuse = false;
    rig.registry.publish_pending(rig.node, 30U);
    const auto blocks = rig.all_values(Topics::kFirstTopicId);
    CHECK(blocks.size() == 2U);
    if (blocks.size() == 2U) {
        CHECK((blocks[0] == std::vector<double>{1, 8000, 1, -2, 3}));
        CHECK((blocks[1] == std::vector<double>{2, 8000, -2048, 2047, 0, 5}));
    }
    CHECK(rig.registry.stream_stats(0).sent == 2U && rig.registry.stream_stats(0).queued == 0U);
    CHECK(std::strcmp(rig.registry.stream_stats(0).name, "audio") == 0);

    // The next block carries seq 5: the receiver sees the two that never left.
    CHECK(rig.registry.write_stream("audio", c, 1U, 16000U, 600U));
    rig.registry.publish_pending(rig.node, 40U);
    const auto more = rig.all_values(Topics::kFirstTopicId);
    CHECK(more.size() == 3U && (more.back() == std::vector<double>{5, 16000, 7}));
}

void streams_must_fit_one_frame() {
    std::string message;
    {
        Topics t;
        t.stream("big", 3000U);
        CHECK(install_error(t, message) == BTPDEV_ERR_BAD_TOPIC);
        CHECK_CONTAINS(message.c_str(), "at most 2022 samples");
    }
    {
        Topics t;
        t.stream("s", 16U).f32("extra", &temperature);
        CHECK(install_error(t, message) == BTPDEV_ERR_BAD_TOPIC);
        CHECK_CONTAINS(message.c_str(), "fields are fixed");
    }
}

void bytes_topics_publish_an_opaque_body() {
    Rig rig;
    Topics topics;
    std::vector<std::uint8_t> body;  // empty: nothing new
    int calls = 0;
    topics
        .bytes("camera.matrix", btpdev::Hz(10), 8U,
               [&](std::uint8_t* out, std::size_t capacity) -> std::size_t {
                   ++calls;
                   if (body.empty() || body.size() > capacity) return 0U;
                   std::memcpy(out, body.data(), body.size());
                   const std::size_t size = body.size();
                   body.clear();
                   return size;
               })
        .id(0x0202)
        .version(2);
    CHECK(rig.install(topics) == ESP_OK);
    const btp::CatalogTopic* topic = rig.node.catalog().topic(0x0202);
    CHECK(topic != nullptr);
    if (topic != nullptr) {
        CHECK(topic->field_count == 0U);
        CHECK(topic->encoding == static_cast<std::uint8_t>(btp::TelemetryEncoding::OpaqueBytes));
        CHECK(topic->schema_version == 2U);
        CHECK(topic->max_rate_millihz == 10000U);
    }

    body = {1, 2, 3};
    rig.registry.publish_pending(rig.node, 1000U);
    CHECK(calls == 0 && rig.texts(0x0202).empty());  // not subscribed

    CHECK(rig.subscribe(0x0202));
    rig.registry.publish_pending(rig.node, 2000U);
    std::vector<std::string> sent = rig.texts(0x0202);  // the body after schema_version
    CHECK(sent.size() == 1U && sent[0] == std::string("\x01\x02\x03", 3));
    CHECK(!rig.config.frames.empty() && rig.config.frames.back().size() > 0U);

    // Nothing new: nothing sent, and the topic stays due -- asked again on
    // the very next pass, and sent as soon as there is a body.
    rig.registry.publish_pending(rig.node, 2200U);
    const int asked = calls;
    rig.registry.publish_pending(rig.node, 2201U);
    CHECK(calls == asked + 1);
    CHECK(rig.texts(0x0202).size() == 1U);
    body = {9};
    rig.registry.publish_pending(rig.node, 2202U);
    sent = rig.texts(0x0202);
    CHECK(sent.size() == 2U && sent[1] == std::string("\x09", 1));

    // The schema_version prefix is the declared one.
    btp::DecodedFrame decoded{};
    CHECK(btp::decode(rig.config.frames.back().data(), rig.config.frames.back().size(), btp::kSerialTransport,
                      &decoded) == btp::Error::Ok);
    CHECK(decoded.payload.size == 3U && decoded.payload.data[0] == 2U && decoded.payload.data[1] == 0U);
}

void bytes_topics_are_checked() {
    std::string message;
    {
        Topics t;
        t.bytes("img", btpdev::Hz(1), 4000U, [](std::uint8_t*, std::size_t) -> std::size_t { return 0U; });
        CHECK(install_error(t, message) == BTPDEV_ERR_BAD_TOPIC);
        CHECK_CONTAINS(message.c_str(), "max_size must be 1..2030");
    }
    {
        Topics t;
        t.bytes("img", btpdev::Hz(1), 16U, [](std::uint8_t*, std::size_t) -> std::size_t { return 0U; })
            .u8("x", "");
        CHECK(install_error(t, message) == BTPDEV_ERR_BAD_TOPIC);
        CHECK_CONTAINS(message.c_str(), "cannot have fields");
    }
    {
        Topics t;
        t.bytes("img", btpdev::Hz(1), 16U, nullptr);
        CHECK(install_error(t, message) == BTPDEV_ERR_BAD_TOPIC);
        CHECK_CONTAINS(message.c_str(), "has no function");
    }
}

void text_topics_carry_their_version() {
    Rig rig;
    Topics topics;
    topics.text("status", btpdev::Hz(1), [](btpdev::Text& out) { out.append("ok"); }).version(3);
    CHECK(rig.install(topics) == ESP_OK);
    const btp::CatalogTopic* topic = rig.node.catalog().topic(Topics::kFirstTopicId);
    CHECK(topic != nullptr && topic->schema_version == 3U);
    CHECK(rig.subscribe(Topics::kFirstTopicId));
    rig.registry.publish_pending(rig.node, 1000U);
    CHECK(!rig.config.frames.empty());
    if (!rig.config.frames.empty()) {
        btp::DecodedFrame decoded{};
        CHECK(btp::decode(rig.config.frames.back().data(), rig.config.frames.back().size(),
                          btp::kSerialTransport, &decoded) == btp::Error::Ok);
        CHECK(decoded.payload.size == 4U && decoded.payload.data[0] == 3U);
    }
}

void text_buffer_is_bounded() {
    btpdev::Text text;
    const std::string big(3000, 'x');
    text.append(big.c_str());
    CHECK(text.size() == btpdev::Text::kCapacity);
    text.printf("more");
    CHECK(text.size() == btpdev::Text::kCapacity);
}

}  // namespace

int main() {
    bound_variables_become_samples();
    fill_functions_write_the_sample();
    ranges_reach_the_catalogue_for_any_type();
    a_wrong_fill_drops_the_sample();
    text_topics_publish_utf8();
    event_publish_only_reaches_subscribers();
    handles_reach_the_same_topics_as_names();
    ids_are_automatic_unless_pinned();
    mistakes_are_reported_by_name();
    streams_send_every_block_in_order();
    streams_must_fit_one_frame();
    bytes_topics_publish_an_opaque_body();
    bytes_topics_are_checked();
    text_topics_carry_their_version();
    text_buffer_is_bounded();
    return test_result("test_topics");
}
