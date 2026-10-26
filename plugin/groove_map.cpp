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

// The grid's swing unit, in quarter-note beats: the options are these, in this order.
inline constexpr ef::Division kSwingUnits[] = {{"1/16", 0.25}, {"1/8", 0.5}};

// A rate list is the DSP's divisions, by name, in the DSP's order: the option picked is the index.
template <size_t N>
constexpr bool ratesMatch(int param, const ef::Division (&rates)[N]) {
    const ParamInfo& p = PARAM_INFO[param];
    if (p.nopts != static_cast<int>(N)) return false;
    for (size_t i = 0; i < N; ++i)
        if (!sameName(p.opts[i], rates[i].name)) return false;
    return true;
}

// A list whose options mean the DSP's enum values: option `index` is called `name`.
constexpr bool optionIs(int param, int index, const char* name) {
    const ParamInfo& p = PARAM_INFO[param];
    return index >= 0 && index < p.nopts && sameName(p.opts[index], name);
}
constexpr bool onOff(int param) { return PARAM_INFO[param].nopts == 2 && optionIs(param, 0, "Off") && optionIs(param, 1, "On"); }
constexpr bool targets(int param) {
    return PARAM_INFO[param].nopts == 3 && optionIs(param, Groove::kAll, "All") && optionIs(param, Groove::kDry, "Dry") &&
           optionIs(param, Groove::kEcho, "Echo");
}

} // namespace

// The option lists in surface.py against the DSP's: a picked option is an index into the DSP's list or enum.
static_assert(curvesMatch(), "surface.py PUMP_CURVES must match dsp/pump_curves.h kPumpCurveNames");
static_assert(shapesMatch(P_VOL_SHAPE) && shapesMatch(P_FLT_SHAPE) && shapesMatch(P_PAN_SHAPE),
              "surface.py SHAPES must match the library dsp/shapes.cpp plays (build/shape_data.h)");
static_assert(ratesMatch(P_PUMP_RATE, kPumpRates), "surface.py PUMP_RATES must match dsp/pump.h kPumpRates");
static_assert(ratesMatch(P_VOL_RATE, kShapeRates) && ratesMatch(P_FLT_RATE, kShapeRates) && ratesMatch(P_PAN_RATE, kShapeRates),
              "surface.py SHAPE_RATES must match dsp/shape.h kShapeRates");
static_assert(ratesMatch(P_SWING_UNIT, kSwingUnits), "surface.py's Swing Unit options must be kSwingUnits");
static_assert(onOff(P_PUMP_ON) && onOff(P_SHAPE_ON) && onOff(P_VOL_ON) && onOff(P_FLT_ON) && onOff(P_PAN_ON),
              "surface.py ON_OFF: option 1 is On");
static_assert(targets(P_PUMP_TARGET) && targets(P_SHAPE_TARGET), "surface.py TARGETS must match Groove::Target");
static_assert(PARAM_INFO[P_PUMP_TRIGGER].nopts == 2 && optionIs(P_PUMP_TRIGGER, Pump::kGrid, "Grid") &&
                  optionIs(P_PUMP_TRIGGER, Pump::kInput, "Input"),
              "surface.py's Pump Trigger options must match Pump::Trigger");
static_assert(PARAM_INFO[P_PUMP_BAND].nopts == 3 && optionIs(P_PUMP_BAND, Pump::kFull, "Full") &&
                  optionIs(P_PUMP_BAND, Pump::kLows, "Lows") && optionIs(P_PUMP_BAND, Pump::kHighs, "Highs"),
              "surface.py's Pump Band options must match Pump::Band");
static_assert(PARAM_INFO[P_FLT_TYPE].nopts == 3 && optionIs(P_FLT_TYPE, Shape::kLP, "LP") && optionIs(P_FLT_TYPE, Shape::kBP, "BP") &&
                  optionIs(P_FLT_TYPE, Shape::kHP, "HP"),
              "surface.py's Filter Type options must match Shape::FilterType");

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

