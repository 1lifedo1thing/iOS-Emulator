#include "recorder.h"

#include "skin.h"

#include <objbase.h>

#include <codecapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace {

constexpr int kFramesPerSecond = 30;
constexpr int kSampleRate = 44100, kChannels = 2, kBytesPerSample = 2 * kChannels;   // as the engine's mixer gives it
constexpr int64_t kSecond = 10'000'000;   // Media Foundation counts time in units of 100 ns

// How far (in seconds) the sound may fall behind the picture before silence fills the gap: while
// nothing arrives, and when a packet arrives. Both are more than a packet takes to arrive, so sound
// that's playing is never interrupted.
constexpr double kQuietGap = 0.4, kPacketGap = 0.2;

void Check(HRESULT result, const char *what) {
    if (FAILED(result)) {
        char text[128];
        snprintf(text, sizeof text, "Couldn't %s (error 0x%08lX)", what, static_cast<unsigned long>(result));
        throw std::runtime_error(text);
    }
}

// What the video's stream looks like, before and after the encoder. The colours are BT.709.
ComPtr<IMFMediaType> VideoType(const GUID &format, int width, int height) {
    ComPtr<IMFMediaType> type;
    Check(MFCreateMediaType(&type), "describe the video");
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, format);
    type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, kFramesPerSecond, 1);
    MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    return type;
}

// The same for the sound.
ComPtr<IMFMediaType> AudioType(const GUID &format, int bytesPerSecond) {
    ComPtr<IMFMediaType> type;
    Check(MFCreateMediaType(&type), "describe the sound");
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    type->SetGUID(MF_MT_SUBTYPE, format);
    type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kSampleRate);
    type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kChannels);
    type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bytesPerSecond);
    return type;
}

}  // namespace

Recorder::Recorder(const fs::path &file, int panelWidth, int panelHeight, bool landscape)
    : file_(file), panelWidth_(panelWidth), panelHeight_(panelHeight) {
    // The video's size: the screen upright (a panel wider than tall scans a quarter turn from
    // upright, see ToPanel), then on its side in landscape. The encoder needs even sizes.
    const bool turned = panelWidth > panelHeight;
    width_ = std::min(panelWidth, panelHeight);
    height_ = std::max(panelWidth, panelHeight);
    if (landscape)
        std::swap(width_, height_);
    width_ &= ~1;
    height_ &= ~1;

    const Orientation orientation = landscape ? Orientation::Landscape : Orientation::Portrait;
    panelOffset_.resize(size_t(width_) * height_);
    for (int y = 0; y < height_; y++)
        for (int x = 0; x < width_; x++) {
            const ImVec2 panel = ToPanel(ImVec2((x + 0.5f) / width_, (y + 0.5f) / height_), orientation, turned);
            const int column = std::min(int(panel.x * panelWidth), panelWidth - 1);
            const int row = std::min(int(panel.y * panelHeight), panelHeight - 1);
            panelOffset_[size_t(y) * width_ + x] = (size_t(row) * panelWidth + column) * 4;
        }
    latest_.resize(size_t(panelWidth) * panelHeight * 4);
    pixels_.resize(latest_.size());
    nv12_.resize(size_t(width_) * height_ * 3 / 2);

    std::promise<void> opened;
    std::future<void> result = opened.get_future();
    thread_ = std::thread(&Recorder::Run, this, std::move(opened));
    try {
        result.get();
    } catch (...) {
        thread_.join();
        std::error_code ignored;
        fs::remove(file_, ignored);
        throw;
    }
}

Recorder::~Recorder() { Finish(); }

void Recorder::Frame(const uint8_t *bgra, int width, int height, int stride) {
    if (width != panelWidth_ || height != panelHeight_)
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (int row = 0; row < height; row++)
        memcpy(latest_.data() + size_t(row) * width * 4, bgra + size_t(row) * stride, size_t(width) * 4);
    fresh_ = true;
}

