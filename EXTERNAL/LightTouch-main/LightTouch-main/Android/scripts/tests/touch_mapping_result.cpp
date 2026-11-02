#include "../../app/src/main/cpp/touch_mapping_result.h"
#include <cassert>
#include <iostream>

int main() {
    const std::string expected = "Display input outside guest viewport; gesture cancelled";
    assert(touch_mapping_rejection(LT_AGAIN, "") == expected);
    const auto invalid = touch_mapping_rejection(LT_INVALID, "");
    assert(invalid != expected && invalid == "Invalid touch coordinates or viewport; gesture cancelled");
    const auto failed_release = touch_mapping_rejection(LT_AGAIN, "native cancellation rejected");
    assert(failed_release != expected);
    assert(failed_release == "Touch left guest viewport; gesture cancelled; input release failed: native cancellation rejected");
    assert(touch_mapping_rejection(LT_INVALID, "release unavailable").find("input release failed: release unavailable") != std::string::npos);
    // Other statuses never become the expected boundary-cancellation sentinel.
    for (const auto status : {LT_OK, LT_IO, LT_BUSY, LT_ENGINE})
        assert(touch_mapping_rejection(status, "") != expected);
    std::cout << "PASS production mapping/release result classification\n";
}
