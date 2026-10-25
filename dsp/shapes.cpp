// The shape library's evaluator: see shapes.h. The shapes themselves are surface/build/shape_data.h.
#include "shapes.h"

#include "pump_curves.h"
#include "shape_data.h"

#include <algorithm>
#include <cmath>

namespace gf {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kNoFraction = 1e15;   // a double this large has no fraction left to be a phase
constexpr int kSteps = 16;             // Steps and RandomSteps: a cycle's steps
constexpr int kKnots = 4;              // RandomSmooth: a cycle's segments

float unit(float v) { return v >= 0.0f ? (v <= 1.0f ? v : 1.0f) : 0.0f; }   // NaN is 0

// Murmur3's 32-bit finalizer (Wang's lowbias variant): every input bit moves every output bit.
uint32_t mix(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

// A random shape's level 0..1 for (shape, cycle, step): a hash, so the same position gives the same
// level whenever it is asked, with no generator to keep in step with the song position.
float drawn(int shape, int64_t cycle, int step) {
    const uint32_t h = mix(mix(mix(static_cast<uint32_t>(shape) + 0x9E3779B9u) ^ static_cast<uint32_t>(cycle)) +
                           static_cast<uint32_t>(step) * 0x85EBCA6Bu);
    return static_cast<float>(h >> 8) * (1.0f / 16777216.0f);   // 24 bits: 0 .. 1 - 2^-24
}

float points(const ShapeDef& d, double phase) {
    if (d.count < 1) return 0.0f;
    const ShapePoint* p = kShapePoints + d.first;
    int i = 0;
    while (i + 1 < d.count && phase >= p[i + 1].x) ++i;
    const bool last = i + 1 == d.count;
    const double x1 = last ? 1.0 : p[i + 1].x;      // the last segment runs on to the first point, at x = 1
    const float y1 = last ? p[0].y : p[i + 1].y;
    const double width = x1 - p[i].x;
    return segmentValue(p[i].seg, p[i].y, y1, p[i].tension, width > 0.0 ? (phase - p[i].x) / width : 0.0);
}

} // namespace

int numShapes() { return kNumShapes; }

const char* shapeName(int shape) { return kShapes[std::clamp(shape, 0, kNumShapes - 1)].name; }

float segmentValue(Seg seg, float a, float b, float tension, double u) {
    if (!(u > 0.0)) u = 0.0;   // also NaN
    if (u > 1.0) u = 1.0;
    double along = u;          // how far from a to b
    switch (seg) {
        case Seg::Hold: return a;
        case Seg::Smooth: along = 0.5 * (1.0 - std::cos(kPi * u)); break;
        case Seg::Curve: {
            const double t = std::clamp(static_cast<double>(tension), -1.0, 1.0);
            if (std::fabs(t) >= 1e-9) along = std::expm1(6.0 * t * u) / std::expm1(6.0 * t);
            break;
        }
        case Seg::Linear: break;
    }
    return static_cast<float>(a + (static_cast<double>(b) - a) * along);
}

float shapeAt(int shape, double cycles) {
    shape = std::clamp(shape, 0, kNumShapes - 1);
    const ShapeDef& d = kShapes[shape];
    if (!(std::fabs(cycles) < kNoFraction)) cycles = 0.0;   // not a number, infinite or beyond a fraction
    double whole = std::floor(cycles), phase = cycles - whole;
    if (phase >= 1.0) { phase = 0.0; whole += 1.0; }        // a hair under a whole number rounded up to it
    const int64_t cycle = static_cast<int64_t>(whole);
    const int step = std::min(static_cast<int>(phase * kSteps), kSteps - 1);

    switch (d.kind) {
        case ShapeKind::Points: return unit(points(d, phase));
        case ShapeKind::Steps: return unit(kShapePoints[d.first + step].y);
        case ShapeKind::Pump: return unit(pumpLevel(d.pumpCurve, static_cast<float>(phase)));
        case ShapeKind::RandomSteps: return drawn(shape, cycle, step);
        case ShapeKind::RandomSmooth: {
            const int k = std::min(static_cast<int>(phase * kKnots), kKnots - 1);
            const float a = drawn(shape, cycle, k);
            const float b = k + 1 < kKnots ? drawn(shape, cycle, k + 1) : drawn(shape, cycle + 1, 0);
            return segmentValue(Seg::Smooth, a, b, 0.0f, phase * kKnots - k);
        }
    }
    return 0.0f;
}

} // namespace gf
