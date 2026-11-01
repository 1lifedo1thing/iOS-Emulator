// Typing into iOS by tapping the keys of its on-screen keyboard, as a finger would.
//
// None of the emulated boards gives iOS a keyboard it can use on every version, so this is how the
// emulator types everywhere: iPhone, iPod touch and iPad, iPhone OS 2 to iOS 7, any orientation,
// any scale. Whenever it needs to, it reads the keyboard off the screen:
//   - where it is: the four rows of keys along the bottom of the interface, whichever edge of the
//     panel that is, and where each key sits in its row;
//   - which page it shows: letters, or numbers and symbols (told apart by how many keys a row has);
//   - whether Shift is on (iOS turns it on by itself at the start of a sentence).
// Then it taps what a character needs: the page key, Shift, and the key itself.
//
// A text field must have focus, so that the keyboard shows. Characters a US keyboard has no key
// for (` and anything outside ASCII) are skipped. iOS may auto-correct the words, as it would a
// person's typing.
#pragma once

#include "common.h"
#include "engine.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

class OnScreenKeyboard {
public:
    // Touches the screen with one finger: x and y from 0 to 1 across the panel, phase as the
    // engine's (QEMU_IOS_TOUCH_BEGIN, _END). Returns the engine's id for the touch batch.
    using Touch = std::function<uint64_t(double x, double y, int phase)>;
    // Tells the user what couldn't be typed, and why.
    using Problem = std::function<void(const std::string &message)>;

    OnScreenKeyboard(Engine &engine, Touch touch, Problem problem);
    ~OnScreenKeyboard();

    // Queues text (UTF-8) to type. '\n' taps Return and '\b' taps Delete (a backspace).
    void Type(const std::string &text);

    // What it reads off a screen frame (BGRA): where the keyboard is, its keys, page and Shift.
    // For tests ("iOS-Emulator --keyboard shot.png").
    static std::string Describe(const uint8_t *bgra, int width, int height, int stride);

    struct Layout;   // a keyboard read off the screen (keyboard.cpp)

private:
    static bool Read(Layout &layout, const uint8_t *bgra, int width, int height, int stride);
    bool Stopping();
    void Run();
    void TypeText(const std::string &text);
    bool ReadKeyboard(Layout &layout);
    void Tap(const Layout &layout, int row, int key);

    Engine &engine_;
    Touch touch_;
    Problem problem_;

    std::vector<uint8_t> pixels_;   // the newest screen frame (BGRA)
    int width_ = 0, height_ = 0, stride_ = 0;
    uint64_t serial_ = 0;
    uint64_t lastTouch_ = 0;        // the engine's id for the last touch batch sent

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::string> queue_;
    bool stop_ = false;
    std::thread thread_;   // declared last: it starts once everything above is set up
};
