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
//   Ring       A <- x + yD   B <- yA   C <- yB   D <- yC   (with all four on; with fewer it closes over those: below)
//   Spread     A <- x   B, C, D <- yA
//
// What a routing passes from one line to another is the source line's read times the line's loopScale()
// (R36: 1 / its filter's peak gain when that is over 1, a resonant low-pass or high-pass In loop), so a
// resonance never raises what is passed on. What is heard is not scaled: each line that is on is heard at
// its Level and Pan whatever the routing. A line that is off is neither read nor written, and gives a
// routing nothing (a Serial chain with B off passes nothing to C and D; a Ring closes over the others). A routing change crosses the
// lines' inputs over time: the design says one chunk (32 samples, 0.73 ms), but a chunk is faster than
// the plugin's rule that no gain edge is faster than 1 ms, so it takes a hair over 1 ms (45 samples,
// ruling R40) and ends in the next chunk. The crossfade moves each of the five coefficients a line's
// input is made of (x, yA..yD) from where it is to the routing's 0 or 1, and the feed gain below.
//
// What the feedback knobs mean (rulings R39, R44, R45, R46). In Serial, Pairs and Spread, which have no cycle,
// every line keeps its own feedback (its read, filtered In loop, fed back to its own input: a line of repeats) and
// passes y x loopScale on at gain 1, feed-forward and stable. In Ring, the one routing with a cycle, a line's
// feedback is its SEND round the ring and the line has NO feedback of its own. The ring closes over the lines that
// are on: each takes the previous one's feed (fb x loopScale x y of that line), the first of them also takes the
// send x, and a lone line feeds itself, which makes it a line with that feedback. So line i writes x_i + fb(prev) x
// loopScale(prev) x y(prev), where "fb" is the feedback with the offset and the freeze, and the ring's loop gain
// is the product of the feedbacks of the lines that are on, at most 1: it never runs away (feedback 1 on all of
// them holds it, and the limiters only ever catch drive or a sum of the send and the ring), and a 0 on any line opens
// it. Switching a line on or off re-wires the ring through the routing crossfade, a line fading out leaves the
// ring at once and rings out on its own. Frozen (R44), the feeds between lines stop in the feed-forward routings
// as the send does (the lines hold what they have: a feed that went on would swell the lines downstream into their
// limiters); in a Ring the feedbacks go to 1, which holds it. A ping-pong line in a Ring (R45) runs as Stereo
// inside, because the cross-over of its repeats lives in the own feedback a Ring does not have: it takes the send as
// its mono sum in L only, and passes its feed on with L and R swapped, so the ping-pong goes round the ring (one
// ping-pong line swaps the sides every trip, four of them every pass) without a loss. The gain a feed is passed on
// at (1 in the feed-forward routings, fb x loopScale in a Ring, 0 frozen) is slewed like any gain, so a switch into
// or out of a Ring is as clean as any routing change.
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
// cutoff slews at most an octave a millisecond, the time glides by Tape or Fade). One transient detector
// and one follower run on the key for the whole Echo (R41), only in the chunks where some line that runs
// is in Env or Follow, and each line's modulator is given what they found (EchoMod::nextShared): every Env
// falls at its own rate, and the detector's sensitivity is the Echo's.
//
// The Echo's controls:
//   send          the input into the lines, 0..1 (a gain, slewed). Only the first line of a routing hears it
//                 (in a Ring the first that is on), or all four in Parallel, or A and C in Pairs.
//   duck          the wet ducks under the KEY with EffectForce's law: the key's louder side (up to +18 dBFS)
//                 through a 5 ms attack, 250 ms release envelope, gain 1 / (1 + 16 duck env): at duck 1,
//                 -6 dB with the key at -24 dBFS, -19 dB at -6 dBFS. Only the heard wet ducks, not the lines,
//                 so the repeats bloom in the gaps. The gain is worked out once a chunk and slewed.
//   freeze        the send closes and every line's feedback goes to 1, and the feeds from line to line stop (in a
//                 Ring they are the feedbacks: they go to 1 and hold the ring), all along a straight line over
//                 20 ms (the lines then slew their own feedback a full scale in 10 ms): the lines loop what they
//                 hold, and nothing new gets into them. The tail is then endless.
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
// full-scale noise at feedback 1 reaches 3.4), 4 x 1.25 x sqrt2 = 7.1 with every line panned hard, and a
// side of the width at 1.5 is at most 1.5 times the sum's. A resonant filter In loop can put out more than
// was written to it (its peak gain, up to 12): the line's loopScale() keeps that out of the loops and the
// routings, not out of the ears. Input and key may be anything: NaN and infinity are silence, finite input
// is held to +-4 before any sum (the device's float maths turns 0.5 a + 0.5 b into 0.5 (a + b), which
// overflows for +-3e38), so the wet is finite for any input and any parameters.
//
// tailSamples(): the longest ring of a line that runs, along its routing (Serial: the lines in a row add
// up; Pairs and Spread: the longest path), endless when frozen or when a Ring of all four goes round with
// feedbacks whose product is 1; below that, the trips round it take to fall 60 dB.
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
        EchoLine::Params p;           // timeMod and cutoffModOct are the Echo's (the modulator, tone): what is set here is replaced; feedback: see R39 in the header
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
        bool freeze = false;        // feedback to 1 (every line's, or in a Ring its send round the ring) and the send closed, glided 20 ms
        int glide = EchoLine::kTape;
        float width = 1.0f;         // 0..1.5: M/S width of the wet
        float level = 1.0f;         // 0..1: the wet into the chain
        float feedbackOffset = 0.0f;// -1..1 added to every line's feedback (clamped 0..1); in a Ring, to its send round the ring
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
    ef::Ramp feedG_[kLines];     // the gain each line's read is passed on at: loopScale (x the feedback in a Ring, x (1 - freeze) elsewhere)
    ef::Ramp swap_[kLines];      // R45: how far a ping-pong line in a Ring passes its feed on with the sides swapped, 0..1
    bool detLive_ = false;       // the shared detector / follower ran in the last chunk (else they start from nothing: R47)
    bool folLive_ = false;
    TransientDetector det_;      // R41: one for the whole Echo, on the key
    KeyFollower fol_;
    float sens_ = -1.0f;         // what the detector is set to (-1: nothing yet)
    float freeze_ = 0.0f;        // 0..1: the freeze as it has got to
    float duckEnv_ = 0.0f;
    // The routing's coefficients as they have got to: the lines' inputs are wx x + sum wy[k] feed[k].
    float wx_[kLines] = {}, wy_[kLines][kLines] = {};
    bool wireX_[kLines] = {};    // the wiring those head for, for the lines that are on (a Ring closes over them: R46)
    int wireSrc_[kLines] = {};   // the line whose feed line i takes, or -1
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
    float swapBuf_[kLines][ef::kChunk];                            // R45: the swap's share per sample, where it is not 0
    float xpL_[kLines][ef::kChunk], xpR_[kLines][ef::kChunk];      // the send as a ping-pong line in a Ring takes it
    float xL_[ef::kChunk], xR_[ef::kChunk];                        // the send
    float hL_[ef::kChunk], hR_[ef::kChunk];                        // the heard sum
    float zero_[ef::kChunk] = {};
};

} // namespace gf
