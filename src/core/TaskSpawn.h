#ifndef TRACEKIT_TASK_SPAWN_H
#define TRACEKIT_TASK_SPAWN_H

// The library's tasks, with their stack in PSRAM when DeviceConfig::psram is
// Psram::Stacks (and the build allows it), internal otherwise.
//
// A task with an external stack is created static (xTaskCreateStatic over
// PSRAM), so FreeRTOS never frees it: such a task must not delete itself.
// It ends with finish_task(), which parks it; the owner then calls
// release_task(), which deletes it from outside and frees the memory.
//
//   spawn_task(&entry, "name", 3072, this, 9, 0, /*may_be_external=*/true, &task_, &task_memory_);
//   ...in the task, last line:  finish_task(task_memory_);
//   ...in stop(), once the task said it is done:  release_task(task_, task_memory_);

#include <cstdint>

#include <tracekit/config.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace tracekit {

/// True when the chip has PSRAM mapped into the heap.
bool psram_available() noexcept;

struct TaskMemory {
    StaticTask_t* tcb = nullptr;   ///< internal RAM (FreeRTOS requires it)
    StackType_t* stack = nullptr;  ///< PSRAM; nullptr for an ordinary task
    bool external() const noexcept { return stack != nullptr; }
};

/// xTaskCreatePinnedToCore(), with the stack in PSRAM when `may_be_external`
/// and the policy is Psram::Stacks. core < 0: no affinity.
bool spawn_task(TaskFunction_t entry, const char* name, std::uint32_t stack_bytes, void* arg,
                UBaseType_t priority, int core, bool may_be_external, TaskHandle_t* out,
                TaskMemory* memory) noexcept;

/// The task's last call: deletes an ordinary task, parks a static one.
[[noreturn]] void finish_task(const TaskMemory& memory) noexcept;

/// After finish_task() (or for a task never finished): deletes it if it is
/// static and frees its memory. Safe on a null task.
void release_task(TaskHandle_t task, TaskMemory* memory) noexcept;

/// Where a task's stack is, for btp -mem: "psram" or "".
const char* stack_place(const TaskMemory& memory) noexcept;

}  // namespace tracekit

#endif  // TRACEKIT_TASK_SPAWN_H
