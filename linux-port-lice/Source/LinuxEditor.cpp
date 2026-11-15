#include "LinuxEditor.hpp"

#include "OxoSynth2VST3/source/OxoSynth2Controller.h"
#include "OxoSynth2Identity.h"
#include "EditorCore/Settings.hpp"
#include "EditorCore/Storage.hpp"
#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/gui/iplugview.h"

#include "pluginterfaces/base/keycodes.h" // KEY_UP, kControlKey (IPlugView::onKeyDown)
#include <X11/keysym.h> // the X key symbols the editor maps

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

using namespace Steinberg;

namespace oxo {

namespace {
// The host run loop's pump: DPF's interval (DistrhoUIVST3.cpp, ~60 fps). It
// also carries the 30 Hz parameter polling.
constexpr Linux::TimerInterval kPumpMs = 16;
constexpr uint64_t kPollMs = 33;
constexpr uint32_t kDoubleClickMs = 400;
constexpr float kDoubleClickPixels = 3.f;
constexpr int kMinWidth = 578, kMaxWidth = 2310; // 75 % .. 300 % of the 770-px design

// Nothing may throw into the host: a DAW whose event loop unwinds through a
// plugin is a DAW crash. Every host-facing entry point runs inside this.
template <class F>
void guard(const char *what, F &&body) {
    try {
        body();
    } catch (const std::exception &error) {
        static int logged = 0;
        if (logged++ < 4) std::fprintf(stderr, "Oxo Synth2: %s threw (%s); ignored\n", what, error.what());
    } catch (...) {
        static int logged = 0;
        if (logged++ < 4) std::fprintf(stderr, "Oxo Synth2: %s threw; ignored\n", what);
    }
}

uint64_t nowMs() {
    using namespace std::chrono;
    return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// The editor's zoom preference: key "linuxEditorWidth" of the per-user
// settings.json, the same shape as the Windows port's "windowsEditorWidth".
int LoadEditorWidth() {
    const std::optional<std::string> token = settings::Load(storage::Root(), "linuxEditorWidth");
    if (!token) return int(LinuxPanel::kDesignWidth);
    int width = 0;
    const char *end = token->data() + token->size();
    const auto result = std::from_chars(token->data(), end, width);
    if (result.ec != std::errc{} || result.ptr != end || (*token)[0] < '0' || (*token)[0] > '9' ||
        width < kMinWidth || width > kMaxWidth)
        return int(LinuxPanel::kDesignWidth);
    return width;
}
} // namespace

// The host IRunLoop handlers (the same shape as the SDK's
// vstgui_linux_runloop_support.cpp: a small FUnknown wrapper each).
namespace {

struct PumpTimer : U::Implements<U::Directly<Linux::ITimerHandler>> {
    LinuxEditor *editor = nullptr;
    void PLUGIN_API onTimer() final {
        if (editor) editor->tick();
    }
};

struct PumpEvents : U::Implements<U::Directly<Linux::IEventHandler>> {
    LinuxEditor *editor = nullptr;
    void PLUGIN_API onFDIsSet(Linux::FileDescriptor) final {
        if (editor) editor->tick();
    }
};

} // namespace

struct LinuxEditor::RunLoop {
    // The host's run loop (not owned), registered while attached.
    FUnknownPtr<Linux::IRunLoop> loop;
    IPtr<PumpTimer> timer;
    IPtr<PumpEvents> events;

