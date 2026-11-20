#ifndef TRACEKIT_RECOVERY_CONTRACT_H
#define TRACEKIT_RECOVERY_CONTRACT_H

// The contract between the application (the library, in the `factory`
// partition) and the recovery firmware (recovery/, in `ota_0`). The two are
// built separately and evolve in different versions, so everything they
// share is here, versioned by kAbi (docs/PLANO.md D22, section 4.14).
//
// Header-only and portable: the recovery includes it too.

#include <cstddef>
#include <cstdint>

namespace tracekit::recovery {

/// Bump when a key below changes meaning or layout. The recovery refuses a
/// newer contract it does not know (it still serves uploads, without the
/// app's networks or reason).
constexpr std::uint8_t kAbi = 1U;

/// NVS namespace the recovery reads. The device key stays where the library
/// already keeps it (kKeyNamespace / kKeyBlob).
constexpr const char* kNamespace = "btprec";
constexpr const char* kKeyAbi = "abi";        // u8, kAbi
constexpr const char* kKeyName = "name";      // str, the device name (mDNS, /status)
constexpr const char* kKeyWifi = "wifi";      // blob, WifiBlob
constexpr const char* kKeyReason = "reason";  // u8, Reason

constexpr const char* kKeyNamespace = "btpdev";  // the old name, kept: devices in the field have data there
constexpr const char* kKeyBlob = "keyfile";   // the 96-octet key file (KeyStore)

/// Why the recovery is running. Written by the app just before it points the
/// otadata at the recovery; the recovery keeps it until an upload succeeds.
enum class Reason : std::uint8_t {
    None = 0U,           ///< nothing recorded (e.g. booted by hand)
    Command = 1U,        ///< `btp -recovery`: goes back to the app after a timeout
    CrashLoop = 2U,      ///< the app kept crashing: stays until a firmware arrives
    FactoryReset = 3U,   ///< erase app + LittleFS, keep NVS, then wait
    FactoryResetAll = 4U,///< erase app + LittleFS + NVS, then wait
};

inline const char* reason_name(Reason reason) noexcept {
    switch (reason) {
        case Reason::Command: return "command";
        case Reason::CrashLoop: return "crash_loop";
        case Reason::FactoryReset: return "factory_reset";
        case Reason::FactoryResetAll: return "factory_reset_all";
        case Reason::None: break;
    }
    return "none";
}

/// The Wi-Fi networks the app knows, mirrored for the recovery: a count
/// octet, then kMaxNetworks fixed slots (NUL-padded ssid and password).
struct WifiBlob {
    static constexpr std::size_t kMaxNetworks = 8U;
    static constexpr std::size_t kSsid = 33U;      // 32 + NUL
    static constexpr std::size_t kPassword = 65U;  // 64 + NUL
    struct Network {
        char ssid[kSsid];
        char password[kPassword];
    };
    std::uint8_t count;
    Network networks[kMaxNetworks];
};

/// Partition labels the bundled tables use (partitions/*.csv). The recovery
/// finds the app by subtype (factory) and the file system by this label.
constexpr const char* kStorageLabel = "storage";

/// Recovery HTTP server (D20): the port TraceView's OTA tab uses.
constexpr std::uint16_t kHttpPort = 80U;

}  // namespace tracekit::recovery

#endif  // TRACEKIT_RECOVERY_CONTRACT_H
