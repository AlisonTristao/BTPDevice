// FileSystem on a real directory of the host (the same POSIX calls the
// device makes on its VFS mount points).

#include <btpdev/filesystem.h>

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "check.h"

namespace {

std::string make_root() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "btpdev_fs_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    std::string text = root.generic_string();
    return text;
}

void unmounted_does_nothing() {
    btpdev::FileSystem fs;
    char buffer[8];
    std::size_t size = 0U;
    CHECK(!fs.mounted());
    CHECK(!fs.write("a.txt", "x", 1U));
    CHECK(!fs.read("a.txt", buffer, sizeof(buffer), &size));
    CHECK(fs.size("a.txt") == -1);
    std::uint64_t total = 0U, used = 0U;
    CHECK(!fs.usage(&total, &used));
}

void write_read_append_rename_remove(const std::string& root) {
    btpdev::FileSystem fs;
    fs.attach((root + "/").c_str());  // a trailing slash is accepted
    CHECK(fs.mounted());

    CHECK(fs.write("wifi.txt", "casa,123\n", 9U));
    CHECK(fs.append("wifi.txt", "oficina,456\n", 12U));
    CHECK(fs.size("wifi.txt") == 21);
    CHECK(fs.exists("wifi.txt"));

    char buffer[64] = {};
    std::size_t size = 0U;
    CHECK(fs.read("wifi.txt", buffer, sizeof(buffer), &size));
    CHECK(size == 21U && std::memcmp(buffer, "casa,123\noficina,456\n", 21U) == 0);

    // Exactly full is fine; one byte short is refused, not truncated.
    CHECK(fs.read("wifi.txt", buffer, 21U, &size) && size == 21U);
    CHECK(!fs.read("wifi.txt", buffer, 20U, &size));

    CHECK(fs.write("bally.key", "k", 1U));
    CHECK(fs.write("bally.key.imported", "old", 3U));
    CHECK(fs.rename("bally.key", "bally.key.imported"));  // replaces the old one
    CHECK(!fs.exists("bally.key"));
    CHECK(fs.size("bally.key.imported") == 1);

    CHECK(fs.remove("bally.key.imported"));
    CHECK(!fs.exists("bally.key.imported"));
    CHECK(!fs.remove("bally.key.imported"));
    CHECK(fs.write("empty.txt", nullptr, 0U) && fs.size("empty.txt") == 0);
}

void names_cannot_leave_the_root(const std::string& root) {
    btpdev::FileSystem fs;
    fs.attach(root.c_str());
    char out[btpdev::FileSystem::kMaxPath];
    CHECK(!fs.path("../secret", out, sizeof(out)));
    CHECK(!fs.path("data/../../x", out, sizeof(out)));
    CHECK(!fs.path("/etc/passwd", out, sizeof(out)));
    CHECK(!fs.path("", out, sizeof(out)));
    CHECK(fs.path("..hidden", out, sizeof(out)));  // a name that merely starts with ".."
    CHECK(fs.path("data/log.csv", out, sizeof(out)));
    const std::string long_name(200, 'a');
    CHECK(!fs.path(long_name.c_str(), out, sizeof(out)));
}

struct Listing {
    std::vector<std::string> names;
    long total = 0;
};

void list_visits_files_only(const std::string& root) {
    btpdev::FileSystem fs;
    fs.attach(root.c_str());
    std::filesystem::create_directories(root + "/subdir");
    fs.write("a.txt", "12", 2U);
    fs.write("b.txt", "345", 3U);
    Listing listing;
    const std::size_t count = fs.list(
        [](void* context, const char* name, long size) {
            auto* l = static_cast<Listing*>(context);
            l->names.emplace_back(name);
            l->total += size;
        },
        &listing);
    CHECK(count == listing.names.size());
    bool has_a = false, has_subdir = false;
    for (const auto& n : listing.names) {
        has_a = has_a || n == "a.txt";
        has_subdir = has_subdir || n == "subdir";
    }
    CHECK(has_a && !has_subdir);
}

void usage_comes_from_the_backend(const std::string& root) {
    btpdev::FileSystem fs;
    fs.attach(root.c_str(), [](void*, std::uint64_t* total, std::uint64_t* used) {
        *total = 1000U;
        *used = 250U;
        return true;
    });
    std::uint64_t total = 0U, used = 0U;
    CHECK(fs.usage(&total, &used) && total == 1000U && used == 250U);
}

}  // namespace

int main() {
    const std::string root = make_root();
    unmounted_does_nothing();
    write_read_append_rename_remove(root);
    names_cannot_leave_the_root(root);
    list_visits_files_only(root);
    usage_comes_from_the_backend(root);
    std::filesystem::remove_all(root);
    return test_result("test_filesystem");
}
