/* rapture-recomp runtime: guest memory, dispatch, faults. */
#include "recomp.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

uint8_t *rc_mem;

/* Guest addresses of the import thunks: one 16-byte slot per import. */
#define RC_THUNK_BASE 0xFFF00000u
/* Guest address that ends rc_invoke (a return here stops the run). */
#define RC_EXIT_ADDR  0xFFFFFFF0u

int rc_init(void) {
    rc_mem = (uint8_t *)VirtualAlloc(NULL, (SIZE_T)1 << 32, MEM_RESERVE, PAGE_NOACCESS);
    if (!rc_mem) return -1;
    return rc_commit(RC_THUNK_BASE, 0x00100000u);
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
    static uint32_t next = 0x00100000u;   /* below the exe's 0x400000 */
    uint32_t base = next;
    next += size;
    if (rc_commit(base, size)) return 0;
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

int rc_trace;   /* set by the host (e.g. from an environment variable) to log indirect calls */

void rc_call(CPU *c, uint32_t target) {
    if (rc_trace) fprintf(stderr, "rc: call %08x esp=%08x\n", target, c->esp);
    rc_fn fn = rc_lookup(target);
    if (fn) { fn(c); return; }
    if (target >= RC_THUNK_BASE && target < RC_THUNK_BASE + rc_import_count * 16u) {
        const rc_import *im = &rc_import_table[(target - RC_THUNK_BASE) / 16u];
        fprintf(stderr, "rc: import %s!%s (ord %u) not implemented\n",
                im->dll, im->name ? im->name : "?", im->ordinal);
        rc_fault(c, target, "unimplemented import");
    }
    rc_fault(c, target, "indirect call to an unknown address");
}

uint32_t rc_invoke(CPU *c, uint32_t fn, int argc, const uint32_t *argv) {
    for (int k = argc - 1; k >= 0; k--) { c->esp -= 4; wr32(c->esp, argv[k]); }
    uint32_t sp = c->esp;
    c->esp -= 4; wr32(c->esp, RC_EXIT_ADDR);
    rc_call(c, fn);
    (void)sp;   /* cdecl: caller pops; stdcall: callee already popped */
    c->esp = sp + 4u * (uint32_t)argc;
    return c->eax;
}

void rc_fault(CPU *c, uint32_t addr, const char *what) {
    fprintf(stderr, "rc: fault at %08x: %s (eax=%08x ecx=%08x edx=%08x esp=%08x)\n",
            addr, what, c->eax, c->ecx, c->edx, c->esp);
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
    if (!(r >= -lim && r < lim)) return bits == 64 ? INT64_MIN : -((int64_t)1 << (bits - 1));  /* 0x80..0 */
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
