#include "common.h"
#include <windows.h>

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const std::string& what) {
    g_checks++;
    if (!ok) g_failures++;
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
}

int main(int argc, char** argv) {
    std::printf("console test: win32run phase 3\n");
    test_args(argc, argv);
    test_memory();
    test_files();
    test_unicode();
    test_threads();
    test_cpp();
    std::printf("console test: %d checks, %d failed -> %s\n", g_checks, g_failures, g_failures ? "FAILED" : "PASSED");
    return g_failures ? 1 : 0;
}
