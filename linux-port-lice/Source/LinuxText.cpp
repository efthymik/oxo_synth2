#include "LinuxText.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include "lice.h"
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

namespace oxo::text {

namespace {

// The desktop's Xft settings, as the X server publishes them.
struct Settings {
    enum class Hint { None, Slight, Medium, Full } hint = Hint::Slight;
    enum class Subpixel { None, RGB, BGR, VRGB, VBGR } subpixel = Subpixel::None;
    bool antialias = true;
};

Settings gSettings;
FT_Library gLibrary = nullptr;
bool gReady = false;
bool gInitialised = false;
std::mutex gMutex;

struct Face {
    FT_Face face = nullptr;
    std::string file;
    int index = 0;
    bool hasGlyphs = false;
};

// Face 0 = sans bold, face 1 = mono bold; further entries are per-codepoint
// fallbacks found through fontconfig.
std::vector<Face> gFaces;

struct Glyph {
    std::vector<unsigned char> bitmap; // coverage, or RGB triplets for LCD
    int width = 0, height = 0, span = 0;
    int left = 0, top = 0;
    double advance = 0;
    bool lcd = false;
};

std::map<uint64_t, Glyph> gCache;

uint32_t codepointAt(const std::string &s, size_t &at) {
    if (at >= s.size()) return 0;
    const auto byte = [&](size_t i) { return static_cast<unsigned char>(s[i]); };
    const unsigned char first = byte(at);
    if (first < 0x80) {
        ++at;
        return first;
    }
    int length = 0;
    uint32_t value = 0;
    if ((first & 0xE0) == 0xC0) {
        length = 2;
        value = first & 0x1F;
    } else if ((first & 0xF0) == 0xE0) {
        length = 3;
        value = first & 0x0F;
    } else if ((first & 0xF8) == 0xF0) {
        length = 4;
        value = first & 0x07;
    } else {
        ++at;
        return 0;
    }
    if (at + size_t(length) > s.size()) {
        at = s.size();
        return 0;
    }
    for (int i = 1; i < length; ++i) {
        const unsigned char b = byte(at + size_t(i));
        if ((b & 0xC0) != 0x80) {
            at += size_t(i);
            return 0;
        }
        value = (value << 6) | (b & 0x3F);
    }
    at += size_t(length);
    return value;
}

bool isControl(uint32_t code) { return code < 0x20 || code == 0x7F || (code >= 0x80 && code <= 0x9F); }

std::string trimmed(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

// Reads Xft.hintstyle / Xft.rgba / Xft.antialias from RESOURCE_MANAGER.
void readXftSettings(xcb_connection_t *connection) {
    if (!connection) return;
    xcb_screen_iterator_t screens = xcb_setup_roots_iterator(xcb_get_setup(connection));
    if (!screens.rem) return;
    const xcb_window_t root = screens.data->root;
    xcb_get_property_cookie_t cookie =
        xcb_get_property(connection, 0, root, XCB_ATOM_RESOURCE_MANAGER, XCB_ATOM_STRING, 0, 1u << 20);
    xcb_get_property_reply_t *reply = xcb_get_property_reply(connection, cookie, nullptr);
    if (!reply) return;
    const int length = xcb_get_property_value_length(reply);
    std::string text(static_cast<const char *>(xcb_get_property_value(reply)), size_t(std::max(0, length)));
    free(reply);
    for (size_t at = 0; at < text.size();) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(at, end - at);
        at = end + 1;
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string key = trimmed(line.substr(0, colon));
        const std::string value = trimmed(line.substr(colon + 1));
        if (key == "Xft.hintstyle") {
            if (value == "hintnone") gSettings.hint = Settings::Hint::None;
            else if (value == "hintslight") gSettings.hint = Settings::Hint::Slight;
            else if (value == "hintmedium") gSettings.hint = Settings::Hint::Medium;
            else if (value == "hintfull") gSettings.hint = Settings::Hint::Full;
        } else if (key == "Xft.rgba") {
            if (value == "rgb") gSettings.subpixel = Settings::Subpixel::RGB;
            else if (value == "bgr") gSettings.subpixel = Settings::Subpixel::BGR;
            else if (value == "vrgb") gSettings.subpixel = Settings::Subpixel::VRGB;
            else if (value == "vbgr") gSettings.subpixel = Settings::Subpixel::VBGR;
            else gSettings.subpixel = Settings::Subpixel::None;
        } else if (key == "Xft.antialias") {
            gSettings.antialias = value != "0";
        }
    }
    // Under a Wayland session the compositor may scale the X11 surface
    // (fractional scaling: this desktop reports Xft.dpi 144, i.e. 1.5x), and
    // resampling LCD triplets turns them into colour fringes — measured on
    // such a desktop, grayscale is the sane default there. A plain X11 session
    // honours Xft.rgba exactly as the desktop sets it.
    if (std::getenv("WAYLAND_DISPLAY")) gSettings.subpixel = Settings::Subpixel::None;
}

FT_UInt loadFlags() {
    switch (gSettings.hint) {
    case Settings::Hint::None: return FT_LOAD_NO_HINTING;
    case Settings::Hint::Slight: return FT_LOAD_TARGET_LIGHT;
    case Settings::Hint::Medium:
    case Settings::Hint::Full: return FT_LOAD_TARGET_NORMAL;
    }
    return FT_LOAD_TARGET_LIGHT;
}

FT_Render_Mode renderMode() {
    if (!gSettings.antialias) return FT_RENDER_MODE_MONO;
    switch (gSettings.subpixel) {
    case Settings::Subpixel::None: return FT_RENDER_MODE_NORMAL;
    case Settings::Subpixel::RGB:
    case Settings::Subpixel::BGR: return FT_RENDER_MODE_LCD;
    case Settings::Subpixel::VRGB:
    case Settings::Subpixel::VBGR: return FT_RENDER_MODE_LCD_V;
    }
    return FT_RENDER_MODE_NORMAL;
}

bool openFace(const std::string &file, int index, Face &face) {
    if (!gLibrary) return false;
    if (FT_New_Face(gLibrary, file.c_str(), index, &face.face) != 0 || !face.face) return false;
    face.file = file;
    face.index = index;
    face.hasGlyphs = true;
    return true;
}

// fontconfig lookup: a family name, or the font that covers `codepoint`.
bool findFont(const char *family, uint32_t codepoint, std::string &file, int &index) {
    FcPattern *pattern = FcPatternCreate();
    if (!pattern) return false;
    if (family) {
        FcPatternAddString(pattern, FC_FAMILY, reinterpret_cast<const FcChar8 *>(family));
        FcPatternAddInteger(pattern, FC_WEIGHT, FC_WEIGHT_BOLD);
        FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ROMAN);
    }
    if (codepoint) {
        FcCharSet *set = FcCharSetCreate();
        if (set) {
            FcCharSetAddChar(set, codepoint);
            FcPatternAddCharSet(pattern, FC_CHARSET, set);
            FcCharSetDestroy(set);
        }
    }
    FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult result = FcResultNoMatch;
    FcPattern *match = FcFontMatch(nullptr, pattern, &result);
    FcPatternDestroy(pattern);
    if (!match) return false;
    FcChar8 *path = nullptr;
    int fontIndex = 0;
    const bool ok = FcPatternGetString(match, FC_FILE, 0, &path) == FcResultMatch && path &&
                    FcPatternGetInteger(match, FC_INDEX, 0, &fontIndex) == FcResultMatch;
    if (ok) {
        file = reinterpret_cast<const char *>(path);
        index = fontIndex;
    }
    FcPatternDestroy(match);
    return ok;
}

int faceFor(uint32_t codepoint, bool mono);

const Glyph *glyphFor(int faceIndex, uint32_t codepoint, int pixelSize) {
    const uint64_t key = (uint64_t(faceIndex) << 40) | (uint64_t(uint32_t(pixelSize) & 0xFF) << 32) | codepoint;
    const auto found = gCache.find(key);
    if (found != gCache.end()) return &found->second;
    if (gCache.size() > 4096) gCache.clear(); // readouts come and go
    Face &face = gFaces[size_t(faceIndex)];
    if (FT_Set_Pixel_Sizes(face.face, 0, FT_UInt(pixelSize)) != 0) return nullptr;
    const FT_UInt glyphIndex = FT_Get_Char_Index(face.face, codepoint);
    if (!glyphIndex) return nullptr;
    if (FT_Load_Glyph(face.face, glyphIndex, loadFlags()) != 0) return nullptr;
    if (FT_Render_Glyph(face.face->glyph, renderMode()) != 0) return nullptr;
    const FT_GlyphSlot slot = face.face->glyph;
    Glyph glyph;
    glyph.lcd = slot->bitmap.pixel_mode == FT_PIXEL_MODE_LCD || slot->bitmap.pixel_mode == FT_PIXEL_MODE_LCD_V;
    glyph.width = int(slot->bitmap.width);
    glyph.height = int(slot->bitmap.rows);
    glyph.span = int(slot->bitmap.pitch);
    glyph.left = slot->bitmap_left;
    glyph.top = slot->bitmap_top;
    glyph.advance = slot->advance.x / 64.0;
    const size_t bytes = size_t(std::abs(glyph.span)) * size_t(glyph.height);
    glyph.bitmap.assign(slot->bitmap.buffer, slot->bitmap.buffer + bytes);
    return &gCache.emplace(key, std::move(glyph)).first->second;
}

// The face that can render `codepoint`: the primary one, else a fallback
// found through fontconfig (a CJK name gets a CJK font if one is installed).
int faceFor(uint32_t codepoint, bool mono) {
    const int primary = mono ? 1 : 0;
    if (gFaces[size_t(primary)].face && FT_Get_Char_Index(gFaces[size_t(primary)].face, codepoint)) return primary;
    // A fallback already opened for this codepoint?
    for (size_t i = 2; i < gFaces.size(); ++i)
        if (gFaces[i].face && FT_Get_Char_Index(gFaces[i].face, codepoint)) return int(i);
    std::string file;
    int index = 0;
    if (!findFont(nullptr, codepoint, file, index)) return -1;
    Face face;
    if (!openFace(file, index, face)) return -1;
    gFaces.push_back(std::move(face));
    return int(gFaces.size() - 1);
}

} // namespace

