#pragma once
// The cases the CPU bench (tools/bench.cpp) times and the profile-guided build's trainer (tools/pgo_train.cpp) plays:
// one header, so the profile is made from what the bench measures. A case is a set of parameters set through the
// plugin's own entry points, so a tool needs nothing of the plugin but the .so (or its objects).
//
// Every setting is read back with getParameter and compared with what was asked for. A key that was renamed, an
// option that moved or a parameter that has become something else stops the run with a message, instead of being
// timed (or trained) as another case: a bench that quietly measures an Echo that never came on is worse than none.
// (A stepped parameter is set to exactly a step, so a read-back that is off is off by a whole step at least.)
//
// The heaviest settings, part by part (docs/PERFORMANCE.md has them in a table):
//   Pump        1/16, band Lows, trigger Input, depth 100%, push -10 ms
//   Shape       three lanes on 1/16, a band-pass at 1 kHz with full resonance, Random Smooth on the filter
//   Echo        four lines in a Ring, In loop band-pass at full resonance, drive and wow 100%, an LFO on each line moving its
//               cutoff, time and level, the duck, a wide wet; and the same with the Fade glide and Ping-Pong lines, which is
//               the heavier: the LFO moves every line's time, so a 50 ms crossfade between two read heads never ends
//   Time        a Move's pattern: Roll Build 1 Bar, going round (a jump every few beats, the last ones every 1/8 of a beat)
//   Everything  all of them (the Echo with the Fade glide), Shape and Pump on All (so on the dry and on the wet), swing 75%
#include "../plugin/vst2.h"
#include "param_ids.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gfc {

using namespace gf;

// --- setting a parameter and reading it back -------------------------------------------------------------------------

// A real value (dB, ms, Hz, 0..1, an option's index) as MPC's 0..1: the curves of groove_map.cpp's paramNorm, which the
// tools don't link (they only have the plugin's entry points), NaN guard included.
inline float norm(int id, float v) {
    const ParamSpec& s = PARAM_SPECS[id];
    float n = 0.0f;
    switch (s.curve) {
        case Curve::Lin:
        case Curve::Int: n = s.hi > s.lo ? (v - s.lo) / (s.hi - s.lo) : 0.0f; break;
        case Curve::Log: n = v > 0.0f ? std::log(v / s.lo) / std::log(s.hi / s.lo) : 0.0f; break;
        case Curve::Pow: n = s.hi > 0.0f && v > 0.0f ? std::cbrt(v / s.hi) : 0.0f; break;
        case Curve::Enum: n = s.hi > 0.0f ? v / s.hi : 0.0f; break;
        default: break;
    }
    return std::isnan(n) ? 0.0f : std::clamp(n, 0.0f, 1.0f);
}

[[noreturn]] inline void fail(const char* what, int id, const char* detail) {
    std::fprintf(stderr, "gfcases: %s: %s%s%s\n", id >= 0 ? PARAM_INFO[id].key : "?", what, *detail ? ", " : "", detail);
    std::exit(1);
}

// MPC's value n for a parameter, read back at once: it must be what was set. Only the sound's parameters (a button
// springs back, a tile is a gesture, a readout can't be set), whose read-back is the value itself.
inline void put(AEffect* e, int id, float n) {
    const Kind k = PARAM_INFO[id].kind;
    if (k != Kind::Synth && k != Kind::Ui) fail("is not a parameter to set (a button, a tile, a stepper or a readout)", id, "");
    n = std::clamp(n, 0.0f, 1.0f);
    e->setParameter(e, id, n);
    const float got = e->getParameter(e, id);
    if (!(std::fabs(got - n) <= 1e-4f)) {
        char b[96];
        std::snprintf(b, sizeof b, "set %.4f, read back %.4f", static_cast<double>(n), static_cast<double>(got));
        fail("did not land", id, b);
    }
}

inline void num(AEffect* e, int id, float v) { put(e, id, norm(id, v)); }

// The index of an option by its name, as surface.py shows it: a renamed option stops the run instead of being measured as another.
inline int optionOf(int id, const char* name) {
    const ParamInfo& p = PARAM_INFO[id];
    for (int o = 0; o < p.nopts; ++o)
        if (!std::strcmp(p.opts[o], name)) return o;
    char b[96];
    std::snprintf(b, sizeof b, "no option \"%s\"", name);
    fail("is not what the case thinks", id, b);
}

inline void opt(AEffect* e, int id, const char* name) { num(e, id, static_cast<float>(optionOf(id, name))); }

// --- the Echo's lines ------------------------------------------------------------------------------------------------

// A line's parameter: the four lines' blocks are alike (surface.py's LINES), so line k's is the first line's plus k blocks.
constexpr int kLineStride = P_B_ON - P_A_ON;
static_assert(P_C_ON - P_B_ON == kLineStride && P_D_ON - P_C_ON == kLineStride, "the four lines' parameters are laid out alike");
static_assert(P_B_MLEVEL - P_B_ON == P_A_MLEVEL - P_A_ON && P_D_MLEVEL - P_D_ON == P_A_MLEVEL - P_A_ON, "line blocks are as long as A's");
constexpr int line(int a, int k) { return a + k * kLineStride; }

// --- the parts at their heaviest -------------------------------------------------------------------------------------

inline void pumpHeaviest(AEffect* e) {
    opt(e, P_PUMP_ON, "On");
    opt(e, P_PUMP_RATE, "1/16");
    opt(e, P_PUMP_BAND, "Lows");
    opt(e, P_PUMP_TRIGGER, "Input");
    num(e, P_PUMP_DEPTH, 1.0f);
    num(e, P_PUMP_PUSH, -10.0f);
}

