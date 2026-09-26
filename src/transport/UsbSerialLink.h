#ifndef BTPDEV_USB_SERIAL_LINK_H
#define BTPDEV_USB_SERIAL_LINK_H

// Serial link over the chip's native USB (TinyUSB CDC-ACM): ESP32-S2/S3/P4.
// Ported from bally_OS lib/UsbSerialBtpServer, with three changes: the queue
// is a FrameQueue (no allocation per frame), the link installs the USB driver
// itself (bally_OS left that to USBMassStorage), and the device name goes into
// the USB product and CDC interface strings, which is how TraceView pairs a
// port with a device name.
//
// A "connection" is the host raising DTR (TraceView does on open and drops it
// on close: the SerialTransport DTR contract). Bytes arriving while DTR is low
// still open one, for hosts that never touch DTR. One port, one host: no
// second-session rejection here.
//
// Needs CONFIG_TINYUSB_CDC_ENABLED (esp_tinyusb menu). Without it -- or on a
// chip without USB OTG -- start() reports BTPDEV_ERR_LINK_UNSUPPORTED.

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "core/FrameQueue.h"
#include "core/Memory.h"
#include "transport/Link.h"
#include "transport/LinkMutex.h"

namespace btpdev {

struct UsbSerialConfig {
    /// Device name: USB product string and CDC interface string. Must stay
    /// valid while the link runs (the USB stack keeps the pointer).
    const char* product_name = nullptr;
    std::size_t send_queue_bytes = 16384U;
    /// false: the application already installed the TinyUSB driver (for
    /// example a composite CDC + MSC device); the link only starts the CDC.
    bool install_driver = true;
};

class UsbSerialLink final : public Link {
public:
    explicit UsbSerialLink(const UsbSerialConfig& config) noexcept : config_(config) {}
    ~UsbSerialLink() override;

    const char* name() const noexcept override { return "Serial"; }
    Kind kind() const noexcept override { return Kind::Stream; }
    esp_err_t start(const LinkEvents& events, ErrorInfo& error) noexcept override;
    void stop() noexcept override;
    bool send(const std::uint8_t* data, std::size_t size, Priority priority) noexcept override;
    bool connected() const noexcept override { return connected_.load(); }
    LinkStats stats() const noexcept override;

    // TinyUSB C callbacks (file-scope trampolines in the .cpp). Public only so
    // they can be reached from there.
    void handle_line_state(bool dtr) noexcept;
    void handle_rx() noexcept;
    void drain() noexcept;

private:
    void open_session() noexcept;
    void close_session() noexcept;

    UsbSerialConfig config_;
    LinkEvents events_{};
    LinkMutex mutex_;
    Buffer queue_storage_;
    FrameQueue queue_;   // guarded by mutex_
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<std::uint32_t> connections_{0U};
    std::atomic<std::uint32_t> bytes_in_{0U};
    std::atomic<std::uint32_t> bytes_out_{0U};
    std::atomic<std::uint32_t> send_refused_{0U};

    // USB string descriptors; the stack keeps these pointers.
    char serial_[13] = {};
    const char* strings_[5] = {};

    std::uint8_t rx_buffer_[512] = {};   // one tinyusb_cdcacm_read() chunk
};

}  // namespace btpdev

#endif  // BTPDEV_USB_SERIAL_LINK_H
