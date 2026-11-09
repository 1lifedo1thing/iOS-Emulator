#include "library.h"

#include <algorithm>

fs::path LibraryDir() { return DataDir() / "library"; }

std::string ProductName(const std::string &board) {
    static const std::map<std::string, std::string> names = {
        {"n45ap", "iPod touch 1G"}, {"m68ap", "iPhone (original)"},    {"n72ap", "iPod touch 2G"},
        {"n18ap", "iPod touch 3G"}, {"n81ap", "iPod touch 4G"}, {"n88ap", "iPhone 3GS"},
        {"n90ap", "iPhone 4"},      {"k48ap", "iPad"},          {"n94ap", "iPhone 4S"},
    };
    auto found = names.find(board);
    return found != names.end() ? found->second : board;
}

Json Device::Lock() const { return Json::parse(LoadFile(Base() / "device.lock.json")); }

std::string Device::Product() const { return ProductName(board); }

std::string Device::Version() const {
    try {
        return Lock().value("product_version", build);
    } catch (...) {
        return build;
    }
}

bool Device::Jailbroken() const {
    // firmwarekit's --jailbreak turns on the "jailbreak" option of the recipe it records.
    try {
        Json options = Lock()["entry"]["content"]["recipe"]["options"];
        return options.is_object() && options.value("jailbreak", false);
    } catch (...) {
        return false;
    }
}

bool Device::Dirty() const { return fs::exists(State() / ".dirty"); }

Device OpenDevice(const fs::path &dir) {
    Json record = Json::parse(LoadFile(dir / "record.json"));
    Device device;
    device.id = record.at("id").get<std::string>();
    device.name = record.value("name", device.id);
    device.board = record.at("board").get<std::string>();
    device.build = record.at("build").get<std::string>();
    device.dir = dir;
    return device;
}

static void SaveRecord(const Device &device) {
    Json record = {{"schema", 1},           {"id", device.id},       {"name", device.name},
                   {"board", device.board}, {"build", device.build}, {"base_sha256", Sha256File(device.Base() / "device.lock.json")}};
    SaveFile(device.dir / "record.json", record.dump());
}

std::vector<Device> ListDevices() {
    std::vector<Device> devices;
    fs::path records = LibraryDir() / "records";
    if (!fs::exists(records))
        return devices;
    for (const auto &entry : fs::directory_iterator(records)) {
        if (!entry.is_directory() || !fs::exists(entry.path() / "record.json"))
            continue;
        try {
            devices.push_back(OpenDevice(entry.path()));
        } catch (const std::exception &e) {
            Log("Skipping device folder %s: %s", PathText(entry.path()).c_str(), e.what());
        }
    }
    std::sort(devices.begin(), devices.end(), [](const Device &a, const Device &b) { return a.name < b.name; });
    return devices;
}

Device FindDevice(const std::string &idOrName) {
    std::vector<Device> matches;
    for (const auto &device : ListDevices())
        if (device.name == idOrName || device.id.rfind(idOrName, 0) == 0)
            matches.push_back(device);
    if (matches.size() != 1)
        throw std::runtime_error(matches.empty() ? "No device called \"" + idOrName + "\""
                                                 : "\"" + idOrName + "\" matches more than one device");
    return matches[0];
}

Device AddPreparedDevice(const fs::path &prepared, const std::string &name, const fs::path &bootrom) {
    Device device;
    device.id = RandomHex(16);
    device.dir = LibraryDir() / "records" / device.id;
    Json lock = Json::parse(LoadFile(prepared / "device.lock.json"));
    device.board = lock.at("board").get<std::string>();
    device.build = lock.at("build").get<std::string>();
    device.name = name.empty() ? ProductName(device.board) : name;

    fs::create_directories(device.dir);
    fs::remove_all(prepared / "work");   // firmwarekit's scratch space, if it kept any
    fs::rename(prepared, device.Base());
    fs::create_directories(device.State());
    if (!bootrom.empty()) {
        fs::create_directories(device.Assets());
        fs::copy_file(bootrom, device.Assets() / "bootrom.bin", fs::copy_options::overwrite_existing);
    }
    SaveRecord(device);
    return device;
}

Device ImportDevice(const fs::path &recordDir) {
    Device source = OpenDevice(recordDir);
    const fs::path target = LibraryDir() / "records" / source.id;
    if (fs::exists(target))
        throw std::runtime_error("The library already has this device (" + source.id + ")");
    const fs::path staging = LibraryDir() / "records" / (source.id + ".importing");
    fs::remove_all(staging);
    fs::create_directories(staging);
    for (auto it = fs::recursive_directory_iterator(recordDir); it != fs::recursive_directory_iterator(); ++it) {
        const fs::directory_entry &entry = *it;
        const fs::path relative = fs::relative(entry.path(), recordDir);
        if (relative == fs::path("state") / "work") {   // LightTouch's temporary files
            it.disable_recursion_pending();
            continue;
        }
        const fs::path destination = staging / relative;
        if (entry.is_directory()) {
            fs::create_directories(destination);
            continue;
        }
        fs::create_directories(destination.parent_path());
        std::error_code linked;
        if (*relative.begin() == "base")   // never written, so both libraries can share the same file
            fs::create_hard_link(entry.path(), destination, linked);
        if (*relative.begin() != "base" || linked)
            fs::copy_file(entry.path(), destination);
    }
    fs::rename(staging, target);

    // LightTouch keeps the device's USB pairing record in <data>\pairing\<id>, beside "library".
    // Reusing it means iOS doesn't have to trust this computer again (it can't while locked).
    const fs::path pairing = recordDir.parent_path().parent_path().parent_path() / "pairing" / source.id;
    const fs::path ourPairing = DataDir() / "pairing" / source.id;
    if (fs::exists(pairing) && !fs::exists(ourPairing))
        fs::copy(pairing, ourPairing, fs::copy_options::recursive);
    return OpenDevice(target);
}

void RenameDevice(Device &device, const std::string &name) {
    if (name.empty())
        return;
    device.name = name;
    SaveRecord(device);
}

void EraseDevice(const Device &device) {
    // The state folder holds every change the device made; the base stays as prepared.
    fs::remove_all(device.State());
    fs::create_directories(device.State());
    fs::remove_all(device.dir / "snapshots");
    fs::remove(device.LoadMarker());
}

void DeleteDevice(const Device &device) { fs::remove_all(device.dir); }
