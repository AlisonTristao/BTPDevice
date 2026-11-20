#include <BtpDevice.h>

#include <new>

#include <btp/version.hpp>
#include <btpdev/version.h>
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
              "BTPDevice needs BTP 2.51.0 or newer (2.x)");

using btpdev::ErrorInfo;

namespace btpdev {

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

}  // namespace btpdev

namespace {

void apply_log_level() {
    const auto level = static_cast<esp_log_level_t>(CONFIG_BTPDEV_LOG_LEVEL);
    for (const char* tag : btpdev::tag::kAll) esp_log_level_set(tag, level);
}

// Every failure reaches the console the same way: the module's own tag, the
// actionable message, then the code's name for searching.
void log_failure(const ErrorInfo& error) {
    ESP_LOGE(error.module, "%s [%s]", error.message, btpdev::err_name(error.code));
}

}  // namespace

BtpDevice::BtpDevice(const btpdev::DeviceConfig& device,
                     const btpdev::StorageConfig& storage) noexcept
    : device_(device), storage_(storage) {}

BtpDevice::BtpDevice(btpdev::App& app) noexcept : app_(&app) {}

BtpDevice::~BtpDevice() { delete core_; }

namespace {

// begin() does its work on a task of its own: mounting LittleFS, importing a
// key file and starting Wi-Fi go deep, and app_main's stack is 3.5 KB by
// default. Run on the caller's stack, an overflow there landed in the heap
// block next to it and crashed somewhere else, much later.
constexpr std::uint32_t kBeginStackBytes = 8192U;

struct StartJob {
    btpdev::DeviceCore* core;
    const BtpDevice::CommandsFn* commands;
    const BtpDevice::TopicsFn* topics;
    const BtpDevice::SettingsFn* settings;
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

esp_err_t BtpDevice::start_core(const CommandsFn& commands, const TopicsFn& topics,
                                const SettingsFn& settings) noexcept {
    core_->set_begin_task(xTaskGetCurrentTaskHandle());
    StartJob job{core_, &commands, &topics, &settings, ESP_FAIL, xSemaphoreCreateBinary()};
    // Same core as the caller: drivers install their interrupts on the core
    // that installs them. Same priority, so nothing changes for the caller.
    TaskHandle_t task = nullptr;
    if (job.done != nullptr &&
        xTaskCreatePinnedToCore(&start_task, "btpdev_begin", kBeginStackBytes, &job, uxTaskPriorityGet(nullptr),
                                &task, xPortGetCoreID()) == pdPASS) {
        xSemaphoreTake(job.done, portMAX_DELAY);
        vSemaphoreDelete(job.done);
        return job.result;
    }
    // No memory for the helper: run here and hope the caller's stack holds.
    if (job.done != nullptr) vSemaphoreDelete(job.done);
    ESP_LOGW(btpdev::tag::kCore, "no memory for a %u-byte begin() task: running on the caller's stack",
             static_cast<unsigned>(kBeginStackBytes));
    return core_->start(commands, topics, settings);
}

esp_err_t BtpDevice::begin(CommandsFn commands, TopicsFn topics, SettingsFn settings) noexcept {
    if (started_) {
        btpdev::fail(error_, BTPDEV_ERR_ALREADY_STARTED, btpdev::tag::kCore,
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
        btpdev::App* app = app_;
        if (!commands) commands = [app](btpdev::Commands& c) { app->commands(c); };
        if (!topics) topics = [app](btpdev::Topics& t) { app->topics(t); };
        if (!settings) settings = [app](btpdev::Settings& s) { app->settings(s); };
    }

    if (btpdev::check_device_config(device_, error_) != ESP_OK ||
        btpdev::check_storage_config(storage_, error_) != ESP_OK) {
        log_failure(error_);
        return error_.code;
    }
    ESP_LOGI(btpdev::tag::kCore, "BTPDevice %s starting \"%s\" (requested link %s)",
             BTPDEV_VERSION_STRING, device_.name, btpdev::link_name(device_.link));

    core_ = new (std::nothrow) btpdev::DeviceCore(device_, storage_, error_);
    if (core_ == nullptr) {
        btpdev::fail(error_, ESP_ERR_NO_MEM, btpdev::tag::kCore, "no memory for the device core");
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

std::uint32_t BtpDevice::boot_id() const noexcept {
    return core_ != nullptr ? core_->device_boot_id() : 0U;
}

bool BtpDevice::run(const char* command_line) noexcept {
    return core_ != nullptr && core_->run(command_line);
}

bool BtpDevice::set_setting(const char* name, const char* value) noexcept {
    return core_ != nullptr && core_->set_setting(name, value);
}

bool BtpDevice::save_setting(const char* name) noexcept {
    return core_ != nullptr && core_->save_setting(name);
}

bool BtpDevice::get_setting(const char* name, char* out, std::size_t size) const noexcept {
    return core_ != nullptr && core_->get_setting(name, out, size);
}

bool BtpDevice::time_valid() const noexcept { return core_ != nullptr && core_->wall_clock().valid(); }
std::time_t BtpDevice::now() const noexcept { return std::time(nullptr); }
std::int64_t BtpDevice::now_us() const noexcept { return core_ != nullptr ? core_->wall_clock().now_us() : 0; }
btpdev::TimeSource BtpDevice::time_source() const noexcept {
    return core_ != nullptr ? core_->wall_clock().source() : btpdev::TimeSource::None;
}
bool BtpDevice::set_time(std::time_t utc) noexcept {
    return core_ != nullptr && core_->wall_clock().set(static_cast<std::int64_t>(utc), btpdev::TimeSource::Application);
}

bool BtpDevice::subscribed(const char* topic) const noexcept {
    return core_ != nullptr && core_->subscribed(topic);
}

bool BtpDevice::publish(const char* topic) noexcept {
    return core_ != nullptr && core_->publish(topic);
}

bool BtpDevice::publish(btpdev::Topic topic) noexcept {
    return core_ != nullptr && core_->publish(topic);
}

bool BtpDevice::subscribed(btpdev::Topic topic) const noexcept {
    return core_ != nullptr && core_->subscribed(topic);
}

bool BtpDevice::write_stream(btpdev::Topic topic, const std::int16_t* samples, std::size_t count,
                             std::uint32_t rate_hz, std::uint64_t timestamp_us) noexcept {
    return core_ != nullptr && core_->write_stream(topic, samples, count, rate_hz, timestamp_us);
}

bool BtpDevice::write_stream(const char* topic, const std::int16_t* samples, std::size_t count,
                             std::uint32_t rate_hz, std::uint64_t timestamp_us) noexcept {
    return core_ != nullptr && core_->write_stream(topic, samples, count, rate_hz, timestamp_us);
}

void BtpDevice::routine() noexcept {
    if (core_ != nullptr && device_.threading == btpdev::Threading::Manual && core_->claim_manual_task()) {
        core_->routine();
    }
}

void BtpDevice::tick() noexcept {
    if (core_ != nullptr && device_.threading == btpdev::Threading::Manual && core_->claim_manual_task()) {
        core_->tick();
    }
}

btpdev::State BtpDevice::state() const noexcept {
    if (core_ != nullptr) return core_->state();
    return started_ ? btpdev::State::Failed : btpdev::State::Stopped;
}

std::uint8_t BtpDevice::degraded() const noexcept { return core_ != nullptr ? core_->degraded() : 0U; }

btpdev::Link BtpDevice::active_link() const noexcept {
    return core_ != nullptr ? core_->active_link() : device_.link;
}

bool BtpDevice::connected() const noexcept { return core_ != nullptr && core_->connected(); }

std::uint32_t BtpDevice::source_id() const noexcept {
    return core_ != nullptr ? core_->device_source_id() : 0U;
}

btpdev::FileSystem* BtpDevice::fs() noexcept { return core_ != nullptr ? core_->fs() : nullptr; }

esp_err_t BtpDevice::set_link(btpdev::Link link) noexcept {
    return core_ != nullptr ? core_->set_link(link) : ESP_ERR_INVALID_STATE;
}

TinyShell* BtpDevice::shell() noexcept { return core_ != nullptr ? &core_->shell() : nullptr; }

btp::Node* BtpDevice::node() noexcept { return core_ != nullptr ? core_->node() : nullptr; }

bool BtpDevice::peer_receive(const std::uint8_t* datagram, std::size_t size) noexcept {
    btpdev::PeerLink* peer = core_ != nullptr ? core_->peer() : nullptr;
    return peer != nullptr && peer->push_rx(datagram, size);
}

bool BtpDevice::peer_send(std::uint16_t channel, const std::uint8_t* data, std::size_t size) noexcept {
    btpdev::PeerLink* peer = core_ != nullptr ? core_->peer() : nullptr;
    return peer != nullptr && peer->push_tx(channel, data, size);
}

std::size_t BtpDevice::max_peer_message() const noexcept {
    btpdev::PeerLink* peer = core_ != nullptr ? core_->peer() : nullptr;
    return peer != nullptr ? peer->max_message() : 0U;
}

btpdev::PeerStats BtpDevice::peer_stats() const noexcept {
    btpdev::PeerLink* peer = core_ != nullptr ? core_->peer() : nullptr;
    return peer != nullptr ? peer->stats() : btpdev::PeerStats{};
}

esp_err_t BtpDevice::set_peer_password(const char* password) noexcept {
    btpdev::PeerLink* peer = core_ != nullptr ? core_->peer() : nullptr;
    return peer != nullptr ? peer->set_password(password) : ESP_ERR_INVALID_STATE;
}

bool BtpDevice::has_peer_key() const noexcept {
    btpdev::PeerLink* peer = core_ != nullptr ? core_->peer() : nullptr;
    return peer != nullptr && peer->has_key();
}
