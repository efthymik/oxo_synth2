#include "LinuxCanvas.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace oxo {

namespace {
constexpr float kPi = 3.14159265358979323846f;

// Text size rule: below 13 px the font is drawn at 1.08x, never smaller than
// 10.8, so small labels stay legible.
double designFontSize(float size) { return size < 13.f ? std::max(10.8, double(size) * 1.08) : double(size); }

LICE_pixel blendColour(uint32_t from, uint32_t to, float t) {
    t = std::clamp(t, 0.f, 1.f);
    const int r = int(colourRed(from) + (colourRed(to) - colourRed(from)) * t + 0.5f);
    const int g = int(colourGreen(from) + (colourGreen(to) - colourGreen(from)) * t + 0.5f);
    const int b = int(colourBlue(from) + (colourBlue(to) - colourBlue(from)) * t + 0.5f);
    return liceColour(uint32_t((r << 16) | (g << 8) | b));
}

// The horizontal inset of a rounded rect at row `row` (device px).
int rowInset(int row, int height, float radius) {
    if (radius <= 0.f) return 0;
    const float r = radius;
    float dy = 0.f;
    if (row < r) dy = r - float(row) - 0.5f;
    else if (row >= float(height) - r) dy = float(row) - (float(height) - r) + 0.5f;
    else return 0;
    const float d = r * r - dy * dy;
    if (d <= 0.f) return int(r);
    return int(r - std::sqrt(d) + 0.5f);
}
} // namespace

double Canvas::pixelSizeFor(float designSize) const {
    const double size = designFontSize(designSize) * scale_;
    return std::max(1.0, std::floor(size + 0.5));
}

void Canvas::fillRect(float x, float y, float w, float h) {
    if (!target_ || w <= 0 || h <= 0) return;
    LICE_FillRect(target_, deviceX(x), deviceY(y), deviceSize(w), deviceSize(h), colour_, float(alpha_),
                  LICE_BLIT_MODE_COPY);
}

void Canvas::fillRect(uint32_t rgb, float x, float y, float w, float h) {
    if (!target_ || w <= 0 || h <= 0) return;
    LICE_FillRect(target_, deviceX(x), deviceY(y), deviceSize(w), deviceSize(h), liceColour(rgb), 1.f,
                  LICE_BLIT_MODE_COPY);
}

void Canvas::rectOutline(uint32_t rgb, float x, float y, float w, float h, float width) {
    if (!target_ || w <= 0 || h <= 0) return;
    const int px = deviceX(x), py = deviceY(y), pw = std::max(1, deviceSize(w)), ph = std::max(1, deviceSize(h));
    const int t = std::max(1, deviceSize(width));
    const LICE_pixel c = liceColour(rgb);
    LICE_FillRect(target_, px, py, pw, t, c, 1.f, LICE_BLIT_MODE_COPY);
    LICE_FillRect(target_, px, py + ph - t, pw, t, c, 1.f, LICE_BLIT_MODE_COPY);
    LICE_FillRect(target_, px, py, t, ph, c, 1.f, LICE_BLIT_MODE_COPY);
    LICE_FillRect(target_, px + pw - t, py, t, ph, c, 1.f, LICE_BLIT_MODE_COPY);
}

void Canvas::roundedFill(uint32_t rgb, float x, float y, float w, float h, float radius) {
    if (!target_ || w <= 0 || h <= 0) return;
    fillRounded(deviceX(x), deviceY(y), deviceSize(w), deviceSize(h), float(deviceSize(radius)), liceColour(rgb),
                1.0);
}

// LICE_RoundRect() draws an OUTLINE only (lice_arc.cpp: four lines + four
// arcs), so a filled rounded rect has to be scanlined by hand.
void Canvas::fillRounded(int px, int py, int pw, int ph, float radius, LICE_pixel colour, double alpha) {
    if (pw <= 0 || ph <= 0) return;
    for (int row = 0; row < ph; ++row) {
        const int inset = rowInset(row, ph, radius);
        const int width = pw - 2 * inset;
        if (width <= 0) continue;
        LICE_FillRect(target_, px + inset, py + row, width, 1, colour, float(alpha), LICE_BLIT_MODE_COPY);
    }
}

