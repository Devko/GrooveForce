// The whole suite (make test): every module's tests in turn, then the totals. A module's tests are
// a `void <name>Tests();` in test/<name>_test.cpp; add its call below. make test-module M=<name>
// runs one module on its own (module_main.cpp).
#include "check.h"

#include <cstdio>

int eft::g_fail = 0, eft::g_pass = 0;

void dspTests();
void gridTests();
void pumpCurvesTests();
void shapesTests();
void transientTests();
void crossoverTests();
void pumpTests();
void shapeTests();

int main() {
    dspTests();
    gridTests();
    pumpCurvesTests();
    shapesTests();
    transientTests();
    crossoverTests();
    pumpTests();
    shapeTests();
    std::printf("%d passed, %d failed\n", eft::g_pass, eft::g_fail);
    return eft::g_fail ? 1 : 0;
}
