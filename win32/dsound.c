/* DirectSound on SDL2 audio.
 *
 * Sound buffers keep their samples in guest memory (Lock returns guest
 * addresses, Unlock has nothing to copy); the SDL audio callback mixes every
 * playing secondary buffer (8/16-bit, mono/stereo, any rate; volume in 1/100 dB,
 * pan) into a 22050 Hz stereo stream. The primary buffer only accepts its calls.
 */
#include "w32.h"

#include <SDL.h>
#include <math.h>

#define OUT_RATE 22050
#define MAX_BUF 128
#define DS_OK 0u
#define DSERR_INVALIDPARAM 0x80070057u
#define DSERR_OUTOFMEMORY 0x8007000Eu
#define E_NOINTERFACE_ 0x80004002u

typedef struct {
    int used, primary;
    uint32_t obj;                /* guest COM object */
    uint32_t data, size;         /* guest sample memory */
    uint32_t refs;
    int channels, bits, rate, freq;
    int playing, looping;
    double pos;                  /* play cursor, in source frames (fractional) */
    int32_t volume, pan;         /* 1/100 dB */
    float gain_l, gain_r;
} dsbuf;

static dsbuf bufs[MAX_BUF];
static SDL_AudioDeviceID dev;

/* Host PCM voices (the sound track of the AVI movies), 16-bit interleaved. */
typedef struct { const int16_t *s; uint32_t frames; int ch, rate, on; double pos; } hostvoice;
static hostvoice hv[4];
static uint32_t ds_vtbl, dsb_vtbl;

static void lock_audio(void) { if (dev) SDL_LockAudioDevice(dev); }
static void unlock_audio(void) { if (dev) SDL_UnlockAudioDevice(dev); }

static void update_gain(dsbuf *b) {
    float g = b->volume <= -10000 ? 0.0f : powf(10.0f, (float)b->volume / 2000.0f);
    float l = 1.0f, r = 1.0f;
    if (b->pan < 0) r = b->pan <= -10000 ? 0.0f : powf(10.0f, (float)b->pan / 2000.0f);
    if (b->pan > 0) l = b->pan >= 10000 ? 0.0f : powf(10.0f, (float)-b->pan / 2000.0f);
    b->gain_l = g * l;
    b->gain_r = g * r;
}

