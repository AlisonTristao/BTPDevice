#include <TraceKit.h>

#include <new>

#include <btp/version.hpp>
#include <tracekit/version.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "core/ConfigCheck.h"
#include "core/DeviceCore.h"
#include "core/LogTags.h"

// BTP 2.49 is the first release that is an ESP-IDF component on its own
// (docs/PLANO.md D15); 2.51 brings btp::NodeWith<> (the presets below the
// node) and the compile-time check of the NodeConfig has_X()/X() pairs.
static_assert(btp::kLibraryVersionMajor == 2U && btp::kLibraryVersionMinor >= 51U,
              "TraceKit needs BTP 2.51.0 or newer (2.x)");

using tracekit::ErrorInfo;

namespace tracekit {

const char* state_name(State state) noexcept {
    switch (state) {
        case State::Stopped: return "stopped";
        case State::Starting: return "starting";
        case State::Ready: return "ready (no peer)";
        case State::Connected: return "connected";
        case State::Failed: return "failed";
    }
    return "?";
}

}  // namespace tracekit

namespace {

void apply_log_level() {
    const auto level = static_cast<esp_log_level_t>(CONFIG_TRACEKIT_LOG_LEVEL);
    for (const char* tag : tracekit::tag::kAll) esp_log_level_set(tag, level);
}

// Every failure reaches the console the same way: the module's own tag, the
// actionable message, then the code's name for searching.
void log_failure(const ErrorInfo& error) {
    ESP_LOGE(error.module, "%s [%s]", error.message, tracekit::err_name(error.code));
}

}  // namespace

TraceKit::TraceKit(const tracekit::DeviceConfig& device,
                     const tracekit::StorageConfig& storage) noexcept
    : device_(device), storage_(storage) {}

TraceKit::TraceKit(tracekit::App& app) noexcept : app_(&app) {}

TraceKit::~TraceKit() { delete core_; }

namespace {

// begin() does its work on a task of its own: mounting LittleFS, importing a
// key file and starting Wi-Fi go deep, and app_main's stack is 3.5 KB by
// default. Run on the caller's stack, an overflow there landed in the heap
// block next to it and crashed somewhere else, much later.
constexpr std::uint32_t kBeginStackBytes = 8192U;

struct StartJob {
    tracekit::DeviceCore* core;
    const TraceKit::CommandsFn* commands;
    const TraceKit::TopicsFn* topics;
    const TraceKit::SettingsFn* settings;
    esp_err_t result;
    SemaphoreHandle_t done;
};

void start_task(void* arg) {
    auto* job = static_cast<StartJob*>(arg);
    job->result = job->core->start(*job->commands, *job->topics, *job->settings);
    xSemaphoreGive(job->done);
    vTaskDelete(nullptr);
}

}  // namespace

esp_err_t TraceKit::start_core(const CommandsFn& commands, const TopicsFn& topics,
                                const SettingsFn& settings) noexcept {
    core_->set_begin_task(xTaskGetCurrentTaskHandle());
    StartJob job{core_, &commands, &topics, &settings, ESP_FAIL, xSemaphoreCreateBinary()};
    // Same core as the caller: drivers install their interrupts on the core
    // that installs them. Same priority, so nothing changes for the caller.
    TaskHandle_t task = nullptr;
    if (job.done != nullptr &&
        xTaskCreatePinnedToCore(&start_task, "tracekit_begin", kBeginStackBytes, &job, uxTaskPriorityGet(nullptr),
                                &task, xPortGetCoreID()) == pdPASS) {
        xSemaphoreTake(job.done, portMAX_DELAY);
        vSemaphoreDelete(job.done);
        return job.result;
    }
    // No memory for the helper: run here and hope the caller's stack holds.
    if (job.done != nullptr) vSemaphoreDelete(job.done);
    ESP_LOGW(tracekit::tag::kCore, "no memory for a %u-byte begin() task: running on the caller's stack",
             static_cast<unsigned>(kBeginStackBytes));
    return core_->start(commands, topics, settings);
}