bool init(xcb_connection_t *connection) {
    std::lock_guard<std::mutex> lock(gMutex);
    if (gInitialised) return gReady;
    gInitialised = true;
    readXftSettings(connection);
    if (!FcInit()) {
        std::fprintf(stderr, "Oxo Synth2: fontconfig is unavailable; the panel has no text\n");
        return false;
    }
    if (FT_Init_FreeType(&gLibrary) != 0) {
        std::fprintf(stderr, "Oxo Synth2: FreeType is unavailable; the panel has no text\n");
        return false;
    }
    gFaces.resize(2);
    std::string file;
    int index = 0;
    if (findFont("sans-serif", 0, file, index)) openFace(file, index, gFaces[0]);
    if (findFont("monospace", 0, file, index)) openFace(file, index, gFaces[1]);
    if (!gFaces[0].face && gFaces[1].face) gFaces[0] = gFaces[1];
    if (!gFaces[1].face && gFaces[0].face) gFaces[1] = gFaces[0];
    gReady = gFaces[0].face != nullptr;
    if (!gReady) std::fprintf(stderr, "Oxo Synth2: no font found; the panel has no text\n");
    return gReady;
}

Extents measure(const std::string &utf8, double pixelSize, bool mono) {
    Extents extents;
    std::lock_guard<std::mutex> lock(gMutex);
    const int size = std::max(1, int(pixelSize + 0.5));
    const int primary = mono ? 1 : 0;
    if (gFaces[size_t(primary)].face) {
        if (FT_Set_Pixel_Sizes(gFaces[size_t(primary)].face, 0, FT_UInt(size)) == 0) {
            extents.ascent = gFaces[size_t(primary)].face->size->metrics.ascender / 64.0;
            extents.descent = -gFaces[size_t(primary)].face->size->metrics.descender / 64.0;
        }
    }
    size_t at = 0;
    while (at < utf8.size()) {
        const uint32_t code = codepointAt(utf8, at);
        if (code == 0 || isControl(code)) continue;
        const int face = faceFor(code, mono);
        if (face < 0) {
            extents.width += pixelSize * 0.65; // the fallback box
            continue;
        }
        const Glyph *glyph = glyphFor(face, code, size);
        extents.width += glyph ? glyph->advance : pixelSize * 0.65;
    }
    return extents;
}

