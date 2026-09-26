#ifndef BTPDEV_LOG_TAGS_H
#define BTPDEV_LOG_TAGS_H

// The ESP_LOG tag of every module (CONTRIBUTING.md, "Erros e logs"). One list,
// so begin() can apply CONFIG_BTPDEV_LOG_LEVEL to all of them and a new module
// cannot forget to register its tag.

#include <cstddef>

namespace btpdev {
namespace tag {

constexpr const char* kCore = "btpdev";
constexpr const char* kSerial = "btpdev.serial";
constexpr const char* kTcp = "btpdev.tcp";
constexpr const char* kWifi = "btpdev.wifi";
constexpr const char* kBle = "btpdev.ble";
constexpr const char* kEspNow = "btpdev.espnow";
constexpr const char* kKey = "btpdev.key";
constexpr const char* kFs = "btpdev.fs";
constexpr const char* kCmd = "btpdev.cmd";
constexpr const char* kTopic = "btpdev.topic";

constexpr const char* kAll[] = {kCore, kSerial, kTcp,  kWifi, kBle,
                                kEspNow, kKey, kFs, kCmd, kTopic};
constexpr std::size_t kCount = sizeof(kAll) / sizeof(kAll[0]);

}  // namespace tag
}  // namespace btpdev

#endif  // BTPDEV_LOG_TAGS_H