inline void shapeHeaviest(AEffect* e) {
    opt(e, P_SHAPE_ON, "On");
    opt(e, P_VOL_ON, "On");
    opt(e, P_FLT_ON, "On");
    opt(e, P_PAN_ON, "On");
    opt(e, P_VOL_RATE, "1/16");
    opt(e, P_FLT_RATE, "1/16");
    opt(e, P_PAN_RATE, "1/16");
    opt(e, P_FLT_TYPE, "BP");
    opt(e, P_FLT_SHAPE, "Random Smooth");
    num(e, P_FLT_RESO, 1.0f);
    num(e, P_FLT_CUTOFF, 1000.0f);
    num(e, P_VOL_DEPTH, 1.0f);
    num(e, P_FLT_DEPTH, 1.0f);
    num(e, P_PAN_DEPTH, 1.0f);
}

// Pump and Shape together at their heaviest, swing 75% on 1/16.
inline void bothHeaviest(AEffect* e) {
    pumpHeaviest(e);
    shapeHeaviest(e);
    num(e, P_SWING, 0.75f);
    opt(e, P_SWING_UNIT, "1/16");
}

// The Echo with all four lines running at their heaviest. The lines are panned apart, where a centred pan would let the
// gains be one; `glide` and `mode` are the Echo's Tape or Fade and every line's Stereo, Ping-Pong or Mono.
inline void echo(AEffect* e, const char* glide, const char* mode) {
    opt(e, P_ECHO_ON, "On");
    opt(e, P_ECHO_ROUTE, "Ring");
    opt(e, P_ECHO_GLIDE, glide);
    num(e, P_ECHO_SEND, 1.0f);
    num(e, P_ECHO_DUCK, 1.0f);
    num(e, P_ECHO_WIDTH, 1.3f);
    num(e, P_ECHO_TONE, -0.1f);   // a hair under: the filters never rest at 20 kHz
    num(e, P_ECHO_LEVEL, 1.0f);
    static constexpr float pan[4] = {-0.6f, 0.6f, -0.3f, 0.3f};
    static constexpr const char* rate[4] = {"1/16", "1/8", "1/4", "1/2"};
    for (int k = 0; k < 4; ++k) {
        opt(e, line(P_A_ON, k), "On");
        num(e, line(P_A_LEVEL, k), 1.0f);
        num(e, line(P_A_PAN, k), pan[k]);
        num(e, line(P_A_FB, k), 0.9f);
        opt(e, line(P_A_MODE, k), mode);
        opt(e, line(P_A_FTYPE, k), "BP");
        num(e, line(P_A_CUT, k), 1000.0f);
        num(e, line(P_A_RESO, k), 1.0f);
        opt(e, line(P_A_PLACE, k), "In Loop");
        num(e, line(P_A_DRIVE, k), 1.0f);
        num(e, line(P_A_WOW, k), 1.0f);
        opt(e, line(P_A_MOD, k), "LFO");
        opt(e, line(P_A_MSHAPE, k), "Random Smooth");
        opt(e, line(P_A_MRATE, k), rate[k]);
        num(e, line(P_A_MCUT, k), 1.0f);
        num(e, line(P_A_MTIME, k), 1.0f);
        num(e, line(P_A_MLEVEL, k), 1.0f);
    }
}

inline void echoHeaviest(AEffect* e) { echo(e, "Tape", "Stereo"); }
inline void echoFadeHeaviest(AEffect* e) { echo(e, "Fade", "Ping-Pong"); }

// A Move fired at the next 1/16 of the grid, so that a case has it playing at once. `mode` is "Loop" or "One Shot". The
// Move parameter is the selected Move, and a change of it fires; the mode and the quantize are set before.
inline void fireMove(AEffect* e, const char* move, const char* mode = "Loop") {
    opt(e, P_MOVE_MODE, mode);
    opt(e, P_MOVE_QUANT, "1/16");
    opt(e, P_MOVE, move);
}

// Time on, and a Move that plays from its pattern, going round. (The patterns are only reachable through the Moves:
// Time's pattern is no parameter.)
inline void timeMove(AEffect* e, const char* move) {
    opt(e, P_TIME_ON, "On");
    fireMove(e, move);
}

// The tile a Move's name belongs to (it is lit while the Move plays or waits): -1 when there is no such Move.
inline int tileOf(const char* move) {
    const int o = optionOf(P_MOVE, move);   // 0 is None; the tiles follow it
    return o >= 1 ? P_MOVE_1 + o - 1 : -1;
}

// The two Moves the cases play: Roll Build 1 Bar (the plan's case: a jump every few beats, the last ones every 1/8 of a beat),
// and Stutter 1/16, which jumps every 1/16, so that Time's crossfade (a second read per sample, the weights) runs in 7% of the
// blocks at 128 BPM.
inline constexpr const char* kRollMove = "Roll Build 1 Bar";
inline constexpr const char* kFadesMove = "Stutter 1/16";
inline void timeRoll(AEffect* e) { timeMove(e, kRollMove); }
inline void timeFades(AEffect* e) { timeMove(e, kFadesMove); }

// All four parts at their heaviest, the Pump and the Shape on All (on the dry and on the wet) and Time on a Move.
inline void everythingHeaviest(AEffect* e) {
    bothHeaviest(e);
    opt(e, P_PUMP_TARGET, "All");
    opt(e, P_SHAPE_TARGET, "All");
    echoFadeHeaviest(e);
    timeRoll(e);
}

} // namespace gfc
