// The pump curves (dsp/pump_curves.h): what every curve family has to satisfy, whichever shapes
// the ten named curves take, and that the names mean something. The curves themselves are the
// user's (dsp/pump_curves.cpp); these are the only hard rules.
#include "check.h"
#include "../dsp/pump_curves.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

const char* name(int c) { return gf::kPumpCurveNames[c]; }

void curvesEnds() {        // every curve: 0 at the bottom, 1 recovered, x clamped outside 0..1
    for (int c = 0; c < gf::PC_COUNT; ++c) {
        CHECK(gf::pumpLevel(c, 0.0f) == 0.0f && gf::pumpLevel(c, 1.0f) == 1.0f);
        CHECK(gf::pumpLevel(c, -0.5f) == 0.0f && gf::pumpLevel(c, 7.0f) == 1.0f);
    }
}

void curvesRise() {        // never falls, stays in 0..1, finite (NaN x: 1)
    constexpr int kSteps = 4096;
    for (int c = 0; c < gf::PC_COUNT; ++c) {
        bool rises = true, inRange = true;
        float prev = gf::pumpLevel(c, 0.0f);
        for (int i = 0; i <= kSteps; ++i) {
            const float level = gf::pumpLevel(c, static_cast<float>(i) / kSteps);
            if (!(level >= prev)) rises = false;   // also false for NaN
            if (!(level >= 0.0f && level <= 1.0f)) inRange = false;
            prev = level;
        }
        CHECK(rises);
        CHECK(inRange);
        if (!rises || !inRange) std::printf("  curve %s\n", name(c));
        CHECK(gf::pumpLevel(c, NAN) == 1.0f);
        CHECK(std::isfinite(gf::pumpLevel(c, INFINITY)) && std::isfinite(gf::pumpLevel(c, -INFINITY)));
    }
}

void curvesDiffer() {      // each curve is its own: any two differ by > 0.05 somewhere in 0..1
    constexpr int kSteps = 256;
    for (int a = 0; a < gf::PC_COUNT; ++a)
        for (int b = a + 1; b < gf::PC_COUNT; ++b) {
            float most = 0.0f;
            for (int i = 0; i < kSteps; ++i) {
                const float x = static_cast<float>(i) / (kSteps - 1);
                most = std::max(most, std::fabs(gf::pumpLevel(a, x) - gf::pumpLevel(b, x)));
            }
            CHECK(most > 0.05f);
            if (!(most > 0.05f)) std::printf("  %s and %s differ by at most %.3f\n", name(a), name(b), static_cast<double>(most));
        }
}

void curvesCharacter() {   // the names mean something
    CHECK(gf::pumpLevel(gf::PC_LINEAR, 0.5f) == 0.5f);
    CHECK(gf::pumpLevel(gf::PC_HOLD, 0.5f) < 0.05f);                                // held down, then back
    CHECK(gf::pumpLevel(gf::PC_LATE, 0.5f) < gf::pumpLevel(gf::PC_SMOOTH, 0.5f));   // comes back late
    CHECK(gf::pumpLevel(gf::PC_SNAP, 0.25f) > gf::pumpLevel(gf::PC_SMOOTH, 0.25f)); // snaps back early
}

} // namespace

void pumpCurvesTests() {
    curvesEnds();
    curvesRise();
    curvesDiffer();
    curvesCharacter();
}

// make test-module M=pump_curves builds with -DMODULE_TESTS=<M>Tests, i.e. pump_curvesTests; the
// suite (main.cpp) calls pumpCurvesTests.
void pump_curvesTests() { pumpCurvesTests(); }
