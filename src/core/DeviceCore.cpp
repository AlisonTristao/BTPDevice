#include "core/DeviceCore.h"

#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include <btpdev/version.h>
#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <esp_system.h>
#include <cstdarg>
#include <esp_timer.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <sdkconfig.h>

#include "core/ConfigCheck.h"
#include "core/Identity.h"
#include "core/LogTags.h"
#include "transport/BleLink.h"
#include "transport/EspNowLink.h"
#include "transport/TcpLink.h"
#include "transport/UsbSerialLink.h"

namespace btpdev {
namespace {

constexpr const char* kTag = tag::kCore;
constexpr const char* kNvsNamespace = "btpdev";

std::uint64_t now_ms() { return static_cast<std::uint64_t>(esp_timer_get_time() / 1000LL); }

// One StaticNode of preset M, placement-new'd into aligned heap storage.
template <Memory M>
bool make_node(btp::NodeConfig& config, std::uint64_t reassembly_ms, void** storage,
               btp::Node** node, btp::Catalog** catalog, void (**destroy)(btp::Node*)) {
    using N = typename NodePreset<M>::Node;
    void* memory = alloc_aligned(alignof(N), sizeof(N));
    if (memory == nullptr) return false;
    N* created = new (memory) N(config, reassembly_ms);
    *storage = memory;
    *node = created;
    *catalog = &created->catalog();
    *destroy = [](btp::Node* p) { static_cast<N*>(p)->~N(); };
    return true;
}

const btp::TransportLimits& transport_of(Link link) {
    switch (link) {
        case Link::Tcp: return btp::kTcpTransport;
        case Link::Ble: return btp::kBleTransport;
        case Link::EspNow: return btp::kEspNowTransport;
        default: return btp::kSerialTransport;
    }
}

const char* memory_name(Memory memory) {
    switch (memory) {
        case Memory::Small: return "Small";
        case Memory::Large: return "Large";
        default: return "Medium";
    }
}

}  // namespace

DeviceCore* DeviceCore::active_ = nullptr;

DeviceCore::DeviceCore(const DeviceConfig& device, const StorageConfig& storage,
                       ErrorInfo& error) noexcept
    : device_(device), storage_(storage), error_(error) {}

DeviceCore::~DeviceCore() {
    if (active_ == this) active_ = nullptr;
    if (comms_task_ != nullptr) vTaskDelete(comms_task_);
    if (shell_task_ != nullptr) vTaskDelete(shell_task_);
    if (link_ != nullptr) {
        link_->stop();
        delete link_;
    }
    if (node_ != nullptr && destroy_node_ != nullptr) destroy_node_(node_);
    free_buffer(node_storage_);
    if (shell_queue_ != nullptr) vQueueDelete(shell_queue_);
}

// ============================================================================
// start(): the whole of begin()
// ============================================================================

esp_err_t DeviceCore::start(BtpDevice::CommandsFn commands, BtpDevice::TopicsFn topics) noexcept {
    state_.store(State::Starting);
    const PresetSizes sizes = preset_sizes(device_.memory);

    load_identity();
    const Link requested = resolve_link();

    esp_err_t err = allocate(sizes);
    if (err != ESP_OK) return err;
    err = start_link_with_fallback(requested, sizes);
    if (err != ESP_OK) return err;
    err = create_node(topics);
    if (err != ESP_OK) return err;

    terminal_.configure(
        [this](const std::string& input, std::string* out, std::size_t max) -> std::size_t {
            const std::vector<std::string> matches = shell_.complete_line(input, max);
            std::size_t written = 0U;
            for (const std::string& match : matches) {
                if (written >= max) break;
                out[written++] = match;
            }
            return written;
        },
        &DeviceCore::terminal_submit, &DeviceCore::terminal_emit, this,
        (std::string(device_.name) + "> ").c_str(), kTerminalChunk);

    shell_.set_output_callback([this](std::string_view text) {
        // Shell task only, while a command runs: completion_ is not handed
        // over yet (completion_ready_ is false), so the comms task never reads
        // it concurrently.
        const std::size_t room = Terminal::kMaxCommandOut - capture_length_;
        const std::size_t take = text.size() < room ? text.size() : room;
        std::memcpy(completion_.output + capture_length_, text.data(), take);
        capture_length_ += take;
        completion_.output[capture_length_] = '\0';
    });
    shell_.begin();
    register_builtin_commands();
    if (commands != nullptr) {
        Commands declared(shell_);
        commands(declared);
        if (declared.error_code() != 0U) {
            return fail(error_, BTPDEV_ERR_BAD_COMMAND, tag::kCmd, "%s", declared.error_message());
        }
    }
    if (shell_.dropped_registrations() != 0U) {
        return fail(error_, BTPDEV_ERR_BAD_COMMAND, tag::kCmd,
                    "%u command(s) did not fit TinyShell's tables: raise TINYSHELL_MAX_* in menuconfig",
                    static_cast<unsigned>(shell_.dropped_registrations()));
    }
    if (!print_mutex_.create() || !print_storage_.allocate(kPrintQueueBytes)) {
        return fail(error_, BTPDEV_ERR_MEMORY_BUDGET, kTag, "no memory for the print queue");
    }
    print_queue_.bind(print_storage_.data(), print_storage_.size(), 0U);
    active_ = this;

    err = start_tasks();
    if (err != ESP_OK) return err;

    state_.store(State::Ready);
    ESP_LOGI(kTag, "\"%s\" ready on %s (source %08lx, boot %lu, memory %s)%s", device_.name,
             link_name(active_link_), static_cast<unsigned long>(source_id),
             static_cast<unsigned long>(boot_id), memory_name(device_.memory),
             has_key_ ? "" : ", no key: traffic in clear");
    return ESP_OK;
}

void DeviceCore::load_identity() noexcept {
    esp_efuse_mac_get_default(mac_);
    source_id = source_id_from_mac(mac_);
    uuid_from_mac(mac_, uuid_);

    // The boot counter. Never erase NVS to get it: the device key lives there
    // (docs/PLANO.md 4.9). Without NVS the boot_id is random, which still
    // tells peers this is a new boot.
    esp_err_t err = nvs_flash_init();
    nvs_handle_t handle = 0;
    if (err == ESP_OK) err = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    std::uint32_t previous = 0U;
    if (err == ESP_OK) {
        const esp_err_t get = nvs_get_u32(handle, "boot", &previous);
        if (get != ESP_OK && get != ESP_ERR_NVS_NOT_FOUND) err = get;
    }
    if (err == ESP_OK) {
        boot_id = next_boot_id(previous);
        err = nvs_set_u32(handle, "boot", boot_id);
        if (err == ESP_OK) err = nvs_commit(handle);
    }
    if (handle != 0) nvs_close(handle);
    if (err != ESP_OK) {
        boot_id = esp_random() | 1U;
        ESP_LOGW(kTag,
                 "NVS unavailable (%s): boot counter not saved, using a random boot id. "
                 "If NVS is full or from an older layout, erase it (idf.py erase-flash) and "
                 "provision the key again",
                 esp_err_to_name(err));
    }
}

Link DeviceCore::resolve_link() noexcept {
    Link requested = device_.link;
    if (requested == Link::FromSettings) {
        requested = device_.fallback;
        nvs_handle_t handle = 0;
        std::uint8_t saved = 0U;
        if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) == ESP_OK) {
            if (nvs_get_u8(handle, "link", &saved) == ESP_OK && saved <= static_cast<std::uint8_t>(Link::EspNow)) {
                requested = static_cast<Link>(saved);
            }
            nvs_close(handle);
        }
    }

    has_key_ = false;  // Phase 5: the key store
    degraded_.fetch_or(kNoKey);
    if (requested != Link::Serial && !has_key_) {
#if CONFIG_BTPDEV_INSECURE_CLEARTEXT_RADIO
        ESP_LOGW(tag::kKey,
                 "no device key: %s runs WITHOUT encryption (CONFIG_BTPDEV_INSECURE_CLEARTEXT_RADIO). "
                 "Development only",
                 link_name(requested));
#else
        fail(error_, BTPDEV_ERR_NO_KEY, tag::kKey,
             "%s needs the device key and there is none: using Serial instead. Provision the "
             "key, or enable CONFIG_BTPDEV_INSECURE_CLEARTEXT_RADIO for bench tests",
             link_name(requested));
        ESP_LOGE(error_.module, "%s", error_.message);
        degraded_.fetch_or(kLinkFallback);
        requested = Link::Serial;
#endif
    }
    return requested;
}

