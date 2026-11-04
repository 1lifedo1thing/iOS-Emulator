#include "services.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <libimobiledevice/afc.h>
#include <libimobiledevice/installation_proxy.h>
#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/lockdown.h>
#include <libimobiledevice/syslog_relay.h>
#include <plist/plist.h>
#include <usbmuxd.h>

#include <condition_variable>
#include <fstream>

#pragma comment(lib, "ws2_32.lib")

static const char *kLabel = "iOS-Emulator";   // how we introduce ourselves to iOS

// ---- Helpers ---------------------------------------------------------------------------------

// The libraries allocate with msvcrt's malloc, so their strings go back through libplist's free.
static std::string TakeString(char *text) {
    std::string copy = text ? text : "";
    if (text)
        plist_mem_free(text);
    return copy;
}

// A property list as JSON (binary data becomes base64).
static Json PlistToJson(plist_t node) {
    if (!node)
        return nullptr;
    switch (plist_get_node_type(node)) {
    case PLIST_DICT: {
        Json object = Json::object();
        plist_dict_iter iter = nullptr;
        plist_dict_new_iter(node, &iter);
        for (;;) {
            char *key = nullptr;
            plist_t value = nullptr;
            plist_dict_next_item(node, iter, &key, &value);
            if (!key)
                break;
            object[TakeString(key)] = PlistToJson(value);
        }
        plist_mem_free(iter);
        return object;
    }
    case PLIST_ARRAY: {
        Json array = Json::array();
        for (uint32_t i = 0; i < plist_array_get_size(node); i++)
            array.push_back(PlistToJson(plist_array_get_item(node, i)));
        return array;
    }
    case PLIST_STRING: {
        char *text = nullptr;
        plist_get_string_val(node, &text);
        return TakeString(text);
    }
    case PLIST_BOOLEAN: {
        uint8_t value = 0;
        plist_get_bool_val(node, &value);
        return value != 0;
    }
    case PLIST_INT: {
        uint64_t value = 0;
        plist_get_uint_val(node, &value);
        return value;
    }
    case PLIST_REAL: {
        double value = 0;
        plist_get_real_val(node, &value);
        return value;
    }
    case PLIST_DATA: {
        char *data = nullptr;
        uint64_t length = 0;
        plist_get_data_val(node, &data, &length);
        std::string bytes(data ? data : "", size_t(length));
        if (data)
            plist_mem_free(data);
        return Base64Encode(bytes);
    }
    default:
        return nullptr;
    }
}

static std::string AfcError(afc_error_t error) { return afc_strerror(error) ? afc_strerror(error) : "AFC error"; }

static void CheckAfc(afc_error_t error, const std::string &what) {
    if (error != AFC_E_SUCCESS)
        throw std::runtime_error(what + ": " + AfcError(error));
}

// A lockdown session, the service directory every other service is opened through.
struct Lockdown {
    lockdownd_client_t client = nullptr;

    explicit Lockdown(idevice_t device) {
        // Pairs on first use (the record is kept by the USB helper), then starts a TLS session.
        lockdownd_error_t error = lockdownd_client_new_with_handshake(device, &client, kLabel);
        if (error == LOCKDOWN_E_PASSWORD_PROTECTED)
            throw std::runtime_error("Unlock the device first: iOS won't pair with a computer while locked.");
        if (error != LOCKDOWN_E_SUCCESS)
            throw std::runtime_error(std::string("Couldn't talk to iOS (lockdown: ") + lockdownd_strerror(error) + ")");
    }
    ~Lockdown() {
        if (client)
            lockdownd_client_free(client);
    }

    lockdownd_service_descriptor_t Start(const char *service) {
        lockdownd_service_descriptor_t descriptor = nullptr;
        if (lockdownd_start_service(client, service, &descriptor) != LOCKDOWN_E_SUCCESS)
            return nullptr;
        return descriptor;
    }
};

// ---- Connection ------------------------------------------------------------------------------

Services::~Services() {
    StopForward();
    StopSyslog();
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    ResetAfc();
    if (device_)
        idevice_free(device_);
}

