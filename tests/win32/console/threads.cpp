#include "common.h"
#include <windows.h>
#include <process.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>
#include <condition_variable>

static CRITICAL_SECTION cs;
static long counter = 0;
static DWORD tls_index;

static DWORD WINAPI worker(void* arg) {
    int id = (int)(INT_PTR)arg;
    TlsSetValue(tls_index, (void*)(INT_PTR)(id * 100));
    for (int i = 0; i < 20000; i++) {
        EnterCriticalSection(&cs);
        long v = counter;
        if ((i & 255) == 0) Sleep(0);                // try to get preempted in the middle
        counter = v + 1;
        LeaveCriticalSection(&cs);
    }
    return TlsGetValue(tls_index) == (void*)(INT_PTR)(id * 100) ? 7 : 1;
}

static unsigned __stdcall crt_worker(void* arg) { *(int*)arg = 99; return 5; }

void test_threads() {
    std::printf("[threads and synchronization]\n");
    InitializeCriticalSection(&cs);
    tls_index = TlsAlloc();
    check(tls_index != TLS_OUT_OF_INDEXES, "TlsAlloc");
    TlsSetValue(tls_index, (void*)123);
    HANDLE t[4];
    DWORD ids[4];
    for (int i = 0; i < 4; i++) t[i] = CreateThread(nullptr, 0, worker, (void*)(INT_PTR)(i + 1), 0, &ids[i]);
    check(t[0] && t[3] && ids[0] != ids[1], "CreateThread x4");
    DWORD w = WaitForMultipleObjects(4, t, TRUE, 30000);
    check(w == WAIT_OBJECT_0, "WaitForMultipleObjects (all)");
    check(counter == 80000, "critical section kept the counter exact (80000)");
    bool codes = true;
    for (int i = 0; i < 4; i++) { DWORD c = 0; GetExitCodeThread(t[i], &c); codes &= c == 7; CloseHandle(t[i]); }
    check(codes, "each thread had its own TLS value (exit code 7)");
    check(TlsGetValue(tls_index) == (void*)123, "the main thread's TLS value is untouched");
    DeleteCriticalSection(&cs);

    HANDLE ev = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    check(WaitForSingleObject(ev, 50) == WAIT_TIMEOUT, "an unset event times out");
    SetEvent(ev);
    check(WaitForSingleObject(ev, 0) == WAIT_OBJECT_0, "a set event is signaled");
    CloseHandle(ev);
    HANDLE sem = CreateSemaphoreA(nullptr, 2, 2, nullptr);
    check(WaitForSingleObject(sem, 0) == WAIT_OBJECT_0 && WaitForSingleObject(sem, 0) == WAIT_OBJECT_0 && WaitForSingleObject(sem, 10) == WAIT_TIMEOUT, "semaphore counts down");
    CloseHandle(sem);

    int out = 0;
    uintptr_t ct = _beginthreadex(nullptr, 0, crt_worker, &out, 0, nullptr);
    WaitForSingleObject((HANDLE)ct, INFINITE);
    DWORD code = 0;
    GetExitCodeThread((HANDLE)ct, &code);
    check(out == 99 && code == 5, "_beginthreadex");
    CloseHandle((HANDLE)ct);

    // the C++ standard library's threads (built on the Win32 API by MinGW)
    std::atomic<int> total{0};
    std::mutex m;
    std::condition_variable cv;
    int ready = 0;
    std::vector<std::thread> pool;
    for (int i = 0; i < 3; i++)
        pool.emplace_back([&, i] {
            for (int k = 0; k < 1000; k++) total += i + 1;
            std::lock_guard<std::mutex> g(m);
            ready++;
            cv.notify_one();
        });
    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] { return ready == 3; });
    }
    for (auto& th : pool) th.join();
    check(total == 6000, "std::thread + std::mutex + std::condition_variable");

    DWORD t0 = GetTickCount();
    ULONGLONG t64 = GetTickCount64();
    LARGE_INTEGER f, c0, c1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c0);
    Sleep(100);
    QueryPerformanceCounter(&c1);
    double ms = (c1.QuadPart - c0.QuadPart) * 1000.0 / f.QuadPart;
    check(ms >= 95 && ms < 400, "Sleep(100) took " + std::to_string((int)ms) + " ms by QueryPerformanceCounter");
    check(GetTickCount() - t0 >= 90 && GetTickCount64() >= t64, "GetTickCount / GetTickCount64 advance");
    SYSTEMTIME st;
    GetLocalTime(&st);
    check(st.wYear >= 2024 && st.wMonth >= 1 && st.wMonth <= 12, "GetLocalTime year " + std::to_string(st.wYear));
}
