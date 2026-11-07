/* USER32: virtual windows, the message queue and input from the SDL window.
 *
 * Windows are emulated (no host HWND): a top-level window's client area is the
 * 640x480 screen surface shown by window.cpp; its DC is a host memory DC on a
 * 32-bit DIB section (gdi32.c), copied to the SDL framebuffer when drawn to.
 * Window procedures are guest functions, called with rc_invoke.
 */
#include "w32.h"

#include <windows.h>

#define GP(p) ((p) ? G(p) : NULL)
#define H(v) ((HANDLE)(intptr_t)(int32_t)(v))
#define HG(h) ((uint32_t)(uintptr_t)(h))

int w32_host_scripted(void);
uint32_t w32_screen_dc(void);           /* gdi32.c */
void w32_screen_dirty(void);
void w32_screen_present_if_dirty(void);
uint32_t w32_load_library(const char *name);

/* ---------------------------------------------------------------------- */
typedef struct {
    int used;
    char name[64];
    uint32_t style, wndproc, cls_extra, wnd_extra, hinst, icon, cursor, brush;
} wclass;

typedef struct {
    int used;
    int cls;
    uint32_t wndproc, style, exstyle, parent, id, hinst, userdata;
    int x, y, w, h;               /* window rect in screen coordinates (= client: no frame) */
    int visible, enabled, shown_once;
    int invalid;                  /* has a pending update region */
    RECT upd;
    char text[128];
    uint32_t extra[16];           /* cbWndExtra */
} wnd;

#define MAX_CLASSES 32
#define MAX_WINDOWS 64
#define HWND_BASE 0x00020000u
static wclass classes[MAX_CLASSES];
static wnd windows[MAX_WINDOWS];
static uint32_t focus_hwnd, capture_hwnd, active_hwnd;
static uint32_t def_wndproc_thunk;

static wnd *W(uint32_t hwnd) {
    uint32_t k = (hwnd - HWND_BASE) / 4u;
    if (hwnd < HWND_BASE || k >= MAX_WINDOWS || !windows[k].used) return NULL;
    return &windows[k];
}
static uint32_t HW(wnd *w) { return HWND_BASE + 4u * (uint32_t)(w - windows); }

/* -- message queue ------------------------------------------------------ */
typedef struct { uint32_t hwnd, msg, wp, lp, time, x, y; } qmsg;
#define QSIZE 1024
static qmsg queue[QSIZE];
static unsigned qhead, qtail;
static CRITICAL_SECTION qlock;
static int qlock_init;
static int quit_posted;
static uint32_t quit_code;
static int mouse_x = 320, mouse_y = 240;
static uint8_t keystate[256];

static void qinit(void) { if (!qlock_init) { InitializeCriticalSection(&qlock); qlock_init = 1; } }

void w32_post_message(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    qinit();
    EnterCriticalSection(&qlock);
    if (((qtail + 1) % QSIZE) != qhead) {
        qmsg *m = &queue[qtail];
        m->hwnd = hwnd; m->msg = msg; m->wp = wp; m->lp = lp;
        m->time = w32_host_ticks(); m->x = (uint32_t)mouse_x; m->y = (uint32_t)mouse_y;
        qtail = (qtail + 1) % QSIZE;
    }
    LeaveCriticalSection(&qlock);
}

static uint32_t wsend(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);

static uint32_t main_window(void) {
    for (int k = 0; k < MAX_WINDOWS; k++)
        if (windows[k].used && !windows[k].parent && windows[k].visible) return HW(&windows[k]);
    for (int k = 0; k < MAX_WINDOWS; k++)
        if (windows[k].used && !windows[k].parent) return HW(&windows[k]);
    return 0;
}

/* -- host input -> messages ---------------------------------------------- */
static uint32_t vk_from_sdl(int key) {
    /* SDL keycodes: printable ASCII for letters/digits, 1<<30 | scancode for the rest. */
    if (key >= 'a' && key <= 'z') return (uint32_t)(key - 32);
    if (key >= '0' && key <= '9') return (uint32_t)key;
    switch (key) {
    case ' ': return VK_SPACE; case '\r': return VK_RETURN; case 27: return VK_ESCAPE;
    case '\t': return VK_TAB; case 8: return VK_BACK; case 127: return VK_DELETE;
    case '-': return VK_OEM_MINUS; case '=': return VK_OEM_PLUS; case ',': return VK_OEM_COMMA;
    case '.': return VK_OEM_PERIOD;
    }
    int sc = key & ~(1 << 30);
    if (key & (1 << 30)) {
        if (sc >= 58 && sc <= 69) return VK_F1 + (uint32_t)(sc - 58);      /* F1..F12 */
        switch (sc) {
        case 79: return VK_RIGHT; case 80: return VK_LEFT; case 81: return VK_DOWN; case 82: return VK_UP;
        case 73: return VK_INSERT; case 74: return VK_HOME; case 75: return VK_PRIOR; case 77: return VK_END;
        case 78: return VK_NEXT; case 76: return VK_DELETE;
        case 84: return VK_DIVIDE; case 85: return VK_MULTIPLY; case 86: return VK_SUBTRACT; case 87: return VK_ADD;
        case 88: return VK_RETURN;
        case 89: case 90: case 91: case 92: case 93: case 94: case 95: case 96: case 97:
            return VK_NUMPAD1 + (uint32_t)(sc - 89);
        case 98: return VK_NUMPAD0; case 99: return VK_DECIMAL;
        case 224: case 228: return VK_CONTROL; case 225: case 229: return VK_SHIFT;
        case 226: case 230: return VK_MENU;
        }
    }
    return 0;
}

static uint32_t mk_flags(void) {
    uint32_t f = 0;
    if (keystate[VK_LBUTTON] & 0x80) f |= MK_LBUTTON;
    if (keystate[VK_RBUTTON] & 0x80) f |= MK_RBUTTON;
    if (keystate[VK_MBUTTON] & 0x80) f |= MK_MBUTTON;
    if (keystate[VK_SHIFT] & 0x80) f |= MK_SHIFT;
    if (keystate[VK_CONTROL] & 0x80) f |= MK_CONTROL;
    return f;
}

