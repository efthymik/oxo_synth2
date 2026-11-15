// LICE's platform bitmap (LICE_SysBitmap) draws through SWELL's memory
// contexts. The editor never creates one — it draws into LICE_MemBitmap and
// blits the pixels with xcb — but lice.cpp references these three SWELL entry
// points, and linking SWELL would drag in GTK (the thing this port exists to
// avoid). These stubs satisfy the linker; nothing calls them.
//
// Signatures from WDL/swell/swell-functions.h at the pinned commit
// (linux-port-lice/Dependencies/wdl/VENDOR.md).

#include "../Dependencies/wdl/swell/swell-types.h"

HDC SWELL_CreateMemContext(HDC, int, int) { return nullptr; }
void SWELL_DeleteGfxContext(HDC) {}
void *SWELL_GetCtxFrameBuffer(HDC) { return nullptr; }
