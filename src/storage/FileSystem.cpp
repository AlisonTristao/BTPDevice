#include <btpdev/filesystem.h>

#include <cstdio>
#include <cstring>

#include <dirent.h>
#include <sys/stat.h>

namespace btpdev {

void FileSystem::attach(const char* root, UsageFn usage, void* usage_context) noexcept {
    root_[0] = '\0';
    usage_ = usage;
    usage_context_ = usage_context;
    if (root == nullptr) return;
    std::snprintf(root_, sizeof(root_), "%s", root);
    // "/fs/" and "/fs" name the same root.
    const std::size_t length = std::strlen(root_);
    if (length > 1U && root_[length - 1U] == '/') root_[length - 1U] = '\0';
}

bool FileSystem::path(const char* name, char* out, std::size_t capacity) const noexcept {
    if (!mounted() || name == nullptr || name[0] == '\0' || name[0] == '/' || out == nullptr) return false;
    // No way out of the root: ".." as any path component is refused.
    for (const char* p = name; *p != '\0'; ++p) {
        const bool at_component = p == name || p[-1] == '/';
        if (at_component && p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == '\0')) return false;
    }
    const int written = std::snprintf(out, capacity, "%s/%s", root_, name);
    return written > 0 && static_cast<std::size_t>(written) < capacity;
}

bool FileSystem::read(const char* name, void* buffer, std::size_t capacity,
                      std::size_t* size_out) const noexcept {
    char full[kMaxPath];
    if (buffer == nullptr || size_out == nullptr || !path(name, full, sizeof(full))) return false;
    std::FILE* file = std::fopen(full, "rb");
    if (file == nullptr) return false;
    // One byte more than the buffer: a file that fills it exactly is fine,
    // one that does not fit is refused instead of silently cut.
    std::size_t total = 0U;
    bool too_big = false;
    for (;;) {
        if (total == capacity) {
            unsigned char probe = 0U;
            too_big = std::fread(&probe, 1U, 1U, file) == 1U;
            break;
        }
        const std::size_t got = std::fread(static_cast<unsigned char*>(buffer) + total, 1U, capacity - total, file);
        if (got == 0U) break;
        total += got;
    }
    const bool failed = std::ferror(file) != 0;
    std::fclose(file);
    if (failed || too_big) return false;
    *size_out = total;
    return true;
}

namespace {

bool write_mode(const char* full, const void* data, std::size_t size, const char* mode) noexcept {
    std::FILE* file = std::fopen(full, mode);
    if (file == nullptr) return false;
    const bool written = size == 0U || std::fwrite(data, 1U, size, file) == size;
    const bool closed = std::fclose(file) == 0;
    return written && closed;
}

}  // namespace

bool FileSystem::write(const char* name, const void* data, std::size_t size) noexcept {
    char full[kMaxPath];
    if ((data == nullptr && size != 0U) || !path(name, full, sizeof(full))) return false;
    return write_mode(full, data, size, "wb");
}

bool FileSystem::append(const char* name, const void* data, std::size_t size) noexcept {
    char full[kMaxPath];
    if ((data == nullptr && size != 0U) || !path(name, full, sizeof(full))) return false;
    return write_mode(full, data, size, "ab");
}

long FileSystem::size(const char* name) const noexcept {
    char full[kMaxPath];
    struct stat info {};
    if (!path(name, full, sizeof(full)) || ::stat(full, &info) != 0 || !S_ISREG(info.st_mode)) return -1;
    return static_cast<long>(info.st_size);
}

bool FileSystem::exists(const char* name) const noexcept { return size(name) >= 0; }

bool FileSystem::remove(const char* name) noexcept {
    char full[kMaxPath];
    return path(name, full, sizeof(full)) && std::remove(full) == 0;
}

bool FileSystem::rename(const char* from, const char* to) noexcept {
    char full_from[kMaxPath];
    char full_to[kMaxPath];
    if (!path(from, full_from, sizeof(full_from)) || !path(to, full_to, sizeof(full_to))) return false;
    // Some VFS backends refuse to rename onto an existing file.
    if (exists(to)) std::remove(full_to);
    return std::rename(full_from, full_to) == 0;
}

std::size_t FileSystem::list(void (*visit)(void* context, const char* name, long size),
                             void* context) const noexcept {
    if (!mounted() || visit == nullptr) return 0U;
    DIR* dir = ::opendir(root_);
    if (dir == nullptr) return 0U;
    std::size_t count = 0U;
    while (struct dirent* entry = ::readdir(dir)) {
        if (entry->d_name[0] == '.') continue;
        const long bytes = size(entry->d_name);
        if (bytes < 0) continue;  // a directory (or gone meanwhile)
        visit(context, entry->d_name, bytes);
        ++count;
    }
    ::closedir(dir);
    return count;
}

bool FileSystem::usage(std::uint64_t* total_bytes, std::uint64_t* used_bytes) const noexcept {
    return mounted() && usage_ != nullptr && total_bytes != nullptr && used_bytes != nullptr &&
           usage_(usage_context_, total_bytes, used_bytes);
}

}  // namespace btpdev
