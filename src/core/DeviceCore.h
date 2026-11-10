#ifndef BTPDEV_DEVICE_CORE_H
#define BTPDEV_DEVICE_CORE_H

// Everything BtpDevice owns, behind the facade (include/BtpDevice.h keeps the
// public header free of BTP, TinyShell and FreeRTOS internals).
//
// Threads (docs/PLANO.md 4.4):
//   * link driver tasks  -> on_link_*(): copy into rx_queue_, wake the comms
//                           task. Nothing else.
//   * comms task         -> routine(): the ONLY context that touches node_,
//     (or the user's task    terminal_ and the link's send side of the node.
//      with Manual)
//   * shell task         -> runs one command at a time on shell_, hands the
//                           result back through completion_.
//
// It is the node's NodeConfig: send(), terminal() and command() below are
// called by btp::Node from inside routine().

#include <btp/node.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <BtpDevice.h>
// TinyShell.h still injects `using namespace std` for 1.x compatibility unless
// told not to; keep std out of the global namespace of our translation units.
#ifndef TINYSHELL_NO_USING_NAMESPACE_STD
#define TINYSHELL_NO_USING_NAMESPACE_STD
#endif
#include <TinyShell.h>

#include "core/FrameQueue.h"
#include "core/Memory.h"
#include "core/NodePresets.h"
#include "core/RuntimeErrors.h"
#include "core/StreamFraming.h"
#include "protocol/ShellText.h"
#include "protocol/Terminal.h"
#include "protocol/TopicRegistry.h"
#include "security/DeviceKey.h"
#include "transport/Link.h"
#include "transport/LinkMutex.h"
#include "transport/WifiNetworks.h"

#include <btpdev/filesystem.h>

namespace btpdev {

class DeviceCore final : public btp::NodeConfig {
public:
    DeviceCore(const DeviceConfig& device, const StorageConfig& storage, ErrorInfo& error) noexcept;
    ~DeviceCore() override;
    DeviceCore(const DeviceCore&) = delete;
    DeviceCore& operator=(const DeviceCore&) = delete;

    /// The whole of begin(). On failure, `error` says why and nothing runs.
    esp_err_t start(const BtpDevice::CommandsFn& commands, const BtpDevice::TopicsFn& topics) noexcept;

    /// Any task: publish a topic on the next pass (see TopicRegistry).
    bool publish(const char* topic) noexcept;
    bool publish(Topic topic) noexcept;
    bool subscribed(Topic topic) const noexcept { return registry_.subscribed(topic); }
    bool subscribed(const char* topic) const noexcept { return registry_.subscribed(topic); }
    /// Any task (one per stream): a block for a stream topic (BtpDevice::write_stream()).
    bool write_stream(const char* topic, const std::int16_t* samples, std::size_t count, std::uint32_t rate_hz,
                      std::uint64_t timestamp_us) noexcept;
    bool write_stream(Topic topic, const std::int16_t* samples, std::size_t count, std::uint32_t rate_hz,
                      std::uint64_t timestamp_us) noexcept;
    /// Threading::Manual: true when the calling task owns routine()/tick().
    /// The first caller becomes the owner; another task is refused (D8: one
    /// context touches the node) and logged once.
    bool claim_manual_task() noexcept;
    /// Any task: text for TraceView's terminal (btpdev::printf()).
    void print(const char* text) noexcept;
    /// Any task: a shell line from the application (BtpDevice::run()).
    bool run(const char* line) noexcept;
    /// Any task: a line for TraceView's log panel (btpdev::log()). True when queued.
    bool log(LogLevel level, std::uint64_t timestamp_us, const char* text, std::size_t length) noexcept;
    /// True on the shell task (a command is running).
    bool in_command() const noexcept { return shell_task_ != nullptr && xTaskGetCurrentTaskHandle() == shell_task_; }
    /// The running device, for btpdev::printf() (one device per firmware).
    static DeviceCore* active() noexcept { return active_; }

    void routine() noexcept;
    void tick() noexcept;

    State state() const noexcept { return state_.load(); }
    std::uint8_t degraded() const noexcept { return degraded_.load(); }
    Link active_link() const noexcept { return active_link_; }
    bool connected() const noexcept { return state_.load() == State::Connected; }
    std::uint32_t device_source_id() const noexcept { return source_id; }
    std::uint32_t device_boot_id() const noexcept { return boot_id; }
    TinyShell& shell() noexcept { return shell_; }
    /// The file system the library uses (LittleFS, else the SD card), or nullptr.
    FileSystem* fs() noexcept;
    /// Saves the link for Link::FromSettings (applies at the next boot).
    esp_err_t set_link(Link link) noexcept;
    btp::Node* node() noexcept { return node_; }

