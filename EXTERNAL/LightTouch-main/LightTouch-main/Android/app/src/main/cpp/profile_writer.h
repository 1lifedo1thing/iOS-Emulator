#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

// Producers enqueue records only. Storage writes never hold the JNI/audio guard
// or this queue's mutex. A stalled writer fails a bounded capture instead of
// blocking presentation and PCM delivery.
class ProfileWriter {
public:
    explicit ProfileWriter(FILE *output, bool sync = false) : file(output), synchronous(sync),
        queue(sync ? 0 : 1024 * 1024) {
        if (!sync) worker=std::thread([this] { run(); });
    }
    ~ProfileWriter() { finish(); }
    void append(const char *format, ...) {
        char line[1024];
        va_list args;
        va_start(args, format);
        if (synchronous) {
            const int n=vfprintf(file,format,args);
            va_end(args);
            if (n < 0) error=true;
            else { bytes+=n; if(bytes>16*1024*1024)error=true; }
            return;
        }
        const int n = vsnprintf(line, sizeof(line), format, args);
        va_end(args);
        std::lock_guard<std::mutex> lock(mutex);
        if (error || closing) return;
        if (n < 0 || size_t(n) >= sizeof(line) || size_t(n) > queue.size() - queued ||
            bytes + size_t(n) > 16 * 1024 * 1024) { error = true; return; }
        const size_t tail = (head + queued) % queue.size();
        const size_t first = std::min(size_t(n), queue.size() - tail);
        memcpy(queue.data() + tail, line, first);
        memcpy(queue.data(), line + first, size_t(n) - first);
        queued += n; bytes += n;
        ready.notify_one();
    }
    bool failed() const { return error; }
    bool finish() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (closing) return !error;
            closing = true; ready.notify_one();
        }
        if (synchronous) { if (fclose(file))error=true; }
        else if (worker.joinable()) worker.join();
        return !error;
    }
private:
    void run() {
        char buffer[65536];
        for (;;) {
            size_t n;
            {
                std::unique_lock<std::mutex> lock(mutex);
                ready.wait(lock, [&] { return queued || closing; });
                n = std::min(queued, sizeof(buffer));
                if (!n && closing) break;
                const size_t first = std::min(n, queue.size() - head);
                memcpy(buffer, queue.data() + head, first);
                memcpy(buffer + first, queue.data(), n - first);
                head = (head + n) % queue.size(); queued -= n;
            }
            if (fwrite(buffer, 1, n, file) != n) error = true;
        }
        if (fclose(file)) error = true;
    }
    FILE *file;
    bool synchronous;
    std::vector<char> queue;
    std::mutex mutex;
    std::condition_variable ready;
    size_t head = 0, queued = 0, bytes = 0;
    bool closing = false;
    std::atomic<bool> error{false};
    std::thread worker;
};
