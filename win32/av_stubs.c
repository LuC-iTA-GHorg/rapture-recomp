/* Temporary: no DirectSound device and no AVI window yet. */
#include "w32.h"

uint32_t w32_dsound_create(uint32_t *obj) { *obj = 0; return 0x88780078u; /* DSERR_NODRIVER */ }
void w32_mciwnd_create(CPU *c) { W32_STUB("MCIWndCreateA"); RETC(0); }
