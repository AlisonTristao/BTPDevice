#ifndef BTPDEV_RECOVERY_H
#define BTPDEV_RECOVERY_H

// The application's side of the recovery (docs/PLANO.md D19-D26, 4.14):
// finds the recovery partition, mirrors what the recovery needs into NVS,
// counts crash loops and switches the boot partition to the recovery.
//
// Every method is a no-op (or reports "not available") when the partition
// table has no recovery: the library keeps working on the old tables.

#include <cstddef>
#include <cstdint>

#include <btpdev/config.h>
#include <esp_err.h>

#include "transport/WifiNetworks.h"
#include "update/RecoveryContract.h"

#include <esp_partition.h>

namespace btpdev {

class Recovery {
public:
    /// First thing in begin(): finds the partitions, logs what is missing and
    /// counts abnormal resets. With `config.crash_loop` resets in a row it
    /// enters the recovery and DOES NOT RETURN.
    void boot(const RecoveryConfig& config) noexcept;

    /// After begin() succeeded: the count is cleared once the application has
    /// run `config.stable_ms` without a crash.
    void arm_stable_timer() noexcept;

    /// Writes the contract (name, networks) when it differs from what NVS
    /// already holds, so a normal boot does not wear the flash.
    void mirror(const char* name, const WifiNetwork* networks, std::size_t count) noexcept;

    /// A recovery image is installed and the application runs from factory.
    bool available() const noexcept { return available_; }
    /// Why not, for the user ("no recovery partition..."); "" when available.
    const char* unavailable_reason() const noexcept { return why_; }

    /// Records `reason`, points the otadata at the recovery and restarts
    /// after `delay_ms` (so a command's reply can leave first).
    esp_err_t enter(recovery::Reason reason, std::uint32_t delay_ms) noexcept;

    /// Multi-line report for `btp -recovery_info`.
    void describe(char* out, std::size_t size) const noexcept;

    std::uint32_t crash_count() const noexcept;

    /// The next reset is the library's own doing (a watchdog it armed on
    /// purpose): do not count it as a crash.
    static void expect_reset() noexcept;

private:
    void find_partitions() noexcept;

    RecoveryConfig config_{};
    const esp_partition_t* app_ = nullptr;
    const esp_partition_t* recovery_ = nullptr;
    bool available_ = false;
    char why_[96] = "";
    char recovery_version_[32] = "";
};

}  // namespace btpdev

#endif  // BTPDEV_RECOVERY_H
