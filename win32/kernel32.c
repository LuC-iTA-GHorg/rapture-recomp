/* KERNEL32: guest heap and virtual memory, threads, process environment,
 * plus forwarding of string/locale/file functions to the host. */
#include "w32.h"

#include <windows.h>

int w32_log_level;

#define GP(p) ((p) ? G(p) : NULL)              /* guest pointer -> host pointer, NULL stays NULL */
#define H(v) ((HANDLE)(intptr_t)(int32_t)(v))  /* guest handle -> host handle (sign-extended) */
#define HG(h) ((uint32_t)(uintptr_t)(h))        /* host handle -> guest handle (32-bit significant) */

/* Kernel handles seen by the guest are numbers of our own that are never reused:
   the games wait on and close handles of objects they already closed (a freed
   MsgQueue's mutexes), which on Windows 98 simply fails; with host handle values
   the number could already belong to another object. */
#define KH_BASE 0x00001000u
#define KH_MAX 65536
static HANDLE kh_tab[KH_MAX];
static uint32_t kh_next;
static CRITICAL_SECTION kh_lock;
static int kh_init;
static uint32_t KHG(HANDLE h) {
    if (!h || h == INVALID_HANDLE_VALUE) return HG(h);
    if (!kh_init) { InitializeCriticalSection(&kh_lock); kh_init = 1; }
    EnterCriticalSection(&kh_lock);
    uint32_t k = kh_next < KH_MAX ? kh_next++ : 0;
    kh_tab[k] = h;
    LeaveCriticalSection(&kh_lock);
    return KH_BASE + 4u * k;
}
static HANDLE KH(uint32_t g) {
    if (g == 0xFFFFFFFFu) return GetCurrentProcess();
    if (g == 0xFFFFFFFEu) return GetCurrentThread();
    uint32_t k = (g - KH_BASE) / 4u;
    if (g < KH_BASE || k >= KH_MAX) return NULL;
    return kh_tab[k];
}
static HANDLE KH_take(uint32_t g) {
    uint32_t k = (g - KH_BASE) / 4u;
    if (g < KH_BASE || k >= KH_MAX) return NULL;
    HANDLE h = kh_tab[k];
    kh_tab[k] = NULL;
    return h;
}

/* ======================================================================
 * Guest heap: one allocator for every HeapCreate heap and the process heap.
 * Blocks carry an 8-byte header {size, magic}; freed blocks go to segregated
 * free lists (16-byte classes up to 4 KiB, one first-fit list above).
 * ====================================================================== */
#define HEAP_BASE 0x20000000u
#define HEAP_END  0x60000000u
#define HEAP_MAGIC 0x48454150u   /* "HEAP" */
#define HEAP_FREE  0x46524545u   /* "FREE" */
#define NCLASS 257
static CRITICAL_SECTION heap_lock;
static uint32_t heap_top = HEAP_BASE, heap_committed = HEAP_BASE;
static uint32_t free_head[NCLASS];          /* guest address of the header, 0 = empty */

static void heap_init(void) {
    static int done;
    if (!done) { InitializeCriticalSection(&heap_lock); done = 1; }
}

static uint32_t cls(uint32_t size) { return size <= 4096 ? size / 16 : NCLASS - 1; }

uint32_t w32_heap_alloc(uint32_t size, int zero) {
    uint32_t need = (size + 15u) & ~15u;
    if (!need) need = 16;
    uint32_t total = need + 16;              /* 16-byte header keeps the payload 16-aligned */
    EnterCriticalSection(&heap_lock);
    uint32_t blk = 0;
    uint32_t k = cls(total);
    if (k < NCLASS - 1) {
        if (free_head[k]) { blk = free_head[k]; free_head[k] = rd32(blk + 8); }
    } else {
        uint32_t *prev = &free_head[NCLASS - 1];
        for (uint32_t b = *prev; b; prev = (uint32_t *)rc_ptr(b + 8), b = *prev)
            if (rd32(b) >= total) { *prev = rd32(b + 8); blk = b; total = rd32(b); break; }
    }
    if (!blk) {
        if (heap_top + total > HEAP_END) { LeaveCriticalSection(&heap_lock); return 0; }
        blk = heap_top;
        heap_top += total;
        while (heap_committed < heap_top) {
            rc_commit(heap_committed, 0x100000u);
            heap_committed += 0x100000u;
        }
    }
    wr32(blk, total);
    wr32(blk + 4, HEAP_MAGIC);
    LeaveCriticalSection(&heap_lock);
    if (zero) memset(rc_ptr(blk + 16), 0, total - 16);
    return blk + 16;
}

