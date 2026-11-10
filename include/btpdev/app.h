#ifndef BTPDEV_APP_H
#define BTPDEV_APP_H

// btpdev::App -- a whole device in one class. Derive from it, say who the
// device is in config(), what it can do in commands() and what it measures in
// topics(), and keep the device's state as members:
//
//   class GardenPump : public btpdev::App {
//   public:
//       btpdev::DeviceConfig config() const override {
//           return {.name = "bomba-jardim", .memory = btpdev::Memory::Small};
//       }
//       void commands(btpdev::Commands& c) override {
//           c.module("relay", "Pump relay")
//               .add("set", [this](uint8_t on) { return set(on); }, "relay -set <0|1>");
//       }
//       void topics(btpdev::Topics& t) override {
//           relay_ = t.topic("relay", btpdev::Hz(1)).boolean("on", &on_);
//       }
//
//   private:
//       uint8_t set(uint8_t on) {
//           on_ = on != 0U;
//           device().publish(relay_);   // the plot sees the change at once
//           return RESULT_OK;
//       }
//       bool on_ = false;
//       btpdev::Topic relay_;
//   };
//
//   GardenPump app;
//   BtpDevice device(app);
//
//   extern "C" void app_main() { device.begin(); }
//
// The virtual functions are called once, from begin(), after every global
// object is built -- never from a constructor -- so their order across files
// does not matter. The App must outlive the device (a global, as above):
// commands and fill() functions capture `this`.
//
// The free-function style (BtpDevice(device_config, storage_config) and
// begin(app_commands, app_topics)) keeps working; both end in the same place.

#include <btpdev/commands.h>
#include <btpdev/config.h>
#include <btpdev/topics.h>

class BtpDevice;

namespace btpdev {

class App {
public:
    virtual ~App() = default;

    /// Who the device is and how it talks (name, link, memory, network).
    /// The strings it points to must live forever (literals do).
    virtual DeviceConfig config() const = 0;
    /// File system and key. Default: LittleFS on "storage", key from NVS.
    virtual StorageConfig storage() const { return {}; }
    /// What TraceView's terminal can call. Default: nothing.
    virtual void commands(Commands& commands) { (void)commands; }
    /// What TraceView can plot. Default: nothing.
    virtual void topics(Topics& topics) { (void)topics; }

    /// The device running this App: valid from inside commands() / topics()
    /// on, i.e. once begin() was called. Any task.
    BtpDevice& device() noexcept { return *device_; }
    bool has_device() const noexcept { return device_ != nullptr; }

private:
    friend class ::BtpDevice;
    BtpDevice* device_ = nullptr;
};

}  // namespace btpdev

#endif  // BTPDEV_APP_H