static void mouse_message(uint32_t msg) {
    uint32_t hwnd = capture_hwnd ? capture_hwnd : main_window();
    wnd *w = W(hwnd);
    if (!w) return;
    int cx = mouse_x - w->x, cy = mouse_y - w->y;
    if (msg == WM_MOUSEMOVE || msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN)
        w32_post_message(hwnd, WM_SETCURSOR, hwnd, HTCLIENT | (msg << 16));
    w32_post_message(hwnd, msg, mk_flags(), ((uint32_t)(uint16_t)cy << 16) | (uint16_t)cx);
}

void w32_pump_host_events(void) {
    static int in_pump;
    if (in_pump) return;
    in_pump = 1;
    extern void w32_movie_tick(void);
    extern void w32_poll_joysticks(void);
    w32_movie_tick();
    w32_poll_joysticks();
    w32_screen_present_if_dirty();
    w32_host_event e;
    while (w32_host_poll(&e)) {
        switch (e.type) {
        case W32_EV_QUIT: {
            uint32_t mw = main_window();
            if (mw) w32_post_message(mw, WM_CLOSE, 0, 0);
            else { quit_posted = 1; }
            break;
        }
        case W32_EV_MOUSEMOVE:
            mouse_x = e.x; mouse_y = e.y;
            mouse_message(WM_MOUSEMOVE);
            break;
        case W32_EV_MOUSEDOWN: case W32_EV_MOUSEUP: {
            int down = e.type == W32_EV_MOUSEDOWN;
            mouse_x = e.x; mouse_y = e.y;
            uint32_t vk = e.button == 3 ? VK_RBUTTON : e.button == 2 ? VK_MBUTTON : VK_LBUTTON;
            keystate[vk] = down ? 0x80 : 0;
            uint32_t msg = e.button == 3 ? (down ? WM_RBUTTONDOWN : WM_RBUTTONUP)
                         : e.button == 2 ? (down ? WM_MBUTTONDOWN : WM_MBUTTONUP)
                         : (down ? WM_LBUTTONDOWN : WM_LBUTTONUP);
            mouse_message(msg);
            break;
        }
        case W32_EV_KEYDOWN: case W32_EV_KEYUP: {
            uint32_t vk = vk_from_sdl(e.key);
            if (!vk) break;
            int down = e.type == W32_EV_KEYDOWN;
            if (down) keystate[vk] = (uint8_t)(0x80 | ((keystate[vk] & 1) ^ 1));
            else keystate[vk] &= 1;
            uint32_t sc = MapVirtualKeyA(vk, 0 /* MAPVK_VK_TO_VSC */);
            uint32_t lp = 1u | (sc << 16) | (down ? 0 : 0xC0000000u);
            uint32_t hwnd = focus_hwnd ? focus_hwnd : main_window();
            int sys = (keystate[VK_MENU] & 0x80) || vk == VK_MENU || vk == VK_F10;
            uint32_t msg = down ? (sys ? WM_SYSKEYDOWN : WM_KEYDOWN) : (sys ? WM_SYSKEYUP : WM_KEYUP);
            if (hwnd) w32_post_message(hwnd, msg, vk, lp);
            break;
        }
        default:
            break;
        }
    }
    in_pump = 0;
}

/* -- window procedures --------------------------------------------------- */
static uint32_t call_proc(uint32_t proc, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    if (!proc) return 0;
    uint32_t args[4] = {hwnd, msg, wp, lp};
    CPU *c = w32_cpu();
    return rc_invoke(c, proc, 4, args);
}

static uint32_t wsend(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    wnd *w = W(hwnd);
    if (!w) return 0;
    return call_proc(w->wndproc, hwnd, msg, wp, lp);
}

