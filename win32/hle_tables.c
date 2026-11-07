/* List of the HLE tables of the Win32 layer (one per emulated DLL). */
#include "recomp.h"

extern const rc_hle_entry w32_kernel32_table[];
extern const rc_hle_entry w32_user32_table[];
extern const rc_hle_entry w32_gdi32_table[];
extern const rc_hle_entry w32_winmm_table[];
extern const rc_hle_entry w32_advapi32_table[];
extern const rc_hle_entry w32_version_table[];
extern const rc_hle_entry w32_dx_table[];

const rc_hle_entry *const rc_hle_tables[] = {
    w32_kernel32_table,
    w32_user32_table,
    w32_gdi32_table,
    w32_winmm_table,
    w32_advapi32_table,
    w32_version_table,
    w32_dx_table,
    0
};
