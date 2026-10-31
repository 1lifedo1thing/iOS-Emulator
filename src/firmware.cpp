#include "firmware.h"

#include "engine.h"

#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <fstream>

#pragma comment(lib, "winhttp.lib")

// ---- Catalog ---------------------------------------------------------------------------------

bool Firmware::Tested() const {
    // The firmwares that pass LightTouch's compatibility runs (its docs/COMPATIBILITY.md).
    static const char *tested[] = {"n72ap-7E18",   "n72ap-8C148", "n18ap-8C148", "n81ap-10B500",
                                   "n88ap-9B206", "n90ap-8C148", "n90ap-8L1"};
    return std::find(std::begin(tested), std::end(tested), id) != std::end(tested);
}

std::string Firmware::JailbreakUnavailable() const {
    // firmwarekit refuses the same firmwares.
    if (std::atoi(version.c_str()) < 2)
        return "Needs iPhone OS 2.0 or later.";
    if (board == "n72ap")   // Substrate's launchd hook hangs every boot after the first (LightTouch's TODO)
        return "Not on the iPod touch 2G yet: it wouldn't start up again after its first restart.";
    return "";
}

const std::vector<Firmware> &Catalog() {
    static const std::vector<Firmware> catalog = [] {
        std::vector<Firmware> list;
        Json json = Json::parse(LoadFile(ResourcesDir() / "firmware-catalog.json"));
        for (const auto &entry : json.at("entries")) {
            if (entry.value("status", "") == "coming_soon" || !entry.contains("source"))
                continue;
            Firmware f;
            f.id = entry.at("id");
            f.board = entry.at("board");
            f.productType = entry.at("product_type");
            f.version = entry.at("version");
            f.label = f.version;
            if (std::string pre = entry.value("prerelease", ""); !pre.empty()) {
                f.label += " " + pre;
                if (int number = entry.value("prerelease_number", 0); number > 0)
                    f.label += " " + std::to_string(number);
            }
            f.build = entry.at("build");
            f.released = entry.value("released", "");
            f.status = entry.value("status", "");
            const Json &source = entry.at("source");
            f.sha1 = source.value("sha1", "");
            f.bytes = source.value("bytes", uint64_t(0));
            f.urls.push_back(source.at("url"));
            for (const auto &mirror : source.value("mirrors", Json::array()))
                f.urls.push_back(mirror.at("url"));
            std::string archived = "https://archive.org/download/iOS_Firmware_Exhibit_Collection/" + f.FileName();
            for (size_t comma; (comma = archived.find(',')) != std::string::npos;)
                archived.replace(comma, 1, "%2C");
            if (std::find(f.urls.begin(), f.urls.end(), archived) == f.urls.end())
                f.urls.push_back(archived);
            const Json estimates = entry.value("estimates", Json::object());
            f.preparedBytes = estimates.value("prepared_bytes", uint64_t(0));
            f.peakBytes = estimates.value("peak_bytes", uint64_t(0));
            f.sibling = entry.at("recipe").value("keybag_ramdisk_from", "");
            list.push_back(f);
        }
        // Devices in the order they came out, each one's versions oldest first.
        static const std::vector<std::string> order = {"iPod1,1",   "iPod2,1",   "iPod3,1",   "iPod4,1", "iPhone1,1",
                                                       "iPhone1,2", "iPhone2,1", "iPhone3,1", "iPad1,1"};
        auto rank = [](const Firmware &f) {
            return std::find(order.begin(), order.end(), f.productType) - order.begin();
        };
        std::stable_sort(list.begin(), list.end(), [&](const Firmware &a, const Firmware &b) {
            return rank(a) != rank(b) ? rank(a) < rank(b) : a.released < b.released;
        });
        return list;
    }();
    return catalog;
}

const Firmware *FindFirmware(const std::string &id) {
    for (const auto &firmware : Catalog())
        if (firmware.id == id)
            return &firmware;
    return nullptr;
}

// ---- Downloads -------------------------------------------------------------------------------