void Canvas::roundedGradient(uint32_t top, uint32_t bottom, float x, float y, float w, float h, float radius) {
    if (!target_ || w <= 0 || h <= 0) return;
    const int px = deviceX(x), py = deviceY(y), pw = deviceSize(w), ph = deviceSize(h);
    const float r = float(std::max(0, deviceSize(radius)));
    for (int row = 0; row < ph; ++row) {
        const int inset = rowInset(row, ph, r);
        const int width = pw - 2 * inset;
        if (width <= 0) continue;
        const float t = ph > 1 ? float(row) / float(ph - 1) : 0.f;
        LICE_FillRect(target_, px + inset, py + row, width, 1, blendColour(top, bottom, t), 1.f, LICE_BLIT_MODE_COPY);
    }
}

void Canvas::roundedRing(uint32_t rgb, float x, float y, float w, float h, float radius, float width, double alpha) {
    if (!target_ || w <= 0 || h <= 0) return;
    const int px = deviceX(x), py = deviceY(y), pw = deviceSize(w), ph = deviceSize(h);
    const int t = std::max(1, deviceSize(width));
    const int r = std::max(0, deviceSize(radius));
    if (pw <= 0 || ph <= 0 || px >= target_->getWidth() || py >= target_->getHeight()) return;
    // Snapshot the area, paint the outer rounded rect, put the inside back:
    // exact, no corner notches, works over any background and with alpha.
    const int x0 = std::max(0, px), y0 = std::max(0, py);
    const int x1 = std::min(target_->getWidth(), px + pw), y1 = std::min(target_->getHeight(), py + ph);
    const int saveW = x1 - x0, saveH = y1 - y0;
    if (saveW <= 0 || saveH <= 0) return;
    const int span = target_->getRowSpan();
    std::vector<LICE_pixel> saved(size_t(saveW) * size_t(saveH));
    for (int row = 0; row < saveH; ++row)
        std::copy_n(target_->getBits() + size_t(y0 + row) * size_t(span) + size_t(x0), saveW,
                    saved.begin() + size_t(row) * size_t(saveW));
    fillRounded(px, py, pw, ph, float(r), liceColour(rgb), alpha);
    for (int row = t; row < ph - t; ++row) {
        const int inset = t + rowInset(row - t, ph - 2 * t, float(std::max(0, r - t)));
        const int width = pw - 2 * inset;
        if (width <= 0) continue;
        const int dstY = py + row;
        if (dstY < y0 || dstY >= y1) continue;
        const int startX = std::max(px + inset, x0), endX = std::min(px + inset + width, x1);
        if (endX <= startX) continue;
        std::copy_n(saved.begin() + size_t(dstY - y0) * size_t(saveW) + size_t(startX - x0), endX - startX,
                    target_->getBits() + size_t(dstY) * size_t(span) + size_t(startX));
    }
}

void Canvas::linearGradient(uint32_t top, uint32_t bottom, float x, float y, float w, float h) {
    if (!target_ || w <= 0 || h <= 0) return;
    const int px = deviceX(x), py = deviceY(y), pw = std::max(1, deviceSize(w)), ph = std::max(1, deviceSize(h));
    const float dr = float(colourRed(bottom) - colourRed(top)) / 255.f / float(ph);
    const float dg = float(colourGreen(bottom) - colourGreen(top)) / 255.f / float(ph);
    const float db = float(colourBlue(bottom) - colourBlue(top)) / 255.f / float(ph);
    LICE_GradRect(target_, px, py, pw, ph, colourRed(top) / 255.f, colourGreen(top) / 255.f, colourBlue(top) / 255.f,
                  1.f, 0.f, 0.f, 0.f, 0.f, dr, dg, db, 0.f, LICE_BLIT_MODE_COPY);
}

