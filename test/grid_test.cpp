// dsp/grid.h: swing as a map between MPC's song position and straight time.
#include "check.h"
#include "../dsp/grid.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {

// A small deterministic generator (0..1), so a failure reproduces.
struct Rng {
    uint32_t seed = 12345;
    double next() {
        seed = seed * 1664525u + 1013904223u;
        return (seed >> 8) / 16777216.0;
    }
};

void gridStraight() {   // 0.5 is the identity, bit for bit, also before beat 0
    for (double b : {-3.7, 0.0, 0.124, 1.0, 17.375, 1e6 + 0.3}) {
        CHECK(gf::toStraight(b, {0.5f, 0.25}) == b);
        CHECK(gf::toSwung(b, {0.5f, 0.25}) == b);
        CHECK(gf::toStraight(b, {0.5f, 0.5}) == b);
        CHECK(gf::toSwung(b, {0.5f, 0.5}) == b);
    }
}

void gridSwing() {   // at 66% on 1/16, the second 16th of each 8th starts at 0.33 beats (40 ms at 120 BPM)
    const gf::Swing s{0.66f, 0.25};
    CHECK(std::fabs(gf::toSwung(0.25, s) - 0.33) < 1e-6);
    CHECK(std::fabs(gf::toStraight(0.33, s) - 0.25) < 1e-6);
    CHECK(gf::toStraight(0.5, s) == 0.5 && gf::toStraight(4.0, s) == 4.0);   // pair boundaries fixed
    CHECK(gf::toSwung(0.5, s) == 0.5 && gf::toSwung(4.0, s) == 4.0);
    CHECK(gf::toStraight(0.0, s) == 0.0 && gf::toSwung(0.0, s) == 0.0);
    CHECK(std::fabs((gf::toSwung(0.25, {0.58f, 0.25}) - 0.25) * 500.0 - 20.0) < 0.01);   // 58%: 20 ms at 120
    CHECK(std::fabs(gf::toSwung(0.5, {0.66f, 0.5}) - 0.66) < 1e-6);   // on 1/8: the second 8th of a beat
    CHECK(std::fabs(gf::toStraight(0.66, {0.66f, 0.5}) - 0.5) < 1e-6);
    CHECK(std::fabs(gf::toSwung(-0.25, s) - -0.17) < 1e-6);   // before beat 0 the same: the pair [-0.5, 0)
    CHECK(std::fabs(gf::toStraight(-0.17, s) - -0.25) < 1e-6);
    CHECK(std::fabs(gf::toSwung(8.25, s) - 8.33) < 1e-6);   // and the same in every pair
    // The halves are straight lines: the first 16th stretches, the second shrinks.
    CHECK(std::fabs(gf::toStraight(0.165, s) - 0.125) < 1e-6);
    CHECK(std::fabs(gf::toStraight(0.415, s) - 0.375) < 1e-6);
}

void gridRoundTrip() {   // monotonic, and each the other's inverse, across every amount and both units
    Rng rng;
    double worst = 0.0;
    bool monotonic = true;
    for (int i = 0; i < 10000; ++i) {
        const gf::Swing s{static_cast<float>(0.5 + 0.25 * rng.next()), rng.next() < 0.5 ? 0.25 : 0.5};
        const double b1 = -8.0 + 72.0 * rng.next();
        const double b2 = b1 + 2.0 * rng.next() * rng.next();   // mostly close above, sometimes a pair or more
        worst = std::max(worst, std::fabs(gf::toSwung(gf::toStraight(b1, s), s) - b1));
        worst = std::max(worst, std::fabs(gf::toStraight(gf::toSwung(b1, s), s) - b1));
        if (gf::toStraight(b1, s) > gf::toStraight(b2, s)) monotonic = false;
        if (gf::toSwung(b1, s) > gf::toSwung(b2, s)) monotonic = false;
    }
    CHECK(worst < 1e-9);
    CHECK(monotonic);
}