void Download(const std::string &url, const fs::path &file, const std::function<bool(uint64_t, uint64_t)> &progress) {
    std::wstring wideUrl = Widen(url);
    URL_COMPONENTS parts{sizeof parts};
    wchar_t host[256], path[2048];
    parts.lpszHostName = host;
    parts.dwHostNameLength = DWORD(std::size(host));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = DWORD(std::size(path));
    if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts))
        throw std::runtime_error("Bad download address " + url);

    // Small RAII holder so every exit closes the WinHTTP handles.
    struct Handle {
        HINTERNET h;
        ~Handle() {
            if (h)
                WinHttpCloseHandle(h);
        }
    };
    Handle session{WinHttpOpen(L"iOS-Emulator/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.h)
        throw std::runtime_error("Couldn't start a download");
    WinHttpSetTimeouts(session.h, 0, 30000, 30000, 60000);
    Handle connection{WinHttpConnect(session.h, host, parts.nPort, 0)};
    Handle request{connection.h ? WinHttpOpenRequest(connection.h, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                     parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                                : nullptr};
    if (!request.h)
        throw std::runtime_error("Couldn't connect to " + Narrow(host));

    // Resume where an earlier attempt stopped.
    uint64_t have = fs::exists(file) ? fs::file_size(file) : 0;
    std::wstring headers = have ? L"Range: bytes=" + std::to_wstring(have) + L"-" : L"";
    if (!WinHttpSendRequest(request.h, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), DWORD(-1L),
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr))
        throw std::runtime_error("No answer from " + Narrow(host) + " (error " + std::to_string(GetLastError()) + ")");

    DWORD status = 0, size = sizeof status;
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &size,
                        nullptr);
    if (status == 416 && have > 0)
        return;   // nothing left to fetch
    if (status != 200 && status != 206)
        throw std::runtime_error(Narrow(host) + " answered HTTP " + std::to_string(status));
    if (status == 200)
        have = 0;   // the server ignored the range: start over
    wchar_t lengthText[32] = L"0";
    size = sizeof lengthText;
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_LENGTH, nullptr, lengthText, &size, nullptr);
    const uint64_t total = have + _wcstoui64(lengthText, nullptr, 10);

    std::ofstream out(file, std::ios::binary | (have ? std::ios::app : std::ios::trunc));
    if (!out)
        throw std::runtime_error("Can't write " + PathText(file));
    std::vector<char> buffer(256 << 10);
    for (;;) {
        DWORD read = 0;
        if (!WinHttpReadData(request.h, buffer.data(), DWORD(buffer.size()), &read))
            throw std::runtime_error("The download from " + Narrow(host) + " broke off");
        if (read == 0)
            break;
        if (!out.write(buffer.data(), read))
            throw std::runtime_error("Can't write " + PathText(file) + " (is the disk full?)");
        have += read;
        if (!progress(have, total))
            throw std::runtime_error("Cancelled");
    }
}

// SHA-1 (as the catalog lists IPSWs) of a file, with progress.
static std::string Sha1File(const fs::path &path, const std::function<bool(uint64_t, uint64_t)> &progress) {
    std::ifstream in(path, std::ios::binary);
    const uint64_t total = fs::file_size(path);
    BCRYPT_HASH_HANDLE hash = nullptr;
    BCryptCreateHash(BCRYPT_SHA1_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0);
    std::vector<char> buffer(4 << 20);
    uint64_t done = 0;
    while (in.read(buffer.data(), std::streamsize(buffer.size())) || in.gcount() > 0) {
        BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), ULONG(in.gcount()), 0);
        done += uint64_t(in.gcount());
        if (!progress(done, total)) {
            BCryptDestroyHash(hash);
            throw std::runtime_error("Cancelled");
        }
    }
    uint8_t digest[20];
    BCryptFinishHash(hash, digest, sizeof digest, 0);
    BCryptDestroyHash(hash);
    char hex[41];
    for (int i = 0; i < 20; i++)
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    return hex;
}

// ---- Preparation -----------------------------------------------------------------------------

Preparation::Preparation(PrepareRequest request) : request_(std::move(request)) {
    status_.step = "Starting";
    thread_ = std::thread(&Preparation::Run, this);
}

Preparation::~Preparation() {
    Cancel();
    if (thread_.joinable())
        thread_.join();
}

Preparation::Status Preparation::GetStatus() {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

void Preparation::Cancel() {
    cancelled_ = true;
    std::lock_guard<std::mutex> lock(mutex_);
    firmwarekit_.Kill();   // its own helper processes end with it
}

void Preparation::Report(const std::string &step, double fraction) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_.step = step;
    status_.fraction = std::clamp(fraction, 0.0, 1.0);
}

