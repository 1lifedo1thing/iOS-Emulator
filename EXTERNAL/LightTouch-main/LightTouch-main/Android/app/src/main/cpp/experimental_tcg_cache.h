#pragma once

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <initializer_list>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

// Called only before the first engine start, serialized with worker consumption.
// No caller-provided environment names, cache paths, or cache-record imports.
inline std::string experimental_cache_environment(bool requested,
                                                 const std::string &library,
                                                 const std::string &record,
                                                 bool aggregate_diagnostics = false) {
    if (unsetenv("LT_TCG_IR_CACHE_ENABLE") || unsetenv("LT_TCG_IR_CACHE") ||
        unsetenv("LT_TCG_IR_CACHE_DIAGNOSTICS"))
        return "Cannot clear inherited experimental cache settings";
    if (!requested) return "";
    if (library.empty() || library[0] != '/' || record.empty() || record.size() >= 64 ||
        record.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos)
        return "Experimental cache requires a private library and local device identity";
    int parent = open(library.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (parent < 0) return "Cannot open the private experimental cache library";
    std::string failure;
    for (const auto &name : {std::string("tcg-ir-cache"), record}) {
        if (mkdirat(parent, name.c_str(), 0700) && errno != EEXIST) {
            failure = "Cannot create the private experimental cache directory";
            break;
        }
        int child = openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        struct stat st{};
        if (child < 0 || fstat(child, &st) || st.st_uid != geteuid() ||
            (st.st_mode & 07777) != 0700) {
            if (child >= 0) close(child);
            failure = "Experimental cache requires private 0700 directories; existing files retained";
            break;
        }
        close(parent);
        parent = child;
    }
    close(parent);
    if (!failure.empty()) return failure;
    const auto path = library + "/tcg-ir-cache/" + record;
    if (setenv("LT_TCG_IR_CACHE", path.c_str(), 1) ||
        setenv("LT_TCG_IR_CACHE_ENABLE", "experimental-v1", 1) ||
        (aggregate_diagnostics && setenv("LT_TCG_IR_CACHE_DIAGNOSTICS", "aggregate-v1", 1))) {
        unsetenv("LT_TCG_IR_CACHE_ENABLE");
        unsetenv("LT_TCG_IR_CACHE");
        unsetenv("LT_TCG_IR_CACHE_DIAGNOSTICS");
        return "Cannot configure experimental cache environment";
    }
    return "";
}
