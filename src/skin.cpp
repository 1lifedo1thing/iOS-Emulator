#include "skin.h"

#include "../third_party/qemu-ios/qemu-ios-ui.h"

#include <GL/gl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#pragma warning(push, 0)   // nanosvg's own conversions
#define NANOSVG_IMPLEMENTATION
#include "../third_party/nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "../third_party/nanosvg/nanosvgrast.h"
#pragma warning(pop)

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

namespace {

ImVec2 Min(ImVec2 a, ImVec2 b) { return ImVec2(std::min(a.x, b.x), std::min(a.y, b.y)); }
ImVec2 Max(ImVec2 a, ImVec2 b) { return ImVec2(std::max(a.x, b.x), std::max(a.y, b.y)); }

// The art each board wears. From the front, the iPod touch 3G looks the same as the 2G, and the
// iPhone 4S as the iPhone 4.
std::string SkinFile(const std::string &board) {
    static const std::map<std::string, std::string> files = {
        {"n45ap", "ipod-touch-1g.svg"}, {"n72ap", "ipod-touch-2g.svg"}, {"n18ap", "ipod-touch-2g.svg"},
        {"n81ap", "ipod-touch-4g.svg"}, {"m68ap", "iphone-2g.svg"},     {"n88ap", "iphone-3gs.svg"},
        {"n90ap", "iphone-4.svg"},      {"k48ap", "ipad.svg"},          {"n94ap", "iphone-4.svg"},
    };
    auto found = files.find(board);
    return found != files.end() ? found->second : "ipod-touch-2g.svg";
}

// A shape's part: its id up to the first ".", so "power.shine" belongs to "power".
std::string PartOf(const NSVGshape *shape) {
    const char *dot = strchr(shape->id, '.');
    return dot ? std::string(shape->id, dot) : std::string(shape->id);
}

int ButtonFor(const std::string &part) {
    if (part == "home")
        return QEMU_IOS_BUTTON_HOME;
    if (part == "power")
        return QEMU_IOS_BUTTON_POWER;
    if (part == "volume-up")
        return QEMU_IOS_BUTTON_VOLUME_UP;
    if (part == "volume-down")
        return QEMU_IOS_BUTTON_VOLUME_DOWN;
    return -1;
}

enum class Layer { Body, Buttons, Glare };

Layer LayerOf(const NSVGshape *shape) {
    const std::string part = PartOf(shape);
    return ButtonFor(part) >= 0 ? Layer::Buttons : part == "glare" ? Layer::Glare : Layer::Body;
}

unsigned UploadTexture(const std::vector<uint8_t> &rgba, int width, int height) {
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    return id;
}

void DeleteTextures(const std::initializer_list<unsigned> &ids) {
    for (GLuint id : ids)
        if (id)
            glDeleteTextures(1, &id);
}

}   // namespace

ImVec2 ToPanel(ImVec2 shown, Orientation orientation, bool turned) {
    // Undo the device's turn: shown with Home on the right, the device's top is on the left.
    const ImVec2 device = orientation == Orientation::Landscape ? ImVec2(1 - shown.y, shown.x) : shown;
    // Then the quarter turn iOS gives its portrait UI on a landscape panel.
    return turned ? ImVec2(device.y, 1 - device.x) : device;
}

// ---- Loading ---------------------------------------------------------------------------------

Skin *Skin::For(const std::string &board) {
    static std::map<std::string, std::unique_ptr<Skin>> loaded;   // by file; null when it can't be read
    const std::string file = SkinFile(board);
    if (auto found = loaded.find(file); found != loaded.end())
        return found->second.get();

    std::unique_ptr<Skin> skin(new Skin);
    try {
        std::string svg = LoadFile(ResourcesDir() / "skins" / file);
        skin->image_ = nsvgParse(svg.data(), "px", 96);   // parses in place
    } catch (const std::exception &e) {
        Log("skin %s: %s", file.c_str(), e.what());
    }
    if (!skin->image_ || skin->image_->width <= 0) {
        loaded[file] = nullptr;
        return nullptr;
    }
    skin->width_ = skin->image_->width;
    skin->height_ = skin->image_->height;
    skin->bodyMax_ = ImVec2(skin->width_, skin->height_);

    // Find the parts the app uses, from their shapes' bounds.
    std::map<int, Button> buttons;
    for (NSVGshape *shape = skin->image_->shapes; shape; shape = shape->next) {
        const ImVec2 min(shape->bounds[0], shape->bounds[1]), max(shape->bounds[2], shape->bounds[3]);
        const std::string part = PartOf(shape);
        if (part == "screen") {
            skin->screenMin_ = min;
            skin->screenMax_ = max;
        } else if (part == "body") {
            skin->bodyMin_ = min;
            skin->bodyMax_ = max;
        } else if (int button = ButtonFor(part); button >= 0) {
            auto [it, added] = buttons.try_emplace(button, Button{button, min, max});
            it->second.min = Min(it->second.min, min);
            it->second.max = Max(it->second.max, max);
        }
    }
    for (auto &[id, button] : buttons)
        skin->buttons_.push_back(button);
    if (skin->screenMax_.x <= skin->screenMin_.x) {   // no screen: the art can't be used
        Log("skin %s has no \"screen\" element", file.c_str());
        loaded[file] = nullptr;
        return nullptr;
    }
    return (loaded[file] = std::move(skin)).get();
}

