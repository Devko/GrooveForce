#pragma once
// Echo (docs/DESIGN.md, "Echo"): Sigmund 2's depth on EffectForce's delay. Four stereo lines, A to D
// (dsp/echo_line.h), each with a modulator of its own (dsp/echo_mod.h), wired by one of five routings
// and heard at a level and pan each. The Echo makes the wet bus; the chain (Groove, plan 2) adds it to
// the dry, ducks, shapes and pumps it as its target says.
//
// A chunk (n <= 32 samples) goes: the modulators step, every line that runs reads (all four first,
// since the minimum time of 1 ms is more than a chunk), the heard sum is made, and then the routing
// writes each line's input. In the table x is the send (the input, sanitised, times the send) and y the
// lines' reads:
//
//   Parallel   A, B, C, D <- x
//   Serial     A <- x   B <- yA   C <- yB   D <- yC
//   Pairs      A <- x   B <- yA   C <- x    D <- yC
//   Ring       A <- x + yD   B <- yA   C <- yB   D <- yC
//   Spread     A <- x   B, C, D <- yA
//
// What a routing passes from one line to another is the source line's read times the line's loopScale()
// (1 / its filter's peak gain when that is over 1: a resonant low-pass or high-pass In loop), so the
// loop gain round any routing never exceeds the feedback and a Ring cannot build up. What is heard is
// not scaled: each line that is on is heard at its Level and Pan whatever the routing. A line that is
// off is neither read nor written, and gives a routing nothing (a Serial chain with B off passes nothing
// to C and D). A routing change crossfades the lines' inputs: the design says over one chunk (32
// samples, 0.73 ms), but a chunk is faster than the plugin's rule that no gain edge is faster than 1 ms,
// so it takes a hair over 1 ms (45 samples) and ends in the next chunk. The crossfade moves each of the
// five coefficients a line's input is made of (x, yA..yD) from where it is to the routing's 0 or 1.
//
// Lines. On/off fades the line over 10 ms: its input (so what it records starts and ends gently) and
// its output (what is heard and what it feeds on). Off and faded out it is not touched: no read, no
// write, no set, no modulator, so it costs nothing. Coming on again it starts clean (reset(), and the
// modulator's reset(): a stopped LFO starts its cycle again, the Env warms up for 60 ms) unless it is
// switched back on before its fade-out is over. A line's Level, Pan (constant power, Shape's Pan law:
// left cos a - sin a, right cos a + sin a with a = pi/4 pan: 1 and 1 in the middle, sqrt2 and 0 at the
// edges) and modLevel go to two gains that move no faster than a full scale in a hair over 1 ms, so
// nothing clicks whatever a control or a modulator does.
//
// Modulators. Each line's modulator makes one value 0..1 per chunk; next() is called every chunk for
// every line that runs (it advances the stopped grid and the detector). The key is the chain's input
// (R26), not the send's source: an Env or Follow hears what the Echo is fed, never its own repeats. The
// value is slewed (a full scale in a hair over 1 ms: next() steps on a shape's edge, a hit, a locate, a
// mode change) before it moves anything, and moves three things by an amount each, all at the
// modulator's 1 (at 0 nothing moves): the cutoff by modCutoff x 4 octaves, the time by modTime x 2%, and
// the level, which dips to 1 - modLevel at the modulator's 0. The lines limit the rest themselves (the
// cutoff slews at most an octave a millisecond, the time glides by Tape or Fade). Each line runs its own
// detector or follower over the key (Envs on all four lines cost about 5k ARM instructions a chunk more
// than LFOs do; four Envs with one sensitivity could share a detector, which this does not). The Env
// modulators share one sensitivity (the Echo's).
//
// The Echo's controls:
//   send          the input into the lines, 0..1 (a gain, slewed). Only the first line of a routing hears it,
//                 or all four in Parallel, or A and C in Pairs.
//   duck          the wet ducks under the KEY with EffectForce's law: the key's louder side (up to +18 dBFS)
//                 through a 5 ms attack, 250 ms release envelope, gain 1 / (1 + 16 duck env): at duck 1,
//                 -6 dB with the key at -24 dBFS, -19 dB at -6 dBFS. Only the heard wet ducks, not the lines,
//                 so the repeats bloom in the gaps. The gain is worked out once a chunk and slewed.
//   freeze        the send closes and every line's feedback goes to 1, both along a straight line over
//                 20 ms (the lines then slew their feedback a full scale in 10 ms): the lines loop what they
//                 hold. The tail is then endless.
//   glide         Tape or Fade, for every line.
//   width         M/S on the wet, 0 (mono) .. 1.5; 1 is the wet untouched, bit for bit.
//   level         the wet into the chain, 0..1.
//   feedbackOffset  added to every line's feedback, then held to 0..1.
//   toneOct       octaves added to every line's cutoff (a low-pass at 20 kHz is a filter at all once this
//                 takes it under, so -2 darkens every line to 5 kHz).
//   sensitivity   the Env modulators' detector.
//   swing         the LFOs' grid (Groove::stepSwing).
// Width, level and the send move no faster than a full scale in a hair over 1 ms too.
//
// Memory: allocate() (UI thread; the plugin calls it the first time the Echo is switched on) makes all
// four lines (about 4.2 MB, 1 bar at 80 BPM and the wow's depth, stereo, each) and hands them over through
// an atomic pointer, as EffectForce's looper does: the audio thread takes the pointer at its next set(),
// and never touches a line before. Until then the wet is silent, nothing is read or written, and
// running() is 0. (All four, not the ones that are on: the call has no parameters, and a line switched
// on later must not wait for memory.) Lines taken up this way fade in over 10 ms like any line.
//
// Bounds. Every line writes under 0 dBFS (its limiter), so with the filters off a read is at most 1.25
// (the Hermite overshoot) and the wet at most 4 x 1.25 = 5 at the default pan and width (a Ring of
// full-scale noise at feedback 1 reaches 4.2), 4 x 1.25 x sqrt2 = 7.1 with every line panned hard, and a
// side of the width at 1.5 is at most 1.5 times the sum's. A resonant filter In loop can put out more than
// was written to it (its peak gain, up to 12): the line's loopScale() keeps that out of the routings,
// not out of the ears. Input and key may be anything: NaN and infinity are silence, finite input is held
// to +-4 before any sum (the device's float maths turns 0.5 a + 0.5 b into 0.5 (a + b), which
// overflows for +-3e38), so the wet is finite for any input and any parameters.
//
// tailSamples(): the longest ring of a line that runs, along its routing (Serial: the lines in a row add
// up; Pairs and Spread: the longest path), endless when frozen or when a Ring of all four goes round.
#include "common.h"
#include "echo_line.h"
#include "echo_mod.h"
#include "grid.h"

