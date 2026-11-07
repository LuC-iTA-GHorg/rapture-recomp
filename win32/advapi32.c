/* ADVAPI32: the registry, kept in an .ini file instead of the real registry.
 *
 * Same mapping as rapture-engine's aaron/RegKey: a key is a section named by its
 * full path with the hive prefix ([HKLM\Software\...]); REG_DWORD values are
 * `Name=1234`, REG_BINARY `Name=hex:0a0b...`, REG_SZ `Name=str:text`. The file
 * is %APPDATA%\<folder>\<file> (w32_settings_path, set from the game config or
 * --settings FILE) and is rewritten after every change.
 */
#include "w32.h"

#include <windows.h>

typedef struct { char *name, *value; } ini_val;
typedef struct { char *name; ini_val *v; int nv, cap; } ini_sec;
static ini_sec *secs;
static int nsecs, capsecs, loaded;
static char settings_path[MAX_PATH * 2];
static CRITICAL_SECTION reg_lock;

void w32_set_settings_path(const char *path) { snprintf(settings_path, sizeof settings_path, "%s", path); }

static char *dup(const char *s) { size_t l = strlen(s) + 1; char *d = (char *)malloc(l); memcpy(d, s, l); return d; }

static ini_sec *find_sec(const char *name, int create) {
    for (int i = 0; i < nsecs; i++) if (!lstrcmpiA(secs[i].name, name)) return &secs[i];
    if (!create) return NULL;
    if (nsecs == capsecs) { capsecs = capsecs ? capsecs * 2 : 16; secs = (ini_sec *)realloc(secs, sizeof *secs * capsecs); }
    ini_sec *s = &secs[nsecs++];
    memset(s, 0, sizeof *s);
    s->name = dup(name);
    return s;
}
static ini_val *find_val(ini_sec *s, const char *name, int create) {
    for (int i = 0; i < s->nv; i++) if (!lstrcmpiA(s->v[i].name, name)) return &s->v[i];
    if (!create) return NULL;
    if (s->nv == s->cap) { s->cap = s->cap ? s->cap * 2 : 8; s->v = (ini_val *)realloc(s->v, sizeof *s->v * s->cap); }
    ini_val *v = &s->v[s->nv++];
    v->name = dup(name);
    v->value = dup("");
    return v;
}

static void load(void) {
    if (loaded) return;
    loaded = 1;
    InitializeCriticalSection(&reg_lock);
    if (!settings_path[0]) {
        const char *ad = getenv("APPDATA");
        snprintf(settings_path, sizeof settings_path, "%s\\Rapture\\recomp.ini", ad ? ad : ".");
    }
    FILE *f = fopen(settings_path, "rb");
    if (!f) return;
    char line[4096];
    ini_sec *cur = NULL;
    while (fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        if (!l || line[0] == ';') continue;
        if (line[0] == '[') {
            char *e = strrchr(line, ']');
            if (e) *e = 0;
            cur = find_sec(line + 1, 1);
        } else if (cur) {
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            ini_val *v = find_val(cur, line, 1);
            free(v->value);
            v->value = dup(eq + 1);
        }
    }
    fclose(f);
}

static void save(void) {
    char dir[MAX_PATH * 2];
    snprintf(dir, sizeof dir, "%s", settings_path);
    char *s = strrchr(dir, '\\');
    if (s) { *s = 0; CreateDirectoryA(dir, NULL); }
    FILE *f = fopen(settings_path, "wb");
    if (!f) return;
    for (int i = 0; i < nsecs; i++) {
        fprintf(f, "[%s]\r\n", secs[i].name);
        for (int j = 0; j < secs[i].nv; j++) fprintf(f, "%s=%s\r\n", secs[i].v[j].name, secs[i].v[j].value);
        fprintf(f, "\r\n");
    }
    fclose(f);
}

/* -- handles ------------------------------------------------------------- */
#define KEY_BASE 0x00A00000u
#define MAX_KEYS 256
static char *keys[MAX_KEYS];

static const char *key_path(uint32_t h) {
    switch (h) {
    case 0x80000000u: return "HKCR";
    case 0x80000001u: return "HKCU";
    case 0x80000002u: return "HKLM";
    case 0x80000003u: return "HKU";
    }
    uint32_t k = (h - KEY_BASE) / 4u;
    if (h < KEY_BASE || k >= MAX_KEYS || !keys[k]) return NULL;
    return keys[k];
}

