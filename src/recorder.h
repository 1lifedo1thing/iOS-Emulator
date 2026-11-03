// Records a running device's screen and sound to an MP4 file (H.264 and AAC), with Media Foundation,
// the media framework built into Windows.
//
// The worker hands it every new screen frame and sound packet. Those calls only store them: a thread
// of its own encodes, so a recording never slows the guest down. It writes 30 frames per second
// whatever the guest draws, repeating the last frame while the screen stands still, and fills the
// gaps in the sound (iOS sends none while it's silent) so that picture and sound stay together.
#pragma once

#include "common.h"

#include <deque>
#include <future>
#include <mutex>
#include <thread>

struct IMFSinkWriter;

class Recorder {
public:
    // Starts writing `file`. The panel's size is the frames' size. The video stands upright, or on its
    // side with `landscape`, for its whole length. Throws if the file can't be started.
    Recorder(const fs::path &file, int panelWidth, int panelHeight, bool landscape);
    ~Recorder();
    Recorder(const Recorder &) = delete;
    Recorder &operator=(const Recorder &) = delete;

    void Frame(const uint8_t *bgra, int width, int height, int stride);   // a new screen frame
    void Sound(const void *pcm, size_t bytes);                            // 44.1 kHz, stereo, 16-bit
    void Blank(bool blank) { blank_ = blank; }                            // the display is off: black

    // Ends the file and returns what went wrong, if anything (the file is then deleted).
    std::string Finish();
    const fs::path &File() const { return file_; }

private:
    void Run(std::promise<void> opened);
    void Open();
    void Encode();
    void WriteFrame(int64_t index);
    void WriteSound(double now);
    void WriteSilence(double until);
    void WritePcm(const uint8_t *pcm, size_t bytes);
    void WriteSample(unsigned long stream, const uint8_t *data, size_t bytes, int64_t time, int64_t duration);
    void Black();
    void Convert();
    double Seconds() const { return (NowNs() - start_) / 1e9; }

    fs::path file_;
    int panelWidth_, panelHeight_;
    int width_ = 0, height_ = 0;          // the video's size
    std::vector<size_t> panelOffset_;     // for each video pixel, its byte offset in a frame
    uint64_t start_ = 0;                  // NowNs() at the video's first frame

    std::mutex mutex_;                    // what the worker hands over:
    std::vector<uint8_t> latest_;         //   the newest frame...
    bool fresh_ = false;                  //   ...not encoded yet
    std::deque<std::vector<uint8_t>> sound_;
    std::atomic<bool> blank_{false}, finishing_{false};

    // The rest belongs to the encoding thread.
    std::thread thread_;
    IMFSinkWriter *writer_ = nullptr;
    unsigned long videoStream_ = 0, audioStream_ = 0;
    std::vector<uint8_t> pixels_, nv12_;  // the frame being encoded, and as the encoder takes it
    bool wasBlank_ = true;                // what nv12_ holds: starts black
    int64_t soundWritten_ = 0;            // audio samples (per channel) in the file
    std::string error_;
};