    ~RunLoop() {
        if (loop) {
            if (events) loop->unregisterEventHandler(events);
            if (timer) loop->unregisterTimer(timer);
        }
    }
};

LinuxEditor::LinuxEditor(OxoSynth2Controller *controller)
    : CPluginView(nullptr), controller_(controller) {
    const int width = LoadEditorWidth();
    rect = ViewRect(0, 0, width, (Steinberg::int32)std::lround(width * (double)LinuxPanel::kDesignHeight /
                                                               LinuxPanel::kDesignWidth));
    if (controller_) hooks_ = controller_->MakeHooks();
    panel_ = std::make_unique<LinuxPanel>(&hooks_);
    panel_->setViewWidth(width);
}

LinuxEditor::~LinuxEditor() {
    removedFromParent();
    releaseSurfaces();
    panel_.reset(); // before hooks_ goes away
}

LinuxEditor::RunLoop &LinuxEditor::runLoop() {
    if (!runLoop_) runLoop_ = std::make_unique<RunLoop>();
    return *runLoop_;
}

tresult PLUGIN_API LinuxEditor::isPlatformTypeSupported(FIDString type) {
    return type && strcmp(type, kPlatformTypeX11EmbedWindowID) == 0 ? kResultTrue : kResultFalse;
}

tresult PLUGIN_API LinuxEditor::checkSizeConstraint(ViewRect *r) {
    if (!r) return kInvalidArgument;
    const int width = std::clamp(int(r->getWidth()), kMinWidth, kMaxWidth);
    r->right = r->left + width;
    r->bottom = r->top + int(std::lround(width * (double)LinuxPanel::kDesignHeight / LinuxPanel::kDesignWidth));
    return kResultTrue;
}

tresult PLUGIN_API LinuxEditor::onSize(ViewRect *newSize) {
    if (!newSize) return kInvalidArgument;
    rect = *newSize;
    guard("onSize", [this] {
        if (view_.isOpen()) view_.resize(rect.getWidth(), rect.getHeight());
    });
    return kResultTrue;
}

void LinuxEditor::attachedToParent() {
    guard("attachedToParent", [this] { attachedBody(); });
}

void LinuxEditor::attachedBody() {
    if (!systemWindow || view_.isOpen()) return;
    if (!view_.open(reinterpret_cast<uintptr_t>(systemWindow), rect.getWidth(), rect.getHeight(), this))
        return; // open() logged the cause
    // Text needs the connection for the desktop's Xft settings (hinting,
    // subpixel); the panel draws nothing before this point.
    text::init(view_.connection());
    // The host pumps us: the connection's fd for input, plus a timer for the
    // polling loop. Without a run loop the UI cannot run.
    runLoop().loop = FUnknownPtr<Linux::IRunLoop>(plugFrame);
    if (runLoop().loop) {
        runLoop().events = IPtr<PumpEvents>(owned(new PumpEvents()));
        runLoop().events->editor = this;
        runLoop().timer = IPtr<PumpTimer>(owned(new PumpTimer()));
        runLoop().timer->editor = this;
        runLoop().loop->registerEventHandler(runLoop().events, view_.fd());
        runLoop().loop->registerTimer(runLoop().timer, kPumpMs);
    } else {
        std::fprintf(stderr, "Oxo Synth2: host provided no Linux::IRunLoop; editor idle\n");
    }
}

void LinuxEditor::removedFromParent() {
    runLoop_.reset(); // unregisters before the connection goes
    view_.close();
    releaseSurfaces();
}

void LinuxEditor::releaseSurfaces() {
    staticBitmap_.reset();
    backBitmap_.reset();
    canvas_.setTarget(nullptr, 1.0);
    surfaceWidth_ = surfaceHeight_ = 0;
    staticDirty_ = true;
}

// The static layer and the composed image, at the view's device size.
void LinuxEditor::ensureSurfaces() {
    const int width = std::max(1, view_.width());
    const int height = std::max(1, view_.height());
    if (backBitmap_ && surfaceWidth_ == width && surfaceHeight_ == height && !staticDirty_) return;
    if (!backBitmap_ || surfaceWidth_ != width || surfaceHeight_ != height) {
        backBitmap_ = std::make_unique<LICE_MemBitmap>(width, height, 4);
        staticBitmap_ = std::make_unique<LICE_MemBitmap>(width, height, 4);
        surfaceWidth_ = width;
        surfaceHeight_ = height;
        staticDirty_ = true;
    }
    if (!staticBitmap_ || !staticDirty_) return;
    Canvas staticCanvas(staticBitmap_.get(),
                        double(width) / LinuxPanel::kDesignWidth);
    panel_->drawStatic(staticCanvas);
    staticDirty_ = false;
}

void LinuxEditor::paint(int x, int y, int width, int height) {
    if (width <= 0 || height <= 0 || !view_.isOpen()) return;
    ensureSurfaces();
    if (!backBitmap_ || !staticBitmap_) return;
    // The cached static layer for the dirty rows, then the values. Drawing a
    // control that pokes out of the dirty rect is harmless: the back buffer
    // gets the same pixels it already had, and only the dirty rect is blitted
    // to the window.
    const int span = backBitmap_->getRowSpan();
    const int x0 = std::clamp(x, 0, surfaceWidth_), x1 = std::clamp(x + width, 0, surfaceWidth_);
    const int y0 = std::clamp(y, 0, surfaceHeight_), y1 = std::clamp(y + height, 0, surfaceHeight_);
    if (x1 <= x0 || y1 <= y0) return;
    LICE_pixel *back = backBitmap_->getBits();
    const LICE_pixel *cached = staticBitmap_->getBits();
    // The whole static layer, not just the dirty rows: the dynamic pass draws
    // every control, and a control that moved (a menu, a dialog) must not
    // leave its previous pixels behind outside the dirty rect.
    for (int row = 0; row < surfaceHeight_; ++row)
        std::copy_n(cached + size_t(row) * size_t(span), surfaceWidth_, back + size_t(row) * size_t(span));
    canvas_.setTarget(backBitmap_.get(), double(surfaceWidth_) / LinuxPanel::kDesignWidth);
    panel_->drawDynamic(canvas_);
    view_.putImage(reinterpret_cast<const unsigned char *>(back), span * int(sizeof(LICE_pixel)), x0, y0, x1 - x0,
                   y1 - y0);
}

void LinuxEditor::onExpose(int x, int y, int width, int height) {
    paint(x, y, width, height);
}

void LinuxEditor::onConfigure(int width, int height) {
    if (width <= 0 || height <= 0) return;
    rect = ViewRect(0, 0, width, height);
    panel_->setViewWidth(width);
    staticDirty_ = true; // the cached layer is at the old size
    paint(0, 0, width, height);
}

void LinuxEditor::deviceRect(const PanelRect &design, int &x, int &y, int &width, int &height) const {
    const double sx = (double)std::max(1, view_.width()) / LinuxPanel::kDesignWidth;
    const double sy = (double)std::max(1, view_.height()) / LinuxPanel::kDesignHeight;
    x = std::max(0, int(std::floor(design.x * sx)) - 2);
    y = std::max(0, int(std::floor(design.y * sy)) - 2);
    const int right = std::min(view_.width(), int(std::ceil((design.x + design.w) * sx)) + 2);
    const int bottom = std::min(view_.height(), int(std::ceil((design.y + design.h) * sy)) + 2);
    width = std::max(0, right - x);
    height = std::max(0, bottom - y);
}

void LinuxEditor::postDirty() {
    const PanelRect dirty = panel_->takeDirty();
    if (dirty.empty()) return;
    // A panel-wide change (theme, patch load) also invalidates the static layer.
    if (dirty.w >= LinuxPanel::kDesignWidth - 1.f && dirty.h >= LinuxPanel::kDesignHeight - 1.f) staticDirty_ = true;
    int x = 0, y = 0, width = 0, height = 0;
    deviceRect(dirty, x, y, width, height);
    if (width > 0 && height > 0) paint(x, y, width, height);
}

void LinuxEditor::onButton(int button, bool press, int x, int y, x11::Modifiers mods, uint32_t timeMs) {
    const double sx = (double)std::max(1, view_.width()) / LinuxPanel::kDesignWidth;
    const double sy = (double)std::max(1, view_.height()) / LinuxPanel::kDesignHeight;
    const float dx = float(x / sx), dy = float(y / sy);
    (void)mods;
    if (button == 2) { // right: the library menu
        if (press) panel_->rightClick(dx, dy);
        postDirty();
        return;
    }
    if (button != 0) return; // left only
    if (press) {
        // Keyboard focus follows a click (VSTGUI's x11frame does the same with
        // RevertToParent): the host owns the pointer, we need the keys.
        view_.grabFocus();
        const bool doubleClick = lastPressTime_ != 0 && timeMs >= lastPressTime_ &&
                                 timeMs - lastPressTime_ < kDoubleClickMs &&
                                 std::fabs(dx - lastPressX_) < kDoubleClickPixels &&
                                 std::fabs(dy - lastPressY_) < kDoubleClickPixels;
        lastPressTime_ = timeMs ? timeMs : 1;
        lastPressX_ = dx;
        lastPressY_ = dy;
        panel_->mouseDown(dx, dy, doubleClick);
    } else {
        panel_->mouseUp(dx, dy);
    }
    postDirty();
}

void LinuxEditor::onMotion(int x, int y, x11::Modifiers mods) {
    const double sx = (double)std::max(1, view_.width()) / LinuxPanel::kDesignWidth;
    const double sy = (double)std::max(1, view_.height()) / LinuxPanel::kDesignHeight;
    panel_->mouseMove(float(x / sx), float(y / sy), mods.shift);
    postDirty();
}

void LinuxEditor::onScroll(int direction, int x, int y) {
    const double sx = (double)std::max(1, view_.width()) / LinuxPanel::kDesignWidth;
    const double sy = (double)std::max(1, view_.height()) / LinuxPanel::kDesignHeight;
    if (direction && panel_->wheel(direction, float(x / sx), float(y / sy))) postDirty();
}

void LinuxEditor::onKey(unsigned long keysym, bool press, x11::Modifiers mods) {
    if (!press) return;
    if (mods.ctrl || mods.alt || mods.super) return; // the host's
    using Key = LinuxPanel::Key;
    Key key = Key::Escape;
    switch (keysym) {
    case XK_Escape: key = Key::Escape; break;
    case XK_Return:
    case XK_KP_Enter: key = Key::Return; break;
    case XK_BackSpace: key = Key::Backspace; break;
    case XK_Delete: key = Key::Delete; break;
    case XK_Home: key = Key::Home; break;
    case XK_End: key = Key::End; break;
    case XK_Up: key = Key::Up; break;
    case XK_Down: key = Key::Down; break;
    case XK_Left: key = Key::Left; break;
    case XK_Right: key = Key::Right; break;
    default:
        // Any other key is text input for the fields (ignored elsewhere).
        if (const std::string text = view_.textForKey(keysym); !text.empty()) {
            panel_->textInput(text);
            postDirty();
        }
        return;
    }
    if (panel_->keyDown(key)) postDirty();
}

void LinuxEditor::onLeave() {
    panel_->setHover(-1, -1);
    postDirty();
}

void LinuxEditor::onFocus(bool) {}

void LinuxEditor::onDestroy() {
    // The host destroyed the window under us: keep the editor object alive
    // (the host still owns the view) but stop drawing.
    releaseSurfaces();
}

tresult PLUGIN_API LinuxEditor::onKeyDown(char16, int16 keyCode, int16 modifiers) {
    if (modifiers & (kAlternateKey | kCommandKey | kControlKey)) return kResultFalse;
    using Key = LinuxPanel::Key;
    Key key = Key::Escape;
    switch (keyCode) {
    case KEY_UP: key = Key::Up; break;
    case KEY_DOWN: key = Key::Down; break;
    case KEY_LEFT: key = Key::Left; break;
    case KEY_RIGHT: key = Key::Right; break;
    case KEY_RETURN:
    case KEY_ENTER: key = Key::Return; break;
    case KEY_ESCAPE: key = Key::Escape; break;
    default: return kResultFalse;
    }
    if (!panel_->keyDown(key)) return kResultFalse;
    postDirty();
    return kResultTrue;
}

void LinuxEditor::tick() {
    guard("tick", [this] { tickBody(); });
}

void LinuxEditor::tickBody() {
    if (!view_.isOpen()) return;
    // A UI size picked in the logo menu: the host owns the window size, so ask
    // it to resize (the mac's resize callback, the Windows requestWidth).
    if (const int width = panel_->takeRequestedWidth(); width > 0 && plugFrame) {
        ViewRect wanted(0, 0, width, 0);
        checkSizeConstraint(&wanted);
        if (plugFrame->resizeView(this, &wanted) == kResultOk)
            settings::Save(storage::Root(), "linuxEditorWidth", std::to_string(wanted.getWidth()));
    }
    view_.pump(); // input first: an event may change what has to be drawn
    const uint64_t now = nowMs();
    if (now - lastPoll_ >= kPollMs) {
        lastPoll_ = now;
        panel_->poll(now);
        postDirty();
    }
}

} // namespace oxo
