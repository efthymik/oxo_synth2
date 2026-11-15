#include "LinuxPanel.hpp"

#include "EditorCore/Json.hpp"
#include "EditorCore/Trash.hpp"
#include "EditorCore/Storage.hpp"
#include "EngineCore/PatchLibrary.hpp"
#include "OxoSynth2Identity.h"

#include <algorithm>
#include <chrono>
#include <fcntl.h>
#include <unistd.h>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <vector>


namespace oxo {

void PanelRect::unite(const PanelRect &o) {
    if (o.empty()) return;
    if (empty()) {
        *this = o;
        return;
    }
    const float x0 = std::min(x, o.x), y0 = std::min(y, o.y);
    const float x1 = std::max(x + w, o.x + o.w), y1 = std::max(y + h, o.y + o.h);
    x = x0, y = y0, w = x1 - x0, h = y1 - y0;
}

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr int kPanel = themes::kPanel, kHeader = themes::kHeader, kHeaderText = themes::kHeaderText,
              kLabel = themes::kLabel, kKnob = themes::kKnobBody, kKnobHighlight = themes::kKnobGradient,
              kDial = themes::kKnobDial, kButton = themes::kButton, kLed = themes::kLed, kRadio = themes::kRadio,
              kDisplayText = themes::kDisplayText, kDisplayTop = themes::kDisplayTop,
              kDisplayBottom = themes::kDisplayBottom;
// Theme.lcdText (Shared/SynthControls.swift): fixed browser / LCD text.
constexpr uint32_t kLcdText = 0xE0FFFF;

uint32_t muted(uint32_t c, float factor) {
    auto part = [&](int shift) { return uint32_t(std::lround(((c >> shift) & 255) * factor)); };
    return (part(16) << 16) | (part(8) << 8) | part(0);
}
uint32_t blend(uint32_t background, uint32_t foreground, float amount) {
    amount = std::clamp(amount, 0.f, 1.f);
    auto part = [&](int shift) {
        const float back = float((background >> shift) & 255), front = float((foreground >> shift) & 255);
        return uint32_t(std::lround(back * (1.f - amount) + front * amount));
    };
    return (part(16) << 16) | (part(8) << 8) | part(0);
}
// ---- drawing helpers over the Canvas (LICE + the embedded text) ----
void fill(Canvas &cr, uint32_t c, float x, float y, float w, float h) { cr.fillRect(c, x, y, w, h); }
void stroke(Canvas &cr, uint32_t c, float x, float y, float w, float h, float width = 1) {
    cr.rectOutline(c, x, y, w, h, width);
}
void roundedFill(Canvas &cr, uint32_t c, float x, float y, float w, float h, float radius) {
    cr.roundedFill(c, x, y, w, h, radius);
}
void roundedOutline(Canvas &cr, uint32_t c, float x, float y, float w, float h, float radius, float width = 1.f) {
    cr.roundedRing(c, x, y, w, h, radius, width);
}
// The original SwiftUI LED shadow: bloom only when the LED is on (two small
// translucent halos, as the Windows port draws them).
void led(Canvas &cr, uint32_t c, float x, float y, float radius = 3, bool glowing = false) {
    if (glowing) {
        cr.setColor(c, 22 / 255.0);
        cr.circleAt(x, y, radius + 3.f);
        cr.setColor(c, 40 / 255.0);
        cr.circleAt(x, y, radius + 1.5f);
    }
    cr.setColor(c);
    cr.circleAt(x, y, radius);
    cr.setColor(0x000000, 130 / 255.0);
    cr.ringAt(x, y, radius);
}
void darkButton(Canvas &cr, uint32_t face, float x, float y, float w, float h, bool hovered = false) {
    if (hovered) face = blend(face, 0xFFFFFF, .07f);
    const uint32_t top = blend(face, 0xFFFFFF, .10f);
    cr.roundedGradient(top, face, x, y, w, h, 3.f);
    cr.roundedRing(0x000000, x, y, w, h, 3.f, 1.f, 170 / 255.0);
}
uint64_t nowMs() {
    using namespace std::chrono;
    return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// Defined with the file dialog below; the version box and "Library folder"
// use it too.
void openWithDesktop(const std::string &target);

// The panel's face: fontconfig's default sans, i.e. what every other Linux
// UI uses. The layout tolerates the wider families — a ToggleButton centres
// its LED + label group, so "sync" fits even in DejaVu Sans.
constexpr const char *kSansFamily = "sans-serif";
constexpr const char *kMonoFamily = "monospace";


// True when every byte is printable text: decodes UTF-8 and rejects C0
// controls, DEL, the C1 block and broken sequences. Used by the text fields,
// so a name can never absorb a null or an invisible control byte.
bool printableText(const std::string &utf8) {
    size_t at = 0;
    while (at < utf8.size()) {
        const unsigned char first = static_cast<unsigned char>(utf8[at]);
        uint32_t code = 0;
        size_t length = 1;
        if (first < 0x80) {
            code = first;
        } else if ((first & 0xE0) == 0xC0) {
            length = 2;
            code = first & 0x1F;
        } else if ((first & 0xF0) == 0xE0) {
            length = 3;
            code = first & 0x0F;
        } else if ((first & 0xF8) == 0xF0) {
            length = 4;
            code = first & 0x07;
        } else {
            return false;
        }
        if (at + length > utf8.size()) return false;
        for (size_t i = 1; i < length; ++i) {
            const unsigned char next = static_cast<unsigned char>(utf8[at + i]);
            if ((next & 0xC0) != 0x80) return false;
            code = (code << 6) | (next & 0x3F);
        }
        if (code < 0x20 || code == 0x7F || (code >= 0x80 && code <= 0x9F)) return false;
        at += length;
    }
    return true;
}

// ThemeStore.shared: one themes.json per process, shared by every editor.
themes::Store &themeStore() {
    static themes::Store store{storage::Root() / "themes.json"};
    return store;
}

// Bank list order (UI only: hook indices, slot numbers and host state are
// untouched): digits compare as numbers ("Bank 2" < "Bank 10"), letters
// case-insensitively.
bool naturalLess(const std::string &a, const std::string &b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const bool digitA = a[i] >= '0' && a[i] <= '9', digitB = b[j] >= '0' && b[j] <= '9';
        if (digitA && digitB) {
            const size_t ai = i, bj = j;
            while (i < a.size() && a[i] >= '0' && a[i] <= '9') ++i;
            while (j < b.size() && b[j] >= '0' && b[j] <= '9') ++j;
            size_t nzA = ai, nzB = bj;
            while (nzA < i && a[nzA] == '0') ++nzA;
            while (nzB < j && b[nzB] == '0') ++nzB;
            const size_t la = i - nzA, lb = j - nzB;
            if (la != lb) return la < lb;
            for (size_t k = 0; k < la; k++)
                if (a[nzA + k] != b[nzB + k]) return a[nzA + k] < b[nzB + k];
            if (i - ai != j - bj) return i - ai < j - bj; // fewer leading zeroes first
        } else {
            const unsigned char ac = (unsigned char)std::tolower((unsigned char)a[i]);
            const unsigned char bc = (unsigned char)std::tolower((unsigned char)b[j]);
            if (ac != bc) return ac < bc;
            ++i, ++j;
        }
    }
    return i == a.size() && j != b.size();
}

} // namespace

// ---------------------------------------------------------------------------
// Text: antialiased outlines in design coordinates (no grid-fitted bitmaps:
// letter positions stay put while the view is resized), built once per
// (text, size, box width, face) and then only filled.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

LinuxPanel::LinuxPanel(const OxoSynth2Hooks *hooks) : hooks_(hooks) {
    constructLayout();
    for (size_t i = 0; i < knownValues_.size(); ++i) knownValues_[i] = value(int(i));
    knownSerial_ = hooks_ && hooks_->patchSerial ? hooks_->patchSerial(hooks_->ctx) : 0;
    adoptTheme();
}

LinuxPanel::~LinuxPanel() = default;

// SynthModel.init: the session's palette, else the last one picked; either
// way pinned to the session.
void LinuxPanel::adoptTheme() {
    const auto restored = hooks_ && hooks_->themeText ? themes::FromStateText(hooks_->themeText(hooks_->ctx))
                                                     : std::nullopt;
    if (restored) {
        theme_ = *restored;
    } else {
        themeStore().Load();
        theme_ = themeStore().Current();
    }
    for (size_t i = 0; i < colors_.size(); ++i) colors_[i] = theme_[i] & 0xFFFFFF;
    if (hooks_ && hooks_->setThemeText) hooks_->setThemeText(hooks_->ctx, themes::StateText(theme_).c_str());
}

// SynthModel.applyTheme / setThemeColor: this instance wears it (session),
// and it becomes the palette the next new instance starts with.
void LinuxPanel::setTheme(const themes::Palette &palette) {
    theme_ = palette;
    for (size_t i = 0; i < colors_.size(); ++i) colors_[i] = theme_[i] & 0xFFFFFF;
    if (hooks_ && hooks_->setThemeText) hooks_->setThemeText(hooks_->ctx, themes::StateText(theme_).c_str());
    if (themeStore().Remember(theme_)) themeStore().Write();
    touch({0, 0, kDesignWidth, kDesignHeight});
}

void LinuxPanel::knob(int id, const char *name, float sx, float sy, float x, float y, float size) {
    // KnobView's stack: the circular knob's centre is at (x, y), the label
    // underneath; the control rect is the knob plus room for the label.
    controls_.push_back({Kind::Knob, id, 0, sx + x - size * .65f, sy + y - size * .65f, size * 1.3f,
                         size * 1.65f, name});
}
void LinuxPanel::button(int id, const char *name, Kind kind, float sx, float sy, float x, float y, float width) {
    controls_.push_back({kind, id, 0, sx + x - width / 2, sy + y - 8.5f, width, 17, name});
}
void LinuxPanel::display(int id, float sx, float sy, float x, float y, float width) {
    controls_.push_back({Kind::Display, id, 0, sx + x - width / 2, sy + y - 8.5f, width, 17, ""});
}
void LinuxPanel::radio(int id, float sx, float sy, float x, float y, std::vector<std::string> choices,
                       int first, bool horizontal, bool trailing) {
    const int n = int(choices.size());
    const float step = horizontal ? (id == 71 ? 44.f : 38.f)
                                 : (id == 14 || id == 31 || id == 32 || id == 38 ? 14.f : 13.f);
    for (int i = 0; i < n; i++) {
        const float px = sx + x + (horizontal ? (i - (n - 1) / 2.f) * step : 0.f);
        const float py = sy + y + (horizontal ? 0.f : (i - (n - 1) / 2.f) * step);
        const bool iconOnly = (id == 41 || id == 46);
        if (id == 14 || id == 71) {
            controls_.push_back({Kind::Radio, id, first + i, px - 22.f, py - 6.f, 44.f, 12.f,
                                 choices[size_t(i)], 0});
        } else if (id == 97) {
            // Sub octave switches need a full-width label at the same font size.
            controls_.push_back({Kind::Radio, id, first + i, sx + x - 42.f + float(i) * 54.f, sy + y - 6.f,
                                 50.f, 12.f, choices[size_t(i)], 0});
        } else {
            const float width = iconOnly ? 14.f : horizontal ? step : 65.f;
            controls_.push_back({Kind::Radio, id, first + i,
                                 iconOnly ? px - 7.f : px - (horizontal ? step / 2.f : 15.f), py - 6.f, width,
                                 12.f, choices[size_t(i)], trailing ? 1 : 0});
        }
    }
}
void LinuxPanel::waves(int id, float sx, float sy, float x, float y, const std::vector<int> &values,
                       const std::vector<int> &shapes) {
    // Original WaveRadio is a two-by-two grid with a 2px gap; slightly larger
    // cells and a 3px gap, still centred on the original x/y.
    for (size_t i = 0; i < values.size(); i++) {
        const float cx = sx + x + (i % 2 == 0 ? -13.5f : 13.5f);
        const float cy = sy + y + (i / 2 == 0 ? -9.f : 9.f);
        controls_.push_back({Kind::Wave, id, values[i], cx - 12.f, cy - 7.5f, 24.f, 15.f, "", shapes[i]});
    }
}
void LinuxPanel::label(float x, float y, const char *name) {
    controls_.push_back({Kind::Label, -1, 0, x - 40, y - 8, 80, 16, name});
}
void LinuxPanel::badge(float x, float y, const char *name, int id) {
    controls_.push_back({Kind::Badge, id, 0, x - 8, y - 8, 16, 16, name});
}

void LinuxPanel::constructLayout() {
    // Layout transcribed from Shared/SynthPanelView.swift (via
    // WindowsEditor.cpp), using each section's original local coordinate
    // system. Every visible Synth1 parameter keeps its original ID and is
    // driven by OxoSynth2Hooks.
    constexpr float ox = 6, oy = 34, lx = 6, ly = 278, mx = 6, my = 412, ax = 324, ay = 34, fx = 324, fy = 118,
                    arx = 324, ary = 278, ex = 548, ey = 34, qx = 548, qy = 118, dx = 548, dy = 180,
                    cx = 548, cy = 242, vx = 548, vy = 304;
    badge(ox + 14, oy + 36, "1"); // Leave the oscillator on/off button fixed.
    waves(0, ox, oy, 54, 44, {0, 1, 2, 3}, {0, 1, 2, 3});
    knob(76, "det", ox, oy, 104, 40);
    knob(45, "FM", ox, oy, 146, 40);
    label(ox + 194, oy + 24, "sub");
    waves(96, ox, oy, 216, 48, {0, 1, 2, 3}, {0, 3, 1, 5});
    knob(95, "sub", ox, oy, 282, 40);
    radio(97, ox, oy, 242, 74, {"0oct", "-1oct"}, 0, true);
    badge(ox + 14, oy + 98, "2");
    waves(1, ox, oy, 54, 102, {1, 2, 3, 4}, {1, 2, 3, 4});
    button(7, "ring", Kind::Toggle, ox, oy, 108, 98);
    button(6, "sync", Kind::Toggle, ox, oy, 154, 98);
    display(9, ox, oy, 260, 94, 32);
    label(ox + 260, oy + 110, "key shift");
    button(4, "track", Kind::Toggle, ox, oy, 36, 140, 52);
    knob(2, "pitch", ox, oy, 104, 136);
    knob(3, "fine", ox, oy, 146, 136);
    knob(5, "mix", ox, oy, 238, 136);
    knob(8, "p/w", ox, oy, 282, 136);
    button(10, "m.env", Kind::Toggle, ox, oy, 36, 184, 52);
    knob(12, "A", ox, oy, 104, 184);
    knob(13, "D", ox, oy, 146, 184);
    knob(11, "amt", ox, oy, 188, 184);
    button(71, "dest.", Kind::Cycle, ox, oy, 36, 220, 52);
    radio(71, ox, oy, 131, 220, {"osc2", "FM", "p/w"}, 0, true);
    knob(91, "phase", ox, oy, 238, 201);
    knob(72, "tune", ox, oy, 282, 201);
    badge(lx + 14, ly + 30, "1", 57);
    badge(lx + 298, ly + 30, "2", 58);
    controls_.push_back({Kind::Wave, 42, 0, lx + 42 - 13, ly + 30 - 8, 26, 17, "", 0});
    controls_.push_back({Kind::Wave, 47, 0, lx + 270 - 13, ly + 30 - 8, 26, 17, "", 0});
    button(41, "dst", Kind::Cycle, lx, ly, 78, 30, 32);
    button(46, "dst", Kind::Cycle, lx, ly, 234, 30, 32);
    knob(43, "spd", lx, ly, 30, 62);
    knob(44, "amt", lx, ly, 76, 62);
    knob(48, "spd", lx, ly, 236, 62);
    knob(49, "amt", lx, ly, 282, 62);
    label(lx + 52, ly + 99, "-sync-");
    label(lx + 260, ly + 99, "-sync-");
    button(67, "tempo", Kind::Toggle, lx, ly, 32, 114, 56);
    button(68, "key", Kind::Toggle, lx, ly, 84, 114, 36);
    button(69, "tempo", Kind::Toggle, lx, ly, 236, 114, 56);
    button(70, "key", Kind::Toggle, lx, ly, 288, 114, 36);
    std::vector<std::string> dests = {"osc2", "osc1,2", "filter", "amp", "p/w", "FM", "pan"};
    radio(41, lx, ly, 124, 72, dests, 1);
    radio(46, lx, ly, 188, 72, dests, 1, false, true);
    for (int i = 0; i < 7; i++) label(lx + 156, ly + 72 + (i - 3) * 13, dests[size_t(i)].c_str());
    label(mx + 32, my + 28, "p.b range");
    display(40, mx, my, 32, 46, 30);
    label(mx + 82, my + 30, "src1");
    label(mx + 82, my + 51, "src2");
    display(86, mx, my, 158, 30, 120);
    display(88, mx, my, 158, 51, 120);
    label(mx + 228, my + 30, "->");
    label(mx + 228, my + 51, "->");
    display(87, mx, my, 298, 30, 120);
    display(89, mx, my, 298, 51, 120);
    knob(50, "sens1", mx, my, 400, 34, 22);
    knob(51, "sens2", mx, my, 446, 34, 22);
    knob(25, "A", ax, ay, 20, 40);
    knob(26, "D", ax, ay, 55, 40);
    knob(27, "S", ax, ay, 90, 40);
    knob(28, "R", ax, ay, 125, 40);
    knob(29, "gain", ax, ay, 162, 40);
    knob(30, "vel", ax, ay, 198, 40);
    knob(15, "A", fx, fy, 20, 44);
    knob(16, "D", fx, fy, 52, 44);
    knob(17, "S", fx, fy, 84, 44);
    knob(18, "R", fx, fy, 116, 44);
    knob(21, "amt", fx, fy, 150, 44);
    button(14, "type", Kind::Cycle, fx, fy, 196, 44, 36);
    radio(14, fx, fy, 196, 96, {"LP12", "LP24", "HP12", "BP12", "LPDL"}, 0);
    knob(19, "frq", fx, fy, 20, 117);
    knob(20, "res", fx, fy, 52, 117);
    knob(23, "sat", fx, fy, 84, 117);
    knob(22, "trk", fx, fy, 116, 117);
    button(24, "vel", Kind::Toggle, fx, fy, 116, 84, 36);
    button(59, "ON", Kind::Toggle, arx, ary, 28, 30, 44);
    button(31, "type", Kind::Cycle, arx, ary, 78, 30);
    button(32, "range", Kind::Cycle, arx, ary, 128, 30, 44);
    radio(31, arx, ary, 40, 78, {"updown", "up", "down", "random"}, 1);
    radio(32, arx, ary, 126, 78, {"1oct", "2oct", "3oct", "4oct"}, 0);
    knob(33, "beat", arx, ary, 190, 36);
    knob(34, "gate", arx, ary, 190, 88);
    button(77, "ON", Kind::Toggle, ex, ey, 25, 31, 40);
    display(78, ex, ey, 25, 53, 40);
    knob(79, "ctl1", ex, ey, 108, 40);
    knob(80, "ctl2", ex, ey, 150, 40);
    knob(81, "level", ex, ey, 192, 40);
    knob(61, "freq", qx, qy, 24, 31, 22);
    knob(62, "level", qx, qy, 66, 31, 22);
    knob(63, "Q", qx, qy, 108, 31, 22);
    knob(60, "tone", qx, qy, 150, 31, 22);
    knob(90, "L-R", qx, qy, 192, 31, 22);
    button(65, "ON", Kind::Toggle, dx, dy, 23, 27, 40);
    display(82, dx, dy, 23, 47, 40);
    knob(35, "time", dx, dy, 68, 31, 22);
    knob(83, "sprd", dx, dy, 100, 31, 22);
    knob(36, "fdbk", dx, dy, 132, 31, 22);
    knob(98, "tone", dx, dy, 164, 31, 22);
    knob(37, "d/w", dx, dy, 196, 31, 22);
    button(66, "ON", Kind::Toggle, cx, cy, 23, 27, 40);
    display(64, cx, cy, 23, 47, 40);
    knob(52, "time", cx, cy, 68, 31, 22);
    knob(53, "depth", cx, cy, 100, 31, 22);
    knob(54, "rate", cx, cy, 132, 31, 22);
    knob(55, "fdbk", cx, cy, 164, 31, 22);
    knob(56, "level", cx, cy, 196, 31, 22);
    radio(38, vx, vy, 34, 46, {"poly", "mono", "legato"}, 0);
    display(94, vx, vy, 130, 38, 30);
    label(vx + 130, vy + 55, "poly");
    button(73, "unison", Kind::Toggle, vx, vy, 36, 100, 56);
    label(vx + 153, vy + 77, "portamento");
    knob(39, "", vx, vy, 128, 100);
    button(74, "auto", Kind::Toggle, vx, vy, 176, 100, 46);
    display(93, vx, vy, 24, 144, 26);
    label(vx + 24, vy + 161, "num");
    knob(75, "det", vx, vy, 68, 142);
    knob(92, "phase", vx, vy, 106, 142);
    knob(84, "sprd", vx, vy, 144, 142);
    knob(85, "pitch", vx, vy, 182, 142);
    knob(99, "vol", 6, 484, 19, 29, 26);
}

