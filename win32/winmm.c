/* WINMM: time, mmio (forwarded), MIDI output (forwarded to the host synthesizer),
 * joysticks (forwarded; capture emulated by polling), mixer and CD audio (none). */
#include "w32.h"

#include <windows.h>
#include <mmsystem.h>

int w32_host_scripted(void);

/* Host handles that are pointers on 64-bit Windows (HMMIO, HMIDIOUT) go
   through a small table; guest handles are 0x00B00000 + 4 * index. */
#define HT_BASE 0x00B00000u
#define HT_MAX 256
static void *htab[HT_MAX];
static uint32_t ht_put(void *h) {
    for (uint32_t k = 0; k < HT_MAX; k++) if (!htab[k]) { htab[k] = h; return HT_BASE + 4 * k; }
    return 0;
}
static void *ht_get(uint32_t g) {
    uint32_t k = (g - HT_BASE) / 4;
    return (g >= HT_BASE && k < HT_MAX) ? htab[k] : NULL;
}
static void ht_del(uint32_t g) { uint32_t k = (g - HT_BASE) / 4; if (g >= HT_BASE && k < HT_MAX) htab[k] = NULL; }

static void m_timeGetTime(CPU *c) { RET(0, w32_host_ticks()); }

/* -- mmio ----------------------------------------------------------------- */
static void m_mmioOpenA(CPU *c) {
    const char *name = GS(ARG(0));
    uint32_t info = ARG(1), flags = ARG(2);
    MMIOINFO mi;
    memset(&mi, 0, sizeof mi);
    HMMIO h = mmioOpenA((LPSTR)name, info ? &mi : NULL, flags);
    if (info) wr32(info + 0x24, mi.wErrorRet);   /* MMIOINFO.wErrorRet (32-bit layout) */
    W32_LOG("mmioOpen %s %08x -> %p", name ? name : "(null)", flags, (void *)h);
    RET(3, h ? ht_put(h) : 0);
}
static void m_mmioClose(CPU *c) { HMMIO h = (HMMIO)ht_get(ARG(0)); ht_del(ARG(0)); RET(2, h ? mmioClose(h, ARG(1)) : MMSYSERR_INVALHANDLE); }
static void m_mmioRead(CPU *c) { RET(3, (uint32_t)mmioRead((HMMIO)ht_get(ARG(0)), (HPSTR)G(ARG(1)), (LONG)ARG(2))); }
static void m_mmioWrite(CPU *c) { RET(3, (uint32_t)mmioWrite((HMMIO)ht_get(ARG(0)), (const char *)G(ARG(1)), (LONG)ARG(2))); }
static void m_mmioSeek(CPU *c) { RET(3, (uint32_t)mmioSeek((HMMIO)ht_get(ARG(0)), (LONG)ARG(1), (int)ARG(2))); }
static void m_mmioFlush(CPU *c) { RET(2, mmioFlush((HMMIO)ht_get(ARG(0)), ARG(1))); }
static void m_mmioDescend(CPU *c) {
    RET(4, mmioDescend((HMMIO)ht_get(ARG(0)), (LPMMCKINFO)G(ARG(1)), ARG(2) ? (const MMCKINFO *)G(ARG(2)) : NULL, ARG(3)));
}
static void m_mmioAscend(CPU *c) { RET(3, mmioAscend((HMMIO)ht_get(ARG(0)), (LPMMCKINFO)G(ARG(1)), ARG(2))); }

/* -- MIDI out ----------------------------------------------------------------- */
static void m_midiOutGetDevCapsA(CPU *c) {
    RET(3, midiOutGetDevCapsA((UINT_PTR)(int32_t)ARG(0), (LPMIDIOUTCAPSA)G(ARG(1)), ARG(2)));
}
static void m_midiOutOpen(CPU *c) {
    uint32_t out = ARG(0), dev = ARG(1), flags = ARG(4);
    if ((flags & CALLBACK_TYPEMASK) != CALLBACK_NULL) W32_LOG("midiOutOpen: callback type %08x ignored", flags);
    if (w32_host_scripted()) { wr32(out, ht_put((void *)1)); RET(5, MMSYSERR_NOERROR); }   /* silent */
    HMIDIOUT h = NULL;
    MMRESULT r = midiOutOpen(&h, (UINT)(int32_t)dev, 0, 0, CALLBACK_NULL);
    if (r == MMSYSERR_NOERROR) wr32(out, ht_put(h));
    RET(5, r);
}
static void m_midiOutShortMsg(CPU *c) {
    void *h = ht_get(ARG(0));
    if (h == (void *)1) RET(2, MMSYSERR_NOERROR);
    RET(2, h ? midiOutShortMsg((HMIDIOUT)h, ARG(1)) : MMSYSERR_INVALHANDLE);
}
static void m_midiOutReset(CPU *c) {
    void *h = ht_get(ARG(0));
    if (h == (void *)1) RET(1, MMSYSERR_NOERROR);
    RET(1, h ? midiOutReset((HMIDIOUT)h) : MMSYSERR_INVALHANDLE);
}
static void m_midiOutClose(CPU *c) {
    void *h = ht_get(ARG(0));
    ht_del(ARG(0));
    if (h == (void *)1) RET(1, MMSYSERR_NOERROR);
    RET(1, h ? midiOutClose((HMIDIOUT)h) : MMSYSERR_INVALHANDLE);
}

/* -- joysticks ------------------------------------------------------------------ */
static struct { uint32_t hwnd, period, changed, last_buttons, last_x, last_y, next; int on; } cap[2];