static uint32_t def_proc(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    wnd *w = W(hwnd);
    switch (msg) {
    case WM_NCCREATE: return 1;
    case WM_CLOSE: {
        uint32_t args[1] = {hwnd};
        (void)args;
        if (w) { extern void w32_destroy_window(uint32_t); w32_destroy_window(hwnd); }
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT:
        if (w) w->invalid = 0;
        return 0;
    case WM_SETCURSOR:
        if (w && classes[w->cls].cursor) {
            extern void w32_set_cursor(uint32_t);
            w32_set_cursor(classes[w->cls].cursor);
        }
        return 1;
    case WM_NCHITTEST: return HTCLIENT;
    case WM_MOUSEACTIVATE: return MA_ACTIVATE;
    case WM_SYSKEYDOWN:
        if (wp == VK_F4 && (lp & (1u << 29))) w32_post_message(hwnd, WM_CLOSE, 0, 0);
        return 0;
    }
    return 0;
}

static void u_DefWindowProcA(CPU *c) { RET(4, def_proc(ARG(0), ARG(1), ARG(2), ARG(3))); }

static void u_CallWindowProcA(CPU *c) {
    uint32_t proc = ARG(0);
    if (proc == def_wndproc_thunk) RET(5, def_proc(ARG(1), ARG(2), ARG(3), ARG(4)));
    RET(5, call_proc(proc, ARG(1), ARG(2), ARG(3), ARG(4)));
}

/* -- classes -------------------------------------------------------------- */
static int find_class(const char *name) {
    for (int k = 0; k < MAX_CLASSES; k++)
        if (classes[k].used && lstrcmpiA(classes[k].name, name) == 0) return k;
    return -1;
}

static void u_RegisterClassA(CPU *c) {
    uint32_t p = ARG(0);
    if (!def_wndproc_thunk) def_wndproc_thunk = rc_host_thunk(u_DefWindowProcA, "DefWindowProcA");
    const char *name = rd32(p + 36) >= 0x10000u ? GS(rd32(p + 36)) : "#atom";
    int k = find_class(name);
    if (k < 0) for (k = 0; k < MAX_CLASSES && classes[k].used; k++) {}
    if (k >= MAX_CLASSES) RET(1, 0);
    wclass *cl = &classes[k];
    cl->used = 1;
    snprintf(cl->name, sizeof cl->name, "%s", name);
    cl->style = rd32(p); cl->wndproc = rd32(p + 4); cl->cls_extra = rd32(p + 8); cl->wnd_extra = rd32(p + 12);
    cl->hinst = rd32(p + 16); cl->icon = rd32(p + 20); cl->cursor = rd32(p + 24); cl->brush = rd32(p + 28);
    W32_LOG("RegisterClass %s proc %08x", name, cl->wndproc);
    RET(1, 0xC000u + (uint32_t)k);
}
static void u_UnregisterClassA(CPU *c) {
    int k = ARG(0) >= 0x10000u ? find_class(GS(ARG(0))) : (int)(ARG(0) - 0xC000u);
    if (k >= 0 && k < MAX_CLASSES) classes[k].used = 0;
    RET(2, 1);
}
static void u_GetClassInfoA(CPU *c) {
    uint32_t name = ARG(1), p = ARG(2);
    int k = name >= 0x10000u ? find_class(GS(name)) : (int)(name - 0xC000u);
    if (k < 0 || k >= MAX_CLASSES || !classes[k].used) RET(3, 0);
    wclass *cl = &classes[k];
    wr32(p, cl->style); wr32(p + 4, cl->wndproc); wr32(p + 8, cl->cls_extra); wr32(p + 12, cl->wnd_extra);
    wr32(p + 16, cl->hinst); wr32(p + 20, cl->icon); wr32(p + 24, cl->cursor); wr32(p + 28, cl->brush);
    wr32(p + 32, 0); wr32(p + 36, name);
    RET(3, 1);
}

/* -- windows ------------------------------------------------------------- */
static void u_CreateWindowExA(CPU *c) {
    uint32_t exstyle = ARG(0), cname = ARG(1), title = ARG(2), style = ARG(3);
    int x = (int)ARG(4), y = (int)ARG(5), w_ = (int)ARG(6), h_ = (int)ARG(7);
    uint32_t parent = ARG(8), menu = ARG(9), hinst = ARG(10), param = ARG(11);
    int k = cname >= 0x10000u ? find_class(GS(cname)) : (int)(cname - 0xC000u);
    if (k < 0 || k >= MAX_CLASSES || !classes[k].used) {
        W32_LOG("CreateWindowEx: unknown class %s", cname >= 0x10000u ? GS(cname) : "#");
        RET(12, 0);
    }
    int i;
    for (i = 0; i < MAX_WINDOWS && windows[i].used; i++) {}
    if (i >= MAX_WINDOWS) RET(12, 0);
    wnd *w = &windows[i];
    memset(w, 0, sizeof *w);
    w->used = 1; w->cls = k; w->wndproc = classes[k].wndproc; w->style = style; w->exstyle = exstyle;
    w->parent = parent; w->id = menu; w->hinst = hinst; w->enabled = 1;
    if (x == (int)CW_USEDEFAULT) x = 0;
    if (y == (int)CW_USEDEFAULT) y = 0;
    if (w_ == (int)CW_USEDEFAULT) w_ = 640;
    if (h_ == (int)CW_USEDEFAULT) h_ = 480;
    w->x = x; w->y = y; w->w = w_; w->h = h_;
    if (title) snprintf(w->text, sizeof w->text, "%s", GS(title));
    uint32_t hwnd = HW(w);
    W32_LOG("CreateWindowEx %s '%s' %d,%d %dx%d style %08x -> %08x", classes[k].name, w->text, x, y, w_, h_, style, hwnd);
    /* CREATESTRUCTA in guest memory for WM_NCCREATE / WM_CREATE. */
    uint32_t cs = w32_heap_alloc(48, 1);
    wr32(cs + 0, param); wr32(cs + 4, hinst); wr32(cs + 8, menu); wr32(cs + 12, parent);
    wr32(cs + 16, (uint32_t)h_); wr32(cs + 20, (uint32_t)w_); wr32(cs + 24, (uint32_t)y); wr32(cs + 28, (uint32_t)x);
    wr32(cs + 32, style); wr32(cs + 36, title); wr32(cs + 40, cname); wr32(cs + 44, exstyle);
    if (!wsend(hwnd, WM_NCCREATE, 0, cs)) { w->used = 0; w32_heap_free(cs); RET(12, 0); }
    if ((int32_t)wsend(hwnd, WM_CREATE, 0, cs) == -1) { w->used = 0; w32_heap_free(cs); RET(12, 0); }
    w32_heap_free(cs);
    wsend(hwnd, WM_SIZE, 0, ((uint32_t)(uint16_t)h_ << 16) | (uint16_t)w_);
    wsend(hwnd, WM_MOVE, 0, ((uint32_t)(uint16_t)y << 16) | (uint16_t)x);
    if (style & WS_VISIBLE) {
        extern void w32_show(uint32_t, int);
        w32_show(hwnd, SW_SHOW);
    }
    RET(12, hwnd);
}

void w32_show(uint32_t hwnd, int cmd) {
    wnd *w = W(hwnd);
    if (!w) return;
    int vis = cmd != SW_HIDE;
    if (vis == w->visible) return;
    w->visible = vis;
    wsend(hwnd, WM_SHOWWINDOW, (uint32_t)vis, 0);
    if (vis && !w->parent) {
        if (!w32_host_window_open(w->text[0] ? w->text : "Rapture")) { fprintf(stderr, "w32: no window\n"); exit(1); }
        if (!w->shown_once) {
            w->shown_once = 1;
            active_hwnd = hwnd;
            wsend(hwnd, WM_ACTIVATEAPP, 1, 0);
            wsend(hwnd, WM_NCACTIVATE, 1, 0);
            wsend(hwnd, WM_ACTIVATE, WA_ACTIVE, 0);
            if (!focus_hwnd) { focus_hwnd = hwnd; wsend(hwnd, WM_SETFOCUS, 0, 0); }
        }
    }
    if (vis) { w->invalid = 1; SetRect(&w->upd, 0, 0, w->w, w->h); }
}

void w32_destroy_window(uint32_t hwnd) {
    wnd *w = W(hwnd);
    if (!w) return;
    for (int k = 0; k < MAX_WINDOWS; k++)
        if (windows[k].used && windows[k].parent == hwnd) w32_destroy_window(HW(&windows[k]));
    wsend(hwnd, WM_DESTROY, 0, 0);
    wsend(hwnd, WM_NCDESTROY, 0, 0);
    w->used = 0;
    if (focus_hwnd == hwnd) focus_hwnd = 0;
    if (capture_hwnd == hwnd) capture_hwnd = 0;
    if (active_hwnd == hwnd) active_hwnd = 0;
}

/* A child window whose window procedure is host code (MCIWnd): wndproc is a
   host thunk address. Returns the HWND or 0. */
uint32_t w32_create_child(uint32_t parent, uint32_t wndproc, int x, int y, int w_, int h_) {
    int i;
    for (i = 0; i < MAX_WINDOWS && windows[i].used; i++) {}
    if (i >= MAX_WINDOWS) return 0;
    wnd *w = &windows[i];
    memset(w, 0, sizeof *w);
    w->used = 1; w->cls = 0; w->wndproc = wndproc; w->style = WS_CHILD | WS_VISIBLE; w->parent = parent;
    w->enabled = 1; w->visible = 1;
    w->x = x; w->y = y; w->w = w_; w->h = h_;
    return HW(w);
}
void w32_window_screen_rect(uint32_t hwnd, int *x, int *y, int *w_, int *h_) {
    extern void w32_origin(uint32_t, int *, int *);
    wnd *w = W(hwnd);
    w32_origin(hwnd, x, y);
    *w_ = w ? w->w : 0;
    *h_ = w ? w->h : 0;
}
int w32_window_visible(uint32_t hwnd) {
    for (wnd *w = W(hwnd); w; w = W(w->parent)) if (!w->visible) return 0;
    return W(hwnd) != NULL;
}

static void u_DestroyWindow(CPU *c) { w32_destroy_window(ARG(0)); RET(1, 1); }
static void u_ShowWindow(CPU *c) {
    wnd *w = W(ARG(0));
    int was = w ? w->visible : 0;
    w32_show(ARG(0), (int)ARG(1));
    RET(2, was);
}
static void u_UpdateWindow(CPU *c) {
    wnd *w = W(ARG(0));
    if (w && w->invalid && w->visible) wsend(ARG(0), WM_PAINT, 0, 0);
    RET(1, 1);
}
static void u_IsWindowVisible(CPU *c) { wnd *w = W(ARG(0)); RET(1, w && w->visible); }
static void u_IsIconic(CPU *c) { RET(1, 0); }
static void u_EnableWindow(CPU *c) {
    wnd *w = W(ARG(0));
    int was = w ? !w->enabled : 0;
    if (w) w->enabled = ARG(1) != 0;
    RET(2, was);
}
static void u_FindWindowA(CPU *c) {
    const char *cn = ARG(0) >= 0x10000u ? GS(ARG(0)) : NULL, *tn = GS(ARG(1));
    for (int k = 0; k < MAX_WINDOWS; k++) {
        wnd *w = &windows[k];
        if (!w->used || w->parent) continue;
        if (cn && lstrcmpiA(classes[w->cls].name, cn)) continue;
        if (tn && strcmp(w->text, tn)) continue;
        RET(2, HW(w));
    }
    RET(2, 0);
}
static void u_SetWindowTextA(CPU *c) {
    wnd *w = W(ARG(0));
    if (w) snprintf(w->text, sizeof w->text, "%s", ARG(1) ? GS(ARG(1)) : "");
    RET(2, w != NULL);
}
static void u_GetWindowTextA(CPU *c) {
    wnd *w = W(ARG(0));
    uint32_t buf = ARG(1), n = ARG(2);
    if (!w || !n) RET(3, 0);
    lstrcpynA((char *)G(buf), w->text, (int)n);
    RET(3, strlen((char *)G(buf)));
}

static uint32_t *long_slot(wnd *w, int32_t idx) {
    static uint32_t dummy;
    switch (idx) {
    case -4: return &w->wndproc;
    case GWL_STYLE: return &w->style;
    case GWL_EXSTYLE: return &w->exstyle;
    case -21: return &w->userdata;
    case GWL_ID: return &w->id;
    case -6: return &w->hinst;
    case -8 /* GWL_HWNDPARENT */: return &w->parent;
    }
    if (idx >= 0 && idx / 4 < 16) return &w->extra[idx / 4];
    dummy = 0;
    return &dummy;
}
static void u_GetWindowLongA(CPU *c) {
    wnd *w = W(ARG(0));
    RET(2, w ? *long_slot(w, (int32_t)ARG(1)) : 0);
}
static void u_SetWindowLongA(CPU *c) {
    wnd *w = W(ARG(0));
    if (!w) RET(3, 0);
    uint32_t *s = long_slot(w, (int32_t)ARG(1));
    uint32_t old = *s;
    *s = ARG(2);
    RET(3, old);
}

static void get_rect(uint32_t r, int l, int t, int rr, int b) { wr32(r, (uint32_t)l); wr32(r + 4, (uint32_t)t); wr32(r + 8, (uint32_t)rr); wr32(r + 12, (uint32_t)b); }
static void u_GetClientRect(CPU *c) {
    wnd *w = W(ARG(0));
    if (!w) RET(2, 0);
    get_rect(ARG(1), 0, 0, w->w, w->h);
    RET(2, 1);
}
static void u_AdjustWindowRectEx(CPU *c) { RET(4, 1); }   /* no non-client area */
static void u_SetWindowPos(CPU *c) {
    wnd *w = W(ARG(0));
    uint32_t flags = ARG(6);
    if (!w) RET(7, 0);
    if (!(flags & SWP_NOMOVE)) { w->x = (int)ARG(2); w->y = (int)ARG(3); }
    if (!(flags & SWP_NOSIZE)) { w->w = (int)ARG(4); w->h = (int)ARG(5); }
    if (flags & SWP_SHOWWINDOW) w32_show(ARG(0), SW_SHOW);
    if (flags & SWP_HIDEWINDOW) w32_show(ARG(0), SW_HIDE);
    if (!(flags & SWP_NOSIZE)) wsend(ARG(0), WM_SIZE, 0, ((uint32_t)(uint16_t)w->h << 16) | (uint16_t)w->w);
    RET(7, 1);
}
/* WINDOWPLACEMENT: length, flags, showCmd, ptMin, ptMax, rcNormal (44 bytes). */
static void u_GetWindowPlacement(CPU *c) {
    wnd *w = W(ARG(0));
    uint32_t p = ARG(1);
    if (!w) RET(2, 0);
    memset(G(p), 0, 44);
    wr32(p, 44);
    wr32(p + 8, w->visible ? SW_SHOWNORMAL : SW_HIDE);
    get_rect(p + 28, w->x, w->y, w->x + w->w, w->y + w->h);
    RET(2, 1);
}
static void u_SetWindowPlacement(CPU *c) {
    wnd *w = W(ARG(0));
    uint32_t p = ARG(1);
    if (!w) RET(2, 0);
    w->x = (int)rd32(p + 28); w->y = (int)rd32(p + 32);
    w->w = (int)rd32(p + 36) - w->x; w->h = (int)rd32(p + 40) - w->y;
    w32_show(ARG(0), (int)rd32(p + 8));
    RET(2, 1);
}
void w32_origin(uint32_t hwnd, int *ox, int *oy);
static void origin(uint32_t hwnd, int *ox, int *oy) { w32_origin(hwnd, ox, oy); }
void w32_origin(uint32_t hwnd, int *ox, int *oy) {
    *ox = *oy = 0;
    for (wnd *w = W(hwnd); w; w = W(w->parent)) { *ox += w->x; *oy += w->y; }
}
static void u_ClientToScreen(CPU *c) {
    int ox, oy;
    origin(ARG(0), &ox, &oy);
    uint32_t p = ARG(1);
    wr32(p, rd32(p) + (uint32_t)ox); wr32(p + 4, rd32(p + 4) + (uint32_t)oy);
    RET(2, 1);
}
static void u_MapWindowPoints(CPU *c) {
    int fx, fy, tx, ty;
    origin(ARG(0), &fx, &fy);
    origin(ARG(1), &tx, &ty);
    uint32_t p = ARG(2), n = ARG(3);
    for (uint32_t i = 0; i < n; i++) {
        wr32(p + 8 * i, rd32(p + 8 * i) + (uint32_t)(fx - tx));
        wr32(p + 8 * i + 4, rd32(p + 8 * i + 4) + (uint32_t)(fy - ty));
    }
    RET(4, ((uint32_t)(uint16_t)(fy - ty) << 16) | (uint16_t)(fx - tx));
}

/* -- painting ----------------------------------------------------------- */
static void u_InvalidateRect(CPU *c) {
    uint32_t hwnd = ARG(0), r = ARG(1);
    wnd *w = W(hwnd);
    if (!hwnd) { for (int k = 0; k < MAX_WINDOWS; k++) if (windows[k].used) { windows[k].invalid = 1; SetRect(&windows[k].upd, 0, 0, windows[k].w, windows[k].h); } RET(3, 1); }
    if (!w) RET(3, 0);
    RECT nr;
    if (r) memcpy(&nr, G(r), 16); else SetRect(&nr, 0, 0, w->w, w->h);
    if (w->invalid) UnionRect(&w->upd, &w->upd, &nr); else w->upd = nr;
    w->invalid = 1;
    RET(3, 1);
}
static void u_ValidateRect(CPU *c) {
    wnd *w = W(ARG(0));
    if (w) w->invalid = 0;
    RET(2, 1);
}
/* PAINTSTRUCT (32-bit): hdc, fErase, rcPaint, fRestore, fIncUpdate, rgbReserved[32]. */
static void u_BeginPaint(CPU *c) {
    wnd *w = W(ARG(0));
    uint32_t p = ARG(1);
    memset(G(p), 0, 64);
    uint32_t dc = w32_screen_dc();
    wr32(p, dc);
    if (w) {
        RECT r = w->invalid ? w->upd : (RECT){0, 0, 0, 0};
        memcpy(G(p + 8), &r, 16);
        w->invalid = 0;
    }
    RET(2, dc);
}
static void u_EndPaint(CPU *c) { w32_screen_dirty(); RET(2, 1); }
static void u_GetDC(CPU *c) { RET(1, w32_screen_dc()); }
static void u_ReleaseDC(CPU *c) { w32_screen_dirty(); RET(2, 1); }
static void u_FillRect(CPU *c) {
    int r = FillRect((HDC)H(ARG(0)), (RECT *)G(ARG(1)), (HBRUSH)H(ARG(2)));
    GdiFlush();
    if (ARG(0) == w32_screen_dc()) w32_screen_dirty();
    RET(3, r);
}
static void u_FrameRect(CPU *c) {
    int r = FrameRect((HDC)H(ARG(0)), (RECT *)G(ARG(1)), (HBRUSH)H(ARG(2)));
    GdiFlush();
    if (ARG(0) == w32_screen_dc()) w32_screen_dirty();
    RET(3, r);
}
static void u_DrawTextA(CPU *c) {
    int r = DrawTextA((HDC)H(ARG(0)), GS(ARG(1)), (int)ARG(2), (RECT *)G(ARG(3)), ARG(4));
    GdiFlush();
    if (ARG(0) == w32_screen_dc()) w32_screen_dirty();
    RET(5, r);
}
static void u_ScrollDC(CPU *c) {
    RECT upd;
    int r = ScrollDC((HDC)H(ARG(0)), (int)ARG(1), (int)ARG(2), (RECT *)GP(ARG(3)), (RECT *)GP(ARG(4)),
                     (HRGN)H(ARG(5)), ARG(6) ? &upd : NULL);
    if (ARG(6)) memcpy(G(ARG(6)), &upd, 16);
    GdiFlush();
    if (ARG(0) == w32_screen_dc()) w32_screen_dirty();
    RET(7, r);
}

/* -- messages ------------------------------------------------------------- */
static int paint_pending(uint32_t filter, qmsg *out) {
    for (int k = 0; k < MAX_WINDOWS; k++) {
        wnd *w = &windows[k];
        if (w->used && w->visible && w->invalid && (!filter || filter == HW(w))) {
            memset(out, 0, sizeof *out);
            out->hwnd = HW(w);
            out->msg = WM_PAINT;
            return 1;
        }
    }
    return 0;
}

static void write_msg(uint32_t p, const qmsg *m) {
    wr32(p, m->hwnd); wr32(p + 4, m->msg); wr32(p + 8, m->wp); wr32(p + 12, m->lp);
    wr32(p + 16, m->time); wr32(p + 20, m->x); wr32(p + 24, m->y);
}

static void u_PeekMessageA(CPU *c) {
    uint32_t p = ARG(0), filter = ARG(1), mn = ARG(2), mx = ARG(3), remove = ARG(4);
    w32_pump_host_events();
    qinit();
    EnterCriticalSection(&qlock);
    for (unsigned i = qhead; i != qtail; i = (i + 1) % QSIZE) {
        qmsg m = queue[i];
        if (filter && m.hwnd != filter) continue;
        if ((mn || mx) && (m.msg < mn || m.msg > mx)) continue;
        if (remove & PM_REMOVE) {
            for (unsigned j = i; j != qhead; j = (j + QSIZE - 1) % QSIZE) queue[j] = queue[(j + QSIZE - 1) % QSIZE];
            qhead = (qhead + 1) % QSIZE;
        }
        LeaveCriticalSection(&qlock);
        write_msg(p, &m);
        RET(5, 1);
    }
    LeaveCriticalSection(&qlock);
    if (quit_posted && (!mn || (WM_QUIT >= mn && WM_QUIT <= mx))) {
        qmsg m = {0, WM_QUIT, quit_code, 0, w32_host_ticks(), 0, 0};
        if (remove & PM_REMOVE) quit_posted = 0;
        write_msg(p, &m);
        RET(5, 1);
    }
    qmsg pm;
    if ((!mn || (WM_PAINT >= mn && WM_PAINT <= mx)) && paint_pending(filter, &pm)) {
        write_msg(p, &pm);
        RET(5, 1);
    }
    RET(5, 0);
}
static void u_TranslateMessage(CPU *c) {
    uint32_t p = ARG(0), msg = rd32(p + 4), vk = rd32(p + 8);
    if (msg != WM_KEYDOWN && msg != WM_SYSKEYDOWN) RET(1, 0);
    BYTE ks[256];
    memcpy(ks, keystate, 256);
    WORD ch = 0;
    if (ToAscii(vk, (rd32(p + 12) >> 16) & 0xFF, ks, &ch, 0) == 1)
        w32_post_message(rd32(p), msg == WM_KEYDOWN ? WM_CHAR : WM_SYSCHAR, ch & 0xFF, rd32(p + 12));
    RET(1, 1);
}
static void u_DispatchMessageA(CPU *c) {
    uint32_t p = ARG(0);
    uint32_t hwnd = rd32(p), msg = rd32(p + 4);
    if (msg == WM_PAINT) {
        wnd *w = W(hwnd);
        uint32_t r = wsend(hwnd, WM_PAINT, 0, 0);
        if (w) w->invalid = 0;   /* a WM_PAINT handler that never calls BeginPaint would loop forever */
        RET(1, r);
    }
    RET(1, wsend(hwnd, msg, rd32(p + 8), rd32(p + 12)));
}
static void u_PostMessageA(CPU *c) {
    uint32_t hwnd = ARG(0);
    if (!hwnd) hwnd = main_window();
    w32_post_message(hwnd, ARG(1), ARG(2), ARG(3));
    RET(4, 1);
}
static void u_SendMessageA(CPU *c) { RET(4, wsend(ARG(0), ARG(1), ARG(2), ARG(3))); }
static void u_PostQuitMessage(CPU *c) { quit_posted = 1; quit_code = ARG(0); RET(1, 0); }
static void u_IsDialogMessageA(CPU *c) { RET(2, 0); }

/* -- focus, capture, cursor, keyboard ---------------------------------- */
static void u_SetFocus(CPU *c) {
    uint32_t old = focus_hwnd, n = ARG(0);
    if (n != old) {
        if (old) wsend(old, WM_KILLFOCUS, n, 0);
        focus_hwnd = n;
        if (n) wsend(n, WM_SETFOCUS, old, 0);
    }
    RET(1, old);
}
static void u_GetFocus(CPU *c) { RET(0, focus_hwnd); }
static void u_SetForegroundWindow(CPU *c) { RET(1, 1); }
static void u_SetCapture(CPU *c) { uint32_t old = capture_hwnd; capture_hwnd = ARG(0); RET(1, old); }
static void u_ReleaseCapture(CPU *c) { capture_hwnd = 0; RET(0, 1); }
static void u_GetKeyState(CPU *c) {
    uint32_t vk = ARG(0) & 0xFF;
    uint8_t s = keystate[vk];
    if (vk == VK_LSHIFT || vk == VK_RSHIFT) s = keystate[VK_SHIFT];
    if (vk == VK_LCONTROL || vk == VK_RCONTROL) s = keystate[VK_CONTROL];
    RET(1, (uint32_t)(int32_t)(int16_t)((s & 0x80 ? 0xFF80 : 0) | (s & 1)));
}
static void u_GetCursorPos(CPU *c) {
    wr32(ARG(0), (uint32_t)mouse_x); wr32(ARG(0) + 4, (uint32_t)mouse_y);
    RET(1, 1);
}
static void u_SetCursorPos(CPU *c) {
    int x = (int)ARG(0), y = (int)ARG(1);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > 639) x = 639;
    if (y > 479) y = 479;
    if (x != mouse_x || y != mouse_y) { mouse_x = x; mouse_y = y; mouse_message(WM_MOUSEMOVE); }
    RET(2, 1);
}
static void u_mouse_event(CPU *c) {
    uint32_t f = ARG(0);
    if (f & MOUSEEVENTF_MOVE) {
        if (f & MOUSEEVENTF_ABSOLUTE) { mouse_x = (int)(ARG(1) * 640u / 65536u); mouse_y = (int)(ARG(2) * 480u / 65536u); }
        else { mouse_x += (int32_t)ARG(1); mouse_y += (int32_t)ARG(2); }
        mouse_message(WM_MOUSEMOVE);
    }
    if (f & MOUSEEVENTF_LEFTDOWN) { keystate[VK_LBUTTON] = 0x80; mouse_message(WM_LBUTTONDOWN); }
    if (f & MOUSEEVENTF_LEFTUP) { keystate[VK_LBUTTON] = 0; mouse_message(WM_LBUTTONUP); }
    if (f & MOUSEEVENTF_RIGHTDOWN) { keystate[VK_RBUTTON] = 0x80; mouse_message(WM_RBUTTONDOWN); }
    if (f & MOUSEEVENTF_RIGHTUP) { keystate[VK_RBUTTON] = 0; mouse_message(WM_RBUTTONUP); }
    RET(5, 0);
}

