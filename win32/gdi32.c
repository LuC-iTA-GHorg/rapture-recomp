/* GDI32: forwarded to the host GDI.
 *
 * GDI handles are 32-bit significant on 64-bit Windows, so they pass through
 * unchanged. DIB sections created by the guest get their pixel memory in guest
 * memory: the guest address space is a section (runtime.c), and CreateDIBSection
 * is called with that section and offset = guest address. Every drawing call is
 * followed by GdiFlush, so guest code that reads or writes the pixels directly
 * sees GDI's work. The screen (window DC) is a host memory DC holding a 640x480
 * 32-bit DIB section; drawing on it marks it dirty and the next message pump
 * copies it to the SDL window.
 */
#include "w32.h"

#include <windows.h>

#define GP(p) ((p) ? G(p) : NULL)
#define H(v) ((HANDLE)(intptr_t)(int32_t)(v))
#define HG(h) ((uint32_t)(uintptr_t)(h))
#define HDCA(n) ((HDC)H(ARG(n)))

extern void *rc_mem_section;

/* -- the screen ---------------------------------------------------------- */
static HDC screen_dc;
static uint32_t *screen_px;
static int screen_dirty;
static HPALETTE screen_default_pal;

static void screen_init(void) {
    if (screen_dc) return;
    HDC ref = GetDC(NULL);
    screen_dc = CreateCompatibleDC(ref);
    ReleaseDC(NULL, ref);
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = 640;
    bi.bmiHeader.biHeight = -480;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void *bits = NULL;
    HBITMAP bm = CreateDIBSection(screen_dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    SelectObject(screen_dc, bm);
    screen_px = (uint32_t *)bits;
    memset(screen_px, 0, 640 * 480 * 4);
    screen_default_pal = (HPALETTE)GetCurrentObject(screen_dc, OBJ_PAL);
}

uint32_t w32_screen_dc(void) { screen_init(); return HG(screen_dc); }
void w32_screen_dirty(void) { screen_dirty = 1; }
uint32_t *w32_screen_pixels(void) { screen_init(); return screen_px; }
void w32_present(void) {
    screen_init();
    GdiFlush();
    static uint32_t argb[640 * 480];
    for (int i = 0; i < 640 * 480; i++) argb[i] = screen_px[i] | 0xFF000000u;
    w32_host_present(argb);
    screen_dirty = 0;
}
void w32_screen_present_if_dirty(void) { if (screen_dirty) w32_present(); }

#define DRAWN(n) do { GdiFlush(); if (ARG(n) == HG(screen_dc)) screen_dirty = 1; } while (0)

/* -- DIB sections in guest memory ------------------------------------------ */
typedef struct { uint32_t hbm, bits; } dibmem;
static dibmem dibs[4096];
static int ndibs;

static uint32_t dib_size(const BITMAPINFOHEADER *h) {
    uint32_t stride = (((uint32_t)h->biWidth * h->biBitCount + 31u) / 32u) * 4u;
    int32_t ht = h->biHeight < 0 ? -h->biHeight : h->biHeight;
    return stride * (uint32_t)ht;
}

static void g_CreateDIBSection(CPU *c) {
    uint32_t hdc = ARG(0), bmi = ARG(1), usage = ARG(2), ppv = ARG(3), hsec = ARG(4);
    if (hsec) { fprintf(stderr, "w32: CreateDIBSection with a guest section is not supported\n"); RET(6, 0); }
    const BITMAPINFOHEADER *h = (const BITMAPINFOHEADER *)G(bmi);
    uint32_t size = dib_size(h);
    /* page-aligned guest block, so the section offset is aligned too */
    uint32_t raw = w32_heap_alloc(size + 0x1000u, 1);
    if (!raw) RET(6, 0);
    uint32_t bits = (raw + 0xFFFu) & ~0xFFFu;
    void *host_bits = NULL;
    HBITMAP hbm = CreateDIBSection((HDC)H(hdc), (const BITMAPINFO *)h, usage, &host_bits, (HANDLE)rc_mem_section, bits);
    if (!hbm) {
        fprintf(stderr, "w32: CreateDIBSection failed (%lu) %ldx%ld %u bpp\n", GetLastError(), h->biWidth, h->biHeight, h->biBitCount);
        w32_heap_free(raw);
        RET(6, 0);
    }
    if (ndibs < 4096) { dibs[ndibs].hbm = HG(hbm); dibs[ndibs].bits = raw; ndibs++; }
    if (ppv) wr32(ppv, bits);
    RET(6, HG(hbm));
}

static void g_DeleteObject(CPU *c) {
    uint32_t h = ARG(0);
    BOOL r = DeleteObject(H(h));
    if (r)
        for (int i = 0; i < ndibs; i++)
            if (dibs[i].hbm == h) { w32_heap_free(dibs[i].bits); dibs[i] = dibs[--ndibs]; break; }
    RET(1, r);
}

/* -- DCs and objects ------------------------------------------------------- */
static void g_CreateCompatibleDC(CPU *c) {
    HDC ref = ARG(0) ? HDCA(0) : NULL;
    RET(1, HG(CreateCompatibleDC(ref ? ref : screen_dc ? screen_dc : NULL)));
}
static void g_DeleteDC(CPU *c) { RET(1, ARG(0) == HG(screen_dc) ? 1 : DeleteDC(HDCA(0))); }
static void g_SelectObject(CPU *c) { RET(2, HG(SelectObject(HDCA(0), H(ARG(1))))); }
static void g_GetStockObject(CPU *c) { RET(1, HG(GetStockObject((int)ARG(0)))); }
static void g_SaveDC(CPU *c) { RET(1, SaveDC(HDCA(0))); }
static void g_RestoreDC(CPU *c) { RET(2, RestoreDC(HDCA(0), (int)ARG(1))); }
static void g_GetDeviceCaps(CPU *c) {
    int idx = (int)ARG(1);
    if (ARG(0) == HG(screen_dc) || !ARG(0)) {
        /* the display the games expect: 640x480, 16 bits per pixel, no palette */
        switch (idx) {
        case HORZRES: RET(2, 640);
        case VERTRES: RET(2, 480);
        case BITSPIXEL: RET(2, 16);
        case PLANES: RET(2, 1);
        case NUMCOLORS: RET(2, (uint32_t)-1);
        case SIZEPALETTE: RET(2, 0);
        case RASTERCAPS: RET(2, GetDeviceCaps(HDCA(0) ? HDCA(0) : screen_dc, RASTERCAPS) & ~RC_PALETTE);
        }
    }
    RET(2, GetDeviceCaps(HDCA(0) ? HDCA(0) : screen_dc, idx));
}
/* LOGBRUSH: 32-bit {style, color, hatch}; host has a pointer-sized hatch. */
static void g_CreateBrushIndirect(CPU *c) {
    LOGBRUSH lb;
    lb.lbStyle = rd32(ARG(0)); lb.lbColor = rd32(ARG(0) + 4); lb.lbHatch = rd32(ARG(0) + 8);
    RET(1, HG(CreateBrushIndirect(&lb)));
}
static void g_CreatePenIndirect(CPU *c) { RET(1, HG(CreatePenIndirect((LOGPEN *)G(ARG(0))))); }
static void g_CreateFontIndirectA(CPU *c) { RET(1, HG(CreateFontIndirectA((LOGFONTA *)G(ARG(0))))); }
static void g_CreatePalette(CPU *c) { RET(1, HG(CreatePalette((LOGPALETTE *)G(ARG(0))))); }
static void g_SelectPalette(CPU *c) { RET(3, HG(SelectPalette(HDCA(0), (HPALETTE)H(ARG(1)), ARG(2)))); }
static void g_RealizePalette(CPU *c) { RET(1, RealizePalette(HDCA(0))); }
static void g_SetDIBColorTable(CPU *c) { RET(4, SetDIBColorTable(HDCA(0), ARG(1), ARG(2), (RGBQUAD *)G(ARG(3)))); }
static void g_GetSystemPaletteEntries(CPU *c) {
    RET(4, GetSystemPaletteEntries(HDCA(0), ARG(1), ARG(2), (PALETTEENTRY *)GP(ARG(3))));
}
static void g_SetBkColor(CPU *c) { RET(2, SetBkColor(HDCA(0), ARG(1))); }
static void g_SetBkMode(CPU *c) { RET(2, SetBkMode(HDCA(0), (int)ARG(1))); }
static void g_SetTextColor(CPU *c) { RET(2, SetTextColor(HDCA(0), ARG(1))); }
static void g_IntersectClipRect(CPU *c) { RET(5, IntersectClipRect(HDCA(0), (int)ARG(1), (int)ARG(2), (int)ARG(3), (int)ARG(4))); }
static void g_ExcludeClipRect(CPU *c) { RET(5, ExcludeClipRect(HDCA(0), (int)ARG(1), (int)ARG(2), (int)ARG(3), (int)ARG(4))); }
static void g_SetViewportOrgEx(CPU *c) { RET(4, SetViewportOrgEx(HDCA(0), (int)ARG(1), (int)ARG(2), (POINT *)GP(ARG(3)))); }
static void g_OffsetViewportOrgEx(CPU *c) { RET(4, OffsetViewportOrgEx(HDCA(0), (int)ARG(1), (int)ARG(2), (POINT *)GP(ARG(3)))); }

/* -- drawing ----------------------------------------------------------------- */
static void g_BitBlt(CPU *c) {
    BOOL r = BitBlt(HDCA(0), (int)ARG(1), (int)ARG(2), (int)ARG(3), (int)ARG(4), HDCA(5), (int)ARG(6), (int)ARG(7), ARG(8));
    DRAWN(0);
    RET(9, r);
}
static void g_StretchDIBits(CPU *c) {
    int r = StretchDIBits(HDCA(0), (int)ARG(1), (int)ARG(2), (int)ARG(3), (int)ARG(4), (int)ARG(5), (int)ARG(6),
                          (int)ARG(7), (int)ARG(8), GP(ARG(9)), (BITMAPINFO *)G(ARG(10)), ARG(11), ARG(12));
    DRAWN(0);
    RET(13, (uint32_t)r);
}
static void g_Rectangle(CPU *c) { BOOL r = Rectangle(HDCA(0), (int)ARG(1), (int)ARG(2), (int)ARG(3), (int)ARG(4)); DRAWN(0); RET(5, r); }
static void g_Ellipse(CPU *c) { BOOL r = Ellipse(HDCA(0), (int)ARG(1), (int)ARG(2), (int)ARG(3), (int)ARG(4)); DRAWN(0); RET(5, r); }
static void g_RoundRect(CPU *c) {
    BOOL r = RoundRect(HDCA(0), (int)ARG(1), (int)ARG(2), (int)ARG(3), (int)ARG(4), (int)ARG(5), (int)ARG(6));
    DRAWN(0);
    RET(7, r);
}
static void g_Polygon(CPU *c) { BOOL r = Polygon(HDCA(0), (POINT *)G(ARG(1)), (int)ARG(2)); DRAWN(0); RET(3, r); }
static void g_Polyline(CPU *c) { BOOL r = Polyline(HDCA(0), (POINT *)G(ARG(1)), (int)ARG(2)); DRAWN(0); RET(3, r); }
static void g_GetEnhMetaFileA(CPU *c) { RET(1, HG(GetEnhMetaFileA(GS(ARG(0))))); }
static void g_DeleteEnhMetaFile(CPU *c) { RET(1, DeleteEnhMetaFile((HENHMETAFILE)H(ARG(0)))); }
static void g_PlayEnhMetaFile(CPU *c) {
    BOOL r = PlayEnhMetaFile(HDCA(0), (HENHMETAFILE)H(ARG(1)), (RECT *)G(ARG(2)));
    DRAWN(0);
    RET(3, r);
}

#define D(n, f) {"gdi32.dll", n, f}
const rc_hle_entry w32_gdi32_table[] = {
    D("CreateDIBSection", g_CreateDIBSection), D("DeleteObject", g_DeleteObject),
    D("CreateCompatibleDC", g_CreateCompatibleDC), D("DeleteDC", g_DeleteDC), D("SelectObject", g_SelectObject),
    D("GetStockObject", g_GetStockObject), D("SaveDC", g_SaveDC), D("RestoreDC", g_RestoreDC),
    D("GetDeviceCaps", g_GetDeviceCaps), D("CreateBrushIndirect", g_CreateBrushIndirect),
    D("CreatePenIndirect", g_CreatePenIndirect), D("CreateFontIndirectA", g_CreateFontIndirectA),
    D("CreatePalette", g_CreatePalette), D("SelectPalette", g_SelectPalette), D("RealizePalette", g_RealizePalette),
    D("SetDIBColorTable", g_SetDIBColorTable), D("GetSystemPaletteEntries", g_GetSystemPaletteEntries),
    D("SetBkColor", g_SetBkColor), D("SetBkMode", g_SetBkMode), D("SetTextColor", g_SetTextColor),
    D("IntersectClipRect", g_IntersectClipRect), D("ExcludeClipRect", g_ExcludeClipRect),
    D("SetViewportOrgEx", g_SetViewportOrgEx), D("OffsetViewportOrgEx", g_OffsetViewportOrgEx),
    D("BitBlt", g_BitBlt), D("StretchDIBits", g_StretchDIBits), D("Rectangle", g_Rectangle),
    D("Ellipse", g_Ellipse), D("RoundRect", g_RoundRect), D("Polygon", g_Polygon), D("Polyline", g_Polyline),
    D("GetEnhMetaFileA", g_GetEnhMetaFileA), D("DeleteEnhMetaFile", g_DeleteEnhMetaFile),
    D("PlayEnhMetaFile", g_PlayEnhMetaFile),
    {NULL, NULL, NULL}
};