// The skins live until the app exits, after its OpenGL context is gone, so textures aren't freed here.
Skin::~Skin() {
    if (image_)
        nsvgDelete(image_);
}

// ---- Layout ----------------------------------------------------------------------------------

ImVec2 Skin::ToWindow(const SkinLayout &layout, ImVec2 skin) const {
    if (layout.orientation == Orientation::Landscape)   // the device's top goes left, its right side up
        return ImVec2(layout.min.x + skin.y * layout.scale, layout.min.y + (width_ - skin.x) * layout.scale);
    return ImVec2(layout.min.x + skin.x * layout.scale, layout.min.y + skin.y * layout.scale);
}

SkinLayout Skin::Fit(ImVec2 origin, ImVec2 area, Orientation orientation) const {
    SkinLayout layout;
    layout.orientation = orientation;
    const bool landscape = orientation == Orientation::Landscape;
    const ImVec2 shown = landscape ? ImVec2(height_, width_) : ImVec2(width_, height_);
    layout.scale = std::max(0.01f, std::min(area.x / shown.x, area.y / shown.y));
    layout.min = ImVec2(std::floor(origin.x + (area.x - shown.x * layout.scale) / 2),
                        std::floor(origin.y + (area.y - shown.y * layout.scale) / 2));
    layout.max = ImVec2(layout.min.x + shown.x * layout.scale, layout.min.y + shown.y * layout.scale);

    // A quarter turn keeps rectangles upright: turn two corners and sort them.
    auto turnRect = [&](ImVec2 min, ImVec2 max, ImVec2 &outMin, ImVec2 &outMax) {
        const ImVec2 a = ToWindow(layout, min), b = ToWindow(layout, max);
        outMin = Min(a, b);
        outMax = Max(a, b);
    };
    turnRect(screenMin_, screenMax_, layout.screenMin, layout.screenMax);
    const float fingertip = ImGui::GetFontSize() * 1.4f;
    for (const Button &button : buttons_) {
        SkinLayout::Button b{button.button};
        turnRect(button.min, button.max, b.min, b.max);
        const ImVec2 center((b.min.x + b.max.x) / 2, (b.min.y + b.max.y) / 2);
        const ImVec2 half(std::max(b.max.x - b.min.x, fingertip) / 2, std::max(b.max.y - b.min.y, fingertip) / 2);
        b.hitMin = ImVec2(center.x - half.x, center.y - half.y);
        b.hitMax = ImVec2(center.x + half.x, center.y + half.y);
        layout.buttons.push_back(b);
    }
    return layout;
}

// ---- Rasterizing -----------------------------------------------------------------------------

Skin::Raster Skin::Rasterize(int height) const {
    Raster raster;
    const float scale = height / height_;
    raster.width = std::max(1, int(std::ceil(width_ * scale)));
    raster.height = height;
    std::vector<uint8_t> rgba(size_t(raster.width) * raster.height * 4);
    NSVGrasterizer *rasterizer = nsvgCreateRasterizer();
    for (Layer layer : {Layer::Body, Layer::Buttons, Layer::Glare}) {
        for (NSVGshape *shape = image_->shapes; shape; shape = shape->next)
            shape->flags = LayerOf(shape) == layer ? NSVG_FLAGS_VISIBLE : 0;
        nsvgRasterize(rasterizer, image_, 0, 0, scale, rgba.data(), raster.width, raster.height, raster.width * 4);
        const unsigned texture = UploadTexture(rgba, raster.width, raster.height);
        (layer == Layer::Body ? raster.body : layer == Layer::Buttons ? raster.buttons : raster.glare) = texture;
    }
    nsvgDeleteRasterizer(rasterizer);
    return raster;
}

