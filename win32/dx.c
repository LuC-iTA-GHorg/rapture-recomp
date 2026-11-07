/* DirectDraw, DirectSound, MCIWnd: reached through LoadLibraryA + GetProcAddress.
 *
 * COM objects live in guest memory: {vtable pointer, refcount, host data index};
 * the vtable slots are host thunks (rc_host_thunk). Methods are stdcall with
 * `this` as the first argument.
 */
#include "w32.h"

#include <windows.h>

#define E_NOINTERFACE_ 0x80004002u
#define DSERR_NODRIVER_ 0x88780078u

static uint32_t make_object(const rc_fn *methods, const char *const *names, int n, uint32_t extra) {
    uint32_t vt = w32_heap_alloc(4u * (uint32_t)n, 1);
    for (int i = 0; i < n; i++) wr32(vt + 4u * (uint32_t)i, rc_host_thunk(methods[i], names[i]));
    uint32_t obj = w32_heap_alloc(16 + extra, 1);
    wr32(obj, vt);
    wr32(obj + 4, 1);
    return obj;
}

/* -- IDirectDraw: only the display-mode change is used; it is a no-op (the
      window shows a 640x480 surface whatever the host desktop is). ------- */
static void dd_QueryInterface(CPU *c) { RET(3, E_NOINTERFACE_); }
static void dd_AddRef(CPU *c) { uint32_t o = ARG(0); wr32(o + 4, rd32(o + 4) + 1); RET(1, rd32(o + 4)); }
static void dd_Release(CPU *c) { uint32_t o = ARG(0); uint32_t n = rd32(o + 4) - 1; wr32(o + 4, n); RET(1, n); }
static void dd_unimpl(CPU *c) { rc_fault(c, rd32(c->esp), "IDirectDraw method not implemented"); }
static void dd_ok1(CPU *c) { RET(1, 0); }
static void dd_SetCooperativeLevel(CPU *c) { RET(3, 0); }
static void dd_SetDisplayMode(CPU *c) { W32_LOG("DirectDraw SetDisplayMode %ux%ux%u (ignored)", ARG(1), ARG(2), ARG(3)); RET(4, 0); }

static void x_DirectDrawCreate(CPU *c) {
    /* IDirectDraw vtable: 0 QI, 1 AddRef, 2 Release, 3 Compact, 4 CreateClipper, 5 CreatePalette,
       6 CreateSurface, 7 DuplicateSurface, 8 EnumDisplayModes, 9 EnumSurfaces, 10 FlipToGDISurface,
       11 GetCaps, 12 GetDisplayMode, 13 GetFourCCCodes, 14 GetGDISurface, 15 GetMonitorFrequency,
       16 GetScanLine, 17 GetVerticalBlankStatus, 18 Initialize, 19 RestoreDisplayMode,
       20 SetCooperativeLevel, 21 SetDisplayMode, 22 WaitForVerticalBlank */
    static rc_fn m[23];
    static const char *names[23];
    for (int i = 0; i < 23; i++) { m[i] = dd_unimpl; names[i] = "IDirectDraw::?"; }
    m[0] = dd_QueryInterface; m[1] = dd_AddRef; m[2] = dd_Release;
    m[19] = dd_ok1; names[19] = "IDirectDraw::RestoreDisplayMode";
    m[20] = dd_SetCooperativeLevel; names[20] = "IDirectDraw::SetCooperativeLevel";
    m[21] = dd_SetDisplayMode; names[21] = "IDirectDraw::SetDisplayMode";
    uint32_t obj = make_object(m, names, 23, 0);
    wr32(ARG(1), obj);
    RET(3, 0);
}

/* DirectSound: see dsound.c (w32_dsound_create). */
uint32_t w32_dsound_create(uint32_t *obj);
static void x_DirectSoundCreate(CPU *c) {
    uint32_t obj = 0;
    uint32_t hr = w32_dsound_create(&obj);
    if (ARG(1)) wr32(ARG(1), obj);
    RET(3, hr);
}

/* MCIWnd (msvfw32): see movie.cpp. */
void w32_mciwnd_create(CPU *c);

const rc_hle_entry w32_dx_table[] = {
    {"ddraw.dll", "DirectDrawCreate", x_DirectDrawCreate},
    {"dsound.dll", "DirectSoundCreate", x_DirectSoundCreate},
    {"msvfw32.dll", "MCIWndCreateA", w32_mciwnd_create},
    {NULL, NULL, NULL}
};
