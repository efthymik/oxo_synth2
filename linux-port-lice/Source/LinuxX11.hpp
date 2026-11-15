#pragma once

// The plugin window on xcb.
//
// The subset the editor needs: create an XEmbed window in the host's parent,
// select the input events, put an image and turn events into callbacks — on
// libxcb alone, plus libxcb-keysyms for key symbols.

#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>

#include <cstdint>
#include <string>
#include <vector>

namespace oxo::x11 {

struct Modifiers {
    bool shift = false, ctrl = false, alt = false, super = false;
};

// Delivered in device (window) pixels.
struct Listener {
    virtual ~Listener() = default;
    virtual void onExpose(int x, int y, int width, int height) = 0;
    virtual void onConfigure(int width, int height) = 0;
    virtual void onButton(int button, bool press, int x, int y, Modifiers mods, uint32_t timeMs) = 0;
    virtual void onMotion(int x, int y, Modifiers mods) = 0;
    virtual void onScroll(int direction, int x, int y) = 0; // +1 = up
    virtual void onKey(unsigned long keysym, bool press, Modifiers mods) = 0;
    virtual void onLeave() = 0;
    virtual void onFocus(bool in) = 0;
    virtual void onDestroy() = 0;
};

class View {
public:
    View() = default;
    ~View();
    View(const View &) = delete;
    View &operator=(const View &) = delete;

    // Creates the connection and the XEmbed window inside `parent`.
    bool open(uintptr_t parent, int width, int height, Listener *listener);
    void close();
    bool isOpen() const { return connection_ != nullptr; }

    int fd() const; // the connection's socket, for the host's run loop
    xcb_connection_t *connection() const { return connection_; }
    int width() const { return width_; }
    int height() const { return height_; }
    void resize(int width, int height);
    void grabFocus();
    void flush();

    // Blits an ARGB32 (B,G,R,A) image at (x, y).
    void putImage(const unsigned char *data, int stride, int x, int y, int width, int height);

    // Dispatches every pending event; true = something was delivered.
    bool pump();

    // The UTF-8 text a key press produces ("" = none).
    std::string textForKey(unsigned long keysym) const;

private:
    void handle(xcb_generic_event_t *event);
    Modifiers modifiers(uint16_t state) const;
    void ensureFormat();

    xcb_connection_t *connection_ = nullptr;
    xcb_screen_t *screen_ = nullptr;
    xcb_window_t window_ = 0;
    xcb_gcontext_t gc_ = 0; // xcb_put_image wants one (unused for ZPixmap)
    xcb_key_symbols_t *symbols_ = nullptr;
    Listener *listener_ = nullptr;
    int width_ = 0, height_ = 0;
    int depth_ = 24, bitsPerPixel_ = 32;
    bool ownFormat_ = true;      // 24/32 ZPixmap: the source buffer can be sent as is
    uint32_t redMask_ = 0xFF0000, greenMask_ = 0x00FF00, blueMask_ = 0x0000FF;
    std::vector<uint8_t> packed_;  // a narrow rect, packed for xcb_put_image
    std::vector<uint8_t> scratch_; // the converted image when the format differs
};

} // namespace oxo::x11