double draw(LICE_IBitmap *target, const std::string &utf8, double x, double baseline, double pixelSize, bool mono,
            uint32_t rgb, double alpha) {
    if (!target || !gReady) return 0;
    std::lock_guard<std::mutex> lock(gMutex);
    const int size = std::max(1, int(pixelSize + 0.5));
    const int red = int((rgb >> 16) & 0xFF), green = int((rgb >> 8) & 0xFF), blue = int(rgb & 0xFF);
    const LICE_pixel colour = LICE_RGBA(red, green, blue, 255);
    double penX = x;
    size_t at = 0;
    while (at < utf8.size()) {
        const uint32_t code = codepointAt(utf8, at);
        if (code == 0 || isControl(code)) continue;
        const int faceIndex = faceFor(code, mono);
        if (faceIndex < 0) {
            // A hollow box: an uncovered character is visible, never a gap.
            LICE_DrawRect(target, int(penX + pixelSize * 0.08), int(baseline - pixelSize * 0.62),
                          int(pixelSize * 0.5), int(pixelSize * 0.62), colour, float(alpha), LICE_BLIT_MODE_COPY);
            penX += pixelSize * 0.65;
            continue;
        }
        const Glyph *glyph = glyphFor(faceIndex, code, size);
        if (!glyph) {
            penX += pixelSize * 0.65;
            continue;
        }
        const int left = int(penX + 0.5) + glyph->left;
        const int top = int(baseline + 0.5) - glyph->top;
        if (glyph->width > 0 && glyph->height > 0) {
            if (!glyph->lcd) {
                LICE_DrawGlyphEx(target, left, top, colour, glyph->bitmap.data(), glyph->width, glyph->span,
                                 glyph->height, float(alpha), LICE_BLIT_MODE_COPY);
            } else {
                // LCD: three coverage bytes per pixel; blend each channel.
                const bool swapped = gSettings.subpixel == Settings::Subpixel::BGR ||
                                     gSettings.subpixel == Settings::Subpixel::VBGR;
                const int channels = glyph->lcd ? 3 : 1;
                for (int row = 0; row < glyph->height; ++row) {
                    const unsigned char *src = glyph->bitmap.data() + size_t(row) * size_t(glyph->span);
                    const int y = top + row;
                    if (y < 0 || y >= target->getHeight()) continue;
                    LICE_pixel *dst = target->getBits() + size_t(y) * size_t(target->getRowSpan());
                    for (int column = 0; column < glyph->width; ++column) {
                        const int px = left + column;
                        if (px < 0 || px >= target->getWidth()) continue;
                        const int base = column * channels;
                        int cov[3] = {src[base], src[base + 1], src[base + 2]};
                        if (swapped) std::swap(cov[0], cov[2]);
                        const int source[3] = {red, green, blue};
                        LICE_pixel &pixel = dst[px];
                        int out[3] = {int(pixel & 0xFF), int((pixel >> 8) & 0xFF), int((pixel >> 16) & 0xFF)};
                        for (int channel = 0; channel < 3; ++channel) {
                            const int c = int(cov[channel] * alpha + 0.5);
                            out[channel] = out[channel] + ((source[channel] - out[channel]) * c) / 255;
                        }
                        pixel = LICE_RGBA(out[0], out[1], out[2], 255);
                    }
                }
            }
        }
        penX += glyph->advance;
    }
    return penX - x;
}

const char *hintMode() {
    switch (gSettings.hint) {
    case Settings::Hint::None: return "none";
    case Settings::Hint::Slight: return "slight";
    case Settings::Hint::Medium: return "medium";
    case Settings::Hint::Full: return "full";
    }
    return "slight";
}

const char *subpixelMode() {
    if (!gSettings.antialias) return "mono";
    switch (gSettings.subpixel) {
    case Settings::Subpixel::None: return "grayscale";
    case Settings::Subpixel::RGB: return "rgb";
    case Settings::Subpixel::BGR: return "bgr";
    case Settings::Subpixel::VRGB: return "vrgb";
    case Settings::Subpixel::VBGR: return "vbgr";
    }
    return "grayscale";
}

} // namespace oxo::text
