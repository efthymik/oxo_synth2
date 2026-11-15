#pragma once

// Text for the panel: system FreeType (hinted glyph bitmaps) laid out with
// the desktop's own settings.
//
// The fonts come from fontconfig ("sans-serif" / "monospace", bold), the same
// way every Linux UI picks them; a codepoint the primary face lacks (a
// Japanese patch name, say) is looked up in whatever font fontconfig finds
// for it. Hinting and subpixel rendering follow the X server's Xft resources
// (RESOURCE_MANAGER): hintstyle none/slight/medium/full -> FT_LOAD_NO_HINTING
// / FT_LOAD_TARGET_LIGHT / FT_LOAD_TARGET_NORMAL, and rgba none/rgb/bgr/vrgb/
// vbgr -> grayscale / LCD / LCD_V. Under Wayland (WAYLAND_DISPLAY set) the
// compositor may scale the surface, so grayscale is used there, as GTK does.
//
// Glyph bitmaps are cached per (face, pixel size, glyph) and composited with
// LICE_DrawGlyphEx (grayscale) or a per-channel blend (LCD).

#include <xcb/xcb.h>

#include <cstdint>
#include <string>

class LICE_IBitmap;

namespace oxo::text {

// Opens fontconfig / FreeType and reads the Xft settings from `connection`
// (may be null: defaults are grayscale + slight hinting). Idempotent.
bool init(xcb_connection_t *connection);

struct Extents {
    double width = 0;   // the advance width, in device pixels
    double ascent = 0;  // of the face, at that size
    double descent = 0;
};

Extents measure(const std::string &utf8, double pixelSize, bool mono);

// Draws with the first glyph's origin at (x, baseline) in device pixels.
// Returns the advance width. Control characters are never drawn.
double draw(LICE_IBitmap *target, const std::string &utf8, double x, double baseline, double pixelSize, bool mono,
            uint32_t rgb, double alpha = 1.0);

// Which hinting / subpixel mode was picked (for the log and the docs).
const char *hintMode();
const char *subpixelMode();

} // namespace oxo::text
