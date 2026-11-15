# WDL / LICE (vendored)

Subset of [WDL](https://github.com/justinfrankel/WDL) at commit
`6a67ec5788ca6ee252b19cd8071796db49d763a7` (2026-11-13). LICE is Cockos'
"Lightweight Image Compositing Engine", the drawing library behind REAPER's
UI since 2007 — zlib/libpng licence, stated in each file's header (no separate
LICENSE file upstream).

Taken: `lice/lice.{h,cpp}`, `lice/lice_line.cpp`, `lice/lice_arc.cpp`,
`lice/lice_bezier.h`, `lice/lice_combine.h`, `lice/lice_extended.h`,
`swell/swell-types.h`, `swell/swell.h`, `swell/swell-functions.h` (headers
only — SWELL's GTK implementation is not used) and `wdltypes.h`.

Not taken: the image codecs (png/jpg/gif/bmp/ico/pcx/lcf), the GL variants,
`lice_text*` (the panel's text is system FreeType, see
`linux-port-lice/Source/LinuxText.cpp`).

Changes to the vendored files: **none**. `lice.cpp` references three SWELL
memory-context functions from its unused `LICE_SysBitmap`;
`linux-port-lice/Source/LiceSwellStubs.cpp` provides them so SWELL/GTK is not
linked.

Two things to know when using it: `lice.h` includes `swell-types.h` on
non-Windows, which defines `min`/`max` macros (undefine them after including
`lice.h`, as `LinuxCanvas.hpp` does), and `LICE_GETR/G/B` are macros without
outer parentheses (`(v>>16)&0xff`) — unusable inside an expression; use the
`colourRed/Green/Blue` helpers in `LinuxCanvas.hpp`.