idevice_t Services::Device() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (device_)
        return device_;
    // The guest appears on the USB helper a few seconds into booting.
    char **udids = nullptr;
    int count = 0;
    if (idevice_get_device_list(&udids, &count) != IDEVICE_E_SUCCESS || count == 0) {
        if (udids)
            idevice_device_list_free(udids);
        throw std::runtime_error("iOS hasn't connected over USB yet. Wait until it has started up.");
    }
    idevice_error_t error = idevice_new_with_options(&device_, udids[0], IDEVICE_LOOKUP_USBMUX);
    idevice_device_list_free(udids);
    if (error != IDEVICE_E_SUCCESS)
        throw std::runtime_error("Couldn't open the device over USB");
    return device_;
}

std::string Services::Udid() {
    char *udid = nullptr;
    idevice_get_udid(Device(), &udid);
    return TakeString(udid);
}

afc_client_t Services::Afc() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (afc_)
        return afc_;
    Lockdown lockdown(Device());
    // AFC2 is the jailbreak's root-filesystem file service; plain AFC sees only the Media folder.
    lockdownd_service_descriptor_t service = lockdown.Start("com.apple.afc2");
    afcIsRoot_ = service != nullptr;
    if (!service)
        service = lockdown.Start("com.apple.afc");
    if (!service)
        throw std::runtime_error("iOS refused the file service");
    afc_error_t error = afc_client_new(Device(), service, &afc_);
    lockdownd_service_descriptor_free(service);
    CheckAfc(error, "Couldn't open the file service");
    return afc_;
}

void Services::ResetAfc() {
    if (afc_)
        afc_client_free(afc_);
    afc_ = nullptr;
}

// ---- Information -----------------------------------------------------------------------------

Json Services::Info() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Lockdown lockdown(Device());
    plist_t values = nullptr;
    lockdownd_get_value(lockdown.client, nullptr, nullptr, &values);
    Json info = PlistToJson(values);
    plist_free(values);
    if (!info.is_object())
        info = Json::object();
    // The disk, as the file service sees it.
    char **disk = nullptr;
    if (afc_get_device_info(Afc(), &disk) == AFC_E_SUCCESS && disk) {
        for (int i = 0; disk[i] && disk[i + 1]; i += 2)
            info["Disk"][disk[i]] = disk[i + 1];
        afc_dictionary_free(disk);
    }
    info["RootFileAccess"] = afcIsRoot_;
    return info;
}

bool Services::RootAccess() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Afc();
    return afcIsRoot_;
}

// ---- Files -----------------------------------------------------------------------------------

static std::string Join(const std::string &folder, const std::string &name) {
    return folder.empty() || folder.back() == '/' ? folder + name : folder + "/" + name;
}

Json Services::ListFiles(const std::string &path) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    afc_client_t afc = Afc();
    char **names = nullptr;
    afc_error_t error = afc_read_directory(afc, path.c_str(), &names);
    if (error != AFC_E_SUCCESS) {
        ResetAfc();   // a broken connection is reopened next time
        CheckAfc(error, "Can't list " + path);
    }
    Json entries = Json::array();
    for (int i = 0; names && names[i]; i++) {
        const std::string name = names[i];
        if (name == "." || name == "..")
            continue;
        Json entry = {{"name", name}, {"type", "file"}, {"size", 0}, {"modified", 0}};
        char **info = nullptr;
        if (afc_get_file_info(afc, Join(path, name).c_str(), &info) == AFC_E_SUCCESS && info) {
            for (int k = 0; info[k] && info[k + 1]; k += 2) {
                const std::string key = info[k], value = info[k + 1];
                if (key == "st_ifmt")
                    entry["type"] = value == "S_IFDIR" ? "folder" : value == "S_IFLNK" ? "link" : "file";
                else if (key == "st_size")
                    entry["size"] = std::stoull(value);
                else if (key == "st_mtime")
                    entry["modified"] = std::stoull(value) / 1000000000ull;   // nanoseconds -> seconds
                else if (key == "LinkTarget")
                    entry["target"] = value;
            }
            afc_dictionary_free(info);
        }
        entries.push_back(entry);
    }
    if (names)
        afc_dictionary_free(names);
    // Folders first, then by name.
    std::sort(entries.begin(), entries.end(), [](const Json &a, const Json &b) {
        const bool af = a["type"] == "folder", bf = b["type"] == "folder";
        return af != bf ? af : a["name"].get<std::string>() < b["name"].get<std::string>();
    });
    return entries;
}

