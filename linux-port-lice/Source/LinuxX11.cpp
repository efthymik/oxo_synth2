#include "LinuxX11.hpp"

#include <xcb/xcb_keysyms.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace oxo::x11 {

namespace {
constexpr uint32_t kXEmbedVersion = 0;
} // namespace

View::~View() { close(); }

bool View::open(uintptr_t parent, int width, int height, Listener *listener) {
    if (connection_) return true;
    int screenIndex = 0;
    connection_ = xcb_connect(nullptr, &screenIndex);
    if (!connection_ || xcb_connection_has_error(connection_)) {
        // xcb_connect never returns null: a failed connect is a connection in
        // the error state. The usual cause is the server's client limit
        // (Xorg/Xwayland MaxClients, 256 per display) being reached — one
        // connection per open editor counts against the whole desktop.
        std::fprintf(stderr,
                     "Oxo Synth2: cannot connect to the X display (the server may be at its client limit, "
                     "MaxClients = 256 per display); the editor will not open, the audio side is unaffected\n");
        close();
        return false;
    }
    const xcb_setup_t *setup = xcb_get_setup(connection_);
    xcb_screen_iterator_t screens = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < screenIndex && screens.rem; ++i) xcb_screen_next(&screens);
    screen_ = screens.data;
    if (!screen_) {
        close();
        return false;
    }
    listener_ = listener;
    width_ = std::max(1, width);
    height_ = std::max(1, height);

    // The child inherits the parent's depth, visual and colormap
    // (CopyFromParent): passing them explicitly is what X rejects with
    // BadMatch when the host made its parent window with a private colormap.
    window_ = xcb_generate_id(connection_);
    const uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    const uint32_t values[] = {
        screen_->white_pixel,
        XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
            XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE |
            XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW | XCB_EVENT_MASK_STRUCTURE_NOTIFY |
            XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_PROPERTY_CHANGE};
    xcb_void_cookie_t createCookie =
        xcb_create_window_checked(connection_, XCB_COPY_FROM_PARENT, window_, xcb_window_t(parent), 0, 0,
                                  (uint16_t)width_, (uint16_t)height_, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                                  XCB_COPY_FROM_PARENT, mask, values);
    if (xcb_generic_error_t *error = xcb_request_check(connection_, createCookie)) {
        std::fprintf(stderr, "Oxo Synth2: create_window failed (%d)\n", int(error->error_code));
        free(error);
        close();
        return false;
    }
    // Our depth follows the parent's; blitting needs to know it.
    xcb_get_geometry_cookie_t geometryCookie = xcb_get_geometry(connection_, window_);
    if (xcb_get_geometry_reply_t *geometry = xcb_get_geometry_reply(connection_, geometryCookie, nullptr)) {
        depth_ = geometry->depth;
        free(geometry);
    }

    // The XEmbed protocol announcement: the host refuses a plug window without
    // it (the SDK's editorhost exits), and it is what VSTGUI's child window
    // sets too. XEMBED_PROTOCOL_VERSION 0, flags 0 (not mapped yet).
    const char *xembedInfoName = "_XEMBED_INFO";
    xcb_intern_atom_cookie_t atomCookie = xcb_intern_atom(connection_, 0, (uint16_t)std::strlen(xembedInfoName),
                                                          xembedInfoName);
    xcb_intern_atom_reply_t *atomReply = xcb_intern_atom_reply(connection_, atomCookie, nullptr);
    if (atomReply) {
        // Both the property and its type are the _XEMBED_INFO atom (the XEmbed
        // spec; hosts read it with a typed XGetWindowProperty, so a CARDINAL
        // property reads as absent).
        const uint32_t info[2] = {kXEmbedVersion, 0}; // protocol version, flags (not mapped)
        xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, window_, atomReply->atom, atomReply->atom, 32, 2,
                            info);
        free(atomReply);
    }
    gc_ = xcb_generate_id(connection_);
    xcb_create_gc(connection_, gc_, window_, 0, nullptr);
    symbols_ = xcb_key_symbols_alloc(connection_);
    xcb_map_window(connection_, window_);
    flush();
    return true;
}

void View::close() {
    if (symbols_) {
        xcb_key_symbols_free(symbols_);
        symbols_ = nullptr;
    }
    if (connection_) {
        if (gc_) xcb_free_gc(connection_, gc_);
        if (window_) xcb_destroy_window(connection_, window_);
        xcb_flush(connection_);
        xcb_disconnect(connection_);
    }
    connection_ = nullptr;
    screen_ = nullptr;
    window_ = 0;
    gc_ = 0;
    listener_ = nullptr;
    scratch_.clear();
}

int View::fd() const { return connection_ ? xcb_get_file_descriptor(connection_) : -1; }