void Preparation::Run() {
    const fs::path out = LibraryDir() / ".prepared" / ("job-" + RandomHex(8));
    std::vector<fs::path> downloadedFiles;
    try {
        const Firmware &firmware = request_.firmware;
        // 1. The IPSW (and a second one, for firmwares that borrow its keybag ramdisk).
        const bool local = !request_.ipsw.empty();
        bool downloaded = false;
        fs::path ipsw = local ? request_.ipsw : GetIpsw(firmware, 0.0, 0.30, downloaded);
        if (downloaded)
            downloadedFiles.push_back(ipsw);
        fs::path sibling;
        if (!firmware.sibling.empty()) {
            const Firmware *other = FindFirmware(firmware.sibling);
            if (!other)
                throw std::runtime_error("The catalog lacks " + firmware.sibling + ", which this version needs");
            sibling = GetIpsw(*other, 0.30, 0.35, downloaded);
            if (downloaded)
                downloadedFiles.push_back(sibling);
        }
        // 2. The boot ROM, for boards that start from their SecureROM.
        fs::path bootrom = NeedsBootrom(firmware.board) ? GetBootrom() : fs::path();
        // 3. firmwarekit builds the device.
        RunFirmwarekit(ipsw, sibling, bootrom, out);
        // 4. Into the library it goes.
        Report("Adding the device", 0.98);
        Device device = AddPreparedDevice(out, request_.name, bootrom);
        for (const auto &file : downloadedFiles)   // like LightTouch: IPSWs are large and can be fetched again
            fs::remove(file);
        std::lock_guard<std::mutex> lock(mutex_);
        status_.step = "Done";
        status_.fraction = 1;
        status_.finished = true;
        status_.deviceId = device.id;
    } catch (const std::exception &e) {
        std::error_code ignored;
        fs::remove_all(out, ignored);
        Log("Preparation failed: %s", e.what());
        std::lock_guard<std::mutex> lock(mutex_);
        status_.finished = true;
        status_.failed = true;
        status_.error = cancelled_ ? "Cancelled." : e.what();
    }
}

fs::path Preparation::GetIpsw(const Firmware &firmware, double from, double to, bool &downloaded) {
    downloaded = false;
    const fs::path dir = DataDir() / "downloads";
    fs::create_directories(dir);
    const fs::path target = dir / firmware.FileName();
    const std::string title = firmware.Product() + " iOS " + firmware.label;
    auto checking = [&](uint64_t done, uint64_t total) {
        Report("Checking the " + title + " download", to - (to - from) * 0.1 + (to - from) * 0.1 * done / std::max<uint64_t>(total, 1));
        return !cancelled_.load();
    };
    if (fs::exists(target) && fs::file_size(target) == firmware.bytes) {
        if (Sha1File(target, checking) == firmware.sha1)
            return target;   // already here from an earlier attempt
        fs::remove(target);
    }
    const fs::path partial = dir / (firmware.FileName() + ".partial");
    std::string failure = "No download link worked";
    for (const auto &url : firmware.urls) {
        const std::string host = url.substr(url.find("//") + 2, url.find('/', url.find("//") + 2) - url.find("//") - 2);
        try {
            Download(url, partial, [&](uint64_t done, uint64_t total) {
                Report("Downloading " + title + " from " + host + ": " + FormatBytes(done) + " of " + FormatBytes(total),
                       from + (to - from) * 0.9 * done / std::max<uint64_t>(total, 1));
                return !cancelled_.load();
            });
        } catch (const std::exception &e) {
            if (cancelled_)
                throw;
            failure = host + ": " + e.what();
            Log("Download from %s failed: %s", host.c_str(), e.what());
            continue;
        }
        if (fs::file_size(partial) != firmware.bytes || Sha1File(partial, checking) != firmware.sha1) {
            fs::remove(partial);
            failure = host + ": the download didn't match Apple's file";
            continue;
        }
        fs::rename(partial, target);
        downloaded = true;
        return target;
    }
    throw std::runtime_error("Couldn't download the IPSW. " + failure);
}

// The SecureROM the oldest boards start from: mask ROM in the chip, the same for every iOS
// version and never part of an IPSW (LightTouch's crates/lighttouch-catalog/src/download.rs).
fs::path Preparation::GetBootrom() {
    const bool s5l8720 = request_.firmware.board == "n72ap";
    const std::string label = s5l8720 ? "iBoot-240.4" : "S5L8900";
    const std::string sha256 = s5l8720 ? "55f4d8ea2791ba51dd89934168f38f0fb21ce8762ff614c1e742407c0d3ca054"
                                       : "220027d6f8ea24603f8c5ad5e920ab553d85a9cd809d79c7e6f6f0eefdc2bc95";
    const std::vector<std::string> links =
        s5l8720 ? std::vector<std::string>{
                      "https://securerom.fun/resources/APROM/SecureROM%20for%20s5l8720xsi%2C%20iBoot-240.4",
                      "https://github.com/hekapooios/hekapooios.github.io/raw/master/resources/APROM/"
                      "SecureROM%20for%20s5l8720xsi%2C%20iBoot-240.4",
                      "https://github.com/devos50/qemu-ios/releases/download/n72ap_v1/bootrom_240_4"}
                : std::vector<std::string>{"https://github.com/devos50/qemu-ios/releases/download/n45ap_v1/bootrom_s5l8900"};
    const fs::path dir = DataDir() / "bootroms";
    fs::create_directories(dir);
    const fs::path target = dir / label;
    if (fs::exists(target) && Sha256File(target) == sha256)
        return target;
    Report("Downloading the " + label + " boot ROM", 0.35);
    for (const auto &link : links) {
        const fs::path partial = dir / (label + ".partial");
        fs::remove(partial);
        try {
            Download(link, partial, [&](uint64_t, uint64_t) { return !cancelled_.load(); });
        } catch (const std::exception &e) {
            if (cancelled_)
                throw;
            Log("Boot ROM download from %s failed: %s", link.c_str(), e.what());
            continue;
        }
        if (Sha256File(partial) == sha256) {
            fs::rename(partial, target);
            return target;
        }
        fs::remove(partial);
    }
    throw std::runtime_error("Couldn't download the " + label + " boot ROM.");
}

