#pragma once
// Shape (docs/DESIGN.md, "Shape"): ShaperBox without the drawing. Three lanes, each switched on on
// its own, move the sound along a named shape from the library (dsp/shapes.h), locked to MPC's grid.
// v is the shape's value 0..1 and d the lane's depth times `amount`:
//
//   Volume   gain = 1 - d (1 - v), d 0..1: never a boost
//   Filter   cutoff = cutoffHz 2^(4 d (v - 1)), d -1..1: a resonant state-variable filter (LP, BP or
//            HP) whose cutoff dips from the shape's top by up to four octaves (a negative d rises
//            from it); cutoffHz is where it sits at v = 1
//   Pan      p = d (2 v - 1), d -1..1: Pulse's constant-power balance, theta = pi/4 (1 + p), gains
//            sqrt2 cos(theta) (left) and sqrt2 sin(theta) (right), so L^2 + R^2 stays the input's power
//
// The lanes run in the order Filter, Volume, Pan: a gate that closes also cuts the filter's ring.
//
// Time: a lane's position in its shape is the song position, on straight time (the global swing
// is undone by gf::toStraight, so the steps land late on the swung audio), over the lane's rate,
// plus its phase: cycles = toStraight(beats) / rateBeats + phase. While MPC plays (and says where)
// that is MPC's beat; so a loop, a locate or a tempo change leaves the lane on the grid. Stopped, the
// cycle runs on at the tempo from where it was, and starting to play moves it to the song position.
// The random shapes are seeded by the whole cycle, so a one-bar loop plays the same bar every time.
//
// Per chunk (set() is called once per chunk, 32 samples) each running lane evaluates its shape at the
// chunk's start and end, on the song position the tempo reaches then, and the shape goes in a straight
// line between the two values (no per-sample evaluation). Each sample the lane's value moves toward that
// line by at most a full scale per millisecond: a gate's edge, a Pump shape's fall at its wrap, a locate's
// step are all 1 ms ramps, never faster, on every lane, the filter's too (a gate on the cutoff cannot move
// it four octaves inside a chunk). The middle of an edge lands within half a millisecond of its place on
// the grid. (The filter lane runs the same limiter over the chunk in set(), and its cutoff glides to where
// the value is at the chunk's end.) The rule is for the gains: Volume's gain moves at most d times a full
// scale per millisecond. The pan law steepens the value by up to sqrt2 pi/2 = 2.22 (a side's gain
// moves sqrt2 over a sweep from one side to the other), so the Pan lane's value is limited that much
// harder, and a full sweep takes 2.2 ms: its gains stay within the 1 ms rule too. Only while depth,
// amount, mix or a lane's on/off fade (10 ms) move at the same time can a gain's step pass it, by at most
// 0.11 of the rule.
//
// What the controls change goes through slopes too: depth, amount, mix, a lane switched on or off
// (a fade of 10 ms), the resonance, the filter type (a blend of LP, BP and HP weights) and the cutoff
// (10 octaves in 10 ms). Volume and Pan take them as they take the shape, in a straight line across the
// chunk; the filter's g glides across the chunk (dsp/svf.h's Glide), the cutoff evenly in octaves,
// and its coefficients follow sample by sample, so a swept cutoff makes no click and cannot blow it up.
// The first set() after reset() jumps to its targets instead.
//
// Mix blends gains with unity for Volume and Pan (gain' = 1 + mix (gain - 1)) and dry with wet for
// the filter. Lanes that are off cost nothing, and when none is running process() leaves the buffers
// alone: Params{} passes the input through bit for bit. So does a lane that is on with nothing to do:
// depth 0 (or amount 0, or mix 0) gives Volume and Pan gains of exactly 1. The input of a running lane
// goes through ef::safeIn: a NaN or an infinity is silence, and never gets into the filter's state.
// Only the filter lane rings on (tailSamples()).
#include "common.h"
#include "grid.h"
#include "shapes.h"
#include "svf.h"

