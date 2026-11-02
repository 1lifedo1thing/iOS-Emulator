#include "../src/PcmEpoch.hpp"
#include "../src/VmPauseControl.hpp"
#include <array>
#include <cassert>
#include <cstring>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

struct Packet {
    uint32_t bytes;
    int marker;
};

int main() {
    struct AudioPacket {
        std::array<int16_t, 8192> samples{};
        uint32_t bytes = 0;
        uint64_t timestamp = 0;
    };
    lt::PcmEpochQueue<AudioPacket> audio;
    std::atomic<uint64_t> pulled{17};
    std::array<int16_t, 8192> output;
    output.fill(1234);
    lt_pcm_info info{sizeof(info), 1, 123, 7, 99, 0, 55, 66};
    const auto original_info = info;
    const auto original_output = output;
    for (int poll = 0; poll < 1000; ++poll)
        assert(!lt::pull_pcm(audio, output.data(), info, pulled));
    assert(output == original_output && memcmp(&info, &original_info, sizeof(info)) == 0);
    assert(pulled == 17 && audio.epoch == 1 && audio.dropped == 0 && audio.packets.empty());
    AudioPacket packet;
    packet.samples[0] = -123;
    packet.samples[1] = 456;
    packet.bytes = 4;
    packet.timestamp = 9876;
    assert(audio.append(audio.epoch, packet));
    assert(lt::pull_pcm(audio, output.data(), info, pulled));
    assert(output[0] == -123 && output[1] == 456 && output[2] == 1234);
    assert(info.sample_rate == 44100 && info.channels == 2 && info.frames == 1 && info.timestamp_ns == 9876);
    assert(pulled == 18 && audio.packets.empty());
    assert(audio.append(audio.epoch, packet));
    audio.transition(true); // Same production pause epoch clears pending PCM.
    const auto paused_info = info;
    assert(!lt::pull_pcm(audio, output.data(), info, pulled));
    assert(memcmp(&info, &paused_info, sizeof(info)) == 0 && pulled == 18 && audio.dropped == 1);
    audio.transition(false);
    assert(audio.append(audio.epoch, packet));
    assert(lt::pull_pcm(audio, output.data(), info, pulled));
    assert(info.dropped_frames == 1 && pulled == 19 && audio.epoch == 3);
    lt::PcmEpochQueue<Packet> queue;
    std::mutex mutex;
    std::condition_variable ready;
    bool read_begun = false, finish_read = false;
    assert(queue.append(queue.epoch, {16, 1}));
    std::thread publisher([&] {
        std::unique_lock<std::mutex> lock(mutex);
        const auto old_epoch = queue.epoch;
        read_begun = true;
        ready.notify_one();
        // Actual publisher releases its queue lock during the backend read.
        ready.wait(lock, [&] { return finish_read; });
        assert(!queue.append(old_epoch, {32, 2}));
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        ready.wait(lock, [&] { return read_begun; });
        queue.transition(true);
        assert(queue.packets.empty() && queue.dropped == 4);
        const auto paused_epoch = queue.epoch;
        assert(!queue.append(paused_epoch, {4, 3}));
        queue.transition(false);
        assert(!queue.append(paused_epoch, {4, 4}));
        assert(queue.append(queue.epoch, {8, 5}));
        finish_read = true;
        ready.notify_one();
    }
    publisher.join();
    assert(queue.packets.size() == 1 && queue.packets.front().marker == 5 && queue.dropped == 14);
    const auto resume_epoch = queue.epoch;
    queue.transition(true);
    assert(!queue.append(resume_epoch, {8, 6}));
    queue.transition(false);
    for (int i = 0; i < 17; ++i)
        assert(queue.append(queue.epoch, {4, i}));
    assert(queue.packets.size() == 16 && queue.packets.front().marker == 1);

    lt::PcmCaptureEpoch capture;
    uint64_t next_generation = 0;
    std::vector<uint64_t> stopped;
    auto stop = [&](uint64_t generation) { stopped.push_back(generation); };
    auto start = [&] { return ++next_generation; };
    capture.synchronize(1, false, false, stop, start);
    assert(capture.generation == 1);
    capture.synchronize(2, true, false, stop, start); // Pause requested, not acknowledged yet.
    assert(capture.generation == 0 && stopped == std::vector<uint64_t>{1});
    capture.synchronize(3, false, true, stop, start); // Resume requested, VM still paused.
    assert(capture.generation == 0 && next_generation == 1);
    capture.synchronize(3, false, false, stop, start);
    assert(capture.generation == 2);
    capture.synchronize(4, false, false, stop, start); // Epochs may supersede between publisher ticks.
    assert(capture.generation == 3 && (stopped == std::vector<uint64_t>{1, 2}));
    capture.synchronize(4, false, false, stop, start);
    assert(capture.generation == 3 && next_generation == 3);

    lt::VmPauseControl control;
    using Action = lt::VmPauseControl::Action;
    assert(control.capture_allowed(false));
    assert(control.step(true, true) == Action::none && !control.capture_allowed(true));
    control.request(true);
    control.request(false); // Both host calls precede the next publisher tick.
    assert(control.step(true, false) == Action::none); // Pre-init default PAUSED is not an acknowledgement.
    assert(!control.capture_allowed(false));
    assert(control.step(false, true) == Action::pause);
    assert(control.step(false, true) == Action::none && !control.capture_allowed(false));
    assert(control.step(true, true) == Action::resume && !control.capture_allowed(true));
    assert(control.step(true, true) == Action::none);
    assert(control.step(false, true) == Action::none && control.capture_allowed(false));
    control.request(true);
    assert(control.step(false, true) == Action::pause);
    assert(control.step(true, true) == Action::none && !control.capture_allowed(true));
    control.request(false);
    assert(control.step(true, true) == Action::resume);
    control.request(true); // A new Pause while Resume is in flight stays fenced.
    assert(!control.capture_allowed(false));
    assert(control.step(false, true) == Action::pause);
    assert(control.step(true, true) == Action::none);
    control.request(false);
    assert(control.step(true, true) == Action::resume);
    assert(control.step(false, true) == Action::none && control.capture_allowed(false));

    lt::VmPauseControl shutdown;
    shutdown.request(true);
    assert(shutdown.step(false, true) == Action::pause); // Action returned/queued, VM still running.
    shutdown.shutdown_target();
    assert(shutdown.step(false, true) == Action::none && !shutdown.capture_allowed(false));
    assert(shutdown.step(true, true) == Action::resume && !shutdown.capture_allowed(true));
    assert(shutdown.step(true, true) == Action::none && !shutdown.capture_allowed(true));
    assert(shutdown.step(false, true) == Action::none && shutdown.capture_allowed(false));
    // Production consumes shutdown_requested only after this same ready gate.
    lt::VmPauseControl early_stop;
    early_stop.request(true);
    early_stop.shutdown_target(); // No backend Pause submitted, safe to discard.
    assert(early_stop.step(true, false) == Action::none); // Default pre-init PAUSED cannot acknowledge.
    assert(early_stop.step(false, true) == Action::none && early_stop.capture_allowed(false));
    lt::VmPauseControl unrelated;
    assert(unrelated.step(true, true) == Action::none); // No automatic resume before Stop.
    unrelated.shutdown_target();
    assert(unrelated.step(true, true) == Action::resume);
    assert(unrelated.step(false, true) == Action::none && unrelated.capture_allowed(false));

    const auto stop_read_epoch = queue.epoch;
    queue.transition(true); // request_shutdown clears under the same queue lock.
    assert(queue.packets.empty());
    assert(!queue.append(stop_read_epoch, {4, 99})); // Check→append gap crosses Stop.
}
