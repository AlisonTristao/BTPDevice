#ifndef TRACEKIT_APP_H
#define TRACEKIT_APP_H

// tracekit::App -- a whole device in one class. Derive from it, say who the
// device is in config(), what it can do in commands() and what it measures in
// topics(), and keep the device's state as members:
//
//   class GardenPump : public tracekit::App {
//   public:
//       tracekit::DeviceConfig config() const override {
//           return {.name = "bomba-jardim", .memory = tracekit::Memory::Small};
//       }
//       void commands(tracekit::Commands& c) override {
//           c.module("relay", "Pump relay")
//               .add("set", [this](uint8_t on) { return set(on); }, "relay -set <0|1>");
//       }
//       void topics(tracekit::Topics& t) override {
//           relay_ = t.topic("relay", tracekit::Hz(1)).boolean("on", &on_);
//       }
//
//   private:
//       uint8_t set(uint8_t on) {
//           on_ = on != 0U;
//           device().publish(relay_);   // the plot sees the change at once
//           return RESULT_OK;
//       }
//       bool on_ = false;
//       tracekit::Topic relay_;
//   };
//
//   GardenPump app;
//   TraceKit device(app);
//
//   extern "C" void app_main() { device.begin(); }
//
// The virtual functions are called once, from begin(), after every global
// object is built -- never from a constructor -- so their order across files
// does not matter. The App must outlive the device (a global, as above):
// commands and fill() functions capture `this`.
//
// The free-function style (TraceKit(device_config, storage_config) and
// begin(app_commands, app_topics)) keeps working; both end in the same place.

#include <tracekit/commands.h>
#include <tracekit/config.h>
#include <tracekit/settings.h>
#include <tracekit/topics.h>

class TraceKit;

namespace tracekit {

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
    /// Values kept in NVS and changed with `cfg -set` (tracekit/settings.h).
    /// Loaded before commands() and topics(). Default: none.
    virtual void settings(Settings& settings) { (void)settings; }

    /// The device running this App: valid from inside commands() / topics()
    /// on, i.e. once begin() was called. Any task.
    TraceKit& device() noexcept { return *device_; }
    bool has_device() const noexcept { return device_ != nullptr; }

private:
    friend class ::TraceKit;
    TraceKit* device_ = nullptr;
};

}  // namespace tracekit

#endif  // TRACEKIT_APP_H
