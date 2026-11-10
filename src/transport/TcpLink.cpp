#include "transport/TcpLink.h"

#include <sdkconfig.h>

#include "core/LogTags.h"

#if CONFIG_BTPDEV_LINK_TCP
#include <cerrno>
#include <cstring>

#include <esp_log.h>
#include <esp_timer.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>
#include <mdns.h>
#endif

namespace btpdev {

#if CONFIG_BTPDEV_LINK_TCP

namespace {

constexpr const char* kTag = tag::kTcp;
constexpr int kListenBacklog = 2;
// Also the worst-case wait for a frame queued by another task: select() only
// watches for writability when the queue was non-empty as it went to sleep.
// At 200 ms bally_OS turned 50 Hz telemetry into 5 Hz bursts.
constexpr int kPollTimeoutMs = 10;

std::uint32_t now_ms() {
    return static_cast<std::uint32_t>(esp_timer_get_time() / 1000LL);
}

void close_socket(int fd) {
    if (fd >= 0) {
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
    }
}

}  // namespace

TcpLink::~TcpLink() { stop(); }

esp_err_t TcpLink::start(const LinkEvents& events, ErrorInfo& error) noexcept {
    if (running_.load()) return ESP_OK;
    if (events.on_receive == nullptr || config_.port == 0U) {
        return fail(error, ESP_ERR_INVALID_ARG, kTag, "TCP link started without on_receive or port");
    }
    if (!mutex_.create()) return fail(error, ESP_ERR_NO_MEM, kTag, "no memory for the TCP link mutex");
    if (!queue_storage_.allocate(config_.send_queue_bytes)) {
        return fail(error, BTPDEV_ERR_MEMORY_BUDGET, kTag, "no memory for the TCP send queue (%u bytes)",
                    static_cast<unsigned>(config_.send_queue_bytes));
    }
    queue_.bind(queue_storage_.data(), queue_storage_.size(), queue_storage_.size() / 4U);
    events_ = events;

    // The same host name for DHCP (the router's client list) and for mDNS
    // (<name>.local): see mdns_host_from_name().
    char host[64];
    const bool has_host = mdns_host_from_name(config_.device_name, host, sizeof(host));
    esp_err_t err = wifi_.start(config_.networks, config_.network_count, error, has_host ? host : nullptr);
    if (err != ESP_OK) {
        queue_storage_.reset();
        return err;
    }
    // Binding to INADDR_ANY works before there is an address: the server is
    // ready the moment DHCP answers, and stays up across Wi-Fi reconnections.
    err = open_listener(error);
    if (err != ESP_OK) {
        wifi_.stop();
        queue_storage_.reset();
        return err;
    }

    running_.store(true);
    const BaseType_t created = xTaskCreatePinnedToCore(
        &TcpLink::task_entry, "btpdev_tcp", config_.task_stack_bytes, this,
        config_.task_priority, &task_, config_.task_core < 0 ? tskNO_AFFINITY : config_.task_core);
    if (created != pdPASS) {
        running_.store(false);
        close_socket(listen_fd_);
        listen_fd_ = -1;
        wifi_.stop();
        queue_storage_.reset();
        return fail(error, ESP_ERR_NO_MEM, kTag, "could not create the TCP server task (%u bytes of stack)",
                    static_cast<unsigned>(config_.task_stack_bytes));
    }
    ESP_LOGI(kTag, "server on port %u, waiting for Wi-Fi", static_cast<unsigned>(config_.port));
    return ESP_OK;
}

esp_err_t TcpLink::open_listener(ErrorInfo& error) noexcept {
    const int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) return fail(error, BTPDEV_ERR_LINK_START, kTag, "socket() failed (errno %d)", errno);
    int reuse = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(config_.port);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
        ::listen(fd, kListenBacklog) < 0) {
        const int e = errno;
        ::close(fd);
        return fail(error, BTPDEV_ERR_LINK_START, kTag, "cannot listen on TCP port %u (errno %d)",
                    static_cast<unsigned>(config_.port), e);
    }
    listen_fd_ = fd;
    return ESP_OK;
}

void TcpLink::stop() noexcept {
    if (!running_.exchange(false)) return;
    const int listen_fd = listen_fd_;
    listen_fd_ = -1;
    close_socket(listen_fd);
    close_client();
    close_pending();
    while (task_ != nullptr) vTaskDelay(1);  // run() clears it on exit
    wifi_.stop();
    {
        LinkLock lock(mutex_);
        queue_.bind(nullptr, 0U, 0U);
    }
    queue_storage_.reset();
}

