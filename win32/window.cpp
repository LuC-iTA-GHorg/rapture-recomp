// Host side of the guest's window: rapture-engine's platform::Window (SDL2,
// 640x480 framebuffer scaled to the window, F11/Alt+Enter fullscreen), plus a
// scripted mode for unattended runs.
//
// Options (parsed by w32_host_parse_args):
//   --autoplay SCRIPT   hidden window, real input ignored, events come from SCRIPT
//   --outdir DIR        where `shot` writes its PNG files (default .)
//   --fast N            the game clock runs N times faster (timeGetTime, waits)
//   --fullscreen        start in desktop fullscreen
//
// Script (one command per line, '#' comment; coordinates in 640x480):
//   wait MS | move X Y | click X Y | rclick X Y | down X Y | up X Y
//   key NAME (space esc enter tab backspace up down left right shift ctrl alt f1..f12 a..z 0..9)
//   keydown NAME | keyup NAME (hold a key, e.g. to steer: GetKeyState sees it held)
//   shot NAME (writes OUTDIR/NAME.png) | quit
//   waitpix X0 Y0 X1 Y1 RRGGBB TOL TIMEOUT
//     waits until some pixel of the screen in the rectangle X0..X1, Y0..Y1 (inclusive) is
//     within TOL of the colour RRGGBB on every channel, for at most TIMEOUT game ms; then
//     the script goes on (a timeout is reported on stderr). Use it where a fixed wait drifts,
//     e.g. to wait for a dialog; keys held with keydown stay held meanwhile.
#include "w32.h"

#include "platform/Window.h"

#include <SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

Window g_win;
bool g_open = false;
bool g_fullscreen = false;
std::string g_outdir = ".";

struct Cmd { std::string op, s; int a = 0, b = 0, line = 0; int x1 = 0, y1 = 0, tol = 0, timeout = 0; uint32_t rgb = 0; };
std::vector<Cmd> g_script;
size_t g_pc = 0;
bool g_scripted = false;
uint32_t g_wait_until = 0;
int g_mx = 320, g_my = 240;
struct Pending { w32_host_event e; uint32_t due; };
std::vector<Pending> g_pending;   // scripted events, in order, each due at a game time

int KeyFromName(const std::string& n) {
    static const struct { const char* name; int key; } names[] = {
        {"space", SDLK_SPACE}, {"esc", SDLK_ESCAPE}, {"escape", SDLK_ESCAPE}, {"enter", SDLK_RETURN},
        {"return", SDLK_RETURN}, {"tab", SDLK_TAB}, {"backspace", SDLK_BACKSPACE}, {"up", SDLK_UP},
        {"down", SDLK_DOWN}, {"left", SDLK_LEFT}, {"right", SDLK_RIGHT}, {"plus", SDLK_KP_PLUS},
        {"minus", SDLK_KP_MINUS}, {"shift", SDLK_LSHIFT}, {"ctrl", SDLK_LCTRL}, {"alt", SDLK_LALT},
    };
    for (auto& k : names) if (n == k.name) return k.key;
    if (n.size() >= 2 && n[0] == 'f' && isdigit((unsigned char)n[1])) return SDLK_F1 + std::atoi(n.c_str() + 1) - 1;
    if (n.size() == 1) return (unsigned char)n[0];
    return 0;
}

bool LoadScript(const char* path) {
    std::ifstream in(path);
    if (!in) { std::fprintf(stderr, "autoplay: cannot open %s\n", path); return false; }
    std::string line;
    int ln = 0;
    while (std::getline(in, line)) {
        ++ln;
        size_t h = line.find('#');
        if (h != std::string::npos) line.resize(h);
        std::istringstream ss(line);
        Cmd c;
        c.line = ln;
        if (!(ss >> c.op)) continue;
        if (c.op == "key" || c.op == "keydown" || c.op == "keyup" || c.op == "shot") ss >> c.s;
        else if (c.op == "waitpix") {
            std::string hex;
            if (!(ss >> c.a >> c.b >> c.x1 >> c.y1 >> hex >> c.tol >> c.timeout) || hex.size() != 6) {
                std::fprintf(stderr, "autoplay: line %d: waitpix X0 Y0 X1 Y1 RRGGBB TOL TIMEOUT\n", ln);
                return false;
            }
            c.rgb = uint32_t(std::strtoul(hex.c_str(), nullptr, 16));
        }
        else ss >> c.a >> c.b;
        if ((c.op == "key" || c.op == "keydown" || c.op == "keyup") && !KeyFromName(c.s)) { std::fprintf(stderr, "autoplay: line %d: unknown key %s\n", ln, c.s.c_str()); return false; }
        g_script.push_back(c);
    }
    return true;
}

