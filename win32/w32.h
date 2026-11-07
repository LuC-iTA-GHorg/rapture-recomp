/* Win32 layer: shared helpers.
 *
 * Every function implements one 32-bit Win32 API for recompiled guest code.
 * Conventions: arguments are read with ARG(n) from the guest stack, results are
 * returned with RET(nargs, value) (stdcall: pops the arguments) or RETC(value)
 * (cdecl/varargs). Guest pointers are 32-bit guest addresses; G(p) turns one
 * into a host pointer.
 *
 * Strategy (docs/DESIGN.md §5): APIs that only compute or work on memory are
 * forwarded to the host Windows (strings, locale, GDI drawing into DIB sections
 * whose pixels live in guest memory, mmio, midiOut); APIs tied to 32-bit
 * pointers, the process layout or the presentation are emulated (heap and
 * virtual memory, modules and resources, the window and its message queue on
 * SDL, DirectSound, the registry kept in an .ini file).
 */
#ifndef RAPTURE_W32_H
#define RAPTURE_W32_H

#include "recomp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RETC(v) do { c->eax = (uint32_t)(v); c->esp += 4u; return; } while (0)
#define G(p) ((void *)rc_ptr((uint32_t)(p)))
#define GS(p) ((const char *)((p) ? rc_ptr((uint32_t)(p)) : NULL))   /* guest string or NULL */

#define W32_LOG(...) do { if (w32_log_level) { fprintf(stderr, "w32: " __VA_ARGS__); fputc('\n', stderr); } } while (0)
#define W32_STUB(name) W32_LOG("stub %s", name)
extern int w32_log_level;

/* -- process (kernel32.c) --------------------------------------------- */
uint32_t w32_heap_alloc(uint32_t size, int zero);  /* guest heap, 16-byte aligned; 0 on failure */
void w32_heap_free(uint32_t p);
uint32_t w32_heap_size(uint32_t p);
uint32_t w32_heap_realloc(uint32_t p, uint32_t size, int zero);
uint32_t w32_guest_strdup(const char *s);          /* copy a host string into the guest heap */
void w32_set_last_error(uint32_t e);

/* Thread state: every host thread that runs guest code has one CPU. */
CPU *w32_cpu(void);
/* Create the main thread's CPU, stack and TEB. */
CPU *w32_main_cpu(void);

/* Guest thread slots (threads.c): TEB + guest stack per thread, reused after the
   thread ends. Slot 0 is the main thread; acquire returns -1 when all are busy. */
#define W32_TEB_AREA 0x7E000000u
#define W32_THREAD_SLOTS 56
#define W32_THREAD_STACK 0x40000u
void w32_slots_init(void);
uint32_t w32_slot_teb(int slot);
int w32_slot_acquire(uint32_t *teb, uint32_t *stack_top);
void w32_slot_release(int slot);
int w32_slot_of_thread(uint32_t tid);              /* slot of a running guest thread, -1 if none */
int w32_slots_in_use(void);

/* -- modules and resources (module.c) ---------------------------------- */
int w32_load_modules(const char *gen_dir);         /* map images, call DllMain of the static DLLs */
uint32_t w32_module_handle(const char *name);      /* base of a guest module, 0 if none */
int w32_module_index(uint32_t hmod);               /* -1 if not a guest module */
uint32_t w32_module_of_address(uint32_t addr);     /* base of the module containing addr */
uint32_t w32_get_proc(uint32_t hmod, const char *name, uint32_t ordinal);
/* Resource lookup in a guest image: returns the guest address of the data and its size. */
uint32_t w32_find_resource(uint32_t hmod, uint32_t type, uint32_t name, uint32_t *size);
uint32_t w32_exe_base(void);
const char *w32_game_dir(void);

/* -- the window and its messages (user32.c / window.cpp) ---------------- */
void w32_pump_host_events(void);                   /* SDL events -> guest messages */
void w32_present(void);                            /* show the screen surface */
uint32_t *w32_screen_pixels(void);                 /* 640x480 ARGB host framebuffer */
void w32_post_message(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);

/* -- host side of the window (window.cpp, uses rapture-engine's platform) -- */
typedef struct { int type, x, y, button, key, mod, text; } w32_host_event;
enum { W32_EV_NONE, W32_EV_QUIT, W32_EV_MOUSEMOVE, W32_EV_MOUSEDOWN, W32_EV_MOUSEUP,
       W32_EV_KEYDOWN, W32_EV_KEYUP, W32_EV_TEXT };
int w32_host_window_open(const char *title);
int w32_host_poll(w32_host_event *ev);
void w32_host_present(const uint32_t *argb);       /* 640x480 */
void w32_host_show_cursor(int show);
uint32_t w32_host_ticks(void);

#ifdef __cplusplus
}
#endif
#endif