esp_err_t DeviceCore::allocate(const PresetSizes& sizes) noexcept {
    const std::size_t needed = sizes.node_bytes + sizes.link_queue_bytes + sizes.rx_queue_bytes +
                               StreamDecoder::kStorageSize + kRxChunkMax +
                               stream_encoded_capacity(kTxFrameMax);
    const std::size_t available = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    if (needed > available) {
        return fail(error_, BTPDEV_ERR_MEMORY_BUDGET, kTag,
                    "memory preset %s needs about %u bytes and only %u are free: use a smaller "
                    "preset (DeviceConfig::memory)",
                    memory_name(device_.memory), static_cast<unsigned>(needed),
                    static_cast<unsigned>(available));
    }
    if (!rx_mutex_.create() || !rx_storage_.allocate(sizes.rx_queue_bytes) ||
        !rx_chunk_.allocate(kRxChunkMax) ||
        !tx_wire_.allocate(stream_encoded_capacity(kTxFrameMax))) {
        return fail(error_, BTPDEV_ERR_MEMORY_BUDGET, kTag, "no memory for the receive/send buffers");
    }
    rx_queue_.bind(rx_storage_.data(), rx_storage_.size(), 0U);
    shell_queue_ = xQueueCreate(2, sizeof(ShellRequest));
    if (shell_queue_ == nullptr) {
        return fail(error_, BTPDEV_ERR_MEMORY_BUDGET, kTag, "no memory for the shell queue");
    }
    return ESP_OK;
}

