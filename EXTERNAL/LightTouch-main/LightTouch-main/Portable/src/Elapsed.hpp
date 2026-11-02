#pragma once
#include <cstdint>
#include "lighttouch.h"

namespace lt {
inline constexpr uint64_t worker_shutdown_timeout_ns = uint64_t(LT_SHUTDOWN_CONFIRMATION_TIMEOUT_MS) * 1000000;
inline constexpr uint64_t parent_shutdown_timeout_ns = uint64_t(LT_SHUTDOWN_GRACE_MS) * 1000000;

inline bool elapsed_exceeds(uint64_t current, uint64_t started, uint64_t duration) {
    return current >= started && current - started > duration;
}
} // namespace lt
