// win32run - threads, waiting, synchronization objects, time, and pipes.
//
// silicon threads are scheduled preemptively but there is no kernel wait queue a user program can sleep
// on, so every wait here is the same loop: check, yield, and after a while sleep a millisecond between
// checks. Waits that end quickly cost a few yields; long waits cost almost nothing.
#include "rt.h"

namespace rt {

static Lock sync_lock;                 // guards the state of every waitable object
enum { WAIT_OBJECT_0 = 0, WAIT_ABANDONED_0 = 0x80, WAIT_TIMEOUT_ = 258, WAIT_FAILED = 0xFFFFFFFF, WAIT_IO_COMPLETION = 0xC0 };
static const u32 INFINITE = 0xFFFFFFFF;

struct ThreadObj : Obj { Thr* t; };
struct EventObj : Obj { bool manual, set; };
struct MutexObj : Obj { u32 owner; int count; };
struct SemObj : Obj { i32 count, max; };
struct TimerObj : Obj { u64 due_ms; u32 period; bool manual, set; };

static void plain_destroy(Obj* o) { ifree(o); }
static void thread_destroy(Obj* o) { ThreadObj* t = (ThreadObj*)o; if (t->t) { t->t->obj = nullptr; thr_release(t->t); } ifree(o); }

Obj* thread_obj(Thr* t) {
    if (t->obj) return t->obj;
    ThreadObj* o = (ThreadObj*)izalloc(sizeof(ThreadObj));
    o->type = OBJ_THREAD; o->refs = 1; o->destroy = thread_destroy;     // the thread's own reference: never released while it exists
    o->t = t;
    __sync_fetch_and_add(&t->refs, 1);
    t->obj = o;
    return o;
}

// ---------------------------------------------------------------- waiting
static bool signaled(Obj* o, u32 me) {
    switch (o->type) {
    case OBJ_THREAD: return ((ThreadObj*)o)->t->done != 0;
    case OBJ_EVENT: return ((EventObj*)o)->set;
    case OBJ_MUTEX: { MutexObj* m = (MutexObj*)o; return !m->owner || m->owner == me; }
    case OBJ_SEMAPHORE: return ((SemObj*)o)->count > 0;
    case OBJ_TIMER: { TimerObj* t = (TimerObj*)o; if (!t->set && t->due_ms && now_ms() >= t->due_ms) t->set = true; return t->set; }
    case OBJ_PROCESS: return false;
    case OBJ_CONSOLE: case OBJ_FILE: case OBJ_NUL: case OBJ_PIPE: return true;
    default: return false;
    }
}

static void consume(Obj* o, u32 me) {
    switch (o->type) {
    case OBJ_EVENT: if (!((EventObj*)o)->manual) ((EventObj*)o)->set = false; break;
    case OBJ_MUTEX: { MutexObj* m = (MutexObj*)o; m->owner = me; m->count++; break; }
    case OBJ_SEMAPHORE: ((SemObj*)o)->count--; break;
    case OBJ_TIMER: {
        TimerObj* t = (TimerObj*)o;
        if (t->period) { t->due_ms = now_ms() + t->period; t->set = false; }
        else if (!t->manual) t->set = false;
        break;
    }
    default: break;
    }
}

static u32 wait_objects(u32 n, Obj** objs, bool all, u32 timeout) {
    u32 me = cur_thr()->id;
    u64 end = timeout == INFINITE ? ~0ull : now_ms() + timeout;
    for (u32 spins = 0;; spins++) {
        {
            Guard g(sync_lock);
            if (all) {
                bool ok = true;
                for (u32 i = 0; i < n; i++) if (!signaled(objs[i], me)) { ok = false; break; }
                if (ok) { for (u32 i = 0; i < n; i++) consume(objs[i], me); return WAIT_OBJECT_0; }
            } else {
                for (u32 i = 0; i < n; i++)
                    if (signaled(objs[i], me)) { consume(objs[i], me); return WAIT_OBJECT_0 + i; }
            }
        }
        if (now_ms() >= end) return WAIT_TIMEOUT_;
        nap(spins);
    }
}

static u32 WINAPI WaitForMultipleObjectsEx(u32 n, const HANDLE* hs, BOOL all, u32 timeout, BOOL alertable) {
    (void)alertable;
    Obj* objs[64];
    if (!n || n > 64) { set_error(ERROR_INVALID_PARAMETER); return WAIT_FAILED; }
    for (u32 i = 0; i < n; i++) if (!(objs[i] = h_get(hs[i]))) return WAIT_FAILED;
    return wait_objects(n, objs, all != 0, timeout);
}
static u32 WINAPI WaitForMultipleObjects(u32 n, const HANDLE* hs, BOOL all, u32 timeout) { return WaitForMultipleObjectsEx(n, hs, all, timeout, FALSE); }
static u32 WINAPI WaitForSingleObjectEx(HANDLE h, u32 timeout, BOOL alertable) { return WaitForMultipleObjectsEx(1, &h, FALSE, timeout, alertable); }
static u32 WINAPI WaitForSingleObject(HANDLE h, u32 timeout) { return WaitForMultipleObjectsEx(1, &h, FALSE, timeout, FALSE); }
static u32 WINAPI SignalObjectAndWait(HANDLE sig, HANDLE w, u32 timeout, BOOL alertable) {
    extern BOOL WINAPI rt_SetEvent(HANDLE);
    Obj* o = h_get(sig);
    if (!o) return WAIT_FAILED;
    if (o->type == OBJ_EVENT) rt_SetEvent(sig);
    else if (o->type == OBJ_MUTEX) { Guard g(sync_lock); MutexObj* m = (MutexObj*)o; if (m->count && !--m->count) m->owner = 0; }
    else if (o->type == OBJ_SEMAPHORE) { Guard g(sync_lock); ((SemObj*)o)->count++; }
    return WaitForSingleObjectEx(w, timeout, alertable);
}

// ---------------------------------------------------------------- events, mutexes, semaphores, timers
static HANDLE new_event(bool manual, bool initial) {
    EventObj* e = (EventObj*)izalloc(sizeof(EventObj));
    e->type = OBJ_EVENT; e->refs = 1; e->destroy = plain_destroy;
    e->manual = manual; e->set = initial;
    return h_new(e);
}
static HANDLE WINAPI CreateEventA(SECURITY_ATTRIBUTES* sa, BOOL manual, BOOL initial, const char* name) { (void)sa; (void)name; return new_event(manual, initial); }
static HANDLE WINAPI CreateEventW(SECURITY_ATTRIBUTES* sa, BOOL manual, BOOL initial, const WCHAR* name) { (void)sa; (void)name; return new_event(manual, initial); }
static HANDLE WINAPI CreateEventExW(SECURITY_ATTRIBUTES* sa, const WCHAR* name, u32 flags, u32 access) { (void)sa; (void)name; (void)access; return new_event(flags & 1, flags & 2); }
static HANDLE WINAPI CreateEventExA(SECURITY_ATTRIBUTES* sa, const char* name, u32 flags, u32 access) { (void)sa; (void)name; (void)access; return new_event(flags & 1, flags & 2); }
static HANDLE WINAPI OpenEventW(u32 a, BOOL i, const WCHAR* n) { (void)a; (void)i; (void)n; set_error(ERROR_FILE_NOT_FOUND); return nullptr; }
BOOL WINAPI rt_SetEvent(HANDLE h) { EventObj* e = (EventObj*)h_get(h, OBJ_EVENT); if (!e) return FALSE; Guard g(sync_lock); e->set = true; return TRUE; }
static BOOL WINAPI ResetEvent(HANDLE h) { EventObj* e = (EventObj*)h_get(h, OBJ_EVENT); if (!e) return FALSE; Guard g(sync_lock); e->set = false; return TRUE; }
static BOOL WINAPI PulseEvent(HANDLE h) { return rt_SetEvent(h); }

static HANDLE new_mutex(bool owned) {
    MutexObj* m = (MutexObj*)izalloc(sizeof(MutexObj));
    m->type = OBJ_MUTEX; m->refs = 1; m->destroy = plain_destroy;
    if (owned) { m->owner = cur_thr()->id; m->count = 1; }
    return h_new(m);
}
static HANDLE WINAPI CreateMutexA(SECURITY_ATTRIBUTES* sa, BOOL owned, const char* name) { (void)sa; (void)name; return new_mutex(owned); }
static HANDLE WINAPI CreateMutexW(SECURITY_ATTRIBUTES* sa, BOOL owned, const WCHAR* name) { (void)sa; (void)name; return new_mutex(owned); }
static HANDLE WINAPI CreateMutexExW(SECURITY_ATTRIBUTES* sa, const WCHAR* name, u32 flags, u32 access) { (void)sa; (void)name; (void)access; return new_mutex(flags & 1); }
static HANDLE WINAPI OpenMutexW(u32 a, BOOL i, const WCHAR* n) { (void)a; (void)i; (void)n; set_error(ERROR_FILE_NOT_FOUND); return nullptr; }
static BOOL WINAPI ReleaseMutex(HANDLE h) {
    MutexObj* m = (MutexObj*)h_get(h, OBJ_MUTEX);
    if (!m) return FALSE;
    Guard g(sync_lock);
    if (m->owner != cur_thr()->id) { set_error(ERROR_NOT_OWNER); return FALSE; }
    if (!--m->count) m->owner = 0;
    return TRUE;
}

static HANDLE new_sem(i32 initial, i32 max) {
    if (max <= 0 || initial < 0 || initial > max) { set_error(ERROR_INVALID_PARAMETER); return nullptr; }
    SemObj* s = (SemObj*)izalloc(sizeof(SemObj));
    s->type = OBJ_SEMAPHORE; s->refs = 1; s->destroy = plain_destroy;
    s->count = initial; s->max = max;
    return h_new(s);
}
static HANDLE WINAPI CreateSemaphoreA(SECURITY_ATTRIBUTES* sa, i32 init, i32 max, const char* name) { (void)sa; (void)name; return new_sem(init, max); }
static HANDLE WINAPI CreateSemaphoreW(SECURITY_ATTRIBUTES* sa, i32 init, i32 max, const WCHAR* name) { (void)sa; (void)name; return new_sem(init, max); }
static HANDLE WINAPI CreateSemaphoreExW(SECURITY_ATTRIBUTES* sa, i32 init, i32 max, const WCHAR* name, u32 f, u32 a) { (void)sa; (void)name; (void)f; (void)a; return new_sem(init, max); }
static BOOL WINAPI ReleaseSemaphore(HANDLE h, i32 n, i32* prev) {
    SemObj* s = (SemObj*)h_get(h, OBJ_SEMAPHORE);
    if (!s) return FALSE;
    Guard g(sync_lock);
    if (n <= 0 || s->count + n > s->max) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }   // ERROR_TOO_MANY_POSTS on Windows
    if (prev) *prev = s->count;
    s->count += n;
    return TRUE;
}

