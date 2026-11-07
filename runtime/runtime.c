/* rapture-recomp runtime: guest memory, dispatch, imports, faults, x87 helpers. */
#include "recomp.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

uint8_t *rc_mem;
int rc_trace;   /* 1: log indirect calls; 2: also log every import/host call */

/* Guest addresses of the import thunks: one 16-byte slot per import. */
#define RC_THUNK_BASE 0xFFF00000u
/* Dynamic host thunks (COM methods, GetProcAddress results): 16 bytes each. */
#define RC_HOST_THUNK_BASE 0xFFE00000u
#define RC_HOST_THUNK_MAX  0x10000u
/* Guest address that ends rc_invoke (a return here stops the run). */
#define RC_EXIT_ADDR  0xFFFFFFF0u

int rc_init(void) {
    rc_mem = (uint8_t *)VirtualAlloc(NULL, (SIZE_T)1 << 32, MEM_RESERVE, PAGE_NOACCESS);
    if (!rc_mem) return -1;
    return rc_commit(RC_HOST_THUNK_BASE, 0x00200000u);
}

int rc_commit(uint32_t addr, uint32_t size) {
    return VirtualAlloc(rc_mem + addr, size, MEM_COMMIT, PAGE_READWRITE) ? 0 : -1;
}

int rc_load_image(const char *path, uint32_t base, uint32_t size) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "rc: cannot open %s\n", path); return -1; }
    if (rc_commit(base, size)) { fclose(f); return -1; }
    size_t n = fread(rc_mem + base, 1, size, f);
    fclose(f);
    if (n != size) { fprintf(stderr, "rc: short image %s\n", path); return -1; }
    return 0;
}

uint32_t rc_alloc_stack(uint32_t size) {
    static uint32_t next = 0x00100000u;   /* stacks live in 0x00100000..0x003FFFFF */
    uint32_t base = next;
    next += size;
    if (next > 0x00400000u || rc_commit(base, size)) return 0;
    return base + size - 16;
}

rc_fn rc_lookup(uint32_t target) {
    uint32_t lo = 0, hi = rc_func_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (rc_func_table[mid].addr < target) lo = mid + 1;
        else hi = mid;
    }
    if (lo < rc_func_count && rc_func_table[lo].addr == target) return rc_func_table[lo].fn;
    return NULL;
}

/* -- imports and host thunks ----------------------------------------- */
static rc_fn host_thunk_fn[RC_HOST_THUNK_MAX];
static const char *host_thunk_name[RC_HOST_THUNK_MAX];
static uint32_t host_thunk_count;
static rc_fn *import_fn;          /* resolved lazily, one per rc_import_table entry */

uint32_t rc_host_thunk(rc_fn fn, const char *name) {
    for (uint32_t k = 0; k < host_thunk_count; k++)
        if (host_thunk_fn[k] == fn) return RC_HOST_THUNK_BASE + 16u * k;
    if (host_thunk_count == RC_HOST_THUNK_MAX) { fprintf(stderr, "rc: out of host thunks\n"); abort(); }
    host_thunk_fn[host_thunk_count] = fn;
    host_thunk_name[host_thunk_count] = name;
    return RC_HOST_THUNK_BASE + 16u * host_thunk_count++;
}

int rc_ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return *a == *b;
}

rc_fn rc_hle_find(const char *dll, const char *name) {
    for (const rc_hle_entry *const *t = rc_hle_tables; *t; t++)
        for (const rc_hle_entry *e = *t; e->name; e++)
            if (rc_ieq(e->dll, dll) && strcmp(e->name, name) == 0) return e->fn;
    return NULL;
}

void rc_call_import(CPU *c, uint32_t idx) {
    if (!import_fn) import_fn = (rc_fn *)calloc(rc_import_count + 1, sizeof *import_fn);
    rc_fn fn = import_fn[idx];
    if (!fn) {
        const rc_import *im = &rc_import_table[idx];
        fn = im->name ? rc_hle_find(im->dll, im->name) : NULL;
        if (!fn) {
            fprintf(stderr, "rc: import %s!%s (ord %u) not implemented\n",
                    im->dll, im->name ? im->name : "?", im->ordinal);
            rc_fault(c, rd32(c->esp), "unimplemented import (address = return address)");
        }
        import_fn[idx] = fn;
    }
    if (rc_trace > 1)
        fprintf(stderr, "rc: %s!%s from %08x\n", rc_import_table[idx].dll, rc_import_table[idx].name, rd32(c->esp));
    fn(c);
}