void TcpLink::poll(std::uint32_t now) noexcept {
    wifi_.poll(now);
    if (wifi_.has_ip() && !mdns_started_) start_mdns();
}

void TcpLink::start_mdns() noexcept {
    mdns_started_ = true;  // one attempt per boot; a failure is logged, not retried
    char host[64];
    if (!mdns_host_from_name(config_.device_name, host, sizeof(host))) {
        ESP_LOGE(kTag, "the device name has no letter or digit, so there is no mDNS name: "
                       "TraceView must use the IP address %s", wifi_.ip());
        return;
    }
    esp_err_t err = mdns_init();
    if (err == ESP_OK) err = mdns_hostname_set(host);
    if (err == ESP_OK) err = mdns_instance_name_set(config_.device_name);
    if (err == ESP_OK) {
        // The _btp service is for future discovery; TraceView resolves the
        // hostname today.
        mdns_txt_item_t txt[] = {{"name", config_.device_name}};
        err = mdns_service_add(nullptr, "_btp", "_tcp", config_.port, txt, 1);
    }
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "mDNS failed (%s): TraceView must use the IP address %s instead of %s.local",
                 esp_err_to_name(err), wifi_.ip(), host);
        return;
    }
    ESP_LOGI(kTag, "reachable as %s.local:%u (%s)", host, static_cast<unsigned>(config_.port),
             wifi_.ip());
}

bool TcpLink::send(const std::uint8_t* data, std::size_t size, Priority priority) noexcept {
    bool queued = false;
    if (client_fd_.load() >= 0) {
        LinkLock lock(mutex_, ms_to_ticks(20));
        queued = lock.locked() && queue_.push(data, size, priority);
    }
    if (!queued) send_refused_.fetch_add(1U, std::memory_order_relaxed);
    // The server task writes it out on its next select() pass (<= 10 ms).
    return queued;
}

bool TcpLink::send_pending(const std::uint8_t* data, std::size_t size) noexcept {
    const int fd = pending_fd_.load();
    if (fd < 0 || data == nullptr || size == 0U) return false;
    return ::send(fd, data, size, MSG_DONTWAIT) == static_cast<int>(size);
}

void TcpLink::close_pending() noexcept { close_socket(pending_fd_.exchange(-1)); }

void TcpLink::close_client() noexcept {
    const int fd = client_fd_.exchange(-1);
    if (fd < 0) return;
    close_socket(fd);
    {
        LinkLock lock(mutex_, ms_to_ticks(20));
        queue_.clear();  // nothing queued for this client may reach the next one
    }
    if (events_.on_disconnect != nullptr) events_.on_disconnect(events_.context);
}

void TcpLink::task_entry(void* context) noexcept {
    static_cast<TcpLink*>(context)->run();
    vTaskDelete(nullptr);
}

