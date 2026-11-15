# Linux port — LICE panel

An alternative Linux editor for the same plugin: the mac / Windows panel redrawn
on LICE (Cockos WDL), on a raw xcb XEmbed window, with system FreeType text.
It ships the same engine, state, banks and MIDI mapping as the other ports;
only the editor differs. The upstream Linux port (shared `DesktopEditor/`,
cairo + X11) lives in `Linux_Build/` and is the default one.

## Build

Root of the repo, after `git submodule update --init --recursive`:

```sh
./build_linux_lice.sh              # builds + validates
./build_linux_lice.sh --install    # ... and copies into ~/.vst3
```

The result is `linux-port-lice/build/VST3/Release/OxoSynth2.vst3`. The script
wipes its build folder first (a build dir shared between machines caches the
other machine's absolute paths), runs the Steinberg validator on the bundle
(the same 47 checks as on macOS and Windows) and the dependency check.

Needs a Linux x64 desktop with a C++20 compiler (GCC 13+ or Clang 16+), CMake
3.25+, `pkg-config`, and the development packages for libxcb, libxcb-keysyms,
FreeType, fontconfig and zlib — on Debian/Ubuntu
`libxcb1-dev libxcb-keysyms1-dev libfreetype-dev libfontconfig1-dev zlib1g-dev`,
on Arch `libxcb xcb-keysyms freetype2 fontconfig zlib`, on Fedora
`libxcb-devel xcb-util-keysyms-devel freetype-devel fontconfig-devel zlib-devel`.
The drawing engine (LICE) is compiled in, and text uses the system FreeType
like every other Linux UI; no toolkit (GTK, Qt) and no cairo are needed.

By hand:

```sh
cmake -S linux-port-lice -B linux-port-lice/build -DCMAKE_BUILD_TYPE=Release
cmake --build linux-port-lice/build -j"$(nproc)"
linux-port-lice/build/bin/Release/validator linux-port-lice/build/VST3/Release/OxoSynth2.vst3
mkdir -p ~/.vst3 && cp -R linux-port-lice/build/VST3/Release/OxoSynth2.vst3 ~/.vst3/
```

Plugin editors use X11 embedding, which on a Wayland session means XWayland —
the same path every Linux VST3 host uses.

## Test host

A minimal host (no DAW needed) opens the editor in an X11 window and pumps it
through the host's `Linux::IRunLoop`, the way a DAW does:

```sh
cmake -S linux-port-lice -B linux-port-lice/build -DOXO_HOST_TEST=ON
cmake --build linux-port-lice/build --target oxo_editorhost OxoSynth2VST3 -j"$(nproc)"
HOME=$(mktemp -d) DISPLAY=:0 \
  linux-port-lice/build/bin/Release/oxo_editorhost \
  linux-port-lice/build/VST3/Release/OxoSynth2.vst3
```

`HOME=$(mktemp -d)` keeps your own patch library out of the test.
`HostTest/README.md` explains how to script clicks, keys and screenshots.

## Dependency contract

`tools/check_deps.sh <so>` fails on a new NEEDED library or a leaked symbol:

```sh
tools/check_deps.sh linux-port-lice/build/VST3/Release/OxoSynth2.vst3/Contents/x86_64-linux/OxoSynth2.so
```

The plugin links libc, libm, libxcb, libxcb-keysyms, FreeType, fontconfig and
zlib (plus libpthread / libdl on glibc < 2.34, where they are separate
libraries); the C++ runtime is static, and only `GetPluginFactory`,
`ModuleEntry` and `ModuleExit` are exported (`vst3.map`).

## Files

- Settings and themes: `$XDG_CONFIG_HOME/Oxo Music/Oxo Synth2/` (default
  `~/.config/...`): `settings.json`, `themes.json`, `midi_mappings.json`.
  Shared with the other Linux port.
- Bank library: the desktop's Documents folder (`XDG_DOCUMENTS_DIR` in
  `user-dirs.dirs`, default `~/Documents`) / `Oxo Music/Oxo Synth2/`.
- Deleted banks and patches go to the XDG Trash (`~/.local/share/Trash`).