esp_err_t TraceKit::begin(CommandsFn commands, TopicsFn topics, SettingsFn settings) noexcept {
    if (started_) {
        tracekit::fail(error_, TRACEKIT_ERR_ALREADY_STARTED, tracekit::tag::kCore,
                     "begin() called twice: the device is already running");
        log_failure(error_);
        return error_.code;
    }
    started_ = true;
    apply_log_level();

    if (app_ != nullptr) {
        // Every global exists by now: reading the App here, not in the
        // constructor, keeps the order of the two globals irrelevant.
        app_->device_ = this;
        device_ = app_->config();
        storage_ = app_->storage();
        tracekit::App* app = app_;
        if (!commands) commands = [app](tracekit::Commands& c) { app->commands(c); };
        if (!topics) topics = [app](tracekit::Topics& t) { app->topics(t); };
        if (!settings) settings = [app](tracekit::Settings& s) { app->settings(s); };
    }

    if (tracekit::check_device_config(device_, error_) != ESP_OK ||
        tracekit::check_storage_config(storage_, error_) != ESP_OK) {
        log_failure(error_);
        return error_.code;
    }
    ESP_LOGI(tracekit::tag::kCore, "TraceKit %s starting \"%s\" (requested link %s)",
             TRACEKIT_VERSION_STRING, device_.name, tracekit::link_name(device_.link));

    core_ = new (std::nothrow) tracekit::DeviceCore(device_, storage_, error_);
    if (core_ == nullptr) {
        tracekit::fail(error_, ESP_ERR_NO_MEM, tracekit::tag::kCore, "no memory for the device core");
        log_failure(error_);
        return error_.code;
    }
    const esp_err_t err = start_core(commands, topics, settings);
    if (err != ESP_OK) {
        log_failure(error_);
        delete core_;  // stops whatever did start
        core_ = nullptr;
        return err;
    }
    return ESP_OK;
}

std::uint32_t TraceKit::boot_id() const noexcept {
    return core_ != nullptr ? core_->device_boot_id() : 0U;
}

bool TraceKit::run(const char* command_line) noexcept {
    return core_ != nullptr && core_->run(command_line);
}

bool TraceKit::set_setting(const char* name, const char* value) noexcept {
    return core_ != nullptr && core_->set_setting(name, value);
}

bool TraceKit::save_setting(const char* name) noexcept {
    return core_ != nullptr && core_->save_setting(name);
}

bool TraceKit::get_setting(const char* name, char* out, std::size_t size) const noexcept {
    return core_ != nullptr && core_->get_setting(name, out, size);
}

bool TraceKit::time_valid() const noexcept { return core_ != nullptr && core_->wall_clock().valid(); }
std::time_t TraceKit::now() const noexcept { return std::time(nullptr); }
std::int64_t TraceKit::now_us() const noexcept { return core_ != nullptr ? core_->wall_clock().now_us() : 0; }
tracekit::TimeSource TraceKit::time_source() const noexcept {
    return core_ != nullptr ? core_->wall_clock().source() : tracekit::TimeSource::None;
}
void TraceKit::set_power_mode(tracekit::PowerMode mode) noexcept {
    if (core_ != nullptr) core_->set_power_mode(mode);
}
tracekit::PowerMode TraceKit::power_mode() const noexcept {
    return core_ != nullptr ? core_->power_mode() : device_.power.mode;
}
bool TraceKit::sleep_for(std::uint64_t ms) noexcept { return core_ != nullptr && core_->sleep_for(ms); }
bool TraceKit::sleep_until(std::time_t utc) noexcept {
    if (core_ == nullptr || !core_->wall_clock().valid()) return false;
    const std::time_t now = std::time(nullptr);
    if (utc <= now) return false;
    return core_->sleep_for(static_cast<std::uint64_t>(utc - now) * 1000ULL);
}
tracekit::WakeReason TraceKit::wake_reason() const noexcept {
    return core_ != nullptr ? core_->wake_reason() : tracekit::WakeReason::PowerOn;
}
std::uint32_t TraceKit::wake_count() const noexcept { return core_ != nullptr ? core_->wake_count() : 0U; }
bool TraceKit::stay_awake() const noexcept { return core_ != nullptr && core_->stay_awake(); }
bool TraceKit::set_time(std::time_t utc) noexcept {
    return core_ != nullptr && core_->wall_clock().set(static_cast<std::int64_t>(utc), tracekit::TimeSource::Application);
}

