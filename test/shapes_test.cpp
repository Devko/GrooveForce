// The shape library (dsp/shapes.h) against the data surface.py generates: the order and the names,
// every shape in range and finite for any position, the values the waves, gates, accents and pumps
// have at known phases, the wrap, the random shapes as pure functions of the position, and the
// curve segment's law. The pump curves' own values are not assumed: a Pump shape is compared with
// pumpLevel itself.
#include "check.h"
#include "../dsp/pump_curves.h"
#include "../dsp/shapes.h"
#include "shape_data.h"   // generated: surface/build

#include <cmath>
#include <cstdio>
#include <string>

namespace {

constexpr double kPi = 3.14159265358979323846;

int idx(const char* name) {
    for (int i = 0; i < gf::numShapes(); ++i)
        if (std::string(gf::shapeName(i)) == name) return i;
    std::printf("  no shape named %s\n", name);
    CHECK(false);
    return 0;
}
float at(const char* name, double cycles) { return gf::shapeAt(idx(name), cycles); }
bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// EffectForce's gate patterns (Pulse::kPatternSteps, 'x' open) without "1/16" and "Pump": the library
// has them as 16 steps at 1 or 0.
struct Gate { const char* name; const char* steps; };
constexpr Gate kGates[] = {
    {"1/8", "x.x.x.x.x.x.x.x."},       {"1/4", "x...x...x...x..."},       {"Offbeat", "..x...x...x...x."},
    {"Off 16ths", ".x.x.x.x.x.x.x.x"}, {"Dotted", "x..x..x..x..x..x"},    {"Tresillo", "x..x..x.x..x..x."},
    {"Gallop", "x.xxx.xxx.xxx.xx"},    {"Rev Gallop", "xxx.xxx.xxx.xxx."}, {"Trance 1", "x.xx.xx.x.xx.xx."},
    {"Trance 2", "xx.xx.x.xx.xx.x."},  {"Trance 3", "x.xxx.x.x.xxx.x."},  {"Stutter", "x.x.x.x.xxxxxxxx"},
    {"Half Bar", "xxxxxxxx........"},  {"Build", "x...x...x.x.xxxx"},
};

void shapesNamed() {      // the library is the generated one, in surface.py's order
    CHECK(gf::numShapes() == 38 && gf::kNumShapes == 38);
    CHECK(std::string(gf::shapeName(0)) == "Sine" && std::string(gf::shapeName(37)) == "Random Smooth");
    // waves, then one pump per curve, the gates, the accents, the random pair
    CHECK(idx("Triangle") == 1 && idx("Saw Up") == 2 && idx("Saw Down") == 3 && idx("Ramp Up") == 4);
    CHECK(idx("Ramp Down") == 5 && idx("Square") == 6 && idx("Pulse 25") == 7);
    CHECK(idx("Pump Smooth") == 8 && idx("Pump Hold") == 17 && idx("1/8") == 18 && idx("Build") == 31);
    CHECK(idx("Offbeat Accent") == 32 && idx("Shuffle") == 35 && idx("Random Steps") == 36);
    for (int i = 0; i < gf::numShapes(); ++i) {
        const std::string name = gf::shapeName(i);
        CHECK(!name.empty() && name.size() <= 14);
        for (int j = i + 1; j < gf::numShapes(); ++j) CHECK(name != gf::shapeName(j));
    }
    // the pumps are the pump curves, in their order and by their names
    for (int c = 0; c < gf::PC_COUNT; ++c) {
        const gf::ShapeDef& d = gf::kShapes[8 + c];
        CHECK(d.kind == gf::ShapeKind::Pump && d.pumpCurve == c);
        CHECK(std::string(d.name) == std::string("Pump ") + gf::kPumpCurveNames[c]);
    }
    CHECK(gf::kShapes[36].kind == gf::ShapeKind::RandomSteps && gf::kShapes[37].kind == gf::ShapeKind::RandomSmooth);
    // an index out of range (never from the surface) is the nearest shape
    CHECK(std::string(gf::shapeName(-3)) == "Sine" && std::string(gf::shapeName(99)) == "Random Smooth");
    CHECK(gf::shapeAt(-3, 0.25) == gf::shapeAt(0, 0.25) && gf::shapeAt(99, 3.3) == gf::shapeAt(37, 3.3));
}

void shapesInRange() {    // every shape: 0..1 and finite at 4096 phases over 8 cycles, also cycles < 0
    for (int s = 0; s < gf::numShapes(); ++s) {
        bool ok = true;
        for (int i = 0; i < 4096; ++i) {
            const double c = 8.0 * i / 4096;
            for (double cycles : {c, -c, c + 1e6 + 0.37, -c - 1e6 - 0.37, c + 123456789.0}) {
                const float v = gf::shapeAt(s, cycles);
                if (!(v >= 0.0f && v <= 1.0f)) ok = false;   // also false for NaN
            }
        }
        CHECK(ok);
        if (!ok) std::printf("  shape %s\n", gf::shapeName(s));
        // a position that is not a number or not finite is still a value
        for (double bad : {std::nan(""), HUGE_VAL, -HUGE_VAL, 1e300, -1e300}) {
            const float v = gf::shapeAt(s, bad);
            CHECK(v >= 0.0f && v <= 1.0f);
        }
    }
}

void shapesValues() {
    const int sine = 0, offbeat = idx("Offbeat"), pumpHard = idx("Pump Hard");
    CHECK(std::fabs(gf::shapeAt(sine, 0.25) - 0.5f) < 1e-4f && std::fabs(gf::shapeAt(sine, 0.5) - 1.0f) < 1e-6f);
    CHECK(gf::shapeAt(offbeat, 2.0 / 16 + 0.01) == 1.0f && gf::shapeAt(offbeat, 0.01) == 0.0f);  // "..x...x..."
    CHECK(gf::shapeAt(pumpHard, 0.0) == 0.0f);
    CHECK(gf::shapeAt(pumpHard, 0.5) == gf::pumpLevel(gf::PC_HARD, 0.5f));
}

void shapesWaves() {      // the wave shapes at their known phases
    for (int i = 0; i < 256; ++i) {
        const double p = i / 256.0;
        CHECK(near(at("Sine", p), 0.5 - 0.5 * std::cos(2.0 * kPi * p), 1e-6));   // a raised cosine from 0
        CHECK(near(at("Triangle", p), p < 0.5 ? 2.0 * p : 2.0 - 2.0 * p, 1e-6));
        CHECK(near(at("Saw Up", p), p, 2e-4) && near(at("Saw Down", p), 1.0 - p, 2e-4));
        CHECK(at("Square", p) == (p < 0.5 ? 1.0f : 0.0f));
        CHECK(at("Pulse 25", p) == (p < 0.25 ? 1.0f : 0.0f));
    }
    CHECK(at("Sine", 0.0) == 0.0f && near(at("Sine", 0.75), 0.5, 1e-6));
    CHECK(at("Triangle", 0.0) == 0.0f && at("Triangle", 0.5) == 1.0f);
    CHECK(at("Saw Up", 0.0) == 0.0f && at("Saw Down", 0.0) == 1.0f);
    CHECK(at("Saw Up", 0.99995) == 1.0f && at("Saw Down", 0.99995) == 0.0f);   // the edge is at the wrap
    // the ramps: the saws' directions, bent: one starts slow and builds, the other falls fast and trails off
    CHECK(at("Ramp Up", 0.0) == 0.0f && at("Ramp Down", 0.0) == 1.0f);
    CHECK(at("Ramp Up", 0.5) < 0.3f && at("Ramp Down", 0.5) < 0.3f);
    CHECK(at("Ramp Up", 0.99995) > 0.99f && at("Ramp Down", 0.99995) < 0.01f);
    float up = at("Ramp Up", 0.0), down = at("Ramp Down", 0.0);
    bool rises = true, falls = true;
    for (int i = 1; i < 1000; ++i) {
        const float u = at("Ramp Up", i / 1000.0), d = at("Ramp Down", i / 1000.0);
        if (!(u >= up)) rises = false;
        if (!(d <= down)) falls = false;
        up = u;
        down = d;
    }
    CHECK(rises && falls);
}

void shapesGates() {      // EffectForce's gate patterns, step by step, wherever in the cycle and in any cycle
    for (const Gate& g : kGates) {
        const int s = idx(g.name);
        CHECK(gf::kShapes[s].kind == gf::ShapeKind::Steps && gf::kShapes[s].count == 16);
        for (int i = 0; i < 16; ++i) {
            const float want = g.steps[i] == 'x' ? 1.0f : 0.0f;
            const double from = i / 16.0, to = (i + 1) / 16.0;
            for (double cycle : {0.0, 5.0, -2.0}) {
                CHECK(gf::shapeAt(s, cycle + from) == want);               // the step's first instant
                CHECK(gf::shapeAt(s, cycle + (from + to) / 2) == want);
                CHECK(gf::shapeAt(s, cycle + to - 1e-6) == want);          // and its last
            }
        }
    }
}

void shapesAccents() {    // the accent patterns' levels
    const char* gallop = "x.xxx.xxx.xxx.xx";
    const float roll[4] = {1.0f, 0.55f, 0.75f, 0.55f}, shuffle[4] = {1.0f, 0.3f, 0.7f, 0.5f};
    for (int i = 0; i < 16; ++i) {
        const double mid = (i + 0.5) / 16;
        CHECK(at("Offbeat Accent", mid) == (i % 4 == 2 ? 1.0f : 0.45f));   // the offbeat eighths, the other 16ths
        CHECK(at("Rolling 16ths", mid) == roll[i % 4]);
        CHECK(at("Shuffle", mid) == shuffle[i % 4]);
        // Gallop's open steps; the head of each gallop (steps 0, 4, 8, 12) at 1, the other hits 0.6
        CHECK(at("Gallop Accent", mid) == (gallop[i] != 'x' ? 0.0f : i % 4 == 0 ? 1.0f : 0.6f));
    }
}

void shapesPumps() {      // a Pump shape is 0 at phase 0 and recovers by pumpLevel across the cycle
    for (int c = 0; c < gf::PC_COUNT; ++c) {
        const int s = 8 + c;
        CHECK(gf::shapeAt(s, 0.0) == 0.0f && gf::shapeAt(s, 7.0) == 0.0f);
        for (int i = 0; i < 64; ++i) {
            const double p = i / 64.0;
            CHECK(gf::shapeAt(s, p) == gf::pumpLevel(c, static_cast<float>(p)));
            CHECK(gf::shapeAt(s, 2.0 + p) == gf::pumpLevel(c, static_cast<float>(p)));
        }
    }
}

void shapesWrap() {       // Points and Pump: continuous across the wrap unless the last segment is Hold
    for (int s = 0; s < gf::numShapes(); ++s) {
        const gf::ShapeDef& d = gf::kShapes[s];
        if (d.kind == gf::ShapeKind::Points) {
            const gf::ShapePoint& last = gf::kShapePoints[d.first + d.count - 1];
            if (last.seg != gf::Seg::Hold)
                for (double cycle : {0.0, 3.0, -1.0}) {
                    const bool joined = near(gf::shapeAt(s, cycle + 1.0 - 1e-9), gf::shapeAt(s, cycle + 1.0), 1e-5);
                    CHECK(joined);
                    if (!joined) std::printf("  shape %s\n", d.name);
                }
        }
        // a Pump shape ends recovered and the next cycle's duck falls back to 0 (the part smooths that
        // edge); Points and Steps repeat every cycle, and the cycle they are in makes no difference
        if (d.kind == gf::ShapeKind::Pump) CHECK(gf::shapeAt(s, 1.0 - 1e-9) == 1.0f && gf::shapeAt(s, 1.0) == 0.0f);
        if (d.kind != gf::ShapeKind::RandomSteps && d.kind != gf::ShapeKind::RandomSmooth)
            for (int i = 0; i < 100; ++i) {
                const double p = (i + 0.37) / 100;
                const bool same = near(gf::shapeAt(s, p), gf::shapeAt(s, p + 3.0), 1e-6) &&
                                  near(gf::shapeAt(s, p), gf::shapeAt(s, p - 5.0), 1e-6);
                CHECK(same);
                if (!same) std::printf("  shape %s at %.2f\n", d.name, p);
            }
    }
}

void shapesRandom() {     // the same `cycles` gives the same value; cycles 3 and 4 differ; 3.0 and 3.0 + 1e-9 agree
    for (const char* name : {"Random Steps", "Random Smooth"}) {
        const int s = idx(name);
        // pure functions of the position: asking in another order, or again, changes nothing
        const float a = gf::shapeAt(s, 3.37), b = gf::shapeAt(s, 9.81);
        CHECK(gf::shapeAt(s, 9.81) == b && gf::shapeAt(s, 3.37) == a && gf::shapeAt(s, 3.37) == a);
        CHECK(gf::shapeAt(s, 3.0) != gf::shapeAt(s, 4.0));
        CHECK(near(gf::shapeAt(s, 3.0), gf::shapeAt(s, 3.0 + 1e-9), 1e-6));
        // every bar has its own pattern, and over the bars the levels fill 0..1 evenly
        int differ = 0;
        double sum = 0.0, lo = 1.0, hi = 0.0;
        for (int c = -64; c < 192; ++c) {
            if (gf::shapeAt(s, c) != gf::shapeAt(s, c + 1)) ++differ;
            const double v = gf::shapeAt(s, c + 0.01);
            sum += v;
            lo = std::fmin(lo, v);
            hi = std::fmax(hi, v);
        }
        CHECK(differ >= 250);
        CHECK(near(sum / 256, 0.5, 0.06) && lo < 0.05 && hi > 0.95);
    }
    // the two are not each other's copy
    bool apart = false;
    for (int c = 0; c < 8; ++c) apart |= gf::shapeAt(idx("Random Steps"), c) != gf::shapeAt(idx("Random Smooth"), c);
    CHECK(apart);
    // Random Steps: 16 steps a cycle, each one value, a new one at nearly every step
    const int steps = idx("Random Steps");
    int distinct = 0, n = 0;
    for (int c = 0; c < 64; ++c) {
        float prev = -1.0f;
        for (int i = 0; i < 16; ++i) {
            const float v = gf::shapeAt(steps, c + (i + 0.01) / 16);
            CHECK(gf::shapeAt(steps, c + (i + 0.5) / 16) == v && gf::shapeAt(steps, c + (i + 1) / 16.0 - 1e-6) == v);
            if (v != prev) ++distinct;
            prev = v;
            ++n;
        }
    }
    CHECK(distinct > n * 95 / 100);
    // Random Smooth: 4 smooth segments a cycle between random levels, joined across the bar lines
    const int smooth = idx("Random Smooth");
    for (int c = -4; c < 12; ++c)
        for (int k = 0; k < 4; ++k) {
            const double from = c + k / 4.0;
            const float a = gf::shapeAt(smooth, from), b = gf::shapeAt(smooth, from + 0.25 - 1e-9);
            const float mid = gf::shapeAt(smooth, from + 0.125), next = gf::shapeAt(smooth, from + 0.25);
            CHECK(near(b, next, 1e-5));                        // continuous, also across the bar line
            CHECK(near(mid, (a + next) / 2.0f, 1e-6));         // a raised cosine from knot to knot
            const float lo = std::fmin(a, next), hi = std::fmax(a, next);
            for (int i = 1; i < 8; ++i) {
                const float v = gf::shapeAt(smooth, from + i / 32.0);
                CHECK(v >= lo - 1e-6f && v <= hi + 1e-6f);     // no overshoot
            }
        }
}

void shapesCurveSeg() {   // Seg::Curve, tension t, a to b: a + (b - a) (exp(6 t u) - 1) / (exp(6 t) - 1); t = 0: linear
    const float levels[][2] = {{0.0f, 1.0f}, {1.0f, 0.0f}, {0.2f, 0.7f}, {0.9f, 0.1f}};
    for (const auto& ab : levels) {
        const float a = ab[0], b = ab[1];
        for (float t : {-1.0f, -0.5f, 0.25f, 0.5f, 1.0f})
            for (int i = 0; i <= 20; ++i) {
                const double u = i / 20.0;
                const double want = a + (b - a) * (std::exp(6.0 * t * u) - 1.0) / (std::exp(6.0 * t) - 1.0);
                CHECK(near(gf::segmentValue(gf::Seg::Curve, a, b, t, static_cast<float>(u)), want, 2e-6));
            }
        for (int i = 0; i <= 20; ++i) {
            const float u = static_cast<float>(i) / 20.0f;
            CHECK(near(gf::segmentValue(gf::Seg::Curve, a, b, 0.0f, u), a + (b - a) * u, 1e-6));   // t = 0 is a line
            CHECK(near(gf::segmentValue(gf::Seg::Curve, a, b, 1e-7f, u), a + (b - a) * u, 1e-5));   // and near it
            CHECK(near(gf::segmentValue(gf::Seg::Linear, a, b, 0.8f, u), a + (b - a) * u, 1e-6));   // tension: Curve's only
            CHECK(near(gf::segmentValue(gf::Seg::Smooth, a, b, 0.8f, u),
                       a + (b - a) * (1.0 - std::cos(kPi * u)) / 2.0, 1e-6));
            CHECK(gf::segmentValue(gf::Seg::Hold, a, b, 0.8f, u) == a);
        }
        for (gf::Seg seg : {gf::Seg::Linear, gf::Seg::Smooth, gf::Seg::Curve}) {
            CHECK(gf::segmentValue(seg, a, b, 0.6f, 0.0f) == a);
            CHECK(near(gf::segmentValue(seg, a, b, 0.6f, 1.0f), b, 1e-6));
        }
    }
    // a positive tension starts slow (below the line when rising), a negative one fast
    CHECK(gf::segmentValue(gf::Seg::Curve, 0.0f, 1.0f, 0.5f, 0.5f) < 0.5f);
    CHECK(gf::segmentValue(gf::Seg::Curve, 0.0f, 1.0f, -0.5f, 0.5f) > 0.5f);
    // in the library: each Ramp's first segment is a Curve, and shapeAt follows its law across it
    for (const char* name : {"Ramp Up", "Ramp Down"}) {
        const gf::ShapeDef& d = gf::kShapes[idx(name)];
        const gf::ShapePoint& a = gf::kShapePoints[d.first];
        const gf::ShapePoint& b = gf::kShapePoints[d.first + 1];
        CHECK(a.seg == gf::Seg::Curve && a.tension != 0.0f);
        for (int i = 0; i < 50; ++i) {
            const double phase = a.x + (b.x - a.x) * (i / 50.0);
            const double u = (phase - a.x) / (b.x - a.x);
            const double t6 = 6.0 * a.tension;
            const double want = a.y + (b.y - a.y) * (std::exp(t6 * u) - 1.0) / (std::exp(t6) - 1.0);
            CHECK(near(gf::shapeAt(idx(name), phase), want, 2e-6));
        }
    }
}

} // namespace

void shapesTests() {
    shapesNamed();
    shapesInRange();
    shapesValues();
    shapesWaves();
    shapesGates();
    shapesAccents();
    shapesPumps();
    shapesWrap();
    shapesRandom();
    shapesCurveSeg();
}
