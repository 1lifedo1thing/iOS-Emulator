#include "snapshot.h"

#include <winsock2.h>

#include <algorithm>
#include <ctime>
#include <fstream>
#include <thread>

#pragma comment(lib, "ws2_32.lib")

using namespace std::chrono_literals;

namespace {

// ---- Local TCP -------------------------------------------------------------------------------

// A socket that closes itself.
struct Socket {
    SOCKET s = INVALID_SOCKET;
    explicit Socket(SOCKET socket) : s(socket) {}
    Socket(const Socket &) = delete;
    Socket &operator=(const Socket &) = delete;
    ~Socket() {
        if (s != INVALID_SOCKET)
            closesocket(s);
    }
};

sockaddr_in LocalAddress(int port) {
    static WSADATA wsa;
    static const bool started = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;   // once per process
    (void)started;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(uint16_t(port));
    return address;
}

// A socket listening on 127.0.0.1 at a port Windows picks; `port` gets that port.
SOCKET Listen(int &port) {
    sockaddr_in address = LocalAddress(0);
    int size = sizeof address;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET || bind(s, reinterpret_cast<sockaddr *>(&address), size) != 0 || listen(s, 1) != 0 ||
        getsockname(s, reinterpret_cast<sockaddr *>(&address), &size) != 0) {
        if (s != INVALID_SOCKET)
            closesocket(s);
        throw std::runtime_error("Couldn't open a local port");
    }
    port = ntohs(address.sin_port);
    return s;
}

// Connects to 127.0.0.1:<port>, trying again until `patience` runs out (the engine may not listen yet).
SOCKET Connect(int port, std::chrono::seconds patience) {
    const auto deadline = std::chrono::steady_clock::now() + patience;
    for (;;) {
        sockaddr_in address = LocalAddress(port);
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (connect(s, reinterpret_cast<sockaddr *>(&address), sizeof address) == 0)
            return s;
        closesocket(s);
        if (std::chrono::steady_clock::now() > deadline)
            throw std::runtime_error("The engine isn't listening on port " + std::to_string(port));
        std::this_thread::sleep_for(100ms);
    }
}

void SetTimeout(SOCKET s, DWORD milliseconds) {
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&milliseconds), sizeof milliseconds);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&milliseconds), sizeof milliseconds);
}

void SendAll(SOCKET s, const char *data, size_t size) {
    while (size > 0) {
        const int sent = send(s, data, int(std::min<size_t>(size, 1 << 20)), 0);
        if (sent <= 0)
            throw std::runtime_error("The engine closed the connection");
        data += sent;
        size -= size_t(sent);
    }
}

// ---- What a save writes ----------------------------------------------------------------------

constexpr int kThumbnailSide = 240;   // pixels, along the screen's long side

// The size of a folder's files.
uint64_t FolderBytes(const fs::path &folder) {
    uint64_t bytes = 0;
    for (const auto &entry : fs::recursive_directory_iterator(folder))
        if (entry.is_regular_file())
            bytes += entry.file_size();
    return bytes;
}

// Saves the screen, shrunk by a whole factor: each pixel averages the block it replaces.
void WriteThumbnail(const ScreenPicture &screen, const fs::path &file) {
    const int factor = std::max(1, (std::max(screen.width, screen.height) + kThumbnailSide - 1) / kThumbnailSide);
    const int width = screen.width / factor, height = screen.height / factor;
    std::vector<uint8_t> small(size_t(width) * height * 4);
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++)
            for (int channel = 0; channel < 4; channel++) {
                int sum = 0;
                for (int by = 0; by < factor; by++)
                    for (int bx = 0; bx < factor; bx++)
                        sum += screen.bgra[(size_t(y * factor + by) * screen.width + x * factor + bx) * 4 + channel];
                small[(size_t(y) * width + x) * 4 + channel] = uint8_t(sum / (factor * factor));
            }
    WritePng(file, small.data(), width, height);
}

// Copies a device's state folder, without the serial log and the unclean-shutdown marker.
void CopyState(const fs::path &from, const fs::path &to) {
    fs::create_directories(to);
    for (const auto &entry : fs::directory_iterator(from))
        if (entry.path().filename() != "serial.log" && entry.path().filename() != ".dirty")
            fs::copy(entry.path(), to / entry.path().filename(), fs::copy_options::recursive);
}

