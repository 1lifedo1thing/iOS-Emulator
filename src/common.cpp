#include "common.h"

#include <bcrypt.h>
#include <wincodec.h>

#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")

// ---- Folders ---------------------------------------------------------------------------------

static fs::path EnvPath(const wchar_t *name) {
    wchar_t buffer[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(name, buffer, DWORD(std::size(buffer)));
    return n > 0 && n < std::size(buffer) ? fs::path(buffer) : fs::path();
}

fs::path ExeDir() {
    wchar_t buffer[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buffer, DWORD(std::size(buffer)));
    return fs::path(std::wstring(buffer, n)).parent_path();
}

fs::path ResourcesDir() {
    static const fs::path found = [] {
        if (fs::path custom = EnvPath(L"IOS_EMULATOR_RESOURCES"); !custom.empty())
            return custom;
        // Released builds keep "resources" beside the exe. While developing, the exe sits in
        // x64\Debug and the folder is at the repository root, so we also look a few levels up.
        fs::path dir = ExeDir();
        for (int level = 0; level < 5 && !dir.empty(); level++) {
            if (fs::exists(dir / "resources" / "libqemu-arm.dll"))
                return dir / "resources";
            if (dir == dir.parent_path())
                break;
            dir = dir.parent_path();
        }
        throw std::runtime_error("Can't find the \"resources\" folder (with libqemu-arm.dll) next to the program.");
    }();
    return found;
}

fs::path DataDir() {
    static const fs::path dir = [] {
        fs::path custom = EnvPath(L"IOS_EMULATOR_DATA");
        fs::path path = !custom.empty() ? custom : EnvPath(L"LOCALAPPDATA") / "iOS-Emulator";
        fs::create_directories(path);
        return path;
    }();
    return dir;
}

// ---- Text and files --------------------------------------------------------------------------

std::wstring Widen(const std::string &utf8) {
    if (utf8.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
    std::wstring wide(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), wide.data(), n);
    return wide;
}

std::string Narrow(const std::wstring &wide) {
    if (wide.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), utf8.data(), n, nullptr, nullptr);
    return utf8;
}

std::string PathText(const fs::path &path) { return Narrow(path.wstring()); }

std::string LoadFile(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("Can't read " + PathText(path));
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

void SaveFile(const fs::path &path, const std::string &data) {
    fs::path temp = path;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out || !out.write(data.data(), std::streamsize(data.size())))
            throw std::runtime_error("Can't write " + PathText(temp));
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Can't replace " + PathText(path));
}

static std::string Hex(const uint8_t *bytes, size_t count) {
    static const char digits[] = "0123456789abcdef";
    std::string text;
    for (size_t i = 0; i < count; i++) {
        text += digits[bytes[i] >> 4];
        text += digits[bytes[i] & 15];
    }
    return text;
}

std::string RandomHex(size_t bytes) {
    std::vector<uint8_t> random(bytes);
    BCryptGenRandom(nullptr, random.data(), ULONG(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return Hex(random.data(), bytes);
}

// SHA-256 with Windows' own CNG, fed by `feed` until it returns an empty chunk.
template <typename Feed>
static std::string Sha256(Feed feed) {
    BCRYPT_HASH_HANDLE hash = nullptr;
    BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0);
    for (;;) {
        std::string_view chunk = feed();
        if (chunk.empty())
            break;
        BCryptHashData(hash, (PUCHAR)chunk.data(), ULONG(chunk.size()), 0);
    }
    uint8_t digest[32];
    BCryptFinishHash(hash, digest, sizeof digest, 0);
    BCryptDestroyHash(hash);
    return Hex(digest, sizeof digest);
}

std::string Sha256Text(const std::string &data) {
    bool fed = false;
    return Sha256([&]() -> std::string_view {
        if (fed)
            return {};
        fed = true;
        return data.empty() ? std::string_view() : std::string_view(data);
    });
}

std::string Sha256File(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("Can't read " + PathText(path));
    std::vector<char> buffer(1 << 20);
    return Sha256([&]() -> std::string_view {
        in.read(buffer.data(), std::streamsize(buffer.size()));
        return std::string_view(buffer.data(), size_t(in.gcount()));
    });
}

static const char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64Encode(const std::string &data) {
    std::string out;
    size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        uint32_t v = uint8_t(data[i]) << 16 | uint8_t(data[i + 1]) << 8 | uint8_t(data[i + 2]);
        out += {kBase64[v >> 18], kBase64[(v >> 12) & 63], kBase64[(v >> 6) & 63], kBase64[v & 63]};
    }
    if (i + 1 == data.size()) {
        uint32_t v = uint8_t(data[i]) << 16;
        out += {kBase64[v >> 18], kBase64[(v >> 12) & 63], '=', '='};
    } else if (i + 2 == data.size()) {
        uint32_t v = uint8_t(data[i]) << 16 | uint8_t(data[i + 1]) << 8;
        out += {kBase64[v >> 18], kBase64[(v >> 12) & 63], kBase64[(v >> 6) & 63], '='};
    }
    return out;
}