esp_err_t DeviceCore::create_link(Link link, const PresetSizes& sizes) noexcept {
    delete link_;
    link_ = nullptr;
    switch (link) {
        case Link::Tcp: {
            network_count_ = 0U;
            if (device_.wifi.ssid != nullptr && device_.wifi.ssid[0] != '\0') {
                std::snprintf(networks_[0].ssid, sizeof(networks_[0].ssid), "%s", device_.wifi.ssid);
                std::snprintf(networks_[0].password, sizeof(networks_[0].password), "%s",
                              device_.wifi.password != nullptr ? device_.wifi.password : "");
                network_count_ = 1U;
            }
            // Phase 5 appends the networks file here.
            TcpLinkConfig config;
            config.device_name = device_.name;
            config.port = device_.tcp.port;
            config.send_queue_bytes = sizes.link_queue_bytes;
            config.networks = networks_;
            config.network_count = network_count_;
            config.task_core = device_.comms.core;
            link_ = new (std::nothrow) TcpLink(config);
            break;
        }
        case Link::Ble: {
            BleLinkConfig config;
            config.device_name = device_.name;
            config.send_queue_bytes = sizes.link_queue_bytes / 2U;
            link_ = new (std::nothrow) BleLink(config);
            break;
        }
        case Link::EspNow: {
            EspNowLinkConfig config;
            std::memcpy(config.peer_mac, device_.espnow.peer_mac, sizeof(config.peer_mac));
            config.channel = device_.espnow.channel;
            config.normal_queue_bytes = sizes.link_queue_bytes / 2U;
            config.telemetry_queue_bytes = sizes.link_queue_bytes / 4U;
            link_ = new (std::nothrow) EspNowLink(config);
            break;
        }
        default: {
            UsbSerialConfig config;
            config.product_name = device_.name;
            config.send_queue_bytes = sizes.link_queue_bytes;
            link_ = new (std::nothrow) UsbSerialLink(config);
            break;
        }
    }
    if (link_ == nullptr) return fail(error_, ESP_ERR_NO_MEM, kTag, "no memory for the %s link", link_name(link));
    if (link_->kind() == LinkDriver::Kind::Stream && !decoder_.bound()) {
        if (!decoder_storage_.allocate(StreamDecoder::kStorageSize) ||
            !decoder_.bind(decoder_storage_.data(), decoder_storage_.size())) {
            return fail(error_, BTPDEV_ERR_MEMORY_BUDGET, kTag, "no memory for the stream decoder");
        }
    }
    LinkEvents events;
    events.context = this;
    events.on_receive = &DeviceCore::on_link_receive;
    events.on_connect = &DeviceCore::on_link_connect;
    events.on_disconnect = &DeviceCore::on_link_disconnect;
    return link_->start(events, error_);
}

esp_err_t DeviceCore::start_link_with_fallback(Link requested, const PresetSizes& sizes) noexcept {
    esp_err_t err = create_link(requested, sizes);
    if (err != ESP_OK && requested != Link::Serial) {
        ESP_LOGE(error_.module, "%s [%s]", error_.message, err_name(err));
        ESP_LOGE(kTag, "%s did not start: falling back to Serial", link_name(requested));
        degraded_.fetch_or(kLinkFallback);
        requested = Link::Serial;
        err = create_link(requested, sizes);
    }
    if (err != ESP_OK) return err;
    active_link_ = requested;
    session_link_ = link_->kind() == LinkDriver::Kind::Stream;
    transport = transport_of(requested);
    return ESP_OK;
}

