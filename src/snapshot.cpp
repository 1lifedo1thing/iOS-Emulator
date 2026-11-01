#include "snapshot.h"

#include <winsock2.h>

#include <fstream>
#include <thread>

#pragma comment(lib, "ws2_32.lib")

namespace {

using namespace std::chrono_literals;

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

// ---- qemu's monitor (QMP) --------------------------------------------------------------------

// One JSON object per line. A command is answered with "return" or "error"; "event" lines can
// come in between and are skipped.
class Monitor {
public:
    explicit Monitor(int port) : socket_(Connect(port, 5s)) {
        SetTimeout(socket_.s, 5 * 60000);   // it may not answer while qemu sends a large machine
        ReadLine();   // the greeting
        Execute("qmp_capabilities");
    }

    Json Execute(const std::string &command, const Json &arguments = nullptr) {
        Json request = {{"execute", command}};
        if (!arguments.is_null())
            request["arguments"] = arguments;
        const std::string line = request.dump() + "\r\n";
        SendAll(socket_.s, line.data(), line.size());
        for (;;) {
            Json reply = Json::parse(ReadLine());
            if (reply.contains("return"))
                return reply["return"];
            if (reply.contains("error"))
                throw std::runtime_error(command + ": " + reply["error"].value("desc", "failed"));
        }
    }

private:
    std::string ReadLine() {
        for (;;) {
            const size_t end = pending_.find('\n');
            if (end != std::string::npos) {
                std::string line = pending_.substr(0, end);
                pending_.erase(0, end + 1);
                return line;
            }
            char buffer[4096];
            const int n = recv(socket_.s, buffer, sizeof buffer, 0);
            if (n <= 0)
                throw std::runtime_error("The engine's monitor stopped answering");
            pending_.append(buffer, size_t(n));
        }
    }

    Socket socket_;
    std::string pending_;   // bytes read past the last full line
};

// ---- Saving and restoring --------------------------------------------------------------------

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

std::vector<std::string> MonitorArgs(int &port) {
    { Socket probe(Listen(port)); }   // a port that is free now; the engine listens on it
    return {"-qmp", "tcp:127.0.0.1:" + std::to_string(port) + ",server=on,wait=off", "-global",
            "migration.store-global-state=off"};
}

void SaveSnapshot(int monitorPort, const fs::path &stateDir, const fs::path &folder) {
    fs::path saving = folder;
    saving += ".saving";
    fs::remove_all(saving);
    fs::create_directories(saving);

    Monitor monitor(monitorPort);
    monitor.Execute("stop");
    std::string problem;
    try {
        ReceiveMachine(monitor, saving / "machine.state");
        CopyState(stateDir, saving / "state");   // the guest is still stopped: the disk matches
    } catch (const std::exception &e) {
        problem = e.what();
    }
    monitor.Execute("cont");   // also takes the disks back from the finished migration
    if (!problem.empty()) {
        std::error_code ignored;
        fs::remove_all(saving, ignored);
        throw std::runtime_error(problem);
    }
    fs::remove_all(folder);
    fs::rename(saving, folder);
}

std::vector<std::string> RestoreSnapshot(const fs::path &folder, const fs::path &stateDir) {
    fs::remove_all(stateDir);
    CopyState(folder / "state", stateDir);
    int port = 0;
    { Socket probe(Listen(port)); }   // a port that is free now; the engine listens on it
    std::thread([port, file = folder / "machine.state"] {
        try {
            Socket engine(Connect(port, 120s));
            std::ifstream in(file, std::ios::binary);
            std::vector<char> buffer(1 << 20);
            while (in.read(buffer.data(), std::streamsize(buffer.size())) || in.gcount() > 0)
                SendAll(engine.s, buffer.data(), size_t(in.gcount()));
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
