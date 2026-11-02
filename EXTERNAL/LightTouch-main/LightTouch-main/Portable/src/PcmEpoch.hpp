#pragma once
#include "../include/lighttouch.h"
#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <utility>

namespace lt {
// Only the publisher calls this helper and the engine's capture APIs.
struct PcmCaptureEpoch {
    uint64_t epoch = 0, generation = 0;
    template <class Stop, class Start>
    void synchronize(uint64_t requested_epoch, bool suspended, bool vm_paused, Stop stop, Start start) {
        if (epoch != requested_epoch || suspended || vm_paused) {
            if (generation)
                stop(generation);
            generation = 0;
            epoch = requested_epoch;
        }
        if (!suspended && !vm_paused && !generation)
            generation = start(); // Engine start clears its ring synchronously.
    }
};
// All methods require the runtime queue mutex. Capture calls remain owned by
// the publisher; a read begun before a lifecycle boundary cannot append later.
template <class Packet> struct PcmEpochQueue {
    uint64_t epoch = 1, dropped = 0;
    bool suspended = false;
    std::deque<Packet> packets;
    void transition(bool pause) {
        ++epoch;
        suspended = pause;
        for (const auto &packet : packets)
            dropped += packet.bytes / 4;
        packets.clear();
    }
    bool append(uint64_t read_epoch, Packet packet) {
        if (read_epoch != epoch || suspended) {
            dropped += packet.bytes / 4;
            return false;
        }
        if (packets.size() == 16) {
            dropped += packets.front().bytes / 4;
            packets.pop_front();
        }
        packets.push_back(std::move(packet));
        return true;
    }
};
// Caller holds the runtime queue mutex and has validated output ABI/capacity.
// Empty is normal polling, with no exception or mutation of caller outputs.
template <class Packet>
bool pull_pcm(PcmEpochQueue<Packet> &queue, int16_t *out, lt_pcm_info &info, std::atomic<uint64_t> &pulled) {
    if (queue.packets.empty())
        return false;
    const auto &packet = queue.packets.front();
    memcpy(out, packet.samples.data(), packet.bytes);
    info = {sizeof(info), 1, 44100, 2, packet.bytes / 4, 0, packet.timestamp, queue.dropped};
    pulled += packet.bytes / 4;
    queue.packets.pop_front();
    return true;
}
} // namespace lt
