#ifndef BTPDEV_DEBUG_REPORT_H
#define BTPDEV_DEBUG_REPORT_H

// Text of the serial debug log (DeviceConfig::debug_log) and of `btp -mem`:
// one line per begin() step, then the heap and the task table. Portable --
// the numbers are gathered by DeviceCore, only the formatting lives here --
// and covered by test/host/test_debug_report.cpp.

#include <cstddef>
#include <cstdint>

namespace btpdev {

struct HeapUsage {
    std::size_t free_bytes = 0U;
    std::size_t total_bytes = 0U;
    std::size_t minimum_bytes = 0U;   ///< lowest free since boot
    std::size_t largest_block = 0U;   ///< biggest single allocation possible now
    std::size_t psram_free = 0U;
    std::size_t psram_total = 0U;     ///< 0: no PSRAM line
};

struct TaskUsage {
    const char* name = nullptr;
    int core = -1;                    ///< -1: either core
    unsigned priority = 0U;
    std::uint32_t stack_total = 0U;   ///< bytes; 0 when unknown (not our task)
    std::uint32_t stack_min_free = 0U;  ///< high-water mark, bytes
};

/// Receives one finished line (no newline).
using ReportLineFn = void (*)(void* context, const char* line);

/// Longest line any of these functions produces, NUL included.
constexpr std::size_t kReportLineMax = 128U;

/// "[  0.031] storage   LittleFS /fs           heap  -4.1 KB": time since
/// boot, the step, its detail and the heap it took (`heap_used` bytes;
/// negative when it freed memory, hidden when it rounds to 0.0 KB).
/// Returns the length written; `out` is always NUL-terminated.
std::size_t format_boot_step(char* out, std::size_t capacity, std::uint64_t since_boot_us,
                             const char* step, const char* detail, long long heap_used) noexcept;

/// "4.1" -- bytes as KB with one decimal, rounded.
std::size_t format_kb(char* out, std::size_t capacity, std::size_t bytes) noexcept;

/// The heap line(s), then the task table, one `line` call each.
void write_memory_report(const HeapUsage& heap, const TaskUsage* tasks, std::size_t task_count,
                         ReportLineFn line, void* context) noexcept;

}  // namespace btpdev

#endif  // BTPDEV_DEBUG_REPORT_H