void View::resize(int width, int height) {
    if (!connection_ || !window_) return;
    width_ = std::max(1, width);
    height_ = std::max(1, height);
    const uint32_t values[] = {(uint32_t)width_, (uint32_t)height_};
    xcb_configure_window(connection_, window_, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, values);
    flush();
}

void View::grabFocus() {
    if (!connection_ || !window_) return;
    xcb_set_input_focus(connection_, XCB_INPUT_FOCUS_PARENT, window_, XCB_CURRENT_TIME);
    flush();
}

void View::flush() {
    if (connection_) xcb_flush(connection_);
}

Modifiers View::modifiers(uint16_t state) const {
    Modifiers mods;
    mods.shift = (state & XCB_KEY_BUT_MASK_SHIFT) != 0;
    mods.ctrl = (state & XCB_KEY_BUT_MASK_CONTROL) != 0;
    mods.alt = (state & XCB_MOD_MASK_1) != 0;
    mods.super = (state & XCB_MOD_MASK_4) != 0;
    return mods;
}

void View::ensureFormat() {
    if (ownFormat_ || depth_ == 24 || depth_ == 32) return;
    // Unusual depth: look the visual's masks up once and convert.
    xcb_depth_iterator_t depths = xcb_screen_allowed_depths_iterator(screen_);
    for (; depths.rem; xcb_depth_next(&depths)) {
        if (depths.data->depth != depth_) continue;
        xcb_visualtype_iterator_t visuals = xcb_depth_visuals_iterator(depths.data);
        for (; visuals.rem; xcb_visualtype_next(&visuals)) {
            if (visuals.data->_class != XCB_VISUAL_CLASS_TRUE_COLOR) continue;
            redMask_ = visuals.data->red_mask;
            greenMask_ = visuals.data->green_mask;
            blueMask_ = visuals.data->blue_mask;
            ownFormat_ = false;
            return;
        }
    }
}

void View::putImage(const unsigned char *data, int stride, int x, int y, int width, int height) {
    if (!connection_ || !window_ || width <= 0 || height <= 0) return;
    ensureFormat();
    // xcb_put_image wants the rows packed with no padding, so a rectangle
    // narrower than the surface is copied out first (the dirty rects are
    // small; a full-width blit is sent straight from the surface).
    const size_t rowBytes = size_t(width) * 4;
    const uint8_t *source = data + size_t(y) * size_t(stride) + size_t(x) * 4;
    if (rowBytes != size_t(stride) || x != 0) {
        packed_.resize(rowBytes * size_t(height));
        for (int row = 0; row < height; ++row)
            std::memcpy(packed_.data() + size_t(row) * rowBytes, source + size_t(row) * size_t(stride), rowBytes);
        source = packed_.data();
    }
    if (depth_ == 24) {
        // The buffer is 0xAARRGGBB, i.e. B,G,R,A in memory: the same bytes a
        // depth-24 ZPixmap wants (the fourth byte is ignored).
        // xcb_put_image takes the data length in BYTES (it converts to the
        // protocol's 4-byte units itself).
        xcb_put_image(connection_, XCB_IMAGE_FORMAT_Z_PIXMAP, window_, gc_, (uint16_t)width, (uint16_t)height,
                      (int16_t)x, (int16_t)y, 0, depth_, (uint32_t)(rowBytes * size_t(height)), source);
    } else {
        // Depth 32 (an alpha-carrying visual) or an exotic depth: build a
        // correctly packed, opaque image.
        scratch_.resize(rowBytes * size_t(height));
        auto *out = reinterpret_cast<uint32_t *>(scratch_.data());
        for (int row = 0; row < height; ++row) {
            const auto *in = reinterpret_cast<const uint32_t *>(source + size_t(row) * rowBytes);
            for (int column = 0; column < width; ++column) {
                const uint32_t pixel = in[column];
                const uint32_t r = (pixel >> 16) & 0xFF, g = (pixel >> 8) & 0xFF, b = pixel & 0xFF;
                uint32_t value = ((r << 16) | (g << 8) | b);
                if (depth_ == 32) {
                    value |= 0xFF000000u; // opaque: the window is not translucent
                } else {
                    auto pack = [](uint32_t component, uint32_t mask) {
                        if (!mask) return 0u;
                        int shift = 0;
                        while (((mask >> shift) & 1u) == 0 && shift < 32) ++shift;
                        uint32_t bits = 0;
                        while (((mask >> (shift + bits)) & 1u) != 0 && shift + bits < 32) ++bits;
                        if (!bits) return 0u;
                        const uint32_t maximum = (1u << bits) - 1u;
                        return ((component * maximum + 127) / 255) << shift;
                    };
                    value = pack(r, redMask_) | pack(g, greenMask_) | pack(b, blueMask_);
                }
                out[size_t(row) * size_t(width) + size_t(column)] = value;
            }
        }
        xcb_put_image(connection_, XCB_IMAGE_FORMAT_Z_PIXMAP, window_, gc_, (uint16_t)width, (uint16_t)height,
                      (int16_t)x, (int16_t)y, 0, depth_, (uint32_t)scratch_.size(), scratch_.data());
    }
    flush();
}