// firmwarekit create: one JSON object per line on stdout ("begin" with each step's expected
// seconds, "step", "progress", "warning", "done", "error"); diagnostics go to its log.
void Preparation::RunFirmwarekit(const fs::path &ipsw, const fs::path &sibling, const fs::path &bootrom,
                                 const fs::path &out) {
    if (cancelled_)
        throw std::runtime_error("Cancelled");
    fs::create_directories(out.parent_path());
    const fs::path cache = DataDir() / "cache" / "firmwarekit";
    fs::create_directories(cache);
    wchar_t locale[LOCALE_NAME_MAX_LENGTH] = L"en-US";
    GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH);

    LaunchOptions options;
    options.args = {L"create",    L"--catalog",     (ResourcesDir() / "firmware-catalog.json").wstring(),
                    L"--id",      Widen(request_.firmware.id),
                    L"--ipsw",    ipsw.wstring(),
                    L"--out",     out.wstring(),
                    L"--guest-tools", (ResourcesDir() / "guest-tools").wstring(),
                    L"--cache",   cache.wstring(),
                    L"--language", locale,
                    L"--skip-setup"};   // straight to the home screen, no setup assistant
    if (!bootrom.empty())
        options.args.insert(options.args.end(), {L"--bootrom", bootrom.wstring()});
    if (!sibling.empty())
        options.args.insert(options.args.end(), {L"--sibling-ipsw", sibling.wstring()});
    if (request_.jailbreak)
        options.args.push_back(L"--jailbreak");
    // firmwarekit boots the engine itself for its setup steps.
    options.env = {{"LTM_QEMU_DYLIB", PathText(ResourcesDir() / "libqemu-arm.dll")}, {"TMPDIR", PathText(cache)}};
    options.pipeStdout = true;
    options.log = DataDir() / "logs" / "firmwarekit.log";
    fs::create_directories(options.log.parent_path());
    fs::remove(options.log);
    {
        std::lock_guard<std::mutex> lock(mutex_);   // Cancel() may be killing it from the UI thread
        firmwarekit_ = ChildProcess::Launch(ResourcesDir() / "firmwarekit.exe", options);
        if (cancelled_)
            firmwarekit_.Kill();
    }

    const double from = 0.36, to = 0.97;
    std::vector<double> weights;   // each step's expected share of the time
    size_t step = 0;
    std::string stepName = "Starting", error;
    bool done = false;
    std::string line;
    while (firmwarekit_.ReadLine(line)) {
        Json event;
        try {
            event = Json::parse(line);
        } catch (...) {
            continue;
        }
        const std::string kind = event.value("event", "");
        if (kind == "begin") {
            for (const auto &seconds : event.value("seconds", Json::array()))
                weights.push_back(std::max(1.0, seconds.get<double>()));
        } else if (kind == "step" || kind == "progress") {
            if (kind == "step") {
                step = event.value("index", size_t(1));
                stepName = event.value("name", "Working");
            }
            double total = 0, before = 0;
            for (size_t i = 0; i < weights.size(); i++) {
                total += weights[i];
                if (i + 1 < step)
                    before += weights[i];
            }
            const double current = step >= 1 && step <= weights.size() ? weights[step - 1] : 0;
            const double fraction = total > 0 ? (before + current * event.value("fraction", 0.0)) / total : 0;
            Report(kind == "progress" ? event.value("detail", stepName) : stepName, from + (to - from) * fraction);
        } else if (kind == "warning") {
            Log("firmwarekit warning: %s", event.value("message", "").c_str());
        } else if (kind == "error") {
            error = event.value("message", "firmwarekit failed");
        } else if (kind == "done") {
            done = true;
        }
    }
    const int code = firmwarekit_.Wait(INFINITE);
    std::error_code ignored;
    fs::remove_all(cache, ignored);   // decrypted firmware: large, and not needed any more
    if (cancelled_)
        throw std::runtime_error("Cancelled");
    if (code != 0 || !done)
        throw std::runtime_error(!error.empty() ? error
                                                : "firmwarekit failed (exit code " + std::to_string(code) +
                                                      "); see " + PathText(options.log));
}
