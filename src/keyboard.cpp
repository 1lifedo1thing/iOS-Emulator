#include "keyboard.h"

#include <array>
#include <string_view>

using namespace std::chrono_literals;

// ---- What the keys type ----------------------------------------------------------------------

namespace {

enum Page { kLetters, kNumbers, kSymbols, kUnknown, kAnyPage };

// The characters of one row's keys, left to right, from key `first` on (the keys before and after
// them are Shift, Delete and the page keys). On the letters page, `shifted` is what they type with
// Shift on.
struct RowChars {
    int first;
    std::string_view plain, shifted;
};

// A keyboard design: how many keys its rows have, and what they type on each page. Rows count
// from the top, 0 to 3; rows 0 to 2 hold the characters, row 3 the page key and Space.
struct Design {
    size_t topRowKeys;     // tells the designs apart
    size_t letterRowKeys;  // keys in row 2 on the letters page (the other pages have two fewer)
    RowChars pages[3][3];  // [kLetters, kNumbers, kSymbols][row 0 to 2]
    int deleteRow;         // Delete is the last key of this row
    int returnRow;         // Return is the last key of this row
};

// The iPhone and iPod touch keyboard, iPhone OS 2 to iOS 7, upright or sideways. On iOS 4 and
// later, the symbols page has € £ ¥ • after these on its second row.
constexpr Design kPhone = {
    10,
    9,
    {{{0, "qwertyuiop", "QWERTYUIOP"}, {0, "asdfghjkl", "ASDFGHJKL"}, {1, "zxcvbnm", "ZXCVBNM"}},
     {{0, "1234567890", ""}, {0, "-/:;()$&@\"", ""}, {1, ".,?!'", ""}},
     {{0, "[]{}#%^*+=", ""}, {0, "_\\|~<>", ""}, {1, ".,?!'", ""}}},
    2,
    3,
};

// The iPad keyboard: Delete ends the top row and Return the second; the letters page has , and .
// (! and ? with Shift) after M, and a Shift key at each end of row 2. The other pages start row 2
// with #+= (or 123) and Undo (or Redo).
constexpr Design kPad = {
    11,
    11,
    {{{0, "qwertyuiop", "QWERTYUIOP"}, {0, "asdfghjkl", "ASDFGHJKL"}, {1, "zxcvbnm,.", "ZXCVBNM!?"}},
     {{0, "1234567890", ""}, {0, "-/:;()$&@", ""}, {2, ".,?!'\"", ""}},
     {{0, "[]{}#%^*+=", ""}, {0, "_\\|~<>", ""}, {2, ".,?!'\"", ""}}},
    0,
    1,
};

constexpr const Design *kDesigns[] = {&kPhone, &kPad};

// What typing one character takes. Space, Return and Delete are on every page.
struct Target {
    Page page = kAnyPage;
    int row = 0, key = 0;   // key -1: the row's last key; -2: Space
    bool shift = false;     // on the letters page: Shift must be on
};

// Where c is on the keyboard, looking on page `showing` first (a . or , is on two pages).
bool FindTarget(char c, const Design &design, Page showing, Target &target) {
    if (c == ' ' || c == '\n' || c == '\b') {
        target = c == ' ' ? Target{kAnyPage, 3, -2} : Target{kAnyPage, c == '\n' ? design.returnRow : design.deleteRow, -1};
        return true;
    }
    for (Page page : {showing, kLetters, kNumbers, kSymbols}) {
        if (page > kSymbols)
            continue;
        for (int row = 0; row < 3; row++) {
            const RowChars &chars = design.pages[page][row];
            for (bool shift : {false, true}) {
                const size_t at = (shift ? chars.shifted : chars.plain).find(c);
                if (at != std::string_view::npos) {
                    target = {page, row, chars.first + int(at), shift};
                    return true;
                }
            }
        }
    }
    return false;
}

// How long a tap holds the key down, and how long to wait after it before the next one.
constexpr auto kHold = 50ms;
constexpr auto kGap = 90ms;
// Before reading the keyboard, the guest must have received every tap sent (it can lag well
// behind), and then the screen must have stopped changing for kQuiet (the key pressed has been
// drawn, a page has switched), or kSettleLimit must have passed. A read that finds no keyboard is
// tried again until kFindLimit has passed (a key's popup can still be covering a row).
constexpr auto kDeliveryLimit = 5s;
constexpr auto kQuiet = 200ms;
constexpr auto kSettleLimit = 1500ms;
constexpr auto kFindLimit = 4s;
// A key stands out from the keyboard's background by more than this, in luminance (0 to 255).
constexpr int kContrast = 20;

}  // namespace

