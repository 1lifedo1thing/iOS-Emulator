// Host-only private-directory/environment contracts; never loads QEMU or iOS.
// c++ -std=c++17 -Wall -Wextra -Werror -UNDEBUG ExperimentalTcgCache.cpp -o /private/cache-contract
#include "../../main/cpp/experimental_tcg_cache.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/wait.h>

namespace fs = std::filesystem;
static constexpr const char *enable_key = "LT_TCG_IR_CACHE_ENABLE";
static constexpr const char *directory_key = "LT_TCG_IR_CACHE";
static constexpr const char *diagnostics_key = "LT_TCG_IR_CACHE_DIAGNOSTICS";
static void inherited() {
    assert(setenv(enable_key, "experimental-v1", 1) == 0);
    assert(setenv(directory_key, "untrusted", 1) == 0);
    assert(setenv(diagnostics_key, "aggregate-v1", 1) == 0);
}
static void cleared() {
    assert(getenv(enable_key) == nullptr && getenv(directory_key) == nullptr &&
           getenv(diagnostics_key) == nullptr);
}
int main() {
    char name[] = "/tmp/lighttouch-cache-contract-XXXXXX";
    char *created = mkdtemp(name);
    assert(created);
    const fs::path root(created), library = root / "library";
    fs::create_directory(library);
    inherited();
    assert(setenv("LT_TEST_UNRELATED", "retained", 1) == 0);
    assert(experimental_cache_environment(false, "", "").empty());
    cleared();
    assert(std::string(getenv("LT_TEST_UNRELATED")) == "retained");
    assert(!fs::exists(library / "tcg-ir-cache"));

    for (const auto &record : {"", "../escape", "..", "a/b", "a b"}) {
        inherited();
        assert(!experimental_cache_environment(true, library.string(), record).empty());
        cleared();
        assert(!fs::exists(library / "tcg-ir-cache"));
    }
    assert(!experimental_cache_environment(true, "relative", "record").empty());
    const fs::path alias = root / "alias";
    fs::create_directory_symlink(library, alias);
    assert(!experimental_cache_environment(true, alias.string(), "record").empty());
    assert(!fs::exists(library / "tcg-ir-cache"));

    const fs::path outside = root / "outside", cache = library / "tcg-ir-cache";
    fs::create_directory(outside);
    fs::create_directory_symlink(outside, cache);
    assert(!experimental_cache_environment(true, library.string(), "record").empty());
    assert(fs::is_empty(outside));
    fs::remove(cache);
    fs::create_directory(cache);
    assert(chmod(cache.c_str(), 0755) == 0);
    assert(!experimental_cache_environment(true, library.string(), "record").empty());
    struct stat st{};
    assert(stat(cache.c_str(), &st) == 0 && (st.st_mode & 07777) == 0755);
    assert(!fs::exists(cache / "record"));
    assert(chmod(cache.c_str(), 0700) == 0);
    fs::create_directory_symlink(outside, cache / "record");
    assert(!experimental_cache_environment(true, library.string(), "record").empty());
    assert(fs::is_empty(outside));
    fs::remove(cache / "record");

    inherited();
    assert(experimental_cache_environment(true, library.string(), "record").empty());
    const fs::path selected = cache / "record";
    assert(std::string(getenv(directory_key)) == selected.string());
    assert(std::string(getenv(enable_key)) == "experimental-v1");
    assert(getenv(diagnostics_key) == nullptr); // Never inherit aggregate clocks.
    for (const auto &folder : {cache, selected})
        assert(stat(folder.c_str(), &st) == 0 && st.st_uid == geteuid() && (st.st_mode & 07777) == 0700);
    std::ofstream(selected / "retained") << "existing private record";
    assert(experimental_cache_environment(true, library.string(), "record").empty());
    std::ifstream retained(selected / "retained");
    std::string contents;
    std::getline(retained, contents);
    assert(contents == "existing private record");
    assert(experimental_cache_environment(true, library.string(), "second").empty());
    assert(std::string(getenv(directory_key)) != selected.string());
    assert(fs::exists(selected / "retained"));
    assert(experimental_cache_environment(true, library.string(), "second", true).empty());
    assert(std::string(getenv(diagnostics_key)) == "aggregate-v1");
    // A genuine exec inherits only the selected worker environment.
    const pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        execl("/bin/sh", "sh", "-c",
              "test \"$LT_TCG_IR_CACHE_ENABLE\" = experimental-v1 && test -d \"$LT_TCG_IR_CACHE\" && "
              "test \"$LT_TCG_IR_CACHE_DIAGNOSTICS\" = aggregate-v1", nullptr);
        _exit(127);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(experimental_cache_environment(false, "", "").empty());
    cleared();
    inherited();
    assert(experimental_cache_environment(false, "", "", true).empty());
    cleared(); // A diagnostics request alone cannot enable the cache or clocks.
    inherited();
    assert(!experimental_cache_environment(true, library.string(), "../escape", true).empty());
    cleared();
    assert(fs::exists(selected / "retained"));
    fs::remove_all(root);
    std::cout << "PASS: default-off, inherited scrub, identity/path/mode/symlink rejection, per-record retention, fresh exec\n";
}
