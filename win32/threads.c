/* Guest thread slots: the TEB and the guest stack of every thread that runs
 * guest code, taken from a fixed pool and given back when the thread ends, so
 * a game can create any number of threads over its lifetime (MuShoot starts
 * one per sound, movie and dialog). Slot 0 is the main thread.
 *
 * Layout (guest addresses):
 *   0x7E000000 + i * 0x1000            TEB of slot i (slot 0: main thread)
 *   0x7E100000 + (i - 1) * 0x40000     256 KiB stack of slot i >= 1
 * The main thread's 1 MiB stack comes from rc_alloc_stack (low memory, as on
 * Windows). Only depends on the runtime, so tests/test_threads.c links it alone.
 */
#include "w32.h"

#include <windows.h>

#define SLOT_STACK_AREA 0x7E100000u

static CRITICAL_SECTION slot_lock;
static int slot_lock_init;            /* set once by w32_slots_init (main thread, before any thread) */
static uint8_t slot_used[W32_THREAD_SLOTS];
static uint8_t slot_committed[W32_THREAD_SLOTS];
static int slots_in_use;

void w32_slots_init(void) {
    if (slot_lock_init) return;
    InitializeCriticalSection(&slot_lock);
    slot_lock_init = 1;
}

uint32_t w32_slot_teb(int slot) { return W32_TEB_AREA + 0x1000u * (uint32_t)slot; }

int w32_slot_acquire(uint32_t *teb, uint32_t *stack_top) {
    int slot = -1;
    EnterCriticalSection(&slot_lock);
    for (int i = 1; i < W32_THREAD_SLOTS; i++)
        if (!slot_used[i]) { slot = i; break; }
    if (slot > 0) {
        uint32_t t = w32_slot_teb(slot);
        uint32_t lo = SLOT_STACK_AREA + W32_THREAD_STACK * (uint32_t)(slot - 1);
        if (!slot_committed[slot]) {
            if (rc_commit(t, 0x1000) || rc_commit(lo, W32_THREAD_STACK)) slot = -1;
            else slot_committed[slot] = 1;
        }
        if (slot > 0) {
            slot_used[slot] = 1;
            slots_in_use++;
            memset(rc_ptr(t), 0, 0x1000);     /* a reused TEB starts clean */
            *teb = t;
            *stack_top = lo + W32_THREAD_STACK;
        }
    }
    LeaveCriticalSection(&slot_lock);
    return slot;
}

void w32_slot_release(int slot) {
    if (slot <= 0 || slot >= W32_THREAD_SLOTS) return;
    EnterCriticalSection(&slot_lock);
    if (slot_used[slot]) { slot_used[slot] = 0; slots_in_use--; }
    LeaveCriticalSection(&slot_lock);
}

int w32_slot_of_thread(uint32_t tid) {
    int found = -1;
    EnterCriticalSection(&slot_lock);
    for (int i = 1; i < W32_THREAD_SLOTS; i++)
        if (slot_used[i] && rd32(w32_slot_teb(i) + 0x24) == tid) { found = i; break; }
    LeaveCriticalSection(&slot_lock);
    return found;
}

int w32_slots_in_use(void) {
    EnterCriticalSection(&slot_lock);
    int n = slots_in_use;
    LeaveCriticalSection(&slot_lock);
    return n;
}