// ---- Reading the keyboard off the screen -----------------------------------------------------

// A keyboard read off a frame. Positions are in the interface's upright pixels: u across, v down,
// in a width x height interface that is the panel turned `turn` quarter-turns (see Panel).
struct OnScreenKeyboard::Layout {
    struct Key {
        int left, right;
    };
    struct Row {
        int center = 0;          // v of the row's middle, where taps go
        std::vector<Key> keys;   // left to right
    };

    const uint8_t *pixels = nullptr;   // the frame (BGRA)
    int stride = 0, panelWidth = 0, panelHeight = 0;
    int turn = 0, width = 0, height = 0;
    const Design *design = nullptr;
    Row rows[4];
    int keyHeight = 0;      // a key's height, top to bottom
    bool letters = false;   // the letters page shows (otherwise numbers or symbols)
    bool shift = false;     // Shift is on (letters page)
    std::vector<std::pair<int, int>> bands;   // what Find saw: lines through a row of letter keys (top, bottom)

    // The panel pixel at upright (u, v). Turn 1 has the interface's bottom along the panel's right
    // edge (the iPad upright), 2 is upside down, 3 has the bottom along the left edge.
    std::pair<int, int> Panel(int u, int v) const {
        switch (turn) {
        case 1: return {v, panelHeight - 1 - u};
        case 2: return {panelWidth - 1 - u, panelHeight - 1 - v};
        case 3: return {panelWidth - 1 - v, u};
        default: return {u, v};
        }
    }

    std::array<int, 3> Rgb(int u, int v) const {
        const auto [x, y] = Panel(u, v);
        const uint8_t *p = pixels + size_t(y) * stride + size_t(x) * 4;
        return {p[2], p[1], p[0]};
    }

    int Luma(int u, int v) const {
        const auto [r, g, b] = Rgb(u, v);
        return (299 * r + 587 * g + 114 * b) / 1000;
    }

    int LastKey(int row) const { return int(rows[row].keys.size()) - 1; }

    // Space: the widest key between the first and the last of the bottom row; -1 if the keyboard
    // has none (the web address keyboard).
    int SpaceKey() const {
        const auto &keys = rows[3].keys;
        int best = -1;
        for (int i = 1; i + 1 < int(keys.size()); i++)
            if (best < 0 || keys[i].right - keys[i].left > keys[best].right - keys[best].left)
                best = i;
        return best >= 0 && keys[best].right - keys[best].left >= width / 4 ? best : -1;
    }

    // The keys along line v: the stretches that stand out from the keyboard's background, which
    // shows at the line's left end (keys never touch the edge). Breaks of a pixel or two inside a
    // key (where a label's edge happens to match the background) are joined; specks are dropped.
    std::vector<Key> KeysAlong(int v) const {
        const int background = Luma(1, v);
        const int join = std::max(2, width / 100), smallest = std::max(4, width / 40);
        std::vector<Key> keys;
        int start = -1, last = -1;
        auto close = [&] {
            if (start >= 0 && last - start + 1 >= smallest)
                keys.push_back({start, last});
        };
        for (int u = 0; u < width; u++) {
            if (std::abs(Luma(u, v) - background) <= kContrast)
                continue;
            if (start >= 0 && u - last - 1 > join) {
                close();
                start = -1;
            }
            if (start < 0)
                start = u;
            last = u;
        }
        close();
        return keys;
    }

    // Whether a key's label is blue somewhere (how the iPad shows Shift on): looks over the key's
    // middle half, top to bottom.
    bool HasBlue(int row, int key) const {
        const Key &k = rows[row].keys[key];
        int blue = 0;
        for (int v = rows[row].center - keyHeight / 3; v <= rows[row].center + keyHeight / 3; v++)
            for (int u = k.left + (k.right - k.left) / 4; u <= k.right - (k.right - k.left) / 4; u++) {
                const auto [r, g, b] = Rgb(u, v);
                blue += b > 150 && b - r > 50;
            }
        return blue >= 3;
    }