#include <atomic>

namespace gf {

class Echo {
public:
    static constexpr int kLines = 4;
    static constexpr int kRoutings = 5;
    enum Routing : int { kParallel, kSerial, kPairs, kRing, kSpread };
    struct Line {
        bool on = false; float level = 1.0f; float pan = 0.0f;   // pan -1..1, constant power
        EchoLine::Params p;           // timeMod and cutoffModOct are the Echo's (the modulator, tone): what is set here is replaced
        EchoMod::Params mod;          // sensitivity and swing are the Echo's
        float modCutoff = 0.0f;   // -1..1: ±4 octaves at the modulator's swing
        float modTime = 0.0f;     // -1..1: ±2% of the time
        float modLevel = 0.0f;    // 0..1: the level dips to 1 - modLevel at the modulator's 0
    };
    struct Params {
        Line line[kLines];
        int routing = kParallel;
        float send = 1.0f;          // 0..1: the input into the lines
        float duck = 0.0f;          // 0..1: the wet ducks under the key (EffectForce's law)
        bool freeze = false;        // feedback to 1 and the send closed, glided 20 ms
        int glide = EchoLine::kTape;
        float width = 1.0f;         // 0..1.5: M/S width of the wet
        float level = 1.0f;         // 0..1: the wet into the chain
        float feedbackOffset = 0.0f;// -1..1 added to every line's feedback (clamped 0..1)
        float toneOct = 0.0f;       // -4..4 octaves on every line's cutoff
        float sensitivity = 0.5f;   // the Env modulators' detector
        Swing swing;                // the LFOs' grid (Groove::stepSwing)
    };

