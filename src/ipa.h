// What an .ipa says about itself, read on Windows before it is sent to iOS.
//
// installd only compares an app's minimum iOS after unpacking it, which takes minutes, and reports
// nothing while it unpacks. Reading both facts here lets the worker refuse an app that can't run
// and show how the unpacking is going.
#pragma once

#include "common.h"

struct IpaInfo {
    std::string minimumOS;       // Info.plist's MinimumOSVersion, empty if unknown
    uint64_t unpackedBytes = 0;  // the size of all its files, 0 if unknown
};

// Never fails: what it can't read stays unknown, and installd has the last word. It uses libplist
// and resources\libz-1.dll, so it runs only in the worker (services.h says why).
IpaInfo ReadIpa(const fs::path &ipa);

// Whether `device` ("6.1.6") is older than `minimum` ("12.0"). False when either is unknown.
bool IosOlderThan(const std::string &device, const std::string &minimum);
