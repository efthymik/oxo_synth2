#pragma once

#include "public.sdk/source/common/pluginview.h"
#include "EditorCore/OxoSynth2Bridge.h"
#include "LinuxCanvas.hpp"
#include "LinuxPanel.hpp"
#include "LinuxX11.hpp"

#include <cstdint>
#include <memory>

namespace oxo {
class OxoSynth2Controller;

// Linux IPlugView: an XEmbed window on raw xcb (LinuxX11) showing a LICE
// image that is blitted with XPutImage. Driven by the host's
// Linux::IRunLoop (the connection's fd + a 16 ms timer, the DPF model).
//
// The panel (LinuxPanel) draws in design coordinates into a device-sized
// image; this class owns the static layer (rebuilt only on resize / theme
// change), turns the panel's dirty rectangles into an immediate repaint +
// blit, and polls the host at 30 Hz for automation / patch loads.
class LinuxEditor final : public Steinberg::CPluginView, private x11::Listener {
public:
    explicit LinuxEditor(OxoSynth2Controller *controller);
    ~LinuxEditor() override;
    Steinberg::tresult PLUGIN_API isPlatformTypeSupported(Steinberg::FIDString type) override;
    Steinberg::tresult PLUGIN_API canResize() override { return Steinberg::kResultTrue; }
    Steinberg::tresult PLUGIN_API checkSizeConstraint(Steinberg::ViewRect *r) override;
    Steinberg::tresult PLUGIN_API onSize(Steinberg::ViewRect *r) override;
    void attachedToParent() override;
    void removedFromParent() override;
    // Keys from hosts that deliver them to the view (else the X event pump).
    Steinberg::tresult PLUGIN_API onKeyDown(Steinberg::char16 key, Steinberg::int16 keyCode,
                                            Steinberg::int16 modifiers) override;

    // Called from the run loop handlers.
    void tick();

private:
    struct RunLoop; // the host's IRunLoop handlers
    RunLoop &runLoop();
    // x11::Listener
    void onExpose(int x, int y, int width, int height) override;
    void onConfigure(int width, int height) override;
    void onButton(int button, bool press, int x, int y, x11::Modifiers mods, uint32_t timeMs) override;
    void onMotion(int x, int y, x11::Modifiers mods) override;
    void onScroll(int direction, int x, int y) override;
    void onKey(unsigned long keysym, bool press, x11::Modifiers mods) override;
    void onLeave() override;
    void onFocus(bool in) override;
    void onDestroy() override;

    void tickBody();
    void attachedBody();
    void paint(int x, int y, int width, int height);
    void ensureSurfaces();
    void releaseSurfaces();
    void postDirty();
    void deviceRect(const PanelRect &design, int &x, int &y, int &width, int &height) const;

    OxoSynth2Controller *controller_ = nullptr;
    OxoSynth2Hooks hooks_{};
    std::unique_ptr<LinuxPanel> panel_;
    std::unique_ptr<RunLoop> runLoop_;
    x11::View view_;
    // The static layer (panel without values) and the composed image, both at
    // device size; canvas_ draws into the latter.
    std::unique_ptr<LICE_MemBitmap> staticBitmap_;
    std::unique_ptr<LICE_MemBitmap> backBitmap_;
    Canvas canvas_;
    int surfaceWidth_ = 0, surfaceHeight_ = 0;
    bool staticDirty_ = true;
    uint64_t lastPoll_ = 0;
    // Double click (X11 has no such event): 400 ms / 3 px.
    uint32_t lastPressTime_ = 0;
    float lastPressX_ = 0, lastPressY_ = 0;
};
} // namespace oxo