    Echo();                          // no allocation
    ~Echo();
    Echo(const Echo&) = delete;
    Echo& operator=(const Echo&) = delete;
    bool allocate();                 // UI thread, the first time the Echo is switched on; false if out of memory
    bool allocated() const { return store_.load(std::memory_order_acquire) != nullptr; }   // until then, the wet is silent and nothing runs
    void reset();
    void set(const Params& p, const ef::Transport& t);
    // in: the send's source; key: the chain's input (duck and the Env / Follow modulators listen to it); the
    // wet comes out in wetL / wetR (written, not added to; they may be the input buffers). n <= kChunk.
    void process(const float* inL, const float* inR, const float* keyL, const float* keyR, float* wetL, float* wetR, int n);
    int tailSamples() const;

    int running() const { return running_; }   // lines read and written in the last process(): 0 when nothing runs
    // The largest magnitude the four lines hold (for tests): the limiters keep it at 1. Infinity if anything
    // in them is not a number. 0 before allocate().
    float peakStored() const;

private:
    struct Store {
        EchoLine line[kLines];
    };
    // A line as the Echo sees it.
    struct Slot {
        float fade = 0.0f;       // 0..1: the share of the line that is fed, heard and passed on
        float goal = 0.0f;       // where the fade is going
        bool dirty = false;      // has run since it was last reset
        bool runs = false;       // runs in this chunk (set())
        bool live = false;       // ran in the last chunk (else the values jump to where they are going)
        bool fading = false;     // the fade moves in this chunk
        float mod = 0.0f;        // the modulator's value as it has got to (slewed)
        float pan = 2.0f;        // what panL / panR were worked out for (2: nothing yet)
        float panL = 1.0f, panR = 1.0f;
        ef::Ramp gl, gr;         // the heard gains, left and right
    };

    void arm(int i, bool jump);
    void wake();

    std::atomic<Store*> store_{nullptr};
    Store* st_ = nullptr;        // the audio thread's copy of store_

    Params p_;
    ef::Transport t_;
    Slot slot_[kLines];
    EchoMod mod_[kLines];
    ef::Ramp send_, level_, width_, duckGain_;
    float freeze_ = 0.0f;        // 0..1: the freeze as it has got to
    float duckEnv_ = 0.0f;
    // The routing's coefficients as they have got to: the lines' inputs are wx x + sum wy[k] feed[k].
    float wx_[kLines] = {}, wy_[kLines][kLines] = {};
    bool xfade_ = false;         // the coefficients are on their way to the routing's
    bool fresh_ = true;          // the next set() jumps
    bool active_ = false;        // something runs in this chunk
    bool awake_ = false;         // the controls' values are where they are going (they jump when the Echo wakes)
    int running_ = 0;

    // A chunk's signals.
    float yL_[kLines][ef::kChunk], yR_[kLines][ef::kChunk];       // the reads, faded
    float sL_[kLines][ef::kChunk], sR_[kLines][ef::kChunk];       // the reads times loopScale, where that is not 1
    float inL_[kLines][ef::kChunk], inR_[kLines][ef::kChunk];     // the inputs made for the lines, where they are not x or a feed as it is
    float fadeBuf_[kLines][ef::kChunk];
    float xL_[ef::kChunk], xR_[ef::kChunk];                        // the send
    float hL_[ef::kChunk], hR_[ef::kChunk];                        // the heard sum
    float zero_[ef::kChunk] = {};
};

} // namespace gf
