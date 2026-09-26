#ifndef BTPDEV_FILESYSTEM_H
#define BTPDEV_FILESYSTEM_H

// Files of the device: the library's own (the networks file, the key file to
// import) and the application's. One class for every backend -- LittleFS on a
// flash partition or an SD card -- because both are mounted into ESP-IDF's
// VFS and then are plain POSIX files under a mount point ("/fs", "/sd").
//
//   btpdev::FileSystem* fs = device.fs();       // nullptr: nothing mounted
//   if (fs != nullptr) fs->append("log.csv", line, len);
//
// Names are relative to the mount point ("wifi.txt", "data/log.csv"); ".."
// and absolute names are refused. Every call is independent (open, work,
// close), so the files can be used from any task, one call at a time per
// file. Portable: covered on the host by test/host/test_filesystem.cpp.

#include <cstddef>
#include <cstdint>

namespace btpdev {

class FileSystem {
public:
    static constexpr std::size_t kMaxPath = 160U;

    /// Space reporting is backend-specific; the mount code provides it.
    using UsageFn = bool (*)(void* context, std::uint64_t* total_bytes, std::uint64_t* used_bytes);

    FileSystem() noexcept = default;

    /// Points at a mounted root ("/fs"). nullptr / "" detaches.
    void attach(const char* root, UsageFn usage = nullptr, void* usage_context = nullptr) noexcept;
    bool mounted() const noexcept { return root_[0] != '\0'; }
    const char* root() const noexcept { return root_; }

    /// Reads the whole file into `buffer`. False when missing, unreadable, or
    /// larger than `capacity` (nothing is truncated silently).
    bool read(const char* name, void* buffer, std::size_t capacity, std::size_t* size_out) const noexcept;
    /// Replaces the file's contents (creating it).
    bool write(const char* name, const void* data, std::size_t size) noexcept;
    bool append(const char* name, const void* data, std::size_t size) noexcept;
    bool exists(const char* name) const noexcept;
    /// Size in bytes, or -1 when the file does not exist.
    long size(const char* name) const noexcept;
    bool remove(const char* name) noexcept;
    /// Renames, replacing `to` if it exists.
    bool rename(const char* from, const char* to) noexcept;
    /// Calls visit(context, name, size) for each file in the root (not
    /// recursive). Returns how many files were visited.
    std::size_t list(void (*visit)(void* context, const char* name, long size), void* context) const noexcept;
    /// Total and used bytes of the volume.
    bool usage(std::uint64_t* total_bytes, std::uint64_t* used_bytes) const noexcept;

    /// "<root>/<name>" into `out`. False for a refused or too long name.
    bool path(const char* name, char* out, std::size_t capacity) const noexcept;

private:
    char root_[64] = {};
    UsageFn usage_ = nullptr;
    void* usage_context_ = nullptr;
};

}  // namespace btpdev

#endif  // BTPDEV_FILESYSTEM_H