esp_err_t DeviceCore::create_node(BtpDevice::TopicsFn topics) noexcept {
    // NodeConfig fields read by the node's constructor / begin().
    bool made = false;
    switch (device_.memory) {
        case Memory::Small:
            made = make_node<Memory::Small>(*this, kReassemblyTimeoutMs, &node_storage_, &node_, &catalog_, &destroy_node_);
            break;
        case Memory::Large:
            made = make_node<Memory::Large>(*this, kReassemblyTimeoutMs, &node_storage_, &node_, &catalog_, &destroy_node_);
            break;
        default:
            made = make_node<Memory::Medium>(*this, kReassemblyTimeoutMs, &node_storage_, &node_, &catalog_, &destroy_node_);
            break;
    }
    if (!made) return fail(error_, BTPDEV_ERR_MEMORY_BUDGET, kTag, "no memory for the BTP node");

    fill_catalog();
    if (topics != nullptr) {
        Topics declared;
        topics(declared);
        const esp_err_t installed = registry_.install(declared, *node_, *catalog_,
                                                      preset_sizes(device_.memory).catalog_topics, error_);
        if (installed != ESP_OK) return installed;
        ESP_LOGI(tag::kTopic, "%u topic(s) declared", static_cast<unsigned>(registry_.size()));
    }
    node_->serve_catalog(catalog_, static_cast<std::uint8_t>(btp::Role::Producer), uuid_, device_.name);
    node_->enable_status(kStatusPeriodMs);
    if (!node_->begin(false)) {
        return fail(error_, ESP_FAIL, kTag, "btp::Node::begin() failed (identity or storage)");
    }
    const PresetSizes sizes = preset_sizes(device_.memory);
    hello_ = btp::HelloBuilder(btp::Role::Producer, uuid_)
                 .max_logical_payload(static_cast<std::uint32_t>(sizes.slot_bytes))
                 .max_inflight_reassemblies(static_cast<std::uint16_t>(sizes.slots))
                 .max_subscriptions(static_cast<std::uint16_t>(sizes.max_subscriptions))
                 .max_dedup_entries(static_cast<std::uint32_t>(sizes.max_commands))
                 .config_revision(catalog_->config_revision())
                 .build();
    if (session_link_) node_->enable_session(hello_, kHelloDeadlineMs);
    return ESP_OK;
}

void DeviceCore::fill_catalog() noexcept {
    const esp_app_desc_t* app = esp_app_get_description();
    if (app != nullptr) std::snprintf(fw_build_, sizeof(fw_build_), "%s %s", app->date, app->time);
    struct Entry {
        const char* key;
        const char* label;
        const char* value;
    };
    const Entry automatic[] = {
        {"name", "Name", device_.name},
        {"fw_version", "Firmware", app != nullptr ? app->version : ""},
        {"fw_build", "Built", fw_build_},
        {"idf_version", "ESP-IDF", app != nullptr ? app->idf_ver : ""},
        {"chip", "Chip", CONFIG_IDF_TARGET},
        {"btpdevice", "BTPDevice", BTPDEV_VERSION_STRING},
    };
    auto add = [this](const char* key, const char* label, const char* value) {
        if (catalog_->add_source_info(key, label != nullptr ? label : "", value) != btp::MessageError::Ok) {
            ESP_LOGW(kTag, "source_info \"%s\" left out: the memory preset's pool is full", key);
        }
    };
    for (const Entry& e : automatic) add(e.key, e.label, e.value);
    for (const SourceInfo& e : device_.source_info) {
        if (e.key == nullptr) break;
        add(e.key, e.label, e.value);
    }
    // The revision is a digest of the content: a changed topic changes it, and
    // TraceView's manifest cache notices by itself.
    catalog_->set_config_revision_auto();
}

esp_err_t DeviceCore::start_tasks() noexcept {
    if (xTaskCreatePinnedToCore(&DeviceCore::shell_task_entry, "btpdev_shell", 8192, this,
                                device_.comms.priority > 1U ? device_.comms.priority - 1U : 1U,
                                &shell_task_, tskNO_AFFINITY) != pdPASS) {
        return fail(error_, ESP_ERR_NO_MEM, kTag, "could not create the shell task");
    }
    if (device_.threading == Threading::Auto &&
        xTaskCreatePinnedToCore(&DeviceCore::comms_task_entry, "btpdev_comms", device_.comms.stack_bytes,
                                this, device_.comms.priority, &comms_task_,
                                device_.comms.core < 0 ? tskNO_AFFINITY : device_.comms.core) != pdPASS) {
        return fail(error_, ESP_ERR_NO_MEM, kTag, "could not create the communication task (%u bytes)",
                    static_cast<unsigned>(device_.comms.stack_bytes));
    }
    return ESP_OK;
}

// ============================================================================
// Link driver side: copy and wake, nothing else
// ============================================================================

void DeviceCore::on_link_receive(void* context, const std::uint8_t* data, std::size_t size) noexcept {
    static_cast<DeviceCore*>(context)->push_rx(kTagData, data, size);
}

void DeviceCore::on_link_connect(void* context) noexcept {
    static_cast<DeviceCore*>(context)->push_rx(kTagConnect, nullptr, 0U);
}

void DeviceCore::on_link_disconnect(void* context) noexcept {
    static_cast<DeviceCore*>(context)->push_rx(kTagDisconnect, nullptr, 0U);
}

