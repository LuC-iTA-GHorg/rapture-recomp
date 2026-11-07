/* rapture-recomp runtime: guest CPU state and guest memory.
 *
 * The guest is a flat 32-bit address space reserved inside the 64-bit host
 * process (rc_mem + guest_address). Generated code includes only this header.
 */
#ifndef RAPTURE_RECOMP_H
#define RAPTURE_RECOMP_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
#define RC_NORETURN [[noreturn]]
extern "C" {
#else
#define RC_NORETURN _Noreturn
#endif

typedef struct CPU {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t df;        /* direction flag */
    uint32_t fs_base;   /* guest address of the TEB (fs:[0] = SEH chain) */
    /* x87: 8 registers as 80-bit long double (exact on x86-64 GCC),
       st(i) = st[(top + i) & 7]. sw holds the condition codes C0..C3 (bits 8,
       9, 10, 14); TOP is merged in on fnstsw. cw: precision control in bits
       8-9, rounding control in bits 10-11. Exception flags are not modelled. */
    long double st[8];
    uint32_t top;
    uint16_t sw, cw;
} CPU;

typedef void (*rc_fn)(CPU *c);

/* Optional call trace (compile the generated code with -DRC_CALLTRACE): a
   per-thread ring of the last guest function entries, printed by rc_fault. */
#define RC_RING 64
extern __thread uint32_t rc_ring[RC_RING];
extern __thread uint32_t rc_ring_pos;
#ifdef RC_CALLTRACE
/* RC_BREAK=addr[,addr...] (hex): print the guest call stack whenever one of these functions is entered. */
extern uint32_t rc_break[8];
void rc_break_hit(struct CPU *c, uint32_t a);
#define RC_ENTER(a) do { rc_ring[rc_ring_pos++ & (RC_RING - 1)] = (a); \
    if (rc_break[0] && (rc_break[0] == (a) || rc_break[1] == (a) || rc_break[2] == (a) || rc_break[3] == (a))) rc_break_hit(c, (a)); } while (0)
#else
#define RC_ENTER(a) ((void)0)
#endif
typedef long double f80;

extern uint8_t *rc_mem;

static inline uint8_t  rd8 (uint32_t a) { return rc_mem[a]; }
static inline uint16_t rd16(uint32_t a) { uint16_t v; memcpy(&v, rc_mem + a, 2); return v; }
static inline uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, rc_mem + a, 4); return v; }
static inline void wr8 (uint32_t a, uint8_t v)  { rc_mem[a] = v; }
static inline void wr16(uint32_t a, uint16_t v) { memcpy(rc_mem + a, &v, 2); }
static inline void wr32(uint32_t a, uint32_t v) { memcpy(rc_mem + a, &v, 4); }
static inline uint32_t rc_parity(uint32_t r) { return !__builtin_parity(r & 0xFFu); }

static inline float  rdf32(uint32_t a) { float v;  memcpy(&v, rc_mem + a, 4); return v; }
static inline double rdf64(uint32_t a) { double v; memcpy(&v, rc_mem + a, 8); return v; }
static inline void wrf32(uint32_t a, float v)  { memcpy(rc_mem + a, &v, 4); }
static inline void wrf64(uint32_t a, double v) { memcpy(rc_mem + a, &v, 8); }
static inline uint64_t rd64(uint32_t a) { uint64_t v; memcpy(&v, rc_mem + a, 8); return v; }
static inline void wr64(uint32_t a, uint64_t v) { memcpy(rc_mem + a, &v, 8); }

/* -- x87 helpers ------------------------------------------------------ */
#define ST(i) (c->st[(c->top + (i)) & 7u])
static inline void fpush(CPU *c, f80 v) { c->top = (c->top - 1u) & 7u; c->st[c->top] = v; }
static inline void fpop(CPU *c) { c->top = (c->top + 1u) & 7u; }
/* Round an arithmetic result to the precision selected by cw (24/53/64 bits).
   The host computes in long double with round-to-nearest; the double rounding
   this implies for directed rounding modes is ignored. */