// ---------------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------------

int LinuxPanel::value(int id) const {
    return hooks_ && hooks_->getValue ? int(hooks_->getValue(hooks_->ctx, id)) : 0;
}

std::string LinuxPanel::formatted(int id) const {
    if (id == 86 || id == 88) {
        const int v = value(id);
        if ((v >> 8) == 0xE0) return "pitch bend";
        if ((v >> 8) == 0xD0) return "ch pressure";
        if ((v >> 8) == 0xB0) return "#" + std::to_string(v & 0xFF) + ":cc";
        return "none";
    }
    if (id == 87 || id == 89) {
        const int v = value(id);
        if (v < 0) return "none";
        return hooks_ && hooks_->paramName ? hooks_->paramName(hooks_->ctx, v) : "";
    }
    if (!hooks_ || !hooks_->formatValue) return std::to_string(value(id));
    // formatValue uses a shared scratch buffer; consume immediately.
    return hooks_->formatValue(hooks_->ctx, id, value(id));
}

std::string LinuxPanel::patchName() const {
    return hooks_ && hooks_->patchName ? hooks_->patchName(hooks_->ctx) : "Init";
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void LinuxPanel::toggleLayout(Canvas &cr, const Control &c, float &ledX, float &textX, float &textWidth) const {
    // The mac centres the group; the Windows port left-aligns it at +12 with
    // 14 px reserved, which clips "sync" / "key" at Segoe-less font metrics.
    const double ink = cr.textWidth(c.label, 10.f, false); // Theme.buttonFont
    const double group = 5 + 3 + ink;
    const double start = c.x + (c.w - group) * .5;
    ledX = float(start + 2.5);
    textX = float(start + 8);
    textWidth = float(c.w - (start - c.x) - 8);
}

void LinuxPanel::drawControlStatic(Canvas &cr, const Control &c) const {
    const auto &p = colors_;
    switch (c.kind) {
    case Kind::Label:
        cr.text(c.label, c.x, c.y, c.w, c.h, 9.5f, p[kLabel], false, true);
        return;
    case Kind::Knob: {
        const float size = c.w / 1.3f;
        const float x = c.x + c.w / 2, y = c.y + size * .65f;
        // Theme.knobTicks: label ink at 30% opacity.
        cr.setColor(p[kLabel], 77 / 255.0);
        for (int i = 0; i < 11; i++) {
            const float a = (-140 + i * 28 - 90) * kPi / 180;
            const float dotRing = size <= 22.f ? size * .65f - 2.3f : size * .65f - 1.5f;
            cr.fillRect(x + std::cos(a) * dotRing - .8f, y + std::sin(a) * dotRing - .8f, 1.6f, 1.6f);
        }
        const float radius = size * .5f;
        // Off-centre highlight: the SwiftUI knob's radial gradient.
        cr.radialGradient(p[kKnobHighlight], p[kKnob], x, y, radius, x - radius * .30f, y - radius * .38f);
        cr.ring(muted(p[kPanel], .5f), x, y, radius); // Theme.knobRim
        cr.text(c.label, x - 25, y + radius + 1, 50, 12, 9.5f, p[kLabel], false, true);
        return;
    }
    case Kind::Toggle:
    case Kind::Cycle:
        darkButton(cr, p[kButton], c.x, c.y, c.w, c.h);
        if (c.kind == Kind::Toggle) {
            float ledX = 0, textX = 0, textWidth = 0;
            toggleLayout(cr, c, ledX, textX, textWidth);
            cr.text(c.label, textX, c.y, textWidth, c.h, 10, 0xE8E8E8, true, false);
        } else {
            cr.text(c.label, c.x, c.y, c.w, c.h, 10, 0xE8E8E8, true, true);
        }
        return;
    case Kind::Display:
        cr.roundedGradient(p[kDisplayTop], p[kDisplayBottom], c.x, c.y, c.w, c.h, 2.f);
        cr.roundedRing(0x0B0B0B, c.x, c.y, c.w, c.h, 2.f);
        return;
    case Kind::Radio:
        if (c.id != 41 && c.id != 46) {
            const float left = c.id == 14 ? 13.f : (c.symbol ? 0.f : 15.f);
            const float width = c.id == 14 ? c.w - 14.f : c.w - (c.symbol ? 14.f : 16.f);
            cr.text(c.label, c.x + left, c.y, width, c.h, 9.5f, p[kLabel], false, false);
        }
        return;
    case Kind::Badge:
    case Kind::Wave:
        return; // their face depends on the value: dynamic
    }
}

void LinuxPanel::drawControlDynamic(Canvas &cr, const Control &c, bool hovered) {
    const auto &p = colors_;
    switch (c.kind) {
    case Kind::Label:
        return;
    case Kind::Badge: {
        const bool on = (c.id < 0 || value(c.id) != 0);
        darkButton(cr, on ? 0x0F1111 : 0x393939, c.x, c.y, c.w, c.h, hovered);
        // Move the four Osc/LFO badge digits down exactly one design pixel.
        cr.text(c.label, c.x, c.y + 1.f, c.w, c.h - 1.f, 12,
             c.id < 0 ? 0xFFFFFF : on ? 0xFFDA59 : 0x777777, true, true);
        return;
    }
    case Kind::Knob: {
        const float size = c.w / 1.3f, radius = size * .5f;
        const float x = c.x + c.w / 2, y = c.y + size * .65f;
        int low = 0, high = 127, def = 64;
        if (hooks_ && hooks_->paramInfo) hooks_->paramInfo(hooks_->ctx, c.id, &low, &high, &def);
        const float frac = std::clamp((float(value(c.id)) - float(low)) / float(std::max(1, high - low)), 0.f, 1.f);
        // A short capsule near the rim, matching the -140 to +140 degree travel.
        const float angle = (-140 + 280 * frac - 90) * kPi / 180;
        const float dx = std::cos(angle), dy = std::sin(angle);
        cr.thickLine(p[kDial], x + dx * radius * .18f, y + dy * radius * .18f, x + dx * radius * .84f,
                     y + dy * radius * .84f, 2.f);
        return;
    }
    case Kind::Toggle:
    case Kind::Cycle:
        if (hovered) {
            darkButton(cr, p[kButton], c.x, c.y, c.w, c.h, true);
            if (c.kind == Kind::Toggle) {
                float ledX = 0, textX = 0, textWidth = 0;
                toggleLayout(cr, c, ledX, textX, textWidth);
                cr.text(c.label, textX, c.y, textWidth, c.h, 10, 0xE8E8E8, true, false);
            } else {
                cr.text(c.label, c.x, c.y, c.w, c.h, 10, 0xE8E8E8, true, true);
            }
        }
        if (c.kind == Kind::Toggle) {
            const bool on = value(c.id) != 0;
            float ledX = 0, textX = 0, textWidth = 0;
            toggleLayout(cr, c, ledX, textX, textWidth);
            led(cr, on ? p[kLed] : muted(p[kLed], .3f), ledX, c.y + c.h / 2, 2.5f, on);
        }
        return;
    case Kind::Display: {
        std::string v = formatted(c.id);
        if (c.id == 9 || c.id == 40) { // 2-digit displays, as in SynthPanelView.swift
            char out[32];
            std::snprintf(out, sizeof out, "%02d", value(c.id));
            v = out;
        }
        cr.text(v, c.x + 1, c.y + 1, c.w - 2, c.h - 2, 11, p[kDisplayText], true, true, true);
        return;
    }
    case Kind::Radio: {
        const bool on = value(c.id) == c.value;
        const bool justLed = (c.id == 41 || c.id == 46);
        const float ledx = justLed ? c.x + c.w * .5f : c.symbol ? c.x + c.w - 7.f : c.x + 7.f;
        led(cr, on ? p[kRadio] : muted(p[kRadio], .42f), ledx, c.y + c.h / 2, 4.4f, on);
        return;
    }
    case Kind::Wave:
        drawWave(cr, c);
        return;
    }
}

void LinuxPanel::drawWave(Canvas &cr, const Control &c) {
    const auto &p = colors_;
    const bool on = (value(c.id) == c.value) || (c.id == 42 || c.id == 47);
    // Original WaveCell: flat fill (no gradient), 2 px corner radius.
    const uint32_t waveFace = on ? p[kDisplayText] : p[kButton];
    // Three thin, low-alpha rings add a soft aura while selected.
    if (on) {
        cr.roundedRing(blend(0x000000, waveFace, 20 / 255.0), c.x - 2.5f, c.y - 2.5f, c.w + 5.f, c.h + 5.f, 4.5f, 1.5f);
        cr.roundedRing(blend(0x000000, waveFace, 35 / 255.0), c.x - 1.5f, c.y - 1.5f, c.w + 3.f, c.h + 3.f, 3.5f, 1.2f);
        cr.roundedRing(blend(0x000000, waveFace, 55 / 255.0), c.x - .5f, c.y - .5f, c.w + 1.f, c.h + 1.f, 2.5f, 1.f);
    }
    cr.roundedFill(waveFace, c.x, c.y, c.w, c.h, 2.f);
    cr.roundedRing(0x000000, c.x, c.y, c.w, c.h, 2.f, 1.f, 170 / 255.0);
    const uint32_t ink = on ? 0x101719 : 0xDDDDDD;
    const float x = c.x + 4, y = c.y + 3.5f, w = c.w - 8, h = c.h - 7;
    int shape = c.symbol;
    if (c.id == 42 || c.id == 47) {
        static constexpr int cycleShapes[] = {6, 3, 5, 7, 8, 0};
        shape = cycleShapes[std::clamp(value(c.id), 0, 5)];
    }
    switch (shape) {
    case 0: { // sine
        float previousX = 0, previousY = 0;
        for (int i = 0; i < 25; i++) {
            const float t = i / 24.f;
            const float px = x + w * t, py = y + h * (.5f - .5f * std::sin(2 * kPi * t));
            if (i) cr.thickLine(ink, previousX, previousY, px, py, 1.7f);
            previousX = px;
            previousY = py;
        }
        break;
    }
    case 1:
    case 6: // saw / falling saw, the original two-stroke shape
        cr.thickLine(ink, x, y + h, x, y, 1.7f);
        cr.thickLine(ink, x, y, x + w, y + h, 1.7f);
        break;
    case 2:
    case 5: { // pulse (30%) / square (50%)
        const float duty = (shape == 2 ? .30f : .50f);
        cr.thickLine(ink, x, y + h, x, y, 1.7f);
        cr.thickLine(ink, x, y, x + w * duty, y, 1.7f);
        cr.thickLine(ink, x + w * duty, y, x + w * duty, y + h, 1.7f);
        cr.thickLine(ink, x + w * duty, y + h, x + w, y + h, 1.7f);
        cr.thickLine(ink, x + w, y + h, x + w, y, 1.7f);
        break;
    }
    case 3: // triangle
        cr.thickLine(ink, x, y + h, x + w * .5f, y, 1.7f);
        cr.thickLine(ink, x + w * .5f, y, x + w, y + h, 1.7f);
        break;
    case 4: { // fixed sample of noise; the same samples as WaveIcon.noise
        static constexpr float ys[] = {.5f, .1f, .8f, .3f, .95f, .2f, .6f, 0.f, .7f, .4f, .9f, .25f, .55f};
        for (int i = 1; i < 13; i++)
            cr.thickLine(ink, x + w * (i - 1) / 12.f, y + h * ys[i - 1], x + w * i / 12.f, y + h * ys[i], 1.7f);
        break;
    }
    case 7:
    case 8: { // stepped or smoothly interpolated random LFO
        static constexpr float ys[] = {.6f, .1f, .8f, .35f, .95f};
        const float step = w / 5.f, curve = step * .35f;
        const bool smooth = shape == 8;
        float previousX = x, previousY = y + h * ys[0];
        for (int i = 1; i < 5; i++) {
            const float edge = x + step * i;
            const float previous = y + h * ys[i - 1], level = y + h * ys[i];
            if (smooth) {
                cr.thickLine(ink, previousX, previousY, edge - curve, previous, 1.7f);
                cr.bezier(ink, edge - curve, previous, edge, previous, edge, level);
                cr.bezier(ink, edge, level, edge, level, edge + curve, level);
                previousX = edge + curve;
            } else {
                cr.thickLine(ink, previousX, previousY, edge, previous, 1.7f);
                cr.thickLine(ink, edge, previous, edge, level, 1.7f);
                previousX = edge;
            }
            previousY = level;
        }
        cr.thickLine(ink, previousX, previousY, x + w, previousY, 1.7f);
        break;
    }
    default:
        break;
    }
}

PanelRect LinuxPanel::chromeRect(int id) {
    switch (id) {
    case 3: return {420, 495, 24, 20};
    case 4: return {449, 495, 24, 20};
    case 5: return {492, 488, 50, 26};
    case 6: return {492, 522, 50, 26};
    case 8: return {558, 488, 186, 24};
    case 9: return {514, 38, 84, 20};
    case 10: return {604, 38, 74, 20};
    case 11: return {430, 38, 66, 20};
    case 12: return {502, 38, 112, 20};
    case 13: return {620, 38, 60, 20};
    case 14: return {690, 38, 64, 20};
    default: return {};
    }
}

// The panel's own chrome buttons (< >, write, menu).
void LinuxPanel::drawChrome(Canvas &cr, int id, bool hovered) const {
    const auto &p = colors_;
    const PanelRect r = chromeRect(id);
    const uint32_t face = id == 5 ? 0xB81715 : p[kButton];
    darkButton(cr, face, r.x, r.y, r.w, r.h, hovered);
    if (id == 3 || id == 4) {
        cr.text(id == 3 ? "<" : ">", r.x + 1, r.y, r.w - 3, r.h, 13, 0xFFFFFF, true, true);
    } else {
        cr.text(id == 5 ? "write" : "menu", r.x, r.y, r.w, r.h, 11, 0xFFFFFF, true, true);
    }
}

void LinuxPanel::drawStatic(Canvas &cr) const {
    const auto &p = colors_;
    cr.fillRect(p[kPanel], 0, 0, kDesignWidth, kDesignHeight);
    cr.text("Synth2", 8, 5, 94, 23, 20, p[kLabel], true);
    cr.text("Virtual Analog", 84, 7, 135, 20, 12, muted(p[kLabel], .75f));
    // SynthPanelView's version box: "V1.1.2(6)  a Synth1 recreation" (the
    // build only when known), from Identity.xcconfig; sized to its text
    // (10 px each side), right edge fixed.
    const int build = std::atoi(OXO_BUILD_NUMBER);
    std::string version = std::string("V") + OXO_VERSION + (build > 0 ? "(" + std::to_string(build) + ")" : "") +
                          "  a Synth1 recreation";
    const float ink = float(cr.textWidth(version, 10.f, false));
    const float boxWidth = std::clamp(ink + 20.f, 168.f, 400.f);
    const_cast<LinuxPanel *>(this)->versionLeft_ = 761.f - boxWidth;
    cr.fillRect(0xAD0F0C, versionLeft_, 5, boxWidth, 20);
    cr.rectOutline(0x101010, versionLeft_, 5, boxWidth, 20);
    cr.text(version, versionLeft_ + 3, 6, boxWidth - 6, 18, 10, 0xFFFFFF, true, true);
    struct Section { float x, y, w, h; const char *name; };
    static constexpr Section sections[] = {
        {6, 34, 312, 240, "Oscillators"}, {6, 278, 312, 130, "LFO"}, {6, 412, 536, 66, "Wheel / MIDI"},
        {324, 34, 218, 80, "Amplifier"}, {324, 118, 218, 156, "Filter"}, {324, 278, 218, 130, "Arpeggiator"},
        {548, 34, 216, 80, "Effect"}, {548, 118, 216, 58, "Equalizer / Pan"}, {548, 180, 216, 58, "Tempo Delay"},
        {548, 242, 216, 58, "Chorus / Flanger"}, {548, 304, 216, 174, "Voice"},
    };
    for (const auto &sec : sections) {
        // Header fill meets the section border exactly: no one-pixel gutter.
        cr.fillRect(p[kHeader], sec.x, sec.y, sec.w, 16.f);
        cr.rectOutline(muted(p[kHeader], .6f), sec.x, sec.y, sec.w, sec.h);
        cr.text(sec.name, sec.x + 5, sec.y, sec.w - 10, 16, 12, p[kHeaderText], true);
    }
    static constexpr float dividers[][4] = {{180, 56, 180, 110}, {14, 115, 310, 115}, {218, 121, 218, 268},
                                            {14, 197, 210, 197}};
    for (const auto &a : dividers) cr.line(muted(p[kHeader], .45f), a[0], a[1], a[2], a[3]);
    for (const auto &c : controls_) drawControlStatic(cr, c);
    // LCD + write/menu + voice grid geometry from Shared/SynthLibraryViews.swift.
    cr.roundedGradient(p[kDisplayTop], p[kDisplayBottom], 50, 484, 432, 70, 4.f);
    cr.roundedRing(0x070707, 50, 484, 432, 70, 4.f, 1.5f);
    for (const int id : {3, 4, 5, 6}) drawChrome(cr, id, false);
    // Small bottom-right host-resize grip.
    for (int i = 0; i < 3; i++) {
        const float a = float(i * 5);
        cr.line(blend(p[kPanel], p[kLabel], .65f), 752.f + a, 557.f, 767.f, 542.f + a);
    }
}

void LinuxPanel::drawVoiceGrid(Canvas &cr) {
    const uint64_t states = hooks_ && hooks_->voiceStates ? hooks_->voiceStates(hooks_->ctx) : 0;
    // The black squares are the voices the engine can actually allocate
    // (Synth::VoiceNoteOn), not the polyphony knob:
    //   poly          -> the polyphony knob's pool (0..limit-1)
    //   mono / legato -> one voice, or the unison stack (MonoVoice(0..n-1))
    const int poly = std::clamp(value(94), 1, 32);
    const int allocated =
        value(38) == 0 ? poly : (value(73) != 0 ? std::min(std::clamp(value(93), 2, 8), poly) : 1);
    constexpr float cell = 215.f / 16.f;
    const float left = 548, top = 505;
    constexpr float inset = 1.75f; // clear gap from the grid lines
    // VoiceGrid: white = held, gray = releasing, black = available, empty =
    // past the allocatable voices; lines = label ink at 12%. Not theme colours.
    for (int i = 0; i < 32; i++) {
        const int state = int((states >> (i * 2)) & 3);
        const bool hasColour = state == 2 || state == 1 || i < allocated;
        if (!hasColour) continue;
        const uint32_t colour = state == 2 ? 0xFFFFFF : state == 1 ? 0x8C8C8C : 0x000000;
        const float x = left + (i % 16) * cell, y = top + (i / 16) * cell;
        cr.fillRect(colour, x + inset, y + inset, cell - 2 * inset, cell - 2 * inset);
    }
    // One continuous grid instead of 32 overlapping rectangles.
    cr.setColor(colors_[kLabel], 31 / 255.0);
    for (int column = 0; column <= 16; column++) {
        const float x = left + column * cell + .5f;
        cr.lineAt(x, top, x, top + 2 * cell);
    }
    for (int row = 0; row <= 2; row++) {
        const float y = top + row * cell + .5f;
        cr.lineAt(left, y, left + 16 * cell, y);
    }
}

void LinuxPanel::drawLcdText(Canvas &cr) {
    int bank = -1, slot = -1;
    if (hooks_ && hooks_->loadedProgram) hooks_->loadedProgram(hooks_->ctx, &bank, &slot);
    char number[16];
    if (slot >= 0) std::snprintf(number, sizeof number, "%03d: ", slot + 1);
    else std::snprintf(number, sizeof number, "---: ");
    cr.text(std::string(number) + patchName(), 60, 489, 330, 25, 17, 0xDFFAFF, true, false, true);
    std::string second = flashText_;
    if (second.empty()) {
        if (bank >= 0 && bank < int(banks_.size())) {
            char number[16];
            std::snprintf(number, sizeof number, "%02d: ", displayBankNumber(bank));
            second = std::string(number) + banks_[size_t(bank)].name;
        } else {
            second = knownRevision_ > 0 ? "Click here to browse banks" : "Scanning preset banks...";
        }
    }
    cr.text(second, 60, 526, 392, 23, 13, colors_[kDisplayText], false, false, true);
}

void LinuxPanel::drawDynamic(Canvas &cr) {
    // The browser covers the panel area: only the bottom bar shows.
    const float browserBottom = browserRect().y + browserRect().h;
    for (size_t i = 0; i < controls_.size(); ++i) {
        const auto &c = controls_[i];
        if ((browser_ || file_.mode != FileMode::None || themeDialog_) && c.y < browserBottom) continue;
        drawControlDynamic(cr, c, hovered_ == int(i));
    }
    if (hoveredChrome_ >= 3 && hoveredChrome_ <= 6) drawChrome(cr, hoveredChrome_, true);
    drawLcdText(cr);
    if (midiLearn_) {
        if (!browser_) drawLearn(cr);
        // "Exit MIDI Learn" chip and its hint, as the mac's learn bar.
        darkButton(cr, 0xA30D0D, 558, 488, 186, 24, hoveredChrome_ == 8);
        cr.text("Exit MIDI Learn", 558, 488, 186, 24, 11, 0xFFFFFF, true, true);
        cr.text("Select a control, then move a MIDI CC", 548, 518, 206, 20, 9, colors_[kLabel], true, true);
    } else {
        drawVoiceGrid(cr);
    }
    if (browser_) drawBrowser(cr);
    if (file_.mode != FileMode::None) drawFileDialog(cr);
    if (themeDialog_) drawThemeEditor(cr);
    if (press_.active && press_.dragging) {
        // slotDraggable's preview: the label on the display colour
        const PanelRect r = dragLabelRect(cr);
        cr.setColor(colors_[kDisplayTop], 230 / 255.0);
        cr.roundedFillAt(r.x, r.y, r.w, r.h, 2.f);
        cr.text(press_.label, r.x + 3, r.y, r.w - 6, r.h, 10.2f, kLcdText, false, false, true);
    }
    if (confirm_.open) drawConfirm(cr);
    if (menu_.open) drawMenu(cr);
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

PanelRect LinuxPanel::bounds(const Control &c) {
    // Room for the LED / wave glow around a knob.
    return {c.x - 5.f, c.y - 5.f, c.w + 10.f, c.h + 10.f};
}

int LinuxPanel::hit(float x, float y) const {
    // Topmost first: the last control drawn wins.
    for (int i = int(controls_.size()) - 1; i >= 0; --i) {
        const auto &c = controls_[size_t(i)];
        if (c.id < 0) continue;
        if (x < c.x || x >= c.x + c.w || y < c.y || y >= c.y + c.h) continue;
        return i;
    }
    return -1;
}

void LinuxPanel::invalidateParam(int id) {
    for (const auto &c : controls_)
        if (c.id == id) touch(bounds(c));
    if (id == 38 || id == 73 || id == 93 || id == 94) touch({548, 505, 216, 28}); // the voice grid
}

void LinuxPanel::setValueOnce(int id, int v) {
    if (!hooks_ || !hooks_->paramInfo || !hooks_->beginEdit || !hooks_->setValue || !hooks_->endEdit) return;
    int low = 0, high = 127, def = 0;
    hooks_->paramInfo(hooks_->ctx, id, &low, &high, &def);
    v = std::clamp(v, low, high);
    const bool changed = v != value(id); // SynthModel.set: no change, no readout
    hooks_->beginEdit(hooks_->ctx, id);
    hooks_->setValue(hooks_->ctx, id, v);
    hooks_->endEdit(hooks_->ctx, id);
    if (id >= 0 && id < int(knownValues_.size())) knownValues_[size_t(id)] = value(id);
    invalidateParam(id);
    if (changed) show(id);
}

void LinuxPanel::cycle(int id, int step) {
    int low = 0, high = 127, def = 0;
    if (hooks_ && hooks_->paramInfo) hooks_->paramInfo(hooks_->ctx, id, &low, &high, &def);
    const int current = value(id);
    int next = s2::OfferedStep(id, current, step); // skips values no control offers
    if (next == current) {                         // at an end: wrap around
        const int wrap = step > 0 ? low : high;
        next = s2::Offered(id, wrap) ? wrap : s2::OfferedStep(id, wrap, step);
    }
    setValueOnce(id, next);
}

void LinuxPanel::show(int id) {
    if (!hooks_) return;
    const std::string name = hooks_->displayName ? hooks_->displayName(hooks_->ctx, id) : "";
    flashText_ = name + ":  " + formatted(id);
    flashUntil_ = nowMs() + 1500;
    touch(lcdRect());
}

int LinuxPanel::chromeAt(float x, float y) const {
    static constexpr int bar[] = {3, 4, 5, 6}; // the bottom bar
    for (const int id : bar) {
        const PanelRect r = chromeRect(id);
        if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) return id;
    }
    if (file_.mode != FileMode::None) {
        for (const int id : {10, 14, 15}) { // accept, Cancel, Hidden
            const PanelRect r = id == 15 ? PanelRect{498, 38, 96, 20} : chromeRect(id);
            if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) return id;
        }
    }
    if (themeDialog_) {
        for (const int id : {14, 22, 23}) { // Close, Save, Delete
            PanelRect r = chromeRect(id);
            if (id == 22) r = {466, 366, 70, 20};
            if (id == 23) r = {544, 366, 70, 20};
            if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) return id;
        }
    }
    if (browser_) {
        for (const int id : {9, 10, 11, 12, 13, 14}) { // Write patch, New bank, Import, Library, Refresh, Close
            const PanelRect r = chromeRect(id);
            if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) return id;
        }
    }
    return 0;
}

