#include "core/Memory.h"

#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#else
#include <cstdlib>
#endif

namespace btpdev {

std::uint8_t* alloc_buffer(std::size_t size) noexcept {
    if (size == 0U) return nullptr;
#if defined(ESP_PLATFORM)
    void* buffer = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == nullptr) buffer = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return static_cast<std::uint8_t*>(buffer);
#else
    return static_cast<std::uint8_t*>(std::malloc(size));
#endif
}

void free_buffer(void* buffer) noexcept {
#if defined(ESP_PLATFORM)
    heap_caps_free(buffer);
#else
    std::free(buffer);
#endif
}

}  // namespace btpdev
