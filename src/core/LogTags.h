#ifndef TRACEKIT_LOG_TAGS_H
#define TRACEKIT_LOG_TAGS_H

// The ESP_LOG tag of every module (CONTRIBUTING.md, "Erros e logs"). One list,
// so begin() can apply CONFIG_TRACEKIT_LOG_LEVEL to all of them and a new module
// cannot forget to register its tag.

#include <cstddef>

namespace tracekit {
namespace tag {

constexpr const char* kCore = "tracekit";
constexpr const char* kSerial = "tracekit.serial";
constexpr const char* kTcp = "tracekit.tcp";
constexpr const char* kWifi = "tracekit.wifi";
constexpr const char* kBle = "tracekit.ble";
constexpr const char* kEspNow = "tracekit.espnow";
constexpr const char* kKey = "tracekit.key";
constexpr const char* kFs = "tracekit.fs";
constexpr const char* kCmd = "tracekit.cmd";
constexpr const char* kTopic = "tracekit.topic";
constexpr const char* kRecovery = "tracekit.recovery";
constexpr const char* kSettings = "tracekit.cfg";

constexpr const char* kAll[] = {kCore, kSerial, kTcp,  kWifi, kBle,
                                kEspNow, kKey, kFs, kCmd, kTopic, kRecovery, kSettings};
constexpr std::size_t kCount = sizeof(kAll) / sizeof(kAll[0]);

}  // namespace tag
}  // namespace tracekit

#endif  // TRACEKIT_LOG_TAGS_H