static HANDLE WINAPI CreateWaitableTimerW(SECURITY_ATTRIBUTES* sa, BOOL manual, const WCHAR* name) {
    (void)sa; (void)name;
    TimerObj* t = (TimerObj*)izalloc(sizeof(TimerObj));
    t->type = OBJ_TIMER; t->refs = 1; t->destroy = plain_destroy;
    t->manual = manual;
    return h_new(t);
}
static HANDLE WINAPI CreateWaitableTimerExW(SECURITY_ATTRIBUTES* sa, const WCHAR* name, u32 flags, u32 access) { (void)access; return CreateWaitableTimerW(sa, flags & 1, name); }
static BOOL WINAPI SetWaitableTimer(HANDLE h, const LARGE_INTEGER* due, i32 period, void* fn, void* arg, BOOL resume) {
    (void)fn; (void)arg; (void)resume;
    TimerObj* t = (TimerObj*)h_get(h, OBJ_TIMER);
    if (!t) return FALSE;
    Guard g(sync_lock);
    i64 d = due->QuadPart;
    u64 ms = d < 0 ? (u64)(-d) / 10000 : 0;                    // absolute times are taken as "now"
    t->due_ms = now_ms() + ms;
    t->period = period > 0 ? (u32)period : 0;
    t->set = false;
    return TRUE;
}
static BOOL WINAPI CancelWaitableTimer(HANDLE h) { TimerObj* t = (TimerObj*)h_get(h, OBJ_TIMER); if (!t) return FALSE; t->due_ms = 0; t->set = false; return TRUE; }

