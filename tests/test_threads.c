/* Guest threads: creates and ends many threads through the Win32 layer's own
 * CreateThread / ExitThread / TerminateThread / WaitForSingleObject /
 * CloseHandle, and checks that every thread gets its own TEB and stack and that
 * both are given back (a game creates threads all its life: MuShoot one per
 * sound, movie and dialog).
 *
 * The thread routines are host functions behind guest addresses (host thunks),
 * so no game code is needed; the test is linked with the whole Win32 layer.
 * Usage: test_threads   (exit code 0 = pass)
 */
#include "w32.h"

#include <windows.h>

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static CPU *mc;          /* main thread CPU */
static rc_fn f_create, f_exit, f_terminate, f_wait, f_close;

/* Calls an HLE function from the main thread as guest code would (stdcall). */
static uint32_t call(rc_fn fn, int argc, const uint32_t *argv) {
    uint32_t sp0 = mc->esp;
    for (int k = argc - 1; k >= 0; k--) { mc->esp -= 4; wr32(mc->esp, argv[k]); }
    mc->esp -= 4; wr32(mc->esp, 0xFFFFFFF0u);
    fn(mc);
    CHECK(mc->esp == sp0, "stdcall imbalance");
    mc->esp = sp0;
    return mc->eax;
}

/* -- thread routines (guest stdcall DWORD f(LPVOID param)) ------------- */
enum { MODE_RETURN, MODE_EXIT, MODE_BLOCK, MODE_SPIN };
static volatile LONG running, max_running;
static HANDLE release_event;
static volatile LONG stack_errors;

/* Each thread fills a stretch of its own stack with its id and checks it back
   after the others ran: two threads sharing a stack would overwrite it. */
static void use_stack(CPU *c, uint32_t id) {
    uint32_t lo = rd32(c->fs_base + 0x08), hi = rd32(c->fs_base + 0x04);
    if (!(c->esp > lo && c->esp <= hi) || hi - lo != W32_THREAD_STACK || rd32(c->fs_base + 0x18) != c->fs_base)
        InterlockedIncrement(&stack_errors);
    uint32_t base = c->esp - 0x8000u;
    for (uint32_t a = base; a < c->esp - 64; a += 4) wr32(a, id ^ a);
    Sleep(0);
    for (uint32_t a = base; a < c->esp - 64; a += 4)
        if (rd32(a) != (id ^ a)) { InterlockedIncrement(&stack_errors); break; }
}

static void thread_routine(CPU *c) {
    uint32_t param = ARG(0);
    uint32_t mode = param >> 24, id = param & 0xFFFFFFu;
    LONG n = InterlockedIncrement(&running);
    for (LONG m = max_running; n > m && InterlockedCompareExchange(&max_running, n, m) != m; m = max_running) {}
    use_stack(c, id);
    if (mode == MODE_BLOCK) WaitForSingleObject(release_event, INFINITE);
    if (mode == MODE_SPIN) for (;;) Sleep(1);           /* only TerminateThread ends it */
    use_stack(c, id);
    InterlockedDecrement(&running);
    if (mode == MODE_EXIT) {
        /* ExitThread(id) the way guest code calls it: push arg, push return address. */
        c->esp -= 4; wr32(c->esp, id);
        c->esp -= 4; wr32(c->esp, 0xFFFFFFF0u);
        f_exit(c);
        InterlockedIncrement(&stack_errors);             /* not reached */
    }
    RET(1, id);
}

static uint32_t start_addr;

static uint32_t create(uint32_t mode, uint32_t id, uint32_t *tid) {
    uint32_t a[6] = {0, 0, start_addr, (mode << 24) | id, 0, 0};
    uint32_t tid_slot = mc->esp - 0x100;       /* scratch on the main guest stack, below the args */
    a[5] = tid_slot;
    uint32_t h = call(f_create, 6, a);
    if (tid) *tid = rd32(tid_slot);
    return h;
}
static uint32_t wait_h(uint32_t h, uint32_t ms) { uint32_t a[2] = {h, ms}; return call(f_wait, 2, a); }
static uint32_t close_h(uint32_t h) { uint32_t a[1] = {h}; return call(f_close, 1, a); }