bool TraceKit::subscribed(const char* topic) const noexcept {
    return core_ != nullptr && core_->subscribed(topic);
}

bool TraceKit::publish(const char* topic) noexcept {
    return core_ != nullptr && core_->publish(topic);
}

bool TraceKit::publish(tracekit::Topic topic) noexcept {
    return core_ != nullptr && core_->publish(topic);
}

bool TraceKit::subscribed(tracekit::Topic topic) const noexcept {
    return core_ != nullptr && core_->subscribed(topic);
}

bool TraceKit::write_stream(tracekit::Topic topic, const std::int16_t* samples, std::size_t count,
                             std::uint32_t rate_hz, std::uint64_t timestamp_us) noexcept {
    return core_ != nullptr && core_->write_stream(topic, samples, count, rate_hz, timestamp_us);
}

bool TraceKit::write_stream(const char* topic, const std::int16_t* samples, std::size_t count,
                             std::uint32_t rate_hz, std::uint64_t timestamp_us) noexcept {
    return core_ != nullptr && core_->write_stream(topic, samples, count, rate_hz, timestamp_us);
}

void TraceKit::routine() noexcept {
    if (core_ != nullptr && device_.threading == tracekit::Threading::Manual && core_->claim_manual_task()) {
        core_->routine();
    }
}

void TraceKit::tick() noexcept {
    if (core_ != nullptr && device_.threading == tracekit::Threading::Manual && core_->claim_manual_task()) {
        core_->tick();
    }
}

tracekit::State TraceKit::state() const noexcept {
    if (core_ != nullptr) return core_->state();
    return started_ ? tracekit::State::Failed : tracekit::State::Stopped;
}

std::uint8_t TraceKit::degraded() const noexcept { return core_ != nullptr ? core_->degraded() : 0U; }

tracekit::Link TraceKit::active_link() const noexcept {
    return core_ != nullptr ? core_->active_link() : device_.link;
}

bool TraceKit::connected() const noexcept { return core_ != nullptr && core_->connected(); }

std::uint32_t TraceKit::source_id() const noexcept {
    return core_ != nullptr ? core_->device_source_id() : 0U;
}

tracekit::FileSystem* TraceKit::fs() noexcept { return core_ != nullptr ? core_->fs() : nullptr; }

esp_err_t TraceKit::set_link(tracekit::Link link) noexcept {
    return core_ != nullptr ? core_->set_link(link) : ESP_ERR_INVALID_STATE;
}

TinyShell* TraceKit::shell() noexcept { return core_ != nullptr ? &core_->shell() : nullptr; }

btp::Node* TraceKit::node() noexcept { return core_ != nullptr ? core_->node() : nullptr; }

bool TraceKit::peer_receive(const std::uint8_t* datagram, std::size_t size) noexcept {
    tracekit::PeerLink* peer = core_ != nullptr ? core_->peer() : nullptr;
    return peer != nullptr && peer->push_rx(datagram, size);
}

bool TraceKit::peer_send(std::uint16_t channel, const std::uint8_t* data, std::size_t size) noexcept {
    tracekit::PeerLink* peer = core_ != nullptr ? core_->peer() : nullptr;
    return peer != nullptr && peer->push_tx(channel, data, size);
}

std::size_t TraceKit::max_peer_message() const noexcept {
    tracekit::PeerLink* peer = core_ != nullptr ? core_->peer() : nullptr;
    return peer != nullptr ? peer->max_message() : 0U;
}

tracekit::PeerStats TraceKit::peer_stats() const noexcept {
    tracekit::PeerLink* peer = core_ != nullptr ? core_->peer() : nullptr;
    return peer != nullptr ? peer->stats() : tracekit::PeerStats{};
}

esp_err_t TraceKit::set_peer_password(const char* password) noexcept {
    tracekit::PeerLink* peer = core_ != nullptr ? core_->peer() : nullptr;
    return peer != nullptr ? peer->set_password(password) : ESP_ERR_INVALID_STATE;
}

bool TraceKit::has_peer_key() const noexcept {
    tracekit::PeerLink* peer = core_ != nullptr ? core_->peer() : nullptr;
    return peer != nullptr && peer->has_key();
}
