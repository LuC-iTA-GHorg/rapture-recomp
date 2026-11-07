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
extern const char rc_game_title[];
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
        if (!strcmp(argv[i], "--settings") && i + 1 < argc) GetFullPathNameA(argv[++i], sizeof settings, settings, NULL);
    if (!settings[0]) {
        const char *ad = getenv("APPDATA");
        snprintf(settings, sizeof settings, "%s\\%s", ad ? ad : ".", rc_game_settings);
    }
    w32_set_settings_path(settings);
    if (w32_host_parse_args(argc, argv)) return 2;

    /* The game runs in its own folder (the one with the original exe and the data
       files): --game-dir DIR, else the current folder if it is one, else the folder
       this executable was built from. */
    const char *game_dir = NULL;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--game-dir") && i + 1 < argc) game_dir = argv[++i];
    const char *exe_name = rc_module_table[0].name;   /* the original exe comes first in the config */
    if (!game_dir && GetFileAttributesA(exe_name) == INVALID_FILE_ATTRIBUTES) {
#ifdef RC_BUILD_GAME_DIR
        game_dir = RC_BUILD_GAME_DIR;
#endif
    }
    if (game_dir && !SetCurrentDirectoryA(game_dir)) {
        fprintf(stderr, "cannot enter the game folder %s\n", game_dir);
        return 2;
    }
    if (GetFileAttributesA(exe_name) == INVALID_FILE_ATTRIBUTES) {
        char msg[512];
        snprintf(msg, sizeof msg, "The game files (%s and the data files) are not in the current folder.\n"
                 "Start the game from its folder or pass --game-dir FOLDER.", exe_name);
        fprintf(stderr, "%s\n", msg);
        MessageBoxA(NULL, msg, rc_game_title, MB_ICONERROR);
        return 2;
    }
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