void Recorder::Sound(const void *pcm, size_t bytes) {
    const auto *data = static_cast<const uint8_t *>(pcm);
    std::lock_guard<std::mutex> lock(mutex_);
    if (sound_.size() < 256)   // the limit only matters after the encoding thread has failed
        sound_.emplace_back(data, data + bytes);
}

std::string Recorder::Finish() {
    if (thread_.joinable()) {
        finishing_ = true;
        thread_.join();
        if (!error_.empty()) {
            std::error_code ignored;
            fs::remove(file_, ignored);
        }
    }
    return error_;
}

// The encoding thread. Media Foundation's objects are all made and used here.
void Recorder::Run(std::promise<void> opened) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool started = false, framework = false;
    try {
        // The framework's DLLs are delay-loaded: "N" editions of Windows lack them unless the Media
        // Feature Pack is installed, and that must cost the recording, not the whole app.
        if (!LoadLibraryW(L"mfreadwrite.dll"))
            throw std::runtime_error("Recording needs Windows' media components (the Media Feature Pack).");
        Check(MFStartup(MF_VERSION), "start Media Foundation");
        framework = true;
        Open();
        started = true;
        opened.set_value();
        Encode();
    } catch (const std::exception &e) {
        Log("Recording: %s", e.what());
        if (started)
            error_ = e.what();
        else
            opened.set_exception(std::current_exception());
    }
    if (writer_)
        writer_->Release();
    writer_ = nullptr;
    if (framework)
        MFShutdown();
    if (SUCCEEDED(com))
        CoUninitialize();
}

void Recorder::Open() {
    ComPtr<IMFSinkWriter> writer;
    Check(MFCreateSinkWriterFromURL(file_.c_str(), nullptr, nullptr, &writer), "create the video file");

    // Bits per second that keep the iOS interface crisp, whatever the screen's size.
    const UINT32 bitrate = std::max<UINT32>(2'000'000, UINT32(width_ * height_ * kFramesPerSecond * 0.15));
    ComPtr<IMFMediaType> video = VideoType(MFVideoFormat_H264, width_, height_);
    video->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    video->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    Check(writer->AddStream(video.Get(), &videoStream_), "set up the H.264 encoder");
    ComPtr<IMFMediaType> frames = VideoType(MFVideoFormat_NV12, width_, height_);
    frames->SetUINT32(MF_MT_DEFAULT_STRIDE, UINT32(width_));
    Check(writer->SetInputMediaType(videoStream_, frames.Get(), nullptr), "feed the H.264 encoder");

    ComPtr<IMFMediaType> sound = AudioType(MFAudioFormat_AAC, 24000);   // 192 kbit/s
    Check(writer->AddStream(sound.Get(), &audioStream_), "set up the AAC encoder");
    ComPtr<IMFMediaType> samples = AudioType(MFAudioFormat_PCM, kSampleRate * kBytesPerSample);
    samples->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, kBytesPerSample);
    Check(writer->SetInputMediaType(audioStream_, samples.Get(), nullptr), "feed the AAC encoder");

    Check(writer->BeginWriting(), "start the video file");
    writer_ = writer.Detach();
    Black();
    start_ = NowNs();
}

void Recorder::Encode() {
    int64_t nextFrame = 0;
    for (;;) {
        const bool last = finishing_;
        const double now = Seconds();
        const int64_t frame = int64_t(now * kFramesPerSecond);
        if (frame >= nextFrame) {   // frames skipped after a stall just make the last one show longer
            WriteFrame(frame);
            nextFrame = frame + 1;
        }
        WriteSound(now);
        if (last) {
            WriteSilence(std::max(now, 0.1));   // an instant recording still needs a little sound
            break;
        }
        Sleep(5);
    }
    Check(writer_->Finalize(), "finish the video file");
}