/* Slot release happens on the thread after its routine returns: poll briefly. */
static int settle(int want) {
    for (int k = 0; k < 2000 && w32_slots_in_use() != want; k++) Sleep(1);
    return w32_slots_in_use();
}

int main(void) {
    if (rc_init()) { fprintf(stderr, "cannot reserve the guest address space\n"); return 2; }
    mc = w32_main_cpu();
    f_create = rc_hle_find("kernel32.dll", "CreateThread");
    f_exit = rc_hle_find("kernel32.dll", "ExitThread");
    f_terminate = rc_hle_find("kernel32.dll", "TerminateThread");
    f_wait = rc_hle_find("kernel32.dll", "WaitForSingleObject");
    f_close = rc_hle_find("kernel32.dll", "CloseHandle");
    if (!f_create || !f_exit || !f_terminate || !f_wait || !f_close) { fprintf(stderr, "HLE functions missing\n"); return 2; }
    start_addr = rc_host_thunk(thread_routine, "test_thread_routine");
    release_event = CreateEventA(NULL, TRUE, FALSE, NULL);

    /* 1. Many short threads, one after the other, half of them ending with ExitThread:
          far more than the slots, so slots must be reused. */
    const int SEQ = 1000;
    for (int i = 0; i < SEQ; i++) {
        uint32_t h = create(i & 1 ? MODE_EXIT : MODE_RETURN, (uint32_t)i, NULL);
        CHECK(h != 0, "sequential thread %d not created", i);
        if (!h) break;
        CHECK(wait_h(h, 10000) == WAIT_OBJECT_0, "sequential thread %d did not end", i);
        CHECK(close_h(h) == 1, "CloseHandle of thread %d", i);
    }
    CHECK(settle(0) == 0, "slots still in use after sequential threads: %d", w32_slots_in_use());

    /* 2. Batches of concurrent threads, each holding its stack until released. */
    const int BATCH = W32_THREAD_SLOTS - 1;
    for (int round = 0; round < 20; round++) {
        uint32_t hs[W32_THREAD_SLOTS];
        ResetEvent(release_event);
        for (int i = 0; i < BATCH; i++) {
            hs[i] = create(MODE_BLOCK, (uint32_t)(round * 100 + i), NULL);
            CHECK(hs[i] != 0, "round %d: thread %d not created", round, i);
        }
        CHECK(w32_slots_in_use() == BATCH, "round %d: %d slots in use", round, w32_slots_in_use());
        if (round == 0) {
            /* all slots busy: CreateThread fails cleanly (NULL), as on Windows */
            uint32_t h = create(MODE_RETURN, 999999, NULL);
            CHECK(h == 0, "CreateThread with every slot busy returned %08x", h);
        }
        for (int k = 0; k < 2000 && running < BATCH; k++) Sleep(1);
        SetEvent(release_event);
        for (int i = 0; i < BATCH; i++)
            if (hs[i]) { CHECK(wait_h(hs[i], 10000) == WAIT_OBJECT_0, "round %d: thread %d did not end", round, i); close_h(hs[i]); }
        CHECK(settle(0) == 0, "round %d: slots still in use: %d", round, w32_slots_in_use());
    }
    CHECK(max_running == BATCH, "max concurrent threads %ld, expected %d", max_running, BATCH);

    /* 3. TerminateThread gives the slot back too. */
    for (int i = 0; i < 100; i++) {
        uint32_t h = create(MODE_SPIN, (uint32_t)i, NULL);
        CHECK(h != 0, "spinning thread %d not created", i);
        if (!h) break;
        for (int k = 0; k < 1000 && running == 0; k++) Sleep(1);
        uint32_t a[2] = {h, 7};
        CHECK(call(f_terminate, 2, a) == 1, "TerminateThread %d", i);
        CHECK(w32_slots_in_use() == 0, "slot of terminated thread %d not released", i);
        close_h(h);
        InterlockedExchange(&running, 0);
    }

    CHECK(stack_errors == 0, "%ld threads saw a wrong or shared stack", stack_errors);
    printf("test_threads: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