    // ---- btp::NodeConfig (called by node_, on the comms context) ----------
    bool send(const std::uint8_t* frame, std::size_t frame_size) override;
    bool has_clock() const noexcept override { return true; }
    std::uint64_t clock() override;
    bool has_terminal() const noexcept override { return true; }
    void terminal(btp::Node& node, const btp::Header& header, btp::ByteView payload,
                  std::uint64_t now_ms) override;
    bool has_command() const noexcept override { return true; }
    bool has_seal() const noexcept override { return sealed_; }
    bool seal(const btp::Header& header, std::uint16_t payload_size, const std::uint8_t* plaintext,
              std::uint8_t* out) override;
    bool has_open() const noexcept override { return sealed_; }
    bool open(const btp::Header& header, std::uint16_t sealed_size, const std::uint8_t* sealed,
              std::uint8_t* out_plaintext) override;
    void reply_seal(const btp::Header& request_header, btp::EndpointSealFn* out_seal,
                    void** out_seal_ctx) override;
    void command(std::uint16_t action_id, std::uint16_t action_version, btp::ByteView parameters,
                 btp::NodeActionOutcome* outcome, const btp::NodeCommandTicket& ticket) override;

private:
    // Tags in front of every rx_queue_ entry: the connection events travel in
    // the same queue as the bytes, so they are seen in order.
    static constexpr std::uint8_t kTagData = 'D';
    static constexpr std::uint8_t kTagConnect = 'C';
    static constexpr std::uint8_t kTagDisconnect = 'X';
    static constexpr std::size_t kRxChunkMax = 1600U;   // >= every link's receive chunk
    static constexpr std::size_t kPeerLogicalPayload = 4096U;  // TraceView's HELLO max_logical_payload
    static constexpr std::size_t kAeadTagBytes = 16U;
    static constexpr std::uint32_t kHelloDeadlineMs = 2000U;
    static constexpr std::uint32_t kStatusPeriodMs = 1000U;
    static constexpr std::uint32_t kErrorReportPeriodMs = 1000U;
    static constexpr std::uint64_t kReassemblyTimeoutMs = 4000U;
    static constexpr std::size_t kTerminalChunk = 200U;
    static constexpr std::size_t kMaxWifiNetworks = 8U;

    struct ShellRequest {
        enum class Origin : std::uint8_t { Terminal, Remote, Local };
        Origin origin;
        std::uint32_t source_id;
        std::uint32_t boot_id;
        btp::NodeCommandTicket ticket;
        char line[kMaxShellLine + 1U];
    };
    struct ShellCompletion {
        ShellRequest::Origin origin;
        std::uint32_t source_id;
        std::uint32_t boot_id;
        btp::NodeCommandTicket ticket;
        std::uint8_t status;
        char output[Terminal::kMaxCommandOut + 1U];
    };

    // start() steps.
    void load_identity() noexcept;
    void mount_storage() noexcept;
    void load_key() noexcept;
    void load_wifi_networks() noexcept;
    Link resolve_link() noexcept;
    esp_err_t allocate(const PresetSizes& sizes) noexcept;
    esp_err_t create_node(const BtpDevice::TopicsFn& topics) noexcept;
    void fill_catalog() noexcept;
    esp_err_t create_link(Link link, const PresetSizes& sizes) noexcept;
    esp_err_t start_link_with_fallback(Link requested, const PresetSizes& sizes) noexcept;
    esp_err_t start_tasks() noexcept;
    void register_builtin_commands() noexcept;
    void shell_print(const char* text) noexcept;  // shell task, inside a command
    void register_storage_commands() noexcept;

    // Keys (docs/PLANO.md 4.9): which one seals / opens a message.
    bool channel_b(const btp::Header& header) const noexcept;
    const std::uint8_t* send_key(const btp::Header& header) const noexcept;
    static bool seal_with_e(void* context, const btp::Header& header, std::uint16_t payload_size,
                            const std::uint8_t* plaintext, std::uint8_t* out) noexcept;

    // Link driver callbacks (driver tasks).
    static void on_link_receive(void* context, const std::uint8_t* data, std::size_t size) noexcept;
    static void on_link_connect(void* context) noexcept;
    static void on_link_disconnect(void* context) noexcept;
    void push_rx(std::uint8_t tag, const std::uint8_t* data, std::size_t size) noexcept;
    void wake() noexcept;