// ---------------------------------------------------------------- critical sections
static BOOL WINAPI InitializeCriticalSectionEx(RTL_CRITICAL_SECTION* cs, u32 spin, u32 flags) {
    (void)flags;
    memset(cs, 0, sizeof(*cs));
    cs->LockCount = -1;
    cs->SpinCount = spin;
    return TRUE;
}
static void WINAPI InitializeCriticalSection(RTL_CRITICAL_SECTION* cs) { InitializeCriticalSectionEx(cs, 0, 0); }
static BOOL WINAPI InitializeCriticalSectionAndSpinCount(RTL_CRITICAL_SECTION* cs, u32 spin) { return InitializeCriticalSectionEx(cs, spin, 0); }
static void WINAPI DeleteCriticalSection(RTL_CRITICAL_SECTION* cs) { cs->LockCount = -1; cs->OwningThread = nullptr; cs->RecursionCount = 0; }
static BOOL WINAPI TryEnterCriticalSection(RTL_CRITICAL_SECTION* cs) {
    HANDLE me = (HANDLE)(u64)cur_thr()->id;
    if (cs->OwningThread == me) { cs->RecursionCount++; return TRUE; }
    if (__sync_bool_compare_and_swap(&cs->LockCount, -1, 0)) { cs->OwningThread = me; cs->RecursionCount = 1; return TRUE; }
    return FALSE;
}
static void WINAPI EnterCriticalSection(RTL_CRITICAL_SECTION* cs) {
    for (u32 spins = 0; !TryEnterCriticalSection(cs); spins++) nap(spins);
}
static void WINAPI LeaveCriticalSection(RTL_CRITICAL_SECTION* cs) {
    if (--cs->RecursionCount > 0) return;
    cs->RecursionCount = 0;
    cs->OwningThread = nullptr;
    __sync_synchronize();
    cs->LockCount = -1;
}
static u32 WINAPI SetCriticalSectionSpinCount(RTL_CRITICAL_SECTION* cs, u32 n) { u32 o = (u32)cs->SpinCount; cs->SpinCount = n; return o; }

// ---------------------------------------------------------------- slim reader/writer locks, condition variables, once
// SRWLOCK is one pointer: 0 free, -1 held exclusively, n > 0 held by n readers
static void WINAPI InitializeSRWLock(i64* l) { *l = 0; }
static u8 WINAPI TryAcquireSRWLockExclusive(i64* l) { return __sync_bool_compare_and_swap(l, 0, -1); }
static void WINAPI AcquireSRWLockExclusive(i64* l) { for (u32 s = 0; !__sync_bool_compare_and_swap(l, 0, -1); s++) nap(s); }
static void WINAPI ReleaseSRWLockExclusive(i64* l) { __sync_synchronize(); *l = 0; }
static u8 WINAPI TryAcquireSRWLockShared(i64* l) { i64 v = *l; return v >= 0 && __sync_bool_compare_and_swap(l, v, v + 1); }
static void WINAPI AcquireSRWLockShared(i64* l) { for (u32 s = 0; !TryAcquireSRWLockShared(l); s++) nap(s); }
static void WINAPI ReleaseSRWLockShared(i64* l) { __sync_fetch_and_sub(l, 1); }