void Recorder::WriteFrame(int64_t index) {
    const bool blank = blank_;
    bool changed = blank != wasBlank_;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fresh_) {
            pixels_.swap(latest_);
            fresh_ = false;
            changed = true;
        }
    }
    if (changed)
        blank ? Black() : Convert();
    wasBlank_ = blank;
    WriteSample(videoStream_, nv12_.data(), nv12_.size(), index * kSecond / kFramesPerSecond, kSecond / kFramesPerSecond);
}

void Recorder::Black() {
    const size_t pixels = size_t(width_) * height_;
    memset(nv12_.data(), 16, pixels);
    memset(nv12_.data() + pixels, 128, nv12_.size() - pixels);
}

// Turns the frame into NV12, what the encoder takes: full-size brightness, then colour for each 2x2
// pixels. Studio-range BT.709, in 8-bit fixed point.
void Recorder::Convert() {
    uint8_t *luma = nv12_.data(), *chroma = luma + size_t(width_) * height_;
    const size_t *offset = panelOffset_.data();
    for (int y = 0; y < height_; y += 2) {
        for (int x = 0; x < width_; x += 2) {
            int blue = 0, green = 0, red = 0;
            for (int i = 0; i < 4; i++) {
                const int at = y * width_ + (i >> 1) * width_ + x + (i & 1);
                const uint8_t *pixel = &pixels_[offset[at]];
                const int b = pixel[0], g = pixel[1], r = pixel[2];
                luma[at] = uint8_t(((47 * r + 157 * g + 16 * b + 128) >> 8) + 16);
                blue += b, green += g, red += r;
            }
            *chroma++ = uint8_t(((-26 * red - 86 * green + 112 * blue + 512) >> 10) + 128);
            *chroma++ = uint8_t(((112 * red - 102 * green - 10 * blue + 512) >> 10) + 128);
        }
    }
}

// Sound goes into the file as it arrives. Nothing arrives while iOS is silent, so that time is
// filled with silence, and sound arriving after a gap lands where it ends now: in step with the
// picture on screen as it plays.
void Recorder::WriteSound(double now) {
    std::deque<std::vector<uint8_t>> packets;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        packets.swap(sound_);
    }
    for (const auto &pcm : packets) {
        const double length = double(pcm.size()) / (kSampleRate * kBytesPerSample);
        if (now - length - double(soundWritten_) / kSampleRate > kPacketGap)
            WriteSilence(now - length);
        WritePcm(pcm.data(), pcm.size());
    }
    WriteSilence(now - kQuietGap);
}

void Recorder::WriteSilence(double until) {
    static const std::vector<uint8_t> quiet(16384);
    const int64_t end = int64_t(until * kSampleRate);
    while (soundWritten_ < end)
        WritePcm(quiet.data(), size_t(std::min<int64_t>(end - soundWritten_, 4096)) * kBytesPerSample);
}

void Recorder::WritePcm(const uint8_t *pcm, size_t bytes) {
    const int64_t samples = int64_t(bytes) / kBytesPerSample;
    WriteSample(audioStream_, pcm, bytes, soundWritten_ * kSecond / kSampleRate, samples * kSecond / kSampleRate);
    soundWritten_ += samples;
}

void Recorder::WriteSample(unsigned long stream, const uint8_t *data, size_t bytes, int64_t time, int64_t duration) {
    ComPtr<IMFMediaBuffer> buffer;
    ComPtr<IMFSample> sample;
    BYTE *memory = nullptr;
    Check(MFCreateMemoryBuffer(DWORD(bytes), &buffer), "make a video buffer");
    Check(buffer->Lock(&memory, nullptr, nullptr), "fill a video buffer");
    memcpy(memory, data, bytes);
    buffer->Unlock();
    buffer->SetCurrentLength(DWORD(bytes));
    Check(MFCreateSample(&sample), "make a video sample");
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(time);
    sample->SetSampleDuration(duration);
    Check(writer_->WriteSample(stream, sample.Get()), "write to the video file");
}
