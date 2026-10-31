#include "engine.h"

// ---- Loading ---------------------------------------------------------------------------------

Engine::Engine(const fs::path &dll) {
    // LOAD_WITH_ALTERED_SEARCH_PATH: the engine's own DLLs (SDL2, glib, ...) load from its folder.
    HMODULE module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module)
        throw std::runtime_error("Couldn't load the engine " + PathText(dll) + " (error " +
                                 std::to_string(GetLastError()) + ")");
    // QEMU starts threads as soon as it loads, so the DLL must never be unloaded.
    HMODULE pinned;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCWSTR>(module), &pinned);

    auto find = [&](auto &function, const char *name) {
        function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(GetProcAddress(module, name));
        if (!function)
            throw std::runtime_error(std::string("The engine has no ") + name);
    };
    decltype(&qemu_ios_api_version) apiVersion;
    decltype(&qemu_ios_portable_version) portableVersion;
    find(apiVersion, "qemu_ios_api_version");
    find(portableVersion, "qemu_ios_portable_version");
    if (apiVersion() >> 16 != 2 || portableVersion() != QEMU_IOS_PORTABLE_VERSION)
        throw std::runtime_error("This engine's API version isn't supported");

    find(main, "qemu_ios_main");
    find(boardAt, "qemu_ios_portable_board_at");
    find(attach, "qemu_ios_ui_attach");
    find(frameCopy, "qemu_ios_portable_frame_copy");
    find(touch, "qemu_ios_portable_touch");
    find(inputStatus, "qemu_ios_portable_input_status");
    find(motion, "qemu_ios_portable_motion");
    find(cancelInput, "qemu_ios_portable_cancel_input");
    find(button, "qemu_ios_ui_button");
    find(ready, "qemu_ios_ui_ready");
    find(paused, "qemu_ios_ui_paused");
    find(pause, "qemu_ios_ui_pause");
    find(resume, "qemu_ios_ui_resume");
    find(displaySleeping, "qemu_ios_ui_display_sleeping");
    find(storageFailed, "qemu_ios_ui_storage_failed");
    find(shutdown, "qemu_ios_ui_shutdown");
    find(shutdownConfirmed, "qemu_ios_ui_guest_shutdown_confirmed");
    find(quit, "qemu_ios_ui_quit");
    find(netRestrict, "qemu_ios_ui_net_restrict");
    find(agentRequest, "qemu_ios_agent_request");
    find(agentResult, "qemu_ios_agent_result");
    find(agentFreeResult, "qemu_ios_agent_free_result");
    find(agentStatus, "qemu_ios_agent_status");
    find(audioStart, "qemu_ios_audio_capture_start");
    find(audioRead, "qemu_ios_audio_capture_read");
    find(audioStop, "qemu_ios_audio_capture_stop");
}

QemuIosPortableBoard Engine::FindBoard(const std::string &board) const {
    for (int index = 0;; index++) {
        QemuIosPortableBoard facts{};
        facts.size = sizeof facts;
        facts.version = 1;
        if (boardAt(index, &facts) != QEMU_IOS_OK)
            break;
        if (board == facts.board || board == facts.machine)
            return facts;
    }
    throw std::runtime_error("The engine can't run the board " + board);
}

// ---- Boot command line -----------------------------------------------------------------------

// The three ways the supported boards start (ported from LightTouch's Portable/src/Storage.cpp):
//   S5L8900 (iPod touch 1G, original iPhone) and S5L8720 (iPod touch 2G) start from their
//   SecureROM (the boot ROM file); the A4-era boards start from files firmwarekit prepared.
enum class Family { S5L8900, S5L8720, A4 };

static Family BoardFamily(const std::string &board) {
    if (board == "n45ap" || board == "m68ap")
        return Family::S5L8900;
    if (board == "n72ap")
        return Family::S5L8720;
    if (board == "n18ap" || board == "n81ap" || board == "n88ap" || board == "n90ap" || board == "k48ap")
        return Family::A4;
    throw std::runtime_error("No boot route for the board " + board);
}

bool NeedsBootrom(const std::string &board) { return BoardFamily(board) != Family::A4; }