    // How bright a key is along line v: the median across it, so a label crossing the line counts
    // for little.
    int KeyBrightness(int row, int key, int v) const {
        const Key &k = rows[row].keys[key];
        std::vector<int> lumas;
        for (int u = k.left + 2; u <= k.right - 2; u++)
            lumas.push_back(Luma(u, v));
        if (lumas.empty())
            return Luma(k.left, v);
        std::nth_element(lumas.begin(), lumas.begin() + lumas.size() / 2, lumas.end());
        return lumas[lumas.size() / 2];
    }

    bool Find();
};

namespace {

// A line through a row of letter keys: at least nine keys of a letter key's width.
bool ThroughLetterRow(const std::vector<OnScreenKeyboard::Layout::Key> &keys, int width) {
    int letterSized = 0;
    for (const auto &key : keys) {
        const int size = key.right - key.left + 1;
        letterSized += size >= width * 45 / 1000 && size <= width * 130 / 1000;
    }
    return letterSized >= 9;
}

}  // namespace

// Finds the keyboard in the interface at this turn. Its two top rows are bands of lines through a
// row of letter keys, a row's height apart, and its bottom row, two rows further down, sits just
// above the bottom of the screen. Fills in the rows, design, page and Shift.
bool OnScreenKeyboard::Layout::Find() {
    // A row's band of lines goes on over a few lines that miss (where a label's gray edge matches
    // the background and splits a key, the line still crosses most keys), and ends at the gap
    // between two rows, where a line crosses next to no keys.
    const int mend = std::max(4, height / 60);
    bands.clear();   // top to bottom
    int top = -1, lastThrough = -1;
    for (int v = height / 4; v <= height; v++) {
        const auto keys = v < height ? KeysAlong(v) : std::vector<Key>();
        if (ThroughLetterRow(keys, width)) {
            if (top < 0)
                top = v;
            lastThrough = v;
        } else if (top >= 0 && (keys.size() < 5 || v - lastThrough > mend)) {
            bands.push_back({top, lastThrough});
            top = -1;
        }
    }
    std::erase_if(bands, [&](const auto &band) { return band.second - band.first + 1 < std::max(4, height / 60); });
    auto center = [](const std::pair<int, int> &band) { return (band.first + band.second) / 2; };
    auto size = [](const std::pair<int, int> &band) { return band.second - band.first + 1; };
    for (size_t i = 0; i + 1 < bands.size(); i++) {
        const auto &first = bands[i], &second = bands[i + 1];
        const int pitch = center(second) - center(first), rowSize = size(first);
        const int margin = height - (center(first) + 3 * pitch);   // below the bottom row's middle
        if (std::abs(size(second) - rowSize) > std::max(3, rowSize / 4) || pitch <= rowSize ||
            pitch > rowSize * 22 / 10 || margin < pitch / 10 || margin > pitch)
            continue;
        // Each row's keys, along a line in the upper part of the keys, clear of their labels.
        keyHeight = rowSize;
        const int above = rowSize * 3 / 10;
        for (int row = 0; row < 4; row++) {
            rows[row].center = center(first) + row * pitch;
            rows[row].keys = KeysAlong(rows[row].center - above);
        }
        design = nullptr;
        for (const Design *candidate : kDesigns)
            if (rows[0].keys.size() == candidate->topRowKeys)
                design = candidate;
        if (!design || rows[2].keys.size() < 3 || rows[3].keys.size() < 2)
            continue;
        letters = rows[2].keys.size() == design->letterRowKeys;
        // Off, Shift looks like Delete. On, it lights up (iPhone: white, or a glow) or its arrow
        // turns blue (iPad).
        const int deleteRow = design->deleteRow;
        shift = letters && (KeyBrightness(2, 0, rows[2].center - above) -
                                    KeyBrightness(deleteRow, LastKey(deleteRow), rows[deleteRow].center - above) >
                                kContrast ||
                            HasBlue(2, 0));
        return true;
    }
    return false;
}

// ---- Typing ----------------------------------------------------------------------------------

OnScreenKeyboard::OnScreenKeyboard(Engine &engine, Touch touch, Problem problem)
    : engine_(engine), touch_(std::move(touch)), problem_(std::move(problem)), pixels_(SharedFrame::kPixelBytes),
      thread_(&OnScreenKeyboard::Run, this) {}

OnScreenKeyboard::~OnScreenKeyboard() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    wake_.notify_one();
    thread_.join();
}

