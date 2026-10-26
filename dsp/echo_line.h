#pragma once
// EchoLine (docs/DESIGN.md, "Echo"): one of the Echo's four lines. EffectForce's Delay (dsp/delay.h)
// taken apart: the same stereo line (double-precision time, Tape and Fade glides, a cubic drive, the
// limiter, wow, Hermite reads, the cheap reset), but with the Echo's filter instead of its two cuts and
// its process() split in two, so that a routing can wire the lines to each other.
//
// Two phases per chunk. read() gives the line's output for the next n samples, taken from what was
// written at least 1 ms ago (so more than a chunk and a Hermite tap ago); write() then takes n samples
// of input, adds the line's own feedback to them and puts them in the line. The Echo reads all four
// lines first and writes them after, each with the input its routing has made from the others' outputs.
// A line's own feedback In loop is the same as wiring its output to its own input: what read() hands
// out is what the feedback is made of, so a Ring of one line at 1 ms gives the same repeats as feedback
// inside write() (checked in the tests). On output the loop carries the plain read, not the output.
//
// The loop, per side:
//   read:   the line at the time -> (the filter) -> the output
//   write:  input + feedback x (In loop: the filtered read; On output: the plain read)
//           -> low cut at 30 Hz -> drive -> limiter -> the line
// In loop the filter is part of the loop: each repeat is filtered again, and the output is the filtered
// read. On output it only shapes what is heard: every repeat comes out through it once, and the loop
// carries the plain read. Switching between them crossfades over 10 ms. The filter is Shape's, an SVF
// (dsp/svf.h) that is a low-pass, band-pass or high-pass of resonance 0..1 (Q 0.7071 .. 12), bypassed
// altogether while it is a low-pass at 20 kHz or above, so the defaults change nothing. Its cutoff
// follows a modulator through cutoffModOct (octaves, added to the cutoff this chunk). The cutoff slews
// at most 10 octaves in 10 ms, the controls a full scale in 10 ms, so nothing clicks when they move.
// A resonant low-pass or high-pass has a gain over 1 at its cutoff, so with a high feedback In loop a
// strong resonance builds a tone there, which the limiter keeps under 0 dBFS (the band-pass peaks at 1).
//
// What is written always goes through a one-pole low cut at 30 Hz, whether it came in as input or as
// feedback, so nothing builds up below the audible range, a DC offset never gets into the loop, and a
// Ring of lines can't collect it either. It is a trapezoidal one-pole (g = tan(pi 30 / rate), its low-pass
// follows its input by g / (1 + g) of the way a sample, the output is what the low-pass leaves): no gain
// over 1 at any frequency, 1 at Nyquist, 0.99989 at 2 kHz, so a repeat loses about 0.001 dB to it in the
// audible range. An impulse comes back at h = 1 / (1 + g) = 0.9979 of its size, and a lone repeat n of it
// at h^n times what feedback alone makes. The drive is a cubic soft clipper, c - 4/3 c^3 with c = u clamped
// to +-1/2 (unity gain for small signals, flat at +-1/3 from +-1/2 on), blended in by the drive amount: 0 is
// exactly linear. The limiter keeps what is written under 0 dBFS (instant attack, 100 ms release, after
// a clamp at +-4 on the input and again in front of the low cut, so a wild input can't hold its gain down
// for long, no sum can overflow, and the low cut can't push the limiter past +-8). NaN and infinity in the
// input are silence. No stage has a gain over 1 but the filter's resonance, so
// feedback 1 holds the repeats and never runs away; drive 1 makes them fade slowly as it squares them off.
// A tiny constant (1e-18, -360 dB) rides under what is written, so nothing in the loop decays into
// denormals.
//
// Time: free (1..2000 ms) or synced (divBeats quarter notes at MPC's tempo, kEchoDivs below), times
// timeMod (the modulator's factor, this chunk), in double precision, the same on both sides. A new target
// (tempo, division, automation, modulation, mode) is reached one of two ways (`glide`). Tape glides there:
// the time one segment (32 samples) on, the samples between in a straight line, with a 60 ms one-pole as a
// tape delay's motor would, so repeats already in flight bend in pitch (the last millionth of a sample
// snaps, so it lands exactly). Fade keeps reading the old time and crossfades to a second read head at the
// new one over 50 ms (equal power: steady material keeps its level, a lone pure tone swells or dips by up
// to 3 dB halfway, as its two copies meet in or out of phase), no bend. A target that moves during a fade
// waits for it to end, then the latest one is faded to. A change under half a sample just snaps; a time
// within a millionth of a sample of a whole number is made whole (a 1/16T at 100 BPM is 4410 samples, not
// 4410.000000000001, and reads the line without interpolating). A line holds 3 s (1 bar at 80 BPM) plus
// the wow's depth; a longer time is clamped to 3 s, a shorter to 1 ms. No read ever comes nearer than 34
// samples (the wow gives way), so a chunk's reads never reach the samples that chunk is about to write.
//
// Modes. Stereo: each side its own line and feedback. Ping-Pong: the input's mono sum enters L only,
// L's read feeds R's write and R's feeds L's, so the repeats alternate L, R, L... Mono: both lines get the
// mono sum, the same time and the same wow, so both sides are the same. A mode change crossfades the
// routing over 10 ms.
//
// Wow: a 0.5 Hz wow (+-3 ms at wow 1) and a 6 Hz flutter (+-0.2 ms) move the read times; R's wow runs a
// quarter cycle ahead of L's and its flutter at 6.6 Hz. Under 12 ms the depth shrinks to a quarter of the
// time. Reads are cubic (4-point Hermite): linear interpolation would dull the repeats more wherever the
// wow puts them between samples.
//
// Memory: the lines are allocated by allocate() (the UI thread; 2 x 530 KB), not the constructor. Until
// then read() gives silence and write() does nothing, and set() only keeps the parameters. reset() is
// cheap (the rack calls it on the audio thread when a part comes back on): the lines aren't cleared.
// Until they have been written all the way round, each run of samples first zeroes the few old samples
// its taps can reach, so nothing from before ever comes out.
#include "common.h"
#include "svf.h"