// Has qemu migrate the stopped machine to a port of ours, and writes what arrives to `file`.
void ReceiveMachine(Monitor &monitor, const fs::path &file) {
    int port = 0;
    Socket listener(Listen(port));
    // qemu caps a migration's bandwidth (it's meant for live migration over a network): lift it.
    monitor.Execute("migrate-set-parameters", {{"max-bandwidth", 100'000'000'000LL}});
    monitor.Execute("migrate", {{"uri", "tcp:127.0.0.1:" + std::to_string(port)}});

    fd_set set;
    FD_ZERO(&set);
    FD_SET(listener.s, &set);
    timeval wait{30, 0};
    if (select(0, &set, nullptr, nullptr, &wait) != 1)
        throw std::runtime_error("The engine didn't start sending the machine");
    Socket engine(accept(listener.s, nullptr, nullptr));
    SetTimeout(engine.s, 250);

    // A thread writes the stream to the file while this one asks the monitor how the migration
    // is going. (qemu holds its lock while it sends the last of the machine, so the monitor may
    // not answer until then; the stream must keep flowing meanwhile.) qemu doesn't always hang up
    // when it's done: once it reports "completed", everything is on its way, and the first pause
    // in the stream after that is its end.
    std::atomic<bool> completed{false};
    bool written = false;
    std::thread writer([&] {
        std::ofstream out(file, std::ios::binary);
        std::vector<char> buffer(1 << 20);
        for (;;) {
            const int n = recv(engine.s, buffer.data(), int(buffer.size()), 0);
            if (n > 0)
                out.write(buffer.data(), n);
            else if (n == 0 || completed || WSAGetLastError() != WSAETIMEDOUT)
                break;   // qemu hung up, sent everything, or the stream broke off
        }
        written = bool(out.flush());
    });

    std::string problem;
    try {
        for (const auto deadline = std::chrono::steady_clock::now() + 10min;;) {
            const Json info = monitor.Execute("query-migrate");
            const std::string status = info.value("status", "");
            if (status == "completed")
                break;
            if (status == "failed" || status == "cancelled")
                throw std::runtime_error("The engine couldn't save the machine: " + info.value("error-desc", status));
            if (std::chrono::steady_clock::now() > deadline)
                throw std::runtime_error("The engine didn't finish saving the machine");
            std::this_thread::sleep_for(100ms);
        }
    } catch (const std::exception &e) {
        problem = e.what();
        shutdown(engine.s, SD_BOTH);   // ends the writer
    }
    completed = true;
    writer.join();
    if (!problem.empty())
        throw std::runtime_error(problem);
    if (!written)
        throw std::runtime_error("Couldn't write " + PathText(file));
}

}  // namespace

// ---- Save states -----------------------------------------------------------------------------

std::string EngineVersion() {
    static const std::string version = Sha256File(ResourcesDir() / "libqemu-arm.dll");
    return version;
}

SaveState ReadSaveState(const fs::path &folder) {
    if (!fs::exists(folder / "slot.json") || !fs::exists(folder / "machine.state"))
        throw std::runtime_error("There's no save state " + PathText(folder.filename()));
    const Json j = Json::parse(LoadFile(folder / "slot.json"));
    SaveState slot;
    slot.name = PathText(folder.filename());
    slot.folder = folder;
    slot.savedAt = j.value("savedAt", int64_t(0));
    slot.landscape = j.value("landscape", false);
    slot.bytes = j.value("bytes", uint64_t(0));
    slot.engine = j.value("engine", "");
    return slot;
}

std::vector<SaveState> ListSaveStates(const Device &device) {
    std::vector<SaveState> slots;
    std::error_code missing;
    for (const auto &entry : fs::directory_iterator(device.dir / "snapshots", missing)) {
        if (entry.path().extension() == ".saving")
            continue;   // a save that never finished
        try {
            slots.push_back(ReadSaveState(entry.path()));
        } catch (const std::exception &) {
            // a snapshot from before slots: --snapshot still runs it, but its engine isn't known
        }
    }
    std::sort(slots.begin(), slots.end(), [](const SaveState &a, const SaveState &b) {
        if ((a.name == kQuickSlot) != (b.name == kQuickSlot))
            return a.name == kQuickSlot;
        return a.savedAt > b.savedAt;
    });
    return slots;
}

void CheckSaveState(const SaveState &slot) {
    if (slot.engine != EngineVersion())
        throw std::runtime_error("This save state was made by another version of the emulator's engine, and this "
                                 "one can't load it.");
}

bool ValidSlotName(const std::string &name) {
    if (name.find_first_not_of(" .") == std::string::npos)
        return false;
    const bool badEnd = name.back() == '.' || name.back() == ' ';   // Windows drops these
    return !badEnd && name.find_first_of("\\/:*?\"<>|") == std::string::npos && name.find("..") == std::string::npos &&
           fs::path(Widen(name)).extension() != ".saving";
}

void RenameSaveState(const SaveState &slot, const std::string &name) {
    if (!ValidSlotName(name))
        throw std::runtime_error("Not a valid save state name: " + name);
    const fs::path target = slot.folder.parent_path() / Widen(name);
    if (fs::exists(target))
        throw std::runtime_error("There's already a save state called " + name);
    fs::rename(slot.folder, target);
}

void DeleteSaveState(const SaveState &slot) { fs::remove_all(slot.folder); }

// ---- qemu's monitor (QMP) --------------------------------------------------------------------

// One JSON object per line. A command is answered with "return" or "error"; "event" lines can
// come in between and are skipped.
Monitor::Monitor(int port) : socket_(Connect(port, 30s)) {
    SetTimeout(socket_, 5 * 60000);   // it may not answer while qemu sends a large machine
    ReadLine();                       // the greeting
    Execute("qmp_capabilities");
}

Monitor::~Monitor() { closesocket(socket_); }

Json Monitor::Execute(const std::string &command, const Json &arguments) {
    Json request = {{"execute", command}};
    if (!arguments.is_null())
        request["arguments"] = arguments;
    const std::string line = request.dump() + "\r\n";
    SendAll(socket_, line.data(), line.size());
    for (;;) {
        Json reply = Json::parse(ReadLine());
        if (reply.contains("return"))
            return reply["return"];
        if (reply.contains("error"))
            throw std::runtime_error(command + ": " + reply["error"].value("desc", "failed"));
    }
}

std::string Monitor::ReadLine() {
    for (;;) {
        const size_t end = pending_.find('\n');
        if (end != std::string::npos) {
            std::string line = pending_.substr(0, end);
            pending_.erase(0, end + 1);
            return line;
        }
        char buffer[4096];
        const int n = recv(socket_, buffer, sizeof buffer, 0);
        if (n <= 0)
            throw std::runtime_error("The engine's monitor stopped answering");
        pending_.append(buffer, size_t(n));
    }
}

std::vector<std::string> MonitorArgs(int &port) {
    { Socket probe(Listen(port)); }   // a port that is free now; the engine listens on it
    return {"-qmp", "tcp:127.0.0.1:" + std::to_string(port) + ",server=on,wait=off", "-global",
            "migration.store-global-state=off"};
}

// ---- Saving and restoring --------------------------------------------------------------------

void SaveSnapshot(Monitor &monitor, const fs::path &stateDir, const fs::path &folder, const ScreenPicture &screen,
                  bool landscape) {
    fs::path saving = folder;
    saving += ".saving";
    fs::remove_all(saving);
    fs::create_directories(saving);
    const std::string engine = EngineVersion();   // hashing takes a moment: not with the guest stopped

    monitor.Execute("stop");
    std::string problem;
    try {
        ReceiveMachine(monitor, saving / "machine.state");
        CopyState(stateDir, saving / "state");   // the guest is still stopped: the disk matches
        if (screen.width > 0)
            WriteThumbnail(screen, saving / "thumbnail.png");
        const Json slot = {{"savedAt", int64_t(std::time(nullptr))},
                           {"landscape", landscape},
                           {"bytes", FolderBytes(saving)},
                           {"engine", engine}};
        SaveFile(saving / "slot.json", slot.dump(2));
    } catch (const std::exception &e) {
        problem = e.what();
    }
    monitor.Execute("cont");   // also takes the disks back from the finished migration
    if (!problem.empty()) {
        std::error_code ignored;
        fs::remove_all(saving, ignored);
        throw std::runtime_error(problem);
    }
    // The new slot takes over in one step. If the old one can't move, it's still whole.
    fs::path replaced = folder;
    replaced += ".old.saving";
    fs::remove_all(replaced);
    if (fs::exists(folder))
        fs::rename(folder, replaced);
    fs::rename(saving, folder);
    fs::remove_all(replaced);
}

std::vector<std::string> RestoreSnapshot(const fs::path &folder, const fs::path &stateDir) {
    fs::remove_all(stateDir);
    CopyState(folder / "state", stateDir);
    int port = 0;
    { Socket probe(Listen(port)); }   // a port that is free now; the engine listens on it
    std::thread([port, file = folder / "machine.state"] {
        try {
            Socket engine(Connect(port, 120s));
            {   // closed before waiting, as the engine may not hang up: the slot must stay replaceable
                std::ifstream in(file, std::ios::binary);
                std::vector<char> buffer(1 << 20);
                while (in.read(buffer.data(), std::streamsize(buffer.size())) || in.gcount() > 0)
                    SendAll(engine.s, buffer.data(), size_t(in.gcount()));
            }
            shutdown(engine.s, SD_SEND);
            char byte;
            while (recv(engine.s, &byte, 1, 0) > 0) {
            }   // until the engine has read it all and hung up
            Log("Restored the machine from %s", PathText(file).c_str());
        } catch (const std::exception &e) {
            Log("Restoring the machine failed: %s", e.what());
        }
    }).detach();
    return {"-incoming", "tcp:127.0.0.1:" + std::to_string(port)};
}

std::vector<std::string> LoadSaveState(const Device &device, const fs::path &folder) {
    SaveFile(device.LoadMarker(), PathText(folder.filename()));
    std::vector<std::string> args = RestoreSnapshot(folder, device.State());
    fs::remove(device.LoadMarker());
    return args;
}

std::string InterruptedLoad(const Device &device) {
    return fs::exists(device.LoadMarker()) ? LoadFile(device.LoadMarker()) : "";
}