static inline f80 rc_fprec(const CPU *c, f80 v) {
    switch ((c->cw >> 8) & 3u) {
    case 0: return (f80)(float)v;
    case 2: return (f80)(double)v;
    default: return v;
    }
}
#define FPREC(x) rc_fprec(c, (x))
/* fcom: C3 C2 C0 = 000 (>), 001 (<), 100 (=), 111 (unordered). */
static inline void fcmp(CPU *c, f80 a, f80 b) {
    uint16_t cc;
    if (a != a || b != b) cc = 0x4500u;
    else if (a < b) cc = 0x0100u;
    else if (a == b) cc = 0x4000u;
    else cc = 0;
    c->sw = (uint16_t)((c->sw & ~0x4700u) | cc);
}
static inline uint16_t fstsw(CPU *c) { return (uint16_t)((c->sw & ~0x3800u) | ((c->top & 7u) << 11)); }
static inline f80 rdf80(uint32_t a) { f80 v = 0; memcpy(&v, rc_mem + a, 10); return v; }
static inline void wrf80(uint32_t a, f80 v) { memcpy(rc_mem + a, &v, 10); }
f80 rc_fround(CPU *c, f80 v);                  /* round to integer with cw's rounding control */
void rc_fxam(CPU *c);
int64_t rc_fist(CPU *c, f80 v, int bits);      /* integer-indefinite on overflow */

/* Host pointer <-> guest address. */
static inline void *rc_ptr(uint32_t a) { return rc_mem + a; }

/* Indirect call / tail call through a guest address (function, import thunk). */
void rc_call(CPU *c, uint32_t target);
/* Call system import number idx (index into rc_import_table). */
void rc_call_import(CPU *c, uint32_t idx);
extern int rc_trace;
/* Look up an HLE function by DLL and symbol name (case-insensitive DLL). */
rc_fn rc_hle_find(const char *dll, const char *name);
int rc_ieq(const char *a, const char *b);   /* ASCII case-insensitive equality */
/* Look up a recompiled function; NULL if the address is not a known entry. */
rc_fn rc_lookup(uint32_t target);

RC_NORETURN void rc_fault(CPU *c, uint32_t addr, const char *what);
RC_NORETURN void rc_unimpl(CPU *c, uint32_t addr, const char *insn);

/* -- memory / process setup (runtime.c) ------------------------------ */
int  rc_init(void);                              /* reserve the 4 GiB guest space */
void rc_install_crash_handler(CPU *(*current_cpu)(void));
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

typedef struct { const char *name; const char *image; uint32_t base, size, entry; } rc_module;
typedef struct { uint32_t module; const char *name; uint32_t ordinal; uint32_t addr; } rc_export;
extern const rc_export rc_export_table[];
extern const uint32_t rc_export_count;

/* -- host implementations of system imports (HLE) ---------------------
 * An HLE function behaves like a guest function: arguments are on the guest
 * stack above the return address, the result goes in eax (edx:eax), and a
 * stdcall function pops its arguments together with the return address. */
typedef struct { const char *dll; const char *name; rc_fn fn; } rc_hle_entry;
/* NULL-terminated list of NULL-terminated tables, provided by the HLE layer. */
extern const rc_hle_entry *const rc_hle_tables[];
#define ARG(n) rd32(c->esp + 4u + 4u * (uint32_t)(n))
#define RET(nargs, v) do { c->eax = (uint32_t)(v); c->esp += 4u + 4u * (uint32_t)(nargs); return; } while (0)
/* Allocate a guest address that, when called, runs host function fn (COM methods,
   GetProcAddress results, callbacks handed to guest code). */
uint32_t rc_host_thunk(rc_fn fn, const char *name);
extern const rc_module rc_module_table[];
extern const uint32_t rc_module_count;

#ifdef __cplusplus
}
#endif
#endif
