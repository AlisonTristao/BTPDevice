#include "core/DebugReport.h"

#include <string>
#include <vector>

#include "check.h"

using tracekit::format_boot_step;
using tracekit::format_kb;
using tracekit::HeapUsage;
using tracekit::TaskUsage;

namespace {

std::string kb(std::size_t bytes) {
    char out[16];
    format_kb(out, sizeof(out), bytes);
    return out;
}

std::string step(std::uint64_t us, const char* name, const char* detail, long long heap) {
    char out[tracekit::kReportLineMax];
    format_boot_step(out, sizeof(out), us, name, detail, heap);
    return out;
}

void collect(void* context, const char* line) { static_cast<std::vector<std::string>*>(context)->emplace_back(line); }

void kilobytes_round_to_one_decimal() {
    CHECK_STR_EQ(kb(0U).c_str(), "0.0");
    CHECK_STR_EQ(kb(1024U).c_str(), "1.0");
    CHECK_STR_EQ(kb(4198U).c_str(), "4.1");     // 4.0996
    CHECK_STR_EQ(kb(51U).c_str(), "0.0");       // under half a tenth
    CHECK_STR_EQ(kb(52U).c_str(), "0.1");
    CHECK_STR_EQ(kb(186778U).c_str(), "182.4");
}

void boot_steps_line_up_and_show_the_heap_they_took() {
    const std::string line = step(31000U, "storage", "LittleFS /fs", 4198);
    CHECK_STR_EQ(line.c_str(),
                 "[  0.031] storage   LittleFS /fs                             heap   -4.1 KB");
    // Freed memory reads as a plus.
    CHECK(step(1500000U, "x", "y", -2048).find("heap   +2.0 KB") != std::string::npos);
    // Nothing worth showing: no heap column and no trailing pad.
    CHECK_STR_EQ(step(12000U, "identity", "source 3a1f22c0", 10).c_str(), "[  0.012] identity  source 3a1f22c0");
    // Seconds keep growing past three digits of milliseconds.
    CHECK(step(1234567000ULL, "ready", "", 0).rfind("[1234.567] ready", 0) == 0);
}

void a_small_buffer_is_cut_not_overrun() {
    char out[12];
    const std::size_t n = format_boot_step(out, sizeof(out), 31000U, "storage", "LittleFS /fs", 4198);
    CHECK(n == sizeof(out) - 1U);
    CHECK(out[n] == '\0');
}

void the_report_has_heap_psram_and_one_row_per_task() {
    HeapUsage heap;
    heap.free_bytes = 186778U;
    heap.total_bytes = 327680U;
    heap.minimum_bytes = 185242U;
    heap.largest_block = 112640U;
    TaskUsage tasks[2];
    tasks[0].name = "tracekit_comms";
    tasks[0].core = 1;
    tasks[0].priority = 10U;
    tasks[0].stack_total = 8192U;
    tasks[0].stack_min_free = 5018U;
    tasks[1].name = "wifi";             // not ours: total unknown
    tasks[1].priority = 23U;
    tasks[1].stack_min_free = 1100U;

    std::vector<std::string> lines;
    tracekit::write_memory_report(heap, tasks, 2U, &collect, &lines);
    CHECK(lines.size() == 4U);  // heap, header, two tasks (no PSRAM)
    CHECK_STR_EQ(lines[0].c_str(), "heap      182.4 KB free of 320.0 KB, min 180.9 KB, largest block 110.0 KB");
    CHECK_STR_EQ(lines[1].c_str(), "task              core prio  stack peak / total min free");
    CHECK_STR_EQ(lines[2].c_str(), "tracekit_comms       1   10    3.1 / 8.0 KB       4.9 KB");
    CHECK_STR_EQ(lines[3].c_str(), "wifi                 -   23      -                1.1 KB");

    heap.psram_total = 4194304U;
    heap.psram_free = 4108288U;
    lines.clear();
    tracekit::write_memory_report(heap, nullptr, 0U, &collect, &lines);
    CHECK(lines.size() == 2U);  // heap + psram, no task table
    CHECK_STR_EQ(lines[1].c_str(), "psram     4012.0 KB free of 4096.0 KB");
}

}  // namespace

int main() {
    kilobytes_round_to_one_decimal();
    boot_steps_line_up_and_show_the_heap_they_took();
    a_small_buffer_is_cut_not_overrun();
    the_report_has_heap_psram_and_one_row_per_task();
    return test_result("test_debug_report");
}