void DeviceCore::push_rx(std::uint8_t tag, const std::uint8_t* data, std::size_t size) noexcept {
    bool queued = false;
    {
        LinkLock lock(rx_mutex_, pdMS_TO_TICKS(5));
        if (lock.locked()) {
            // A connection event carries no body: the tag is the whole entry.
            queued = size == 0U ? rx_queue_.push(&tag, 1U, Priority::Normal)
                                : rx_queue_.push(&tag, 1U, data, size, Priority::Normal);
        }
    }
    if (!queued) errors_.note(RuntimeError::RxOverflow, static_cast<std::uint32_t>(size + 1U));
    wake();
}

void DeviceCore::wake() noexcept {
    TaskHandle_t task = comms_task_;
    if (task != nullptr) xTaskNotifyGive(task);
}

// ============================================================================
// Communication context
// ============================================================================

void DeviceCore::comms_task_entry(void* context) { static_cast<DeviceCore*>(context)->comms_loop(); }

void DeviceCore::comms_loop() noexcept {
    for (;;) {
        // Woken at once by received bytes or a finished command; otherwise
        // once per period for telemetry, the watchdog and link housekeeping.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(device_.comms.period_ms));
        routine();
    }
}

void DeviceCore::routine() noexcept {
    const State state = state_.load();
    if (state != State::Ready && state != State::Connected) return;
    const std::uint64_t now = now_ms();
    drain_rx(now);
    deliver_completion();
    drain_prints();
    terminal_.pump(static_cast<std::uint32_t>(now));
    link_->poll(static_cast<std::uint32_t>(now));
    tick();
}

void DeviceCore::tick() noexcept {
    const std::uint64_t now = now_ms();
    node_->routine(now);  // due field topics, session watchdog, STATUS
    registry_.publish_pending(*node_, now);  // text topics and event publishes
    if (state_.load() == State::Connected && node_->session_event() == btp::SessionEvent::TimedOut) {
        // The peer went quiet past the negotiated timeout (TraceView keeps an
        // idle session alive every ~5 s): it is gone, even if the transport
        // did not notice. Drop it so the next one can connect.
        errors_.note(RuntimeError::SessionTimeout);
        state_.store(State::Ready);
        terminal_.reset();
        link_->disconnect_peer();
    }
    report_errors(static_cast<std::uint32_t>(now));
}

void DeviceCore::drain_rx(std::uint64_t now) noexcept {
    for (int budget = 0; budget < 32; ++budget) {  // bounded: a flood cannot starve the rest
        std::size_t size = 0U;
        {
            LinkLock lock(rx_mutex_, pdMS_TO_TICKS(5));
            if (!lock.locked() || rx_queue_.empty()) return;
            size = rx_queue_.copy_front(rx_chunk_.data(), rx_chunk_.size());
            rx_queue_.pop_front();
        }
        if (size == 0U) continue;  // larger than any link sends: dropped
        const std::uint8_t tag = rx_chunk_.data()[0];
        const std::uint8_t* body = rx_chunk_.data() + 1;
        const std::size_t body_size = size - 1U;
        if (tag == kTagConnect) {
            handle_connect(now);
        } else if (tag == kTagDisconnect) {
            handle_disconnect();
        } else if (link_->kind() == LinkDriver::Kind::Datagram) {
            handle_datagram(body, body_size, now);
        } else {
            const StreamDecoder::Stats before = decoder_.stats();
            decoder_.feed(body, body_size,
                          [this, now](const btp::DecodedFrame& frame) { handle_frame(frame, now); });
            const StreamDecoder::Stats& after = decoder_.stats();
            const std::uint32_t corrupt = (after.cobs_errors - before.cobs_errors) +
                                          (after.frame_errors - before.frame_errors) +
                                          (after.overflows - before.overflows);
            if (corrupt != 0U) errors_.note(RuntimeError::StreamCorrupt, corrupt);
        }
    }
}

void DeviceCore::handle_connect(std::uint64_t now) noexcept {
    decoder_.reset();
    terminal_.reset();
    node_->reset_link(0U);
    if (session_link_) node_->arm_session(now);
    ESP_LOGI(tag::kCore, "%s: peer connected, waiting for HELLO", link_name(active_link_));
}

void DeviceCore::handle_disconnect() noexcept {
    decoder_.reset();
    terminal_.reset();
    node_->reset_link(0U);
    if (state_.load() == State::Connected) state_.store(State::Ready);
    ESP_LOGI(tag::kCore, "%s: peer disconnected", link_name(active_link_));
}

