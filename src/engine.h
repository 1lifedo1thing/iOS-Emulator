// The emulator engine: libqemu-arm.dll, a build of qemu-ios with a plain C API (qemu_ios_*).
//
// The engine runs one guest per process and can't be started twice or unloaded, which is why
// every running device gets its own worker process (see worker.h).
#pragma once

#include "common.h"
#include "library.h"

#include "../third_party/qemu-ios/qemu-ios-ui.h"
#include "../third_party/qemu-ios/qemu-macos-extras.h"

// The engine's functions we use, looked up by name once the DLL is loaded.
struct Engine {
    explicit Engine(const fs::path &dll);   // throws if it can't load or the API version differs

    // The engine's facts about a board (machine name, screen size, capabilities).
    QemuIosPortableBoard FindBoard(const std::string &board) const;

    decltype(&qemu_ios_main) main = nullptr;
    decltype(&qemu_ios_portable_board_at) boardAt = nullptr;
    decltype(&qemu_ios_device_info) deviceInfo = nullptr;
    decltype(&qemu_ios_ui_hardware_keyboard) hardwareKeyboard = nullptr;
    decltype(&qemu_ios_ui_key_mac) keyMac = nullptr;
    decltype(&qemu_ios_ui_attach) attach = nullptr;
    decltype(&qemu_ios_portable_frame_copy) frameCopy = nullptr;
    decltype(&qemu_ios_portable_touch) touch = nullptr;
    decltype(&qemu_ios_portable_input_status) inputStatus = nullptr;
    decltype(&qemu_ios_portable_motion) motion = nullptr;
    decltype(&qemu_ios_portable_cancel_input) cancelInput = nullptr;
    decltype(&qemu_ios_ui_button) button = nullptr;
    decltype(&qemu_ios_ui_ready) ready = nullptr;
    decltype(&qemu_ios_ui_paused) paused = nullptr;
    decltype(&qemu_ios_ui_pause) pause = nullptr;
    decltype(&qemu_ios_ui_resume) resume = nullptr;
    decltype(&qemu_ios_ui_display_sleeping) displaySleeping = nullptr;
    decltype(&qemu_ios_ui_storage_failed) storageFailed = nullptr;
    decltype(&qemu_ios_ui_shutdown) shutdown = nullptr;
    decltype(&qemu_ios_ui_guest_shutdown_confirmed) shutdownConfirmed = nullptr;
    decltype(&qemu_ios_ui_quit) quit = nullptr;
    decltype(&qemu_ios_ui_net_restrict) netRestrict = nullptr;
    decltype(&qemu_ios_agent_request) agentRequest = nullptr;
    decltype(&qemu_ios_agent_result) agentResult = nullptr;
    decltype(&qemu_ios_agent_free_result) agentFreeResult = nullptr;
    decltype(&qemu_ios_agent_status) agentStatus = nullptr;
    decltype(&qemu_ios_audio_capture_start) audioStart = nullptr;
    decltype(&qemu_ios_audio_capture_read) audioRead = nullptr;
    decltype(&qemu_ios_audio_capture_stop) audioStop = nullptr;
};

struct BootOptions {
    std::string machine;     // the engine's -M name for the board (Engine::FindBoard)
    bool internet = false;   // let the guest reach the internet (NAT); off blocks everything
    bool headphones = false; // route sound to the headphone jack (stereo, no speaker EQ)
    fs::path proxySocket;    // the web proxy's Unix socket (proxy.h); empty: no proxy forward
};

// Prepares the device's state folder for a boot (creates the NAND overlay folder, copies the
// NOR image on first boot) and returns the engine's command line, argv[0] included.
std::vector<std::string> BuildBootArgs(const Device &device, const BootOptions &options);

// Boards that boot from their SecureROM need the boot ROM file; later boards don't.
bool NeedsBootrom(const std::string &board);