static void mix(void *ud, Uint8 *stream, int len) {
    (void)ud;
    int16_t *out = (int16_t *)stream;
    int frames = len / 4;
    static int32_t acc[8192 * 2];
    if (frames > 8192) frames = 8192;
    memset(acc, 0, sizeof(int32_t) * 2 * (size_t)frames);
    for (int k = 0; k < MAX_BUF; k++) {
        dsbuf *b = &bufs[k];
        if (!b->used || b->primary || !b->playing || !b->size) continue;
        int bpf = b->channels * b->bits / 8;
        uint32_t nframes = b->size / (uint32_t)bpf;
        if (!nframes) continue;
        double step = (double)b->freq / OUT_RATE;
        const uint8_t *src = (const uint8_t *)rc_ptr(b->data);
        for (int i = 0; i < frames; i++) {
            uint32_t f = (uint32_t)b->pos;
            if (f >= nframes) {
                if (b->looping) { b->pos -= nframes; f = (uint32_t)b->pos; if (f >= nframes) { b->pos = 0; f = 0; } }
                else { b->playing = 0; b->pos = 0; break; }
            }
            int l, r;
            if (b->bits == 8) {
                l = ((int)src[f * (uint32_t)bpf] - 128) << 8;
                r = b->channels == 2 ? ((int)src[f * (uint32_t)bpf + 1] - 128) << 8 : l;
            } else {
                int16_t s0, s1;
                memcpy(&s0, src + f * (uint32_t)bpf, 2);
                s1 = s0;
                if (b->channels == 2) memcpy(&s1, src + f * (uint32_t)bpf + 2, 2);
                l = s0; r = s1;
            }
            acc[2 * i] += (int32_t)(l * b->gain_l);
            acc[2 * i + 1] += (int32_t)(r * b->gain_r);
            b->pos += step;
        }
    }
    for (int k = 0; k < 4; k++) {
        hostvoice *v = &hv[k];
        if (!v->on) continue;
        double step = (double)v->rate / OUT_RATE;
        for (int i = 0; i < frames; i++) {
            uint32_t f = (uint32_t)v->pos;
            if (f >= v->frames) { v->on = 0; break; }
            int l = v->s[f * (uint32_t)v->ch], r = v->ch == 2 ? v->s[f * 2 + 1] : l;
            acc[2 * i] += l;
            acc[2 * i + 1] += r;
            v->pos += step;
        }
    }
    for (int i = 0; i < 2 * frames; i++) {
        int32_t v = acc[i];
        out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    if (frames * 4 < len) memset(stream + frames * 4, 0, (size_t)(len - frames * 4));
    /* W32_PCMDUMP=file: raw 22050 Hz stereo s16 copy of the mix (checking audio in unattended runs). */
    static FILE *dump;
    static int dump_checked;
    if (!dump_checked) { dump_checked = 1; const char *p = getenv("W32_PCMDUMP"); if (p) dump = fopen(p, "wb"); }
    if (dump) { fwrite(stream, 1, (size_t)len, dump); fflush(dump); }
}

static dsbuf *B(uint32_t obj) {
    uint32_t k = rd32(obj + 8);
    return k < MAX_BUF && bufs[k].used && bufs[k].obj == obj ? &bufs[k] : NULL;
}

/* -- IDirectSoundBuffer ------------------------------------------------------ */
static void dsb_QueryInterface(CPU *c) { RET(3, E_NOINTERFACE_); }
static void dsb_AddRef(CPU *c) { dsbuf *b = B(ARG(0)); RET(1, b ? ++b->refs : 0); }
static void dsb_Release(CPU *c) {
    dsbuf *b = B(ARG(0));
    if (!b) RET(1, 0);
    uint32_t n = --b->refs;
    if (!n) {
        lock_audio();
        b->used = 0;
        unlock_audio();
        if (b->data) w32_heap_free(b->data);
        w32_heap_free(b->obj);
    }
    RET(1, n);
}
static void dsb_GetCaps(CPU *c) {
    dsbuf *b = B(ARG(0));
    uint32_t p = ARG(1);
    if (!b) RET(2, DSERR_INVALIDPARAM);
    wr32(p + 4, b->primary ? 0x1u : 0xE0u);   /* flags: PRIMARY / CTRLVOLUME|PAN|FREQUENCY */
    wr32(p + 8, b->size);
    wr32(p + 12, 0);
    wr32(p + 16, 0);
    RET(2, DS_OK);
}
static void dsb_GetCurrentPosition(CPU *c) {
    dsbuf *b = B(ARG(0));
    if (!b) RET(3, DSERR_INVALIDPARAM);
    lock_audio();
    uint32_t bpf = (uint32_t)(b->channels * b->bits / 8);
    uint32_t play = b->size ? ((uint32_t)b->pos * bpf) % b->size : 0;
    uint32_t ahead = (uint32_t)(b->freq / 50) * bpf;          /* ~20 ms */
    uint32_t write = b->size ? (play + ahead) % b->size : 0;
    unlock_audio();
    if (ARG(1)) wr32(ARG(1), play);
    if (ARG(2)) wr32(ARG(2), write);
    RET(3, DS_OK);
}
static void dsb_GetFormat(CPU *c) {
    dsbuf *b = B(ARG(0));
    uint32_t p = ARG(1);
    if (!b) RET(4, DSERR_INVALIDPARAM);
    if (p && ARG(2) >= 16) {
        wr16(p, 1); wr16(p + 2, (uint16_t)b->channels); wr32(p + 4, (uint32_t)b->rate);
        wr32(p + 8, (uint32_t)(b->rate * b->channels * b->bits / 8)); wr16(p + 12, (uint16_t)(b->channels * b->bits / 8));
        wr16(p + 14, (uint16_t)b->bits);
        if (ARG(2) >= 18) wr16(p + 16, 0);
    }
    if (ARG(3)) wr32(ARG(3), 18);
    RET(4, DS_OK);
}
static void dsb_GetVolume(CPU *c) { dsbuf *b = B(ARG(0)); if (b) wr32(ARG(1), (uint32_t)b->volume); RET(2, b ? DS_OK : DSERR_INVALIDPARAM); }
static void dsb_GetPan(CPU *c) { dsbuf *b = B(ARG(0)); if (b) wr32(ARG(1), (uint32_t)b->pan); RET(2, b ? DS_OK : DSERR_INVALIDPARAM); }
static void dsb_GetFrequency(CPU *c) { dsbuf *b = B(ARG(0)); if (b) wr32(ARG(1), (uint32_t)b->freq); RET(2, b ? DS_OK : DSERR_INVALIDPARAM); }
static void dsb_GetStatus(CPU *c) {
    dsbuf *b = B(ARG(0));
    if (!b) RET(2, DSERR_INVALIDPARAM);
    lock_audio();
    uint32_t s = (b->playing ? 1u : 0u) | (b->playing && b->looping ? 4u : 0u);
    unlock_audio();
    wr32(ARG(1), s);
    RET(2, DS_OK);
}
static void dsb_Initialize(CPU *c) { RET(3, DS_OK); }
static void dsb_Lock(CPU *c) {
    dsbuf *b = B(ARG(0));
    uint32_t off = ARG(1), n = ARG(2), p1 = ARG(3), n1 = ARG(4), p2 = ARG(5), n2 = ARG(6), flags = ARG(7);
    if (!b || !b->size) RET(8, DSERR_INVALIDPARAM);
    if (flags & 2) { off = 0; n = b->size; }                   /* DSBLOCK_ENTIREBUFFER */
    if (flags & 1) {                                           /* DSBLOCK_FROMWRITECURSOR */
        uint32_t bpf = (uint32_t)(b->channels * b->bits / 8);
        off = ((uint32_t)b->pos * bpf + (uint32_t)(b->freq / 50) * bpf) % b->size;
    }
    off %= b->size;
    if (n > b->size) n = b->size;
    uint32_t first = n <= b->size - off ? n : b->size - off;
    wr32(p1, b->data + off);
    wr32(n1, first);
    if (p2) wr32(p2, first < n ? b->data : 0);
    if (n2) wr32(n2, n - first);
    RET(8, DS_OK);
}
static void dsb_Play(CPU *c) {
    dsbuf *b = B(ARG(0));
    if (!b) RET(4, DSERR_INVALIDPARAM);
    lock_audio();
    b->playing = 1;
    b->looping = (ARG(3) & 1) != 0;                            /* DSBPLAY_LOOPING */
    unlock_audio();
    RET(4, DS_OK);
}
static void dsb_SetCurrentPosition(CPU *c) {
    dsbuf *b = B(ARG(0));
    if (!b) RET(2, DSERR_INVALIDPARAM);
    lock_audio();
    int bpf = b->channels * b->bits / 8;
    b->pos = bpf ? (double)(ARG(1) / (uint32_t)bpf) : 0;
    unlock_audio();
    RET(2, DS_OK);
}
static void dsb_SetFormat(CPU *c) { RET(2, DS_OK); }
static void dsb_SetVolume(CPU *c) {
    dsbuf *b = B(ARG(0));
    if (!b) RET(2, DSERR_INVALIDPARAM);
    lock_audio(); b->volume = (int32_t)ARG(1); update_gain(b); unlock_audio();
    RET(2, DS_OK);
}
static void dsb_SetPan(CPU *c) {
    dsbuf *b = B(ARG(0));
    if (!b) RET(2, DSERR_INVALIDPARAM);
    lock_audio(); b->pan = (int32_t)ARG(1); update_gain(b); unlock_audio();
    RET(2, DS_OK);
}
static void dsb_SetFrequency(CPU *c) {
    dsbuf *b = B(ARG(0));
    if (!b) RET(2, DSERR_INVALIDPARAM);
    lock_audio(); b->freq = ARG(1) ? (int)ARG(1) : b->rate; unlock_audio();
    RET(2, DS_OK);
}
static void dsb_Stop(CPU *c) {
    dsbuf *b = B(ARG(0));
    if (!b) RET(1, DSERR_INVALIDPARAM);
    lock_audio(); b->playing = 0; unlock_audio();
    RET(1, DS_OK);
}
static void dsb_Unlock(CPU *c) { RET(5, DS_OK); }
static void dsb_Restore(CPU *c) { RET(1, DS_OK); }

/* -- IDirectSound ---------------------------------------------------------- */
static void ds_QueryInterface(CPU *c) { RET(3, E_NOINTERFACE_); }
static void ds_AddRef(CPU *c) { uint32_t o = ARG(0); wr32(o + 4, rd32(o + 4) + 1); RET(1, rd32(o + 4)); }
static void ds_Release(CPU *c) { uint32_t o = ARG(0); uint32_t n = rd32(o + 4) - 1; wr32(o + 4, n); RET(1, n); }
static void ds_CreateSoundBuffer(CPU *c) {
    uint32_t desc = ARG(1), out = ARG(2);
    uint32_t flags = rd32(desc + 4), bytes = rd32(desc + 8), fmt = rd32(desc + 16);
    int k;
    lock_audio();
    for (k = 0; k < MAX_BUF && bufs[k].used; k++) {}
    if (k == MAX_BUF) { unlock_audio(); RET(4, DSERR_OUTOFMEMORY); }
    dsbuf *b = &bufs[k];
    memset(b, 0, sizeof *b);
    b->primary = (flags & 1) != 0;                             /* DSBCAPS_PRIMARYBUFFER */
    b->channels = 2; b->bits = 16; b->rate = OUT_RATE;
    if (fmt) {
        b->channels = rd16(fmt + 2);
        b->rate = (int)rd32(fmt + 4);
        b->bits = rd16(fmt + 14);
        if (b->channels < 1 || b->channels > 2) b->channels = 1;
        if (b->bits != 8 && b->bits != 16) b->bits = 16;
    }
    b->freq = b->rate;
    b->refs = 1;
    unlock_audio();
    if (!b->primary && bytes) {
        b->data = w32_heap_alloc(bytes, 1);
        if (b->bits == 8) memset(rc_ptr(b->data), 0x80, bytes);
        b->size = bytes;
    }
    uint32_t obj = w32_heap_alloc(16, 1);
    wr32(obj, dsb_vtbl);
    wr32(obj + 4, 1);
    wr32(obj + 8, (uint32_t)k);
    lock_audio();
    b->obj = obj;
    update_gain(b);
    b->used = 1;
    unlock_audio();
    wr32(out, obj);
    W32_LOG("CreateSoundBuffer %s %u bytes %d ch %d bit %d Hz", b->primary ? "primary" : "secondary", bytes, b->channels, b->bits, b->rate);
    RET(4, DS_OK);
}
static void ds_GetCaps(CPU *c) {
    uint32_t p = ARG(1);
    uint32_t size = rd32(p);
    memset(G(p + 4), 0, size > 4 ? size - 4 : 0);
    wr32(p + 4, 0x0F0Fu);           /* primary/secondary 8/16 bit mono/stereo */
    RET(2, DS_OK);
}
static void ds_DuplicateSoundBuffer(CPU *c) {
    dsbuf *s = B(ARG(1));
    if (!s) RET(3, DSERR_INVALIDPARAM);
    int k;
    lock_audio();
    for (k = 0; k < MAX_BUF && bufs[k].used; k++) {}
    if (k == MAX_BUF) { unlock_audio(); RET(3, DSERR_OUTOFMEMORY); }
    dsbuf *b = &bufs[k];
    *b = *s;
    b->playing = 0; b->pos = 0; b->refs = 1;
    b->used = 0;
    unlock_audio();
    /* the duplicate shares the samples: give it its own copy so Release stays simple */
    b->data = w32_heap_alloc(s->size, 0);
    memcpy(rc_ptr(b->data), rc_ptr(s->data), s->size);
    uint32_t obj = w32_heap_alloc(16, 1);
    wr32(obj, dsb_vtbl); wr32(obj + 4, 1); wr32(obj + 8, (uint32_t)k);
    lock_audio();
    b->obj = obj;
    b->used = 1;
    unlock_audio();
    wr32(ARG(2), obj);
    RET(3, DS_OK);
}
static void ds_SetCooperativeLevel(CPU *c) { RET(3, DS_OK); }
static void ds_Compact(CPU *c) { RET(1, DS_OK); }
static void ds_GetSpeakerConfig(CPU *c) { wr32(ARG(1), 4 /* DSSPEAKER_STEREO */); RET(2, DS_OK); }
static void ds_SetSpeakerConfig(CPU *c) { RET(2, DS_OK); }
static void ds_Initialize(CPU *c) { RET(2, DS_OK); }

static uint32_t vtable(const rc_fn *f, int n, const char *prefix) {
    uint32_t vt = w32_heap_alloc(4u * (uint32_t)n, 1);
    for (int i = 0; i < n; i++) wr32(vt + 4u * (uint32_t)i, rc_host_thunk(f[i], prefix));
    return vt;
}

static int open_audio(void) {
    if (dev) return 1;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) { W32_LOG("no audio: %s", SDL_GetError()); return 0; }
    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof want);
    want.freq = OUT_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    want.callback = mix;
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!dev) { W32_LOG("no audio device: %s", SDL_GetError()); return 0; }
    SDL_PauseAudioDevice(dev, 0);
    return 1;
}

