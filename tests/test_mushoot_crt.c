/* Differential tests of recompiled Shoot.dll functions against host libc.
 *
 * The functions under test are the statically linked C runtime routines of
 * Shoot.dll (strlen, strcpy, strcat, memcpy, memmove, memset) plus one pure
 * lookup function. Only their guest addresses appear here; the code under test
 * is generated at build time from the user's own Shoot.dll.
 *
 * Also the x87 routines _ftol, _CIsqrt and _CIatan2 of Aaron.dll's CRT.
 *
 * Usage: test_mushoot_crt GEN_DIR      (GEN_DIR holds the *.bin images)
 */
#include "recomp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Shoot.dll guest addresses (base 0x10000000, configs/mushoot.toml). */
#define SHOOT_STRLEN  0x10020780u
#define SHOOT_STRCPY  0x10020690u
#define SHOOT_STRCAT  0x100206a0u
#define SHOOT_MEMCPY_A 0x1001e0d0u
#define SHOOT_MEMCPY_B 0x10020be0u
#define SHOOT_MEMSET  0x100215e0u
#define SHOOT_CP2LCID 0x100209deu   /* code page -> LCID of the CJK code pages */

/* Aaron.dll guest addresses (base 0x10200000): x87 routines of its static CRT. */
#define AARON_FTOL    0x10215494u   /* _ftol: st(0) -> edx:eax, truncating */
#define AARON_CISQRT  0x102154c0u   /* _CIsqrt: st(0) = sqrt(st(0)) */
#define AARON_CIATAN2 0x1021548au   /* _CIatan2: st(0) = atan2(st(1), st(0)), pops */

#define SCRATCH 0x00200000u        /* guest scratch area */
#define SCRATCH_SIZE 0x10000u
#define TEB 0x00300000u

static int failures, checks;
static CPU cpu;
static uint32_t stack_top;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static uint32_t call(uint32_t fn, int argc, const uint32_t *argv) {
    cpu.esp = stack_top;
    uint32_t saved_ebx = cpu.ebx = 0x11111111u, saved_esi = cpu.esi = 0x22222222u,
             saved_edi = cpu.edi = 0x33333333u, saved_ebp = cpu.ebp = 0x44444444u;
    uint32_t r = rc_invoke(&cpu, fn, argc, argv);
    CHECK(cpu.ebx == saved_ebx && cpu.esi == saved_esi && cpu.edi == saved_edi && cpu.ebp == saved_ebp,
          "callee-saved registers clobbered by %08x", fn);
    return r;
}

#define ST_OF(i) (cpu.st[(cpu.top + (i)) & 7u])

static char *g(uint32_t a) { return (char *)rc_ptr(a); }

static void test_strlen(void) {
    for (int off = 0; off < 4; off++)
        for (int len = 0; len < 40; len++) {
            uint32_t s = SCRATCH + 16 + off;
            memset(g(SCRATCH), 'x', 128);
            g(s)[len] = 0;
            uint32_t args[1] = {s};
            uint32_t r = call(SHOOT_STRLEN, 1, args);
            CHECK(r == (uint32_t)len, "strlen off=%d len=%d -> %u", off, len, r);
        }
}

static void test_strcpy_strcat(void) {
    const char *words[] = {"", "a", "abc", "abcd", "hello world", "0123456789abcdefghij"};
    for (unsigned w = 0; w < 6; w++)
        for (int off = 0; off < 4; off++) {
            uint32_t dst = SCRATCH + 0x100 + off, src = SCRATCH + 0x200 + (3 - off);
            memset(g(dst), 0x55, 64);
            strcpy(g(src), words[w]);
            uint32_t args[2] = {dst, src};
            uint32_t r = call(SHOOT_STRCPY, 2, args);
            CHECK(r == dst && strcmp(g(dst), words[w]) == 0, "strcpy '%s' off=%d", words[w], off);
            r = call(SHOOT_STRCAT, 2, args);
            char want[128];
            snprintf(want, sizeof want, "%s%s", words[w], words[w]);
            CHECK(r == dst && strcmp(g(dst), want) == 0, "strcat '%s' off=%d -> '%s'", words[w], off, g(dst));
        }
}

static void test_mem(uint32_t fn, const char *name) {
    static unsigned char want[512];
    for (int n = 0; n < 80; n += (n < 20 ? 1 : 7))
        for (int so = 0; so < 4; so++)
            for (int doff = 0; doff < 4; doff++) {
                uint32_t src = SCRATCH + 0x1000 + so, dst = SCRATCH + 0x2000 + doff;
                for (int k = 0; k < 256; k++) { g(SCRATCH + 0x1000)[k] = (char)(k * 7 + 1); g(SCRATCH + 0x2000)[k] = (char)0xEE; }
                memcpy(want, g(SCRATCH + 0x2000), 256);
                memcpy(want + doff, g(src), (size_t)n);
                uint32_t args[3] = {dst, src, (uint32_t)n};
                uint32_t r = call(fn, 3, args);
                CHECK(r == dst && memcmp(want, g(SCRATCH + 0x2000), 256) == 0,
                      "%s n=%d so=%d do=%d", name, n, so, doff);
            }
    /* overlapping move (both directions); memcpy in this CRT is memmove */
    for (int n = 1; n < 70; n += 3)
        for (int shift = -9; shift <= 9; shift += 3) {
            uint32_t buf = SCRATCH + 0x3000;
            for (int k = 0; k < 256; k++) g(buf)[k] = (char)k;
            memcpy(want, g(buf), 256);
            memmove(want + 64 + shift, want + 64, (size_t)n);
            uint32_t args[3] = {buf + 64 + (uint32_t)shift, buf + 64, (uint32_t)n};
            call(fn, 3, args);
            CHECK(memcmp(want, g(buf), 256) == 0, "%s overlap n=%d shift=%d", name, n, shift);
        }
}