namespace gf {

// The rates the surface offers, in quarter-note beats: the LFO divisions from 1/16 to 4 bars.
inline constexpr ef::Division kShapeRates[] = {
    {"1/16", 0.25}, {"1/8T", 1.0 / 3}, {"1/8", 0.5}, {"1/4T", 2.0 / 3}, {"1/8.", 0.75}, {"1/4", 1.0},
    {"1/4.", 1.5}, {"1/2", 2.0}, {"1/2.", 3.0}, {"1 bar", 4.0}, {"2 bars", 8.0}, {"4 bars", 16.0}};
inline constexpr int kNumShapeRates = static_cast<int>(sizeof kShapeRates / sizeof kShapeRates[0]);

class Shape {
public:
    enum Lane : int { kVolume, kFilter, kPan, kLanes };
    enum FilterType : int { kLP, kBP, kHP };

    struct LaneParams {
        bool on = false;
        int shape = 0;            // the library: gf::shapeName
        double rateBeats = 1.0;   // one of kShapeRates (0.0625 .. 64 is taken)
        float depth = 0.0f;       // Volume 0..1; Filter and Pan -1..1
        float phase = 0.0f;       // 0..1 cycles
    };
    struct Params {
        LaneParams lane[kLanes];
        float amount = 1.0f;         // 0..1: scales every lane's depth (the GROOVE page's Shape Depth)
        int filterType = kLP;
        float cutoffHz = 20000.0f;   // 20..20000: the filter at the shape's top (value 1)
        float resonance = 0.0f;      // 0..1: Q 0.7071 .. 12, evenly in octaves of Q
        float mix = 1.0f;            // 0..1
        Swing swing;                 // set by the chain
    };

    Shape();
    void reset();
    void set(const Params& p, const ef::Transport& t);
    void process(float* L, float* R, int n);
    int tailSamples() const { return tail_; }   // 0 unless the filter lane is on (then its ring)

private:
    // A value that goes in a straight line across a chunk: the sample i of it is cur + step i.
    struct Line {
        float cur = 0.0f, step = 0.0f, target = 0.0f;
        void jump(float v) { cur = target = v, step = 0.0f; }
        void to(float v) {   // from where it is, over a chunk
            target = v;
            step = (v - cur) * (1.0f / ef::kChunk);
        }
        void between(float a, float b) {   // from a to b over a chunk
            cur = a;
            to(b);
        }
        float at(int i) const { return cur + step * static_cast<float>(i); }
        void advance(int n) {   // n samples on; a whole chunk lands exactly
            if (n >= ef::kChunk) cur = target;
            else cur += step * static_cast<float>(n);
        }
    };
    // Volume and Pan: `shape` is the line the lane's value `y` (limited sample by sample) follows;
    // c scales it: Volume's mix x depth, Pan's depth (signed); m is Pan's mix.
    struct GainLane {
        bool running = false;
        float y = 0.0f;
        Line shape, c, m;
    };
    // The last shape evaluation of a lane: the next chunk starts where this one ended (unless it jumped).
    struct Eval {
        bool valid = false;
        int shape = 0;
        double cycles = 0.0;
        float value = 0.0f;
    };
    // What glides in the filter: g (L and R, in octaves), 1 / Q, the output mix, the wet share.
    enum : int { G, K, M0, M1, M2, WET, kValues };

    void valuesOf(int lane, const LaneParams& q, double start, double end, float& v0, float& v1);
    void setGainLane(int lane, GainLane& s, const LaneParams& q, float cTarget, float mTarget, double start, double end);
    void setFilter(const Params& p, double start, double end);
    void filterTargets(ef::f2* t) const;

    GainLane vol_, pan_;
    Eval last_[kLanes];

    // The filter lane. fY_ is the limited shape value at the chunk's end; the rest are the controls
    // as they have got to (they slew toward the params), and gl_ glides between chunks' targets.
    bool fRunning_ = false;
    float fY_ = 0.0f, fOct_ = 0.0f, fDepth_ = 0.0f, fRes_ = 0.0f, fWet_ = 0.0f;
    float fType_[3] = {};   // weights of LP, BP and HP
    float fState_[8] = {};  // what the glide's targets were last worked out from
    struct Derived {        // from the parameters, worked out when they change
        float hz = -1.0f, res = -1.0f, d = 2.0f;
        float oct = 0.0f;
        int tail = 0;
    } derived_;
    ef::Glide<kValues, 1> gl_;
    ef::SvfUpdate a_ = {};  // a1..a3 at the glide's targets
    ef::SvfState s_;

    double pos_ = 0.0;        // the song position at the chunk's start, quarter-note beats
    double perSample_ = 0.0;  // beats a sample at the tempo
    bool fresh_ = true;
    int tail_ = 0;
};

} // namespace gf