Skin::Raster &Skin::RasterFor(const SkinLayout &layout, bool settle) {
    const double now = ImGui::GetTime();
    const int height = std::max(8, int(std::ceil(height_ * layout.scale / 8)) * 8);   // in steps of 8 pixels

    // Forget sizes not drawn for a while (one is always kept).
    for (size_t i = 0; i < rasters_.size() && rasters_.size() > 1;) {
        if (now - rasters_[i].used > 3 && rasters_[i].height != height) {
            DeleteTextures({rasters_[i].body, rasters_[i].buttons, rasters_[i].glare});
            rasters_.erase(rasters_.begin() + i);
        } else {
            ++i;
        }
    }

    Raster *best = nullptr;
    for (Raster &raster : rasters_)
        if (!best || std::abs(raster.height - height) < std::abs(best->height - height))
            best = &raster;
    if (!best || best->height != height) {
        // A new size: rasterize it now, or once it has held for a moment while the window resizes.
        if (wantedHeight_ != height) {
            wantedHeight_ = height;
            wantedSince_ = now;
        }
        if (!best || !settle || now - wantedSince_ > 0.15) {
            rasters_.push_back(Rasterize(height));
            best = &rasters_.back();
        }
    }
    best->used = now;
    return *best;
}

// ---- Drawing ---------------------------------------------------------------------------------

// Draws the part of a layer between two points of the art, moved by `offset` window pixels.
void Skin::DrawRegion(ImDrawList *draw, const SkinLayout &layout, unsigned texture, ImVec2 skinMin, ImVec2 skinMax,
                      ImVec2 offset, ImU32 tint) const {
    const ImVec2 corners[4] = {skinMin, ImVec2(skinMax.x, skinMin.y), skinMax, ImVec2(skinMin.x, skinMax.y)};
    ImVec2 at[4], uv[4];
    for (int i = 0; i < 4; i++) {
        const ImVec2 p = ToWindow(layout, corners[i]);
        at[i] = ImVec2(p.x + offset.x, p.y + offset.y);
        uv[i] = ImVec2(corners[i].x / width_, corners[i].y / height_);
    }
    draw->AddImageQuad(ImTextureID(intptr_t(texture)), at[0], at[1], at[2], at[3], uv[0], uv[1], uv[2], uv[3], tint);
}

void Skin::Draw(ImDrawList *draw, const SkinLayout &layout, const std::function<bool(int)> &pressed, int hovered,
                bool settle) {
    const Raster &raster = RasterFor(layout, settle);

    // A button's drawn region is a little larger than its shapes, for their antialiased edges. A side
    // button held down sinks into the body by most of what sticks out of it, and one under the mouse
    // rises a little. Home darkens when held, and gets a soft ring under the mouse.
    auto drawButton = [&](const Button &button) {
        const ImVec2 min(button.min.x - 2, button.min.y - 2), max(button.max.x + 2, button.max.y + 2);
        const bool down = pressed(button.button);
        const float out[4] = {bodyMin_.y - button.min.y, button.max.x - bodyMax_.x, button.max.y - bodyMax_.y,
                              bodyMin_.x - button.min.x};   // how far it sticks out of the top, right, bottom, left
        const int edge = int(std::max_element(out, out + 4) - out);
        ImVec2 offset(0, 0);
        if (out[edge] > 0 && (down || hovered == button.button)) {
            const float depth = out[edge] * (down ? 0.7f : -0.25f);   // inward, or outward
            const ImVec2 sink = edge == 0 ? ImVec2(0, depth) : edge == 1 ? ImVec2(-depth, 0)
                              : edge == 2 ? ImVec2(0, -depth)          : ImVec2(depth, 0);
            const ImVec2 center((button.min.x + button.max.x) / 2, (button.min.y + button.max.y) / 2);
            const ImVec2 from = ToWindow(layout, center);
            const ImVec2 to = ToWindow(layout, ImVec2(center.x + sink.x, center.y + sink.y));
            offset = ImVec2(to.x - from.x, to.y - from.y);
        }
        DrawRegion(draw, layout, raster.buttons, min, max, offset, down ? IM_COL32(165, 165, 170, 255) : IM_COL32_WHITE);
        if (out[edge] <= 0 && hovered == button.button && !down) {   // a button on the face (Home)
            const ImVec2 a = ToWindow(layout, button.min), b = ToWindow(layout, button.max);
            draw->AddCircle(ImVec2((a.x + b.x) / 2, (a.y + b.y) / 2), std::abs(b.x - a.x) / 2 + 1.5f,
                            IM_COL32(255, 255, 255, 60), 0, 1.5f);
        }
    };

    for (const Button &button : buttons_)   // side buttons go under the body, which hides all but their tips
        if (button.button != QEMU_IOS_BUTTON_HOME)
            drawButton(button);
    DrawRegion(draw, layout, raster.body, ImVec2(0, 0), ImVec2(width_, height_));
    for (const Button &button : buttons_)
        if (button.button == QEMU_IOS_BUTTON_HOME)
            drawButton(button);
}

void Skin::DrawGlare(ImDrawList *draw, const SkinLayout &layout) {
    DrawRegion(draw, layout, RasterFor(layout, true).glare, screenMin_, screenMax_);
}