// Sixteen steps of the last bit across `at`: neither map jumps or goes back by more than rounding.
// (Not bit for bit: the Force's build is allowed to reorder float math, and may wobble the last bits.)
bool smoothAround(double at, const gf::Swing& s) {
    constexpr double kRounding = 1e-12;
    double b = at;
    for (int i = 0; i < 8; ++i) b = std::nextafter(b, -1e9);
    double lastStraight = gf::toStraight(b, s), lastSwung = gf::toSwung(b, s);
    for (int i = 0; i < 16; ++i) {
        b = std::nextafter(b, 1e9);
        const double st = gf::toStraight(b, s), sw = gf::toSwung(b, s);
        if (std::fabs(st - lastStraight) > kRounding || st < lastStraight - kRounding) return false;
        if (std::fabs(sw - lastSwung) > kRounding || sw < lastSwung - kRounding) return false;
        lastStraight = st;
        lastSwung = sw;
    }
    return true;
}

void gridBoundaries() {   // at the pair edges and where the two lines meet: no jump, no step back, the edges exact
    bool smooth = true, fixedEdges = true;
    for (float amount : {0.5001f, 0.58f, 0.66f, 0.7f, 0.75f}) {
        for (double u : {0.25, 0.5}) {
            const gf::Swing s{amount, u};
            for (int k = -16; k <= 128; ++k) {
                const double edge = k * 2.0 * u;
                smooth = smooth && smoothAround(edge, s);
                smooth = smooth && smoothAround(edge + u, s);   // straight: where the second unit starts
                smooth = smooth && smoothAround(edge + 2.0 * u * amount, s);   // swung: the same
                fixedEdges = fixedEdges && gf::toStraight(edge, s) == edge && gf::toSwung(edge, s) == edge;
            }
        }
    }
    CHECK(smooth);
    CHECK(fixedEdges);
}

void gridExtremes() {   // 75% clamps, 0.4 and NaN amounts act as 0.5, huge positions stay finite
    CHECK(gf::toSwung(0.25, {0.9f, 0.25}) == gf::toSwung(0.25, {0.75f, 0.25}));
    CHECK(gf::toStraight(0.4, {0.9f, 0.25}) == gf::toStraight(0.4, {0.75f, 0.25}));
    CHECK(gf::toSwung(0.25, {0.75f, 0.25}) > 0.37 && gf::toSwung(0.25, {0.75f, 0.25}) < 0.38);   // 75%: 0.375
    CHECK(gf::toStraight(1.3, {0.4f, 0.25}) == 1.3);
    CHECK(gf::toSwung(1.3, {0.4f, 0.25}) == 1.3);
    CHECK(gf::toStraight(1.3, {NAN, 0.25}) == 1.3);
    CHECK(gf::toSwung(1.3, {NAN, 0.25}) == 1.3);
    CHECK(gf::toStraight(1.3, {INFINITY, 0.25}) == 1.3 && gf::toSwung(1.3, {INFINITY, 0.25}) == 1.3);   // not finite: straight
    CHECK(gf::toStraight(1.3, {-INFINITY, 0.25}) == 1.3);
    CHECK(std::isfinite(gf::toStraight(1e12 + 0.1, {0.75f, 0.25})));
    CHECK(std::isfinite(gf::toSwung(1e12 + 0.1, {0.75f, 0.25})));
    CHECK(std::isfinite(gf::toStraight(-1e12 - 0.1, {0.75f, 0.5})));
    CHECK(std::isfinite(gf::toStraight(1.7e308, {0.66f, 0.25})) && std::isfinite(gf::toSwung(-1.7e308, {0.66f, 0.5})));
    CHECK(gf::toStraight(1.3, {0.66f, 0.0}) == 1.3 && gf::toSwung(1.3, {0.66f, -0.25}) == 1.3);   // no unit, no swing
    CHECK(gf::toStraight(1.3, {0.66f, NAN}) == 1.3);
    CHECK(std::isnan(gf::toStraight(NAN, {0.66f, 0.25})));   // not a position: passed on, not made up
}

} // namespace

void gridTests() {
    gridStraight();
    gridSwing();
    gridRoundTrip();
    gridBoundaries();
    gridExtremes();
}