/* Cursors: system cursors (IDC_*) map to the host arrow; resource cursors of a
   guest module are created with CreateIconFromResourceEx (window.cpp shows them). */
static int cursor_count = 1;
static void u_LoadCursorA(CPU *c) {
    uint32_t h = ARG(0), name = ARG(1);
    if (!h || name < 0x10000u) {
        /* system cursor, or a resource id */
        if (!h) RET(2, 0x00010000u | (name & 0xFFFF));
    }
    uint32_t grp_size = 0;
    uint32_t grp = w32_find_resource(h, 12 /* RT_GROUP_CURSOR */, name, &grp_size);
    if (!grp) RET(2, 0x00010000u | 32512u);
    /* first entry of the group: ... nID at offset 6 + 12 */
    uint32_t id = rd16(grp + 6 + 12);
    uint32_t sz = 0, data = w32_find_resource(h, 1 /* RT_CURSOR */, id, &sz);
    if (!data) RET(2, 0x00010000u | 32512u);
    HCURSOR hc = CreateIconFromResourceEx((PBYTE)G(data), sz, FALSE, 0x00030000, 0, 0, 0);
    cursor_count++;
    RET(2, HG(hc));
}
static void u_LoadIconA(CPU *c) { RET(2, 0x00010000u | 32512u); }
static uint32_t cur_cursor;
static int cursor_shown = 0;   /* ShowCursor counter (0 = visible) */
void w32_host_set_cursor_shape(uint32_t hcursor);   /* window.cpp */
void w32_set_cursor(uint32_t h) {
    cur_cursor = h;
    if (h) w32_host_set_cursor_shape(h);
    w32_host_show_cursor(h != 0 && cursor_shown >= 0);
}
static void u_SetCursor(CPU *c) { uint32_t old = cur_cursor; w32_set_cursor(ARG(0)); RET(1, old); }
static void u_ShowCursor(CPU *c) {
    cursor_shown += ARG(0) ? 1 : -1;
    w32_host_show_cursor(cursor_shown >= 0 && cur_cursor != 0);
    RET(1, (uint32_t)cursor_shown);
}

