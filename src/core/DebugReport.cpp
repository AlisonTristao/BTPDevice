#include "core/DebugReport.h"

#include <cstdio>

namespace tracekit {

namespace {

// Columns of the boot steps: the step name, then the detail padded so the
// heap figures line up.
constexpr int kStepWidth = 9;
constexpr int kDetailWidth = 40;

}  // namespace

std::size_t format_kb(char* out, std::size_t capacity, std::size_t bytes) noexcept {
    if (out == nullptr || capacity == 0U) return 0U;
    // Tenths of a KB, rounded, without floating point (newlib-nano printf).
    const unsigned long long tenths = (static_cast<unsigned long long>(bytes) * 10ULL + 512ULL) / 1024ULL;
    const int n = std::snprintf(out, capacity, "%llu.%llu", tenths / 10ULL, tenths % 10ULL);
    if (n < 0) {
        out[0] = '\0';
        return 0U;
    }
    return static_cast<std::size_t>(n) < capacity ? static_cast<std::size_t>(n) : capacity - 1U;
}

std::size_t format_boot_step(char* out, std::size_t capacity, std::uint64_t since_boot_us,
                             const char* step, const char* detail, long long heap_used) noexcept {
    if (out == nullptr || capacity == 0U) return 0U;
    const unsigned long long ms = since_boot_us / 1000ULL;
    char heap[40] = "";
    const unsigned long long magnitude =
        heap_used < 0 ? static_cast<unsigned long long>(-heap_used) : static_cast<unsigned long long>(heap_used);
    char kb[16];
    format_kb(kb, sizeof(kb), static_cast<std::size_t>(magnitude));
    if (kb[0] != '0' || kb[1] != '.' || kb[2] != '0' || kb[3] != '\0') {
        // Used memory reads as a minus, like the heap going down.
        char signed_kb[20];
        std::snprintf(signed_kb, sizeof(signed_kb), "%s%s", heap_used > 0 ? "-" : "+", kb);
        std::snprintf(heap, sizeof(heap), "heap %6s KB", signed_kb);
    }
    const int n = std::snprintf(out, capacity, "[%3llu.%03llu] %-*s %-*s %s", ms / 1000ULL, ms % 1000ULL,
                                kStepWidth, step != nullptr ? step : "", heap[0] != '\0' ? kDetailWidth : 0,
                                detail != nullptr ? detail : "", heap);
    if (n < 0) {
        out[0] = '\0';
        return 0U;
    }
    std::size_t length = static_cast<std::size_t>(n) < capacity ? static_cast<std::size_t>(n) : capacity - 1U;
    while (length > 0U && out[length - 1U] == ' ') out[--length] = '\0';  // no trailing pad
    return length;
}

void write_memory_report(const HeapUsage& heap, const TaskUsage* tasks, std::size_t task_count,
                         ReportLineFn line, void* context) noexcept {
    if (line == nullptr) return;
    char text[kReportLineMax];
    char a[16];
    char b[16];
    char c[16];
    char d[16];

    format_kb(a, sizeof(a), heap.free_bytes);
    format_kb(b, sizeof(b), heap.total_bytes);
    format_kb(c, sizeof(c), heap.minimum_bytes);
    format_kb(d, sizeof(d), heap.largest_block);
    std::snprintf(text, sizeof(text), "heap      %s KB free of %s KB, min %s KB, largest block %s KB", a, b, c, d);
    line(context, text);

    if (heap.psram_total != 0U) {
        format_kb(a, sizeof(a), heap.psram_free);
        format_kb(b, sizeof(b), heap.psram_total);
        std::snprintf(text, sizeof(text), "psram     %s KB free of %s KB", a, b);
        line(context, text);
    }

    if (tasks == nullptr || task_count == 0U) return;
    // Same widths as the rows below.
    std::snprintf(text, sizeof(text), "%-17s %4s %4s  %-18s %8s", "task", "core", "prio", "stack peak / total",
                  "min free");
    line(context, text);
    for (std::size_t i = 0U; i < task_count; ++i) {
        const TaskUsage& t = tasks[i];
        char core[12];  // any int, for -Wformat-truncation
        if (t.core < 0) {
            std::snprintf(core, sizeof(core), "-");
        } else {
            std::snprintf(core, sizeof(core), "%d", t.core);
        }
        char stack[48];
        if (t.stack_total != 0U) {
            const std::uint32_t peak = t.stack_total > t.stack_min_free ? t.stack_total - t.stack_min_free : 0U;
            format_kb(a, sizeof(a), peak);
            format_kb(b, sizeof(b), t.stack_total);
            std::snprintf(stack, sizeof(stack), "%5s / %s KB", a, b);
        } else {
            std::snprintf(stack, sizeof(stack), "    -");  // not our task: size unknown
        }
        format_kb(c, sizeof(c), t.stack_min_free);
        std::snprintf(text, sizeof(text), "%-17s %4s %4u  %-18s %5s KB", t.name != nullptr ? t.name : "?", core,
                      t.priority, stack, c);
        line(context, text);
    }
}

}  // namespace tracekit