void DeviceCore::handle_frame(const btp::DecodedFrame& frame, std::uint64_t now) noexcept {
    // A HELLO while the session is not waiting for one (already Active, or
    // back to Idle after a timeout) means a new client on a link that did not
    // drop -- Windows keeps a BLE link a while after TraceView closes, a
    // serial port may be reopened without toggling DTR. Start over.
    if (session_link_ && frame.header.type == btp::MessageType::Control &&
        frame.header.object_id == btp::object_id::kHello) {
        const btp::Session* session = node_->link_session(0U);
        if (session == nullptr || session->state() != btp::SessionState::AwaitingHello) {
            node_->reset_link(0U);
            node_->arm_session(now);
            terminal_.reset();
            if (state_.load() == State::Connected) state_.store(State::Ready);
        }
    }
    btp::ReceivedMessage message{};
    node_->receive(frame, now, &message);
    after_receive(frame.header.source_id);
}

void DeviceCore::handle_datagram(const std::uint8_t* data, std::size_t size, std::uint64_t now) noexcept {
    btp::ReceivedMessage message{};
    node_->receive(data, size, now, &message);
}

void DeviceCore::after_receive(std::uint32_t peer) noexcept {
    switch (node_->session_event()) {
        case btp::SessionEvent::HelloAccepted:
            if (state_.load() != State::Connected) {
                peer_source_id_ = peer;
                state_.store(State::Connected);
                ESP_LOGI(tag::kCore, "session open with %08lx over %s", static_cast<unsigned long>(peer),
                         link_name(active_link_));
            }
            break;
        case btp::SessionEvent::HelloRejected:
            ESP_LOGW(tag::kCore, "HELLO from %08lx rejected (malformed or no common BTP version)",
                     static_cast<unsigned long>(peer));
            break;
        case btp::SessionEvent::SessionClosed:
            if (state_.load() == State::Connected) {
                state_.store(State::Ready);
                terminal_.reset();
                ESP_LOGI(tag::kCore, "session closed by %08lx", static_cast<unsigned long>(peer));
            }
            break;
        default:
            break;
    }
}

bool DeviceCore::send(const std::uint8_t* frame, std::size_t frame_size) {
    // No peer: STATUS and telemetry simply have nowhere to go. Not an error.
    if (link_ == nullptr || !link_->connected()) return false;
    // TELEMETRY (1) and LOG (2) are the first to be refused when the link backs up.
    const Priority priority = frame_size > 5U && (frame[5] == 0x01U || frame[5] == 0x02U)
                                  ? Priority::Telemetry
                                  : Priority::Normal;
    bool sent = false;
    if (link_->kind() == LinkDriver::Kind::Datagram) {
        sent = link_->send(frame, frame_size, priority);
    } else {
        std::size_t wire_size = 0U;
        sent = stream_encode(frame, frame_size, tx_wire_.data(), tx_wire_.size(), &wire_size) &&
               link_->send(tx_wire_.data(), wire_size, priority);
    }
    if (!sent) errors_.note(RuntimeError::TxRefused);
    return sent;
}

std::uint64_t DeviceCore::clock() { return now_ms(); }

void DeviceCore::terminal(btp::Node&, const btp::Header& header, btp::ByteView payload,
                          std::uint64_t now) {
    if (header.object_id != btp::object_id::kTerminalIn) return;
    terminal_.on_input(header.source_id, header.boot_id, payload.data, payload.size,
                       static_cast<std::uint32_t>(now));
}

bool DeviceCore::terminal_emit(void* context, const std::uint8_t* data, std::size_t size) {
    auto* self = static_cast<DeviceCore*>(context);
    return self->node_->send(btp::MessageType::Terminal, btp::object_id::kTerminalOut, data, size,
                             static_cast<std::uint64_t>(esp_timer_get_time()));
}

bool DeviceCore::terminal_submit(void* context, std::uint32_t source_id, std::uint32_t boot_id,
                                 const char* line) {
    auto* self = static_cast<DeviceCore*>(context);
    ShellRequest request{};
    request.origin = ShellRequest::Origin::Terminal;
    request.source_id = source_id;
    request.boot_id = boot_id;
    std::snprintf(request.line, sizeof(request.line), "%s", line);
    if (xQueueSend(self->shell_queue_, &request, 0) == pdTRUE) return true;
    self->errors_.note(RuntimeError::ShellBusy);
    return false;
}