// CONDITION_VARIABLE is one pointer used as a generation counter: a wake bumps it, a sleeper waits for it to move
static void WINAPI InitializeConditionVariable(u64* cv) { *cv = 0; }
static void WINAPI WakeConditionVariable(u64* cv) { __sync_fetch_and_add(cv, 1); }
static void WINAPI WakeAllConditionVariable(u64* cv) { __sync_fetch_and_add(cv, 1); }
static bool cv_wait(u64* cv, u64 gen, u32 timeout) {
    u64 end = timeout == INFINITE ? ~0ull : now_ms() + timeout;
    for (u32 s = 0; *(volatile u64*)cv == gen; s++) {
        if (now_ms() >= end) return false;
        nap(s);
    }
    return true;
}
static BOOL WINAPI SleepConditionVariableCS(u64* cv, RTL_CRITICAL_SECTION* cs, u32 timeout) {
    u64 gen = *(volatile u64*)cv;
    int rec = (int)cs->RecursionCount;
    cs->RecursionCount = 1;
    LeaveCriticalSection(cs);
    bool ok = cv_wait(cv, gen, timeout);
    EnterCriticalSection(cs);
    cs->RecursionCount = rec;
    if (!ok) { set_error(ERROR_TIMEOUT); return FALSE; }
    return TRUE;
}
static BOOL WINAPI SleepConditionVariableSRW(u64* cv, i64* l, u32 timeout, u32 flags) {
    u64 gen = *(volatile u64*)cv;
    bool shared = flags & 1;
    if (shared) ReleaseSRWLockShared(l); else ReleaseSRWLockExclusive(l);
    bool ok = cv_wait(cv, gen, timeout);
    if (shared) AcquireSRWLockShared(l); else AcquireSRWLockExclusive(l);
    if (!ok) { set_error(ERROR_TIMEOUT); return FALSE; }
    return TRUE;
}

// INIT_ONCE: 0 not started, 1 running, 2 done
typedef BOOL (WINAPI *InitOnceFn)(u64* once, void* param, void** ctx);
static BOOL WINAPI InitOnceExecuteOnce(u64* once, InitOnceFn fn, void* param, void** ctx) {
    for (u32 s = 0;; s++) {
        u64 v = *(volatile u64*)once;
        if ((v & 3) == 2) { if (ctx) *ctx = (void*)(v & ~3ull); return TRUE; }
        if (v == 0 && __sync_bool_compare_and_swap(once, 0, 1)) {
            void* c = nullptr;
            BOOL ok = fn(once, param, &c);
            if (!ok) { *once = 0; return FALSE; }
            *once = ((u64)c & ~3ull) | 2;
            if (ctx) *ctx = c;
            return TRUE;
        }
        nap(s);
    }
}
static BOOL WINAPI InitOnceBeginInitialize(u64* once, u32 flags, BOOL* pending, void** ctx) {
    for (u32 s = 0;; s++) {
        u64 v = *(volatile u64*)once;
        if ((v & 3) == 2) { *pending = FALSE; if (ctx) *ctx = (void*)(v & ~3ull); return TRUE; }
        if (flags & 2) { *pending = TRUE; return TRUE; }        // INIT_ONCE_CHECK_ONLY
        if (v == 0 && __sync_bool_compare_and_swap(once, 0, 1)) { *pending = TRUE; return TRUE; }
        nap(s);
    }
}
static BOOL WINAPI InitOnceComplete(u64* once, u32 flags, void* ctx) {
    if (flags & 4) { *once = 0; return TRUE; }                  // INIT_ONCE_INIT_FAILED
    *once = ((u64)ctx & ~3ull) | 2;
    return TRUE;
}
static void WINAPI InitOnceInitialize(u64* once) { *once = 0; }

// WaitOnAddress: wait while *addr still equals *compare
static BOOL WINAPI WaitOnAddress(volatile void* addr, void* cmp, SIZE_T size, u32 timeout) {
    u64 end = timeout == INFINITE ? ~0ull : now_ms() + timeout;
    for (u32 s = 0; !memcmp((const void*)addr, cmp, size); s++) {
        if (now_ms() >= end) { set_error(ERROR_TIMEOUT); return FALSE; }
        nap(s);
    }
    return TRUE;
}
static void WINAPI WakeByAddressSingle(void* a) { (void)a; }
static void WINAPI WakeByAddressAll(void* a) { (void)a; }

// interlocked singly linked lists
struct SLIST_ENTRY { SLIST_ENTRY* Next; };
struct alignas(16) SLIST_HEADER { SLIST_ENTRY* first; u64 depth; };
static Lock slist_lock;
static void WINAPI InitializeSListHead(SLIST_HEADER* h) { h->first = nullptr; h->depth = 0; }
static SLIST_ENTRY* WINAPI InterlockedPushEntrySList(SLIST_HEADER* h, SLIST_ENTRY* e) {
    Guard g(slist_lock); SLIST_ENTRY* old = h->first; e->Next = old; h->first = e; h->depth++; return old;
}
static SLIST_ENTRY* WINAPI InterlockedPopEntrySList(SLIST_HEADER* h) {
    Guard g(slist_lock); SLIST_ENTRY* e = h->first; if (e) { h->first = e->Next; h->depth--; } return e;
}
static SLIST_ENTRY* WINAPI InterlockedFlushSList(SLIST_HEADER* h) { Guard g(slist_lock); SLIST_ENTRY* e = h->first; h->first = nullptr; h->depth = 0; return e; }
static u16 WINAPI QueryDepthSList(SLIST_HEADER* h) { return (u16)h->depth; }
static i32 WINAPI InterlockedIncrement(volatile i32* p) { return __sync_add_and_fetch(p, 1); }
static i32 WINAPI InterlockedDecrement(volatile i32* p) { return __sync_sub_and_fetch(p, 1); }
static i32 WINAPI InterlockedExchange(volatile i32* p, i32 v) { return __sync_lock_test_and_set(p, v); }
static i32 WINAPI InterlockedCompareExchange(volatile i32* p, i32 v, i32 cmp) { return __sync_val_compare_and_swap(p, cmp, v); }
static i32 WINAPI InterlockedExchangeAdd(volatile i32* p, i32 v) { return __sync_fetch_and_add(p, v); }

