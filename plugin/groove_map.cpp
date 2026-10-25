#include "groove_map.h"

#include "../dsp/pump_curves.h"
#include "shape_data.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace gf {

namespace {

constexpr bool sameName(const char* a, const char* b) {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

constexpr bool curvesMatch() {
    const ParamInfo& p = PARAM_INFO[P_PUMP_CURVE];
    if (p.nopts != PC_COUNT) return false;
    for (int i = 0; i < PC_COUNT; ++i)
        if (!sameName(p.opts[i], kPumpCurveNames[i])) return false;
    return true;
}

// A lane picks from the whole library, by name, in the library's order.
constexpr bool shapesMatch(int param) {
    const ParamInfo& p = PARAM_INFO[param];
    if (p.nopts != kNumShapes) return false;
    for (int i = 0; i < kNumShapes; ++i)
        if (!sameName(p.opts[i], kShapes[i].name)) return false;
    return true;
}

} // namespace

// The option lists in surface.py against the DSP's. (The Pump's and the Shape's rates join them with the chain.)
static_assert(curvesMatch(), "surface.py PUMP_CURVES must match dsp/pump_curves.h kPumpCurveNames");
static_assert(shapesMatch(P_VOL_SHAPE) && shapesMatch(P_FLT_SHAPE) && shapesMatch(P_PAN_SHAPE),
              "surface.py SHAPES must match the library dsp/shapes.cpp plays (build/shape_data.h)");

float paramValue(int id, float n) {
    if (id < 0 || id >= P_COUNT) return 0.0f;
    const ParamSpec& s = PARAM_SPECS[id];
    n = n > 0.0f ? (n < 1.0f ? n : 1.0f) : 0.0f;   // NaN-safe (std::clamp passes NaN through)
    switch (s.curve) {
        case Curve::Lin:  return s.lo + n * (s.hi - s.lo);
        case Curve::Log:  return s.lo * std::pow(s.hi / s.lo, n);
        case Curve::Int:  return std::round(s.lo + n * (s.hi - s.lo));
        case Curve::Enum: return std::round(n * s.hi);   // lo = 0, hi = options - 1
        case Curve::Pow:  return s.hi * n * n * n;
        default:          return 0.0f;
    }
}

float paramNorm(int id, float v) {
    if (id < 0 || id >= P_COUNT) return 0.0f;
    const ParamSpec& s = PARAM_SPECS[id];
    float n = 0.0f;
    switch (s.curve) {
        case Curve::Log: n = v > 0.0f ? std::log(v / s.lo) / std::log(s.hi / s.lo) : 0.0f; break;
        case Curve::Pow: n = s.hi > 0.0f && v > 0.0f ? std::cbrt(v / s.hi) : 0.0f; break;
        case Curve::Enum: n = s.hi > 0.0f ? std::round(v) / s.hi : 0.0f; break;
        case Curve::Lin:
        case Curve::Int: n = s.hi > s.lo ? (v - s.lo) / (s.hi - s.lo) : 0.0f; break;
        default: break;
    }
    // A value too big for float's arithmetic (a hand-edited 1e38) is the top of the range, not the
    // bottom; only a NaN falls back to 0.
    return std::isnan(n) ? 0.0f : std::clamp(n, 0.0f, 1.0f);
}

std::string paramDisplay(int id, float norm) {
    if (id < 0 || id >= P_COUNT) return {};
    const float v = paramValue(id, norm);
    char b[48];
    switch (PARAM_SPECS[id].fmt) {
        case Fmt::Enum:
        case Fmt::Text: {
            const int i = static_cast<int>(v);
            return i >= 0 && i < PARAM_INFO[id].nopts ? PARAM_INFO[id].opts[i] : "";
        }
        case Fmt::Percent: std::snprintf(b, sizeof b, "%.0f%%", v * 100.0f); break;
        case Fmt::SignedPercent:   // "+40%", "-100%"; never "-0%"
            std::snprintf(b, sizeof b, std::fabs(v) < 0.005f ? "0%%" : "%+.0f%%", v * 100.0f);
            break;
        case Fmt::Db: std::snprintf(b, sizeof b, "%+.1f dB", std::fabs(v) < 0.05f ? 0.0f : v); break;   // never "-0.0 dB"
        case Fmt::Ms: std::snprintf(b, sizeof b, std::fabs(v) < 0.05f ? "0.0 ms" : "%+.1f ms", v); break;   // early or late
        // The unit changes where the rounded text would reach the next one ("1000 Hz" is "1.00 kHz").
        case Fmt::Hz:
            if (v < 999.5f) std::snprintf(b, sizeof b, "%.0f Hz", v);
            else std::snprintf(b, sizeof b, v < 9995.0f ? "%.2f kHz" : "%.1f kHz", v / 1000.0f);
            break;
        default: return {};
    }
    return b;
}

} // namespace gf
