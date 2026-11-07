/* rapture-recomp runtime: guest memory, dispatch, imports, faults, x87 helpers. */
#include "recomp.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

uint8_t *rc_mem;
__thread uint32_t rc_ring[RC_RING];
__thread uint32_t rc_ring_pos;
int rc_trace;   /* 1: log indirect calls; 2: also log every import/host call */

/* Guest addresses of the import thunks: one 16-byte slot per import. */
#define RC_THUNK_BASE 0xFFF00000u
/* Dynamic host thunks (COM methods, GetProcAddress results): 16 bytes each. */
#define RC_HOST_THUNK_BASE 0xFFE00000u
#define RC_HOST_THUNK_MAX  0x10000u
/* Guest address that ends rc_invoke (a return here stops the run). */
#define RC_EXIT_ADDR  0xFFFFFFF0u

/* The guest space is one pagefile-backed section of 4 GiB, reserved and mapped
 * in full, committed on demand. Being a section, parts of it can also be handed
 * to Windows as the pixel memory of a DIB section (CreateDIBSection with
 * hSection + offset = guest address), so GDI draws straight into guest memory. */
void *rc_mem_section;

int rc_init(void) {
    HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE | SEC_RESERVE, 1, 0, NULL);
    if (!h) return -1;
    rc_mem = (uint8_t *)MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, (SIZE_T)1 << 32);
    if (!rc_mem) return -1;
    rc_mem_section = h;
    extern void rc_init_debug(void);
    rc_init_debug();
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

uint32_t rc_break[8];

static void print_chain(CPU *c) {
    fprintf(stderr, "rc: ebp chain:");
    uint32_t bp = c->ebp;
    for (int k = 0; k < 24 && bp >= 0x00100000u && bp < 0x00400000u && (bp & 3) == 0; k++) {
        fprintf(stderr, " %08x", rd32(bp + 4));
        uint32_t nb = rd32(bp);
        if (nb <= bp) break;
        bp = nb;
    }
    fprintf(stderr, "\n");
}

void rc_break_hit(CPU *c, uint32_t a) {
    fprintf(stderr, "rc: break %08x (caller %08x, ecx=%08x, args %08x %08x %08x)\n", a, rd32(c->esp), c->ecx,
            rd32(c->esp + 4), rd32(c->esp + 8), rd32(c->esp + 12));
    print_chain(c);
}

void rc_init_debug(void) {
    const char *b = getenv("RC_BREAK");
    for (int k = 0; b && *b && k < 4; k++) {
        char *e;
        rc_break[k] = (uint32_t)strtoul(b, &e, 16);
        b = *e == ',' ? e + 1 : e;
    }
}

void rc_fault(CPU *c, uint32_t addr, const char *what) {
    fprintf(stderr, "rc: fault at %08x: %s (eax=%08x ecx=%08x edx=%08x ebx=%08x esi=%08x edi=%08x ebp=%08x esp=%08x)\n",
            addr, what, c->eax, c->ecx, c->edx, c->ebx, c->esi, c->edi, c->ebp, c->esp);
    fprintf(stderr, "rc: stack:");
    for (int k = 0; k < 16; k++) fprintf(stderr, " %08x", rd32(c->esp + 4u * (uint32_t)k));
    fprintf(stderr, "\n");
    print_chain(c);   /* the games keep frame pointers: guest return addresses, innermost first */
    if (rc_ring_pos) {
        fprintf(stderr, "rc: last function entries (oldest first):");
        uint32_t n = rc_ring_pos < RC_RING ? rc_ring_pos : RC_RING;
        for (uint32_t i = rc_ring_pos - n; i != rc_ring_pos; i++) fprintf(stderr, " %08x", rc_ring[i & (RC_RING - 1)]);
        fprintf(stderr, "\n");
    }
    /* RC_DUMP=addr:len[,addr:len...] (hex): dump guest memory on a fault (debugging). */
    const char *d = getenv("RC_DUMP");
    while (d && *d) {
        char *end;
        uint32_t a = (uint32_t)strtoul(d, &end, 16), n = 64;
        if (*end == ':') n = (uint32_t)strtoul(end + 1, &end, 16);
        for (uint32_t i = 0; i < n; i += 16) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(rc_mem + a + i, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) break;
            fprintf(stderr, "rc: %08x:", a + i);
            for (uint32_t j = 0; j < 16; j += 4) fprintf(stderr, " %08x", rd32(a + i + j));
            fprintf(stderr, "\n");
        }
        d = *end == ',' ? end + 1 : end;
        if (d == end && *d && *d != ',') break;
    }
    fflush(stderr);
    abort();
}