std::string Base64Decode(const std::string &text) {
    std::string out;
    uint32_t value = 0;
    int bits = 0;
    for (char c : text) {
        const char *at = strchr(kBase64, c);
        if (c == '\0' || !at)
            continue;   // skips '=', newlines and anything else that isn't data
        value = value << 6 | uint32_t(at - kBase64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += char((value >> bits) & 0xff);
        }
    }
    return out;
}

std::string FormatBytes(uint64_t bytes) {
    const char *units[] = {"bytes", "KB", "MB", "GB", "TB"};
    double value = double(bytes);
    int unit = 0;
    while (value >= 1000 && unit < 4) {
        value /= 1000;
        unit++;
    }
    char text[32];
    snprintf(text, sizeof text, unit == 0 ? "%.0f %s" : "%.1f %s", value, units[unit]);
    return text;
}

uint64_t NowNs() {
    static const LARGE_INTEGER frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return uint64_t(now.QuadPart / frequency.QuadPart) * 1000000000ull +
           uint64_t(now.QuadPart % frequency.QuadPart) * 1000000000ull / uint64_t(frequency.QuadPart);
}

void Log(const char *format, ...) {
    static std::mutex mutex;
    char message[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof message, format, args);
    va_end(args);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[2200];
    int n = snprintf(line, sizeof line, "%02d:%02d:%02d.%03d %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                     message);
    std::lock_guard<std::mutex> lock(mutex);
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (err && err != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(err, line, DWORD(std::min<int>(n, int(sizeof line) - 1)), &written, nullptr);
    }
}

// ---- Child processes -------------------------------------------------------------------------

// Every child joins this job, which kills them all when this process ends (even if it crashes).
static HANDLE ChildJob() {
    static HANDLE job = [] {
        HANDLE j = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(j, JobObjectExtendedLimitInformation, &limits, sizeof limits);
        return j;
    }();
    return job;
}

// Quotes one argument the way CommandLineToArgvW and the C runtimes split them again.
static std::wstring QuoteArgument(const std::wstring &arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos)
        return arg;
    std::wstring quoted = L"\"";
    size_t slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            slashes++;
            continue;
        }
        if (c == L'"')
            quoted.append(slashes * 2 + 1, L'\\');
        else
            quoted.append(slashes, L'\\');
        slashes = 0;
        quoted += c;
    }
    quoted.append(slashes * 2, L'\\');
    return quoted + L"\"";
}

// This process's environment with `options`' changes, as the block CreateProcessW takes.
static std::wstring EnvironmentBlock(const LaunchOptions &options) {
    struct NoCase {
        bool operator()(const std::wstring &a, const std::wstring &b) const { return _wcsicmp(a.c_str(), b.c_str()) < 0; }
    };
    std::map<std::wstring, std::wstring, NoCase> vars;
    wchar_t *strings = GetEnvironmentStringsW();
    for (wchar_t *entry = strings; *entry; entry += wcslen(entry) + 1) {
        const wchar_t *equals = wcschr(entry + 1, L'=');   // names like "=C:" start with '='
        if (equals)
            vars[std::wstring(entry, size_t(equals - entry))] = equals + 1;
    }
    FreeEnvironmentStringsW(strings);
    for (const auto &name : options.removeEnv)
        vars.erase(Widen(name));
    for (const auto &[name, value] : options.env)
        vars[Widen(name)] = Widen(value);
    std::wstring block;
    for (const auto &[name, value] : vars)
        block += name + L"=" + value + L'\0';
    return block + L'\0';
}

