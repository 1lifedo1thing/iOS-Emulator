// Device art: the picture of the real device drawn around the screen, from resources\skins\<name>.svg.
//
// Each SVG is a front view in tenths of a millimetre. The app reads these element ids (a part can have
// several shapes: "power", "power.shine"; a group passes its id down to its shapes):
//   body                                  the device's outline; pressed side buttons sink into it
//   screen                                exactly the panel; the running device's picture covers it
//   home, power, volume-up, volume-down   the buttons that work (only those the device has)
//   glare                                 a faint reflection drawn over the picture
// Everything else is decoration. The art is rasterized with nanosvg at the size it's shown, in three
// layers (body, buttons, glare), so it stays sharp at any size and can be turned for landscape.
#pragma once

#include "common.h"

#include "imgui.h"

#include <functional>
#include <memory>

// Which way the device is turned on the desktop.
enum class Orientation {
    Portrait,
    Landscape,   // a quarter turn anticlockwise: Home on the right
};

// A point on the screen as shown (u rightward, v downward, 0 to 1) in the panel's own scan, the
// space of the frame's pixels and of touches. `turned` is a panel wider than tall (the iPad's),
// into which iOS turns its portrait UI a quarter clockwise.
ImVec2 ToPanel(ImVec2 shown, Orientation orientation, bool turned);

// Where a skin lands in the window, for one size and orientation. All in window pixels.
struct SkinLayout {
    ImVec2 min, max;                 // the whole picture (the device and its shadow)
    ImVec2 screenMin, screenMax;     // the panel
    struct Button {
        int button;                  // QEMU_IOS_BUTTON_*
        ImVec2 min, max;             // as drawn
        ImVec2 hitMin, hitMax;       // where a click presses it (never smaller than a fingertip)
    };
    std::vector<Button> buttons;
    float scale = 1;                 // window pixels per skin unit
    Orientation orientation = Orientation::Portrait;
};

class Skin {
public:
    // The art for a board ("n72ap"), loaded once. nullptr if its SVG is missing or unreadable.
    static Skin *For(const std::string &board);
    ~Skin();

    // Fits the device into the rectangle at `origin`, `area` pixels big, centred.
    SkinLayout Fit(ImVec2 origin, ImVec2 area, Orientation orientation) const;

    // Draws the device: side buttons, body and Home. `pressed(button)` draws a button held down, and
    // `hovered` (a QEMU_IOS_BUTTON_*, or -1) outlines the one under the mouse. The caller draws the
    // screen over the art, then DrawGlare on top. `settle` waits for a new size to hold for a moment
    // before rasterizing it (while the window is being resized, the last raster is scaled instead).
    void Draw(ImDrawList *draw, const SkinLayout &layout, const std::function<bool(int)> &pressed, int hovered = -1,
              bool settle = true);
    void DrawGlare(ImDrawList *draw, const SkinLayout &layout);

private:
    struct Button {
        int button;
        ImVec2 min, max;             // its shapes' bounds, in skin units
    };
    struct Raster {                  // the three layers at one size, as OpenGL textures
        int width = 0, height = 0;
        unsigned body = 0, buttons = 0, glare = 0;
        double used = 0;             // ImGui::GetTime() it was last drawn
    };

    Skin() = default;
    Raster &RasterFor(const SkinLayout &layout, bool settle);
    Raster Rasterize(int height) const;
    void DrawRegion(ImDrawList *draw, const SkinLayout &layout, unsigned texture, ImVec2 skinMin, ImVec2 skinMax,
                    ImVec2 offset = ImVec2(0, 0), ImU32 tint = IM_COL32_WHITE) const;
    ImVec2 ToWindow(const SkinLayout &layout, ImVec2 skin) const;

    struct NSVGimage *image_ = nullptr;
    float width_ = 0, height_ = 0;           // the SVG's size, in skin units
    ImVec2 screenMin_, screenMax_, bodyMin_, bodyMax_;
    std::vector<Button> buttons_;
    std::vector<Raster> rasters_;
    int wantedHeight_ = 0;                   // a new size asked for while the window resizes...
    double wantedSince_ = 0;                 // ...and since when
};
