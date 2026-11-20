#include "core/TaskSpawn.h"

#include <sdkconfig.h>

#include <esp_heap_caps.h>
#include <esp_log.h>

#include "core/Memory.h"

namespace btpdev {

bool psram_available() noexcept {
#if CONFIG_SPIRAM
    return heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0U;
#else
    return false;
#endif
}

bool spawn_task(TaskFunction_t entry, const char* name, std::uint32_t stack_bytes, void* arg,
                UBaseType_t priority, int core, bool may_be_external, TaskHandle_t* out,
                TaskMemory* memory) noexcept {
    *memory = TaskMemory{};
    const BaseType_t affinity = core < 0 ? tskNO_AFFINITY : core;
#if CONFIG_SPIRAM && CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM && CONFIG_FREERTOS_SUPPORT_STATIC_ALLOCATION
    if (may_be_external && psram_policy() == Psram::Stacks && psram_available()) {
        auto* tcb = static_cast<StaticTask_t*>(
            heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        auto* stack = static_cast<StackType_t*>(heap_caps_malloc(stack_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (tcb != nullptr && stack != nullptr) {
            *out = xTaskCreateStaticPinnedToCore(entry, name, stack_bytes, arg, priority, stack, tcb, affinity);
            if (*out != nullptr) {
                memory->tcb = tcb;
                memory->stack = stack;
                return true;
            }
        }
        heap_caps_free(tcb);
        heap_caps_free(stack);
        // No PSRAM left for it: fall back to an internal stack below.
    }
#else
    (void)may_be_external;
#endif
    return xTaskCreatePinnedToCore(entry, name, stack_bytes, arg, priority, out, affinity) == pdPASS;
}

void finish_task(const TaskMemory& memory) noexcept {
    if (memory.external()) {
        // A static task deleting itself would leave its memory in the
        // termination list until the idle task runs: park instead, the owner
        // deletes it from outside (release_task) and frees it at once.
        for (;;) vTaskSuspend(nullptr);
    }
    vTaskDelete(nullptr);
    for (;;) {
    }
}

void release_task(TaskHandle_t task, TaskMemory* memory) noexcept {
    if (memory->external()) {
        if (task != nullptr) {
            // It may still be on its way to finish_task() on the other core:
            // deleting a running task defers to the idle task, and the memory
            // freed below would still be in use.
            for (int i = 0; i < 100 && eTaskGetState(task) == eRunning; ++i) vTaskDelay(1);
            vTaskDelete(task);
        }
        heap_caps_free(memory->stack);
        heap_caps_free(memory->tcb);
    }
    *memory = TaskMemory{};
}

const char* stack_place(const TaskMemory& memory) noexcept { return memory.external() ? "psram" : ""; }

}  // namespace btpdev
