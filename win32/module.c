/* Guest modules: image mapping, DllMain, LoadLibrary/GetProcAddress, resources. */
#include "w32.h"

#include <windows.h>

static char game_dir[MAX_PATH];
static uint8_t module_attached[64];

/* System DLLs loaded at run time with LoadLibraryA (ddraw, dsound, msvfw32...):
   fake module handles whose GetProcAddress hands out host thunks. */
#define SYS_HANDLE_BASE 0x7F000000u
static char sys_names[16][32];
static uint32_t sys_count;

const char *w32_game_dir(void) { return game_dir; }

uint32_t w32_exe_base(void) {
    for (uint32_t k = 0; k < rc_module_count; k++) {
        const char *n = rc_module_table[k].name;
        size_t l = strlen(n);
        if (l > 4 && rc_ieq(n + l - 4, ".exe")) return rc_module_table[k].base;
    }
    return rc_module_table[0].base;
}

static const char *base_name(const char *path) {
    const char *b = path;
    for (const char *p = path; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':') b = p + 1;
    return b;
}

/* "shoot.dll" matches "Shoot.DLL", "SHOOT" and "c:\x\shoot.dll". */
static int name_matches(const char *module, const char *want) {
    want = base_name(want);
    if (rc_ieq(module, want)) return 1;
    size_t l = strlen(want);
    char tmp[MAX_PATH];
    if (l + 5 >= sizeof tmp || strchr(want, '.')) return 0;
    memcpy(tmp, want, l);
    memcpy(tmp + l, ".dll", 5);
    return rc_ieq(module, tmp);
}

uint32_t w32_module_handle(const char *name) {
    if (!name) return w32_exe_base();
    for (uint32_t k = 0; k < rc_module_count; k++)
        if (name_matches(rc_module_table[k].name, name)) return rc_module_table[k].base;
    for (uint32_t k = 0; k < sys_count; k++)
        if (name_matches(sys_names[k], name)) return SYS_HANDLE_BASE + 0x10000u * k;
    return 0;
}

int w32_module_index(uint32_t hmod) {
    for (uint32_t k = 0; k < rc_module_count; k++)
        if (rc_module_table[k].base == hmod) return (int)k;
    return -1;
}

uint32_t w32_module_of_address(uint32_t addr) {
    for (uint32_t k = 0; k < rc_module_count; k++)
        if (addr >= rc_module_table[k].base && addr - rc_module_table[k].base < rc_module_table[k].size)
            return rc_module_table[k].base;
    return w32_exe_base();
}

static void attach(uint32_t k) {
    if (module_attached[k]) return;
    module_attached[k] = 1;
    const rc_module *m = &rc_module_table[k];
    size_t l = strlen(m->name);
    if (!m->entry || (l > 4 && rc_ieq(m->name + l - 4, ".exe"))) return;
    uint32_t args[3] = {m->base, 1 /* DLL_PROCESS_ATTACH */, 0};
    W32_LOG("DllMain %s", m->name);
    uint32_t r = rc_invoke(w32_cpu(), m->entry, 3, args);
    if (!r) fprintf(stderr, "w32: DllMain of %s failed\n", m->name);
}

int w32_load_modules(const char *gen_dir) {
    char exe[MAX_PATH];
    GetCurrentDirectoryA(sizeof game_dir, game_dir);
    (void)exe;
    for (uint32_t k = 0; k < rc_module_count; k++) {
        char path[MAX_PATH * 2];
        snprintf(path, sizeof path, "%s/%s", gen_dir, rc_module_table[k].image);
        if (rc_load_image(path, rc_module_table[k].base, rc_module_table[k].size)) return -1;
    }
    return 0;
}

/* Attach the DLLs the exe imports, in dependency order (a DLL is attached after the
   DLLs it imports from). The scene DLL is attached by LoadLibraryA. */
void w32_attach_static_dlls(void) {
    uint32_t exe = w32_exe_base();
    /* Collect the guest DLLs reachable through the import tables, depth first. */
    uint32_t order[64], n = 0;
    uint8_t seen[64] = {0};
    uint32_t stack[64], sp = 0;
    int ex = w32_module_index(exe);
    if (ex < 0) return;
    stack[sp++] = (uint32_t)ex;
    uint8_t expanded[64] = {0};
    while (sp) {
        uint32_t k = stack[sp - 1];
        if (!expanded[k]) {
            expanded[k] = 1;
            for (uint32_t i = 0; i < rc_import_count; i++) { (void)i; }
            /* guest-to-guest imports are not in rc_import_table (resolved statically);
               read the import directory of the image instead. */
            uint32_t base = rc_module_table[k].base;
            uint32_t pe = base + rd32(base + 0x3C);
            uint32_t rva = rd32(pe + 24 + 104), size = rd32(pe + 24 + 108);
            if (rva && size) {
                for (uint32_t d = base + rva; rd32(d + 12); d += 20) {
                    const char *dll = (const char *)rc_ptr(base + rd32(d + 12));
                    int j = -1;
                    for (uint32_t q = 0; q < rc_module_count; q++)
                        if (rc_ieq(rc_module_table[q].name, dll)) j = (int)q;
                    if (j >= 0 && !expanded[j] && sp < 64) stack[sp++] = (uint32_t)j;
                }
            }
            continue;
        }
        sp--;
        if (!seen[k]) { seen[k] = 1; order[n++] = k; }
    }
    for (uint32_t i = 0; i < n; i++) attach(order[i]);
}

