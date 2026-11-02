#pragma once
#include "Internal.hpp"
#include <array>
namespace lt {
// Accel/gyro are independent SensorManager streams. One stream cannot consume
// the other's admission clock; equal cross-stream timestamps are valid.
class MotionClock {
    std::array<uint64_t, 2> last{};

  public:
    void validate(uint32_t kinds, uint64_t timestamp, uint64_t now) const {
        require(kinds && !(kinds & ~3u), "invalid motion fields");
        require(timestamp <= now && now - timestamp <= 250000000, "stale/future motion", LT_AGAIN);
        for (unsigned i = 0; i < 2; i++)
            if (kinds & (1u << i))
                require(!last[i] || (timestamp > last[i] && timestamp - last[i] >= 5000000),
                        "motion stream must increase and <=200Hz", LT_AGAIN);
    }
    void commit(uint32_t kinds, uint64_t timestamp) {
        for (unsigned i = 0; i < 2; i++)
            if (kinds & (1u << i))
                last[i] = timestamp;
    }
    void reset() { last = {}; }
};
} // namespace lt