namespace gf {

// The sync divisions the Echo offers, in quarter-note beats, shortest first: a Q-Link turn walks them in
// order. EffectForce's 16 plus 5/16, 7/16 and 5/8 (the polyrhythm times; 3/16 is already there as the
// dotted 1/8, 3/8 as the dotted 1/4).
inline constexpr ef::Division kEchoDivs[] = {   // sorted by length: a Q-Link turn walks them in order (R27)
    {"1/64", 0.0625}, {"1/32T", 1.0 / 12}, {"1/32", 0.125}, {"1/16T", 1.0 / 6}, {"1/16", 0.25}, {"1/8T", 1.0 / 3},
    {"1/16.", 0.375}, {"1/8", 0.5}, {"1/4T", 2.0 / 3}, {"1/8.", 0.75}, {"1/4", 1.0}, {"5/16", 1.25},
    {"1/2T", 4.0 / 3}, {"1/4.", 1.5}, {"7/16", 1.75}, {"1/2", 2.0}, {"5/8", 2.5}, {"1/2.", 3.0}, {"1 bar", 4.0}};
inline constexpr int kNumEchoDivs = static_cast<int>(sizeof kEchoDivs / sizeof kEchoDivs[0]);

class EchoLine {
public:
    enum Mode : int { kStereo, kPingPong, kMono };
    enum FilterType : int { kLP, kBP, kHP };
    enum Place : int { kInLoop, kOnOutput };
    enum Glide : int { kTape, kFade };
    struct Params {
        bool sync = true;               // time from MPC's tempo
        double divBeats = 0.75;         // synced: quarter-note beats (kEchoDivs): 1/8. by default
        float timeMs = 375.0f;          // 1..2000 free
        float feedback = 0.4f;          // 0..1: 1 holds
        int mode = kStereo;
        int filterType = kLP;
        float cutoffHz = 20000.0f;      // 20..20000; a low-pass at 20000 is off
        float resonance = 0.0f;         // 0..1: Q 0.7071 .. 12, evenly in octaves of Q
        int place = kInLoop;
        float drive = 0.0f;             // 0..1: saturation in the loop
        float wow = 0.0f;               // 0..1: wow and flutter
        int glide = kTape;              // how the time changes: Tape (pitch bends) or Fade (crossfade)
        float timeMod = 1.0f;           // a factor on the time this chunk (the modulator's), glided by the glide
        float cutoffModOct = 0.0f;      // octaves added to the cutoff this chunk
    };