std::vector<std::string> BuildBootArgs(const Device &device, const BootOptions &options) {
    const Json lock = device.Lock();
    const Family family = BoardFamily(device.board);
    const fs::path base = device.Base(), state = device.State();
    const std::string strategy = lock.value("boot_strategy", family == Family::A4 ? "kboot" : "iboot");
    const bool modem = device.board == "n88ap" || device.board == "n90ap";   // the iPhone 3GS and 4

    // The device writes into state\: a NAND overlay over the base image, and its own NOR copy.
    fs::create_directories(state / "nand");
    if (fs::exists(base / "nor.bin") && !fs::exists(state / "nor.bin"))
        fs::copy_file(base / "nor.bin", state / "nor.bin");
    const fs::path bootrom = device.Assets() / "bootrom.bin";
    if (family != Family::A4 && !fs::exists(bootrom))
        throw std::runtime_error("This device needs its boot ROM (assets\\bootrom.bin)");

    // -M takes "machine,key=value,...": a comma inside a value is written twice.
    std::string machine = options.machine;
    auto option = [&](const std::string &key, const std::string &value) {
        std::string escaped;
        for (char c : value)
            escaped += c == ',' ? std::string(",,") : std::string(1, c);
        machine += "," + key + "=" + escaped;
    };
    auto file = [](const fs::path &path) { return PathText(path); };

    if (family == Family::S5L8720) {
        option("boot-args", "amfi_allow_any_signature=1 cs_enforcement_disable=1");
        option("direct-iboot", strategy == "iboot" ? file(base / "iBoot.bin") : "");
        option("direct-llb", "");
        option("bootrom", file(bootrom));
        option("nor", file(base / "nor.bin"));
        option("nandrw", file(state / "nand"));
        option("gid-blobs", file(base / "gid-blobs.bin"));
        option("wifi", "on");
    } else if (family == Family::S5L8900) {
        option("bootrom", file(bootrom));
        option("iboot", file(base / "iBoot.bin"));
        option("nand-overlay", file(state / "nand"));
    } else {
        if (strategy == "kboot") {
            option("kboot", file(base / "kboot.bin"));
        } else {
            option("iboot", file(base / "iBoot.bin"));
            option("gid-blobs", file(base / "gid-blobs.bin"));
        }
        option("nand-overlay", file(state / "nand"));
        // The chip's die ID, which iOS's data protection keys depend on.
        if (fs::exists(base / "identity.json")) {
            Json identity = Json::parse(LoadFile(base / "identity.json"));
            if (identity.contains("die-id")) {
                std::string words;
                for (const auto &word : identity["die-id"])
                    words += (words.empty() ? "" : ":") + word.get<std::string>();
                option("die-id", words);
            }
        }
        if (modem)
            option("baseband", "on");
    }
    option("nand", file(base / "nand"));
    if (fs::exists(base / "nor.bin") && family != Family::S5L8900)
        option("nor-rw", file(state / "nor.bin"));

    // Per-unit values firmwarekit recorded (MAC addresses, ECID, ...). Only these are accepted.
    static const char *allowed[] = {"wifi-mac", "bt-mac", "ecid", "aes-uid", "rtc-epoch", "imei"};
    if (lock.contains("machine") && lock["machine"].is_object()) {
        for (const auto &[key, value] : lock["machine"].items()) {
            if (std::find(std::begin(allowed), std::end(allowed), key) == std::end(allowed))
                throw std::runtime_error("Unsupported machine property in the device lock: " + key);
            if (value.is_string() && value.get<std::string>().empty())
                continue;   // no value for this unit: the machine's default
            option(key, value.is_boolean() ? (value.get<bool>() ? "on" : "off")
                        : value.is_string() ? value.get<std::string>()
                                            : value.dump());
        }
    }
    if (options.headphones)
        option("headphones", "on");

    std::vector<std::string> args = {"iOS-Emulator", "-M", machine};
    // Wi-Fi through QEMU's user-mode network. restrict=on blocks all traffic; the UI's internet
    // switch flips it live (qemu_ios_ui_net_restrict). lan=off keeps the guest off your LAN.
    args.insert(args.end(), {"-netdev", std::string("user,id=wifi0,lan=off,restrict=") + (options.internet ? "off" : "on")});
    // Faster code translation: iOS never rewrites code pages in place, so jumps can be chained.
    args.insert(args.end(), {"-accel", "tcg,cross-page-chain=on,indirect-cache=on"});
    if (family == Family::S5L8720)
        args.insert(args.end(), {"-m", "128M"});
    if (family == Family::S5L8900)
        args.insert(args.end(), {"-drive", "if=pflash,format=raw,file=" + file(state / "nor.bin")});
    if (modem)
        args.insert(args.end(), {"-global", "ios-baseband.sim-file=" + file(state / "sim")});
    // No QEMU window or sound card: frames and sound come back through the C API.
    args.insert(args.end(), {"-display", "none", "-no-shutdown", "-audio", "driver=none", "-serial",
                             "file:" + file(state / "serial.log")});
    return args;
}