uint32_t w32_load_library(const char *name) {
    for (uint32_t k = 0; k < rc_module_count; k++)
        if (name_matches(rc_module_table[k].name, name)) {
            attach(k);
            return rc_module_table[k].base;
        }
    for (uint32_t k = 0; k < sys_count; k++)
        if (name_matches(sys_names[k], name)) return SYS_HANDLE_BASE + 0x10000u * k;
    if (sys_count < 16) {
        snprintf(sys_names[sys_count], sizeof sys_names[0], "%s", base_name(name));
        if (!strchr(sys_names[sys_count], '.'))
            strncat(sys_names[sys_count], ".dll", sizeof sys_names[0] - strlen(sys_names[sys_count]) - 1);
        W32_LOG("LoadLibrary %s -> system module %u", name, sys_count);
        return SYS_HANDLE_BASE + 0x10000u * sys_count++;
    }
    return 0;
}

uint32_t w32_get_proc(uint32_t hmod, const char *name, uint32_t ordinal) {
    if (hmod >= SYS_HANDLE_BASE && hmod < SYS_HANDLE_BASE + 0x10000u * sys_count) {
        const char *dll = sys_names[(hmod - SYS_HANDLE_BASE) / 0x10000u];
        if (!name) return 0;
        rc_fn fn = rc_hle_find(dll, name);
        if (!fn) { fprintf(stderr, "w32: GetProcAddress %s!%s not implemented\n", dll, name); return 0; }
        return rc_host_thunk(fn, name);
    }
    int k = w32_module_index(hmod);
    if (k < 0) return 0;
    for (uint32_t i = 0; i < rc_export_count; i++) {
        const rc_export *e = &rc_export_table[i];
        if (e->module != (uint32_t)k) continue;
        if (name ? (e->name && strcmp(e->name, name) == 0) : e->ordinal == ordinal) return e->addr;
    }
    return 0;
}

/* -- resources ----------------------------------------------------------- */
/* Compare a resource directory entry name (length-prefixed UTF-16) with an ASCII name. */
static int res_name_eq(uint32_t s, const char *want) {
    uint16_t len = rd16(s);
    if (strlen(want) != len) return 0;
    for (uint16_t i = 0; i < len; i++) {
        uint16_t ch = rd16(s + 2 + 2u * i);
        char w = want[i];
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        if (w >= 'a' && w <= 'z') w -= 32;
        if (ch != (uint8_t)w) return 0;
    }
    return 1;
}

/* key: an integer id (< 0x10000) or a guest pointer to a name ("#123" means id 123). */
static uint32_t res_lookup(uint32_t root, uint32_t dir, uint32_t key) {
    const char *name = NULL;
    if (key >= 0x10000u && key != 0xFFFFFFFFu) {   /* 0xFFFFFFFF: any (the first entry) */
        name = (const char *)rc_ptr(key);
        if (name[0] == '#') { key = (uint32_t)atoi(name + 1); name = NULL; }
    }
    uint16_t nnamed = rd16(dir + 12), nid = rd16(dir + 14);
    for (uint32_t i = 0; i < (uint32_t)nnamed + nid; i++) {
        uint32_t e = dir + 16 + 8 * i, id = rd32(e), off = rd32(e + 4);
        int match;
        if (id & 0x80000000u) match = name && res_name_eq(root + (id & 0x7FFFFFFFu), name);
        else match = !name && (id == key || key == 0xFFFFFFFFu);
        if (match) return off;
    }
    return 0xFFFFFFFFu;
}

uint32_t w32_find_resource(uint32_t hmod, uint32_t type, uint32_t name, uint32_t *size) {
    if (w32_module_index(hmod) < 0) hmod = w32_exe_base();
    uint32_t pe = hmod + rd32(hmod + 0x3C);
    uint32_t rva = rd32(pe + 24 + 112);
    if (!rva) return 0;
    uint32_t root = hmod + rva;
    uint32_t off = res_lookup(root, root, type);
    if (off == 0xFFFFFFFFu || !(off & 0x80000000u)) return 0;
    off = res_lookup(root, root + (off & 0x7FFFFFFFu), name);
    if (off == 0xFFFFFFFFu || !(off & 0x80000000u)) return 0;
    off = res_lookup(root, root + (off & 0x7FFFFFFFu), 0xFFFFFFFFu);   /* first language */
    if (off == 0xFFFFFFFFu || (off & 0x80000000u)) return 0;
    uint32_t data = root + off;
    if (size) *size = rd32(data + 4);
    return hmod + rd32(data);
}