/* Freed blocks wait in a FIFO quarantine before they can be reused, and their
   payload is left untouched: the games read freed objects shortly after
   deleting them (e.g. Aaron's scene message queues, dispatched once more after
   the scene is gone), which works on the Windows heap because a freed block is
   not handed out again at once. */
#define QUARANTINE 4096
static uint32_t quarantine[QUARANTINE];
static uint32_t q_pos, q_count;

void w32_heap_free(uint32_t p) {
    if (!p) return;
    uint32_t blk = p - 16;
    if (rd32(blk + 4) != HEAP_MAGIC) { W32_LOG("heap: bad free %08x", p); return; }
    EnterCriticalSection(&heap_lock);
    wr32(blk + 4, HEAP_FREE);
    if (q_count == QUARANTINE) {
        uint32_t old = quarantine[q_pos];
        uint32_t k = cls(rd32(old));
        wr32(old + 8, free_head[k]);
        free_head[k] = old;
        q_count--;
    }
    quarantine[q_pos] = blk;
    q_pos = (q_pos + 1) % QUARANTINE;
    q_count++;
    LeaveCriticalSection(&heap_lock);
}

uint32_t w32_heap_size(uint32_t p) {
    if (!p || rd32(p - 12) != HEAP_MAGIC) return 0xFFFFFFFFu;
    return rd32(p - 16) - 16;
}

uint32_t w32_heap_realloc(uint32_t p, uint32_t size, int zero) {
    if (!p) return w32_heap_alloc(size, zero);
    uint32_t old = w32_heap_size(p);
    if (old == 0xFFFFFFFFu) return 0;
    if (size <= old) return p;
    uint32_t n = w32_heap_alloc(size, zero);
    if (!n) return 0;
    memcpy(rc_ptr(n), rc_ptr(p), old);
    w32_heap_free(p);
    return n;
}

uint32_t w32_guest_strdup(const char *s) {
    size_t l = strlen(s) + 1;
    uint32_t g = w32_heap_alloc((uint32_t)l, 0);
    if (g) memcpy(rc_ptr(g), s, l);
    return g;
}

static void k_HeapCreate(CPU *c) { heap_init(); RET(3, 0x00480000u); }   /* any non-zero handle */
static void k_HeapDestroy(CPU *c) { RET(1, 1); }
static void k_HeapAlloc(CPU *c) { RET(3, w32_heap_alloc(ARG(2), ARG(1) & 8u /* HEAP_ZERO_MEMORY */)); }
static void k_HeapFree(CPU *c) { w32_heap_free(ARG(2)); RET(3, 1); }
static void k_HeapSize(CPU *c) { RET(3, w32_heap_size(ARG(2))); }
static void k_HeapReAlloc(CPU *c) {
    uint32_t flags = ARG(1), p = ARG(2), size = ARG(3);
    if (flags & 0x10u /* HEAP_REALLOC_IN_PLACE_ONLY */) RET(4, size <= w32_heap_size(p) ? p : 0);
    uint32_t old = w32_heap_size(p);
    uint32_t n = w32_heap_realloc(p, size, 0);
    if (n && (flags & 8u) && size > old) memset(rc_ptr(n + old), 0, size - old);
    RET(4, n);
}
static void k_HeapCompact(CPU *c) { RET(2, 0x100000u); }

/* ======================================================================
 * Virtual memory: reservations come from a bump allocator, commits commit
 * guest pages. Nothing is ever returned (the CRT frees only at exit).
 * ====================================================================== */
#define VM_BASE 0x60000000u
#define VM_END  0x7E000000u
static uint32_t vm_top = VM_BASE;