void OnScreenKeyboard::Type(const std::string &text) {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push_back(text);
    wake_.notify_one();
}

// The worker is going away, or the device has stopped: typing ends.
bool OnScreenKeyboard::Stopping() {
    std::lock_guard<std::mutex> lock(mutex_);
    return stop_ || !engine_.ready();
}

void OnScreenKeyboard::Run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stop_) {
        if (queue_.empty()) {
            wake_.wait(lock);
            continue;
        }
        std::string text;
        while (!queue_.empty()) {   // what was typed meanwhile goes in the same burst
            text += queue_.front();
            queue_.pop_front();
        }
        lock.unlock();
        if (engine_.ready())
            TypeText(text);
        else
            problem_("The device isn't running yet.");
        lock.lock();
    }
}

// Waits until the guest has the taps sent and its screen has settled (see kQuiet), then reads
// the keyboard off the screen.
bool OnScreenKeyboard::ReadKeyboard(Layout &layout) {
    using Clock = std::chrono::steady_clock;
    const auto delivered = Clock::now() + kDeliveryLimit;
    while (lastTouch_ && engine_.inputStatus(lastTouch_) == QEMU_IOS_QUEUED && Clock::now() < delivered &&
           !Stopping())
        std::this_thread::sleep_for(10ms);
    const auto giveUp = Clock::now() + kFindLimit;
    for (;;) {
        const auto settled = Clock::now() + kSettleLimit;
        auto changed = Clock::now();
        for (;;) {
            if (Stopping())
                return false;
            const auto now = Clock::now();
            QemuIosPortableFrame meta{};
            meta.size = sizeof meta;
            meta.version = 1;
            if (engine_.frameCopy(pixels_.data(), pixels_.size(), serial_, &meta) == QEMU_IOS_OK) {
                serial_ = meta.serial;
                width_ = int(meta.width);
                height_ = int(meta.height);
                stride_ = int(meta.stride);
                changed = now;
            } else if (width_ > 0 && (now - changed >= kQuiet || now >= settled)) {
                break;
            }
            std::this_thread::sleep_for(15ms);
        }
        if (Read(layout, pixels_.data(), width_, height_, stride_))
            return true;
        if (Clock::now() >= giveUp)
            return false;
        std::this_thread::sleep_for(250ms);
    }
}

// Looks for the keyboard at each turn of the panel.
bool OnScreenKeyboard::Read(Layout &layout, const uint8_t *bgra, int width, int height, int stride) {
    layout.pixels = bgra;
    layout.stride = stride;
    layout.panelWidth = width;
    layout.panelHeight = height;
    for (int turn : {0, 1, 3, 2}) {   // upside down last: few apps turn that way
        layout.turn = turn;
        layout.width = turn % 2 ? height : width;
        layout.height = turn % 2 ? width : height;
        if (layout.Find())
            return true;
    }
    return false;
}

std::string OnScreenKeyboard::Describe(const uint8_t *bgra, int width, int height, int stride) {
    Layout layout;
    if (!Read(layout, bgra, width, height, stride)) {
        std::string text = "No keyboard found. Bands through a row of letter keys, by turn:\n";
        for (int turn : {0, 1, 3, 2}) {
            layout.turn = turn;
            layout.width = turn % 2 ? height : width;
            layout.height = turn % 2 ? width : height;
            layout.Find();
            text += "  turn " + std::to_string(turn) + ":";
            for (const auto &[top, bottom] : layout.bands)
                text += " " + std::to_string(top) + "-" + std::to_string(bottom);
            text += "\n";
        }
        return text;
    }
    std::string text = std::string(layout.design == &kPad ? "iPad" : "iPhone") + " keyboard, turned " +
                       std::to_string(layout.turn) + " (interface " + std::to_string(layout.width) + "x" +
                       std::to_string(layout.height) + "), " + (layout.letters ? "letters" : "numbers or symbols") +
                       (layout.letters ? layout.shift ? ", Shift on" : ", Shift off" : "") + "\n";
    for (int row = 0; row < 4; row++) {
        text += "  row " + std::to_string(row) + " at v " + std::to_string(layout.rows[row].center) + ":";
        for (const auto &key : layout.rows[row].keys)
            text += " " + std::to_string(key.left) + "-" + std::to_string(key.right);
        text += "\n";
    }
    text += "  Space: key " + std::to_string(layout.SpaceKey()) + " of row 3";
    return text;
}