// ---------------------------------------------------------------- threads
static HANDLE WINAPI CreateThread(SECURITY_ATTRIBUTES* sa, SIZE_T stack, ThreadFn fn, void* arg, u32 flags, u32* tid) {
    (void)sa;
    Thr* t = thr_create(fn, arg, (flags & 0x10000) ? stack : stack, (flags & 4) != 0);   // CREATE_SUSPENDED = 4
    if (!t) return nullptr;
    if (tid) *tid = t->id;
    Obj* o = thread_obj(t);
    obj_ref(o);
    thr_release(t);                                             // the handle's reference is now the object's
    return h_new(o);
}
static HANDLE WINAPI CreateRemoteThread(HANDLE proc, SECURITY_ATTRIBUTES* sa, SIZE_T stack, ThreadFn fn, void* arg, u32 flags, u32* tid) {
    if ((u64)proc != (u64)-1) { set_error(ERROR_ACCESS_DENIED); return nullptr; }
    return CreateThread(sa, stack, fn, arg, flags, tid);
}
static void WINAPI ExitThread(u32 code) { thr_exit(code); }
static HANDLE WINAPI GetCurrentThread() { return (HANDLE)(i64)-2; }
static u32 WINAPI GetCurrentThreadId() { return cur_thr()->id; }
static Thr* thr_of(HANDLE h) { ThreadObj* o = (ThreadObj*)h_get(h, OBJ_THREAD); return o ? o->t : nullptr; }
static u32 WINAPI GetThreadId(HANDLE h) { Thr* t = thr_of(h); return t ? t->id : 0; }
static u32 WINAPI ResumeThread(HANDLE h) {
    Thr* t = thr_of(h);
    if (!t) return (u32)-1;
    int old = t->suspend;
    if (old > 0) __sync_fetch_and_sub(&t->suspend, 1);
    return (u32)old;
}
static u32 WINAPI SuspendThread(HANDLE h) {
    Thr* t = thr_of(h);
    if (!t) return (u32)-1;
    if (t != cur_thr() && t->started) { set_error(ERROR_NOT_SUPPORTED); return (u32)-1; }   // a running thread can't be stopped from outside
    return (u32)__sync_fetch_and_add(&t->suspend, 1);
}
static BOOL WINAPI GetExitCodeThread(HANDLE h, u32* code) {
    Thr* t = thr_of(h);
    if (!t) return FALSE;
    *code = t->done ? t->exit_code : 259;                       // STILL_ACTIVE
    return TRUE;
}
static BOOL WINAPI TerminateThread(HANDLE h, u32 code) {
    Thr* t = thr_of(h);
    if (!t) return FALSE;
    if (t == cur_thr()) thr_exit(code);
    msg("TerminateThread on another thread is not supported");
    set_error(ERROR_NOT_SUPPORTED);
    return FALSE;
}
static BOOL WINAPI SetThreadPriority(HANDLE h, int p) { (void)h; (void)p; return TRUE; }
static int WINAPI GetThreadPriority(HANDLE h) { (void)h; return 0; }
static BOOL WINAPI SetThreadPriorityBoost(HANDLE h, BOOL d) { (void)h; (void)d; return TRUE; }
static u64 WINAPI SetThreadAffinityMask(HANDLE h, u64 m) { (void)h; (void)m; return 1; }
static u32 WINAPI SetThreadIdealProcessor(HANDLE h, u32 p) { (void)h; (void)p; return 0; }
static HRESULT WINAPI SetThreadDescription(HANDLE h, const WCHAR* d) { (void)h; (void)d; return 0; }
static HRESULT WINAPI GetThreadDescription(HANDLE h, WCHAR** d) { (void)h; *d = (WCHAR*)h_alloc(2, true); return 0; }
static BOOL WINAPI GetThreadTimes(HANDLE h, FILETIME* c, FILETIME* e, FILETIME* k, FILETIME* u) {
    (void)h;
    memset(c, 0, 8); memset(e, 0, 8); memset(k, 0, 8);
    u64 t = now_ms() * 10000;
    u->dwLowDateTime = (u32)t; u->dwHighDateTime = (u32)(t >> 32);
    return TRUE;
}
static BOOL WINAPI SwitchToThread() { si::yield(); return TRUE; }
static void WINAPI Sleep(u32 ms) {
    if (ms == 0) si::yield();
    else if (ms == INFINITE) for (;;) si::sleep(1000000);
    else si::sleep(ms);
}
static u32 WINAPI SleepEx(u32 ms, BOOL alertable) { (void)alertable; Sleep(ms); return 0; }
static BOOL WINAPI QueueUserAPC(void* fn, HANDLE h, u64 data) { (void)fn; (void)h; (void)data; set_error(ERROR_NOT_SUPPORTED); return FALSE; }

