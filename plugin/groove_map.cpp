#include "groove_map.h"

#include "../dsp/pump_curves.h"
#include "moves.h"
#include "move_data.h"
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

// The four lines' parameters, line by line: grooveParams reads them through this table, so a key can only land on the field
// its column names.
struct LineIds {
    int on, level, pan, sync, div, ms, fb, mode, ftype, cut, reso, place, drive, wow, mod, mshape, mrate, mcut, mtime, mlevel;
};
#define GF_LINE(X)                                                                                                      \
    {P_##X##_ON,    P_##X##_LEVEL, P_##X##_PAN,   P_##X##_SYNC,  P_##X##_DIV,   P_##X##_MS,  P_##X##_FB,  P_##X##_MODE,   \
     P_##X##_FTYPE, P_##X##_CUT,   P_##X##_RESO,  P_##X##_PLACE, P_##X##_DRIVE, P_##X##_WOW, P_##X##_MOD, P_##X##_MSHAPE, \
     P_##X##_MRATE, P_##X##_MCUT,  P_##X##_MTIME, P_##X##_MLEVEL}
constexpr LineIds kLineIds[Echo::kLines] = {GF_LINE(A), GF_LINE(B), GF_LINE(C), GF_LINE(D)};
#undef GF_LINE

// Every line's lists against the DSP's: the options are the DSP's enums and tables, in the DSP's order.
constexpr bool linesMatch() {
    for (const LineIds& l : kLineIds) {
        if (!onOff(l.on)) return false;
        if (!(PARAM_INFO[l.sync].nopts == 2 && optionIs(l.sync, 0, "Sync") && optionIs(l.sync, 1, "Free"))) return false;
        if (!ratesMatch(l.div, kEchoDivs)) return false;
        if (!(PARAM_INFO[l.mode].nopts == 3 && optionIs(l.mode, EchoLine::kStereo, "Stereo") &&
              optionIs(l.mode, EchoLine::kPingPong, "Ping-Pong") && optionIs(l.mode, EchoLine::kMono, "Mono")))
            return false;
        if (!(PARAM_INFO[l.ftype].nopts == 3 && optionIs(l.ftype, EchoLine::kLP, "LP") && optionIs(l.ftype, EchoLine::kBP, "BP") &&
              optionIs(l.ftype, EchoLine::kHP, "HP")))
            return false;
        if (!(PARAM_INFO[l.place].nopts == 2 && optionIs(l.place, EchoLine::kInLoop, "In Loop") &&
              optionIs(l.place, EchoLine::kOnOutput, "On Output")))
            return false;
        if (!(PARAM_INFO[l.mod].nopts == 4 && optionIs(l.mod, EchoMod::kOff, "Off") && optionIs(l.mod, EchoMod::kLfo, "LFO") &&
              optionIs(l.mod, EchoMod::kEnv, "Env") && optionIs(l.mod, EchoMod::kFollow, "Follow")))
            return false;
        if (!shapesMatch(l.mshape)) return false;
        if (!ratesMatch(l.mrate, kShapeRates)) return false;
    }
    return true;
}

// The Moves: the Move parameter is None and then the sixteen in tile order (moves.cpp holds the tiles to the same names),
// and the page's mode and quantize are Moves' own enums.
constexpr bool movesMatch() {
    const ParamInfo& p = PARAM_INFO[P_MOVE];
    if (p.nopts != kNumMoves + 1 || !sameName(p.opts[0], "None")) return false;
    for (int i = 0; i < kNumMoves; ++i)
        if (!sameName(p.opts[i + 1], kMoves[i].name)) return false;
    return PARAM_INFO[P_MOVE_MODE].nopts == 2 && optionIs(P_MOVE_MODE, Moves::kOneShot, "One Shot") &&
           optionIs(P_MOVE_MODE, Moves::kLoop, "Loop") && PARAM_INFO[P_MOVE_QUANT].nopts == 3 &&
           optionIs(P_MOVE_QUANT, Moves::kSixteenth, "1/16") && optionIs(P_MOVE_QUANT, Moves::kBeat, "Beat") &&
           optionIs(P_MOVE_QUANT, Moves::kBar, "Bar");
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
// The Echo, Time and the Moves (surface.py's ECHO_*, LINE_*, TIME_* and MOVE_* lists).
static_assert(onOff(P_ECHO_ON) && onOff(P_ECHO_FREEZE) && onOff(P_TIME_ON), "surface.py ON_OFF: option 1 is On");
static_assert(PARAM_INFO[P_ECHO_ORDER].nopts == 2 && optionIs(P_ECHO_ORDER, Groove::kEchoFirst, "Echo First") &&
                  optionIs(P_ECHO_ORDER, Groove::kEchoLast, "Echo Last"),
              "surface.py ECHO_ORDERS must match Groove::Order");
static_assert(PARAM_INFO[P_ECHO_ROUTE].nopts == Echo::kRoutings && optionIs(P_ECHO_ROUTE, Echo::kParallel, "Parallel") &&
                  optionIs(P_ECHO_ROUTE, Echo::kSerial, "Serial") && optionIs(P_ECHO_ROUTE, Echo::kPairs, "Pairs") &&
                  optionIs(P_ECHO_ROUTE, Echo::kRing, "Ring") && optionIs(P_ECHO_ROUTE, Echo::kSpread, "Spread"),
              "surface.py ECHO_ROUTES must match Echo::Routing");
static_assert(PARAM_INFO[P_ECHO_GLIDE].nopts == 2 && optionIs(P_ECHO_GLIDE, EchoLine::kTape, "Tape") &&
                  optionIs(P_ECHO_GLIDE, EchoLine::kFade, "Fade"),
              "surface.py ECHO_GLIDES must match EchoLine::Glide");
static_assert(linesMatch(), "surface.py's line lists (LINE_*, ECHO_DIVS, SHAPE_NAMES, SHAPE_RATES) must match the DSP's");
static_assert(PARAM_INFO[P_TIME_PLACE].nopts == 2 && optionIs(P_TIME_PLACE, Groove::kPost, "Post") &&
                  optionIs(P_TIME_PLACE, Groove::kPre, "Pre"),
              "surface.py TIME_PLACES must match Groove::Place");
static_assert(PARAM_INFO[P_TIME_GROOVE].nopts == 4 && optionIs(P_TIME_GROOVE, Time::kOff, "Off") &&
                  optionIs(P_TIME_GROOVE, Time::kSwing, "Swing") && optionIs(P_TIME_GROOVE, Time::kDrunk, "Drunk") &&
                  optionIs(P_TIME_GROOVE, Time::kPush, "Push"),
              "surface.py TIME_GROOVES must match Time::Groove");
static_assert(movesMatch(), "surface.py MOVE_NAMES, MOVE_MODES and MOVE_QUANTS must match plugin/moves.h");

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
    p.dry = rest(P_DRY, 1.0f);
    p.order = pick(P_ECHO_ORDER);
    p.timeOn = pick(P_TIME_ON) == 1;
    p.timePlace = pick(P_TIME_PLACE);
    p.time.groove = pick(P_TIME_GROOVE);
    p.time.amount = v(P_TIME_AMOUNT);   // (a Move's pattern is the engine's: Time::Params::pattern is not a parameter)

    // The Echo: its own controls, then the four lines.
    p.echoOn = pick(P_ECHO_ON) == 1;
    Echo::Params& e = p.echo;
    e.routing = pick(P_ECHO_ROUTE);
    e.send = rest(P_ECHO_SEND, 1.0f);
    e.duck = rest(P_ECHO_DUCK, 0.0f);
    e.freeze = pick(P_ECHO_FREEZE) == 1;
    e.glide = pick(P_ECHO_GLIDE);
    e.width = rest(P_ECHO_WIDTH, 1.0f);
    e.level = rest(P_ECHO_LEVEL, 1.0f);
    e.feedbackOffset = rest(P_ECHO_FB, 0.0f);
    e.toneOct = 4.0f * rest(P_ECHO_TONE, 0.0f);   // -1..1 is four octaves either way
    e.sensitivity = v(P_ECHO_SENS);
    for (int i = 0; i < Echo::kLines; ++i) {
        const LineIds& k = kLineIds[i];
        Echo::Line& q = e.line[i];
        q.on = pick(k.on) == 1;
        q.level = v(k.level);
        q.pan = rest(k.pan, 0.0f);
        q.p.sync = pick(k.sync) == 0;   // Sync | Free
        q.p.divBeats = kEchoDivs[pick(k.div)].beats;
        q.p.timeMs = v(k.ms);
        q.p.feedback = v(k.fb);
        q.p.mode = pick(k.mode);
        q.p.filterType = pick(k.ftype);
        q.p.cutoffHz = rest(k.cut, 20000.0f, 1.0f);   // (a low-pass at 20 kHz is no filter)
        q.p.resonance = rest(k.reso, 0.0f);
        q.p.place = pick(k.place);
        q.p.drive = rest(k.drive, 0.0f);
        q.p.wow = rest(k.wow, 0.0f);
        q.mod.mode = pick(k.mod);
        q.mod.shape = pick(k.mshape);
        q.mod.rateBeats = kShapeRates[pick(k.mrate)].beats;
        q.modCutoff = rest(k.mcut, 0.0f);
        q.modTime = rest(k.mtime, 0.0f);
        q.modLevel = rest(k.mlevel, 0.0f);
    }

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
