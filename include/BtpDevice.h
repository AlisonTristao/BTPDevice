#ifndef BTP_DEVICE_H
#define BTP_DEVICE_H

// BtpDevice -- the one object an application needs to talk to TraceView over
// BTP. See README.md for the target API and docs/PLANO.md for the plan.
//
// Status (Phase 4): identity, one link per boot, the BTP node with session,
// manifest and STATUS, the TraceView terminal, commands and telemetry topics.
// The key and the file system arrive in Phase 5.

#include <btpdev/app.h>
#include <btpdev/commands.h>
#include <btpdev/config.h>
#include <btpdev/error.h>
#include <btpdev/filesystem.h>
#include <btpdev/identity.h>
#include <btpdev/log.h>
#include <btpdev/topics.h>

#include <cstddef>
#include <cstdint>
#include <functional>

class TinyShell;
namespace btp {
class Node;
}

namespace btpdev {

class DeviceCore;

/// Lifecycle (docs/PLANO.md 4.5).
enum class State : std::uint8_t {
    Stopped,    ///< before begin()
    Starting,   ///< inside begin()
    Ready,      ///< running, no peer
    Connected,  ///< a peer (TraceView, the dongle) is on the link
    Failed,     ///< begin() failed; last_error() says why
};

/// Bits of degraded(): running, but without something that was asked for.
enum Degraded : std::uint8_t {
    kDegradedNone = 0U,
    kNoStorage = 1U << 0,     ///< the file system did not mount
    kNoKey = 1U << 1,         ///< no key: everything goes in clear
    kLinkFallback = 1U << 2,  ///< the requested link could not start: on Serial
};

const char* state_name(State state) noexcept;

}  // namespace btpdev

class BtpDevice {
public:
    /// The application's declaration functions (btp_commands.cpp,
    /// btp_topics.cpp): a plain function, or a lambda that captures.
    using CommandsFn = std::function<void(btpdev::Commands&)>;
    using TopicsFn = std::function<void(btpdev::Topics&)>;

    /// Both configs are copied; they do not need to outlive the device.
    BtpDevice(const btpdev::DeviceConfig& device,
              const btpdev::StorageConfig& storage = {}) noexcept;
    /// A device described by a btpdev::App (include/btpdev/app.h). Nothing of
    /// the App is called here -- its config, commands and topics are read by
    /// begin() -- so the two globals may be declared in any order or file.
    explicit BtpDevice(btpdev::App& app) noexcept;
    ~BtpDevice();

    BtpDevice(const BtpDevice&) = delete;
    BtpDevice& operator=(const BtpDevice&) = delete;

    /// Brings the device up (identity, link, node, commands, topics, tasks).
    /// Returns ESP_OK, or the failure also recorded in last_error() and
    /// printed with ESP_LOGE. Call once, from app_main() or any task. Either
    /// function may be empty. With an App, call it with no arguments: the
    /// App's commands() and topics() are used (a function given here
    /// replaces the App's).
    esp_err_t begin(CommandsFn commands = nullptr, TopicsFn topics = nullptr) noexcept;

    /// Publishes a topic now (an event), on top of its periodic rate -- only
    /// if someone is subscribed. Any task. False: no topic by that name.
    bool publish(const char* topic) noexcept;
    /// The same by handle (Topics declarations convert to btpdev::Topic):
    /// no name lookup. False: an invalid handle, or a stream topic.
    bool publish(btpdev::Topic topic) noexcept;

    /// Any task: whether anybody is subscribed to the topic now (refreshed on
    /// every comms pass). For work only worth doing while someone watches,
    /// like sampling a slow sensor for the topic's fill().
    bool subscribed(const char* topic) const noexcept;
    bool subscribed(btpdev::Topic topic) const noexcept;

    /// Queues a block of samples for a stream topic (Topics::stream()); the
    /// library sends it in order while someone is subscribed. Call it from
    /// ONE task per stream; it never blocks. False, and nothing queued: nobody
    /// subscribed, the stream's queue is full (the link is slower than the
    /// samples: counted in `btp -topics`), not a stream, or `count` is 0 or
    /// over its max_samples. `timestamp_us` is the first sample's time
    /// (esp_timer_get_time()).
    ///
    /// Not from an interrupt: in an ADC / I2S DMA callback, notify a task and
    /// call write_stream() from that task.
    bool write_stream(const char* topic, const std::int16_t* samples, std::size_t count,
                      std::uint32_t rate_hz, std::uint64_t timestamp_us) noexcept;
    bool write_stream(btpdev::Topic topic, const std::int16_t* samples, std::size_t count,
                      std::uint32_t rate_hz, std::uint64_t timestamp_us) noexcept;

    /// Runs a shell line on the library's shell task, as if typed in the
    /// terminal: for a button, a scheduler or a script. Any task, never
    /// blocks: false when the shell queue is full (try again later) or the
    /// line is too long. The output goes to TraceView's log panel
    /// (LogLevel::Command).
    bool run(const char* command_line) noexcept;

    /// This boot's BTP boot_id (an NVS counter), with source_id() the
    /// identity every message carries.
    std::uint32_t boot_id() const noexcept;

    /// Threading::Manual only: one full pass (received frames, commands,
    /// terminal, due telemetry, link housekeeping, then tick()). Call it
    /// often -- every 1 to 10 ms -- from ONE task. With Threading::Auto the
    /// library's own task does this and these calls do nothing.
    ///
    /// Received bytes wait for the next call: the pass rate is the device's
    /// reply latency. Not from an interrupt or an esp_timer callback (it
    /// seals, sends and takes locks): to pace it with a timer, have the timer
    /// notify the task that calls it.
    void routine() noexcept;
    /// Housekeeping only: session watchdog, STATUS, error summaries.
    /// routine() already calls it. Same rule: Threading::Manual, and the SAME
    /// task as routine(). The first task that calls either one owns the
    /// device; a call from any other task is refused (and logged once).
    void tick() noexcept;

    btpdev::State state() const noexcept;
    std::uint8_t degraded() const noexcept;
    /// The link this boot actually uses (after FromSettings / fallback).
    btpdev::Link active_link() const noexcept;
    bool connected() const noexcept;
    std::uint32_t source_id() const noexcept;

    /// The most recent failure (code == ESP_OK when there was none).
    const btpdev::ErrorInfo& last_error() const noexcept { return error_; }

    const btpdev::DeviceConfig& device_config() const noexcept { return device_; }
    const btpdev::StorageConfig& storage_config() const noexcept { return storage_; }

    /// The device's file system (LittleFS, else the SD card); nullptr when
    /// nothing is mounted. Any task.
    btpdev::FileSystem* fs() noexcept;

    /// Saves the link Link::FromSettings uses from the next boot on.
    esp_err_t set_link(btpdev::Link link) noexcept;

    /// Advanced. The shell behind the terminal and COMMAND_REQUEST (register
    /// commands on it after begin()); nullptr before begin().
    TinyShell* shell() noexcept;
    /// Advanced. The BTP node; only touch it from the communication context
    /// (inside routine() / the library's task). nullptr before begin().
    btp::Node* node() noexcept;

private:
    btpdev::DeviceConfig device_;
    btpdev::StorageConfig storage_;
    btpdev::App* app_ = nullptr;
    btpdev::ErrorInfo error_;
    btpdev::DeviceCore* core_ = nullptr;
    bool started_ = false;
};

#endif  // BTP_DEVICE_H