// Queue a scripted event `delay` ms from now: a press and its release are kept
// apart like a person's, so the game runs frames in between (Aaron's buttons
// need to see the pressed state).
void Push(int type, int x, int y, int button, int key, uint32_t delay = 0) {
    w32_host_event e{};
    e.type = type; e.x = x; e.y = y; e.button = button; e.key = key;
    g_pending.push_back({e, Window::Ticks() + delay});
}

void Put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 3; i >= 0; --i) v.push_back(uint8_t(x >> (8 * i))); }
uint32_t Crc(const uint8_t* p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    while (n--) { c ^= *p++; for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u))); }
    return ~c;
}
void Chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    Put32(out, uint32_t(data.size()));
    std::vector<uint8_t> td(type, type + 4);
    td.insert(td.end(), data.begin(), data.end());
    out.insert(out.end(), td.begin(), td.end());
    Put32(out, Crc(td.data(), td.size()));
}
// Uncompressed (stored deflate blocks) RGB PNG.
bool WritePng(const std::string& path, Surface& s) {
    const int w = s.Width(), h = s.Height();
    std::vector<uint8_t> raw;
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        const uint32_t* row = s.Row(y);
        for (int x = 0; x < w; ++x) { raw.push_back(uint8_t(row[x] >> 16)); raw.push_back(uint8_t(row[x] >> 8)); raw.push_back(uint8_t(row[x])); }
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    for (size_t pos = 0; pos < raw.size();) {
        size_t n = std::min<size_t>(65535, raw.size() - pos);
        z.push_back(pos + n >= raw.size() ? 1 : 0);
        z.push_back(uint8_t(n)); z.push_back(uint8_t(n >> 8)); z.push_back(uint8_t(~n)); z.push_back(uint8_t((~n) >> 8));
        z.insert(z.end(), raw.begin() + long(pos), raw.begin() + long(pos + n));
        pos += n;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t ch : raw) { a = (a + ch) % 65521; b = (b + a) % 65521; }
    Put32(z, (b << 16) | a);
    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a}, ihdr;
    Put32(ihdr, uint32_t(w)); Put32(ihdr, uint32_t(h));
    for (uint8_t v : {8, 2, 0, 0, 0}) ihdr.push_back(v);
    Chunk(out, "IHDR", ihdr);
    Chunk(out, "IDAT", z);
    Chunk(out, "IEND", {});
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
    return true;
}

// True when some pixel of the framebuffer in the command's rectangle matches its colour.
bool PixelMatch(const Cmd& c) {
    Surface& fb = g_win.Framebuffer();
    const int x0 = std::max(0, c.a), y0 = std::max(0, c.b);
    const int x1 = std::min(Window::kWidth - 1, c.x1), y1 = std::min(Window::kHeight - 1, c.y1);
    const int r = int(c.rgb >> 16) & 255, g = int(c.rgb >> 8) & 255, b = int(c.rgb) & 255;
    for (int y = y0; y <= y1; ++y) {
        const uint32_t* row = fb.Row(y);
        for (int x = x0; x <= x1; ++x) {
            const int pr = int(row[x] >> 16) & 255, pg = int(row[x] >> 8) & 255, pb = int(row[x]) & 255;
            if (std::abs(pr - r) <= c.tol && std::abs(pg - g) <= c.tol && std::abs(pb - b) <= c.tol) return true;
        }
    }
    return false;
}
size_t g_waitpix_pc = size_t(-1);   // the waitpix being waited on, and since when
uint32_t g_waitpix_since = 0;

