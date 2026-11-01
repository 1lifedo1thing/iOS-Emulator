// Small helpers every module uses: folders, text, files, hashing, child processes, logging,
// and the shared-memory layout the worker uses to hand screen frames to the UI.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "../third_party/json.hpp"

namespace fs = std::filesystem;
using Json = nlohmann::json;

// ---- Folders ---------------------------------------------------------------------------------

fs::path ExeDir();         // the folder holding iOS-Emulator.exe
fs::path ResourcesDir();   // the bundled "resources" folder (next to the exe, or in a parent folder)
fs::path DataDir();        // %LOCALAPPDATA%\iOS-Emulator, where devices, downloads and logs live

// ---- Text and files --------------------------------------------------------------------------

std::wstring Widen(const std::string &utf8);
std::string Narrow(const std::wstring &wide);
std::string PathText(const fs::path &path);   // a path as UTF-8 text
std::string LoadFile(const fs::path &path);
void SaveFile(const fs::path &path, const std::string &data);   // replaces the file atomically
std::string RandomHex(size_t bytes);
std::string Sha256File(const fs::path &path);                    // lowercase hex
std::string Sha256Text(const std::string &data);
std::string Base64Encode(const std::string &data);
std::string Base64Decode(const std::string &text);
std::string FormatBytes(uint64_t bytes);                         // "1.4 GB"
uint64_t NowNs();                                                // monotonic clock, nanoseconds

// printf-style line to stderr, prefixed with the time. Workers and tools send stderr to a log file.
void Log(const char *format, ...);

// ---- Child processes -------------------------------------------------------------------------

struct LaunchOptions {
    std::vector<std::wstring> args;              // arguments after the program path
    std::map<std::string, std::string> env;      // added to (or replacing) this process's environment
    std::vector<std::string> removeEnv;          // variables the child must not inherit
    bool pipeStdin = false;                      // give the child a stdin pipe we write to
    bool pipeStdout = false;                     // give the child a stdout pipe we read from
    fs::path log;                                // stderr (and stdout when not piped) go here; empty = nowhere
    std::vector<HANDLE> inherit;                 // extra handles the child inherits (must be inheritable)
};

// A started child process. It belongs to a job object, so it dies when this process dies.
class ChildProcess {
public:
    ChildProcess() = default;
    ChildProcess(const ChildProcess &) = delete;
    ChildProcess &operator=(const ChildProcess &) = delete;
    ChildProcess(ChildProcess &&other) noexcept { *this = std::move(other); }
    ChildProcess &operator=(ChildProcess &&other) noexcept;
    ~ChildProcess();

    static ChildProcess Launch(const fs::path &program, const LaunchOptions &options);

    bool Running() const;
    int Wait(DWORD milliseconds);   // exit code, or -1 if still running after the wait
    void Kill();
    bool WriteLine(const std::string &line);   // to the child's stdin pipe
    void CloseStdin();
    bool ReadLine(std::string &line);          // from the child's stdout pipe; false at end
    DWORD Pid() const { return pid_; }
    HANDLE Handle() const { return process_; }

private:
    HANDLE process_ = nullptr;
    DWORD pid_ = 0;
    HANDLE stdin_ = nullptr;
    HANDLE stdout_ = nullptr;
    std::string pending_;   // stdout bytes read past the last full line
};

// Reads one '\n'-terminated line from a pipe or file handle. `pending` keeps bytes between calls.
bool ReadLineFrom(HANDLE handle, std::string &pending, std::string &line);
bool WriteAll(HANDLE handle, const std::string &data);

// ---- Frames shared between the worker and the UI ---------------------------------------------

// The worker writes the newest guest screen into a named file mapping; the UI reads it.
// `sequence` works as a seqlock: odd while the worker writes, even when the pixels are stable.
struct SharedFrame {
    static constexpr uint32_t kMagic = 0x4D464549;            // "IEFM"
    static constexpr size_t kHeaderBytes = 64;
    static constexpr size_t kPixelBytes = 16u << 20;          // room for any board's screen
    static constexpr size_t kTotalBytes = kHeaderBytes + kPixelBytes;

    uint32_t magic;
    uint32_t width, height, stride;   // stride in bytes; pixels are BGRA, top row first
    std::atomic<uint64_t> sequence;

    uint8_t *Pixels() { return reinterpret_cast<uint8_t *>(this) + kHeaderBytes; }
};
static_assert(sizeof(SharedFrame) <= SharedFrame::kHeaderBytes);

// A mapped view of a SharedFrame. The UI creates it; the worker opens it by name.
class FrameMapping {
public:
    FrameMapping() = default;
    FrameMapping(const FrameMapping &) = delete;
    FrameMapping &operator=(const FrameMapping &) = delete;
    ~FrameMapping();

    void Create(const std::string &name);
    void Open(const std::string &name);
    SharedFrame *Frame() const { return frame_; }

    // Copies the frame out if its sequence differs from `seen`. Returns true with a new frame.
    bool Read(uint64_t &seen, std::vector<uint8_t> &pixels, int &width, int &height) const;

private:
    HANDLE mapping_ = nullptr;
    SharedFrame *frame_ = nullptr;
};

// Saves a BGRA frame as a PNG file.
void WritePng(const fs::path &path, const uint8_t *bgra, int width, int height);
// Loads an image file (a screenshot) as BGRA pixels, top row first. Throws if it can't.
std::vector<uint8_t> ReadImage(const fs::path &path, int &width, int &height);