void DeviceCore::command(std::uint16_t action_id, std::uint16_t action_version,
                         btp::ByteView parameters, btp::NodeActionOutcome* outcome,
                         const btp::NodeCommandTicket& ticket) {
    if (action_id == kPingActionId && action_version == kPingActionVersion) {
        return;  // pre-set to Success: measures the round trip only
    }
    if (action_id != kShellActionId || action_version != kShellActionVersion) {
        outcome->status = static_cast<std::uint8_t>(btp::ResultStatus::Unsupported);
        outcome->error_code = static_cast<std::uint16_t>(btp::ResultError::UnknownObject);
        outcome->message = "unknown action (1 = shell line, 2 = ping)";
        return;
    }
    ShellRequest request{};
    request.origin = ShellRequest::Origin::Remote;
    request.ticket = ticket;
    const ShellTextError text = copy_shell_line(parameters, request.line, sizeof(request.line));
    if (text != ShellTextError::Ok) {
        outcome->status = static_cast<std::uint8_t>(btp::ResultStatus::Rejected);
        outcome->error_code = static_cast<std::uint16_t>(btp::ResultError::InvalidArgument);
        outcome->message = shell_text_error_message(text);
        return;
    }
    if (xQueueSend(shell_queue_, &request, 0) != pdTRUE) {
        errors_.note(RuntimeError::ShellBusy);
        outcome->status = static_cast<std::uint8_t>(btp::ResultStatus::Busy);
        outcome->error_code = static_cast<std::uint16_t>(btp::ResultError::CapacityExhausted);
        outcome->message = "shell busy, try again";
        return;
    }
    outcome->pending = true;  // answered in deliver_completion()
}

void DeviceCore::deliver_completion() noexcept {
    if (!completion_ready_.load(std::memory_order_acquire)) return;
    const ShellCompletion& done = completion_;
    if (done.origin == ShellRequest::Origin::Terminal) {
        terminal_.deliver(done.source_id, done.boot_id, done.output, done.status);
    } else {
        // COMMAND_RESULT carries at most kMaxResultMessage of the output; the
        // terminal is where long output belongs.
        char message[kMaxResultMessage + 1U];
        const char* source = done.output[0] != '\0' ? done.output
                                                    : (done.status == 0U ? "" : "shell command failed");
        const std::size_t length = strnlen(source, kMaxResultMessage);  // cut on purpose
        std::memcpy(message, source, length);
        message[length] = '\0';
        btp::NodeActionOutcome outcome{};
        if (done.status == RESULT_OK) {
            outcome.status = static_cast<std::uint8_t>(btp::ResultStatus::Success);
        } else if (done.status == FUNCTION_NOT_FOUND || done.status == MODULE_NOT_FOUND) {
            outcome.status = static_cast<std::uint8_t>(btp::ResultStatus::Rejected);
            outcome.error_code = static_cast<std::uint16_t>(btp::ResultError::NotFound);
        } else {
            outcome.status = static_cast<std::uint8_t>(btp::ResultStatus::Failed);
            outcome.error_code = static_cast<std::uint16_t>(btp::ResultError::InternalError);
        }
        outcome.message = message;
        // False when the link was reset meanwhile: the requester is gone.
        node_->complete_command(done.ticket, outcome);
    }
    completion_ready_.store(false, std::memory_order_release);
}

void DeviceCore::report_errors(std::uint32_t now) noexcept {
    errors_.report(now, kErrorReportPeriodMs, [](RuntimeError kind, std::uint32_t count, bool first) {
        if (first) {
            ESP_LOGW(kTag, "%s (x%lu)", runtime_error_name(kind), static_cast<unsigned long>(count));
        } else {
            ESP_LOGW(kTag, "%s: %lu more in the last second", runtime_error_name(kind),
                     static_cast<unsigned long>(count));
        }
    });
}

bool DeviceCore::publish(const char* topic) noexcept {
    const bool known = registry_.request_publish(topic);
    if (known) wake();
    return known;
}

void DeviceCore::print(const char* text) noexcept {
    if (text == nullptr || text[0] == '\0') return;
    if (xTaskGetCurrentTaskHandle() == shell_task_) {
        shell_print(text);  // inside a command: part of its output
        return;
    }
    bool queued = false;
    {
        LinkLock lock(print_mutex_, pdMS_TO_TICKS(2));
        if (lock.locked()) {
            queued = print_queue_.push(reinterpret_cast<const std::uint8_t*>(text), std::strlen(text),
                                       Priority::Normal);
        }
    }
    if (!queued) errors_.note(RuntimeError::TerminalDropped);
    wake();
}

void DeviceCore::drain_prints() noexcept {
    char line[Terminal::kMaxAsyncOut + 1U];
    for (int budget = 0; budget < 8; ++budget) {
        std::size_t size = 0U;
        {
            LinkLock lock(print_mutex_, pdMS_TO_TICKS(2));
            if (!lock.locked() || print_queue_.empty()) return;
            size = print_queue_.copy_front(reinterpret_cast<std::uint8_t*>(line), sizeof(line) - 1U);
            print_queue_.pop_front();
        }
        line[size] = '\0';
        terminal_.async_line(line);
    }
}

// ============================================================================
// Shell task
// ============================================================================

void DeviceCore::shell_task_entry(void* context) { static_cast<DeviceCore*>(context)->shell_loop(); }