void Services::DownloadFile(const std::string &path, const fs::path &to) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    afc_client_t afc = Afc();
    uint64_t handle = 0;
    CheckAfc(afc_file_open(afc, path.c_str(), AFC_FOPEN_RDONLY, &handle), "Can't open " + path);
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    std::vector<char> buffer(1 << 16);
    afc_error_t error = AFC_E_SUCCESS;
    for (;;) {
        uint32_t read = 0;
        error = afc_file_read(afc, handle, buffer.data(), uint32_t(buffer.size()), &read);
        if (error != AFC_E_SUCCESS || read == 0)
            break;
        out.write(buffer.data(), read);
    }
    afc_file_close(afc, handle);
    CheckAfc(error, "Can't read " + path);
    if (!out)
        throw std::runtime_error("Can't write " + PathText(to));
}

void Services::Upload(afc_client_t afc, const fs::path &from, const std::string &path,
                      const std::function<void(uint64_t, uint64_t)> &progress) {
    std::ifstream in(from, std::ios::binary);
    if (!in)
        throw std::runtime_error("Can't read " + PathText(from));
    const uint64_t total = fs::file_size(from);
    uint64_t handle = 0;
    CheckAfc(afc_file_open(afc, path.c_str(), AFC_FOPEN_WRONLY, &handle), "Can't create " + path);
    std::vector<char> buffer(1 << 20);
    uint64_t sent = 0;
    afc_error_t error = AFC_E_SUCCESS;
    while (error == AFC_E_SUCCESS && (in.read(buffer.data(), std::streamsize(buffer.size())) || in.gcount() > 0)) {
        uint32_t written = 0;
        error = afc_file_write(afc, handle, buffer.data(), uint32_t(in.gcount()), &written);
        sent += written;
        if (progress)
            progress(sent, total);
    }
    afc_file_close(afc, handle);
    CheckAfc(error, "Can't write " + path);
}

void Services::UploadFile(const fs::path &from, const std::string &path,
                          const std::function<void(uint64_t, uint64_t)> &progress) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Upload(Afc(), from, path, progress);
}

void Services::DeleteTree(afc_client_t afc, const std::string &path) {
    // AFC only removes empty folders on older iOS, so empty them first.
    char **info = nullptr;
    bool folder = false;
    if (afc_get_file_info(afc, path.c_str(), &info) == AFC_E_SUCCESS && info) {
        for (int k = 0; info[k] && info[k + 1]; k += 2)
            folder |= std::string(info[k]) == "st_ifmt" && std::string(info[k + 1]) == "S_IFDIR";
        afc_dictionary_free(info);
    }
    if (folder) {
        char **names = nullptr;
        if (afc_read_directory(afc, path.c_str(), &names) == AFC_E_SUCCESS && names) {
            for (int i = 0; names[i]; i++)
                if (std::string(names[i]) != "." && std::string(names[i]) != "..")
                    DeleteTree(afc, Join(path, names[i]));
            afc_dictionary_free(names);
        }
    }
    CheckAfc(afc_remove_path(afc, path.c_str()), "Can't delete " + path);
}

void Services::DeletePath(const std::string &path) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (path.empty() || path == "/")
        throw std::runtime_error("Refusing to delete the whole filesystem");
    DeleteTree(Afc(), path);
}

void Services::MakeFolder(const std::string &path) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    CheckAfc(afc_make_directory(Afc(), path.c_str()), "Can't create " + path);
}

void Services::RenamePath(const std::string &from, const std::string &to) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    CheckAfc(afc_rename_path(Afc(), from.c_str(), to.c_str()), "Can't rename " + from);
}

// ---- Apps ------------------------------------------------------------------------------------

// An installation_proxy client for one operation.
struct InstallProxy {
    instproxy_client_t client = nullptr;
    explicit InstallProxy(idevice_t device) {
        if (instproxy_client_start_service(device, &client, kLabel) != INSTPROXY_E_SUCCESS)
            throw std::runtime_error("iOS refused the installation service");
    }
    ~InstallProxy() {
        if (client)
            instproxy_client_free(client);
    }
};