void TcpLink::run() noexcept {
    while (running_.load()) {
        const int listen_fd = listen_fd_;
        if (listen_fd < 0) break;

        fd_set read_set;
        fd_set write_set;
        FD_ZERO(&read_set);
        FD_ZERO(&write_set);
        FD_SET(listen_fd, &read_set);
        int max_fd = listen_fd;

        const int client_fd = client_fd_.load();
        if (client_fd >= 0) {
            FD_SET(client_fd, &read_set);
            bool pending_output = false;
            {
                LinkLock lock(mutex_, ms_to_ticks(5));
                pending_output = lock.locked() && !queue_.empty();
            }
            if (pending_output) FD_SET(client_fd, &write_set);
            if (client_fd > max_fd) max_fd = client_fd;
        }
        const int pending_fd = pending_fd_.load();
        if (pending_fd >= 0) {
            FD_SET(pending_fd, &read_set);
            if (pending_fd > max_fd) max_fd = pending_fd;
        }

        timeval timeout{};
        timeout.tv_usec = kPollTimeoutMs * 1000;
        const int ready = ::select(max_fd + 1, &read_set, &write_set, nullptr, &timeout);

        // The pending deadline is checked even when select() saw nothing, so
        // a peer that never sends a byte is still closed on time.
        if (pending_fd_.load() >= 0 &&
            now_ms() - pending_started_ms_ >= kPendingHelloDeadlineMs) {
            close_pending();
        }
        if (ready <= 0) continue;

        if (FD_ISSET(listen_fd, &read_set)) {
            const int accepted = ::accept(listen_fd, nullptr, nullptr);
            if (accepted >= 0) {
                int expected = -1;
                if (client_fd_.compare_exchange_strong(expected, accepted)) {
                    // The low-latency option matters for the terminal: without
                    // it lwIP holds small writes back (Nagle).
                    int no_delay = 1;
                    (void)::setsockopt(accepted, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay));
                    {
                        LinkLock lock(mutex_);
                        queue_.clear();
                    }
                    connections_.fetch_add(1U, std::memory_order_relaxed);
                    if (events_.on_connect != nullptr) events_.on_connect(events_.context);
                } else {
                    int pending_expected = -1;
                    if (pending_fd_.compare_exchange_strong(pending_expected, accepted)) {
                        pending_started_ms_ = now_ms();
                    } else {
                        close_socket(accepted);  // a third attempt: dropped outright
                    }
                }
            }
        }

        const int active_fd = client_fd_.load();
        if (active_fd >= 0 && FD_ISSET(active_fd, &read_set)) {
            const int received = ::recv(active_fd, receive_buffer_, sizeof(receive_buffer_), 0);
            if (received <= 0) {
                close_client();
            } else {
                bytes_in_.fetch_add(static_cast<std::uint32_t>(received), std::memory_order_relaxed);
                events_.on_receive(events_.context, receive_buffer_, static_cast<std::size_t>(received));
            }
        }

        if (client_fd_.load() >= 0 && FD_ISSET(client_fd_.load(), &write_set)) drain();

        const int watched_pending = pending_fd_.load();
        if (watched_pending >= 0 && FD_ISSET(watched_pending, &read_set)) {
            const int received =
                ::recv(watched_pending, pending_receive_buffer_, sizeof(pending_receive_buffer_), 0);
            if (received <= 0) {
                close_pending();
            } else if (pending_handler_.on_receive != nullptr) {
                pending_handler_.on_receive(pending_handler_.context, pending_receive_buffer_,
                                            static_cast<std::size_t>(received));
            }
        }
    }
    close_client();
    close_pending();
    task_ = nullptr;
}

void TcpLink::drain() noexcept {
    bool broken = false;
    {
        LinkLock lock(mutex_, ms_to_ticks(20));
        if (!lock.locked()) return;
        const int fd = client_fd_.load();
        const std::uint8_t* chunk = nullptr;
        std::size_t size = 0U;
        while (fd >= 0 && queue_.front(&chunk, &size)) {
            const int written = ::send(fd, chunk, size, MSG_DONTWAIT);
            if (written > 0) {
                queue_.consume(static_cast<std::size_t>(written));
                bytes_out_.fetch_add(static_cast<std::uint32_t>(written), std::memory_order_relaxed);
                if (static_cast<std::size_t>(written) < size) break;  // socket full: next pass
                continue;
            }
            // ENOMEM/ENOBUFS is lwIP out of buffers during a burst: transient.
            // Treating it as fatal closed good sessions in bally_OS.
            if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOMEM ||
                                errno == ENOBUFS)) {
                break;
            }
            broken = true;
            break;
        }
    }
    // close_client() takes the mutex itself, so only after releasing it.
    if (broken) close_client();
}

#else  // !CONFIG_BTPDEV_LINK_TCP

TcpLink::~TcpLink() = default;
esp_err_t TcpLink::start(const LinkEvents&, ErrorInfo& error) noexcept {
    return fail(error, BTPDEV_ERR_LINK_UNSUPPORTED, tag::kTcp,
                "the TCP link is disabled in menuconfig (BTPDevice > Links)");
}
void TcpLink::stop() noexcept {}
bool TcpLink::send(const std::uint8_t*, std::size_t, Priority) noexcept { return false; }
void TcpLink::poll(std::uint32_t) noexcept {}
bool TcpLink::send_pending(const std::uint8_t*, std::size_t) noexcept { return false; }
void TcpLink::close_pending() noexcept {}
void TcpLink::close_client() noexcept {}
void TcpLink::task_entry(void*) noexcept {}
void TcpLink::run() noexcept {}
esp_err_t TcpLink::open_listener(ErrorInfo&) noexcept { return ESP_ERR_NOT_SUPPORTED; }
void TcpLink::drain() noexcept {}
void TcpLink::start_mdns() noexcept {}

#endif  // CONFIG_BTPDEV_LINK_TCP

LinkStats TcpLink::stats() const noexcept {
    LinkStats s;
    s.connections = connections_.load(std::memory_order_relaxed);
    s.bytes_in = bytes_in_.load(std::memory_order_relaxed);
    s.bytes_out = bytes_out_.load(std::memory_order_relaxed);
    s.send_refused = send_refused_.load(std::memory_order_relaxed);
    return s;
}

}  // namespace btpdev
