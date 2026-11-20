#ifndef TRACEKIT_NVS_SETTINGS_STORE_H
#define TRACEKIT_NVS_SETTINGS_STORE_H

// The settings' saved copy in NVS, namespace "btpcfg" (docs/PLANO.md D28).
// Opened per call: settings change rarely, and an open handle would pin a
// namespace entry for the whole run.

#include "protocol/SettingsRegistry.h"

namespace tracekit {

class NvsSettingsStore final : public SettingsStore {
public:
    static constexpr const char* kNamespace = "btpcfg";

    bool load(const char* key, std::uint8_t* data, std::size_t* size) override;
    bool save(const char* key, const std::uint8_t* data, std::size_t size) override;
    bool erase(const char* key) override;
};

}  // namespace tracekit

#endif  // TRACEKIT_NVS_SETTINGS_STORE_H
