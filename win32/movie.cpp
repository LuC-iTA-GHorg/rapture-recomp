// MCIWnd (msvfw32 MCIWndCreateA) for the intro movies, on rapture-engine's
// platform::Movie (AVI demuxer + Microsoft Video 1 decoder + PCM track).
//
// The MCI window is a virtual child window whose window procedure is host code.
// Messages used by moses.dll: MCIWNDM_GETEND, MCIWNDM_GETPOSITIONA, MCIWNDM_GET_SOURCE,
// MCI_SEEK (to the start), MCIWNDM_PLAYTO (to the end), MCI_PAUSE / MCI_RESUME,
// WM_CLOSE; the parent receives MCIWNDM_NOTIFYPOS while the movie plays.
// Positions are frame numbers (the AVI time format of MCI).
#include "w32.h"

#include "platform/Movie.h"

#include <cstring>
#include <memory>
#include <string>

extern "C" {
uint32_t w32_create_child(uint32_t parent, uint32_t wndproc, int x, int y, int w, int h);
void w32_window_screen_rect(uint32_t hwnd, int* x, int* y, int* w, int* h);
int w32_window_visible(uint32_t hwnd);
void w32_destroy_window(uint32_t hwnd);
void w32_screen_dirty(void);
int w32_audio_play_host(const int16_t* s, uint32_t frames, int ch, int rate);
void w32_audio_stop_host(int k);
double w32_audio_host_seconds(int k);
}

namespace {

constexpr uint32_t kWmClose = 0x0010;
constexpr uint32_t kGetPosition = 0x0466, kGetEnd = 0x0469, kPlayTo = 0x047B, kGetSource = 0x048E;
constexpr uint32_t kNotifyPos = 0x04C9;
constexpr uint32_t kMciSeek = 0x0807, kMciStop = 0x0808, kMciPause = 0x0809, kMciResume = 0x0855;

struct MciWnd {
    uint32_t hwnd = 0, parent = 0;
    std::unique_ptr<Movie> movie;
    int pos = 0;                  // frames shown
    bool playing = false, paused = false;
    uint32_t t0 = 0, pausedAt = 0;
    int voice = -1;
};
MciWnd g_mci[4];

MciWnd* Find(uint32_t hwnd) {
    for (auto& m : g_mci) if (m.hwnd && m.hwnd == hwnd) return &m;
    return nullptr;
}

void Stop(MciWnd& m) {
    m.playing = false;
    if (m.voice >= 0) { w32_audio_stop_host(m.voice); m.voice = -1; }
}

void Draw(MciWnd& m) {
    if (!w32_window_visible(m.hwnd)) return;
    int x, y, w, h;
    w32_window_screen_rect(m.hwnd, &x, &y, &w, &h);
    const Surface& pic = m.movie->Picture();
    uint32_t* screen = w32_screen_pixels();
    for (int row = 0; row < pic.Height(); ++row) {
        int sy = y + row;
        if (sy < 0 || sy >= 480) continue;
        const uint32_t* src = pic.Row(row);
        for (int col = 0; col < pic.Width(); ++col) {
            int sx = x + col;
            if (sx >= 0 && sx < 640) screen[sy * 640 + sx] = src[col] & 0x00FFFFFFu;
        }
    }
    w32_screen_dirty();
}

void WndProc(CPU* c) {
    uint32_t hwnd = ARG(0), msg = ARG(1), wp = ARG(2), lp = ARG(3);
    (void)wp;
    MciWnd* m = Find(hwnd);
    uint32_t r = 0;
    if (m) {
        switch (msg) {
        case kGetEnd: r = uint32_t(m->movie->FrameCount()); break;
        case kGetPosition: r = uint32_t(m->pos); break;
        case kGetSource:
            if (lp) { wr32(lp, 0); wr32(lp + 4, 0); wr32(lp + 8, uint32_t(m->movie->Width())); wr32(lp + 12, uint32_t(m->movie->Height())); }
            break;
        case kMciSeek:
            Stop(*m);
            m->movie->Rewind();
            m->pos = 0;
            break;
        case kPlayTo:
            m->playing = true;
            m->paused = false;
            m->t0 = w32_host_ticks();
            if (m->movie->AudioTrack()) {
                const auto& a = m->movie->AudioTrack();
                m->voice = w32_audio_play_host(a->samples.data(), uint32_t(a->Frames()), a->channels, a->rate);
            }
            W32_LOG("MCIWnd play (%d frames, %.1f fps)", m->movie->FrameCount(), m->movie->Fps());
            break;
        case kMciStop: Stop(*m); break;
        case kMciPause: if (m->playing && !m->paused) { m->paused = true; m->pausedAt = w32_host_ticks(); } break;
        case kMciResume: if (m->paused) { m->paused = false; m->t0 += w32_host_ticks() - m->pausedAt; } break;
        case kWmClose:
            Stop(*m);
            w32_destroy_window(hwnd);
            m->movie.reset();
            m->hwnd = 0;
            break;
        default: break;
        }
    }
    RET(4, r);
}

}  // namespace

// HWND VFWAPIV MCIWndCreateA(HWND parent, HINSTANCE, DWORD style, LPCSTR file): cdecl.
extern "C" void w32_mciwnd_create(CPU* c) {
    uint32_t parent = ARG(0), file = ARG(3);
    MciWnd* slot = nullptr;
    for (auto& m : g_mci) if (!m.hwnd) { slot = &m; break; }
    if (!slot || !file) RETC(0);
    auto mv = std::make_unique<Movie>();
    std::string err;
    const char* name = static_cast<const char*>(rc_ptr(file));
    if (!mv->Open(name, &err)) {
        std::fprintf(stderr, "w32: cannot open movie %s: %s\n", name, err.c_str());
        RETC(0);
    }
    uint32_t proc = rc_host_thunk(WndProc, "MCIWnd");
    uint32_t hwnd = w32_create_child(parent, proc, 0, 0, mv->Width(), mv->Height());
    if (!hwnd) RETC(0);
    slot->hwnd = hwnd;
    slot->parent = parent;
    slot->movie = std::move(mv);
    slot->pos = 0;
    slot->playing = false;
    W32_LOG("MCIWndCreate %s -> %08x", name, hwnd);
    RETC(hwnd);
}

// Called from the message pump: decode the frames that are due, draw them and
// notify the parent of the new position.
extern "C" void w32_movie_tick(void) {
    for (auto& m : g_mci) {
        if (!m.hwnd || !m.playing || m.paused || !m.movie) continue;
        double t = (w32_host_ticks() - m.t0) / 1000.0;
        double at = m.voice >= 0 ? w32_audio_host_seconds(m.voice) : -1;
        if (at >= 0) t = at;
        int due = int(t * m.movie->Fps()) + 1;
        if (due > m.movie->FrameCount()) due = m.movie->FrameCount();
        bool changed = false;
        while (m.pos < due && m.movie->DecodeNext()) { ++m.pos; changed = true; }
        if (m.pos < due) m.pos = m.movie->FrameCount();   // decoding error: end the movie
        if (changed) Draw(m);
        bool ended = m.pos >= m.movie->FrameCount() && (m.voice < 0 || w32_audio_host_seconds(m.voice) < 0);
        if (changed || ended) w32_post_message(m.parent, kNotifyPos, m.hwnd, uint32_t(m.pos));
        if (ended) Stop(m);
    }
}