/* Host exceptions while running guest code (an access outside committed guest
   memory, a host bug): report the guest address and the guest registers. */
static CPU *(*crash_cpu)(void);
static LONG CALLBACK crash_handler(EXCEPTION_POINTERS *ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_ILLEGAL_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t at = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
    fprintf(stderr, "rc: host exception %08lx at host pc %p", code, ep->ExceptionRecord->ExceptionAddress);
    if (code == EXCEPTION_ACCESS_VIOLATION) {
        if (at >= (uintptr_t)rc_mem && at - (uintptr_t)rc_mem < ((uintptr_t)1 << 32))
            fprintf(stderr, ", guest address %08x (%s)", (uint32_t)(at - (uintptr_t)rc_mem),
                    ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read");
        else
            fprintf(stderr, ", host address %p", (void *)at);
    }
    fprintf(stderr, "\n");
    CPU *c = crash_cpu ? crash_cpu() : NULL;
    if (c) rc_fault(c, 0, "host exception");
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}
void rc_install_crash_handler(CPU *(*current_cpu)(void)) {
    crash_cpu = current_cpu;
    AddVectoredExceptionHandler(1, crash_handler);
}

void rc_unimpl(CPU *c, uint32_t addr, const char *insn) {
    fprintf(stderr, "rc: unimplemented instruction at %08x: %s\n", addr, insn);
    rc_fault(c, addr, "unimplemented instruction");
}

/* -- function replacements usable from any game config ([replace.<module>]) -- */
/* cdecl function that returns 0 (e.g. the VC6 CRT's __sbh_alloc_block: "small-block
   heap full", so malloc/calloc/realloc fall back to HeapAlloc). */
void rc_replace_return0_cdecl(CPU *c) { c->eax = 0; c->esp += 4; }

/* -- x87 --------------------------------------------------------------- */
f80 rc_fround(CPU *c, f80 v) {
    switch ((c->cw >> 10) & 3u) {
    case 0: return nearbyintl(v);   /* host default rounding = to nearest even */
    case 1: return floorl(v);
    case 2: return ceill(v);
    default: return truncl(v);
    }
}

int64_t rc_fist(CPU *c, f80 v, int bits) {
    f80 r = rc_fround(c, v);
    f80 lim = ldexpl(1.0L, bits - 1);
    if (!(r >= -lim && r < lim)) return bits == 64 ? INT64_MIN : -((int64_t)1 << (bits - 1));  /* integer indefinite */
    return (int64_t)r;
}

void rc_fxam(CPU *c) {
    f80 v = ST(0);
    uint16_t cc;                      /* C3 C2 C0 */
    if (v != v) cc = 0x0100u;         /* NaN */
    else if (isinf(v)) cc = 0x0500u;  /* infinity */
    else if (v == 0) cc = 0x4000u;    /* zero */
    else if (fpclassify(v) == FP_SUBNORMAL) cc = 0x4400u;
    else cc = 0x0400u;                /* normal */
    if (signbit(v)) cc |= 0x0200u;    /* C1 = sign */
    c->sw = (uint16_t)((c->sw & ~0x4700u) | cc);
}