void rc_call(CPU *c, uint32_t target) {
    if (rc_trace) fprintf(stderr, "rc: call %08x esp=%08x\n", target, c->esp);
    rc_fn fn = rc_lookup(target);
    if (fn) { fn(c); return; }
    if (target >= RC_THUNK_BASE && target < RC_THUNK_BASE + rc_import_count * 16u) {
        rc_call_import(c, (target - RC_THUNK_BASE) / 16u);
        return;
    }
    if (target >= RC_HOST_THUNK_BASE && target < RC_HOST_THUNK_BASE + host_thunk_count * 16u) {
        uint32_t k = (target - RC_HOST_THUNK_BASE) / 16u;
        if (rc_trace > 1) fprintf(stderr, "rc: host %s\n", host_thunk_name[k] ? host_thunk_name[k] : "?");
        host_thunk_fn[k](c);
        return;
    }
    rc_fault(c, target, "indirect call to an unknown address");
}

uint32_t rc_invoke(CPU *c, uint32_t fn, int argc, const uint32_t *argv) {
    uint32_t sp0 = c->esp;
    for (int k = argc - 1; k >= 0; k--) { c->esp -= 4; wr32(c->esp, argv[k]); }
    c->esp -= 4; wr32(c->esp, RC_EXIT_ADDR);
    rc_call(c, fn);
    c->esp = sp0;   /* works for cdecl (caller pops) and stdcall (callee popped) */
    return c->eax;
}

void rc_fault(CPU *c, uint32_t addr, const char *what) {
    fprintf(stderr, "rc: fault at %08x: %s (eax=%08x ecx=%08x edx=%08x ebx=%08x esi=%08x edi=%08x ebp=%08x esp=%08x)\n",
            addr, what, c->eax, c->ecx, c->edx, c->ebx, c->esi, c->edi, c->ebp, c->esp);
    fprintf(stderr, "rc: stack:");
    for (int k = 0; k < 16; k++) fprintf(stderr, " %08x", rd32(c->esp + 4u * (uint32_t)k));
    fprintf(stderr, "\n");
    fflush(stderr);
    abort();
}

void rc_unimpl(CPU *c, uint32_t addr, const char *insn) {
    fprintf(stderr, "rc: unimplemented instruction at %08x: %s\n", addr, insn);
    rc_fault(c, addr, "unimplemented instruction");
}

/* -- x87 --------------------------------------------------------------- */
double rc_fround(CPU *c, double v) {
    switch ((c->cw >> 10) & 3u) {
    case 0: return nearbyint(v);    /* host default rounding = to nearest even */
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}

int64_t rc_fist(CPU *c, double v, int bits) {
    double r = rc_fround(c, v);
    double lim = ldexp(1.0, bits - 1);
    if (!(r >= -lim && r < lim)) return bits == 64 ? INT64_MIN : -((int64_t)1 << (bits - 1));  /* integer indefinite */
    return (int64_t)r;
}

void rc_fxam(CPU *c) {
    double v = ST(0);
    uint16_t cc;                      /* C3 C2 C0 */
    if (v != v) cc = 0x0100u;         /* NaN */
    else if (isinf(v)) cc = 0x0500u;  /* infinity */
    else if (v == 0) cc = 0x4000u;    /* zero */
    else cc = 0x0400u;                /* normal */
    if (signbit(v)) cc |= 0x0200u;    /* C1 = sign */
    c->sw = (uint16_t)((c->sw & ~0x4700u) | cc);
}

double rdf80(uint32_t a) {
    uint64_t mant = rd64(a);
    uint16_t se = rd16(a + 8);
    int neg = se >> 15, e = se & 0x7FFF;
    double v;
    if (e == 0x7FFF) v = (mant << 1) ? NAN : INFINITY;
    else if (e == 0 && mant == 0) v = 0.0;
    else v = ldexp((double)mant, e - 16383 - 63);
    return neg ? -v : v;
}

void wrf80(uint32_t a, double v) {
    uint16_t se = signbit(v) ? 0x8000u : 0;
    uint64_t mant = 0;
    if (isnan(v)) { se |= 0x7FFF; mant = 0xC000000000000000ull; }
    else if (isinf(v)) { se |= 0x7FFF; mant = 0x8000000000000000ull; }
    else if (v != 0) {
        int e;
        double m = frexp(fabs(v), &e);           /* m in [0.5, 1) */
        mant = (uint64_t)ldexp(m, 64);
        se |= (uint16_t)(e - 1 + 16383);
    }
    wr64(a, mant);
    wr16(a + 8, se);
}