void Canvas::radialGradient(uint32_t inner, uint32_t outer, float cx, float cy, float radius, float innerCx,
                            float innerCy) {
    if (!target_ || radius <= 0) return;
    const int px = deviceX(cx - radius), py = deviceY(cy - radius);
    const int size = deviceSize(2 * radius);
    if (size <= 0) return;
    const double centreX = deviceX(cx), centreY = deviceY(cy);
    const double brightX = deviceX(innerCx), brightY = deviceY(innerCy);
    const double r = double(deviceSize(radius));
    const double far = std::max(1.0, r + std::hypot(brightX - centreX, brightY - centreY));
    const int span = target_->getRowSpan();
    for (int row = 0; row < size; ++row) {
        const int y = py + row;
        if (y < 0 || y >= target_->getHeight()) continue;
        LICE_pixel *dst = target_->getBits() + size_t(y) * size_t(span);
        for (int column = 0; column < size; ++column) {
            const int x = px + column;
            if (x < 0 || x >= target_->getWidth()) continue;
            const double dx = double(x) + 0.5 - centreX, dy = double(y) + 0.5 - centreY;
            const double dist = std::hypot(dx, dy);
            // Coverage of the knob disc, and the gradient from the highlight.
            const double coverage = std::clamp(r - dist + 0.5, 0.0, 1.0);
            if (coverage <= 0.0) continue;
            const double t = std::clamp(std::hypot(double(x) + 0.5 - brightX, double(y) + 0.5 - brightY) / far, 0.0,
                                        1.0);
            const int sr = int(colourRed(inner) + (colourRed(outer) - colourRed(inner)) * t + 0.5);
            const int sg = int(colourGreen(inner) + (colourGreen(outer) - colourGreen(inner)) * t + 0.5);
            const int sb = int(colourBlue(inner) + (colourBlue(outer) - colourBlue(inner)) * t + 0.5);
            const double a = coverage;
            LICE_pixel &pixel = dst[x];
            const int dr = int(pixel & 0xFF), dg = int((pixel >> 8) & 0xFF), db = int((pixel >> 16) & 0xFF);
            const int orr = int(dr + (sr - dr) * a + 0.5);
            const int og = int(dg + (sg - dg) * a + 0.5);
            const int ob = int(db + (sb - db) * a + 0.5);
            pixel = LICE_RGBA(orr, og, ob, 255);
        }
    }
}

void Canvas::line(uint32_t rgb, float x1, float y1, float x2, float y2, bool aa) {
    if (!target_) return;
    LICE_Line(target_, deviceX(x1), deviceY(y1), deviceX(x2), deviceY(y2), liceColour(rgb), 1.f, LICE_BLIT_MODE_COPY,
              aa);
}

void Canvas::lineAt(float x1, float y1, float x2, float y2, bool aa) {
    if (!target_) return;
    LICE_Line(target_, deviceX(x1), deviceY(y1), deviceX(x2), deviceY(y2), colour_, float(alpha_), LICE_BLIT_MODE_COPY,
              aa);
}

void Canvas::thickLine(uint32_t rgb, float x1, float y1, float x2, float y2, float width) {
    if (!target_) return;
    LICE_ThickFLine(target_, double(deviceX(x1)), double(deviceY(y1)), double(deviceX(x2)), double(deviceY(y2)),
                    liceColour(rgb), 1.f, LICE_BLIT_MODE_COPY, std::max(1, deviceSize(width)));
}

void Canvas::circle(uint32_t rgb, float cx, float cy, float radius) {
    if (!target_) return;
    LICE_FillCircle(target_, float(deviceX(cx)), float(deviceY(cy)), float(deviceSize(radius)), liceColour(rgb), 1.f,
                    LICE_BLIT_MODE_COPY, true);
}

void Canvas::circleAt(float cx, float cy, float radius) {
    if (!target_) return;
    LICE_FillCircle(target_, float(deviceX(cx)), float(deviceY(cy)), float(deviceSize(radius)), colour_, float(alpha_),
                    LICE_BLIT_MODE_COPY, true);
}