// ---------------------------------------------------------------- time
static u64 boot_filetime = 0;           // FILETIME at boot (UTC is taken to be silicon's local clock)
u64 filetime_now() {
    if (!boot_filetime) {
        si::Clock c;
        u32 st = si::clock(&c);
        boot_filetime = stamp_to_filetime(st) - si::ticks_us() * 10;
    }
    return boot_filetime + si::ticks_us() * 10;
}
static u32 WINAPI GetTickCount() { return (u32)now_ms(); }
static u64 WINAPI GetTickCount64() { return now_ms(); }
static BOOL WINAPI QueryPerformanceCounter(LARGE_INTEGER* c) { c->QuadPart = (i64)si::ticks_us(); return TRUE; }
static BOOL WINAPI QueryPerformanceFrequency(LARGE_INTEGER* f) { f->QuadPart = 1000000; return TRUE; }
static void WINAPI GetSystemTimeAsFileTime(FILETIME* f) { u64 v = filetime_now(); f->dwLowDateTime = (u32)v; f->dwHighDateTime = (u32)(v >> 32); }
static void WINAPI GetSystemTimePreciseAsFileTime(FILETIME* f) { GetSystemTimeAsFileTime(f); }
static BOOL WINAPI QueryUnbiasedInterruptTime(u64* t) { *t = si::ticks_us() * 10; return TRUE; }
static void WINAPI QueryInterruptTime(u64* t) { *t = si::ticks_us() * 10; }

static bool leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static const int mdays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

static BOOL WINAPI FileTimeToSystemTime(const FILETIME* f, SYSTEMTIME* s) {
    u64 v = ((u64)f->dwHighDateTime << 32) | f->dwLowDateTime;
    if (v >> 63) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
    u64 ms = v / 10000;
    s->wMilliseconds = (u16)(ms % 1000);
    u64 secs = ms / 1000;
    s->wSecond = (u16)(secs % 60); s->wMinute = (u16)(secs / 60 % 60); s->wHour = (u16)(secs / 3600 % 24);
    u64 days = secs / 86400;
    s->wDayOfWeek = (u16)((days + 1) % 7);                      // 1601-01-01 was a Monday
    int y = 1601;
    for (;;) {
        u64 yd = leap(y) ? 366 : 365;
        if (days < yd) break;
        days -= yd;
        y++;
    }
    int m = 0;
    for (;; m++) {
        u64 md = (u64)mdays[m] + (m == 1 && leap(y));
        if (days < md) break;
        days -= md;
    }
    s->wYear = (u16)y; s->wMonth = (u16)(m + 1); s->wDay = (u16)(days + 1);
    return TRUE;
}
static BOOL WINAPI SystemTimeToFileTime(const SYSTEMTIME* s, FILETIME* f) {
    if (s->wYear < 1601 || s->wMonth < 1 || s->wMonth > 12 || s->wDay < 1 || s->wDay > 31 || s->wHour > 23 || s->wMinute > 59 || s->wSecond > 59) {
        set_error(ERROR_INVALID_PARAMETER); return FALSE;
    }
    u64 days = 0;
    for (int y = 1601; y < s->wYear; y++) days += leap(y) ? 366 : 365;
    for (int m = 1; m < s->wMonth; m++) days += (u64)mdays[m - 1] + (m == 2 && leap(s->wYear));
    days += s->wDay - 1u;
    u64 v = (((days * 24 + s->wHour) * 60 + s->wMinute) * 60 + s->wSecond) * 10000000ull + (u64)s->wMilliseconds * 10000;
    f->dwLowDateTime = (u32)v; f->dwHighDateTime = (u32)(v >> 32);
    return TRUE;
}
static void WINAPI GetSystemTime(SYSTEMTIME* s) { FILETIME f; GetSystemTimeAsFileTime(&f); FileTimeToSystemTime(&f, s); }
static void WINAPI GetLocalTime(SYSTEMTIME* s) { GetSystemTime(s); }
void WINAPI rt_GetLocalTime(SYSTEMTIME* s) { GetSystemTime(s); }
static BOOL WINAPI SetLocalTime(const SYSTEMTIME* s) { (void)s; set_error(ERROR_ACCESS_DENIED); return FALSE; }
static BOOL WINAPI FileTimeToLocalFileTime(const FILETIME* in, FILETIME* out) { *out = *in; return TRUE; }
static BOOL WINAPI LocalFileTimeToFileTime(const FILETIME* in, FILETIME* out) { *out = *in; return TRUE; }
static BOOL WINAPI SystemTimeToTzSpecificLocalTime(void* tz, const SYSTEMTIME* in, SYSTEMTIME* out) { (void)tz; *out = *in; return TRUE; }
static BOOL WINAPI TzSpecificLocalTimeToSystemTime(void* tz, const SYSTEMTIME* in, SYSTEMTIME* out) { (void)tz; *out = *in; return TRUE; }
static i32 WINAPI CompareFileTime(const FILETIME* a, const FILETIME* b) {
    u64 x = ((u64)a->dwHighDateTime << 32) | a->dwLowDateTime, y = ((u64)b->dwHighDateTime << 32) | b->dwLowDateTime;
    return x < y ? -1 : x > y ? 1 : 0;
}
struct TIME_ZONE_INFORMATION { i32 Bias; WCHAR StandardName[32]; SYSTEMTIME StandardDate; i32 StandardBias; WCHAR DaylightName[32]; SYSTEMTIME DaylightDate; i32 DaylightBias; };
static u32 WINAPI GetTimeZoneInformation(TIME_ZONE_INFORMATION* tz) {
    memset(tz, 0, sizeof(*tz));
    wide_from_utf8("Coordinated Universal Time", -1, tz->StandardName, 32);
    wide_from_utf8("Coordinated Universal Time", -1, tz->DaylightName, 32);
    return 0;                                                   // TIME_ZONE_ID_UNKNOWN
}
static u32 WINAPI GetDynamicTimeZoneInformation(void* tz) { memset(tz, 0, 432); return 0; }
static BOOL WINAPI FileTimeToDosDateTime(const FILETIME* f, u16* date, u16* time) {
    SYSTEMTIME s;
    if (!FileTimeToSystemTime(f, &s)) return FALSE;
    *date = (u16)(((s.wYear - 1980) << 9) | (s.wMonth << 5) | s.wDay);
    *time = (u16)((s.wHour << 11) | (s.wMinute << 5) | (s.wSecond / 2));
    return TRUE;
}
static BOOL WINAPI DosDateTimeToFileTime(u16 date, u16 time, FILETIME* f) {
    SYSTEMTIME s = {(u16)((date >> 9) + 1980), (u16)((date >> 5) & 15), 0, (u16)(date & 31), (u16)(time >> 11), (u16)((time >> 5) & 63), (u16)((time & 31) * 2), 0};
    return SystemTimeToFileTime(&s, f);
}

