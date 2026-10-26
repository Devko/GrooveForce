#pragma once
// Pump: Kickstart, locked to MPC's grid. The input is ducked at the start of every step of the grid
// (or at every hit of the input) and comes back along a named curve (pump_curves.h), so a bass or a
// pad breathes with the kick. Rate, curve, depth, length, push, trigger and band are in Params.
//
// The model. Everything runs on MPC's song position, in beats, so a loop, a locate, a tempo change
// or a stopped transport can't put the Pump off the grid: each chunk starts from where MPC says the
// song is (playing and valid; the first chunk after reset() whenever valid), or from where the last
// chunk ended, advanced at the tempo. A step's
// bottom is where the duck reaches its lowest: step k of a rate r is straight time k r, which lands
// in the audio at toSwung(k r) (grid.h: 66% swing puts the second 16th 40 ms late at 120 BPM), plus
// the push, which is in milliseconds whatever the swing. The chunk's bottoms are worked out as
// sample offsets: the one just before the chunk and those up to a millisecond past its end.
//
// The recovery's length is `length` of a step in beats at the tempo, whatever the swing: a short,
// swung step is cut off by the next drop, as a compressor's release is.
//
// The level (1: the input as it is, 0: the bottom) is the lower of two things. After a bottom, the
// recovery: pumpLevel(curve, x), x the time since the bottom over `length` of a step. Before the
// next bottom, a drop of kEdgeMs (a hair over 1 ms) that ends on it: the next bottom is known from the song
// position, so a duck can start before the beat and nothing looks ahead in the audio. A slope
// limiter then keeps every edge at 1 ms full scale or slower, whatever the curve, the length (a
// recovery of a few milliseconds), a jump of the song position, a change of tempo or a knob: the
// level slews to its target, like the shortest edge of Pulse's gate. The gain is
// 1 - depth mix (1 - level), the same on both sides.
//
// Input trigger: the bottoms come from the audio instead. A detector (transient.h) listens to the
// input through a 150 Hz low-pass (a kick, not the hats over it) and each onset schedules a bottom
// `push` later, and not sooner than the 1 ms the drop needs: without look-ahead the duck starts when
// the hit is heard (the bottom comes 1.5 to 5 ms after a kick starts: the low-pass's delay, the
// detector's, and the drop). The detector is never reset while audio runs (a reset makes the next
// sample an onset); on starting up, or when Input is picked, its onsets are left alone for 60 ms
// while it warms up on the audio.
//
// The key: process() can be handed a key, a stereo signal the detector listens to (through the same
// low-pass) instead of the Pump's own input. A pad is ducked by the kick next to it, and the Pump on
// the Echo hears the dry kick (Rumble). Null keys mean the Pump's own input, which is what the three
// argument process() does. The key is only read: it may be the input's own buffers, or the chain's dry
// ones, and a NaN or an infinity in it is silence, as in the input.
//
// Bands: Lows or Highs duck one side of a Linkwitz-Riley split (crossover.h) and let the other
// through, the sum of the two being flat; Full ducks the input as it is. The split is only run while
// something is ducked: at depth 0 (or mix 0) the Pump leaves the audio alone, bit for bit, and takes
// the crossover out with a 10 ms crossfade (the split's allpass phase differs from the input's). The
// crossover's cutoff follows the knob through a one-pole on log frequency (20 ms): a one-chunk
// leap across the range would swell a tone near it. The split is an allpass in the path while it is
// live (at any depth over 0): it can raise the peaks of an edgy waveform, by up to about +9 dB on a
// full-scale square wave.
//
// Depth and mix glide (two 5 ms one-poles); band, trigger, curve, length, rate, push and swing
// changes take the level's slope limiter or a 10 ms crossfade. reset() forgets everything and the next
// set() jumps to its targets, and starts the grid from MPC's song position: the first set() after
// reset() takes t.beats when it is valid, playing or not (stopped, the grid runs on from there at the
// tempo; it used to start from beat 0, so its first bottom came at the wrong time). Without a valid
// position it starts from 0.
#include "common.h"
#include "crossover.h"
#include "grid.h"
#include "pump_curves.h"
#include "svf.h"
#include "transient.h"