void LinuxPanel::setHover(float x, float y) {
    if (menu_.open) {
        menuMove(x, y);
        return;
    }
    const int control = hit(x, y);
    const int chrome = chromeAt(x, y);
    if (control == hovered_ && chrome == hoveredChrome_) return;
    if (hovered_ >= 0 && hovered_ < int(controls_.size())) touch(bounds(controls_[size_t(hovered_)]));
    for (const int id : {hoveredChrome_, chrome})
        if (id) touch(chromeRect(id));
    hovered_ = control;
    hoveredChrome_ = chrome;
}

void LinuxPanel::rightClick(float x, float y) {
    if (menu_.open) {
        closeMenu();
        return;
    }
    if (file_.mode != FileMode::None || confirm_.open || !browser_) return;
    if (y < 64 || y > 469) return;
    if (x >= 7 && x < 196) {
        const int bank = bankRowAt(x, y);
        if (bank >= 0 && bank < int(banks_.size())) showLibraryMenu(x, y, bank, -1);
        return;
    }
    if (const int slot = slotAt(x, y); slot >= 0) {
        const bool valid = selectedBank_ >= 0 && selectedBank_ < int(banks_.size()) &&
                           !banks_[size_t(selectedBank_)].slots[size_t(slot)].empty();
        if (valid) showLibraryMenu(x, y, selectedBank_, slot);
    }
}

void LinuxPanel::mouseDown(float x, float y, bool doubleClick) {
    if (confirm_.open) {
        confirmClick(x, y);
        return;
    }
    if (menu_.open) {
        menuClick(x, y);
        return;
    }
    if (file_.mode != FileMode::None) {
        fileClick(x, y, doubleClick);
        return;
    }
    if (themeDialog_) {
        themeClick(x, y, doubleClick);
        return;
    }
    // The browser covers the panel area: its header and rows take the click.
    if (browser_ && x >= 6 && x < 764 && y >= 34 && y < 478) {
        browserClick(x, y, doubleClick);
        return;
    }
    // Learn mode: a hotspot takes the click (the controls under it stop
    // taking input); the volume knob still works.
    if (midiLearn_) {
        if (x >= 558 && x < 744 && y >= 488 && y < 512) { // "Exit MIDI Learn"
            setLearn(false);
            return;
        }
        if (const int id = learnHit(x, y); id >= 0) {
            if (doubleClick) {
                // The first click of the pair armed it: the mac's double tap
                // leaves the selection as it was.
                if (learnCC_[size_t(id)] && hooks_ && hooks_->setMidiCC) {
                    hooks_->setMidiCC(hooks_->ctx, id, 0);
                    flashText_ = std::string("MIDI LEARN: ") +
                                 (hooks_->paramName ? hooks_->paramName(hooks_->ctx, id) : "") + " cleared";
                    flashUntil_ = nowMs() + 1500;
                    touch(lcdRect());
                }
                learnCC_[size_t(id)] = 0;
                learnSelect(learnPrevTarget_);
                touch(learnRects_[size_t(id)]);
            } else {
                learnPrevTarget_ = learnTarget_;
                learnSelect(id);
            }
            return;
        }
    }
    // The title opens the logo menu (UI size, themes); the version box opens
    // the project page (SynthPanelView.projectURL).
    if (y < 32 && x < 100) {
        openLogoMenu();
        return;
    }
    if (x >= versionLeft_ && x < 762.f && y >= 2.f && y < 31.f) {
        openWithDesktop("https://github.com/chab/oxo_synth2");
        return;
    }
    // The bottom bar: write / menu, < > and the LCD (browser).
    if (y >= 484 && y <= 554) {
        if (x >= 492 && x < 542) {
            // write again while the write dialog is open = its Write (save in
            // place: click, click).
            if (y < 518) {
                if (browser_ && writing_) commitWrite();
                else openBrowser(true);
            } else {
                openMainMenu();
            }
            return;
        }
        if (x >= 420 && x < 473 && y < 520) {
            stepPatch(x < 448 ? -1 : 1);
            return;
        }
        if (x >= 50 && x < 482) {
            browser_ ? closeBrowser() : openBrowser();
            return;
        }
    }
    const int index = hit(x, y);
    if (index < 0) return;
    const Control &c = controls_[size_t(index)];
    // Only knobs and non-cycling value displays reset on a double click,
    // exactly as in SwiftUI: cycle buttons, LEDs, waveforms and toggles treat
    // every fast click as another normal selection.
    const bool doubleTapResets = c.kind == Kind::Knob ||
                                 (c.kind == Kind::Display && c.id != 64 && c.id != 78 && c.id != 82);
    if (doubleClick && doubleTapResets) {
        int a = 0, b = 127, d = 0;
        if (hooks_ && hooks_->paramInfo) hooks_->paramInfo(hooks_->ctx, c.id, &a, &b, &d);
        setValueOnce(c.id, d);
        return;
    }
    // The choice displays open a grid, never a value drag: the effect type
    // (2 x 5), the ctrl sources and the ctrl assignments.
    if (c.kind == Kind::Display) {
        if (c.id == 78 || c.id == 64 || c.id == 82) { // type displays
            openTypeMenu(c.id, c.x, c.y + c.h);
            return;
        }
        if (c.id == 86 || c.id == 88) {
            openSourceMenu(c.id);
            return;
        }
        if (c.id == 87 || c.id == 89) {
            openAssignMenu(c.id);
            return;
        }
    }
    switch (c.kind) {
    case Kind::Knob:
    case Kind::Display:
        dragId_ = c.id;
        dragStartY_ = y;
        dragStartValue_ = value(c.id);
        if (hooks_ && hooks_->beginEdit) hooks_->beginEdit(hooks_->ctx, c.id);
        return;
    case Kind::Toggle:
    case Kind::Badge:
        setValueOnce(c.id, value(c.id) ? 0 : 1);
        return;
    case Kind::Cycle:
        cycle(c.id, 1);
        return;
    case Kind::Wave:
        if (c.id == 42 || c.id == 47) cycle(c.id, 1);
        else setValueOnce(c.id, c.value);
        return;
    case Kind::Radio:
        setValueOnce(c.id, c.value);
        return;
    default:
        return;
    }
}

void LinuxPanel::mouseMove(float x, float y, bool fine) {
    if (press_.active) {
        dragMove(x, y);
        return;
    }
    if (dragId_ < 0) setHover(x, y); // no hover changes mid-drag
    if (dragId_ < 0) return;
    int low = 0, high = 127, def = 0;
    if (hooks_ && hooks_->paramInfo) hooks_->paramInfo(hooks_->ctx, dragId_, &low, &high, &def);
    const int span = std::max(1, high - low);
    const float pixels = fine ? 900.f : 180.f; // a full range per 180 px (900 with Shift)
    int next = std::clamp(dragStartValue_ + int(std::lround((dragStartY_ - y) * span / pixels)), low, high);
    if (!s2::Offered(dragId_, next)) // e.g. chorus type 3: snap to a neighbour
        next = s2::OfferedStep(dragId_, next, next > value(dragId_) ? 1 : -1);
    if (next == value(dragId_)) return;
    if (hooks_ && hooks_->setValue) hooks_->setValue(hooks_->ctx, dragId_, next);
    if (dragId_ >= 0 && dragId_ < int(knownValues_.size())) knownValues_[size_t(dragId_)] = value(dragId_);
    invalidateParam(dragId_);
    show(dragId_);
}

