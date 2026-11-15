# HostTest — the controlled test host

An X11/XEmbed host that opens one plugin editor and pumps it through
`Linux::IRunLoop`, the way a DAW does: the controlled host for editor and
run-loop work, without a DAW in the way. It is the SDK's `editorhost` sample
(`public.sdk/samples/vst-hosting/editorhost`, BSD-licensed) with its Linux run
loop corrected; `third_party/` is never edited. Test tooling only — built with
`-DOXO_HOST_TEST=ON` (target `oxo_editorhost`), never part of a release.

## Files

`src/source/` is the SDK's `source/` folder with its includes rewritten to
local paths (`"editorhost.h"`, `"platform/…"`) and `platform/linux/runloop.cpp`
corrected.

## Build and run

    cmake -S linux-port-lice -B linux-port-lice/build -DOXO_HOST_TEST=ON
    cmake --build linux-port-lice/build --target oxo_editorhost OxoSynth2VST3

    HOME=$(mktemp -d) DISPLAY=:0 \
      linux-port-lice/build/bin/Release/oxo_editorhost \
      linux-port-lice/build/VST3/Release/OxoSynth2.vst3

`HOME=$(mktemp -d)` keeps the developer's patch library out of the test (the
plugin creates its bank library on first load).

## Checking what the editor drew (`xgrab.c`)

    cc -O2 xgrab.c -o xgrab -lX11
    ./xgrab <window-id> out.ppm && ffmpeg -i out.ppm out.png

**On a Wayland session the X server is XWayland**, and a root capture
(`ffmpeg -f x11grab`, `import -window root`) is black even when the plugin
draws correctly: XWayland keeps no composited copy of window content in the
root. `xgrab` reads the window's own content with `XGetImage`. Grab the
host's top-level window (`xwininfo -root -tree` → the `"Editor"` window),
which composites the embedded plugin window: XWayland refuses a full-width
`XGetImage` on the embedded child itself (`BadMatch` at exactly the window
width — 769 px works, 770 does not).

## Driving the editor from a script (`xsend.c`)

    cc -O2 xsend.c -o xsend $(pkg-config --cflags --libs x11)
    ./xsend <window-id> down <x> <y>       # button 1 press at view coordinates
    ./xsend <window-id> up <x> <y>
    ./xsend <window-id> move <x> <y>
    ./xsend <window-id> double <x> <y>     # two clicks 80 ms apart
    ./xsend <window-id> down <x> <y> 4     # wheel up (X11 button 4); 5 = wheel down

`xdotool mousemove` cannot be used on a Wayland session: the compositor owns
the pointer and ignores warps from X clients, so the click lands wherever the
pointer already was. `xsend` posts the events to the window directly with
`XSendEvent`, which is what a plugin window sees anyway.