// Runs script commands until one has to wait.
void RunScript() {
    uint32_t now = Window::Ticks();
    while (g_pc < g_script.size() && now >= g_wait_until) {
        const Cmd& c = g_script[g_pc++];
        const int settle = 60;
        if (c.op == "wait") g_wait_until = now + uint32_t(c.a);
        else if (c.op == "waitpix") {
            const size_t pc = g_pc - 1;
            if (g_waitpix_pc != pc) { g_waitpix_pc = pc; g_waitpix_since = now; }
            const uint32_t waited = now - g_waitpix_since;
            if (PixelMatch(c)) {
                std::printf("autoplay: %u ms waitpix line %d matched after %u ms\n", now, c.line, waited);
                std::fflush(stdout);
            } else if (waited >= uint32_t(c.timeout)) {
                std::fprintf(stderr, "autoplay: %u ms waitpix line %d timed out after %u ms\n", now, c.line, waited);
            } else {
                g_pc = pc;   // look again at the next poll
                break;
            }
            g_waitpix_pc = size_t(-1);
        }
        else if (c.op == "move") { g_mx = c.a; g_my = c.b; Push(W32_EV_MOUSEMOVE, c.a, c.b, 0, 0); g_wait_until = now + settle; }
        else if (c.op == "click" || c.op == "rclick") {
            int btn = c.op == "click" ? 1 : 3;
            g_mx = c.a; g_my = c.b;
            Push(W32_EV_MOUSEMOVE, c.a, c.b, 0, 0);
            Push(W32_EV_MOUSEDOWN, c.a, c.b, btn, 0, 80);
            Push(W32_EV_MOUSEUP, c.a, c.b, btn, 0, 200);
            g_wait_until = now + 300;
        } else if (c.op == "down") { Push(W32_EV_MOUSEDOWN, c.a, c.b, 1, 0); g_wait_until = now + settle; }
        else if (c.op == "up") { Push(W32_EV_MOUSEUP, c.a, c.b, 1, 0); g_wait_until = now + settle; }
        else if (c.op == "key") {
            int k = KeyFromName(c.s);
            Push(W32_EV_KEYDOWN, g_mx, g_my, 0, k);
            Push(W32_EV_KEYUP, g_mx, g_my, 0, k, 120);
            g_wait_until = now + 200;
        } else if (c.op == "keydown" || c.op == "keyup") {
            Push(c.op == "keydown" ? W32_EV_KEYDOWN : W32_EV_KEYUP, g_mx, g_my, 0, KeyFromName(c.s));
            g_wait_until = now + settle;
        } else if (c.op == "shot") {
            std::string p = g_outdir + "/" + c.s + ".png";
            if (!WritePng(p, g_win.Framebuffer())) std::fprintf(stderr, "autoplay: cannot write %s\n", p.c_str());
            else std::printf("autoplay: %u ms shot %s\n", now, p.c_str());
            std::fflush(stdout);
        } else if (c.op == "quit") { Push(W32_EV_QUIT, 0, 0, 0, 0); g_wait_until = 0xFFFFFFFFu; }
        else std::fprintf(stderr, "autoplay: line %d: unknown command %s\n", c.line, c.op.c_str());
    }
    if (g_pc >= g_script.size() && now >= g_wait_until) { Push(W32_EV_QUIT, 0, 0, 0, 0); g_wait_until = 0xFFFFFFFFu; }
}

}  // namespace

extern "C" int w32_host_parse_args(int argc, char** argv) {
    double fast = 1.0;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--autoplay" && i + 1 < argc) {
            if (!LoadScript(argv[++i])) return 1;
            g_scripted = true;
        } else if (a == "--outdir" && i + 1 < argc) { char full[1024]; g_outdir = _fullpath(full, argv[++i], sizeof full) ? full : argv[i]; }
        else if (a == "--fast" && i + 1 < argc) fast = std::atof(argv[++i]);
        else if (a == "--fullscreen") g_fullscreen = true;
    }
    if (g_scripted) {
        Window::SetScriptedMode(true);
        SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    }
    if (fast > 1.0) Window::SetTimeScale(fast);
    return 0;
}

extern "C" int w32_host_window_open(const char* title) {
    if (g_open) return 1;
    if (!g_win.Create(title, g_fullscreen)) { std::fprintf(stderr, "cannot create the window: %s\n", SDL_GetError()); return 0; }
    g_open = true;
    return 1;
}

extern "C" int w32_host_poll(w32_host_event* ev) {
    if (!g_open && !g_scripted) return 0;
    if (g_scripted) {
        InputEvent drop;
        if (g_open) while (g_win.PollEvent(drop)) {}   // keep SDL alive; real input is ignored
        if (g_pending.empty()) RunScript();
        if (g_pending.empty() || g_pending.front().due > Window::Ticks()) return 0;
        *ev = g_pending.front().e;
        g_pending.erase(g_pending.begin());
        return 1;
    }
    InputEvent e;
    if (!g_win.PollEvent(e)) return 0;
    std::memset(ev, 0, sizeof *ev);
    switch (e.type) {
    case InputEvent::kQuit: ev->type = W32_EV_QUIT; break;
    case InputEvent::kMouseMove: ev->type = W32_EV_MOUSEMOVE; break;
    case InputEvent::kMouseDown: ev->type = W32_EV_MOUSEDOWN; break;
    case InputEvent::kMouseUp: ev->type = W32_EV_MOUSEUP; break;
    case InputEvent::kKeyDown: ev->type = W32_EV_KEYDOWN; break;
    case InputEvent::kKeyUp: ev->type = W32_EV_KEYUP; break;
    case InputEvent::kText: ev->type = W32_EV_TEXT; break;
    default: ev->type = W32_EV_NONE; break;
    }
    ev->x = e.x; ev->y = e.y; ev->button = e.button; ev->key = e.key; ev->mod = e.mod; ev->text = (unsigned char)e.text;
    return 1;
}