static void k_VirtualAlloc(CPU *c) {
    uint32_t addr = ARG(0), size = ARG(1), type = ARG(2);
    if (!addr) {
        uint32_t sz = (size + 0xFFFFu) & ~0xFFFFu;
        if (vm_top + sz > VM_END) RET(4, 0);
        addr = vm_top;
        vm_top += sz;
    }
    if (type & MEM_COMMIT) {
        uint32_t lo = addr & ~0xFFFu, hi = (addr + size + 0xFFFu) & ~0xFFFu;
        if (rc_commit(lo, hi - lo)) RET(4, 0);
    }
    RET(4, addr);
}
static void k_VirtualFree(CPU *c) { RET(3, 1); }
static void k_VirtualQuery(CPU *c) {
    uint32_t addr = ARG(0), buf = ARG(1);
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(rc_ptr(addr), &mbi, sizeof mbi)) RET(3, 0);
    uint32_t base = (uint32_t)((uint8_t *)mbi.BaseAddress - rc_mem);
    wr32(buf + 0, base);
    wr32(buf + 4, (uint32_t)((uint8_t *)mbi.AllocationBase - rc_mem));
    wr32(buf + 8, mbi.AllocationProtect);
    wr32(buf + 12, (uint32_t)mbi.RegionSize);
    wr32(buf + 16, mbi.State);
    wr32(buf + 20, mbi.Protect);
    wr32(buf + 24, mbi.Type);
    RET(3, 28);
}

/* ======================================================================
 * Threads. Each host thread running guest code has a CPU, a guest stack
 * and a TEB (fs:): [0] SEH chain, [4] stack top, [8] stack limit, [0x18]
 * self, [0x24] thread id, [0x34] last error.
 * ====================================================================== */
#define TEB_AREA W32_TEB_AREA
static __thread CPU *tls_cpu;
static __thread int tls_slot;       /* this thread's slot in threads.c, 0 = main */

static void init_cpu(CPU *c, uint32_t teb, uint32_t stack_top, uint32_t stack_size) {
    uint32_t sp = stack_top - 16;
    wr32(teb + 0x00, 0xFFFFFFFFu);
    wr32(teb + 0x04, stack_top);
    wr32(teb + 0x08, stack_top - stack_size);
    wr32(teb + 0x18, teb);
    wr32(teb + 0x20, GetCurrentProcessId());
    wr32(teb + 0x24, GetCurrentThreadId());
    c->fs_base = teb;
    c->esp = sp;
    c->cw = 0x027F;   /* the x87 control word Windows gives a new thread */
}

CPU *w32_cpu(void) { return tls_cpu; }

CPU *w32_main_cpu(void) {
    w32_slots_init();
    heap_init();
    CPU *c = (CPU *)calloc(1, sizeof *c);
    uint32_t teb = w32_slot_teb(0);
    rc_commit(teb, 0x1000);
    uint32_t sp = rc_alloc_stack(0x100000u);
    if (!sp) { fprintf(stderr, "w32: out of guest stack space\n"); abort(); }
    init_cpu(c, teb, sp + 16, 0x100000u);
    tls_cpu = c;
    tls_slot = 0;
    return c;
}

void w32_set_last_error(uint32_t e) { if (tls_cpu) wr32(tls_cpu->fs_base + 0x34, e); }

/* Ends the guest side of the calling thread: its slot (TEB, stack) can be reused. */
static void end_guest_thread(void) {
    if (tls_slot > 0) {
        w32_slot_release(tls_slot);
        free(tls_cpu);
        tls_cpu = NULL;
        tls_slot = 0;
    }
}

typedef struct { uint32_t start, param, teb, stack_top; int slot; } thread_start;

static DWORD WINAPI thread_main(LPVOID p) {
    thread_start ts = *(thread_start *)p;
    free(p);
    CPU *c = (CPU *)calloc(1, sizeof *c);
    init_cpu(c, ts.teb, ts.stack_top, W32_THREAD_STACK);
    tls_cpu = c;
    tls_slot = ts.slot;
    uint32_t r = rc_invoke(c, ts.start, 1, &ts.param);
    end_guest_thread();
    return r;
}