Json Services::ListApps() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    InstallProxy proxy(Device());
    plist_t options = instproxy_client_options_new();
    instproxy_client_options_add(options, "ApplicationType", "Any", nullptr);
    plist_t apps = nullptr;
    instproxy_error_t error = instproxy_browse(proxy.client, options, &apps);
    instproxy_client_options_free(options);
    if (error != INSTPROXY_E_SUCCESS)
        throw std::runtime_error("Couldn't list the installed apps");
    Json list = Json::array();
    for (const auto &app : PlistToJson(apps)) {
        // Some system apps only carry a device-specific name ("Contacts~iphone") or none at all.
        std::string name = app.value("CFBundleDisplayName", app.value("CFBundleName", ""));
        name = name.substr(0, name.find('~'));
        if (name.empty())
            name = app.value("CFBundleIdentifier", "");
        list.push_back({{"bundle", app.value("CFBundleIdentifier", "")},
                        {"name", name},
                        {"version", app.value("CFBundleShortVersionString", app.value("CFBundleVersion", ""))},
                        {"type", app.value("ApplicationType", "")},
                        {"path", app.value("Path", "")}});
    }
    plist_free(apps);
    std::sort(list.begin(), list.end(), [](const Json &a, const Json &b) {
        return a["type"] != b["type"] ? a["type"] == "User" : a["name"] < b["name"];
    });
    return list;
}

// The free space on the volume an AFC client sees; 0 if iOS doesn't say.
static uint64_t FreeBytes(afc_client_t afc) {
    char *value = nullptr;
    if (afc_get_device_info_key(afc, "FSFreeBytes", &value) != AFC_E_SUCCESS)
        return 0;
    return strtoull(TakeString(value).c_str(), nullptr, 10);
}

void Services::InstallApp(const fs::path &ipa, uint64_t unpackedBytes,
                          const std::function<void(const std::string &)> &progress) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    // 1. Upload it to the Media folder's PublicStaging, where installd picks packages up.
    Lockdown lockdown(Device());
    lockdownd_service_descriptor_t service = lockdown.Start("com.apple.afc");
    if (!service)
        throw std::runtime_error("iOS refused the file service");
    afc_client_t media = nullptr;
    afc_error_t afcError = afc_client_new(Device(), service, &media);
    lockdownd_service_descriptor_free(service);
    CheckAfc(afcError, "Couldn't open the file service");
    const std::string staged = "PublicStaging/" + RandomHex(8) + ".ipa";
    try {
        afc_make_directory(media, "PublicStaging");
        Upload(media, ipa, staged, [&](uint64_t sent, uint64_t total) {
            progress("Copying to the device: " + FormatBytes(sent) + " of " + FormatBytes(total));
        });
    } catch (...) {
        afc_client_free(media);
        throw;
    }

    // 2. Ask installd to install it, following its progress until it finishes.
    struct Outcome {
        std::mutex mutex;
        std::condition_variable changed;
        bool done = false;
        std::string error, status;
        int percent = 0;
    } outcome;
    auto callback = [](plist_t, plist_t status, void *user) {
        auto *o = static_cast<Outcome *>(user);
        char *name = nullptr, *errorName = nullptr, *description = nullptr;
        uint64_t code = 0;
        int percent = -1;
        instproxy_status_get_name(status, &name);
        instproxy_status_get_percent_complete(status, &percent);
        const bool failed = instproxy_status_get_error(status, &errorName, &description, &code) != INSTPROXY_E_SUCCESS;
        std::lock_guard<std::mutex> lock(o->mutex);
        o->status = TakeString(name);
        if (percent >= 0)
            o->percent = percent;
        std::string errorText = TakeString(errorName), detail = TakeString(description);
        if (failed) {
            o->error = errorText + (detail.empty() ? "" : ": " + detail);
            o->done = true;
        } else if (o->status == "Complete") {
            o->done = true;
        }
        o->changed.notify_one();
    };
    const uint64_t freeAtStart = FreeBytes(media);
    uint64_t unpacked = 0;
    InstallProxy proxy(Device());
    if (instproxy_install(proxy.client, staged.c_str(), nullptr, callback, &outcome) != INSTPROXY_E_SUCCESS) {
        afc_remove_path(media, staged.c_str());
        afc_client_free(media);
        throw std::runtime_error("iOS refused the install");
    }
    {
        std::unique_lock<std::mutex> lock(outcome.mutex);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
        while (!outcome.done) {
            if (std::chrono::steady_clock::now() > deadline) {
                outcome.error = "it didn't finish within 10 minutes";
                break;
            }
            outcome.changed.wait_for(lock, std::chrono::seconds(1));
            if (outcome.status == "ExtractingPackage" && unpackedBytes && freeAtStart) {
                // installd stays silent while it unpacks, the longest step, but the free space falls.
                lock.unlock();   // the status callback mustn't wait for iOS's answer
                const uint64_t freeNow = FreeBytes(media);
                if (freeNow)
                    unpacked = std::min(unpackedBytes, freeAtStart - std::min(freeAtStart, freeNow));
                progress("Unpacking: " + FormatBytes(unpacked) + " of " + FormatBytes(unpackedBytes));
                lock.lock();
            } else {
                progress("Installing: " + outcome.status + " (" + std::to_string(outcome.percent) + "%)");
            }
        }
    }
    afc_remove_path(media, staged.c_str());
    afc_client_free(media);
    if (!outcome.error.empty())
        throw std::runtime_error("The install failed: " + outcome.error);
}

