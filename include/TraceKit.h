#ifndef BTP_DEVICE_H
#define BTP_DEVICE_H

// TraceKit -- the one object an application needs to talk to TraceView over
// BTP. See README.md for the target API and docs/PLANO.md for the plan.
//
// Status (Phase 4): identity, one link per boot, the BTP node with session,
// manifest and STATUS, the TraceView terminal, commands and telemetry topics.
// The key and the file system arrive in Phase 5.

#include <tracekit/app.h>
#include <tracekit/commands.h>
#include <tracekit/config.h>
#include <tracekit/error.h>
#include <tracekit/filesystem.h>
#include <tracekit/identity.h>
#include <tracekit/log.h>
#include <tracekit/settings.h>
#include <tracekit/sleep.h>
#include <tracekit/time.h>
#include <tracekit/topics.h>

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <functional>

class TinyShell;
namespace btp {
class Node;
}

namespace tracekit {

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

}  // namespace tracekit

class TraceKit {
public:
    /// The application's declaration functions (btp_commands.cpp,
    /// btp_topics.cpp): a plain function, or a lambda that captures.
    using CommandsFn = std::function<void(tracekit::Commands&)>;
    using TopicsFn = std::function<void(tracekit::Topics&)>;
    using SettingsFn = std::function<void(tracekit::Settings&)>;

    /// Both configs are copied; they do not need to outlive the device.
    TraceKit(const tracekit::DeviceConfig& device,
              const tracekit::StorageConfig& storage = {}) noexcept;
    /// A device described by a tracekit::App (include/tracekit/app.h). Nothing of
    /// the App is called here -- its config, commands and topics are read by
    /// begin() -- so the two globals may be declared in any order or file.
    explicit TraceKit(tracekit::App& app) noexcept;
    ~TraceKit();

    TraceKit(const TraceKit&) = delete;
    TraceKit& operator=(const TraceKit&) = delete;

    /// Brings the device up (identity, link, node, commands, topics, tasks).
    /// Returns ESP_OK, or the failure also recorded in last_error() and
    /// printed with ESP_LOGE. Call once, from app_main() or any task. Either
    /// function may be empty. With an App, call it with no arguments: the
    /// App's commands() and topics() are used (a function given here
    /// replaces the App's).
    esp_err_t begin(CommandsFn commands = nullptr, TopicsFn topics = nullptr,
                    SettingsFn settings = nullptr) noexcept;

    /// Publishes a topic now (an event), on top of its periodic rate -- only
    /// if someone is subscribed. Any task. False: no topic by that name.
    bool publish(const char* topic) noexcept;
    /// The same by handle (Topics declarations convert to tracekit::Topic):
    /// no name lookup. False: an invalid handle, or a stream topic.
    bool publish(tracekit::Topic topic) noexcept;

    /// Any task: whether anybody is subscribed to the topic now (refreshed on
    /// every comms pass). For work only worth doing while someone watches,
    /// like sampling a slow sensor for the topic's fill().
    bool subscribed(const char* topic) const noexcept;
    bool subscribed(tracekit::Topic topic) const noexcept;

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
    bool write_stream(tracekit::Topic topic, const std::int16_t* samples, std::size_t count,
                      std::uint32_t rate_hz, std::uint64_t timestamp_us) noexcept;

    /// Runs a shell line on the library's shell task, as if typed in the
    /// terminal: for a button, a scheduler or a script. Any task, never
    /// blocks: false when the shell queue is full (try again later) or the
    /// line is too long. The output goes to TraceView's log panel
    /// (LogLevel::Command).
    bool run(const char* command_line) noexcept;

    /// What `cfg -set <name>, <value>` does: checks the text against the
    /// setting's type and range, writes the variable, saves it, calls its
    /// on_change. Any task. False with the reason in the log.
    bool set_setting(const char* name, const char* value) noexcept;
    /// Saves the value the application put in the setting's variable.
    bool save_setting(const char* name) noexcept;
    /// The current value as text ("30", "true"). False: no such setting.
    bool get_setting(const char* name, char* out, std::size_t size) const noexcept;