    EchoLine();                          // no allocation
    bool allocate();                     // UI thread: 3 s + wow per side; false if out of memory
    bool allocated() const { return !lineR_.empty(); }
    void reset();
    void set(const Params& p, const ef::Transport& t);
    // Phase 1: the line's output for the next n samples (n <= kChunk), read from what was written >= 1 ms ago.
    void read(float* outL, float* outR, int n);
    // Phase 2: write n samples of input (the same n as the read's); the line adds its own feedback (from the
    // read) inside. The input may be the buffers read() filled (a Ring of one line).
    void write(const float* inL, const float* inR, int n);
    int tailSamples() const { return tail_; }

    // The delay time in samples, without the wow: Tape's glided time; in Fade the head being read (the
    // old one until a fade ends).
    double timeSamples() const { return t_; }
    // The largest magnitude the lines hold (a scan of both, for tests): the limiter keeps it at 1. Infinity if
    // anything in them is not a number.
    float peakStored() const;

private:
    // The write side's values that move in straight lines across a chunk.
    enum : int { FB, DRIVE, MONO_IN, CROSS, R_IN, kRamps };
    // What glides in the filter: g (L and R, in octaves), 1 / Q, the output mix, the wet and loop shares.
    enum : int { G, K, M0, M1, M2, WET, LOOP, kValues };

    void segment();
    void wowNow(double t, float& l, float& r) const;
    void setFilter(const Params& p);
    void filterTargets(ef::f2* t) const;
    template <bool Fading, bool Filt>
    void readRun(float* outL, float* outR, int w, int offset, int n);
    template <bool Moving>
    void writeRun(const float* inL, const float* inR, int n);

    std::vector<float> lineL_, lineR_;
    int w_ = 0;                   // where the next sample goes, in both lines
    int written_ = 0;             // samples written since reset(), up to the lines' length
    int readN_ = 0;               // what the last read() gave out and write() has yet to take
    ef::f2 back_[ef::kChunk] = {};   // per sample of that read: the feedback's source (L, R)

    float cur_[kRamps] = {}, goal_[kRamps] = {}, step_[kRamps] = {};

    // Time. t_ is the glided time at the segment's end; te* are the heads' read times with the wow, per
    // side, this sample, at the segment's end, and the step between.
    double tgtT_ = 1.0, t_ = 1.0;
    double teL_ = 1.0, teR_ = 1.0, teEndL_ = 1.0, teEndR_ = 1.0, teStepL_ = 0.0, teStepR_ = 0.0;
    int segLeft_ = 0;

    // Fade's second read head: its time (the first head's while there's no fade), and the two heads' gains
    // (the same on both sides); fade_ counts the fade's segments, -1 with none running.
    int glideType_ = kTape;
    double tB_ = 1.0, teBL_ = 1.0, teBR_ = 1.0, teBEndL_ = 1.0, teBEndR_ = 1.0, teBStepL_ = 0.0, teBStepR_ = 0.0;
    int fade_ = -1;
    bool fading_ = false;
    float gA_ = 1.0f, gAEnd_ = 1.0f, gAStep_ = 0.0f, gB_ = 0.0f, gBEnd_ = 0.0f, gBStep_ = 0.0f;

    ef::f2 low_ = {};             // the 30 Hz low cut's state
    ef::f2 gain_ = {1.0f, 1.0f};  // the limiter's
    float wowAmt_ = 0.0f, wowTgt_ = 0.0f, mono_ = 0.0f, monoTgt_ = 0.0f;
    float phWow_ = 0.0f, phFlutL_ = 0.0f, phFlutR_ = 0.0f;   // cycles

    // The filter, as the controls have got to (they slew toward the parameters); gl_ glides between
    // chunks' targets. fState_ is what the glide's targets were last worked out from.
    bool fRunning_ = false;
    float fOct_ = 0.0f, fRes_ = 0.0f, fWet_ = 0.0f, fLoop_ = 1.0f;
    float fType_[3] = {};   // weights of LP, BP and HP
    float fState_[7] = {};
    float fRing_ = 0.0f;    // the filter's ring at the targets, samples
    ef::Glide<kValues, 1> gl_;
    ef::SvfUpdate a_ = {};  // a1..a3 at the glide's targets
    ef::SvfState s_;

    int tail_ = 0;
    float tailFb_ = -1.0f;        // what tail_ was worked out for
    double tailLongest_ = -1.0, tailRing_ = -1.0, repeats_ = 1.0;
    bool fresh_ = true;
};

} // namespace gf