    // Comms context.
    void drain_rx(std::uint64_t now_ms) noexcept;
    void handle_connect(std::uint64_t now_ms) noexcept;
    void handle_disconnect() noexcept;
    void handle_frame(const btp::DecodedFrame& frame, std::uint64_t now_ms) noexcept;
    void handle_datagram(const std::uint8_t* data, std::size_t size, std::uint64_t now_ms) noexcept;
    void after_receive(std::uint32_t peer_source_id) noexcept;
    void deliver_completion() noexcept;
    void report_errors(std::uint32_t now_ms) noexcept;
    static bool terminal_submit(void* context, std::uint32_t source_id, std::uint32_t boot_id,
                                const char* line);
    static bool terminal_emit(void* context, const std::uint8_t* data, std::size_t size);

    // Shell task.
    static void shell_task_entry(void* context);
    void shell_loop() noexcept;
    static void comms_task_entry(void* context);
    void comms_loop() noexcept;
    /// TaskConfig::timer: the esp_timer callback, which only wakes the task.
    static void on_comms_timer(void* context) noexcept;
    /// Largest stream block payload (see TopicRegistry::install()).
    std::size_t max_stream_payload() const noexcept;
    std::size_t max_message_payload() const noexcept;

    const DeviceConfig& device_;
    const StorageConfig& storage_;
    ErrorInfo& error_;

    std::atomic<State> state_{State::Stopped};
    std::atomic<std::uint8_t> degraded_{kDegradedNone};
    Link active_link_ = Link::Serial;
    std::uint8_t mac_[6] = {};
    std::uint8_t uuid_[16] = {};
    btp::Hello hello_{};
    bool session_link_ = false;   // stream links run a responder session
    bool has_key_ = false;
    bool sealed_ = false;         // has_key_, unless Serial with serial_cleartext
    DeviceKey key_;
    std::uint32_t dongle_source_id_ = 0U;   // ESP-NOW: the peer that holds key L

    // Storage.
    FileSystem fs_;   // LittleFS
    FileSystem sd_;   // SD card
    FileSystem app_;  // mounted by the application (StorageConfig::app_volume)

    // Node (one StaticNode of the chosen preset, placement-new'd).
    void* node_storage_ = nullptr;
    btp::Node* node_ = nullptr;
    btp::Catalog* catalog_ = nullptr;
    void (*destroy_node_)(btp::Node*) = nullptr;
    char fw_build_[40] = {};

    // Link.
    LinkDriver* link_ = nullptr;
    WifiNetwork networks_[kMaxWifiNetworks] = {};
    std::size_t network_count_ = 0U;

    // Receive side.
    LinkMutex rx_mutex_;
    Buffer rx_storage_;
    FrameQueue rx_queue_;          // guarded by rx_mutex_
    Buffer rx_chunk_;              // one entry copied out of rx_queue_ (comms)
    Buffer decoder_storage_;
    StreamDecoder decoder_;
    std::uint32_t peer_source_id_ = 0U;

    // Send side.
    Buffer tx_wire_;               // COBS scratch for stream links (comms)

    // Terminal and shell.
    Terminal terminal_;
    TinyShell shell_;
    QueueHandle_t shell_queue_ = nullptr;
    ShellRequest shell_request_{};           // shell task only
    ShellCompletion completion_{};           // shell writes, comms reads
    std::atomic<bool> completion_ready_{false};
    std::size_t capture_length_ = 0U;        // shell task only

    // Telemetry.
    TopicRegistry registry_;

    // btpdev::printf() from outside a command: any task -> comms -> terminal.
    static constexpr std::size_t kPrintQueueBytes = 2048U;
    LinkMutex print_mutex_;
    Buffer print_storage_;
    FrameQueue print_queue_;   // guarded by print_mutex_
    void drain_prints() noexcept;

    // btpdev::log(): any task -> comms -> LOG message.
    // Entry: level (1), timestamp_us (8, little-endian), text.
    static constexpr std::size_t kLogHead = 9U;
    static constexpr std::size_t kLogQueueBytes = 2048U;
    LinkMutex log_mutex_;
    Buffer log_storage_;
    FrameQueue log_queue_;     // guarded by log_mutex_
    void drain_logs(std::uint64_t now_ms) noexcept;

    static DeviceCore* active_;

    RuntimeErrors errors_;
    TaskHandle_t comms_task_ = nullptr;
    TaskHandle_t shell_task_ = nullptr;
    esp_timer_handle_t comms_timer_ = nullptr;        // TaskConfig::timer
    std::atomic<TaskHandle_t> manual_task_{nullptr};  // Threading::Manual owner
    std::atomic<bool> manual_refused_{false};         // logged once
    std::uint32_t last_tick_ms_ = 0U;
};

}  // namespace btpdev

#endif  // BTPDEV_DEVICE_CORE_H