static void k_CreateThread(CPU *c) {
    uint32_t start = ARG(2), param = ARG(3), flags = ARG(4), tid = ARG(5);
    thread_start *ts = (thread_start *)malloc(sizeof *ts);
    ts->start = start;
    ts->param = param;
    /* The slot is taken here, not in the new thread: a failure is the caller's
       NULL return, as on Windows, and a suspended thread already owns its stack. */
    ts->slot = w32_slot_acquire(&ts->teb, &ts->stack_top);
    if (ts->slot < 0) {
        fprintf(stderr, "w32: CreateThread: all %d guest thread slots are in use\n", W32_THREAD_SLOTS - 1);
        free(ts);
        w32_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        RET(6, 0);
    }
    DWORD id;
    HANDLE h = CreateThread(NULL, 0x100000, thread_main, ts, flags & CREATE_SUSPENDED, &id);
    if (!h) { w32_slot_release(ts->slot); free(ts); RET(6, 0); }
    wr32(ts->teb + 0x24, id);   /* known before the thread runs (TerminateThread of a suspended one) */
    if (tid) wr32(tid, id);
    W32_LOG("CreateThread %08x -> %u (slot %d, %d in use)", start, (unsigned)id, ts->slot, w32_slots_in_use());
    RET(6, KHG(h));
}
static void k_ExitThread(CPU *c) {
    uint32_t code = ARG(0);
    end_guest_thread();      /* runs on the host stack: the guest stack is no longer needed */
    ExitThread(code);
}
static void k_ResumeThread(CPU *c) { RET(1, ResumeThread(KH(ARG(0)))); }
static void k_SuspendThread(CPU *c) { RET(1, SuspendThread(KH(ARG(0)))); }
static void k_TerminateThread(CPU *c) {
    HANDLE h = KH(ARG(0));
    if (!h) { w32_set_last_error(ERROR_INVALID_HANDLE); RET(2, 0); }
    int slot = w32_slot_of_thread(GetThreadId(h));
    BOOL ok = TerminateThread(h, ARG(1));
    /* TerminateThread is asynchronous: give the slot back once the thread is gone. */
    if (ok && slot > 0 && WaitForSingleObject(h, 5000) == WAIT_OBJECT_0) w32_slot_release(slot);
    RET(2, ok);
}
static void k_SetThreadPriority(CPU *c) { RET(2, SetThreadPriority(KH(ARG(0)), (int)ARG(1))); }
static void k_GetCurrentThread(CPU *c) { RET(0, 0xFFFFFFFEu); }
static void k_GetCurrentProcess(CPU *c) { RET(0, 0xFFFFFFFFu); }
static void k_CreateMutexA(CPU *c) { RET(3, KHG(CreateMutexA(NULL, ARG(1), GS(ARG(2))))); }
static void k_ReleaseMutex(CPU *c) { HANDLE h = KH(ARG(0)); if (!h) { w32_set_last_error(ERROR_INVALID_HANDLE); RET(1, 0); } RET(1, ReleaseMutex(h)); }
static void k_WaitForSingleObject(CPU *c) { HANDLE h = KH(ARG(0)); if (!h) { w32_set_last_error(ERROR_INVALID_HANDLE); RET(2, WAIT_FAILED); } RET(2, WaitForSingleObject(h, ARG(1))); }
static void k_CloseHandle(CPU *c) { HANDLE h = KH_take(ARG(0)); if (!h) { w32_set_last_error(ERROR_INVALID_HANDLE); RET(1, 0); } RET(1, CloseHandle(h)); }
static void k_Sleep(CPU *c) {
    uint32_t ms = ARG(0);
    if (tls_cpu && GetCurrentThreadId() == rd32(TEB_AREA + 0x24)) w32_pump_host_events();
    Sleep(ms);
    RET(1, 0);
}
static void k_InterlockedIncrement(CPU *c) { RET(1, (uint32_t)__atomic_add_fetch((int32_t *)G(ARG(0)), 1, __ATOMIC_SEQ_CST)); }
static void k_InterlockedDecrement(CPU *c) { RET(1, (uint32_t)__atomic_sub_fetch((int32_t *)G(ARG(0)), 1, __ATOMIC_SEQ_CST)); }

