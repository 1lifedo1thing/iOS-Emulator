#pragma once
#include "lighttouch.h"
#include <string>

/* Only an outside-viewport mapping with a satisfied scoped release is an
 * expected cancellation. The caller must perform/confirm its existing release
 * before invoking this helper; an empty error is never fabricated here. */
inline std::string touch_mapping_rejection(lt_status mapping, const std::string &release_error) {
    if (mapping == LT_AGAIN && release_error.empty())
        return "Display input outside guest viewport; gesture cancelled";
    std::string failure = mapping == LT_AGAIN
        ? "Touch left guest viewport; gesture cancelled"
        : "Invalid touch coordinates or viewport; gesture cancelled";
    if (!release_error.empty()) failure += "; input release failed: " + release_error;
    return failure;
}