void Canvas::ringAt(float cx, float cy, float radius) {
    if (!target_) return;
    LICE_Circle(target_, float(deviceX(cx)), float(deviceY(cy)), float(deviceSize(radius)), colour_, float(alpha_),
                LICE_BLIT_MODE_COPY, true);
}

void Canvas::roundedFillAt(float x, float y, float w, float h, float radius) {
    if (!target_) return;
    fillRounded(deviceX(x), deviceY(y), deviceSize(w), deviceSize(h), float(deviceSize(radius)), colour_, alpha_);
}

void Canvas::ring(uint32_t rgb, float cx, float cy, float radius) {
    if (!target_) return;
    LICE_Circle(target_, float(deviceX(cx)), float(deviceY(cy)), float(deviceSize(radius)), liceColour(rgb), 1.f,
                LICE_BLIT_MODE_COPY, true);
}

void Canvas::arc(uint32_t rgb, float cx, float cy, float radius, float a0, float a1) {
    if (!target_) return;
    LICE_Arc(target_, float(deviceX(cx)), float(deviceY(cy)), float(deviceSize(radius)), a0, a1, liceColour(rgb), 1.f,
             LICE_BLIT_MODE_COPY, true);
}

void Canvas::bezier(uint32_t rgb, float x0, float y0, float cx, float cy, float x1, float y1) {
    if (!target_) return;
    LICE_DrawQBezier(target_, double(deviceX(x0)), double(deviceY(y0)), double(deviceX(cx)), double(deviceY(cy)),
                     double(deviceX(x1)), double(deviceY(y1)), liceColour(rgb), 1.f, LICE_BLIT_MODE_COPY, true, 0.0);
}

double Canvas::textWidth(const std::string &utf8, float size, bool mono) const {
    if (!target_) return 0;
    return text::measure(utf8, pixelSizeFor(size), mono).width / std::max(1.0, scale_);
}

void Canvas::text(const std::string &utf8, float x, float y, float w, float h, float size, uint32_t rgb, bool bold,
                  bool centred, bool mono) {
    (void)bold; // the faces are bold by construction
    if (!target_ || utf8.empty() || w <= 2.f || h <= 1.f) return;
    const double pixelSize = pixelSizeFor(size);
    const double boxLeft = x * scale_, boxTop = y * scale_, boxWidth = w * scale_, boxHeight = h * scale_;
    std::string shown = utf8;
    text::Extents extents = text::measure(shown, pixelSize, mono);
    if (extents.width > boxWidth - 2 * scale_) {
        // Long text is ellipsized: the longest prefix that fits plus U+2026.
        static const std::string ellipsis = "\xE2\x80\xA6";
        const double dots = text::measure(ellipsis, pixelSize, mono).width;
        if (dots > boxWidth) return;
        size_t low = 0, high = utf8.size();
        while (low < high) {
            const size_t mid = low + (high - low + 1) / 2;
            size_t cut = mid;
            while (cut > 0 && (static_cast<unsigned char>(utf8[cut]) & 0xC0) == 0x80) --cut;
            if (text::measure(utf8.substr(0, cut) + ellipsis, pixelSize, mono).width <= boxWidth - 2 * scale_)
                low = mid;
            else
                high = cut ? cut - 1 : 0;
        }
        size_t cut = low;
        while (cut > 0 && (static_cast<unsigned char>(utf8[cut]) & 0xC0) == 0x80) --cut;
        shown = utf8.substr(0, cut) + ellipsis;
        extents = text::measure(shown, pixelSize, mono);
    }
    const double left = centred ? boxLeft + (boxWidth - extents.width) * 0.5 : boxLeft + scale_;
    const double baseline = boxTop + (boxHeight - (extents.ascent + extents.descent)) * 0.5 + extents.ascent;
    text::draw(target_, shown, left, baseline, pixelSize, mono, rgb, 1.0);
}

} // namespace oxo