void DeviceCore::shell_loop() noexcept {
    for (;;) {
        if (xQueueReceive(shell_queue_, &shell_request_, portMAX_DELAY) != pdTRUE) continue;
        // One completion slot: wait until the comms task took the previous one.
        while (completion_ready_.load(std::memory_order_acquire)) vTaskDelay(1);
        capture_length_ = 0U;
        completion_.output[0] = '\0';
        const std::uint8_t status = shell_.run_command_line(shell_request_.line);
        completion_.origin = shell_request_.origin;
        completion_.source_id = shell_request_.source_id;
        completion_.boot_id = shell_request_.boot_id;
        completion_.ticket = shell_request_.ticket;
        completion_.status = status;
        completion_ready_.store(true, std::memory_order_release);
        wake();
    }
}

void DeviceCore::shell_print(const char* text) noexcept {
    // Same sink as TinyShell's own output: the running command's capture.
    if (text == nullptr) return;
    const std::size_t length = std::strlen(text);
    const std::size_t room = Terminal::kMaxCommandOut - capture_length_;
    const std::size_t take = length < room ? length : room;
    std::memcpy(completion_.output + capture_length_, text, take);
    capture_length_ += take;
    completion_.output[capture_length_] = '\0';
}

void DeviceCore::register_builtin_commands() noexcept {
    // One module, "btp", so it never collides with the application's own.
    shell_.create_module("btp", "BTPDevice: link, session and errors");
    shell_.add(
        [this]() -> std::uint8_t {
            const LinkStats s = link_->stats();
            char text[512];
            std::snprintf(
                text, sizeof(text),
                "device    %s\n"
                "link      %s%s\n"
                "state     %s\n"
                "source    %08lx  boot %lu\n"
                "peer      %08lx\n"
                "link io   in %lu B, out %lu B, refused %lu\n"
                "memory    preset %s, %u bytes free\n"
                "key       %s\n",
                device_.name, link_name(active_link_),
                (degraded_.load() & kLinkFallback) != 0U ? " (fallback)" : "",
                state_name(state_.load()), static_cast<unsigned long>(source_id),
                static_cast<unsigned long>(boot_id), static_cast<unsigned long>(peer_source_id_),
                static_cast<unsigned long>(s.bytes_in), static_cast<unsigned long>(s.bytes_out),
                static_cast<unsigned long>(s.send_refused), memory_name(device_.memory),
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
                has_key_ ? "loaded" : "none (traffic in clear)");
            shell_print(text);
            return RESULT_OK;
        },
        "status", "Link, session, identity and memory", "btp");
    shell_.add(
        [this]() -> std::uint8_t {
            char line[96];
            for (std::size_t i = 0U; i < RuntimeErrors::kKinds; ++i) {
                const auto kind = static_cast<RuntimeError>(i);
                std::snprintf(line, sizeof(line), "%6lu  %s\n",
                              static_cast<unsigned long>(errors_.total(kind)), runtime_error_name(kind));
                shell_print(line);
            }
            return RESULT_OK;
        },
        "errors", "Runtime error counters since boot", "btp");
    shell_.add(
        [this]() -> std::uint8_t {
            if (registry_.size() == 0U) {
                shell_print("no topics declared\n");
                return RESULT_OK;
            }
            // Declarations only: the subscription table belongs to the
            // communication task and is not read from here.
            for (std::size_t i = 0U; i < catalog_->topic_count(); ++i) {
                const btp::CatalogTopic* t = catalog_->topic_at(i);
                if (t == nullptr) continue;
                char line[96];
                std::snprintf(line, sizeof(line), "0x%04x  %-20s %2u field(s)  max %lu mHz\n",
                              static_cast<unsigned>(t->topic_id), t->name,
                              static_cast<unsigned>(t->field_count),
                              static_cast<unsigned long>(t->max_rate_millihz));
                shell_print(line);
            }
            return RESULT_OK;
        },
        "topics", "Declared telemetry topics", "btp");
    shell_.add(
        [this]() -> std::uint8_t {
            shell_print("restarting in 0.5 s\n");
            // Later, on a timer: the reply has to leave before the reset.
            esp_timer_create_args_t args{};
            args.callback = [](void*) { esp_restart(); };
            args.name = "btpdev_reboot";
            esp_timer_handle_t timer = nullptr;
            if (esp_timer_create(&args, &timer) != ESP_OK || esp_timer_start_once(timer, 500000) != ESP_OK) {
                return RESULT_ERROR;
            }
            return RESULT_OK;
        },
        "reboot", "Restart the device", "btp");
}

void printf(const char* format, ...) noexcept {
    DeviceCore* core = DeviceCore::active();
    if (core == nullptr || format == nullptr) return;
    char text[Terminal::kMaxAsyncOut];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    core->print(text);
}

}  // namespace btpdev