namespace {

// A value within a hair of a part's rest is its rest exactly: 0 dB is 0.5 of -24..+24, and a knob that float
// leaves 0.0000005 dB off would wake the level stage, and a Pump at depth 0.0004 the whole Pump, so the chain at
// its defaults would no longer pass its input bit for bit. The hairs are what a knob can't tell from the rest:
// a tenth of a percent, a thousandth of a dB, what the text shows as 0.0 ms.
float snap(float v, float rest, float hair) { return std::fabs(v - rest) < hair ? rest : v; }

constexpr float kHair = 1e-3f;

} // namespace

Groove::Params grooveParams(const float* n) {
    auto v = [n](int id) { return paramValue(id, n[id]); };
    auto rest = [&v](int id, float at, float hair = kHair) { return snap(v(id), at, hair); };
    // The option picked, as an index into the DSP's list or enum.
    auto pick = [&v](int id) { return std::clamp(static_cast<int>(v(id)), 0, PARAM_INFO[id].nopts - 1); };

    Groove::Params p;
    p.pumpOn = pick(P_PUMP_ON) == 1;
    p.shapeOn = pick(P_SHAPE_ON) == 1;
    p.pumpTarget = pick(P_PUMP_TARGET);
    p.shapeTarget = pick(P_SHAPE_TARGET);
    p.swing.amount = rest(P_SWING, 0.5f);
    p.swing.unitBeats = kSwingUnits[pick(P_SWING_UNIT)].beats;
    p.inputDb = rest(P_INPUT, 0.0f);
    p.outputDb = rest(P_OUTPUT, 0.0f);
    p.mix = rest(P_MIX, 1.0f);

    Pump::Params& u = p.pump;
    u.rateBeats = kPumpRates[pick(P_PUMP_RATE)].beats;
    u.curve = pick(P_PUMP_CURVE);
    u.depth = rest(P_PUMP_DEPTH, 0.0f);
    u.length = v(P_PUMP_LENGTH);
    u.pushMs = rest(P_PUMP_PUSH, 0.0f, 0.05f);
    u.trigger = pick(P_PUMP_TRIGGER);
    u.sensitivity = v(P_PUMP_SENS);
    u.band = pick(P_PUMP_BAND);
    u.crossoverHz = v(P_PUMP_XOVER);
    u.mix = rest(P_PUMP_MIX, 1.0f);

    Shape::Params& s = p.shape;
    s.amount = rest(P_SHAPE_DEPTH, 1.0f);
    s.mix = rest(P_SHAPE_MIX, 1.0f);
    s.filterType = pick(P_FLT_TYPE);
    s.cutoffHz = rest(P_FLT_CUTOFF, 20000.0f, 1.0f);
    s.resonance = rest(P_FLT_RESO, 0.0f);
    // The lanes, in Shape's order: each its switch, shape, rate, depth and phase.
    static constexpr int on[] = {P_VOL_ON, P_FLT_ON, P_PAN_ON}, shape[] = {P_VOL_SHAPE, P_FLT_SHAPE, P_PAN_SHAPE},
                         rate[] = {P_VOL_RATE, P_FLT_RATE, P_PAN_RATE}, depth[] = {P_VOL_DEPTH, P_FLT_DEPTH, P_PAN_DEPTH},
                         phase[] = {P_VOL_PHASE, P_FLT_PHASE, P_PAN_PHASE};
    for (int l = 0; l < Shape::kLanes; ++l) {
        Shape::LaneParams& q = s.lane[l];
        q.on = pick(on[l]) == 1;
        q.shape = pick(shape[l]);
        q.rateBeats = kShapeRates[pick(rate[l])].beats;
        q.depth = rest(depth[l], 0.0f);
        q.phase = v(phase[l]);
    }
    return p;
}

} // namespace gf
