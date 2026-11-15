#include "common.h"
#include <windows.h>
#include <algorithm>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <cmath>

struct Tracker {
    static int alive;
    Tracker() { alive++; }
    ~Tracker() { alive--; }
};
int Tracker::alive = 0;

static int thrower(int depth) {
    Tracker t;
    if (depth == 0) throw std::runtime_error("deep error");
    return thrower(depth - 1) + 1;
}

void test_cpp() {
    std::printf("[C++ runtime]\n");
    std::vector<int> v;
    for (int i = 0; i < 1000; i++) v.push_back((i * 7919) % 1000);
    std::sort(v.begin(), v.end());
    check(std::is_sorted(v.begin(), v.end()) && v.front() == 0 && v.back() == 999, "std::vector + std::sort");
    std::map<std::string, int> m{{"one", 1}, {"two", 2}, {"three", 3}};
    check(m["two"] == 2 && m.size() == 3, "std::map<std::string, int>");
    auto p = std::make_unique<Tracker>();
    check(Tracker::alive == 1, "std::unique_ptr constructs");
    p.reset();
    check(Tracker::alive == 0, "and destroys");
    bool caught = false;
    try {
        thrower(10);
    } catch (const std::exception& e) {
        caught = std::string(e.what()) == "deep error";
    }
    check(caught, "a C++ exception thrown 10 frames deep is caught");
    check(Tracker::alive == 0, "destructors ran while unwinding (SEH unwind tables)");
    try { throw 42; } catch (int x) { caught = x == 42; }
    check(caught, "throw/catch int");
    std::ostringstream os;
    os << "pi=" << 3.25 << " hex=" << std::hex << 255;
    check(os.str() == "pi=3.25 hex=ff", "std::ostringstream: " + os.str());
    std::cout << "  ok   std::cout works" << std::endl;
    g_checks++;
    check(std::fabs(std::sin(M_PI / 6) - 0.5) < 1e-12 && std::fabs(std::pow(2.0, 0.5) - 1.4142135623730951) < 1e-15, "math: sin, pow");
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g %e %08.3f %x %s", 0.1, 12345.678, -3.14159, 48879, "str");
    check(std::string(buf) == "0.10000000000000001 1.234568e+04 -003.142 beef str", std::string("printf formatting: ") + buf);
    double d = 0;
    int i = 0;
    char word[16];
    check(std::sscanf("  2.5e3 -17 word", "%lf %d %15s", &d, &i, word) == 3 && d == 2500 && i == -17 && std::string(word) == "word", "sscanf");
}