void LinuxPanel::mouseUp(float x, float y) {
    if (press_.active) {
        dragEnd(true);
        return;
    }
    if (dragId_ < 0) return;
    if (hooks_ && hooks_->endEdit) hooks_->endEdit(hooks_->ctx, dragId_);
    dragId_ = -1;
}

bool LinuxPanel::wheel(int direction, float x, float y) {
    if (direction == 0) return false;
    if (file_.mode != FileMode::None) return fileWheel(direction);
    const int index = hit(x, y);
    if (index < 0) return false;
    const Control &c = controls_[size_t(index)];
    // Buttons are click-only: scrolling over them changes nothing.
    if (c.kind == Kind::Toggle || c.kind == Kind::Cycle || c.kind == Kind::Badge || c.kind == Kind::Label)
        return false;
    const int id = c.id;
    const int current = value(id);
    const int listStep = direction > 0 ? -1 : 1;  // down = next item, as in menus
    const int numericStep = direction > 0 ? 1 : -1; // up = increase
    if (c.kind == Kind::Radio) {
        // Vertical LED lists (filter / arp type and range, LFO destination,
        // voice mode) do not take the wheel; the horizontal ones (sub octave,
        // modulation destination) step.
        if (!(id == 71 || id == 97)) return false;
        cycle(id, listStep);
        return true;
    }
    if (c.kind == Kind::Wave) {
        cycle(id, listStep); // waveform selectors wrap around
        return true;
    }
    if (c.kind == Kind::Knob) {
        int low = 0, high = 127, def = 0;
        if (hooks_ && hooks_->paramInfo) hooks_->paramInfo(hooks_->ctx, id, &low, &high, &def);
        const int next = std::clamp(current + numericStep, low, high);
        if (next != current) setValueOnce(id, next);
        return true;
    }
    if (c.kind == Kind::Display) {
        if (id == 64 || id == 78 || id == 82) { // type displays advance down their lists
            cycle(id, listStep);
            return true;
        }
        int low = 0, high = 127, def = 0;
        if (hooks_ && hooks_->paramInfo) hooks_->paramInfo(hooks_->ctx, id, &low, &high, &def);
        const int next = std::clamp(current + numericStep, low, high);
        if (next != current) setValueOnce(id, next);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Polling (30 Hz): host automation, patch loads, flash expiry
// ---------------------------------------------------------------------------

bool LinuxPanel::poll(uint64_t nowMsValue) {
    bool changed = false;
    for (int id = 0; id < int(knownValues_.size()); ++id) {
        const int now = value(id);
        if (now == knownValues_[size_t(id)]) continue;
        knownValues_[size_t(id)] = now;
        invalidateParam(id);
        changed = true;
    }
    // SynthModel.incomingCC: every new CC is consumed (none learned while
    // nothing is armed), CC 1..127 only; the control stays armed.
    if (midiLearn_ && hooks_ && hooks_->lastCC) {
        const int token = hooks_->lastCC(hooks_->ctx);
        if (token != lastCcToken_ && token > 0) {
            lastCcToken_ = token;
            const int cc = token & 127;
            if (learnTarget_ >= 0 && learnTarget_ < kLearnCount && cc >= 1 && learnCC_[size_t(learnTarget_)] != cc &&
                hooks_->setMidiCC) {
                hooks_->setMidiCC(hooks_->ctx, learnTarget_, cc);
                learnCC_[size_t(learnTarget_)] = cc;
                touch(learnRects_[size_t(learnTarget_)]);
                flashText_ = std::string("MIDI LEARN: ") +
                             (hooks_->paramName ? hooks_->paramName(hooks_->ctx, learnTarget_) : "") +
                             " = CC " + std::to_string(cc);
                flashUntil_ = nowMs() + 1500;
                touch(lcdRect());
                changed = true;
            }
        }
    }
    const int revision = hooks_ && hooks_->libraryRevision ? hooks_->libraryRevision(hooks_->ctx) : 0;
    if (revision != knownRevision_) {
        knownRevision_ = revision;
        if (revision > 0) refreshBanks();
        changed = true;
    }
    // The voice grid shows the engine's voice states (output params 1301..1304),
    // which are not among the 99 patch values: poll them separately.
    if (!midiLearn_ && hooks_ && hooks_->voiceStates) {
        const uint64_t states = hooks_->voiceStates(hooks_->ctx);
        if (states != knownVoiceStates_) {
            knownVoiceStates_ = states;
            touch({548, 505, 216, 28});
            changed = true;
        }
    }
    const int serial = hooks_ && hooks_->patchSerial ? hooks_->patchSerial(hooks_->ctx) : 0;
    if (serial != knownSerial_) {
        knownSerial_ = serial;
        touch({0, 0, kDesignWidth, kDesignHeight}); // a load changes everything
        changed = true;
    }
    if (flashUntil_ && nowMsValue >= flashUntil_) {
        flashUntil_ = 0;
        flashText_.clear();
        touch(lcdRect());
        changed = true;
    }
    if (!dirty_.empty()) changed = true;
    return changed;
}





// ---------------------------------------------------------------------------
// Browser drag and drop (PatchBrowserView's SlotDrop / BankDrop), inside our
// own window: a patch dragged onto another slot of its bank MOVES there (an
// occupied one swaps; the files are renamed), onto a bank row COPIES into
// that bank's first free slot. Never into the read-only factory bank.
// ---------------------------------------------------------------------------

PanelRect LinuxPanel::dragLabelRect(Canvas &cr) const {
    // slotDraggable's preview: the "NNN:name" label just right of the pointer.
    const float w = float(cr.textWidth(press_.label, 10.2f, true)) + 8.f, h = 16.f;
    return {std::min(press_.x + 12.f, kDesignWidth - w - 4.f), std::min(press_.y + 12.f, kDesignHeight - h - 4.f),
            w, h};
}

void LinuxPanel::dragMove(float x, float y) {
    if (!press_.active) return;
    if (!press_.dragging) {
        if (std::fabs(x - press_.startX) < 4.f && std::fabs(y - press_.startY) < 4.f) return;
        press_.dragging = true;
    }
    press_.x = x;
    press_.y = y;
    int slot = -1, bank = -1;
    const bool sameBank = press_.bank == selectedBank_ && press_.bank >= 0 && press_.bank < int(banks_.size());
    if (const int s = slotAt(x, y); s >= 0 && sameBank && !banks_[size_t(press_.bank)].readOnly) slot = s;
    else if (const int b = bankRowAt(x, y); b >= 0 && b < int(banks_.size()) && !writing_ &&
                                              !banks_[size_t(b)].readOnly) {
        // a full bank refuses the copy
        const auto &slots = banks_[size_t(b)].slots;
        if (std::any_of(slots.begin(), slots.end(), [](const std::string &s2) { return s2.empty(); })) bank = b;
    }
    if (slot != dropSlot_ || bank != dropBank_) {
        dropSlot_ = slot;
        dropBank_ = bank;
    }
    touch(browserRect());
    touch({press_.x + 12.f, press_.y + 12.f, 220.f, 18.f}); // a conservative label area
}

void LinuxPanel::dragEnd(bool drop) {
    if (!press_.active) return;
    const SlotPress press = press_;
    const int toSlot = dropSlot_, toBank = dropBank_;
    press_.active = false;
    dropSlot_ = dropBank_ = -1;
    touch(browserRect());
    if (!press.dragging) { // a plain click
        if (drop && press.bank == selectedBank_) slotClick(press.slot, false);
        return;
    }
    if (!drop) return;
    // The indices as of now: a scan may have landed during the drag.
    if (hooks_ && hooks_->libraryRevision && hooks_->libraryRevision(hooks_->ctx) != knownRevision_)
        adoptLibrary();
    const int from = bankBySource(press.source);
    if (from < 0) return;
    if (toSlot >= 0 && toSlot != press.slot) { // SynthModel.movePatch
        if (!hooks_ || !hooks_->movePatch || !hooks_->movePatch(hooks_->ctx, from, press.slot, toSlot)) return;
        adoptLibrary();
        char moved[32];
        std::snprintf(moved, sizeof moved, "moved %03d -> %03d", press.slot + 1, toSlot + 1);
        flashText_ = moved;
        flashUntil_ = nowMs() + 1500;
        touch(lcdRect());
    } else if (toBank >= 0 && toBank < int(banks_.size()) && hooks_ && hooks_->copyPatch) { // SynthModel.copyPatch
        const std::string target = banks_[size_t(toBank)].source;
        const int to = bankBySource(target);
        if (to < 0) return;
        const std::string name = banks_[size_t(to)].name;
        const int number = displayBankNumber(to);
        const int slot = hooks_->copyPatch(hooks_->ctx, from, press.slot, to);
        if (slot < 0) {
            flashText_ = slot == -1 ? "Bank is full (128 patches): cannot add a patch" : "Cannot copy that patch.";
            flashUntil_ = nowMs() + 3000;
            touch(lcdRect());
            return;
        }
        adoptLibrary();
        char copied[160];
        std::snprintf(copied, sizeof copied, "copied to %02d:%s %03d", number, name.c_str(), slot + 1);
        flashText_ = copied;
        flashUntil_ = nowMs() + 1500;
        touch(lcdRect());
    }
}

// The library menu (right click): Export bank..., Show in library, Delete.
void LinuxPanel::showLibraryMenu(float x, float y, int bank, int slot) {
    menu_.open = true;
    menu_.items.clear();
    if (slot < 0 && bank >= 0 && bank < int(banks_.size())) { // a bank row
        menu_.items.push_back({"Export bank...", MenuAction::ExportBank, bank, 0, false, false});
        menu_.items.push_back({"Show in library", MenuAction::ShowInLibrary, bank, -1, false, false});
        if (!banks_[size_t(bank)].readOnly)
            menu_.items.push_back({"Delete bank...", MenuAction::DeleteBank, bank, -1, false, false});
    } else if (slot >= 0 && bank >= 0 && bank < int(banks_.size())) { // a patch
        menu_.items.push_back({"Show in library", MenuAction::ShowInLibrary, bank, slot, false, false});
        if (!banks_[size_t(bank)].readOnly)
            menu_.items.push_back({"Delete", MenuAction::DeletePatch, bank, slot, false, false});
    }
    if (menu_.items.empty()) {
        menu_.open = false;
        return;
    }
    menu_.columns = 1;
    menu_.w = 168 + 12;
    menu_.h = float(menu_.items.size()) * 18 + 12;
    menu_.x = std::clamp(x, 4.f, kDesignWidth - menu_.w - 4.f);
    menu_.y = std::clamp(y, 4.f, kDesignHeight - menu_.h - 4.f);
    menu_.hover = -1;
    touch({0, 0, kDesignWidth, kDesignHeight});
}

void LinuxPanel::askDelete(int bank, int slot) {
    confirm_.open = true;
    confirm_.bank = bank;
    confirm_.slot = slot;
    confirm_.action = slot < 0 ? MenuAction::DeleteBank : MenuAction::DeletePatch;
    if (slot < 0 && bank >= 0 && bank < int(banks_.size())) {
        confirm_.text = "Delete the bank \"" + banks_[size_t(bank)].name + "\"?";
    } else if (slot >= 0 && bank >= 0 && bank < int(banks_.size())) {
        confirm_.text = "Delete " + slotText(bank, slot) + "?";
    }
    touch({0, 0, kDesignWidth, kDesignHeight});
}

void LinuxPanel::drawConfirm(Canvas &cr) {
    if (!confirm_.open) return;
    const auto &p = colors_;
    const float w = 420, h = 110;
    const float x = (kDesignWidth - w) * .5f, y = (kDesignHeight - h) * .5f;
    cr.fillRect(blend(p[kPanel], 0x000000, .35f), x + 3, y + 3, w, h);
    cr.fillRect(p[kDisplayBottom], x, y, w, h);
    cr.roundedRing(p[kHeader], x, y, w, h, 3.f);
    cr.text(confirm_.text, x + 12, y + 14, w - 24, 22, 11, kLcdText, false, false);
    darkButton(cr, 0xB81715, x + w - 180, y + h - 38, 76, 22, hoveredChrome_ == 20);
    cr.text("Delete", x + w - 180, y + h - 38, 76, 22, 10.5f, 0xFFFFFF, true, true);
    darkButton(cr, p[kButton], x + w - 96, y + h - 38, 76, 22, hoveredChrome_ == 21);
    cr.text("Cancel", x + w - 96, y + h - 38, 76, 22, 10.5f, 0xFFFFFF, true, true);
}

bool LinuxPanel::confirmClick(float x, float y) {
    if (!confirm_.open) return false;
    const float w = 420, h = 110;
    const float bx = (kDesignWidth - w) * .5f, by = (kDesignHeight - h) * .5f;
    const bool deleteHit = x >= bx + w - 180 && x < bx + w - 104 && y >= by + h - 38 && y < by + h - 16;
    const bool cancelHit = x >= bx + w - 96 && x < bx + w - 20 && y >= by + h - 38 && y < by + h - 16;
    if (cancelHit || (!deleteHit && !(x >= bx && x < bx + w && y >= by && y < by + h))) {
        confirm_.open = false;
        touch({0, 0, kDesignWidth, kDesignHeight});
        return true;
    }
    if (!deleteHit) return true; // a click inside the box: nothing
    const MenuAction action = confirm_.action;
    const int bank = confirm_.bank, slot = confirm_.slot;
    confirm_.open = false;
    touch({0, 0, kDesignWidth, kDesignHeight});
    if (action == MenuAction::DeleteBank) {
        if (hooks_ && hooks_->deleteBank && hooks_->deleteBank(hooks_->ctx, bank)) {
            if (selectedBank_ == bank) selectedBank_ = -1;
            adoptLibrary();
            flashText_ = "bank deleted";
            flashUntil_ = nowMs() + 1500;
            touch(lcdRect());
        }
    } else if (action == MenuAction::DeletePatch) {
        if (hooks_ && hooks_->deleteBank && bank >= 0 && bank < int(banks_.size())) {
            // A patch delete goes through the bank folder's file: the hook
            // deletes one slot by writing an empty bank? No: use the trash on
            // the file itself, the same contract as the mac's slot delete.
            const char *path = hooks_->slotPath ? hooks_->slotPath(hooks_->ctx, bank, slot) : nullptr;
            if (path && *path) {
                // The mac moves the .sy1 to the Trash; where there is none
                // (another filesystem) the caller deletes, as Trash.hpp says.
                std::error_code ec;
                if (!MoveToTrash(path)) std::filesystem::remove(path, ec);
                adoptLibrary();
                flashText_ = "patch deleted";
                flashUntil_ = nowMs() + 1500;
                touch(lcdRect());
            }
        }
    }
    return true;
}


// ---------------------------------------------------------------------------
// Theme editor (ThemeDialog): the 13 ThemeColor slots, edited as text. No
// colour picker: a slot is picked by clicking its row, its value typed as
// #RRGGBB in the field and applied live while it parses.
// ---------------------------------------------------------------------------

void LinuxPanel::openThemeEditor() {
    if (browser_) closeBrowser();
    themeDialog_ = true;
    themeRow_ = -1;
    themeName_ = "";
    themeStore().Load();
    focusField(EditTarget::None, "", false);
    touch({0, 0, kDesignWidth, kDesignHeight});
}

void LinuxPanel::closeThemeEditor() {
    if (!themeDialog_) return;
    themeDialog_ = false;
    themeRow_ = -1;
    focusField(EditTarget::None, "", false);
    touch({0, 0, kDesignWidth, kDesignHeight});
}

void LinuxPanel::focusField(EditTarget target, const std::string &text, bool selectAll) {
    editTarget_ = target;
    editText_ = text;
    caret_ = text.size();
    nameSelected_ = selectAll;
}

void LinuxPanel::applyThemeColor() {
    if (themeRow_ < 0 || themeRow_ >= int(themes::kSlotCount)) return;
    // Accept "RRGGBB" and "#RRGGBB"; anything else leaves the palette alone.
    std::string hex = editText_;
    if (!hex.empty() && hex[0] == '#') hex.erase(0, 1);
    if (hex.size() != 6) return;
    uint32_t rgb = 0;
    if (!themes::ParseHex(hex, rgb)) return;
    if (theme_[size_t(themeRow_)] == rgb) return;
    themes::Palette palette = theme_;
    palette[size_t(themeRow_)] = rgb;
    setTheme(palette);
    touch({0, 0, kDesignWidth, kDesignHeight});
}

void LinuxPanel::saveTheme() {
    std::string name = themeName_;
    const auto first = name.find_first_not_of(" \t"), last = name.find_last_not_of(" \t");
    if (first == std::string::npos) {
        flashText_ = "Give the theme a name first.";
        flashUntil_ = nowMs() + 3000;
        touch(lcdRect());
        return;
    }
    name = name.substr(first, last - first + 1);
    themeStore().Save(theme_, name);
    themeStore().Write();
    flashText_ = "theme \"" + name + "\" saved";
    flashUntil_ = nowMs() + 3000;
    touch(lcdRect());
    touch({0, 0, kDesignWidth, kDesignHeight});
}

void LinuxPanel::deleteTheme() {
    std::string name = themeName_;
    const auto first = name.find_first_not_of(" \t"), last = name.find_last_not_of(" \t");
    if (first == std::string::npos) return;
    name = name.substr(first, last - first + 1);
    if (themeStore().Delete(name)) {
        flashText_ = "theme \"" + name + "\" deleted";
        flashUntil_ = nowMs() + 3000;
        touch(lcdRect());
        touch({0, 0, kDesignWidth, kDesignHeight});
    } else {
        flashText_ = "no saved theme named \"" + name + "\"";
        flashUntil_ = nowMs() + 3000;
        touch(lcdRect());
    }
}

PanelRect LinuxPanel::fieldRect() const {
    switch (editTarget_) {
    case EditTarget::ThemeColor: return themeColorRect();
    case EditTarget::ThemeName: return themeNameRect();
    case EditTarget::PatchName: return {300, 38, 200, 19};
    case EditTarget::FileName: return {300, 38, 200, 19};
    case EditTarget::None: return {};
    }
    return {};
}

void LinuxPanel::drawThemeEditor(Canvas &cr) {
    const auto &p = colors_;
    const float x = 6, y = 34, w = 758, h = 444;
    cr.fillRect(p[kPanel], x, y, w, h);
    cr.rectOutline(p[kHeader], x, y, w, h, 1.5f);
    cr.fillRect(p[kHeader], x + 1, y + 1, w - 2, 27);
    cr.text("Themes", x + 7, y + 2, 200, 24, 12, p[kHeaderText], true);
    darkButton(cr, p[kButton], 690, 38, 64, 20, hoveredChrome_ == 14);
    cr.text("Close", 690, 38, 64, 20, 10, 0xFFFFFF, true, true);
    cr.fillRect(p[kDisplayBottom], x + 1, y + 28, w - 2, h - 30);
    // The 13 slots: title, swatch, hex value.
    for (int i = 0; i < int(themes::kSlotCount); ++i) {
        const float ry = 66 + float(i) * 22;
        const bool selected = i == themeRow_;
        if (selected) cr.fillRect(p[kDisplayText], 10, ry, w - 20, 20);
        const uint32_t ink = selected ? p[kDisplayBottom] : kLcdText;
        cr.text(themes::kTitles[size_t(i)], 16, ry, 130, 20, 10.5f, ink, false, false);
        const uint32_t colour = theme_[size_t(i)] & 0xFFFFFF;
        cr.fillRect(colour, 160, ry + 2, 26, 16);
        cr.roundedRing(0x000000, 160, ry + 2, 26, 16, 2.f);
        cr.text("#" + themes::Hex(colour), 196, ry, 90, 20, 10.5f, ink, false, false, true);
    }
    // Fields and buttons.
    const float fy = 366;
    cr.text("Colour", 16, fy, 52, 20, 10.5f, kLcdText, false, false);
    cr.fillRect(p[kDisplayBottom], themeColorRect().x, fy, themeColorRect().w, themeColorRect().h);
    cr.roundedRing(0x0B0B0B, themeColorRect().x, fy, themeColorRect().w, themeColorRect().h, 2.f);
    if (editTarget_ == EditTarget::ThemeColor) {
        cr.text(editText_, themeColorRect().x + 3, fy + 1, themeColorRect().w - 6, themeColorRect().h - 2, 10.5f,
             kLcdText, false, false, true);
        const float caretX = themeColorRect().x + 3 + float(cr.textWidth(editText_.substr(0, caret_), 10.5f, true));
        cr.line(kLcdText, caretX, fy + 4, caretX, fy + themeColorRect().h - 4);
    } else {
        cr.text(themeRow_ >= 0 ? "#" + themes::Hex(theme_[size_t(themeRow_)] & 0xFFFFFF) : "#RRGGBB",
             themeColorRect().x + 3, fy + 1, themeColorRect().w - 6, themeColorRect().h - 2, 10.5f, kLcdText,
             false, false, true);
    }
    cr.text("Name", 200, fy, 36, 20, 10.5f, kLcdText, false, false);
    cr.fillRect(p[kDisplayBottom], themeNameRect().x, fy, themeNameRect().w, themeNameRect().h);
    cr.roundedRing(0x0B0B0B, themeNameRect().x, fy, themeNameRect().w, themeNameRect().h, 2.f);
    if (editTarget_ == EditTarget::ThemeName) {
        cr.text(editText_, themeNameRect().x + 3, fy + 1, themeNameRect().w - 6, themeNameRect().h - 2, 10.5f,
             kLcdText, false, false, true);
        const float caretX = themeNameRect().x + 3 + float(cr.textWidth(editText_.substr(0, caret_), 10.5f, false));
        cr.line(kLcdText, caretX, fy + 4, caretX, fy + themeNameRect().h - 4);
    } else {
        cr.text(themeName_.empty() ? "theme name" : themeName_, themeNameRect().x + 3, fy + 1,
             themeNameRect().w - 6, themeNameRect().h - 2, 10.5f, kLcdText, false, false, true);
    }
    darkButton(cr, p[kButton], 466, fy, 70, 20, hoveredChrome_ == 22);
    cr.text("Save", 466, fy, 70, 20, 10, 0xFFFFFF, true, true);
    darkButton(cr, p[kButton], 544, fy, 70, 20, hoveredChrome_ == 23);
    cr.text("Delete", 544, fy, 70, 20, 10, 0xFFFFFF, true, true);
    cr.text("Pick a row, type #RRGGBB, Enter applies. Save stores it as a named theme.",
         16, 400, w - 32, 20, 10, blend(p[kDisplayBottom], kLcdText, .55f), false, false);
}

bool LinuxPanel::themeClick(float x, float y, bool doubleClick) {
    (void)doubleClick;
    if (!themeDialog_) return false;
    if (y >= 37 && y <= 61) {
        if (x >= 690 && x < 760) closeThemeEditor();
        return true;
    }
    // A colour row.
    if (x >= 10 && x < 758 && y >= 66 && y < 66 + float(themes::kSlotCount) * 22) {
        const int row = int((y - 66) / 22);
        if (row >= 0 && row < int(themes::kSlotCount)) {
            themeRow_ = row;
            focusField(EditTarget::ThemeColor, themes::Hex(theme_[size_t(row)] & 0xFFFFFF), true);
            touch({0, 0, kDesignWidth, kDesignHeight});
        }
        return true;
    }
    const PanelRect colour = themeColorRect(), name = themeNameRect();
    if (x >= colour.x && x < colour.x + colour.w && y >= colour.y && y < colour.y + colour.h) {
        focusField(EditTarget::ThemeColor,
                   themeRow_ >= 0 ? themes::Hex(theme_[size_t(themeRow_)] & 0xFFFFFF) : std::string(), false);
        touch({0, 0, kDesignWidth, kDesignHeight});
        return true;
    }
    if (x >= name.x && x < name.x + name.w && y >= name.y && y < name.y + name.h) {
        focusField(EditTarget::ThemeName, themeName_, false);
        touch({0, 0, kDesignWidth, kDesignHeight});
        return true;
    }
    if (y >= 366 && y < 386) {
        if (x >= 466 && x < 536) { // Save
            saveTheme();
            return true;
        }
        if (x >= 544 && x < 614) { // Delete
            deleteTheme();
            return true;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// The in-panel file dialog (no toolkit chooser, no portal dependency):
// ".." and the current folder's sub-folders, then the files the mode wants.
// Click selects, a double click enters a folder or accepts a file; Return
// accepts, Esc cancels.
// ---------------------------------------------------------------------------

namespace {

bool hasSy1Suffix(const std::string &name) {
    if (name.size() < 4) return false;
    std::string tail = name.substr(name.size() - 4);
    for (auto &c : tail) c = char(std::tolower((unsigned char)c));
    return tail == ".sy1";
}

// Runs a program: a new session and /dev/null on all three descriptors, so the
// launcher's own noise (Qt / AT-SPI warnings, file manager chatter) never
// reaches the host's log, and a closed host does not take it down. posix_spawnp,
// not fork: the host is multithreaded.
bool spawnProgram(const std::vector<std::string> &argv) {
    if (argv.empty()) return false;
    std::vector<char *> args;
    args.reserve(argv.size() + 1);
    for (const auto &a : argv) args.push_back(const_cast<char *>(a.c_str()));
    args.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDWR, 0);
    posix_spawn_file_actions_adddup2(&actions, STDIN_FILENO, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, STDIN_FILENO, STDERR_FILENO);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
    pid_t pid = 0;
    const int rc = posix_spawnp(&pid, args[0], &actions, &attr, args.data(), environ);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    return rc == 0;
}

// Opens a path with the desktop's handler (xdg-open): the file manager for
// "Library folder", the browser for the project page. No shell.
void openWithDesktop(const std::string &target) { spawnProgram({"xdg-open", target}); }

// "Show in library": the file manager selects the item and comes to the front
// (FileManager1 on D-Bus: Nautilus, Dolphin, Nemo, Thunar...), else its folder
// opens. xdg-open alone opens the folder behind the host, so only the taskbar
// entry changes.
void revealInFileManager(const std::string &path) {
    if (path.empty()) return;
    static const char *hex = "0123456789ABCDEF";
    std::string uri = "file://";
    for (unsigned char c : path) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') uri += char(c);
        else { uri += '%'; uri += hex[c >> 4]; uri += hex[c & 15]; }
    }
    if (spawnProgram({"dbus-send", "--session", "--print-reply", "--dest=org.freedesktop.FileManager1",
                      "/org/freedesktop/FileManager1", "org.freedesktop.FileManager1.ShowItems",
                      "array:string:" + uri, "string:"}))
        return;
    std::string folder = path; // no dbus-send: open the folder
    std::error_code ec;
    if (!std::filesystem::is_directory(path, ec)) {
        const size_t slash = folder.find_last_of('/');
        if (slash != std::string::npos) folder = folder.substr(0, slash ? slash : 1);
    }
    openWithDesktop(folder);
}

} // namespace

void LinuxPanel::openFileDialog(FileMode mode) {
    file_ = FileState{};
    // The "Hidden" toggle is a per-user preference (settings.json).
    if (const auto token = settings::Load(storage::Root(), "showHiddenFiles"))
        showHidden_ = *token == "true" || *token == "1";
    file_.mode = mode;
    switch (mode) {
    case FileMode::OpenPatch:
        file_.title = "Open patch file";
        file_.accept = "Open";
        break;
    case FileMode::SavePatch:
        file_.title = "Save patch file";
        file_.accept = "Save";
        focusField(EditTarget::FileName, patchName(), true);
        break;
    case FileMode::Import:
        file_.title = "Import into the library";
        file_.accept = "Import";
        break;
    case FileMode::ExportBank:
        file_.title = "Export the bank into a folder";
        file_.accept = "Export here";
        break;
    case FileMode::None:
        return;
    }
    // Start where the patches are; the user can walk up from there.
    std::error_code ec;
    const std::filesystem::path library = storage::LibraryRoot();
    const std::filesystem::path home = storage::Root().parent_path().parent_path().parent_path();
    file_.dir = std::filesystem::is_directory(library, ec) ? library : home;
    if (!std::filesystem::is_directory(file_.dir, ec)) file_.dir = std::filesystem::path("/");
    refreshFiles();
}

void LinuxPanel::navigateTo(const std::filesystem::path &dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return;
    file_.dir = std::filesystem::weakly_canonical(dir, ec);
    if (ec) file_.dir = dir;
    file_.scroll = 0;
    file_.selected = -1;
    refreshFiles();
}

void LinuxPanel::refreshFiles() {
    file_.entries.clear();
    std::error_code ec;
    std::vector<std::string> dirs, files;
    for (std::filesystem::directory_iterator it(file_.dir, std::filesystem::directory_options::skip_permission_denied, ec), end;
         it != end && !ec; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.empty()) continue;
        if (name[0] == '.' && !showHidden_) continue; // the dialog's "Hidden" toggle
        std::error_code typeError;
        if (it->is_directory(typeError)) dirs.push_back(name + "/");
        else if (it->is_regular_file(typeError)) {
            const bool wanted = file_.mode == FileMode::OpenPatch ? hasSy1Suffix(name) : true;
            if (wanted) files.push_back(name);
        }
    }
    auto byName = [](const std::string &a, const std::string &b) {
        return naturalLess(a, b);
    };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(files.begin(), files.end(), byName);
    if (file_.dir != file_.dir.root_path()) file_.entries.push_back("../");
    file_.entries.insert(file_.entries.end(), dirs.begin(), dirs.end());
    file_.entries.insert(file_.entries.end(), files.begin(), files.end());
    if (file_.selected >= int(file_.entries.size())) file_.selected = -1;
    touch(browserRect());
}

void LinuxPanel::drawFileDialog(Canvas &cr) {
    const auto &p = colors_;
    const float x = 6, y = 34, w = 758, h = 444;
    constexpr float rowHeight = 18;
    cr.fillRect(p[kPanel], x, y, w, h);
    cr.rectOutline(p[kHeader], x, y, w, h, 1.5f);
    cr.fillRect(p[kHeader], x + 1, y + 1, w - 2, 27);
    cr.text(file_.title, x + 7, y + 2, 250, 24, 12, p[kHeaderText], true);
    if (file_.mode == FileMode::SavePatch) {
        // The name field, as in the write dialog.
        const float fx = 300, fy = 38, fw = 200, fh = 19;
        cr.fillRect(p[kDisplayBottom], fx, fy, fw, fh);
        cr.roundedRing(0x0B0B0B, fx, fy, fw, fh, 2.f);
        cr.text(editText_, fx + 3, fy + 1, fw - 6, fh - 2, 10.5f, kLcdText, false, false, true);
    }
    darkButton(cr, p[kButton], 498, 38, 96, 20, hoveredChrome_ == 15);
    cr.text(showHidden_ ? "Show hidden: on" : "Show hidden: off", 498, 38, 96, 20, 10, 0xFFFFFF, true, true);
    darkButton(cr, p[kButton], 604, 38, 74, 20, hoveredChrome_ == 10);
    cr.text(file_.accept, 604, 38, 74, 20, 10, 0xFFFFFF, true, true);
    darkButton(cr, p[kButton], 690, 38, 64, 20, hoveredChrome_ == 14);
    cr.text("Cancel", 690, 38, 64, 20, 10, 0xFFFFFF, true, true);
    cr.fillRect(p[kDisplayBottom], x + 1, y + 28, w - 2, h - 30);
    // The folder we are in.
    cr.text(file_.dir.string(), 13, 66, w - 26, 18, 10.5f, 0xC1DCE0, false, false, true);
    cr.line(p[kHeader], x + 8, 85.5f, x + w - 8, 85.5f);
    const int visible = 18;
    for (int i = 0; i < visible; ++i) {
        const int index = file_.scroll + i;
        if (index >= int(file_.entries.size())) break;
        const std::string &name = file_.entries[size_t(index)];
        const float ry = 88 + rowHeight * i;
        const bool selected = index == file_.selected;
        if (selected) cr.fillRect(p[kDisplayText], 10, ry, w - 20, rowHeight - 1);
        const bool folder = !name.empty() && name.back() == '/';
        cr.text(name, 14, ry, w - 28, rowHeight, 10.5f,
             selected ? p[kDisplayBottom] : folder ? 0xE9D56A : kLcdText, false, false, true);
    }
    if (file_.entries.empty()) cr.text("(empty)", 14, 88, w - 28, 18, 10.5f, kLcdText, false, false, true);
}

bool LinuxPanel::fileWheel(int direction) {
    if (file_.mode == FileMode::None) return false;
    const int rows = int(file_.entries.size());
    const int next = std::clamp(file_.scroll - direction, 0, std::max(0, rows - 18));
    if (next == file_.scroll) return false;
    file_.scroll = next;
    touch(browserRect());
    return true;
}

bool LinuxPanel::fileClick(float x, float y, bool doubleClick) {
    if (file_.mode == FileMode::None) return false;
    if (y >= 37 && y <= 61) {
        if (x >= 690 && x < 760) {
            closeFileDialog();
            return true;
        }
        if (x >= 498 && x < 594) { // the Show hidden toggle
            showHidden_ = !showHidden_;
            settings::Save(storage::Root(), "showHiddenFiles", showHidden_ ? "true" : "false");
            refreshFiles();
            return true;
        }
        if (x >= 604 && x < 681) {
            acceptFile();
            return true;
        }
        return true;
    }
    if (y < 88 || y >= 88 + 18 * 18) return true;
    const int index = file_.scroll + int((y - 88) / 18);
    if (index < 0 || index >= int(file_.entries.size())) return true;
    file_.selected = index;
    const std::string &name = file_.entries[size_t(index)];
    const bool folder = !name.empty() && name.back() == '/';
    touch(browserRect());
    if (!doubleClick) return true;
    if (folder) {
        navigateTo(name == "../" ? file_.dir.parent_path() : file_.dir / name.substr(0, name.size() - 1));
        return true;
    }
    acceptFile();
    return true;
}

void LinuxPanel::acceptFile() {
    if (file_.mode == FileMode::None) return;
    std::error_code ec;
    if (file_.mode == FileMode::SavePatch) {
        std::string name = editText_.empty() ? patchName() : editText_;
        if (name.empty()) return;
        std::filesystem::path target = file_.dir / name;
        if (!hasSy1Suffix(name)) target += ".sy1";
        const bool ok = hooks_ && hooks_->savePatchFile && hooks_->savePatchFile(hooks_->ctx, target.c_str());
        closeFileDialog();
        flashText_ = ok ? "saved " + target.filename().string() : "Cannot write that file.";
        flashUntil_ = nowMs() + 3000;
        touch(lcdRect());
        return;
    }
    if (file_.mode == FileMode::ExportBank) {
        // PatchManager::ExportBank: a folder named after the bank, created in
        // the folder the dialog is in.
        if (hooks_ && hooks_->exportBank) {
            const std::string created = hooks_->exportBank(hooks_->ctx, exportBank_, file_.dir.c_str());
            closeFileDialog();
            flashText_ = created.empty() ? "Cannot export that bank." : "exported to " + created;
            flashUntil_ = nowMs() + 3000;
            touch(lcdRect());
        }
        return;
    }
    if (file_.selected < 0 || file_.selected >= int(file_.entries.size())) return;
    const std::string name = file_.entries[size_t(file_.selected)];
    if (!name.empty() && name.back() == '/') { // a folder: enter it, then the user picks
        navigateTo(name == "../" ? file_.dir.parent_path() : file_.dir / name.substr(0, name.size() - 1));
        return;
    }
    const std::filesystem::path path = file_.dir / name;
    if (file_.mode == FileMode::OpenPatch) {
        const bool ok = hooks_ && hooks_->loadPatchFile && hooks_->loadPatchFile(hooks_->ctx, path.c_str());
        closeFileDialog();
        flashText_ = ok ? "loaded " + name : "Cannot read that patch.";
        flashUntil_ = nowMs() + 3000;
        touch(lcdRect());
        return;
    }
    if (file_.mode == FileMode::Import) {
        if (hooks_ && hooks_->importItems) {
            const std::string added = hooks_->importItems(hooks_->ctx, path.c_str());
            closeFileDialog();
            flashText_ = added.empty() ? "Nothing to import." : "imported into the library";
            flashUntil_ = nowMs() + 3000;
            touch(lcdRect());
            startLibraryScan();
            refreshBanks();
        }
        return;
    }
}

void LinuxPanel::closeFileDialog() {
    if (file_.mode == FileMode::None) return;
    file_ = FileState{};
    focusField(EditTarget::None, "", false);
    touch(browserRect());
    touch(lcdRect());
}

// ---------------------------------------------------------------------------
// MIDI learn: every patch control but the ctrl src / assign menus wears a
// hotspot around all its parts (a radio's LEDs, a wave grid). Dim border =
// not learned, bright 2 px border = armed, learn-colour fill + CC badge =
// learned. Click arms, double click unlearns; an armed control stays armed
// (the next CC replaces). Never survives the editor.
// ---------------------------------------------------------------------------

void LinuxPanel::buildLearnRects() {
    learnRects_.fill({});
    learnOrder_.clear();
    for (const auto &c : controls_) {
        if (c.id < 0 || c.id >= kLearnCount || (c.id >= 86 && c.id <= 89) || c.kind == Kind::Label) continue;
        PanelRect &r = learnRects_[size_t(c.id)];
        const PanelRect own{c.x, c.y, c.w, c.h};
        if (r.w <= 0) {
            r = own;
            continue;
        }
        r.unite(own);
    }
    for (int id = 0; id < kLearnCount; ++id) {
        PanelRect &r = learnRects_[size_t(id)];
        if (r.w <= 0) continue;
        r.inflate(3.f); // .padding(-3)
        learnOrder_.push_back(id);
    }
    // Drawn larger first, hit smaller first: a control inside a group's box
    // (e.g. beside a radio column) keeps its own hotspot.
    std::stable_sort(learnOrder_.begin(), learnOrder_.end(), [this](int a, int b) {
        const PanelRect &ra = learnRects_[size_t(a)], &rb = learnRects_[size_t(b)];
        return ra.w * ra.h > rb.w * rb.h;
    });
}

void LinuxPanel::setLearn(bool on) {
    if (on && browser_) closeBrowser(); // SynthModel.toggleLearn
    midiLearn_ = on;
    learnTarget_ = learnPrevTarget_ = -1;
    if (on && hooks_) {
        if (hooks_->lastCC) lastCcToken_ = hooks_->lastCC(hooks_->ctx);
        for (int id = 0; id < kLearnCount; ++id)
            learnCC_[size_t(id)] = hooks_->midiCC ? hooks_->midiCC(hooks_->ctx, id) : 0;
        buildLearnRects();
    }
    touch({0, 0, kDesignWidth, kDesignHeight});
}

void LinuxPanel::learnSelect(int id) {
    for (const int r : {learnTarget_, id})
        if (r >= 0 && r < kLearnCount) touch(learnRects_[size_t(r)]);
    learnTarget_ = id;
}

int LinuxPanel::learnHit(float x, float y) const {
    for (auto it = learnOrder_.rbegin(); it != learnOrder_.rend(); ++it) {
        const PanelRect &r = learnRects_[size_t(*it)];
        if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) return *it;
    }
    return -1;
}

void LinuxPanel::drawLearn(Canvas &cr) {
    const uint32_t learn = 0x59A6E8; // Theme.learn
    for (const int id : learnOrder_) {
        const PanelRect &r = learnRects_[size_t(id)];
        if (r.w <= 0) continue;
        const int cc = learnCC_[size_t(id)];
        const bool selected = learnTarget_ == id;
        if (cc) cr.roundedFill(blend(0x000000, learn, 102 / 255.0), r.x, r.y, r.w, r.h, 4.f);
        const float width = selected ? 2.f : 1.f; // strokeBorder: inside the box
        cr.roundedRing(selected ? learn : blend(0x000000, learn, 153 / 255.0), r.x + width / 2, r.y + width / 2,
                       r.w - width, r.h - width, 4.f - width / 2, width);
        if (cc) {
            const std::string label = std::to_string(cc);
            const float w = 6.f + 6.2f * float(label.size()), h = 13.f;
            const float bx = r.x + (r.w - w) / 2, by = r.y + (r.h - h) / 2;
            cr.roundedFill(learn, bx, by, w, h, 2.4f);
            cr.roundedRing(0xFFFFFF, bx + .5f, by + .5f, w - 1.f, h - 1.f, 2.4f, 1.f);
            cr.text(label, bx, by, w, h, 9, 0xFFFFFF, true, true);
        }
    }
}

// ---------------------------------------------------------------------------
// In-panel menus
// ---------------------------------------------------------------------------

void LinuxPanel::openMainMenu() {
    menu_.open = true;
    menu_.items.clear();
    menu_.items.push_back({"Initialize patch", MenuAction::InitializePatch, 0, 0, false, false});
    menu_.items.push_back({"", MenuAction::None, 0, 0, false, true});
    menu_.items.push_back({"Load patch file...", MenuAction::LoadPatchFile, 0, 0, false, false});
    menu_.items.push_back({"Save patch file...", MenuAction::SavePatchFile, 0, 0, false, false});
    menu_.items.push_back({"", MenuAction::None, 0, 0, false, true});
    menu_.items.push_back({midiLearn_ ? "Exit MIDI Learn" : "MIDI Learn", MenuAction::MidiLearn, 0, 0, false, false});
    const bool programChange = hooks_ && hooks_->programChange && hooks_->programChange(hooks_->ctx) != 0;
    menu_.items.push_back({"Program change", MenuAction::ProgramChange, 0, 0, programChange, false});
    menu_.items.push_back({"All notes off", MenuAction::AllNotesOff, 0, 0, false, false});
    menu_.items.push_back({"", MenuAction::None, 0, 0, false, true});
    static constexpr int sizeLevels[] = {75, 100, 125, 150, 200, 250, 300};
    const int currentSize = int(std::lround(100.f * float(viewWidth_) / kDesignWidth));
    for (const int level : sizeLevels)
        menu_.items.push_back({std::to_string(level) + "% size", MenuAction::UISize, 0, level,
                               currentSize == level, false});
    menu_.items.push_back({"", MenuAction::None, 0, 0, false, true});
    menu_.items.push_back({"Select theme...", MenuAction::Theme, 0, -1, false, false});
    menu_.items.push_back({"Themes...", MenuAction::ThemeEditor, 0, 0, false, false});
    layoutMenu(1, 168);
}

// The mac's LogoMenu: UI size steps (75..300 % of the 770-pixel canvas) and
// the theme list.
void LinuxPanel::openLogoMenu() {
    menu_.open = true;
    menu_.items.clear();
    static constexpr int levels[] = {75, 100, 125, 150, 200, 250, 300};
    const int current = int(std::lround(100.f * float(viewWidth_) / kDesignWidth));
    for (const int level : levels)
        menu_.items.push_back({std::to_string(level) + "% size", MenuAction::UISize, 0, level, current == level,
                               false});
    menu_.items.push_back({"", MenuAction::None, 0, 0, false, true});
    static constexpr int sizeLevels[] = {75, 100, 125, 150, 200, 250, 300};
    const int currentSize = int(std::lround(100.f * float(viewWidth_) / kDesignWidth));
    for (const int level : sizeLevels)
        menu_.items.push_back({std::to_string(level) + "% size", MenuAction::UISize, 0, level,
                               currentSize == level, false});
    menu_.items.push_back({"", MenuAction::None, 0, 0, false, true});
    menu_.items.push_back({"Select theme...", MenuAction::Theme, 0, -1, false, false});
    menu_.columns = 1;
    menu_.w = 168 + 12;
    menu_.h = float(menu_.items.size()) * 18 + 6 + 12;
    menu_.x = 4;
    menu_.y = 4;
    menu_.hover = -1;
    touch({0, 0, kDesignWidth, kDesignHeight});
}

void LinuxPanel::openThemeMenu() {
    menu_.open = true;
    menu_.items.clear();
    const auto &factory = themes::Factory();
    for (size_t i = 0; i < factory.size(); ++i)
        menu_.items.push_back({factory[i].name, MenuAction::Theme, 0, int(i), factory[i].palette == theme_, false});
    themeStore().Load();
    const auto &saved = themeStore().Saved();
    if (!saved.empty()) menu_.items.push_back({"", MenuAction::None, 0, 0, false, true});
    for (size_t i = 0; i < saved.size(); ++i)
        menu_.items.push_back({saved[i].name, MenuAction::Theme, 0, int(factory.size() + i),
                               saved[i].palette == theme_, false});
    layoutMenu(1, 168);
}

// A pick list for a type display, under the control that was clicked: the
// effect (2 columns) and the delay / chorus types (one column). Values no
// control offers (chorus 3) are left out.
void LinuxPanel::openTypeMenu(int id, float x, float y) {
    menu_.open = true;
    menu_.items.clear();
    int low = 0, high = 9, def = 0;
    if (hooks_ && hooks_->paramInfo) hooks_->paramInfo(hooks_->ctx, id, &low, &high, &def);
    const int current = value(id);
    for (int v = low; v <= high; ++v) {
        if (!s2::Offered(id, v)) continue;
        const std::string name = hooks_ && hooks_->formatValue ? hooks_->formatValue(hooks_->ctx, id, v)
                                                               : std::to_string(v);
        menu_.items.push_back({name, MenuAction::SetParam, id, v, current == v, false});
    }
    menu_.columns = id == 78 ? 2 : 1;
    menu_.w = (id == 78 ? 130.f : 150.f) * float(menu_.columns) + 12.f;
    menu_.h = float((int(menu_.items.size()) + menu_.columns - 1) / menu_.columns) * 18.f + 12.f;
    menu_.x = std::clamp(x, 4.f, kDesignWidth - menu_.w - 4.f);
    menu_.y = std::clamp(y, 4.f, kDesignHeight - menu_.h - 4.f);
    menu_.hover = -1;
    touch({0, 0, kDesignWidth, kDesignHeight});
}

// The ctrl src grid (showParameterMenu): None, Pitch bend, Channel pressure,
// then CC 0..127 in eight columns of sixteen.
void LinuxPanel::openSourceMenu(int id) {
    menu_.open = true;
    menu_.items.clear();
    const int current = value(id);
    menu_.items.push_back({"None", MenuAction::SetParam, id, 0, current == 0 || current == -1, false});
    menu_.items.push_back({"Pitch bend", MenuAction::SetParam, id, 0xE000, current == 0xE000, false});
    menu_.items.push_back({"Channel pressure", MenuAction::SetParam, id, 0xD000, current == 0xD000, false});
    for (int cc = 0; cc < 128; ++cc)
        menu_.items.push_back({"CC " + std::to_string(cc), MenuAction::SetParam, id, 0xB000 | cc,
                               current == (0xB000 | cc), false});
    layoutMenu(8, 84);
}

// The ctrl assign grid: None then Synth1's 51 targets, in the same order the
// menus use on the other platforms (SynthModel's assign list).
void LinuxPanel::openAssignMenu(int id) {
    menu_.open = true;
    menu_.items.clear();
    const int current = value(id);
    menu_.items.push_back({"None", MenuAction::SetParam, id, -1, current < 0, false});
    static constexpr int assigns[] = {76, 45, 95, 2,  12, 13, 11, 5,  8,  91, 15, 16, 17, 18, 19, 20, 21, 23,
                                      22, 25, 26, 27, 28, 29, 79, 80, 81, 61, 62, 63, 60, 90, 35, 36, 37, 83,
                                      98, 52, 53, 54, 55, 56, 39, 75, 92, 84, 85, 34, 43, 44, 48, 49};
    for (const int target : assigns) {
        const std::string name = hooks_ && hooks_->paramName ? hooks_->paramName(hooks_->ctx, target)
                                                            : std::to_string(target);
        menu_.items.push_back({name, MenuAction::SetParam, id, target, current == target, false});
    }
    layoutMenu(3, 132);
}

void LinuxPanel::layoutMenu(int columns, float width) {
    menu_.columns = columns;
    constexpr float rowHeight = 18, padding = 6;
    int rows = 0;
    for (const auto &item : menu_.items) rows += item.separator ? 0 : 1;
    rows = (rows + columns - 1) / columns;
    int separators = 0;
    for (const auto &item : menu_.items) separators += item.separator ? 1 : 0;
    menu_.w = width * columns + padding * 2;
    menu_.h = float(rows) * rowHeight + float(separators) * 6 + padding * 2;
    // Above the menu button, left-aligned with it, kept inside the panel.
    menu_.x = std::clamp(492.f, 4.f, LinuxPanel::kDesignWidth - menu_.w - 4.f);
    menu_.y = std::clamp(548.f - menu_.h, 4.f, LinuxPanel::kDesignHeight - menu_.h - 4.f);
    menu_.hover = -1;
    touch({0, 0, kDesignWidth, kDesignHeight});
}

void LinuxPanel::closeMenu() {
    if (!menu_.open) return;
    menu_.open = false;
    menu_.items.clear();
    touch({0, 0, kDesignWidth, kDesignHeight});
}

void LinuxPanel::drawMenu(Canvas &cr) {
    if (!menu_.open) return;
    const auto &p = colors_;
    constexpr float rowHeight = 18, padding = 6;
    // A drop shadow, then the box.
    cr.fillRect(blend(p[kPanel], 0x000000, .35f), menu_.x + 2, menu_.y + 2, menu_.w, menu_.h);
    cr.fillRect(p[kDisplayBottom], menu_.x, menu_.y, menu_.w, menu_.h);
    cr.roundedRing(p[kHeader], menu_.x, menu_.y, menu_.w, menu_.h, 3.f);
    int index = 0;      // non-separator items so far (the row / column grid)
    int separators = 0; // separators so far (each takes 6 px)
    for (size_t i = 0; i < menu_.items.size(); ++i) {
        const auto &item = menu_.items[i];
        const int column = menu_.columns > 1 ? index % menu_.columns : 0;
        const int row = menu_.columns > 1 ? index / menu_.columns : index;
        const float x = menu_.x + padding + float(column) * (menu_.w - 2 * padding) / float(menu_.columns);
        const float width = (menu_.w - 2 * padding) / float(menu_.columns);
        // Every item (separator or not) sits on the same grid: `row` counts the
        // items before it, `separators` the 6 px gaps before it.
        const float itemY = menu_.y + padding + float(row) * rowHeight + float(separators) * 6;
        if (item.separator) {
            cr.line(p[kHeader], menu_.x + padding, itemY + 3, menu_.x + menu_.w - padding, itemY + 3);
            ++separators;
            continue;
        }
        // A single atomic fill (no scanlines): a hovered row can never show
        // seams, whatever the theme's colours are.
        if (int(i) == menu_.hover) cr.fillRect(p[kDisplayText], x + 2, itemY + 1, width - 5, rowHeight - 2);
        const bool on = int(i) == menu_.hover;
        cr.text(item.label, x + (item.checked ? 16.f : 4.f), itemY, width - 6, rowHeight, 10.5f,
             on ? p[kDisplayBottom] : kLcdText, false, false);
        if (item.checked) { // a checkmark, drawn as a tick
            const uint32_t tick = on ? p[kDisplayBottom] : p[kDisplayText];
            cr.thickLine(tick, x + 5, itemY + rowHeight * .5f, x + 8, itemY + rowHeight * .72f, 1.6f);
            cr.thickLine(tick, x + 8, itemY + rowHeight * .72f, x + 13, itemY + rowHeight * .28f, 1.6f);
        }
        ++index;
    }
}

bool LinuxPanel::menuMove(float x, float y) {
    if (!menu_.open) return false;
    constexpr float rowHeight = 18, padding = 6;
    int hover = -1;
    if (x >= menu_.x && x < menu_.x + menu_.w && y >= menu_.y && y < menu_.y + menu_.h) {
        int index = 0;
        for (size_t i = 0; i < menu_.items.size(); ++i) {
            const auto &item = menu_.items[i];
            if (item.separator) continue;
            const int column = menu_.columns > 1 ? index % menu_.columns : 0;
            const int row = menu_.columns > 1 ? index / menu_.columns : index;
            const float width = (menu_.w - 2 * padding) / float(menu_.columns);
            const float x0 = menu_.x + padding + float(column) * width;
            const float y0 = menu_.y + padding + float(row) * rowHeight +
                             float(std::count_if(menu_.items.begin(), menu_.items.begin() + long(i),
                                                 [](const MenuItem &m) { return m.separator; })) * 6;
            if (x >= x0 && x < x0 + width - 2 && y >= y0 && y < y0 + rowHeight) {
                hover = int(i);
                break;
            }
            ++index;
        }
    }
    if (hover == menu_.hover) return false;
    menu_.hover = hover;
    touch({0, 0, kDesignWidth, kDesignHeight});
    return true;
}

bool LinuxPanel::menuClick(float x, float y) {
    if (!menu_.open) return false;
    if (x < menu_.x || x >= menu_.x + menu_.w || y < menu_.y || y >= menu_.y + menu_.h) {
        closeMenu();
        return true; // a click outside closes and does nothing else, as a popup
    }
    constexpr float rowHeight = 18, padding = 6;
    int index = 0;
    for (size_t i = 0; i < menu_.items.size(); ++i) {
        const auto &item = menu_.items[i];
        if (item.separator) continue;
        const int column = menu_.columns > 1 ? index % menu_.columns : 0;
        const int row = menu_.columns > 1 ? index / menu_.columns : index;
        const float width = (menu_.w - 2 * padding) / float(menu_.columns);
        const float x0 = menu_.x + padding + float(column) * width;
        const float y0 = menu_.y + padding + float(row) * rowHeight +
                         float(std::count_if(menu_.items.begin(), menu_.items.begin() + long(i),
                                             [](const MenuItem &m) { return m.separator; })) * 6;
        if (x >= x0 && x < x0 + width - 2 && y >= y0 && y < y0 + rowHeight) {
            const MenuItem picked = item;
            closeMenu();
            runMenuAction(picked);
            return true;
        }
        ++index;
    }
    closeMenu();
    return true;
}

void LinuxPanel::runMenuAction(const MenuItem &item) {
    switch (item.action) {
    case MenuAction::InitializePatch:
        if (hooks_ && hooks_->initializePatch) hooks_->initializePatch(hooks_->ctx);
        touch({0, 0, kDesignWidth, kDesignHeight});
        return;
    case MenuAction::ProgramChange: {
        const bool on = hooks_ && hooks_->programChange && hooks_->programChange(hooks_->ctx) != 0;
        if (hooks_ && hooks_->setProgramChange) hooks_->setProgramChange(hooks_->ctx, on ? 0 : 1);
        flashText_ = on ? "program change: off" : "program change: on";
        flashUntil_ = nowMs() + 1500;
        touch(lcdRect());
        return;
    }
    case MenuAction::AllNotesOff:
        if (hooks_ && hooks_->allNotesOff) {
            hooks_->allNotesOff(hooks_->ctx);
            flashText_ = "all notes off";
            flashUntil_ = nowMs() + 1500;
            touch(lcdRect());
        }
        return;
    case MenuAction::Theme: {
        if (item.value < 0) { // "Select theme..." opens the list
            openThemeMenu();
            return;
        }
        const auto &factory = themes::Factory();
        if (item.value < int(factory.size())) {
            setTheme(factory[size_t(item.value)].palette);
        } else {
            themeStore().Load();
            const auto &saved = themeStore().Saved();
            const size_t index = size_t(item.value - int(factory.size()));
            if (index < saved.size()) setTheme(saved[index].palette);
        }
        return;
    }
    case MenuAction::SetParam:
        setValueOnce(item.param, item.value);
        return;
    case MenuAction::MidiLearn:
        setLearn(!midiLearn_);
        return;
    case MenuAction::ExportBank: {
        // The bank's patches into a folder picked here: the file dialog
        // exports into the folder it is in when it is accepted.
        exportBank_ = item.param;
        openFileDialog(FileMode::ExportBank);
        return;
    }
    case MenuAction::ShowInLibrary: {
        if (!hooks_) return;
        const char *path = item.value < 0 && hooks_->libraryRoot
                               ? hooks_->libraryRoot(hooks_->ctx)
                               : (hooks_->slotPath ? hooks_->slotPath(hooks_->ctx, item.param, item.value) : nullptr);
        if (path && *path) revealInFileManager(path);
        return;
    }
    case MenuAction::DeleteBank:
    case MenuAction::DeletePatch:
        askDelete(item.param, item.value);
        return;
    case MenuAction::ThemeEditor:
        openThemeEditor();
        return;
    case MenuAction::UISize:
        requestedWidth_ = int(std::lround(kDesignWidth * float(item.value) / 100.f));
        touch({0, 0, kDesignWidth, kDesignHeight});
        return;
    case MenuAction::LoadPatchFile:
        openFileDialog(FileMode::OpenPatch);
        return;
    case MenuAction::SavePatchFile:
        openFileDialog(FileMode::SavePatch);
        return;
    case MenuAction::None:
        return;
    }
}

// ---------------------------------------------------------------------------
// The patch browser (PatchBrowserView): banks left, the shown bank's 128
// slots right. Read-only for now — load / close; write, import, export and
// drag and drop are the next steps.
// ---------------------------------------------------------------------------

void LinuxPanel::startLibraryScan() {
    if (hooks_ && hooks_->startLibraryScan) hooks_->startLibraryScan(hooks_->ctx);
}

void LinuxPanel::refreshBanks() {
    // Even a request to obtain Json() is a blocking scan before revision 1.
    if (knownRevision_ <= 0 || !hooks_ || !hooks_->libraryJSON) return;
    const char *data = hooks_->libraryJSON(hooks_->ctx);
    if (!data) return;
    const std::string text = data; // the hooks share one scratch buffer
    // SynthModel.loadLibrary: a zip found in the library was imported
    // ("X.zip imported and moved to the Trash"), shown 10 s.
    if (hooks_->libraryNotice) {
        if (const char *notice = hooks_->libraryNotice(hooks_->ctx); notice && *notice) {
            flashText_ = notice;
            flashUntil_ = nowMs() + 10000;
            touch(lcdRect());
        }
    }
    json::Value root;
    if (!json::Parse(text, root) || !root.IsObject()) return;
    std::vector<Bank> scanned;
    std::string libraryRoot;
    if (const auto *v = root.Find("root"); v && v->IsString()) libraryRoot = v->string;
    if (const auto *list = root.Find("banks"); list && list->type == json::Value::Type::Array) {
        for (const auto &item : list->array) {
            Bank b;
            if (const auto *v = item.Find("name"); v && v->IsString()) b.name = v->string;
            if (const auto *v = item.Find("source"); v && v->IsString()) b.source = v->string;
            if (const auto *v = item.Find("readOnly")) b.readOnly = v->type == json::Value::Type::Bool && v->boolean;
            if (const auto *v = item.Find("slots"); v && v->type == json::Value::Type::Array)
                for (const auto &slot : v->array) b.slots.push_back(slot.IsString() ? slot.string : std::string());
            scanned.push_back(std::move(b));
        }
    }
    // Backend order / indices stay unchanged: the natural sort is UI only, so
    // .sy1 slot numbers, host state and bank hook indices are untouched. The
    // browser stays on the bank it shows (by source).
    const std::string previous = selectedBank_ >= 0 && selectedBank_ < int(banks_.size())
                                     ? banks_[size_t(selectedBank_)].source
                                     : std::string();
    libraryRoot_ = std::move(libraryRoot);
    banks_ = std::move(scanned);
    if (!previous.empty()) {
        selectedBank_ = bankBySource(previous);
        if (selectedBank_ < 0) selectedSlot_ = -1;
    }
    bankOrder_.resize(banks_.size());
    for (size_t i = 0; i < bankOrder_.size(); ++i) bankOrder_[i] = int(i);
    std::stable_sort(bankOrder_.begin(), bankOrder_.end(), [this](int a, int b) {
        if (a == 0 && banks_[size_t(a)].readOnly) return true; // the factory bank stays first
        if (b == 0 && banks_[size_t(b)].readOnly) return false;
        return naturalLess(banks_[size_t(a)].name, banks_[size_t(b)].name);
    });
    int loadedBank = -1, loadedSlot = -1;
    if (hooks_->loadedProgram) hooks_->loadedProgram(hooks_->ctx, &loadedBank, &loadedSlot);
    if (selectedBank_ == -1 && loadedBank >= 0) selectedBank_ = loadedBank;
    if (selectedBank_ == -1 && !banks_.empty()) selectedBank_ = bankOrder_.front();
    if (selectedBank_ >= int(banks_.size())) selectedBank_ = banks_.empty() ? -1 : bankOrder_.front();
    if (selectedSlot_ < 0 && loadedSlot >= 0 && selectedBank_ == loadedBank) selectedSlot_ = loadedSlot;
    // The write dialog never targets the read-only factory bank: out of it
    // (or out of no bank) it opens on the "User" bank, proposed even when the
    // library has none (the write makes it). A writable bank in view stays
    // (the loaded one: written back). As PatchBrowserView.leaveReadOnlyBank.
    if (writing_ && !banks_.empty() && (selectedBank_ < 0 || banks_[size_t(selectedBank_)].readOnly)) {
        const int user = userBank();
        const int pick = user >= 0 ? user : kNewUserBank;
        if (pick != selectedBank_) {
            selectedBank_ = pick;
            selectedSlot_ = -1;
            scrollBankIntoView(pick);
        }
    }
    if (writing_ && selectedSlot_ < 0 && selectedBank_ == kNewUserBank) selectedSlot_ = 0;
    if (writing_ && selectedSlot_ < 0 && selectedBank_ >= 0) {
        const auto &slots = banks_[size_t(selectedBank_)].slots;
        const auto it = std::find(slots.begin(), slots.end(), "");
        selectedSlot_ = it == slots.end() ? 0 : int(it - slots.begin());
    }
    bankScroll_ = std::clamp(bankScroll_, 0, std::max(0, bankRowCount() - 20));
    touch(browserRect());
    touch(lcdRect());
}

void LinuxPanel::adoptLibrary() {
    if (hooks_ && hooks_->libraryRevision) knownRevision_ = hooks_->libraryRevision(hooks_->ctx);
    refreshBanks();
}

int LinuxPanel::userBank() const {
    for (size_t b = 0; b < banks_.size(); ++b)
        if (!banks_[b].readOnly && banks_[b].name == s2::kUserBankName) return int(b);
    return -1;
}
bool LinuxPanel::offersNewUserBank() const {
    return writing_ && !banks_.empty() && userBank() < 0;
}
int LinuxPanel::bankRowCount() const {
    return int(bankOrder_.size()) + (offersNewUserBank() ? 1 : 0);
}
int LinuxPanel::bankRowOf(int bank) const {
    if (bank == kNewUserBank) return offersNewUserBank() ? int(bankOrder_.size()) : -1;
    const auto it = std::find(bankOrder_.begin(), bankOrder_.end(), bank);
    return it == bankOrder_.end() ? -1 : int(it - bankOrder_.begin());
}
void LinuxPanel::scrollBankIntoView(int bank) {
    const int row = bankRowOf(bank);
    if (row >= 0 && (row < bankScroll_ || row >= bankScroll_ + 20)) bankScroll_ = std::max(0, row - 19);
}
int LinuxPanel::bankBySource(const std::string &source) const {
    for (size_t i = 0; i < banks_.size(); ++i)
        if (banks_[i].source == source) return int(i);
    return -1;
}
int LinuxPanel::displayBankNumber(int bank) const {
    const int row = bankRowOf(bank);
    return row >= 0 ? row : bank;
}
std::string LinuxPanel::slotText(int bank, int slot) const {
    char number[16];
    std::snprintf(number, sizeof number, "%03d:", slot + 1);
    return number + banks_[size_t(bank)].slots[size_t(slot)];
}

void LinuxPanel::openBrowser(bool writing) {
    browser_ = true;
    writing_ = writing;
    bankScroll_ = slotScroll_ = 0;
    selectedSlot_ = -1;
    if (writing) // the name field starts with the loaded patch's name, selected
        focusField(EditTarget::PatchName, patchName(), true);
    // SynthModel.openBrowser: opening selects the loaded program's bank.
    int loadedBank = -1, loadedSlot = -1;
    if (knownRevision_ > 0 && hooks_ && hooks_->loadedProgram)
        hooks_->loadedProgram(hooks_->ctx, &loadedBank, &loadedSlot);
    if (loadedBank >= 0 && loadedBank < int(banks_.size())) selectedBank_ = loadedBank;
    startLibraryScan(); // a background rescan on every open, as the mac
    refreshBanks();
    scrollBankIntoView(selectedBank_);
    touch(browserRect());
    touch(lcdRect());
}

void LinuxPanel::closeBrowser() {
    browser_ = false;
    writing_ = false;
    focusField(EditTarget::None, "", false);
    if (selectedBank_ == kNewUserBank) selectedBank_ = -1; // the next refresh selects the loaded bank
    touch(browserRect());
    touch(lcdRect());
}

int LinuxPanel::bankRowAt(float x, float y) const {
    if (x < 7 || x >= 196 || y < 64 || y > 469) return -1;
    const int visual = bankScroll_ + int((y - 64) / 20);
    if (visual >= 0 && visual < int(bankOrder_.size())) return bankOrder_[size_t(visual)];
    return visual == int(bankOrder_.size()) && offersNewUserBank() ? kNewUserBank : -1;
}

int LinuxPanel::slotAt(float x, float y) const {
    if (x < 204 || x >= 760 || y < 64 || y > 469) return -1;
    int count = -1;
    if (writing_ && selectedBank_ == kNewUserBank) count = s2::kBankSlots;
    else if (selectedBank_ >= 0 && selectedBank_ < int(banks_.size())) count = int(banks_[size_t(selectedBank_)].slots.size());
    else return -1;
    const int rows = std::max(1, (count + 3) / 4);
    const int column = int((x - 204) / 138.5f);
    const int row = slotScroll_ + int((y - 65) / 20);
    if (column < 0 || column > 3 || row < 0 || row >= rows) return -1;
    const int index = column * rows + row;
    return index < count ? index : -1;
}

void LinuxPanel::loadSlot(int bank, int slot) {
    if (!hooks_ || !hooks_->loadLibraryPatch) return;
    if (!hooks_->loadLibraryPatch(hooks_->ctx, bank, slot)) return;
    if (bank != selectedBank_) slotScroll_ = 0;
    selectedBank_ = bank;
    selectedSlot_ = slot;
    scrollBankIntoView(bank);
    if (bank >= 0 && bank < int(banks_.size())) {
        const int rows = std::max(1, (int(banks_[size_t(bank)].slots.size()) + 3) / 4), row = slot % rows;
        if (row < slotScroll_) slotScroll_ = row;
        else if (row >= slotScroll_ + 20) slotScroll_ = row - 19;
    }
    touch(browserRect());
    touch(lcdRect());
}

// A click on a slot of the shown bank: the browser loads it (a double click
// also closes).
void LinuxPanel::slotClick(int index, bool doubleClick) {
    selectedSlot_ = index;
    const bool valid = selectedBank_ >= 0 && selectedBank_ < int(banks_.size()) &&
                       index < int(banks_[size_t(selectedBank_)].slots.size()) &&
                       !banks_[size_t(selectedBank_)].slots[size_t(index)].empty();
    if (!writing_ && valid && hooks_ && hooks_->loadLibraryPatch) {
        hooks_->loadLibraryPatch(hooks_->ctx, selectedBank_, index);
        if (doubleClick) closeBrowser();
    }
    touch(browserRect());
    touch(lcdRect());
}

void LinuxPanel::browserClick(float x, float y, bool doubleClick) {
    if (y >= 37 && y <= 61) {
        if (x >= 690 && x < 760) { closeBrowser(); return; }
        if (writing_) {
            if (x >= 514 && x < 601) { commitWrite(); return; }
            if (x >= 604 && x < 680) { newBank(); return; }
            if (x >= 300 && x < 500) { // the name field: caret to the end
                caret_ = editText_.size();
                nameSelected_ = false;
                touch({300, 38, 200, 19});
                return;
            }
            return;
        }
        if (x >= 430 && x < 498) { // Import...
            openFileDialog(FileMode::Import);
            return;
        }
        if (x >= 502 && x < 617) { // Library folder: the file manager
            if (hooks_ && hooks_->libraryRoot) openWithDesktop(hooks_->libraryRoot(hooks_->ctx));
            return;
        }
        if (x >= 620 && x < 681) { // Refresh
            startLibraryScan();
            refreshBanks();
            return;
        }
        return;
    }
    if (y < 64 || y > 469) return;
    if (x >= 7 && x < 196) {
        const int bank = bankRowAt(x, y);
        if (bank == kNewUserBank ||
            (bank >= 0 && bank < int(banks_.size()) && !(writing_ && banks_[size_t(bank)].readOnly))) {
            selectedBank_ = bank;
            selectedSlot_ = -1;
            slotScroll_ = 0;
            if (bank == kNewUserBank) selectedSlot_ = 0;
            if (writing_ && bank >= 0) { // the first empty slot, as the write dialog
                const auto &slots = banks_[size_t(bank)].slots;
                const auto it = std::find(slots.begin(), slots.end(), "");
                selectedSlot_ = it == slots.end() ? 0 : int(it - slots.begin());
            }
            touch(browserRect());
        }
        return;
    }
    if (const int index = slotAt(x, y); index >= 0) {
        // A patch may be dragged: its click acts on release (as a tap),
        // unless it turned into a drag. Empty slots act at once.
        const bool patch = !writing_ && selectedBank_ >= 0 && selectedBank_ < int(banks_.size()) &&
                           index < int(banks_[size_t(selectedBank_)].slots.size()) &&
                           !banks_[size_t(selectedBank_)].slots[size_t(index)].empty();
        if (doubleClick || !patch) {
            slotClick(index, doubleClick);
            return;
        }
        press_ = {true, false, selectedBank_, index, banks_[size_t(selectedBank_)].source, x, y, x, y,
                  slotText(selectedBank_, index)};
    }
}

void LinuxPanel::stepPatch(int direction) {
    if (banks_.empty()) refreshBanks();
    const int n = int(bankOrder_.size());
    if (n == 0) return;
    int bank = -1, slot = -1;
    if (knownRevision_ > 0 && hooks_ && hooks_->loadedProgram) hooks_->loadedProgram(hooks_->ctx, &bank, &slot);
    int row = bankRowOf(bank);
    if (row < 0 || row >= n) {
        row = std::clamp(bankRowOf(selectedBank_), 0, n - 1);
        slot = -1;
    }
    int p = slot >= 0 ? slot : direction > 0 ? -1 : int(banks_[size_t(bankOrder_[size_t(row)])].slots.size());
    for (int guard = 0; guard < n * s2::kBankSlots + 1; ++guard) {
        const auto &slots = banks_[size_t(bankOrder_[size_t(row)])].slots;
        p += direction > 0 ? 1 : -1;
        if (p >= int(slots.size())) {
            row = (row + 1) % n;
            p = -1;
            continue;
        }
        if (p < 0) {
            row = (row - 1 + n) % n;
            p = int(banks_[size_t(bankOrder_[size_t(row)])].slots.size());
            continue;
        }
        if (!slots[size_t(p)].empty()) {
            loadSlot(bankOrder_[size_t(row)], p);
            return;
        }
    }
}

// Keys (SynthModel.browserKey): Esc closes the browser; with it open, up /
// down = previous / next patch (rolling into the neighbouring banks, as
// < >), left / right = the neighbouring column (nearest patch), Return
// closes; with it closed, left / right = < >.
bool LinuxPanel::keyDown(Key key) {
    if (themeDialog_) {
        switch (key) {
        case Key::Escape:
            closeThemeEditor();
            return true;
        case Key::Return:
            if (editTarget_ == EditTarget::ThemeColor) {
                applyThemeColor();
                focusField(EditTarget::ThemeName, themeName_, false);
            } else {
                saveTheme();
            }
            touch({0, 0, kDesignWidth, kDesignHeight});
            return true;
        case Key::Backspace:
        case Key::Delete:
        case Key::Home:
        case Key::End:
        case Key::Left:
        case Key::Right:
            editKey(key);
            return true;
        default:
            return true;
        }
    }
    if (confirm_.open) {
        if (key == Key::Escape || key == Key::Return) {
            confirm_.open = false;
            touch({0, 0, kDesignWidth, kDesignHeight});
        }
        return true;
    }
    if (file_.mode != FileMode::None) {
        if (key == Key::Escape) {
            closeFileDialog();
            return true;
        }
        if (key == Key::Return) {
            acceptFile();
            return true;
        }
        if (file_.mode == FileMode::SavePatch) {
            switch (key) {
            case Key::Backspace: case Key::Delete: case Key::Home: case Key::End: case Key::Left: case Key::Right:
                editKey(key);
                return true;
            default:
                return true;
            }
        }
        return true;
    }
    if (menu_.open) {
        if (key == Key::Escape || key == Key::Return) closeMenu();
        return true;
    }
    if (writing_) {
        switch (key) {
        case Key::Escape: closeBrowser(); return true;
        case Key::Return: commitWrite(); return true;
        case Key::Backspace: case Key::Delete: case Key::Home: case Key::End: case Key::Left: case Key::Right:
            editKey(key);
            return true;
        default:
            return true; // up / down: nothing in the write dialog
        }
    }
    if (key == Key::Escape) {
        if (browser_) {
            closeBrowser();
            return true;
        }
        return false;
    }
    if (!browser_) {
        if (key != Key::Left && key != Key::Right) return false;
        stepPatch(key == Key::Left ? -1 : 1);
        return true;
    }
    if (key == Key::Return) {
        closeBrowser();
        return true;
    }
    if (key != Key::Up && key != Key::Down && key != Key::Left && key != Key::Right) return false;
    if (selectedBank_ < 0 || selectedBank_ >= int(banks_.size())) return true;
    const auto &slots = banks_[size_t(selectedBank_)].slots;
    int loadedBank = -1, loadedSlot = -1;
    if (knownRevision_ > 0 && hooks_ && hooks_->loadedProgram)
        hooks_->loadedProgram(hooks_->ctx, &loadedBank, &loadedSlot);
    if (loadedBank != selectedBank_ || loadedSlot < 0) { // another bank shown: its first patch
        const auto first = std::find_if(slots.begin(), slots.end(),
                                        [](const std::string &s) { return !s.empty(); });
        if (first != slots.end()) loadSlot(selectedBank_, int(first - slots.begin()));
        return true;
    }
    if (key == Key::Up || key == Key::Down) {
        stepPatch(key == Key::Down ? 1 : -1);
        return true;
    }
    const int count = int(slots.size()), rows = std::max(1, (count + 3) / 4);
    const int column = loadedSlot / rows + (key == Key::Right ? 1 : -1);
    if (column < 0 || column * rows >= count) return true;
    const int wanted = column * rows + loadedSlot % rows;
    int best = -1;
    for (int s = column * rows; s < std::min((column + 1) * rows, count); ++s)
        if (!slots[size_t(s)].empty() && (best < 0 || std::abs(s - wanted) < std::abs(best - wanted))) best = s;
    if (best >= 0) loadSlot(selectedBank_, best);
    return true;
}

void LinuxPanel::drawBrowser(Canvas &cr) {
    const auto &p = colors_;
    // As the mac browser: a selected row = display text colour behind
    // display-bottom ink; other names in Theme.lcdText (empty slots at 30%).
    const uint32_t selectionFill = p[kDisplayText];
    const uint32_t selectionInk = p[kDisplayBottom];
    const uint32_t nameInk = kLcdText;
    const uint32_t emptyInk = blend(p[kDisplayBottom], nameInk, .3f);
    // Numbering is warm yellow on dark rows, dark gold on pale selections.
    const uint32_t numberInk = 0xE9D56A, selectedNumberInk = 0x816000;
    const float x = 6, y = 34, w = 758, h = 444;
    cr.fillRect(p[kPanel], x, y, w, h);
    cr.rectOutline(p[kHeader], x, y, w, h, 1.5f);
    cr.fillRect(p[kHeader], x + 1, y + 1, w - 2, 27);
    cr.text(writing_ ? "Write patch" : "Banks", x + 7, y + 2, 135, 24, 12, p[kHeaderText], true);
    if (writing_) {
        drawNameField(cr);
        darkButton(cr, p[kButton], 514, 38, 84, 20, hoveredChrome_ == 9);
        cr.text("Write patch", 514, 38, 84, 20, 10, 0xFFFFFF, true, true);
        darkButton(cr, p[kButton], 604, 38, 74, 20, hoveredChrome_ == 10);
        cr.text("New bank...", 604, 38, 74, 20, 10, 0xFFFFFF, true, true);
    } else {
        darkButton(cr, p[kButton], 430, 38, 66, 20, hoveredChrome_ == 11);
        cr.text("Import...", 430, 38, 66, 20, 10, 0xFFFFFF, true, true);
        darkButton(cr, p[kButton], 502, 38, 112, 20, hoveredChrome_ == 12);
        cr.text("Library folder", 502, 38, 112, 20, 10, 0xFFFFFF, true, true);
        darkButton(cr, p[kButton], 620, 38, 60, 20, hoveredChrome_ == 13);
        cr.text("Refresh", 620, 38, 60, 20, 10, 0xFFFFFF, true, true);
    }
    darkButton(cr, p[kButton], 690, 38, 64, 20, hoveredChrome_ == 14);
    cr.text("Close", 690, 38, 64, 20, 10, 0xFFFFFF, true, true);
    cr.fillRect(p[kDisplayBottom], x + 1, y + 28, 190, h - 30);
    cr.fillRect(p[kDisplayBottom], x + 191, y + 28, w - 192, h - 30);
    cr.rectOutline(p[kHeader], x + 190, y + 28, 1, h - 30);
    constexpr float row = 20;
    const int visualCount = bankRowCount(); // the banks (+ new User)
    for (int i = 0; i < 20; ++i) {
        const int visual = bankScroll_ + i;
        if (visual < 0 || visual >= visualCount) break;
        const int bi = visual < int(bankOrder_.size()) ? bankOrder_[size_t(visual)] : kNewUserBank;
        const bool isNewUser = bi == kNewUserBank;
        const float ry = 64 + row * i;
        if (bi == selectedBank_) cr.fillRect(selectionFill, 8, ry, 187, 19);
        int count = 0;
        if (!isNewUser)
            for (const auto &slot : banks_[size_t(bi)].slots)
                if (!slot.empty()) ++count;
        char prefix[16];
        std::snprintf(prefix, sizeof prefix, "%02d:", visual);
        const uint32_t bankText = bi == selectedBank_ ? selectionInk : nameInk;
        // the bank to be made has no number yet
        cr.text(isNewUser ? "--:" : prefix, 12, ry, 27, 18, 10.5f,
             bi == selectedBank_ ? selectedNumberInk : numberInk, false, false, true);
        cr.text(isNewUser ? s2::kUserBankName : banks_[size_t(bi)].name, 39, ry, 116, 18, 10.5f, bankText, false,
             false, true);
        cr.text(isNewUser ? "new" : std::to_string(count), 164, ry, 30, 18, 10.5f, bankText, false, true, true);
        if (!isNewUser && bi == dropBank_) // the copy target
            cr.roundedRing(p[kDisplayText], 8.75f, ry + .75f, 185.5f, 17.5f, 1.5f, 1.5f);
    }
    if (knownRevision_ <= 0) {
        cr.text("Scanning preset banks in the background...", 204, 90, 510, 30, 12, 0xC1DCE0);
        return;
    }
    if (selectedBank_ == -1) {
        cr.text("Select a bank.", 204, 90, 510, 30, 12, 0xC1DCE0);
        return;
    }
    const bool newUserView = writing_ && selectedBank_ == kNewUserBank; // all slots free
    if (!newUserView && (selectedBank_ < 0 || selectedBank_ >= int(banks_.size()))) return;
    int loadedBank = -1, loadedSlot = -1;
    if (hooks_ && hooks_->loadedProgram) hooks_->loadedProgram(hooks_->ctx, &loadedBank, &loadedSlot);
    const int count = newUserView ? s2::kBankSlots : int(banks_[size_t(selectedBank_)].slots.size());
    const int rows = std::max(1, (count + 3) / 4);
    for (int rr = 0; rr < 20; ++rr) {
        const int br = rr + slotScroll_;
        if (br >= rows) break;
        for (int col = 0; col < 4; ++col) {
            const int slotIndex = col * rows + br;
            if (slotIndex >= count) continue;
            const float x0 = 204.f + float(col) * 138.5f;
            const float y0 = 65.f + float(rr) * 20.f;
            const std::string label = newUserView ? std::string()
                                                  : banks_[size_t(selectedBank_)].slots[size_t(slotIndex)];
            const bool valid = !label.empty();
            const bool selected = writing_ ? slotIndex == selectedSlot_
                                           : (valid && selectedBank_ == loadedBank && slotIndex == loadedSlot);
            cr.fillRect(selected ? selectionFill : p[kDisplayBottom], x0, y0, 136, 18);
            char prefix[16];
            std::snprintf(prefix, sizeof prefix, "%03d:", slotIndex + 1);
            cr.text(prefix, x0 + 3, y0, 35, 18, 10.2f, selected ? selectedNumberInk : numberInk, false, false,
                 true);
            cr.text(label, x0 + 39, y0, 93, 18, 10.2f, selected ? selectionInk : valid ? nameInk : emptyInk, false,
                 false, true);
            if (slotIndex == dropSlot_) // the move target
                cr.roundedRing(p[kDisplayText], x0 + .75f, y0 + .75f, 134.5f, 16.5f, 2.f, 1.5f);
        }
    }
    if (writing_ && selectedBank_ >= 0 && selectedBank_ < int(banks_.size()) &&
        banks_[size_t(selectedBank_)].readOnly)
        cr.text("Factory bank is read-only. Choose a user bank.", 213, 445, 490, 19, 10, 0xE2A3A3);
}

// The write dialog's patch name (PatchBrowserView's text field): a display
// box with a caret; typing goes through textInput, editing through editKey.
void LinuxPanel::drawNameField(Canvas &cr) {
    const auto &p = colors_;
    const float x = 300, y = 38, w = 200, h = 19;
    cr.fillRect(p[kDisplayBottom], x, y, w, h);
    cr.roundedRing(0x0B0B0B, x, y, w, h, 2.f);
    if (nameSelected_ && !editText_.empty()) { // the mac's selected field
        const float ink = float(cr.textWidth(editText_, 10.5f, true));
        cr.fillRect(p[kDisplayText], x + 2, y + 2, ink + 2, h - 4);
        cr.text(editText_, x + 3, y + 1, w - 6, h - 2, 10.5f, p[kDisplayBottom], false, false, true);
    } else {
        cr.text(editText_, x + 3, y + 1, w - 6, h - 2, 10.5f, kLcdText, false, false, true);
    }
    const float caretX = x + 3 + float(cr.textWidth(editText_.substr(0, caret_), 10.5f, true));
    cr.line(kLcdText, caretX, y + 3, caretX, y + h - 3);
}

// PatchBrowserView.commitWrite: the Write patch button, Return / Enter, or
// the bottom bar's write clicked again.
void LinuxPanel::commitWrite() {
    if (!browser_ || !writing_ || !hooks_) return;
    const bool newUser = selectedBank_ == kNewUserBank;
    if (selectedSlot_ < 0) return;
    if (!newUser && (selectedBank_ < 0 || selectedBank_ >= int(banks_.size()) ||
                     banks_[size_t(selectedBank_)].readOnly))
        return;
    const std::string name = editText_.empty() ? patchName() : editText_;
    // The proposed User bank is made now, by the write (newBank rescans: its
    // index is valid for the write right away).
    int bank = selectedBank_;
    if (newUser && (!hooks_->newBank || (bank = hooks_->newBank(hooks_->ctx, s2::kUserBankName)) < 0)) {
        flashText_ = "Cannot create the User bank. Check the library folder is writable.";
        flashUntil_ = nowMs() + 5000;
        touch(lcdRect());
        return;
    }
    if (hooks_->writeLibraryPatch && hooks_->writeLibraryPatch(hooks_->ctx, bank, selectedSlot_, name.c_str())) {
        const int slot = selectedSlot_;
        closeBrowser();
        refreshBanks();
        if (bank >= 0 && bank < int(banks_.size())) {
            char where[128];
            std::snprintf(where, sizeof where, "written to %02d:%s %03d", displayBankNumber(bank),
                          banks_[size_t(bank)].name.c_str(), slot + 1);
            flashText_ = where;
            flashUntil_ = nowMs() + 1500;
            touch(lcdRect());
        }
    } else {
        flashText_ = "Cannot write that preset. Check the library folder is writable.";
        flashUntil_ = nowMs() + 5000;
        touch(lcdRect());
    }
}

// SynthModel.newBank: a uniquely named folder, then select it. The mac asks
// for the name in a dialog and Windows in a text prompt; without a toolkit
// the port proposes "New Bank", "New Bank 2", ... (banks are renamed in the
// file manager, as on the mac).
void LinuxPanel::newBank() {
    if (!hooks_ || !hooks_->newBank) return;
    std::string name;
    for (int n = 1; n < 1000; ++n) {
        name = n == 1 ? "New Bank" : "New Bank " + std::to_string(n);
        bool taken = false;
        for (const auto &b : banks_)
            if (b.name == name) {
                taken = true;
                break;
            }
        if (!taken) break;
    }
    const int bank = hooks_->newBank(hooks_->ctx, name.c_str());
    if (bank < 0) {
        flashText_ = "Cannot create that bank. Check the library folder is writable.";
        flashUntil_ = nowMs() + 5000;
        touch(lcdRect());
        return;
    }
    if (hooks_->libraryRevision) knownRevision_ = hooks_->libraryRevision(hooks_->ctx);
    refreshBanks();
    if (bank < int(banks_.size())) {
        selectedBank_ = bank;
        selectedSlot_ = 0;
        slotScroll_ = 0;
    }
    touch(browserRect());
}

void LinuxPanel::textInput(const std::string &utf8) {
    if (editTarget_ == EditTarget::None || utf8.empty()) return;
    // A field never absorbs a control byte (C0, DEL, C1) or a broken sequence:
    // Return / Esc arrive as keys, and an input method may hand over several
    // bytes at once. Decode, keep only printable codepoints.
    if (!printableText(utf8)) return;
    if (nameSelected_) { // typing replaces the selected text, as the mac's field
        editText_.clear();
        caret_ = 0;
        nameSelected_ = false;
    }
    editText_.insert(caret_, utf8);
    caret_ += utf8.size();
    if (editTarget_ == EditTarget::ThemeColor) applyThemeColor();
    else if (editTarget_ == EditTarget::ThemeName) themeName_ = editText_;
    touch(fieldRect());
}

void LinuxPanel::editKey(Key key) {
    if (editTarget_ == EditTarget::None) return;
    if (nameSelected_ && (key == Key::Backspace || key == Key::Delete)) {
        editText_.clear();
        caret_ = 0;
        nameSelected_ = false;
        if (editTarget_ == EditTarget::ThemeName) themeName_.clear();
        touch(fieldRect());
        return;
    }
    nameSelected_ = false;
    auto isContinuation = [this](size_t i) { return i < editText_.size() && ((unsigned char)editText_[i] & 0xC0) == 0x80; };
    switch (key) {
    case Key::Backspace:
        if (caret_ > 0) {
            size_t start = caret_ - 1;
            while (start > 0 && isContinuation(start)) --start;
            editText_.erase(start, caret_ - start);
            caret_ = start;
        }
        break;
    case Key::Delete:
        if (caret_ < editText_.size()) {
            size_t end = caret_ + 1;
            while (isContinuation(end)) ++end;
            editText_.erase(caret_, end - caret_);
        }
        break;
    case Key::Left:
        if (caret_ > 0) {
            --caret_;
            while (caret_ > 0 && isContinuation(caret_)) --caret_;
        }
        break;
    case Key::Right:
        if (caret_ < editText_.size()) {
            ++caret_;
            while (isContinuation(caret_)) ++caret_;
        }
        break;
    case Key::Home:
        caret_ = 0;
        break;
    case Key::End:
        caret_ = editText_.size();
        break;
    default:
        break;
    }
    if (editTarget_ == EditTarget::ThemeColor) applyThemeColor();
    else if (editTarget_ == EditTarget::ThemeName) themeName_ = editText_;
    touch(fieldRect());
}

} // namespace oxo