static void m_joyGetNumDevs(CPU *c) { RET(0, w32_host_scripted() ? 0 : joyGetNumDevs()); }
static void m_joyGetDevCapsA(CPU *c) {
    if (w32_host_scripted()) RET(3, JOYERR_UNPLUGGED);
    RET(3, joyGetDevCapsA((UINT_PTR)ARG(0), (LPJOYCAPSA)G(ARG(1)), ARG(2)));
}
static void m_joyGetPos(CPU *c) {
    if (w32_host_scripted()) RET(2, JOYERR_UNPLUGGED);
    RET(2, joyGetPos(ARG(0), (LPJOYINFO)G(ARG(1))));
}
static void m_joySetCapture(CPU *c) {
    uint32_t id = ARG(1);
    if (id > 1 || w32_host_scripted()) RET(4, JOYERR_UNPLUGGED);
    JOYINFO ji;
    if (joyGetPos(id, &ji) != JOYERR_NOERROR) RET(4, JOYERR_UNPLUGGED);
    cap[id].hwnd = ARG(0); cap[id].period = ARG(2) ? ARG(2) : 10; cap[id].changed = ARG(3);
    cap[id].last_buttons = ji.wButtons; cap[id].last_x = ji.wXpos; cap[id].last_y = ji.wYpos;
    cap[id].on = 1;
    RET(4, JOYERR_NOERROR);
}
static void m_joyReleaseCapture(CPU *c) { if (ARG(0) < 2) cap[ARG(0)].on = 0; RET(1, JOYERR_NOERROR); }

/* Called from the message pump: MM_JOYnMOVE / MM_JOYnBUTTONDOWN / UP for captured joysticks. */
void w32_poll_joysticks(void) {
    uint32_t now = w32_host_ticks();
    for (uint32_t id = 0; id < 2; id++) {
        if (!cap[id].on || now < cap[id].next) continue;
        cap[id].next = now + cap[id].period;
        JOYINFO ji;
        if (joyGetPos(id, &ji) != JOYERR_NOERROR) continue;
        uint32_t lp = ((ji.wYpos >> 0) << 16) | (ji.wXpos & 0xFFFF);
        uint32_t moved = !cap[id].changed || ji.wXpos != cap[id].last_x || ji.wYpos != cap[id].last_y;
        if (moved) w32_post_message(cap[id].hwnd, (id ? MM_JOY2MOVE : MM_JOY1MOVE), ji.wButtons, lp);
        uint32_t ch = ji.wButtons ^ cap[id].last_buttons;
        if (ch) {
            uint32_t down = ch & ji.wButtons, up = ch & ~ji.wButtons;
            if (down) w32_post_message(cap[id].hwnd, id ? MM_JOY2BUTTONDOWN : MM_JOY1BUTTONDOWN, (down << 8) | ji.wButtons, lp);
            if (up) w32_post_message(cap[id].hwnd, id ? MM_JOY2BUTTONUP : MM_JOY1BUTTONUP, (up << 8) | ji.wButtons, lp);
        }
        cap[id].last_buttons = ji.wButtons; cap[id].last_x = ji.wXpos; cap[id].last_y = ji.wYpos;
    }
}

/* -- mixer and CD audio: no devices --------------------------------------------- */
static void m_mixerGetNumDevs(CPU *c) { RET(0, 0); }
static void m_mixerOpen(CPU *c) { RET(5, MMSYSERR_NODRIVER); }
static void m_mixerClose(CPU *c) { RET(1, MMSYSERR_NOERROR); }
static void m_mixerGetDevCapsA(CPU *c) { RET(3, MMSYSERR_BADDEVICEID); }
static void m_mixerGetLineInfoA(CPU *c) { RET(3, MMSYSERR_INVALHANDLE); }
static void m_mixerGetLineControlsA(CPU *c) { RET(3, MMSYSERR_INVALHANDLE); }
static void m_mixerGetControlDetailsA(CPU *c) { RET(3, MMSYSERR_INVALHANDLE); }
static void m_mixerSetControlDetails(CPU *c) { RET(3, MMSYSERR_INVALHANDLE); }
static void m_mciSendCommandA(CPU *c) { RET(4, MCIERR_DEVICE_NOT_INSTALLED); }

#define M(n, f) {"winmm.dll", n, f}
const rc_hle_entry w32_winmm_table[] = {
    M("timeGetTime", m_timeGetTime),
    M("mmioOpenA", m_mmioOpenA), M("mmioClose", m_mmioClose), M("mmioRead", m_mmioRead), M("mmioWrite", m_mmioWrite),
    M("mmioSeek", m_mmioSeek), M("mmioFlush", m_mmioFlush), M("mmioDescend", m_mmioDescend), M("mmioAscend", m_mmioAscend),
    M("midiOutGetDevCapsA", m_midiOutGetDevCapsA), M("midiOutOpen", m_midiOutOpen), M("midiOutShortMsg", m_midiOutShortMsg),
    M("midiOutReset", m_midiOutReset), M("midiOutClose", m_midiOutClose),
    M("joyGetNumDevs", m_joyGetNumDevs), M("joyGetDevCapsA", m_joyGetDevCapsA), M("joyGetPos", m_joyGetPos),
    M("joySetCapture", m_joySetCapture), M("joyReleaseCapture", m_joyReleaseCapture),
    M("mixerGetNumDevs", m_mixerGetNumDevs), M("mixerOpen", m_mixerOpen), M("mixerClose", m_mixerClose),
    M("mixerGetDevCapsA", m_mixerGetDevCapsA), M("mixerGetLineInfoA", m_mixerGetLineInfoA),
    M("mixerGetLineControlsA", m_mixerGetLineControlsA), M("mixerGetControlDetailsA", m_mixerGetControlDetailsA),
    M("mixerSetControlDetails", m_mixerSetControlDetails), M("mciSendCommandA", m_mciSendCommandA),
    {NULL, NULL, NULL}
};