/* ======================================================================
 * Process environment.
 * ====================================================================== */
static uint32_t g_cmdline, g_env_a, g_env_w;

static void k_GetCommandLineA(CPU *c) {
    if (!g_cmdline) {
        char buf[MAX_PATH * 2];
        uint32_t exe = w32_exe_base();
        snprintf(buf, sizeof buf, "\"%s\\%s\"", w32_game_dir(), rc_module_table[w32_module_index(exe)].name);
        g_cmdline = w32_guest_strdup(buf);
    }
    RET(0, g_cmdline);
}
static void k_GetEnvironmentStrings(CPU *c) {
    if (!g_env_a) { g_env_a = w32_heap_alloc(4, 1); }
    RET(0, g_env_a);
}
static void k_GetEnvironmentStringsW(CPU *c) {
    if (!g_env_w) { g_env_w = w32_heap_alloc(4, 1); }
    RET(0, g_env_w);
}
static void k_FreeEnvironmentStrings(CPU *c) { RET(1, 1); }
static void k_GetStartupInfoA(CPU *c) {
    uint32_t p = ARG(0);
    memset(G(p), 0, 68);
    wr32(p, 68);
    RET(1, 0);
}
static void k_GetStdHandle(CPU *c) { RET(1, KHG(GetStdHandle(ARG(0)))); }
static void k_GetFileType(CPU *c) { RET(1, GetFileType(KH(ARG(0)))); }
static void k_WriteFile(CPU *c) {
    DWORD n = 0;
    BOOL ok = WriteFile(KH(ARG(0)), G(ARG(1)), ARG(2), &n, NULL);
    if (ARG(3)) wr32(ARG(3), n);
    RET(5, ok);
}
static void k_SetHandleCount(CPU *c) { RET(1, ARG(0)); }
/* Windows XP (5.1, build 2600). On NT 5+ the VC6 CRT gives every allocation to
   the system heap (HeapAlloc) instead of its own small-block heap; the games rely
   on freed objects staying intact for a moment (Aaron dispatches the message queues
   of a scene it has just deleted), which holds on the Windows heap (and on ours,
   see the quarantine) but not with the CRT's small-block heap, which reuses the
   memory at once. */
static void k_GetVersion(CPU *c) { RET(0, 0x0A280105u); }
static void k_GetSystemInfo(CPU *c) {
    uint32_t p = ARG(0);
    memset(G(p), 0, 36);
    wr32(p + 4, 0x1000);         /* dwPageSize */
    wr32(p + 8, 0x10000);        /* lpMinimumApplicationAddress */
    wr32(p + 12, 0x7FFEFFFF);    /* lpMaximumApplicationAddress */
    wr32(p + 16, 1);             /* dwActiveProcessorMask */
    wr32(p + 20, 1);             /* dwNumberOfProcessors */
    wr32(p + 24, 586);           /* dwProcessorType */
    wr32(p + 28, 0x10000);       /* dwAllocationGranularity */
    wr16(p + 32, 6);             /* wProcessorLevel */
    RET(1, 0);
}
static void k_GetLastError(CPU *c) { RET(0, rd32(c->fs_base + 0x34)); }
static void k_SetUnhandledExceptionFilter(CPU *c) { RET(1, 0); }
static void k_UnhandledExceptionFilter(CPU *c) { RET(1, 1); }
static void k_RaiseException(CPU *c) { rc_fault(c, rd32(c->esp), "RaiseException (no guest exception support)"); }
static void k_RtlUnwind(CPU *c) { rc_fault(c, rd32(c->esp), "RtlUnwind (no guest exception support)"); }
static void k_IsBadPtr(CPU *c) { RET(2, 0); }
static void k_IsBadCodePtr(CPU *c) { RET(1, 0); }
static void k_OutputDebugStringA(CPU *c) { W32_LOG("debug: %s", GS(ARG(0))); RET(1, 0); }
static void k_DisableThreadLibraryCalls(CPU *c) { RET(1, 1); }
static void k_ExitProcess(CPU *c) { fflush(NULL); ExitProcess(ARG(0)); }
static void k_TerminateProcess(CPU *c) { fflush(NULL); ExitProcess(ARG(1)); }