ChildProcess &ChildProcess::operator=(ChildProcess &&other) noexcept {
    std::swap(process_, other.process_);
    std::swap(pid_, other.pid_);
    std::swap(stdin_, other.stdin_);
    std::swap(stdout_, other.stdout_);
    std::swap(pending_, other.pending_);
    return *this;
}

ChildProcess::~ChildProcess() {
    CloseStdin();
    if (stdout_)
        CloseHandle(stdout_);
    if (process_)
        CloseHandle(process_);
}

ChildProcess ChildProcess::Launch(const fs::path &program, const LaunchOptions &options) {
    SECURITY_ATTRIBUTES inheritable{sizeof inheritable, nullptr, TRUE};
    HANDLE childIn = nullptr, childOut = nullptr, childErr = nullptr;
    ChildProcess child;
    auto cleanup = [&] {
        for (HANDLE h : {childIn, childOut, childErr})
            if (h && h != INVALID_HANDLE_VALUE)
                CloseHandle(h);
    };
    if (options.pipeStdin) {
        CreatePipe(&childIn, &child.stdin_, &inheritable, 0);
        SetHandleInformation(child.stdin_, HANDLE_FLAG_INHERIT, 0);   // our end stays ours
    }
    if (!options.log.empty()) {
        childErr = CreateFileW(options.log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               &inheritable, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (childErr == INVALID_HANDLE_VALUE)
            childErr = nullptr;
    }
    if (options.pipeStdout) {
        CreatePipe(&child.stdout_, &childOut, &inheritable, 0);
        SetHandleInformation(child.stdout_, HANDLE_FLAG_INHERIT, 0);
    } else if (childErr) {
        DuplicateHandle(GetCurrentProcess(), childErr, GetCurrentProcess(), &childOut, 0, TRUE, DUPLICATE_SAME_ACCESS);
    }

    // Only the handles listed here reach the child, so one child never holds another's pipes open.
    std::vector<HANDLE> handles;
    for (HANDLE h : {childIn, childOut, childErr})
        if (h)
            handles.push_back(h);
    handles.insert(handles.end(), options.inherit.begin(), options.inherit.end());
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<uint8_t> attributeMemory(size);
    auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeMemory.data());
    InitializeProcThreadAttributeList(attributes, 1, 0, &size);
    if (!handles.empty())
        UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles.data(),
                                  handles.size() * sizeof(HANDLE), nullptr, nullptr);

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof startup;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = childIn;
    startup.StartupInfo.hStdOutput = childOut;
    startup.StartupInfo.hStdError = childErr;
    startup.lpAttributeList = attributes;

    std::wstring commandLine = QuoteArgument(program.wstring());
    for (const auto &arg : options.args)
        commandLine += L" " + QuoteArgument(arg);
    std::wstring environment = EnvironmentBlock(options);
    PROCESS_INFORMATION info{};
    BOOL ok = CreateProcessW(program.c_str(), commandLine.data(), nullptr, nullptr, !handles.empty(),
                             CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT |
                                 EXTENDED_STARTUPINFO_PRESENT,
                             environment.data(), program.parent_path().c_str(), &startup.StartupInfo, &info);
    DWORD error = GetLastError();
    DeleteProcThreadAttributeList(attributes);
    cleanup();
    if (!ok)
        throw std::runtime_error("Couldn't start " + PathText(program.filename()) + " (error " +
                                 std::to_string(error) + ")");
    AssignProcessToJobObject(ChildJob(), info.hProcess);
    ResumeThread(info.hThread);
    CloseHandle(info.hThread);
    child.process_ = info.hProcess;
    child.pid_ = info.dwProcessId;
    return child;
}

bool ChildProcess::Running() const { return process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT; }

int ChildProcess::Wait(DWORD milliseconds) {
    if (!process_ || WaitForSingleObject(process_, milliseconds) != WAIT_OBJECT_0)
        return -1;
    DWORD code = 0;
    GetExitCodeProcess(process_, &code);
    return int(code);
}

void ChildProcess::Kill() {
    if (Running())
        TerminateProcess(process_, 1);
}

bool ChildProcess::WriteLine(const std::string &line) { return stdin_ && WriteAll(stdin_, line + "\n"); }

void ChildProcess::CloseStdin() {
    if (stdin_)
        CloseHandle(stdin_);
    stdin_ = nullptr;
}

