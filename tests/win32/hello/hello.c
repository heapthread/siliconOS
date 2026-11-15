/* the smallest check of win32run: C runtime start-up, printf, arguments, and the Windows API by hand */
#include <stdio.h>
#include <windows.h>

int main(int argc, char** argv) {
    printf("hello from a Windows program! argc=%d\n", argc);
    for (int i = 0; i < argc; i++) printf("  argv[%d] = %s\n", i, argv[i]);
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof(exe));
    printf("running as %s, pid %lu, tid %lu\n", exe, GetCurrentProcessId(), GetCurrentThreadId());
    printf("pi is about %.6f, 1e300*10 = %g\n", 3.14159265358979, 1e300 * 10.0);
    DWORD n;
    WriteConsoleA(GetStdHandle(STD_OUTPUT_HANDLE), "WriteConsoleA works\n", 20, &n, NULL);
    return 7;
}