/* -- strings, rects, dialogs --------------------------------------------- */
static void u_LoadStringA(CPU *c) {
    uint32_t h = ARG(0), id = ARG(1), buf = ARG(2), n = ARG(3);
    uint32_t sz, blk = w32_find_resource(h, 6 /* RT_STRING */, id / 16 + 1, &sz);
    if (!blk || !n) { W32_LOG("LoadString %08x %#x: not found", h, id); if (n) ((char *)G(buf))[0] = 0; RET(4, 0); }
    uint32_t p = blk;
    for (uint32_t i = 0; i < (id & 15); i++) p += 2 + 2u * rd16(p);
    uint32_t len = rd16(p);
    int r = WideCharToMultiByte(CP_ACP, 0, (LPCWSTR)G(p + 2), (int)len, (char *)G(buf), (int)n - 1, NULL, NULL);
    ((char *)G(buf))[r] = 0;
    W32_LOG("LoadString %08x %#x -> '%s'", h, id, (char *)G(buf));
    RET(4, (uint32_t)r);
}
static void u_CharNextA(CPU *c) {
    const char *s = GS(ARG(0));
    RET(1, ARG(0) + (uint32_t)(CharNextA(s) - s));
}
static void u_OffsetRect(CPU *c) {
    uint32_t r = ARG(0);
    wr32(r, rd32(r) + ARG(1)); wr32(r + 8, rd32(r + 8) + ARG(1));
    wr32(r + 4, rd32(r + 4) + ARG(2)); wr32(r + 12, rd32(r + 12) + ARG(2));
    RET(3, 1);
}
static void u_SetRect(CPU *c) { get_rect(ARG(0), (int)ARG(1), (int)ARG(2), (int)ARG(3), (int)ARG(4)); RET(5, 1); }
static void u_PtInRect(CPU *c) {
    RECT r;
    memcpy(&r, G(ARG(0)), 16);
    POINT p = {(LONG)ARG(1), (LONG)ARG(2)};
    RET(3, PtInRect(&r, p));
}
static void u_IntersectRect(CPU *c) {
    RECT a, b, d;
    memcpy(&a, G(ARG(1)), 16); memcpy(&b, G(ARG(2)), 16);
    BOOL r = IntersectRect(&d, &a, &b);
    memcpy(G(ARG(0)), &d, 16);
    RET(3, r);
}
static void u_MessageBoxA(CPU *c) {
    fprintf(stderr, "MessageBox: %s: %s\n", ARG(2) ? GS(ARG(2)) : "", ARG(1) ? GS(ARG(1)) : "");
    if (!w32_host_scripted()) MessageBoxA(NULL, GS(ARG(1)), GS(ARG(2)), ARG(3) & ~MB_SYSTEMMODAL);
    RET(4, IDOK);
}
/* The only dialog (Aaron's About box) is not emulated. */
static void u_DialogBoxParamA(CPU *c) { W32_STUB("DialogBoxParamA"); RET(5, IDOK); }
static void u_EndDialog(CPU *c) { RET(2, 1); }
static void u_GetDlgItem(CPU *c) { RET(2, 0); }
static void u_SetDlgItemTextA(CPU *c) { RET(3, 1); }