    /// Wall-clock time (tracekit/time.h). False until SNTP, `btp -time_set`
    /// or set_time() set it since the last power-up.
    bool time_valid() const noexcept;
    /// UTC seconds (meaningless while !time_valid()).
    std::time_t now() const noexcept;
    std::int64_t now_us() const noexcept;
    tracekit::TimeSource time_source() const noexcept;
    /// From the application's own source (an RTC chip, GPS): UTC seconds.
    bool set_time(std::time_t utc) noexcept;

    /// PowerConfig at run time (btp -power_set does the same). Any task.
    void set_power_mode(tracekit::PowerMode mode) noexcept;
    tracekit::PowerMode power_mode() const noexcept;

    /// Deep sleep for `ms` (tracekit/sleep.h): queued data gets
    /// SleepConfig::flush_ms to leave, the link stops, the chip sleeps and
    /// wakes as a new boot. Does not return -- except false when the device
    /// is kept awake (btp -awake, SleepConfig::awake_gpio).
    bool sleep_for(std::uint64_t ms) noexcept;
    /// Until a wall-clock time (needs time_valid()). False: no valid time,
    /// already past, or kept awake.
    bool sleep_until(std::time_t utc) noexcept;
    tracekit::WakeReason wake_reason() const noexcept;
    /// Wakes from deep sleep since the last power-up / reset.
    std::uint32_t wake_count() const noexcept;
    /// sleep_for() would refuse now (btp -awake or the awake GPIO).
    bool stay_awake() const noexcept;

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

    tracekit::State state() const noexcept;
    std::uint8_t degraded() const noexcept;
    /// The link this boot actually uses (after FromSettings / fallback).
    tracekit::Link active_link() const noexcept;
    bool connected() const noexcept;
    std::uint32_t source_id() const noexcept;

    /// The most recent failure (code == ESP_OK when there was none).
    const tracekit::ErrorInfo& last_error() const noexcept { return error_; }

    const tracekit::DeviceConfig& device_config() const noexcept { return device_; }
    const tracekit::StorageConfig& storage_config() const noexcept { return storage_; }

    /// The device's file system (LittleFS, else the SD card); nullptr when
    /// nothing is mounted. Any task.
    tracekit::FileSystem* fs() noexcept;

    /// Saves the link Link::FromSettings uses from the next boot on.
    esp_err_t set_link(tracekit::Link link) noexcept;

    // ---- peer link (DeviceConfig::peer, include/tracekit/peer.h) -------------

    /// The transport received one datagram from the peer. Any task, never
    /// blocks; the message is opened on the communication task. False: no
    /// peer link, or its receive queue is full.
    bool peer_receive(const std::uint8_t* datagram, std::size_t size) noexcept;
    /// Queues one message for the peer on `channel` (the receiver gets it in
    /// PeerConfig::on_message). Any task, never blocks. False: no peer link,
    /// no password set, larger than max_peer_message(), or the queue is full.
    bool peer_send(std::uint16_t channel, const std::uint8_t* data, std::size_t size) noexcept;
    /// Largest message peer_send() takes (one datagram); 0 without a peer link.
    std::size_t max_peer_message() const noexcept;
    tracekit::PeerStats peer_stats() const noexcept;
    /// Sets the peer-link password (every device of the group uses the same),
    /// saves the derived key in NVS and applies it at once; nullptr or ""
    /// erases it. Any task. Same as `btp -peer_password <text>`.
    esp_err_t set_peer_password(const char* password) noexcept;
    bool has_peer_key() const noexcept;

    /// Advanced. The shell behind the terminal and COMMAND_REQUEST (register
    /// commands on it after begin()); nullptr before begin().
    TinyShell* shell() noexcept;
    /// Advanced. The BTP node; only touch it from the communication context
    /// (inside routine() / the library's task). nullptr before begin().
    btp::Node* node() noexcept;

private:
    esp_err_t start_core(const CommandsFn& commands, const TopicsFn& topics,
                         const SettingsFn& settings) noexcept;
    tracekit::DeviceConfig device_;
    tracekit::StorageConfig storage_;
    tracekit::App* app_ = nullptr;
    tracekit::ErrorInfo error_;
    tracekit::DeviceCore* core_ = nullptr;
    bool started_ = false;
};

#endif  // BTP_DEVICE_H
