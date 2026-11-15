// win32run phase 3 test app: a multi-file C++ console program built with x86_64-w64-mingw32-g++
#pragma once
#include <cstdio>
#include <string>

extern int g_failures;
extern int g_checks;
void check(bool ok, const std::string& what);

void test_memory();
void test_files();
void test_unicode();
void test_threads();
void test_args(int argc, char** argv);
void test_cpp();