/* -- wsprintfA / wvsprintfA ----------------------------------------------- */
/* Formats with the guest argument list at guest address `args`. */
static int format(char *out, size_t cap, const char *fmt, uint32_t args) {
    size_t o = 0;
    #define PUT(ch) do { if (o + 1 < cap) out[o] = (ch); o++; } while (0)
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { PUT(*p); continue; }
        p++;
        if (*p == '%') { PUT('%'); continue; }
        char spec[32];
        size_t s = 0;
        spec[s++] = '%';
        while (*p == '-' || *p == '#' || *p == '0' || *p == '+' || *p == ' ') { if (s < 20) spec[s++] = *p; p++; }
        while (*p >= '0' && *p <= '9') { if (s < 20) spec[s++] = *p; p++; }
        if (*p == '.') { if (s < 20) spec[s++] = *p; p++; while (*p >= '0' && *p <= '9') { if (s < 20) spec[s++] = *p; p++; } }
        while (*p == 'l' || *p == 'h') p++;
        char conv = *p;
        if (!conv) break;
        char tmp[512];
        if (conv == 's') {
            spec[s++] = 's'; spec[s] = 0;
            uint32_t sp = rd32(args); args += 4;
            snprintf(tmp, sizeof tmp, spec, sp ? GS(sp) : "(null)");
        } else if (conv == 'c') {
            spec[s++] = 'c'; spec[s] = 0;
            snprintf(tmp, sizeof tmp, spec, (int)(rd32(args) & 0xFF)); args += 4;
        } else if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x' || conv == 'X') {
            spec[s++] = conv; spec[s] = 0;
            uint32_t v = rd32(args); args += 4;
            if (conv == 'd' || conv == 'i') snprintf(tmp, sizeof tmp, spec, (int)v);
            else snprintf(tmp, sizeof tmp, spec, (unsigned)v);
        } else { tmp[0] = conv; tmp[1] = 0; }
        for (char *t = tmp; *t; t++) PUT(*t);
    }
    if (cap) out[o < cap ? o : cap - 1] = 0;
    return (int)(o < 1024 ? o : 1024);
    #undef PUT
}
static void u_wsprintfA(CPU *c) {   /* cdecl */
    char buf[1025];
    int n = format(buf, sizeof buf, GS(ARG(1)), c->esp + 12);
    memcpy(G(ARG(0)), buf, (size_t)n + 1);
    RETC(n);
}
static void u_wvsprintfA(CPU *c) {
    char buf[1025];
    int n = format(buf, sizeof buf, GS(ARG(1)), ARG(2));
    memcpy(G(ARG(0)), buf, (size_t)n + 1);
    RET(3, n);
}

