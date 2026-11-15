#pragma once

// The panel's drawing surface: LICE (vendored, static) behind a small API in
// design coordinates.
//
// LICE is a plain "draw into a bitmap" library with explicit pixel
// coordinates; this class adds what the panel needs on top: the design ->
// device scale, the panel's shape helpers (rounded faces, rings, gradients,
// LEDs), and text through LinuxText (system FreeType, hinted).
//
// Colours are 0xRRGGBB like the themes. LICE's own LICE_GETR/G/B macros have
// no outer parentheses ("(v>>16)&0xff") and cannot be used inside an
// expression, so the channel accessors here are ours.

#include "LinuxText.hpp"

#include "lice.h"
// lice.h pulls in SWELL's swell-types.h on non-Windows, which defines these
// as macros and breaks every C++ header included afterwards.
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include <cstdint>
#include <string>

namespace oxo {

inline int colourRed(uint32_t v) { return int((v >> 16) & 0xFF); }
inline int colourGreen(uint32_t v) { return int((v >> 8) & 0xFF); }
inline int colourBlue(uint32_t v) { return int(v & 0xFF); }
inline LICE_pixel liceColour(uint32_t rgb, int alpha = 255) {
    return LICE_RGBA(colourRed(rgb), colourGreen(rgb), colourBlue(rgb), alpha);
}

class Canvas {
public:
    Canvas() = default;
    explicit Canvas(LICE_IBitmap *target, double scale = 1.0) : target_(target), scale_(scale) {}

    void setTarget(LICE_IBitmap *target, double scale) {
        target_ = target;
        scale_ = scale;
    }
    LICE_IBitmap *target() const { return target_; }
    double scale() const { return scale_; }
    bool valid() const { return target_ != nullptr; }

    void setColor(uint32_t rgb, double alpha = 1.0) {
        colour_ = liceColour(rgb, int(alpha * 255 + 0.5));
        alpha_ = alpha;
    }

    void fillRounded(int px, int py, int pw, int ph, float radius, LICE_pixel colour, double alpha);
    void fillRect(float x, float y, float w, float h);
    void fillRect(uint32_t rgb, float x, float y, float w, float h);
    void rectOutline(uint32_t rgb, float x, float y, float w, float h, float width = 1);
    void roundedFill(uint32_t rgb, float x, float y, float w, float h, float radius);
    // A vertical gradient inside a rounded rect (scanlines, so the corners
    // stay round).
    void roundedGradient(uint32_t top, uint32_t bottom, float x, float y, float w, float h, float radius);
    // A rounded outline of `width` design pixels. Exact: the region is
    // snapshotted, the outer rounded rect is filled, and the inside is put
    // back, so there are no corner notches and any background works.
    void roundedRing(uint32_t rgb, float x, float y, float w, float h, float radius, float width = 1,
                     double alpha = 1.0);
    void linearGradient(uint32_t top, uint32_t bottom, float x, float y, float w, float h);
    // A radial gradient whose bright centre can sit off the knob's centre
    // (the SwiftUI knob highlight), computed per pixel.
    void radialGradient(uint32_t inner, uint32_t outer, float cx, float cy, float radius, float innerCx, float innerCy);
    void line(uint32_t rgb, float x1, float y1, float x2, float y2, bool aa = true);
    // With the current colour and alpha (faint grid lines).
    void lineAt(float x1, float y1, float x2, float y2, bool aa = true);
    void thickLine(uint32_t rgb, float x1, float y1, float x2, float y2, float width);
    void circle(uint32_t rgb, float cx, float cy, float radius);
    // The current colour (setColor), for the LED helpers.
    void circleAt(float cx, float cy, float radius);
    void ringAt(float cx, float cy, float radius);
    void roundedFillAt(float x, float y, float w, float h, float radius);
    void ring(uint32_t rgb, float cx, float cy, float radius);
    void arc(uint32_t rgb, float cx, float cy, float radius, float a0, float a1);
    void bezier(uint32_t rgb, float x0, float y0, float cx, float cy, float x1, float y1);
    // Text in design coordinates (`size` is the design pixel size; the panel's
    // rule for small sizes is applied here).
    void text(const std::string &utf8, float x, float y, float w, float h, float size, uint32_t rgb,
              bool bold = false, bool centred = false, bool mono = false);
    double textWidth(const std::string &utf8, float size, bool mono) const;

private:
    int deviceX(float x) const { return int(x * scale_ + 0.5); }
    int deviceY(float y) const { return int(y * scale_ + 0.5); }
    int deviceSize(float v) const { return int(v * scale_ + 0.5); }
    double pixelSizeFor(float designSize) const;

    LICE_IBitmap *target_ = nullptr;
    double scale_ = 1.0;
    LICE_pixel colour_ = 0;
    double alpha_ = 1.0;
};

} // namespace oxo