/* ======================================================================
 * Modules.
 * ====================================================================== */
uint32_t w32_load_library(const char *name);
static void k_GetModuleHandleA(CPU *c) { RET(1, w32_module_handle(GS(ARG(0)))); }
static void k_GetModuleFileNameA(CPU *c) {
    uint32_t h = ARG(0), buf = ARG(1), size = ARG(2);
    int k = w32_module_index(h ? h : w32_exe_base());
    char path[MAX_PATH * 2];
    snprintf(path, sizeof path, "%s\\%s", w32_game_dir(), k >= 0 ? rc_module_table[k].name : "unknown");
    uint32_t n = (uint32_t)strlen(path);
    if (!size) RET(3, 0);
    if (n >= size) n = size - 1;
    memcpy(G(buf), path, n);
    ((char *)G(buf))[n] = 0;
    RET(3, n);
}
static void k_LoadLibraryA(CPU *c) { RET(1, w32_load_library(GS(ARG(0)))); }
static void k_FreeLibrary(CPU *c) { RET(1, 1); }
static void k_GetProcAddress(CPU *c) {
    uint32_t h = ARG(0), n = ARG(1);
    uint32_t r = n < 0x10000u ? w32_get_proc(h, NULL, n) : w32_get_proc(h, GS(n), 0);
    W32_LOG("GetProcAddress %08x %s -> %08x", h, n < 0x10000u ? "#" : GS(n), r);
    RET(2, r);
}

/* ======================================================================
 * Strings, locale, files: forwarded to the host.
 * ====================================================================== */
static void k_GetACP(CPU *c) { RET(0, GetACP()); }
static void k_GetOEMCP(CPU *c) { RET(0, GetOEMCP()); }
static void k_GetCPInfo(CPU *c) { RET(2, GetCPInfo(ARG(0), (LPCPINFO)G(ARG(1)))); }
static void k_MultiByteToWideChar(CPU *c) {
    RET(6, MultiByteToWideChar(ARG(0), ARG(1), GS(ARG(2)), (int)ARG(3), (LPWSTR)GP(ARG(4)), (int)ARG(5)));
}
static void k_WideCharToMultiByte(CPU *c) {
    RET(8, WideCharToMultiByte(ARG(0), ARG(1), (LPCWSTR)GP(ARG(2)), (int)ARG(3), (LPSTR)GP(ARG(4)), (int)ARG(5),
                               GS(ARG(6)), (LPBOOL)GP(ARG(7))));
}
static void k_LCMapStringA(CPU *c) {
    RET(6, LCMapStringA(ARG(0), ARG(1), GS(ARG(2)), (int)ARG(3), (LPSTR)GP(ARG(4)), (int)ARG(5)));
}
static void k_LCMapStringW(CPU *c) {
    RET(6, LCMapStringW(ARG(0), ARG(1), (LPCWSTR)GP(ARG(2)), (int)ARG(3), (LPWSTR)GP(ARG(4)), (int)ARG(5)));
}
static void k_GetStringTypeA(CPU *c) {
    RET(5, GetStringTypeA(ARG(0), ARG(1), GS(ARG(2)), (int)ARG(3), (LPWORD)GP(ARG(4))));
}
static void k_GetStringTypeW(CPU *c) {
    RET(4, GetStringTypeW(ARG(0), (LPCWSTR)GP(ARG(1)), (int)ARG(2), (LPWORD)GP(ARG(3))));
}
static void k_lstrlenA(CPU *c) { RET(1, ARG(0) ? strlen((const char *)G(ARG(0))) : 0); }
static void k_lstrcpyA(CPU *c) {
    const char *src = (const char *)G(ARG(1));
    memmove(G(ARG(0)), src, strlen(src) + 1);   /* guest buffers: memmove, as overlap is possible */
    RET(2, ARG(0));
}
static void k_lstrcpynA(CPU *c) { lstrcpynA((char *)G(ARG(0)), GS(ARG(1)), (int)ARG(2)); RET(3, ARG(0)); }
static void k_lstrcatA(CPU *c) {
    char *dst = (char *)G(ARG(0));
    const char *src = (const char *)G(ARG(1));
    memmove(dst + strlen(dst), src, strlen(src) + 1);
    RET(2, ARG(0));
}
static void k_lstrcmpA(CPU *c) { RET(2, (uint32_t)lstrcmpA(GS(ARG(0)), GS(ARG(1)))); }
static void k_lstrcmpiA(CPU *c) { RET(2, (uint32_t)lstrcmpiA(GS(ARG(0)), GS(ARG(1)))); }

