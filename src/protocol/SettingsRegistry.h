#ifndef TRACEKIT_SETTINGS_REGISTRY_H
#define TRACEKIT_SETTINGS_REGISTRY_H

// The declared settings at run time (include/tracekit/settings.h, PLANO D28):
// validation, text <-> value, and the saved copy. Portable: the storage is a
// SettingsStore (NVS on the device, a map in the host tests).
//
// Saved format: one NVS blob per setting, keyed by its name:
//   [type (1 octet)] [value: 1, 2 or 4 octets little-endian, or the text]
// The type tag is what notices a declaration that changed type.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include <tracekit/error.h>
#include <tracekit/settings.h>

namespace tracekit {

class SettingsStore {
public:
    virtual ~SettingsStore() = default;
    /// `*size` in: capacity, out: bytes read. False when there is none.
    virtual bool load(const char* key, std::uint8_t* data, std::size_t* size) = 0;
    virtual bool save(const char* key, const std::uint8_t* data, std::size_t size) = 0;
    virtual bool erase(const char* key) = 0;
};

class SettingsRegistry {
public:
    enum class Result : std::uint8_t {
        Ok,
        OkReboot,     ///< saved; takes effect after a restart
        UnknownName,
        BadValue,     ///< not a value of the setting's type
        OutOfRange,
        TooLong,      ///< text longer than the buffer
        NotSaved,     ///< changed in memory, the store refused it
    };

    /// Checks every declaration, then puts the saved value (or the default)
    /// in each variable. `warn` hears about saved values that were dropped.
    /// `store` may be null: no persistence, defaults only.
    esp_err_t install(Settings& settings, SettingsStore* store, ErrorInfo& error,
                      const std::function<void(const char*)>& warn = nullptr);

    std::size_t size() const noexcept { return list_.size(); }
    bool empty() const noexcept { return list_.empty(); }

    /// The current value as text ("30", "true", "bench"). False: no such name.
    bool get(const char* name, char* out, std::size_t size) const noexcept;
    /// Parses, checks, writes the variable, saves, calls on_change.
    Result set(const char* name, const char* text);
    /// Back to the default and the saved copy erased; name "all" resets all.
    Result reset(const char* name);
    /// Saves the variable's current value (the application changed it).
    Result save(const char* name);

    /// One `cfg -list` line for setting `index`.
    void describe(std::size_t index, char* out, std::size_t size) const noexcept;
    /// A sentence for a Result about `name` (with the range when relevant).
    void explain(Result result, const char* name, char* out, std::size_t size) const noexcept;

private:
    const SettingDecl* find(const char* name) const noexcept;
    SettingDecl* find(const char* name) noexcept;
    bool encode(const SettingDecl& d, std::uint8_t* out, std::size_t* size) const noexcept;
    bool decode(const SettingDecl& d, const std::uint8_t* data, std::size_t size) noexcept;
    void apply_default(SettingDecl& d) noexcept;
    bool persist(const SettingDecl& d) noexcept;
    void format(const SettingDecl& d, char* out, std::size_t size, bool fallback) const noexcept;

    std::vector<SettingDecl> list_;
    SettingsStore* store_ = nullptr;
};

}  // namespace tracekit

#endif  // TRACEKIT_SETTINGS_REGISTRY_H