bool ChildProcess::ReadLine(std::string &line) { return stdout_ && ReadLineFrom(stdout_, pending_, line); }

bool ReadLineFrom(HANDLE handle, std::string &pending, std::string &line) {
    for (;;) {
        size_t end = pending.find('\n');
        if (end != std::string::npos) {
            line = pending.substr(0, end);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            pending.erase(0, end + 1);
            return true;
        }
        char buffer[4096];
        DWORD read = 0;
        if (!ReadFile(handle, buffer, sizeof buffer, &read, nullptr) || read == 0)
            return false;
        pending.append(buffer, read);
    }
}

bool WriteAll(HANDLE handle, const std::string &data) {
    size_t done = 0;
    while (done < data.size()) {
        DWORD written = 0;
        if (!WriteFile(handle, data.data() + done, DWORD(data.size() - done), &written, nullptr))
            return false;
        done += written;
    }
    return true;
}

// ---- Frames ----------------------------------------------------------------------------------

FrameMapping::~FrameMapping() {
    if (frame_)
        UnmapViewOfFile(frame_);
    if (mapping_)
        CloseHandle(mapping_);
}

void FrameMapping::Create(const std::string &name) {
    mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, DWORD(SharedFrame::kTotalBytes),
                                  Widen(name).c_str());
    if (!mapping_)
        throw std::runtime_error("Couldn't create the screen memory");
    frame_ = static_cast<SharedFrame *>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, SharedFrame::kTotalBytes));
    if (!frame_)
        throw std::runtime_error("Couldn't map the screen memory");
    frame_->magic = SharedFrame::kMagic;
}

void FrameMapping::Open(const std::string &name) {
    mapping_ = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, Widen(name).c_str());
    if (!mapping_)
        throw std::runtime_error("Couldn't open the screen memory " + name);
    frame_ = static_cast<SharedFrame *>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, SharedFrame::kTotalBytes));
    if (!frame_ || frame_->magic != SharedFrame::kMagic)
        throw std::runtime_error("The screen memory isn't ours");
}

bool FrameMapping::Read(uint64_t &seen, std::vector<uint8_t> &pixels, int &width, int &height) const {
    if (!frame_)
        return false;
    uint64_t before = frame_->sequence.load(std::memory_order_acquire);
    if (before == seen || (before & 1))
        return false;   // nothing new, or the worker is writing right now
    uint32_t w = frame_->width, h = frame_->height, stride = frame_->stride;
    if (w == 0 || h == 0 || stride < w * 4 || size_t(stride) * h > SharedFrame::kPixelBytes)
        return false;
    pixels.resize(size_t(w) * h * 4);
    for (uint32_t row = 0; row < h; row++)
        memcpy(pixels.data() + size_t(row) * w * 4, frame_->Pixels() + size_t(row) * stride, size_t(w) * 4);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (frame_->sequence.load(std::memory_order_relaxed) != before)
        return false;   // torn: the worker wrote meanwhile; the next call gets the newer frame
    seen = before;
    width = int(w);
    height = int(h);
    return true;
}

// Saves a BGRA frame as a PNG with WIC, the image codecs built into Windows.
void WritePng(const fs::path &path, const uint8_t *bgra, int width, int height) {
    HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory *factory = nullptr;
    IWICStream *stream = nullptr;
    IWICBitmapEncoder *encoder = nullptr;
    IWICBitmapFrameEncode *frame = nullptr;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
    if (SUCCEEDED(hr)) hr = frame->SetSize(UINT(width), UINT(height));
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
    // The guest's alpha channel isn't meaningful; the PNG keeps it, so make every pixel opaque.
    std::vector<uint8_t> opaque(bgra, bgra + size_t(width) * height * 4);
    for (size_t i = 3; i < opaque.size(); i += 4)
        opaque[i] = 255;
    if (SUCCEEDED(hr)) hr = frame->WritePixels(UINT(height), UINT(width) * 4, UINT(opaque.size()), opaque.data());
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    for (IUnknown *object : std::initializer_list<IUnknown *>{frame, encoder, stream, factory})
        if (object)
            object->Release();
    if (SUCCEEDED(init))
        CoUninitialize();
    if (FAILED(hr))
        throw std::runtime_error("Couldn't save the screenshot " + PathText(path));
}