static void k_GetCurrentDirectoryA(CPU *c) { RET(2, GetCurrentDirectoryA(ARG(0), (LPSTR)GP(ARG(1)))); }
static void k_GetFullPathNameA(CPU *c) {
    LPSTR part = NULL;
    uint32_t buf = ARG(2);
    DWORD r = GetFullPathNameA(GS(ARG(0)), ARG(1), (LPSTR)GP(buf), &part);
    if (ARG(3)) wr32(ARG(3), part ? (uint32_t)((uint8_t *)part - rc_mem) : 0);
    RET(4, r);
}
static void k_GetDriveTypeA(CPU *c) { RET(1, GetDriveTypeA(GS(ARG(0)))); }
static void k_FindFirstFileA(CPU *c) {
    HANDLE h = FindFirstFileA(GS(ARG(0)), (LPWIN32_FIND_DATAA)G(ARG(1)));
    RET(2, h == INVALID_HANDLE_VALUE ? 0xFFFFFFFFu : KHG(h));
}
static void k_FindClose(CPU *c) { HANDLE h = KH_take(ARG(0)); RET(1, h ? FindClose(h) : 0); }
static void k_FileTimeToLocalFileTime(CPU *c) { RET(2, FileTimeToLocalFileTime((FILETIME *)G(ARG(0)), (FILETIME *)G(ARG(1)))); }
static void k_FileTimeToSystemTime(CPU *c) { RET(2, FileTimeToSystemTime((FILETIME *)G(ARG(0)), (SYSTEMTIME *)G(ARG(1)))); }
static void k_GetDateFormatA(CPU *c) {
    RET(6, GetDateFormatA(ARG(0), ARG(1), (SYSTEMTIME *)GP(ARG(2)), GS(ARG(3)), (LPSTR)GP(ARG(4)), (int)ARG(5)));
}
static void k_GetTimeFormatA(CPU *c) {
    RET(6, GetTimeFormatA(ARG(0), ARG(1), (SYSTEMTIME *)GP(ARG(2)), GS(ARG(3)), (LPSTR)GP(ARG(4)), (int)ARG(5)));
}

/* rapture.ini: a relative name is looked up in the current directory (the game
   folder), not in %WINDIR% as GetPrivateProfile* would do. */
static const char *ini_path(const char *name, char *buf, size_t n) {
    if (!name) return NULL;
    if (strchr(name, '\\') || strchr(name, '/') || strchr(name, ':')) return name;
    snprintf(buf, n, ".\\%s", name);
    return buf;
}
static void k_GetPrivateProfileIntA(CPU *c) {
    char p[MAX_PATH];
    RET(4, GetPrivateProfileIntA(GS(ARG(0)), GS(ARG(1)), (INT)ARG(2), ini_path(GS(ARG(3)), p, sizeof p)));
}
static void k_WritePrivateProfileStringA(CPU *c) {
    char p[MAX_PATH];
    RET(4, WritePrivateProfileStringA(GS(ARG(0)), GS(ARG(1)), GS(ARG(2)), ini_path(GS(ARG(3)), p, sizeof p)));
}

