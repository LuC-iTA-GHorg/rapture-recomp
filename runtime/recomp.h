/* rapture-recomp runtime: guest CPU state and guest memory.
 *
 * The guest is a flat 32-bit address space reserved inside the 64-bit host
 * process (rc_mem + guest_address). Generated code includes only this header.
 */
#ifndef RAPTURE_RECOMP_H
#define RAPTURE_RECOMP_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CPU {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t df;        /* direction flag */
    uint32_t fs_base;   /* guest address of the TEB (fs:[0] = SEH chain) */
} CPU;

typedef void (*rc_fn)(CPU *c);

extern uint8_t *rc_mem;

static inline uint8_t  rd8 (uint32_t a) { return rc_mem[a]; }
static inline uint16_t rd16(uint32_t a) { uint16_t v; memcpy(&v, rc_mem + a, 2); return v; }
static inline uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, rc_mem + a, 4); return v; }
static inline void wr8 (uint32_t a, uint8_t v)  { rc_mem[a] = v; }
static inline void wr16(uint32_t a, uint16_t v) { memcpy(rc_mem + a, &v, 2); }
static inline void wr32(uint32_t a, uint32_t v) { memcpy(rc_mem + a, &v, 4); }
static inline uint32_t rc_parity(uint32_t r) { return !__builtin_parity(r & 0xFFu); }

/* Host pointer <-> guest address. */
static inline void *rc_ptr(uint32_t a) { return rc_mem + a; }

/* Indirect call / tail call through a guest address (function, import thunk). */
void rc_call(CPU *c, uint32_t target);
/* Look up a recompiled function; NULL if the address is not a known entry. */
rc_fn rc_lookup(uint32_t target);

_Noreturn void rc_fault(CPU *c, uint32_t addr, const char *what);
_Noreturn void rc_unimpl(CPU *c, uint32_t addr, const char *insn);

/* -- memory / process setup (runtime.c) ------------------------------ */
int  rc_init(void);                              /* reserve the 4 GiB guest space */
int  rc_commit(uint32_t addr, uint32_t size);    /* commit a guest range */
int  rc_load_image(const char *path, uint32_t base, uint32_t size);
uint32_t rc_alloc_stack(uint32_t size);          /* returns initial esp */

/* Call a guest function with cdecl/stdcall arguments; returns eax. */
uint32_t rc_invoke(CPU *c, uint32_t fn, int argc, const uint32_t *argv);

/* -- generated tables (gen/dispatch.c) ------------------------------- */
typedef struct { uint32_t addr; rc_fn fn; } rc_func_entry;
extern const rc_func_entry rc_func_table[];
extern const uint32_t rc_func_count;

typedef struct { uint32_t slot; const char *dll; const char *name; uint32_t ordinal; uint32_t target; } rc_import;
extern const rc_import rc_import_table[];
extern const uint32_t rc_import_count;

typedef struct { const char *name; const char *image; uint32_t base, size; } rc_module;
extern const rc_module rc_module_table[];
extern const uint32_t rc_module_count;

#ifdef __cplusplus
}
#endif
#endif