// ---- System log ------------------------------------------------------------------------------

void Services::StartSyslog(std::function<void(const std::string &)> onLine) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (syslog_)
        return;
    onSyslogLine_ = std::move(onLine);
    if (syslog_relay_client_start_service(Device(), &syslog_, kLabel) != SYSLOG_RELAY_E_SUCCESS) {
        syslog_ = nullptr;
        throw std::runtime_error("iOS refused the system log service");
    }
    // The relay hands over one character at a time; old iOS separates messages with NULs.
    auto received = [](char c, void *user) {
        auto *self = static_cast<Services *>(user);
        if (c == '\n' || c == '\0') {
            if (!self->syslogLine_.empty())
                self->onSyslogLine_(self->syslogLine_);
            self->syslogLine_.clear();
        } else {
            self->syslogLine_ += c;
        }
    };
    syslog_relay_start_capture(syslog_, received, this);
}

void Services::StopSyslog() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!syslog_)
        return;
    syslog_relay_stop_capture(syslog_);
    syslog_relay_client_free(syslog_);
    syslog_ = nullptr;
}

// ---- Port forwarding (SSH) -------------------------------------------------------------------

void Services::StartForward(uint16_t localPort, uint16_t devicePort) {
    if (Forwarding())
        return;
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    Udid();   // fails early, with a clear message, if the device isn't connected yet
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(localPort);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // only this computer can connect
    if (listener == INVALID_SOCKET || bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof address) != 0 ||
        listen(listener, 8) != 0) {
        closesocket(listener);
        throw std::runtime_error("Port " + std::to_string(localPort) + " is already in use on this computer");
    }
    listener_ = uintptr_t(listener);
    forwardThread_ = std::thread(&Services::ForwardLoop, this, devicePort);
}

void Services::StopForward() {
    if (!Forwarding())
        return;
    closesocket(SOCKET(listener_));   // ends accept() in the loop
    listener_ = ~uintptr_t(0);
    if (forwardThread_.joinable())
        forwardThread_.join();
}

// Copies bytes both ways between two connected sockets until either side closes.
static void Pump(SOCKET a, SOCKET b) {
    std::vector<char> buffer(1 << 16);
    for (;;) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(a, &readable);
        FD_SET(b, &readable);
        if (select(0, &readable, nullptr, nullptr, nullptr) <= 0)
            break;
        bool open = true;
        for (auto [from, to] : {std::pair{a, b}, std::pair{b, a}}) {
            if (!FD_ISSET(from, &readable))
                continue;
            int n = recv(from, buffer.data(), int(buffer.size()), 0);
            for (int sent = 0; n > 0 && sent < n;) {
                int m = send(to, buffer.data() + sent, n - sent, 0);
                if (m <= 0) {
                    n = -1;
                    break;
                }
                sent += m;
            }
            open = open && n > 0;
        }
        if (!open)
            break;
    }
}

// Accepts local connections; each gets its own tunnel to the device port. usbmuxd's Connect
// turns its socket into a plain byte pipe to that port, so after it the two sockets just pump.
void Services::ForwardLoop(uint16_t devicePort) {
    const std::string udid = Udid();
    const SOCKET listener = SOCKET(listener_);
    for (;;) {
        SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET)
            return;   // StopForward closed the listener
        usbmuxd_device_info_t info{};
        int tunnel = -1;
        if (usbmuxd_get_device_by_udid(udid.c_str(), &info) > 0)
            tunnel = usbmuxd_connect(info.handle, devicePort);
        if (tunnel < 0) {
            Log("Port forward: the device refused port %u (for SSH: is OpenSSH installed from Cydia?)", devicePort);
            closesocket(client);
            continue;
        }
        std::thread([client, tunnel] {
            Pump(client, SOCKET(tunnel));
            closesocket(client);
            usbmuxd_disconnect(tunnel);
        }).detach();
    }
}
