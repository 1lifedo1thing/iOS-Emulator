#include "../src/FrameStorage.hpp"
#include <array>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

static lt_frame_info frame(uint64_t sequence, uint32_t width = 2) {
    return {sizeof(lt_frame_info), 1, width, 1, width * 4, 1, sequence, sequence * 11, uint64_t(width) * 4};
}
struct Shared {
    lt_shared_frame header{sizeof(lt_shared_frame), 1, 0, {}, LT_FRAME_CAPACITY};
    std::array<uint8_t, 256> pixels{};
};
static_assert(offsetof(Shared, pixels) == sizeof(lt_shared_frame));

int main() {
    Shared shared;
    lt_frame_info out = frame(999);
    assert(lt::read_frame_metadata(&shared.header, 0, out) == lt::MetadataRead::again);
    assert(out.sequence == 999);
    std::array<uint8_t, 12> pixels{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    auto f = frame(1, 3);
    assert(lt::publish_frame(&shared.header, f, pixels.data(), pixels.size()));
    assert(shared.header.generation == 2 && !memcmp(shared.pixels.data(), pixels.data(), pixels.size()));
    assert(lt::read_frame_metadata(&shared.header, 0, out) == lt::MetadataRead::fresh && out.sequence == 1);
    assert(lt::read_frame_metadata(&shared.header, 1, out) == lt::MetadataRead::again);
    assert(lt::read_frame_metadata(&shared.header, 2, out) == lt::MetadataRead::again); // Never return older as new.
    unsigned observations = 0;
    out = frame(999);
    assert(lt::read_frame_metadata(&shared.header, 0, out, [&](unsigned) {
               ++observations;
               __atomic_add_fetch(&shared.header.generation, 2, __ATOMIC_SEQ_CST);
           }) == lt::MetadataRead::again);
    assert(observations == 3 && out.sequence == 999);

    // Hold an actual writer's odd generation around malformed intermediate
    // fields. The production reader must retry, not diagnose torn bounds.
    std::mutex mutex;
    std::condition_variable ready;
    bool odd = false, finish = false;
    std::thread interrupted_writer([&] {
        __atomic_add_fetch(&shared.header.generation, 1, __ATOMIC_SEQ_CST);
        auto invalid = frame(2);
        invalid.bytes = LT_FRAME_CAPACITY + 1;
        lt::store_frame_metadata(shared.header.frame, invalid);
        std::unique_lock<std::mutex> lock(mutex);
        odd = true;
        ready.notify_one();
        ready.wait(lock, [&] { return finish; });
        lt::store_frame_metadata(shared.header.frame, frame(2));
        memcpy(shared.pixels.data(), pixels.data(), 8);
        __atomic_add_fetch(&shared.header.generation, 1, __ATOMIC_SEQ_CST);
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        ready.wait(lock, [&] { return odd; });
        assert(lt::read_frame_metadata(&shared.header, 0, out) == lt::MetadataRead::again && out.sequence == 999);
        finish = true;
        ready.notify_one();
    }
    interrupted_writer.join();
    assert(lt::read_frame_metadata(&shared.header, 1, out) == lt::MetadataRead::fresh && out.sequence == 2);

    // Concurrent real publication and metadata reads: each accepted snapshot
    // must tie dimensions, bytes and timestamps to its own sequence.
    std::atomic<bool> done{false};
    std::thread writer([&] {
        for (uint64_t sequence = 3; sequence <= 1000; ++sequence) {
            const auto publication = frame(sequence, sequence % 2 ? 2 : 3);
            assert(lt::publish_frame(&shared.header, publication, pixels.data(), pixels.size()));
        }
        done = true;
    });
    uint64_t since = 2;
    do {
        if (lt::read_frame_metadata(&shared.header, since, out) == lt::MetadataRead::fresh) {
            assert(out.width == (out.sequence % 2 ? 2u : 3u));
            assert(out.stride == out.width * 4 && out.bytes == out.stride && out.timestamp_ns == out.sequence * 11);
            since = out.sequence;
        }
    } while (!done);
    writer.join();

    auto invalid = frame(1001);
    invalid.bytes = LT_FRAME_CAPACITY + 1;
    const auto generation = shared.header.generation;
    assert(!lt::publish_frame(&shared.header, invalid, pixels.data(), pixels.size()));
    assert(shared.header.generation == generation);
    __atomic_add_fetch(&shared.header.generation, 1, __ATOMIC_SEQ_CST);
    lt::store_frame_metadata(shared.header.frame, invalid);
    __atomic_add_fetch(&shared.header.generation, 1, __ATOMIC_SEQ_CST);
    assert(lt::read_frame_metadata(&shared.header, 0, out) == lt::MetadataRead::invalid);
    invalid = frame(1);
    invalid.width = UINT32_MAX;
    assert(!lt::valid_frame(invalid));
    invalid = frame(1);
    invalid.stride = UINT32_MAX;
    invalid.height = 4096;
    invalid.bytes = UINT64_MAX;
    assert(!lt::valid_frame(invalid));

    std::vector<uint8_t> scratch;
    out = frame(999);
    uint64_t caller_sequence = 17;
    auto absent = [&](void *p, size_t capacity, uint64_t requested, lt_frame_info &) {
        assert(!p && !capacity && requested == caller_sequence);
        return lt::FrameCopy::no_frame;
    };
    assert(lt::acquire_frame(scratch, caller_sequence, out, absent) == lt::FrameAcquire::no_frame);
    assert(scratch.empty() && out.sequence == 999 && caller_sequence == 17);
    int copies = 0;
    auto grow = [&](void *p, size_t capacity, uint64_t requested, lt_frame_info &metadata) {
        assert(requested == 17);
        metadata = frame(20, p ? 3 : 2);
        if (!p || capacity < metadata.bytes)
            return lt::FrameCopy::small;
        ++copies;
        memset(p, 0x43, size_t(metadata.bytes));
        return lt::FrameCopy::copied;
    };
    assert(lt::acquire_frame(scratch, caller_sequence, out, grow) == lt::FrameAcquire::copied);
    assert(scratch.size() == 12 && out.sequence == 20 && copies == 1 && caller_sequence == 17);
    Shared copied;
    assert(lt::publish_frame(&copied.header, out, scratch.data(), scratch.size()));
    assert(copied.pixels[11] == 0x43);
    scratch.clear();
    out = frame(999);
    unsigned calls = 0;
    auto changing = [&](void *, size_t, uint64_t requested, lt_frame_info &metadata) {
        assert(requested == 17);
        metadata = frame(21, 2 + calls++);
        return lt::FrameCopy::small;
    };
    assert(lt::acquire_frame(scratch, caller_sequence, out, changing) == lt::FrameAcquire::retry);
    assert(calls == 4 && out.sequence == 999 && caller_sequence == 17 && scratch.size() == 16);
    scratch.clear();
    auto oversize = [&](void *, size_t, uint64_t, lt_frame_info &metadata) {
        metadata = frame(1);
        metadata.bytes = LT_FRAME_CAPACITY + 1;
        return lt::FrameCopy::small;
    };
    assert(lt::acquire_frame(scratch, 17, out, oversize) == lt::FrameAcquire::invalid && scratch.empty());
    bool propagated = false;
    try {
        lt::acquire_frame(scratch, 17, out, [](void *, size_t, uint64_t, lt_frame_info &) -> lt::FrameCopy {
            throw std::runtime_error("engine failure");
        });
    } catch (const std::runtime_error &) {
        propagated = true;
    }
    assert(propagated && out.sequence == 999 && caller_sequence == 17);
}