// ---------------------------------------------------------------- anonymous pipes
struct PipeBuf { u8 data[4096]; u32 head, tail; int readers, writers; };
struct PipeObj : Obj { PipeBuf* p; bool write_end; };
static void pipe_destroy(Obj* o) {
    PipeObj* e = (PipeObj*)o;
    bool last;
    {
        Guard g(sync_lock);
        if (e->write_end) e->p->writers--; else e->p->readers--;
        last = !e->p->readers && !e->p->writers;
    }
    if (last) ifree(e->p);
    ifree(o);
}
i64 pipe_read(Obj* o, void* buf, u32 n) {
    PipeObj* e = (PipeObj*)o;
    if (e->write_end) return -1;
    for (u32 s = 0;; s++) {
        {
            Guard g(sync_lock);
            PipeBuf* p = e->p;
            if (p->head != p->tail) {
                u32 k = 0;
                while (k < n && p->head != p->tail) { ((u8*)buf)[k++] = p->data[p->head]; p->head = (p->head + 1) % sizeof(p->data); }
                return k;
            }
            if (!p->writers) return -1;
        }
        nap(s);
    }
}
i64 pipe_write(Obj* o, const void* buf, u32 n) {
    PipeObj* e = (PipeObj*)o;
    if (!e->write_end) return -1;
    u32 done = 0;
    for (u32 s = 0; done < n; s++) {
        {
            Guard g(sync_lock);
            PipeBuf* p = e->p;
            if (!p->readers) return done ? (i64)done : -1;
            while (done < n && (p->tail + 1) % sizeof(p->data) != p->head) {
                p->data[p->tail] = ((const u8*)buf)[done++];
                p->tail = (p->tail + 1) % sizeof(p->data);
            }
        }
        if (done < n) nap(s);
    }
    return done;
}
static BOOL WINAPI CreatePipe(HANDLE* r, HANDLE* w, SECURITY_ATTRIBUTES* sa, u32 size) {
    (void)sa; (void)size;
    PipeBuf* p = (PipeBuf*)izalloc(sizeof(PipeBuf));
    PipeObj* re = (PipeObj*)izalloc(sizeof(PipeObj));
    PipeObj* we = (PipeObj*)izalloc(sizeof(PipeObj));
    if (!p || !re || !we) { set_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    re->type = we->type = OBJ_PIPE;
    re->refs = we->refs = 1;
    re->destroy = we->destroy = pipe_destroy;
    re->p = we->p = p;
    we->write_end = true;
    p->readers = p->writers = 1;
    *r = h_new(re);
    *w = h_new(we);
    return TRUE;
}
static BOOL WINAPI PeekNamedPipe(HANDLE h, void* buf, u32 n, u32* got, u32* avail, u32* left) {
    PipeObj* e = (PipeObj*)h_get(h, OBJ_PIPE);
    if (got) *got = 0;
    if (left) *left = 0;
    if (!e) { if (avail) *avail = 0; return h_get(h) != nullptr; }
    Guard g(sync_lock);
    u32 have = (e->p->tail + sizeof(e->p->data) - e->p->head) % sizeof(e->p->data);
    if (avail) *avail = have;
    (void)buf; (void)n;
    return TRUE;
}

static BOOL WINAPI DuplicateHandle(HANDLE sp, HANDLE src, HANDLE tp, HANDLE* out, u32 access, BOOL inherit, u32 opts) {
    (void)sp; (void)tp; (void)access; (void)inherit;
    if ((u64)src == (u64)-2) { Obj* o = thread_obj(cur_thr()); obj_ref(o); *out = h_new(o); return *out != nullptr; }
    if ((u64)src == (u64)-1) { *out = src; return TRUE; }
    HANDLE h = h_dup(src);
    if (!h) return FALSE;
    if (opts & 1) h_close(src);                                 // DUPLICATE_CLOSE_SOURCE
    *out = h;
    return TRUE;
}
static BOOL WINAPI GetHandleInformation(HANDLE h, u32* f) { if (!h_get(h)) return FALSE; *f = 0; return TRUE; }
static BOOL WINAPI SetHandleInformation(HANDLE h, u32 m, u32 f) { (void)m; (void)f; return h_get(h) != nullptr; }

static const Export sync_exports[] = {
    E(WaitForSingleObject), E(WaitForSingleObjectEx), E(WaitForMultipleObjects), E(WaitForMultipleObjectsEx), E(SignalObjectAndWait),
    E(CreateEventA), E(CreateEventW), E(CreateEventExA), E(CreateEventExW), E(OpenEventW), EA("SetEvent", rt_SetEvent), E(ResetEvent),
    E(PulseEvent), E(CreateMutexA), E(CreateMutexW), E(CreateMutexExW), E(OpenMutexW), E(ReleaseMutex), E(CreateSemaphoreA),
    E(CreateSemaphoreW), E(CreateSemaphoreExW), E(ReleaseSemaphore), E(CreateWaitableTimerW), E(CreateWaitableTimerExW),
    E(SetWaitableTimer), E(CancelWaitableTimer), E(InitializeCriticalSection), E(InitializeCriticalSectionEx),
    E(InitializeCriticalSectionAndSpinCount), E(DeleteCriticalSection), E(EnterCriticalSection), E(LeaveCriticalSection),
    E(TryEnterCriticalSection), E(SetCriticalSectionSpinCount), E(InitializeSRWLock), E(AcquireSRWLockExclusive),
    E(ReleaseSRWLockExclusive), E(AcquireSRWLockShared), E(ReleaseSRWLockShared), E(TryAcquireSRWLockExclusive),
    E(TryAcquireSRWLockShared), E(InitializeConditionVariable), E(WakeConditionVariable), E(WakeAllConditionVariable),
    E(SleepConditionVariableCS), E(SleepConditionVariableSRW), E(InitOnceExecuteOnce), E(InitOnceBeginInitialize),
    E(InitOnceComplete), E(InitOnceInitialize), E(WaitOnAddress), E(WakeByAddressSingle), E(WakeByAddressAll),
    E(InitializeSListHead), E(InterlockedPushEntrySList), E(InterlockedPopEntrySList), E(InterlockedFlushSList), E(QueryDepthSList),
    E(InterlockedIncrement), E(InterlockedDecrement), E(InterlockedExchange), E(InterlockedCompareExchange), E(InterlockedExchangeAdd),
    E(CreateThread), E(CreateRemoteThread), E(ExitThread), E(GetCurrentThread), E(GetCurrentThreadId), E(GetThreadId),
    E(ResumeThread), E(SuspendThread), E(GetExitCodeThread), E(TerminateThread), E(SetThreadPriority), E(GetThreadPriority),
    E(SetThreadPriorityBoost), E(SetThreadAffinityMask), E(SetThreadIdealProcessor), E(SetThreadDescription), E(GetThreadDescription),
    E(GetThreadTimes), E(SwitchToThread), E(Sleep), E(SleepEx), E(QueueUserAPC),
    E(GetTickCount), E(GetTickCount64), E(QueryPerformanceCounter), E(QueryPerformanceFrequency), E(GetSystemTimeAsFileTime),
    E(GetSystemTimePreciseAsFileTime), E(QueryUnbiasedInterruptTime), E(QueryInterruptTime), E(FileTimeToSystemTime),
    E(SystemTimeToFileTime), E(GetSystemTime), E(GetLocalTime), E(SetLocalTime), E(FileTimeToLocalFileTime), E(LocalFileTimeToFileTime),
    E(SystemTimeToTzSpecificLocalTime), E(TzSpecificLocalTimeToSystemTime), E(CompareFileTime), E(GetTimeZoneInformation),
    E(GetDynamicTimeZoneInformation), E(FileTimeToDosDateTime), E(DosDateTimeToFileTime),
    E(CreatePipe), E(PeekNamedPipe), E(DuplicateHandle), E(GetHandleInformation), E(SetHandleInformation),
};
EXPORT_TABLE("kernel32.dll", sync_exports);

static const Export sync_nt[] = {
    EA("RtlInitializeCriticalSection", InitializeCriticalSection), EA("RtlEnterCriticalSection", EnterCriticalSection),
    EA("RtlLeaveCriticalSection", LeaveCriticalSection), EA("RtlDeleteCriticalSection", DeleteCriticalSection),
    EA("RtlTryEnterCriticalSection", TryEnterCriticalSection), EA("RtlInitializeSRWLock", InitializeSRWLock),
    EA("RtlAcquireSRWLockExclusive", AcquireSRWLockExclusive), EA("RtlReleaseSRWLockExclusive", ReleaseSRWLockExclusive),
    EA("RtlAcquireSRWLockShared", AcquireSRWLockShared), EA("RtlReleaseSRWLockShared", ReleaseSRWLockShared),
    EA("RtlWakeConditionVariable", WakeConditionVariable), EA("RtlWakeAllConditionVariable", WakeAllConditionVariable),
    EA("RtlInitializeSListHead", InitializeSListHead), EA("RtlInterlockedPushEntrySList", InterlockedPushEntrySList),
    EA("RtlInterlockedPopEntrySList", InterlockedPopEntrySList), EA("RtlInterlockedFlushSList", InterlockedFlushSList),
    EA("RtlWaitOnAddress", WaitOnAddress), EA("RtlWakeAddressSingle", WakeByAddressSingle), EA("RtlWakeAddressAll", WakeByAddressAll),
};
EXPORT_TABLE("ntdll.dll", sync_nt);

}  // namespace rt