namespace gf {

inline constexpr ef::Division kPumpRates[] = {{"1/16", 0.25}, {"1/8", 0.5}, {"1/4", 1.0}, {"1/2", 2.0}, {"1 bar", 4.0}};

class Pump {
public:
    enum Trigger : int { kGrid, kInput };
    enum Band : int { kFull, kLows, kHighs };
    struct Params {
        double rateBeats = 1.0;     // one of kPumpRates
        int curve = PC_SMOOTH;
        float depth = 0.0f;         // 0..1: 1 is silence at the bottom
        float length = 0.6f;        // 0.05..1: the recovery's share of a step
        float pushMs = 0.0f;        // -10..30: the bottom this far from the step's start (Input: 0..30)
        int trigger = kGrid;
        float sensitivity = 0.5f;
        int band = kFull;
        float crossoverHz = 120.0f; // 60..300
        float mix = 1.0f;           // gain' = 1 + mix (gain - 1)
        Swing swing;                // set by the chain (Groove::stepSwing)
    };

    static constexpr float kEdgeMs = 1.001f;   // the shortest edge: a hair over 1 ms, so rounding never makes a step over 1/44.1
    static constexpr int kTail = 1000;         // the crossover's ring, in samples

    Pump();
    void reset();
    void set(const Params& p, const ef::Transport& t);
    void process(float* L, float* R, int n) { process(L, R, n, nullptr, nullptr); }
    void process(float* L, float* R, int n, const float* keyL, const float* keyR);   // Input listens to the key (both or neither), else to L / R
    int tailSamples() const { return band_ != kFull || fade_ > 0.0f ? kTail : 0; }   // 0 in Full; the crossover's ring when banded

private:
    static constexpr int kMaxBottoms = 6;      // bottoms near one chunk
    static constexpr int kMaxPending = 4;      // Input: bottoms scheduled and not yet reached
    static constexpr float kEdgeSamples = kEdgeMs * 0.001f * ef::kRate;
    static constexpr float kSlope = 1.0f / kEdgeSamples;                 // the level's fastest move a sample
    static constexpr float kFadeStep = 1.0f / (0.010f * ef::kRate);      // a 10 ms crossfade
    static constexpr float kDetectorHz = 150.0f;                         // the low-pass in front of the detector
    static constexpr int kDetectorWarmup = static_cast<int>(0.060f * ef::kRate);
    static constexpr double kNever = -1e18;    // a bottom that never was

    int gridBottoms(double* d, int n) const;
    int inputBottoms(double* d, int n) const;
    void listen(const float* L, const float* R, int n);
    void glideAmount(int n);
    void advance(int n);

    // From set().
    double rate_ = 1.0, lengthBeats_ = 0.6, pushMs_ = 0.0, bps_ = 0.5 / ef::kRate, b0_ = 0.0;   // bps_: beats a sample
    int curve_ = PC_SMOOTH, band_ = kFull, trigger_ = kGrid;
    float amountT_ = 0.0f, xoTarget_ = 0.0f;   // depth x mix; the crossover's cutoff, log2 Hz
    float lowT_ = 1.0f, highT_ = 1.0f;         // how much of the duck each band takes
    bool bandT_ = false;                       // the split should be running
    bool fresh_ = true;
    Swing swing_;

    // State.
    double pos_ = 0.0;         // the song position the next chunk starts at, if MPC doesn't say
    double clock_ = 0.0;       // samples gone by (Input's bottoms are in them)
    float level_ = 1.0f;       // the slope-limited level
    bool sync_ = true;         // the next level starts at its target
    float amount1_ = 0.0f, amount_ = 0.0f;   // depth x mix through its two one-poles
    float fade_ = 0.0f;        // the split's share of the output, 0..1
    float low_ = 1.0f, high_ = 1.0f;
    float xoLog_ = 0.0f;
    bool splitLive_ = false;   // the crossover holds a signal
    double pending_[kMaxPending] = {};
    int numPending_ = 0;
    double last_ = kNever;     // the latest bottom reached
    int mute_ = 0;             // samples the detector's onsets are not taken, warming up

    TransientDetector det_;
    ef::SvfState lp_;
    ef::SvfUpdate lpA_ = {};
    Crossover xo_;
};

} // namespace gf