int w32_audio_play_host(const int16_t *s, uint32_t frames, int ch, int rate) {
    if (!open_audio()) return -1;
    lock_audio();
    int k;
    for (k = 0; k < 4 && hv[k].on; k++) {}
    if (k < 4) { hv[k].s = s; hv[k].frames = frames; hv[k].ch = ch; hv[k].rate = rate; hv[k].pos = 0; hv[k].on = 1; }
    unlock_audio();
    return k < 4 ? k : -1;
}
void w32_audio_stop_host(int k) { if (k < 0 || k >= 4) return; lock_audio(); hv[k].on = 0; unlock_audio(); }
/* Seconds played, or -1 when the voice has ended. */
double w32_audio_host_seconds(int k) {
    if (k < 0 || k >= 4) return -1;
    lock_audio();
    double t = hv[k].on ? hv[k].pos / hv[k].rate : -1;
    unlock_audio();
    return t;
}

uint32_t w32_dsound_create(uint32_t *obj) {
    if (!ds_vtbl) {
        if (!open_audio()) { *obj = 0; return 0x88780078u; }
        static const rc_fn dsf[11] = {ds_QueryInterface, ds_AddRef, ds_Release, ds_CreateSoundBuffer, ds_GetCaps,
                                      ds_DuplicateSoundBuffer, ds_SetCooperativeLevel, ds_Compact,
                                      ds_GetSpeakerConfig, ds_SetSpeakerConfig, ds_Initialize};
        static const rc_fn bf[21] = {dsb_QueryInterface, dsb_AddRef, dsb_Release, dsb_GetCaps, dsb_GetCurrentPosition,
                                     dsb_GetFormat, dsb_GetVolume, dsb_GetPan, dsb_GetFrequency, dsb_GetStatus,
                                     dsb_Initialize, dsb_Lock, dsb_Play, dsb_SetCurrentPosition, dsb_SetFormat,
                                     dsb_SetVolume, dsb_SetPan, dsb_SetFrequency, dsb_Stop, dsb_Unlock, dsb_Restore};
        ds_vtbl = vtable(dsf, 11, "IDirectSound");
        dsb_vtbl = vtable(bf, 21, "IDirectSoundBuffer");
    }
    uint32_t o = w32_heap_alloc(16, 1);
    wr32(o, ds_vtbl);
    wr32(o + 4, 1);
    *obj = o;
    return DS_OK;
}
