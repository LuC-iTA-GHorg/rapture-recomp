/* Differential test runner: executes recompiled guest functions on states
 * prepared by tools/difftest.py and writes the resulting states, which the
 * script compares with the same functions run in Unicorn.
 *
 * Usage: difftest GEN_DIR CASES_FILE RESULTS_FILE
 *
 * Case record (little endian):   u32 func, u32 regs[8] (eax..edi, esp ignored),
 *                                u32 args[ARGS], u8 scratch[SCRATCH_SIZE]
 * Result record:                 u32 regs[8], u32 crc_scratch, u32 crc_stack,
 *                                u32 crc_module[rc_module_count]
 */
#include "recomp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARGS 8
#define SCRATCH 0x00200000u
#define SCRATCH_SIZE 0x4000u
#define TEB 0x00300000u
#define STACK_WINDOW 0x1000u

static uint32_t crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: difftest GEN_DIR CASES RESULTS\n"); return 2; }
    if (rc_init()) return 2;
    uint8_t **pristine = calloc(rc_module_count, sizeof *pristine);
    for (uint32_t k = 0; k < rc_module_count; k++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", argv[1], rc_module_table[k].image);
        if (rc_load_image(path, rc_module_table[k].base, rc_module_table[k].size)) return 2;
        pristine[k] = malloc(rc_module_table[k].size);
        memcpy(pristine[k], rc_ptr(rc_module_table[k].base), rc_module_table[k].size);
    }
    rc_commit(SCRATCH, SCRATCH_SIZE);
    rc_commit(TEB, 0x1000);
    uint32_t stack_top = rc_alloc_stack(0x10000);
    uint32_t stack_lo = stack_top - STACK_WINDOW + 16;

    FILE *in = fopen(argv[2], "rb"), *out = fopen(argv[3], "wb");
    if (!in || !out) { fprintf(stderr, "difftest: cannot open files\n"); return 2; }
    uint32_t hdr[1 + 8 + ARGS];
    static uint8_t scratch[SCRATCH_SIZE];
    int n = 0;
    while (fread(hdr, sizeof hdr, 1, in) == 1 && fread(scratch, SCRATCH_SIZE, 1, in) == 1) {
        if (getenv("DIFFTEST_VERBOSE")) fprintf(stderr, "case %d func %08x\n", n, hdr[0]);
        for (uint32_t k = 0; k < rc_module_count; k++)
            memcpy(rc_ptr(rc_module_table[k].base), pristine[k], rc_module_table[k].size);
        memcpy(rc_ptr(SCRATCH), scratch, SCRATCH_SIZE);
        memset(rc_ptr(stack_top + 16 - 0x10000u), 0, 0x10000u);
        memset(rc_ptr(TEB), 0, 0x1000);
        wr32(TEB, 0xFFFFFFFFu);
        CPU c;
        memset(&c, 0, sizeof c);
        c.cw = 0x027F;
        c.fs_base = TEB;
        uint32_t *r = &c.eax;
        for (int k = 0; k < 8; k++) r[k] = hdr[1 + k];
        c.esp = stack_top;
        for (int k = ARGS - 1; k >= 0; k--) { c.esp -= 4; wr32(c.esp, hdr[9 + k]); }
        c.esp -= 4; wr32(c.esp, 0xFFFFFFF0u);
        rc_call(&c, hdr[0]);
        uint32_t res[8 + 2];
        for (int k = 0; k < 8; k++) res[k] = r[k];
        res[8] = crc32(rc_ptr(SCRATCH), SCRATCH_SIZE);
        res[9] = crc32(rc_ptr(stack_lo), STACK_WINDOW);
        fwrite(res, sizeof res, 1, out);
        for (uint32_t k = 0; k < rc_module_count; k++) {
            uint32_t m = crc32(rc_ptr(rc_module_table[k].base), rc_module_table[k].size);
            fwrite(&m, 4, 1, out);
        }
        fflush(out);
        n++;
    }
    fprintf(stderr, "\n%d cases\n", n);
    return 0;
}