void OnScreenKeyboard::Tap(const Layout &layout, int row, int key) {
    const auto &k = layout.rows[row].keys[key];
    const auto [x, y] = layout.Panel((k.left + k.right) / 2, layout.rows[row].center);
    const double nx = (x + 0.5) / layout.panelWidth, ny = (y + 0.5) / layout.panelHeight;
    touch_(nx, ny, QEMU_IOS_TOUCH_BEGIN);
    std::this_thread::sleep_for(kHold);
    lastTouch_ = touch_(nx, ny, QEMU_IOS_TOUCH_END);
    std::this_thread::sleep_for(kGap);
}

// Types one burst of text. The keyboard is read again whenever a tap may have changed it (the
// page, Shift turned on by iOS after a space, the keyboard closed by Return). Between letters it
// can't change, so those go in quickly.
void OnScreenKeyboard::TypeText(const std::string &text) {
    const auto started = std::chrono::steady_clock::now();
    Layout layout;
    bool current = false;   // `layout` still matches the screen
    // Which page shows. The screen tells letters from the rest; numbers and symbols look alike, so
    // which of those shows is known only from the page keys tapped here.
    Page page = kUnknown;
    int skipped = 0;
    auto read = [&] {
        if (!ReadKeyboard(layout)) {
            if (Stopping())
                return false;
            // The screen it couldn't read, to check with "iOS-Emulator --keyboard".
            std::vector<uint8_t> packed;
            for (int y = 0; y < height_; y++)
                packed.insert(packed.end(), pixels_.begin() + size_t(y) * stride_,
                              pixels_.begin() + size_t(y) * stride_ + size_t(width_) * 4);
            try {
                WritePng(DataDir() / "logs" / "keyboard-not-found.png", packed.data(), width_, height_);
            } catch (const std::exception &) {
            }
            problem_("No keyboard is showing in iOS: tap a text field first, then type.");
            return false;
        }
        if (layout.letters)
            page = kLetters;
        else if (page == kLetters)
            page = kUnknown;   // not the page expected
        current = true;
        return true;
    };

    for (const char c : text) {
        if (Stopping())
            return;
        if (c == '\r' || uint8_t(c) >= 0x80) {   // "\r\n" types one Return; a UTF-8 character counts once
            skipped += uint8_t(c) >= 0x80 && (uint8_t(c) & 0xc0) != 0x80;
            continue;
        }
        if (!current && !read())
            return;
        Target target;
        if (!FindTarget(c, *layout.design, page, target)) {
            skipped++;
            continue;
        }

        // To the character's page: letters -> numbers -> symbols -> numbers, and from numbers or
        // symbols (or not knowing which) back to letters.
        for (int step = 0; target.page != kAnyPage && page != target.page; step++) {
            if (step == 4) {
                problem_("The keyboard in iOS didn't change pages as expected, so typing stopped.");
                return;
            }
            if (page == kLetters) {
                Tap(layout, 3, 0);   // .?123
                page = kNumbers;
            } else if (page == kUnknown || target.page == kLetters) {
                Tap(layout, 3, 0);   // ABC
                page = kLetters;
            } else {
                Tap(layout, 2, 0);   // #+= or 123
                page = page == kNumbers ? kSymbols : kNumbers;
            }
            if (!read())
                return;
        }

        const int key = target.key == -1 ? layout.LastKey(target.row) : target.key == -2 ? layout.SpaceKey() : target.key;
        if (key < 0 || key >= int(layout.rows[target.row].keys.size())) {
            skipped++;   // e.g. a space on the web address keyboard, which has no Space key
            continue;
        }
        if (target.page == kLetters && layout.shift != target.shift) {
            Tap(layout, 2, 0);
            layout.shift = target.shift;
        }
        Tap(layout, target.row, key);
        // After a lowercase letter, Shift stays off and the page unchanged. Anything else may change
        // the keyboard: after a capital, Shift is usually off again, but not always (two Shift taps
        // that reach a lagging guest close together lock it on).
        if (target.page != kLetters || target.shift)
            current = false;
    }
    Log("Typed %zu characters in %.1f s", text.size(),
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
    if (skipped)
        problem_(std::to_string(skipped) + " character(s) have no key on the keyboard in iOS and weren't typed.");
}