#define K(n, f) {"kernel32.dll", n, f}
const rc_hle_entry w32_kernel32_table[] = {
    K("HeapCreate", k_HeapCreate), K("HeapDestroy", k_HeapDestroy), K("HeapAlloc", k_HeapAlloc),
    K("HeapFree", k_HeapFree), K("HeapSize", k_HeapSize), K("HeapReAlloc", k_HeapReAlloc),
    K("HeapCompact", k_HeapCompact), K("VirtualAlloc", k_VirtualAlloc), K("VirtualFree", k_VirtualFree),
    K("VirtualQuery", k_VirtualQuery),
    K("CreateThread", k_CreateThread), K("ExitThread", k_ExitThread), K("ResumeThread", k_ResumeThread),
    K("SuspendThread", k_SuspendThread), K("TerminateThread", k_TerminateThread),
    K("SetThreadPriority", k_SetThreadPriority), K("GetCurrentThread", k_GetCurrentThread),
    K("GetCurrentProcess", k_GetCurrentProcess), K("CreateMutexA", k_CreateMutexA),
    K("ReleaseMutex", k_ReleaseMutex), K("WaitForSingleObject", k_WaitForSingleObject),
    K("CloseHandle", k_CloseHandle), K("Sleep", k_Sleep),
    K("InterlockedIncrement", k_InterlockedIncrement), K("InterlockedDecrement", k_InterlockedDecrement),
    K("GetCommandLineA", k_GetCommandLineA), K("GetEnvironmentStrings", k_GetEnvironmentStrings),
    K("GetEnvironmentStringsW", k_GetEnvironmentStringsW),
    K("FreeEnvironmentStringsA", k_FreeEnvironmentStrings), K("FreeEnvironmentStringsW", k_FreeEnvironmentStrings),
    K("GetStartupInfoA", k_GetStartupInfoA), K("GetStdHandle", k_GetStdHandle), K("GetFileType", k_GetFileType),
    K("WriteFile", k_WriteFile), K("SetHandleCount", k_SetHandleCount), K("GetVersion", k_GetVersion),
    K("GetSystemInfo", k_GetSystemInfo), K("GetLastError", k_GetLastError),
    K("SetUnhandledExceptionFilter", k_SetUnhandledExceptionFilter),
    K("UnhandledExceptionFilter", k_UnhandledExceptionFilter), K("RaiseException", k_RaiseException),
    K("RtlUnwind", k_RtlUnwind), K("IsBadReadPtr", k_IsBadPtr), K("IsBadWritePtr", k_IsBadPtr),
    K("IsBadCodePtr", k_IsBadCodePtr), K("OutputDebugStringA", k_OutputDebugStringA),
    K("DisableThreadLibraryCalls", k_DisableThreadLibraryCalls), K("ExitProcess", k_ExitProcess),
    K("TerminateProcess", k_TerminateProcess),
    K("GetModuleHandleA", k_GetModuleHandleA), K("GetModuleFileNameA", k_GetModuleFileNameA),
    K("LoadLibraryA", k_LoadLibraryA), K("FreeLibrary", k_FreeLibrary), K("GetProcAddress", k_GetProcAddress),
    K("GetACP", k_GetACP), K("GetOEMCP", k_GetOEMCP), K("GetCPInfo", k_GetCPInfo),
    K("MultiByteToWideChar", k_MultiByteToWideChar), K("WideCharToMultiByte", k_WideCharToMultiByte),
    K("LCMapStringA", k_LCMapStringA), K("LCMapStringW", k_LCMapStringW),
    K("GetStringTypeA", k_GetStringTypeA), K("GetStringTypeW", k_GetStringTypeW),
    K("lstrlenA", k_lstrlenA), K("lstrcpyA", k_lstrcpyA), K("lstrcpynA", k_lstrcpynA),
    K("lstrcatA", k_lstrcatA), K("lstrcmpA", k_lstrcmpA), K("lstrcmpiA", k_lstrcmpiA),
    K("GetCurrentDirectoryA", k_GetCurrentDirectoryA), K("GetFullPathNameA", k_GetFullPathNameA),
    K("GetDriveTypeA", k_GetDriveTypeA), K("FindFirstFileA", k_FindFirstFileA), K("FindClose", k_FindClose),
    K("FileTimeToLocalFileTime", k_FileTimeToLocalFileTime), K("FileTimeToSystemTime", k_FileTimeToSystemTime),
    K("GetDateFormatA", k_GetDateFormatA), K("GetTimeFormatA", k_GetTimeFormatA),
    K("GetPrivateProfileIntA", k_GetPrivateProfileIntA),
    K("WritePrivateProfileStringA", k_WritePrivateProfileStringA),
    {NULL, NULL, NULL}
};
