/* Entry point of a recompiled game: sets up the guest process and runs the
 * original executable's entry point.
 *
 * Usage: <game>.exe [--gen DIR] [--log] [--trace N] [host options, see window.cpp]
 * The current directory must be the game folder (the one with the .rrs files).
 * GEN_DIR (default: the "gen" folder next to the executable) holds the rebased
 * images produced by the recompiler at build time.
 */
#include "w32.h"

#include <windows.h>

void w32_attach_static_dlls(void);
void w32_set_settings_path(const char *path);
extern const char rc_game_settings[];
int w32_host_parse_args(int argc, char **argv);   /* window.cpp */

int main(int argc, char **argv) {
    char gen[MAX_PATH * 2] = "";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--gen") && i + 1 < argc) snprintf(gen, sizeof gen, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--log")) w32_log_level = 1;
        else if (!strcmp(argv[i], "--trace") && i + 1 < argc) rc_trace = atoi(argv[++i]);
    }
    if (!gen[0]) {
        char exe[MAX_PATH];
        GetModuleFileNameA(NULL, exe, sizeof exe);
        char *slash = strrchr(exe, '\\');
        if (slash) *slash = 0;
        snprintf(gen, sizeof gen, "%s\\gen", exe);
    }
    char settings[MAX_PATH * 2] = "";
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--settings") && i + 1 < argc) snprintf(settings, sizeof settings, "%s", argv[++i]);
    if (!settings[0]) {
        const char *ad = getenv("APPDATA");
        snprintf(settings, sizeof settings, "%s\\%s", ad ? ad : ".", rc_game_settings);
    }
    w32_set_settings_path(settings);
    if (w32_host_parse_args(argc, argv)) return 2;
    if (rc_init()) { fprintf(stderr, "cannot reserve the guest address space\n"); return 2; }
    CPU *c = w32_main_cpu();
    rc_install_crash_handler(w32_cpu);
    if (w32_load_modules(gen)) {
        fprintf(stderr, "cannot load the recompiled images from %s (run the build first)\n", gen);
        return 2;
    }
    w32_attach_static_dlls();
    uint32_t exe = w32_exe_base();
    int k = w32_module_index(exe);
    W32_LOG("starting %s at %08x", rc_module_table[k].name, rc_module_table[k].entry);
    rc_invoke(c, rc_module_table[k].entry, 0, NULL);
    return (int)c->eax;
}