static void test_memset(void) {
    unsigned char want[256];
    for (int n = 0; n < 100; n += (n < 16 ? 1 : 5))
        for (int off = 0; off < 4; off++) {
            uint32_t buf = SCRATCH + 0x4000;
            memset(g(buf), 0x11, 256);
            memset(want, 0x11, 256);
            memset(want + off, 0xA7, (size_t)n);
            uint32_t args[3] = {buf + (uint32_t)off, 0x1A7u /* only the low byte counts */, (uint32_t)n};
            uint32_t r = call(SHOOT_MEMSET, 3, args);
            CHECK(r == buf + (uint32_t)off && memcmp(want, g(buf), 256) == 0, "memset n=%d off=%d", n, off);
        }
}

static void test_cp2lcid(void) {
    struct { uint32_t cp, lcid; } t[] = {{932, 0x411}, {936, 0x804}, {949, 0x412}, {950, 0x404}, {1252, 0}, {0, 0}, {933, 0}};
    for (unsigned k = 0; k < sizeof t / sizeof t[0]; k++) {
        uint32_t args[1] = {t[k].cp};
        uint32_t r = call(SHOOT_CP2LCID, 1, args);
        CHECK(r == t[k].lcid, "cp2lcid(%u) = %#x, want %#x", t[k].cp, r, t[k].lcid);
    }
}

static void test_x87(void) {
    const double v[] = {0.0, 0.4, 0.5, 0.6, 1.0, 3.7, -3.7, -0.5, 2.5, -2.5, 123456.999, 1e10, -1e10, 4294967295.0};
    for (unsigned k = 0; k < sizeof v / sizeof v[0]; k++) {
        cpu.cw = 0x027F;                     /* round to nearest, as set by the CRT startup */
        uint32_t top = cpu.top;
        fpush(&cpu, v[k]);
        call(AARON_FTOL, 0, NULL);
        int64_t got = (int64_t)(((uint64_t)cpu.edx << 32) | cpu.eax);
        CHECK(got == (int64_t)v[k], "_ftol(%g) = %lld", v[k], (long long)got);
        CHECK(cpu.cw == 0x027F && cpu.top == top, "_ftol(%g) did not restore cw/stack", v[k]);
    }
    const double s[] = {0.0, 1.0, 2.0, 0.25, 1e6, 12345.678};
    for (unsigned k = 0; k < sizeof s / sizeof s[0]; k++) {
        uint32_t top = cpu.top;
        fpush(&cpu, s[k]);
        call(AARON_CISQRT, 0, NULL);
        CHECK(ST_OF(0) == sqrt(s[k]), "_CIsqrt(%g) = %.17g", s[k], ST_OF(0));
        fpop(&cpu);
        CHECK(cpu.top == top, "_CIsqrt stack");
    }
    const double yx[][2] = {{1, 1}, {-1, 1}, {1, -1}, {-1, -1}, {0, 1}, {3, 0}, {-30, 40}};
    for (unsigned k = 0; k < sizeof yx / sizeof yx[0]; k++) {
        uint32_t top = cpu.top;
        fpush(&cpu, yx[k][0]);
        fpush(&cpu, yx[k][1]);
        call(AARON_CIATAN2, 0, NULL);
        CHECK(fabs(ST_OF(0) - atan2(yx[k][0], yx[k][1])) < 1e-15, "_CIatan2(%g, %g) = %.17g", yx[k][0], yx[k][1], ST_OF(0));
        fpop(&cpu);
        CHECK(cpu.top == top, "_CIatan2 stack");
    }
}

int main(int argc, char **argv) {
    const char *gen = argc > 1 ? argv[1] : ".";
    rc_trace = getenv("RC_TRACE") != NULL;
    if (rc_init()) { printf("cannot reserve guest memory\n"); return 2; }
    for (uint32_t k = 0; k < rc_module_count; k++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", gen, rc_module_table[k].image);
        if (rc_load_image(path, rc_module_table[k].base, rc_module_table[k].size)) return 2;
    }
    rc_commit(SCRATCH, SCRATCH_SIZE);
    rc_commit(TEB, 0x1000);
    wr32(TEB, 0xFFFFFFFFu);          /* empty SEH chain */
    cpu.fs_base = TEB;
    stack_top = rc_alloc_stack(0x10000);

    test_strlen();
    test_strcpy_strcat();
    test_mem(SHOOT_MEMCPY_A, "memcpy_a");
    test_mem(SHOOT_MEMCPY_B, "memcpy_b");
    test_memset();
    test_cp2lcid();
    test_x87();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