std::string View::textForKey(unsigned long keysym) const {
    // X11 keysyms: Latin-1 range maps 1:1, 0x01000000 + Unicode for the rest.
    uint32_t code = 0;
    if (keysym >= 0x20 && keysym <= 0x7E) code = uint32_t(keysym);
    else if (keysym >= 0xA0 && keysym <= 0xFF) code = uint32_t(keysym);
    else if (keysym >= 0x01000100 && keysym <= 0x0110FFFF) code = uint32_t(keysym - 0x01000000);
    else return {};
    std::string out;
    if (code < 0x80) {
        out.push_back(char(code));
    } else if (code < 0x800) {
        out.push_back(char(0xC0 | (code >> 6)));
        out.push_back(char(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
        out.push_back(char(0xE0 | (code >> 12)));
        out.push_back(char(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(char(0x80 | (code & 0x3F)));
    } else {
        out.push_back(char(0xF0 | (code >> 18)));
        out.push_back(char(0x80 | ((code >> 12) & 0x3F)));
        out.push_back(char(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(char(0x80 | (code & 0x3F)));
    }
    return out;
}

bool View::pump() {
    if (!connection_) return false;
    bool any = false;
    while (xcb_generic_event_t *event = xcb_poll_for_event(connection_)) {
        handle(event);
        free(event);
        any = true;
    }
    return any;
}

void View::handle(xcb_generic_event_t *event) {
    if (!listener_) return;
    const uint8_t type = event->response_type & 0x7F;
    switch (type) {
    case 0: { // an X error, delivered as an event: log a few, never exit
        auto *error = reinterpret_cast<xcb_generic_error_t *>(event);
        static int logged = 0;
        if (logged++ < 8)
            std::fprintf(stderr, "Oxo Synth2: X error %d (request %d) ignored\n", int(error->error_code),
                         int(error->major_code));
        return;
    }
    case XCB_EXPOSE: {
        auto *expose = reinterpret_cast<xcb_expose_event_t *>(event);
        if (expose->count == 0)
            listener_->onExpose(expose->x, expose->y, expose->width, expose->height);
        return;
    }
    case XCB_CONFIGURE_NOTIFY: {
        auto *configure = reinterpret_cast<xcb_configure_notify_event_t *>(event);
        width_ = std::max(1, int(configure->width));
        height_ = std::max(1, int(configure->height));
        listener_->onConfigure(width_, height_);
        return;
    }
    case XCB_BUTTON_PRESS:
    case XCB_BUTTON_RELEASE: {
        auto *button = reinterpret_cast<xcb_button_press_event_t *>(event);
        // X buttons 4..7 are the wheel: report them as scrolls, not buttons.
        if (type == XCB_BUTTON_PRESS && button->detail >= 4 && button->detail <= 7) {
            const int direction = (button->detail == 4 || button->detail == 6) ? 1 : -1;
            listener_->onScroll(direction, button->event_x, button->event_y);
            return;
        }
        listener_->onButton(button->detail - 1, type == XCB_BUTTON_PRESS, button->event_x, button->event_y,
                            modifiers(button->state), button->time);
        return;
    }
    case XCB_MOTION_NOTIFY: {
        auto *motion = reinterpret_cast<xcb_motion_notify_event_t *>(event);
        listener_->onMotion(motion->event_x, motion->event_y, modifiers(motion->state));
        return;
    }
    case XCB_LEAVE_NOTIFY:
        listener_->onLeave();
        return;
    case XCB_FOCUS_IN:
        listener_->onFocus(true);
        return;
    case XCB_FOCUS_OUT:
        listener_->onFocus(false);
        return;
    case XCB_KEY_PRESS:
    case XCB_KEY_RELEASE: {
        auto *key = reinterpret_cast<xcb_key_press_event_t *>(event);
        if (!symbols_) return;
        // Shift selects the second column of the keyboard map; the keysym is
        // what text input needs, the editor maps the rest.
        const int column = (key->state & XCB_KEY_BUT_MASK_SHIFT) ? 1 : 0;
        const xcb_keysym_t symbol = xcb_key_symbols_get_keysym(symbols_, key->detail, column);
        if (symbol) listener_->onKey(symbol, type == XCB_KEY_PRESS, modifiers(key->state));
        return;
    }
    case XCB_DESTROY_NOTIFY:
        window_ = 0;
        listener_->onDestroy();
        return;
    default:
        return;
    }
}

} // namespace oxo::x11