static int key_exists(const char *path) {
    size_t l = strlen(path);
    for (int i = 0; i < nsecs; i++)
        if (!_strnicmp(secs[i].name, path, l) && (secs[i].name[l] == 0 || secs[i].name[l] == '\\')) return 1;
    return 0;
}

static uint32_t open_key(uint32_t parent, const char *sub, int create) {
    const char *p = key_path(parent);
    if (!p) return 0;
    char path[1024];
    if (sub && *sub) snprintf(path, sizeof path, "%s\\%s", p, sub); else snprintf(path, sizeof path, "%s", p);
    if (!create && !key_exists(path)) return 0;
    if (create && !find_sec(path, 0)) { find_sec(path, 1); save(); }
    for (int k = 0; k < MAX_KEYS; k++)
        if (!keys[k]) { keys[k] = dup(path); return KEY_BASE + 4u * (uint32_t)k; }
    return 0;
}

static void a_RegOpenKeyExA(CPU *c) {
    load();
    EnterCriticalSection(&reg_lock);
    uint32_t h = open_key(ARG(0), GS(ARG(1)), 0);
    LeaveCriticalSection(&reg_lock);
    if (!h) RET(5, ERROR_FILE_NOT_FOUND);
    wr32(ARG(4), h);
    RET(5, ERROR_SUCCESS);
}
static void a_RegCreateKeyExA(CPU *c) {
    load();
    EnterCriticalSection(&reg_lock);
    const char *p = key_path(ARG(0));
    char path[1024];
    snprintf(path, sizeof path, "%s\\%s", p ? p : "?", GS(ARG(1)) ? GS(ARG(1)) : "");
    int existed = key_exists(path);
    uint32_t h = open_key(ARG(0), GS(ARG(1)), 1);
    LeaveCriticalSection(&reg_lock);
    if (!h) RET(9, ERROR_ACCESS_DENIED);
    wr32(ARG(7), h);
    if (ARG(8)) wr32(ARG(8), existed ? REG_OPENED_EXISTING_KEY : REG_CREATED_NEW_KEY);
    RET(9, ERROR_SUCCESS);
}
static void a_RegCloseKey(CPU *c) {
    uint32_t h = ARG(0), k = (h - KEY_BASE) / 4u;
    if (h >= KEY_BASE && k < MAX_KEYS && keys[k]) { free(keys[k]); keys[k] = NULL; }
    RET(1, ERROR_SUCCESS);
}

static int hexv(char ch) { return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : 0; }