extern "C" void w32_host_present(const uint32_t* argb) {
    if (!g_open && !g_scripted) return;
    Surface& fb = g_win.Framebuffer();
    for (int y = 0; y < Window::kHeight; ++y)
        std::memcpy(fb.Row(y), argb + size_t(y) * Window::kWidth, Window::kWidth * 4);
    if (g_open) g_win.Present();
}

extern "C" void w32_host_show_cursor(int show) { if (g_open) g_win.ShowCursor(show != 0); }

// Cursor shape: a host HCURSOR (from a guest cursor resource) becomes an SDL color
// cursor, scaled with the window; anything else (system cursors) is the SDL arrow.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <map>
extern "C" void w32_host_set_cursor_shape(uint32_t hcursor) {
    static std::map<uint32_t, SDL_Cursor*> cache;
    static uint32_t current = 0;
    if (!g_open || hcursor == current) return;
    current = hcursor;
    SDL_Cursor* cur = nullptr;
    if (hcursor && (hcursor & 0xFFFF0000u) != 0x00010000u) {
        auto it = cache.find(hcursor);
        if (it != cache.end()) cur = it->second;
        else {
            HCURSOR hc = (HCURSOR)(intptr_t)(int32_t)hcursor;
            ICONINFO ii;
            if (GetIconInfo(hc, &ii)) {
                BITMAP bm{};
                GetObject(ii.hbmMask, sizeof bm, &bm);
                int w = bm.bmWidth, h = ii.hbmColor ? bm.bmHeight : bm.bmHeight / 2;
                HDC dc = CreateCompatibleDC(nullptr);
                BITMAPINFO bi{};
                bi.bmiHeader.biSize = sizeof bi.bmiHeader;
                bi.bmiHeader.biWidth = w;
                bi.bmiHeader.biHeight = -h;
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                void* bits = nullptr;
                HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
                HGDIOBJ old = SelectObject(dc, dib);
                // Draw the cursor twice, on black and on white: equal pixels are opaque,
                // pixels that follow the background are transparent.
                std::vector<uint32_t> onBlack(size_t(w) * h), onWhite(size_t(w) * h);
                PatBlt(dc, 0, 0, w, h, BLACKNESS);
                DrawIconEx(dc, 0, 0, hc, w, h, 0, nullptr, DI_NORMAL);
                GdiFlush();
                std::memcpy(onBlack.data(), bits, onBlack.size() * 4);
                PatBlt(dc, 0, 0, w, h, WHITENESS);
                DrawIconEx(dc, 0, 0, hc, w, h, 0, nullptr, DI_NORMAL);
                GdiFlush();
                std::memcpy(onWhite.data(), bits, onWhite.size() * 4);
                SelectObject(dc, old);
                DeleteObject(dib);
                DeleteDC(dc);
                SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
                if (s) {
                    uint32_t* px = static_cast<uint32_t*>(s->pixels);
                    for (int i = 0; i < w * h; ++i) {
                        uint32_t b = onBlack[size_t(i)] & 0xFFFFFF, wh = onWhite[size_t(i)] & 0xFFFFFF;
                        px[i] = b == wh ? (0xFF000000u | b) : 0;
                    }
                    cur = SDL_CreateColorCursor(s, int(ii.xHotspot), int(ii.yHotspot));
                    SDL_FreeSurface(s);
                }
                if (ii.hbmColor) DeleteObject(ii.hbmColor);
                DeleteObject(ii.hbmMask);
            }
            cache[hcursor] = cur;
        }
    }
    SDL_SetCursor(cur ? cur : SDL_GetDefaultCursor());
}
extern "C" uint32_t w32_host_ticks(void) { return Window::Ticks(); }
extern "C" int w32_host_scripted(void) { return g_scripted; }
