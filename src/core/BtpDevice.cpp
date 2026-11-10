#include <BtpDevice.h>

#include <new>

#include <btp/version.hpp>
#include <btpdev/version.h>
#include <esp_log.h>

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

esp_err_t BtpDevice::begin(CommandsFn commands, TopicsFn topics) noexcept {
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
    const esp_err_t err = core_->start(commands, topics);
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