static void a_RegQueryValueExA(CPU *c) {
    load();
    uint32_t type_p = ARG(3), data = ARG(4), size_p = ARG(5);
    const char *path = key_path(ARG(0));
    const char *name = ARG(1) ? GS(ARG(1)) : "";
    EnterCriticalSection(&reg_lock);
    ini_sec *s = path ? find_sec(path, 0) : NULL;
    ini_val *v = s ? find_val(s, name, 0) : NULL;
    if (!v) { LeaveCriticalSection(&reg_lock); RET(6, ERROR_FILE_NOT_FOUND); }
    uint8_t buf[2048];
    uint32_t n, type;
    if (!strncmp(v->value, "hex:", 4)) {
        type = REG_BINARY;
        const char *h = v->value + 4;
        for (n = 0; h[0] && h[1] && n < sizeof buf; h += 2) buf[n++] = (uint8_t)(hexv(h[0]) * 16 + hexv(h[1]));
    } else if (!strncmp(v->value, "str:", 4)) {
        type = REG_SZ;
        n = (uint32_t)strlen(v->value + 4) + 1;
        if (n > sizeof buf) n = sizeof buf;
        memcpy(buf, v->value + 4, n);
        buf[n - 1] = 0;
    } else {
        type = REG_DWORD;
        uint32_t d = (uint32_t)strtoul(v->value, NULL, 0);
        memcpy(buf, &d, 4);
        n = 4;
    }
    LeaveCriticalSection(&reg_lock);
    if (type_p) wr32(type_p, type);
    if (!size_p) RET(6, data ? ERROR_INVALID_PARAMETER : ERROR_SUCCESS);
    uint32_t cap = rd32(size_p);
    wr32(size_p, n);
    if (!data) RET(6, ERROR_SUCCESS);
    if (cap < n) RET(6, ERROR_MORE_DATA);
    memcpy(G(data), buf, n);
    RET(6, ERROR_SUCCESS);
}
static void a_RegSetValueExA(CPU *c) {
    load();
    const char *path = key_path(ARG(0));
    const char *name = ARG(1) ? GS(ARG(1)) : "";
    uint32_t type = ARG(3), data = ARG(4), n = ARG(5);
    if (!path) RET(6, ERROR_INVALID_HANDLE);
    char *text = (char *)malloc(2 * n + 16);
    const uint8_t *d = (const uint8_t *)G(data);
    if (type == REG_DWORD && n == 4) { uint32_t x; memcpy(&x, d, 4); sprintf(text, "%u", x); }
    else if (type == REG_SZ) { sprintf(text, "str:"); memcpy(text + 4, d, n); text[4 + (n ? n - 1 : 0)] = 0; }
    else { strcpy(text, "hex:"); for (uint32_t i = 0; i < n; i++) sprintf(text + 4 + 2 * i, "%02x", d[i]); }
    EnterCriticalSection(&reg_lock);
    ini_val *v = find_val(find_sec(path, 1), name, 1);
    free(v->value);
    v->value = text;
    save();
    LeaveCriticalSection(&reg_lock);
    RET(6, ERROR_SUCCESS);
}
static void a_RegDeleteValueA(CPU *c) {
    load();
    const char *path = key_path(ARG(0));
    EnterCriticalSection(&reg_lock);
    ini_sec *s = path ? find_sec(path, 0) : NULL;
    ini_val *v = s ? find_val(s, ARG(1) ? GS(ARG(1)) : "", 0) : NULL;
    if (v) { free(v->name); free(v->value); *v = s->v[--s->nv]; save(); }
    LeaveCriticalSection(&reg_lock);
    RET(2, v ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND);
}
static void a_RegDeleteKeyA(CPU *c) {
    load();
    const char *p = key_path(ARG(0));
    char path[1024];
    snprintf(path, sizeof path, "%s\\%s", p ? p : "?", GS(ARG(1)) ? GS(ARG(1)) : "");
    size_t l = strlen(path);
    EnterCriticalSection(&reg_lock);
    int found = 0;
    for (int i = 0; i < nsecs;) {
        if (!_strnicmp(secs[i].name, path, l) && (secs[i].name[l] == 0 || secs[i].name[l] == '\\')) {
            found = 1;
            secs[i] = secs[--nsecs];
        } else i++;
    }
    if (found) save();
    LeaveCriticalSection(&reg_lock);
    RET(2, found ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND);
}

#define A(n, f) {"advapi32.dll", n, f}
const rc_hle_entry w32_advapi32_table[] = {
    A("RegOpenKeyExA", a_RegOpenKeyExA), A("RegCreateKeyExA", a_RegCreateKeyExA), A("RegCloseKey", a_RegCloseKey),
    A("RegQueryValueExA", a_RegQueryValueExA), A("RegSetValueExA", a_RegSetValueExA),
    A("RegDeleteValueA", a_RegDeleteValueA), A("RegDeleteKeyA", a_RegDeleteKeyA),
    {NULL, NULL, NULL}
};

/* VERSION: the About box reads the version resource of the exe; report none. */
static void v_GetFileVersionInfoSizeA(CPU *c) { if (ARG(1)) wr32(ARG(1), 0); RET(2, 0); }
static void v_GetFileVersionInfoA(CPU *c) { RET(4, 0); }
static void v_VerQueryValueA(CPU *c) { RET(4, 0); }
const rc_hle_entry w32_version_table[] = {
    {"version.dll", "GetFileVersionInfoSizeA", v_GetFileVersionInfoSizeA},
    {"version.dll", "GetFileVersionInfoA", v_GetFileVersionInfoA},
    {"version.dll", "VerQueryValueA", v_VerQueryValueA},
    {NULL, NULL, NULL}
};
