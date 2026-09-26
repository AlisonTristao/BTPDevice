#ifndef BTPDEV_MEMORY_H
#define BTPDEV_MEMORY_H

// The one allocation path of the library (CONTRIBUTING.md "Memória"): used
// only while starting up, never on a hot path. PSRAM first when the chip has
// it, internal RAM otherwise -- the big buffers (queues, COBS scratch) have no
// DMA or speed requirement, and internal RAM is what Wi-Fi and NimBLE need.

#include <cstddef>
#include <cstdint>

namespace btpdev {

/// Returns nullptr when neither PSRAM nor internal RAM has `size` bytes.
std::uint8_t* alloc_buffer(std::size_t size) noexcept;
void free_buffer(void* buffer) noexcept;

/// An owning buffer from alloc_buffer(), freed on reset() / destruction.
class Buffer {
public:
    Buffer() noexcept = default;
    ~Buffer() { reset(); }
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    bool allocate(std::size_t size) noexcept {
        reset();
        data_ = alloc_buffer(size);
        size_ = data_ != nullptr ? size : 0U;
        return data_ != nullptr;
    }
    void reset() noexcept {
        free_buffer(data_);
        data_ = nullptr;
        size_ = 0U;
    }
    std::uint8_t* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }

private:
    std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0U;
};

}  // namespace btpdev

#endif  // BTPDEV_MEMORY_H