#define U(n, f) {"user32.dll", n, f}
const rc_hle_entry w32_user32_table[] = {
    U("DefWindowProcA", u_DefWindowProcA), U("CallWindowProcA", u_CallWindowProcA),
    U("RegisterClassA", u_RegisterClassA), U("UnregisterClassA", u_UnregisterClassA),
    U("GetClassInfoA", u_GetClassInfoA), U("CreateWindowExA", u_CreateWindowExA),
    U("DestroyWindow", u_DestroyWindow), U("ShowWindow", u_ShowWindow), U("UpdateWindow", u_UpdateWindow),
    U("IsWindowVisible", u_IsWindowVisible), U("IsIconic", u_IsIconic), U("EnableWindow", u_EnableWindow),
    U("FindWindowA", u_FindWindowA), U("SetWindowTextA", u_SetWindowTextA), U("GetWindowTextA", u_GetWindowTextA),
    U("GetWindowLongA", u_GetWindowLongA), U("SetWindowLongA", u_SetWindowLongA),
    U("GetClientRect", u_GetClientRect), U("AdjustWindowRectEx", u_AdjustWindowRectEx),
    U("SetWindowPos", u_SetWindowPos), U("GetWindowPlacement", u_GetWindowPlacement),
    U("SetWindowPlacement", u_SetWindowPlacement), U("ClientToScreen", u_ClientToScreen),
    U("MapWindowPoints", u_MapWindowPoints), U("InvalidateRect", u_InvalidateRect),
    U("ValidateRect", u_ValidateRect), U("BeginPaint", u_BeginPaint), U("EndPaint", u_EndPaint),
    U("GetDC", u_GetDC), U("ReleaseDC", u_ReleaseDC), U("FillRect", u_FillRect), U("FrameRect", u_FrameRect),
    U("DrawTextA", u_DrawTextA), U("ScrollDC", u_ScrollDC),
    U("PeekMessageA", u_PeekMessageA), U("TranslateMessage", u_TranslateMessage),
    U("DispatchMessageA", u_DispatchMessageA), U("PostMessageA", u_PostMessageA),
    U("SendMessageA", u_SendMessageA), U("PostQuitMessage", u_PostQuitMessage),
    U("IsDialogMessageA", u_IsDialogMessageA),
    U("SetFocus", u_SetFocus), U("GetFocus", u_GetFocus), U("SetForegroundWindow", u_SetForegroundWindow),
    U("SetCapture", u_SetCapture), U("ReleaseCapture", u_ReleaseCapture), U("GetKeyState", u_GetKeyState),
    U("GetCursorPos", u_GetCursorPos), U("SetCursorPos", u_SetCursorPos), U("mouse_event", u_mouse_event),
    U("LoadCursorA", u_LoadCursorA), U("LoadIconA", u_LoadIconA), U("SetCursor", u_SetCursor),
    U("ShowCursor", u_ShowCursor), U("LoadStringA", u_LoadStringA), U("CharNextA", u_CharNextA),
    U("OffsetRect", u_OffsetRect), U("SetRect", u_SetRect), U("PtInRect", u_PtInRect),
    U("IntersectRect", u_IntersectRect), U("MessageBoxA", u_MessageBoxA),
    U("DialogBoxParamA", u_DialogBoxParamA), U("EndDialog", u_EndDialog), U("GetDlgItem", u_GetDlgItem),
    U("SetDlgItemTextA", u_SetDlgItemTextA), U("wsprintfA", u_wsprintfA), U("wvsprintfA", u_wvsprintfA),
    {NULL, NULL, NULL}
};
