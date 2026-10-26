// Moves: see moves.h. The Moves themselves are surface/build/move_data.h.
#include "moves.h"

#include "move_data.h"

#include "../dsp/time.h"

#include <algorithm>
#include <cmath>

namespace gf {

namespace {

// The data is held to the surface it plays on: the tiles' texts are the Move parameter's options after None, in order,
// and the parts a Move switches on are Off | On switches.
constexpr bool sameName(const char* a, const char* b) {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}
constexpr bool namesMatch() {
    if (PARAM_INFO[P_MOVE].nopts != kNumMoves + 1 || !sameName(PARAM_INFO[P_MOVE].opts[0], "None")) return false;
    for (int i = 0; i < kNumMoves; ++i)
        if (!sameName(kMoves[i].name, PARAM_INFO[P_MOVE].opts[i + 1])) return false;
    return true;
}
constexpr bool onOff(int param) {
    return PARAM_INFO[param].nopts == 2 && sameName(PARAM_INFO[param].opts[0], "Off") && sameName(PARAM_INFO[param].opts[1], "On");
}
static_assert(namesMatch(), "the Moves must be the MOVE parameter's options (surface.py MOVE_NAMES), in order");
static_assert(onOff(P_PUMP_ON) && onOff(P_SHAPE_ON) && onOff(P_ECHO_ON) && onOff(P_TIME_ON), "a part's switch is Off | On");
static_assert(Moves::kBar == 2 && Moves::kLoop == 1, "surface.py MOVE_QUANTS and MOVE_MODES");

struct PartSwitch {
    int bit, param;
};
constexpr PartSwitch kPartSwitches[] = {{kPartPump, P_PUMP_ON}, {kPartShape, P_SHAPE_ON}, {kPartEcho, P_ECHO_ON}, {kPartTime, P_TIME_ON}};

constexpr double kQuantBeats[] = {0.25, 1.0, 4.0};   // Moves::Quant: a 1/16, a beat, a bar
constexpr double kQuantGrace = 1.0 / 16.0;           // a late press counts as on the line within this fraction of the quantize (and kGraceMs)
constexpr double kContinuous = 1e-9;                 // the grid is where the last chunk ended, to rounding
constexpr double kLocate = 0.01;                     // a waiting Move's line moves with the grid up to this, else it is looked for again

float unit(float v) { return v > 0.0f ? (v < 1.0f ? v : 1.0f) : 0.0f; }   // NaN: 0
double unit(double v) { return v > 0.0 ? (v < 1.0 ? v : 1.0) : 0.0; }

}   // namespace

int numMoves() { return kNumMoves; }
const MoveDef& moveDef(int i) { return kMoves[std::clamp(i, 0, kNumMoves - 1)]; }
const MoveRamp& moveRamp(int i) { return kMoveRamps[std::clamp(i, 0, kNumMoveRamps - 1)]; }

Moves::Moves() = default;

void Moves::fire(int move, int mode, int quant) {
    if (move < 0 || move >= kNumMoves) return;
    armMove_ = move;
    armMode_ = std::clamp(mode, static_cast<int>(kOneShot), static_cast<int>(kLoop));
    armQuant_ = std::clamp(quant, static_cast<int>(kSixteenth), static_cast<int>(kBar));
    lineSet_ = false;   // looked for at the next chunk, where the grid is
    armed_.store(move, std::memory_order_relaxed);
}

void Moves::stop() {
    armMove_ = -1;
    lineSet_ = false;
    armed_.store(-1, std::memory_order_relaxed);
    for (Run& r : runs_)
        if (r.move >= 0 && r.live) retire(r);
    playing_.store(-1, std::memory_order_relaxed);
}

void Moves::reset() {
    for (Run& r : runs_) r = Run{};
    armMove_ = -1;
    lineSet_ = false;
    clock_ = beat0_ = 0.0;
    fresh_ = true;
    armed_.store(-1, std::memory_order_relaxed);
    playing_.store(-1, std::memory_order_relaxed);
}

// The Move goes out: its ramps glide back from where they are, an option returns at once, its parts are the knobs' again.
void Moves::retire(Run& r) { r.live = false; }

// A slot for a Move that starts: an empty one, else the one gliding out with the least left (it is nearly gone).
Moves::Run* Moves::freeRun() {
    Run* best = nullptr;
    float least = 0.0f;
    for (Run& r : runs_) {
        if (r.move < 0) return &r;
        if (r.live) continue;
        float left = 0.0f;
        for (float w : r.w) left += w;
        if (!best || left < least) best = &r, least = left;
    }
    return best ? best : &runs_[0];
}

void Moves::begin(int move, int mode, double line) {
    for (Run& r : runs_)
        if (r.move >= 0 && r.live) retire(r);   // replaced on its line: its ramps glide back as the new ones come in
    Run* r = freeRun();
    *r = Run{};
    r->move = move;
    r->live = true;
    r->mode = mode;
    r->start = line;
    playing_.store(move, std::memory_order_relaxed);
}

// The grid is somewhere else than where the last chunk ended (a locate, a loop point, the transport starting after a stop):
// what plays keeps its place in its length, so its start goes with the position; a waiting Move's line does too, unless it
// is a long way off, and then the line is looked for again from where the grid is.
void Moves::follow(double jump) {
    if (armMove_ >= 0 && lineSet_) {
        if (std::fabs(jump) > kLocate) lineSet_ = false;
        else line_ += jump;
    }
    for (Run& r : runs_)
        if (r.move >= 0 && r.live) r.start += jump;
}

void Moves::apply(float* norm, const ef::Transport& t, int* timePattern, double* patternStart, bool* loop, int frames) {
    *timePattern = -1;
    *patternStart = 0.0;
    *loop = false;
    frames = std::max(frames, 1);

    // Where the chunk starts and ends on the grid: TimeCore's rule (and clamps), so a pattern's start means the same to both.
    const double bpm = t.bpm >= 20.0 ? std::min(t.bpm, 999.0) : (t.bpm < 20.0 ? 20.0 : 120.0);
    const double bps = bpm / 60.0 / ef::kRate;
    const double beat0 = t.playing && t.valid && std::isfinite(t.beats) ? t.beats : clock_;
    const double end = beat0 + frames * bps;
    if (!fresh_ && std::fabs(beat0 - clock_) > kContinuous) follow(beat0 - clock_);
    fresh_ = false;
    beat0_ = beat0;
    clock_ = end;

    // A Move waits for its line: the next one of the quantize after the grid, or the one the grid is just past.
    if (armMove_ >= 0) {
        if (!lineSet_) {
            const double q = kQuantBeats[armQuant_];
            const double grace = std::min(q * kQuantGrace, kGraceMs * 0.001 * ef::kRate * bps);
            const double since = beat0 - std::floor(beat0 / q) * q;
            line_ = beat0 - since + (since <= grace ? 0.0 : q);
            lineSet_ = true;
        }
        if (line_ < end) {   // it starts in this chunk
            begin(armMove_, armMode_, line_);
            armMove_ = -1;
            lineSet_ = false;
            armed_.store(-1, std::memory_order_relaxed);
        }
    }

    // Where each Move that plays is in its length, and the pattern Time is to play. A One Shot ends when its length is
    // played; a Loop starts its next lap on the lap's line.
    const double handover = TimeCore::kFade * bps;   // beats: Time's crossfade before a line, where a pattern is handed over
    int pattern = -1;
    double pstart = 0.0;
    bool ploop = false;
    int parts = 0;
    for (Run& r : runs_) {
        if (r.move < 0 || !r.live) continue;
        const MoveDef& d = kMoves[r.move];
        const double len = d.lengthBeats > 0.0f ? d.lengthBeats : 1.0;
        const double elapsed = std::max(beat0 - r.start, 0.0);
        if (r.mode == kOneShot && elapsed >= len) {
            r.live = false;
            r.u = 1.0;
            playing_.store(-1, std::memory_order_relaxed);
            continue;
        }
        const double lap = r.mode == kLoop ? std::floor(elapsed / len) : 0.0;
        r.u = unit((elapsed - lap * len) / len);
        parts |= d.partsOn;
        if (d.timePattern >= 0) {
            pattern = d.timePattern;
            pstart = r.start + lap * len;
            ploop = r.mode == kLoop;
            const double next = r.start + (lap + 1.0) * len;
            if (ploop && next - end <= handover) pstart = next;   // the next lap, once its crossfade begins
        }
    }
    if (armMove_ >= 0 && lineSet_) {   // the Move that waits: its pattern from Time's crossfade before its line, its parts a little earlier
        const MoveDef& d = kMoves[armMove_];
        if (d.timePattern >= 0 && line_ - end <= handover) {
            pattern = d.timePattern;
            pstart = line_;
            ploop = armMode_ == kLoop;
        }
        if (line_ - end <= kLeadMs * 0.001 * ef::kRate * bps) parts |= d.partsOn;
    }

    // The ramps: each glides toward being on (inside its window, while its Move plays) or off, and what is in is laid
    // on the knobs. The Adds of every Move add up and are held within 0..1; the Sets of the Move that plays win.
    constexpr int kMax = kRuns * kMaxMoveRamps;
    int addParam[kMax], setParam[kMax];
    float addSum[kMax], setValue[kMax];
    int nAdd = 0, nSet = 0;
    const float inStep = frames / static_cast<float>(kGlideInMs * 0.001 * ef::kRate);
    const float outStep = frames / static_cast<float>(kGlideOutMs * 0.001 * ef::kRate);
    for (int pass = 0; pass < 2; ++pass) {   // those gliding out first, then the one that plays
        for (Run& r : runs_) {
            if (r.move < 0 || r.live != (pass == 1)) continue;
            const MoveDef& d = kMoves[r.move];
            float left = 0.0f;
            for (int k = 0; k < d.rampCount && k < kMaxMoveRamps; ++k) {
                const MoveRamp& rp = kMoveRamps[d.rampFirst + k];
                const bool on = r.live && r.u >= rp.from && r.u < rp.to;
                const float target = on ? 1.0f : 0.0f;
                float& w = r.w[k];
                w = target > w ? std::min(w + inStep, target) : std::max(w - outStep, target);
                left += w;
                if (rp.param < 0 || rp.param >= P_COUNT) continue;
                if (rp.kind == kMoveSet) {
                    if (on && nSet < kMax) setParam[nSet] = rp.param, setValue[nSet++] = rp.amount;
                    continue;
                }
                if (!(w > 0.0f)) continue;
                const double at = rp.to > rp.from ? unit((r.u - rp.from) / (static_cast<double>(rp.to) - rp.from)) : 0.0;
                const float shaped = rp.shape == kMoveRise ? static_cast<float>(at) : (rp.shape == kMoveFall ? static_cast<float>(1.0 - at) : 1.0f);
                const float change = rp.amount * shaped * w;
                int j = 0;
                while (j < nAdd && addParam[j] != rp.param) ++j;
                if (j == nAdd) {
                    if (nAdd >= kMax) continue;
                    addParam[nAdd] = rp.param, addSum[nAdd++] = 0.0f;
                }
                addSum[j] += change;
            }
            if (!r.live && !(left > 0.0f)) r.move = -1;   // glided out: the slot is free
        }
    }
    for (int j = 0; j < nAdd; ++j) norm[addParam[j]] = unit(norm[addParam[j]] + addSum[j]);
    for (int j = 0; j < nSet; ++j) norm[setParam[j]] = unit(setValue[j]);
    for (const PartSwitch& s : kPartSwitches)
        if (parts & s.bit) norm[s.param] = 1.0f;

    *timePattern = pattern;
    *patternStart = pattern >= 0 ? pstart : 0.0;
    *loop = pattern >= 0 && ploop;
}

} // namespace gf
