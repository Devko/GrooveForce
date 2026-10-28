#pragma once
// A line's modulator for the Echo (docs/DESIGN.md, "Echo", the Modulator row): the one value 0..1 that
// moves the line's cutoff, time and level, each by an amount of its own (the line, a later task, does
// the moving). Sigmund 2's way: a shape, an envelope fired by hits, or the level of the sound.
//
//   LFO     A shape from Shape's library (dsp/shapes.h) at a rate: shapeAt(shape, toStraight(song
//           position) / rateBeats). It sits on MPC's grid as Shape's lanes do: while MPC plays (and
//           says where) the position is MPC's, so a loop, a locate or a tempo change leaves the value
//           on the grid; stopped, the position runs on at the tempo from where it was (0 after reset()),
//           and starting to play moves it to the song position. The global swing is undone by
//           toStraight, so the steps land late on the swung audio. The random shapes are seeded by
//           the whole cycle, so a loop plays the same bar every time.
//   Env     An onset in the key (the TransientDetector of Pump's Input trigger, at `sensitivity`)
//           sets the value to 1; it then falls along an exponential that is down 60 dB (0.001) after
//           rateBeats, at the tempo. A new hit starts it again from 1. The value is 0 until the first hit.
//   Follow  The key's level: the louder side, one pole up in 5 ms and down in 150 ms (EffectWerk's
//           EnvFollower), times 2.14 (+6.6 dB) and cut at 1. A peak follower with that release reads
//           a sine 0.6 dB under its peak (the ripple between the peaks), so a plain +6 dB would read a
//           -6 dBFS sine as 0.93; 2.14 reads it as 1 (0.98 to 1.02 from 30 Hz up). Louder stays at 1.
//   Off     0, and nothing more is done than keeping the position.
//
// The key is the chain's input (the plan 2 ledger's R26), not the line's own output: a line's modulator
// hears what the Echo is fed, so a hit starts the same envelope in every line that uses Env, and a
// line's own repeats can't trigger it. The pointers are only read in Env and Follow, and null is
// silence. A NaN or an infinity in the key is silence too.
//
// next() is called once per chunk (n <= kChunk, 32 samples) and returns the value for it: the LFO's
// and the envelope's at the chunk's start (a hit in the chunk counts: the value is 1 for it, and the
// fall starts from the hit's sample), the follower's at its end. There is no look-ahead: a hit is found
// as the audio goes by, 0.4 to 3 ms after it starts. The value is not smoothed here. A shape's edge, a
// hit, a locate or a mode change moves it a whole step between two chunks, and what the line does with
// it (a cutoff, a delay time, a gain: the 1 ms rule is the gain's) is the line's to limit.
//
// The detector is never reset while audio runs (a reset makes the next sample an onset): picking Env,
// or reset(), leaves its onsets alone for 60 ms while it warms up on the audio, as Pump's Input
// trigger does. The other modes start from nothing when picked: the envelope and the follower from 0.
// Parameters out of range are the nearest value, a mode or number that isn't one the default, and a
// position that isn't a number is not a position (the LFO runs on).
//
// One detector and one follower for all the lines (ruling R41). Every Env hears the same key at the
// same sensitivity, so four of them would find the same hits four times, and four followers would
// follow the same level. The Echo therefore runs one TransientDetector and one KeyFollower (below) over
// the key per chunk, and gives each line's modulator what they found: nextShared(p, t, n, onset, level)
// is next() with the onset (the offset in the chunk of the detector's first onset, or -1) and the follower's
// level instead of the key; it does the same as next() would have with a detector and a follower of its
// own that started with the Echo's and were fed the same key, bit for bit (checked in the tests), and each
// line's Env still falls at its own rate and warms up for 60 ms after its own reset() or pick. The shared
// detector and follower are the Echo's to keep running: they run while some line needs them, and start from
// nothing (reset) when they resume after chunks they skipped, as a follower picked in next() does; their
// sensitivity is the Echo's: Params::sensitivity is read by next() only.
//
// Header only, no allocation, nothing that locks: the audio thread can call next() on any chunk.
#include "common.h"
#include "grid.h"
#include "shapes.h"
#include "svf.h"       // clampParam
#include "transient.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace gf {

// EffectWerk's EnvFollower: the peak of the two sides, one pole up in 5 ms and down in 150 ms. process() takes
// n samples of the key (null is not allowed: the caller gives silence) and returns the level at the chunk's end;
// NaN and infinity are silence. On NEON in a vector's lanes: VFP has no min or max, and its compares stall on
// the flags.
class KeyFollower {
public:
    void reset() { level_ = 0.0f; }
    float process(const float* L, const float* R, int n) {
#if EF_NEON
        float32x2_t e = vdup_n_f32(level_);
        const float32x2_t att = vdup_n_f32(att_), rel = vdup_n_f32(rel_), big = vdup_n_f32(FLT_MAX);
        for (int i = 0; i < n; ++i) {
            float32x2_t v = vld1_lane_f32(R + i, vld1_dup_f32(L + i), 1);
            v = vreinterpret_f32_u32(vand_u32(vreinterpret_u32_f32(v), vcale_f32(v, big)));   // sanitize()
            v = vabs_f32(v);
            const float32x2_t x = vpmax_f32(v, v);
            e = vfma_f32(e, vsub_f32(x, e), vbsl_f32(vcgt_f32(x, e), att, rel));   // fused, as GCC builds the VFP version
        }
        const float last = vget_lane_f32(e, 0);
#else
        float last = level_;
        for (int i = 0; i < n; ++i) {
            const float x = std::max(std::fabs(ef::sanitize(L[i])), std::fabs(ef::sanitize(R[i])));
            last += (x - last) * (x > last ? att_ : rel_);
        }
#endif
        level_ = last < 1e-9f ? 0.0f : last;   // no denormals while it decays
        return level_;
    }

private:
    float level_ = 0.0f;
    float att_ = ef::smoothCoef(5e-3f), rel_ = ef::smoothCoef(150e-3f);
};

class EchoMod {
public:
    enum Mode : int { kOff, kLfo, kEnv, kFollow };

    struct Params {
        int mode = kOff;
        int shape = 0;             // LFO: the library's, gf::shapeName
        double rateBeats = 1.0;    // LFO: the cycle, Env: the fall to -60 dB; 0.0625 .. 64 beats
        float sensitivity = 0.5f;  // Env: the detector's, 0..1 (next() only: nextShared() hears the Echo's detector)
        Swing swing;               // set by the chain
    };

    static constexpr double kMinRateBeats = 0.0625, kMaxRateBeats = 64.0;

    EchoMod() { reset(); }

    // Everything forgotten: the position starts at 0, the next set of parameters starts from nothing.
    void reset() {
        det_.reset();
        fol_.reset();
        pos_ = 0.0;
        env_ = 0.0f;
        mode_ = kOff;
        mute_ = 0;
    }

    // One value 0..1 for the chunk of n samples starting now. key: the chain's input (R26), for Env
    // and Follow.
    float next(const Params& p, const ef::Transport& t, const float* keyL, const float* keyR, int n) {
        return step(p, t, n, keyL, keyR, false, -1, 0.0f);
    }

    // The same for a modulator whose detector and follower are the Echo's (R41): onset is the offset in the
    // chunk of the shared detector's first onset in the key, or -1 (TransientDetector::process()); level the
    // shared KeyFollower's level at the chunk's end (KeyFollower::process()). Both are only used by the mode
    // that needs them, but the Echo's detector and follower must have run on this chunk's key whenever any
    // line is in Env or Follow.
    float nextShared(const Params& p, const ef::Transport& t, int n, int onset, float level) {
        return step(p, t, n, nullptr, nullptr, true, onset, level);
    }

private:
    static constexpr int kWarmup = static_cast<int>(0.060f * ef::kRate);   // as Pump's detector
    static constexpr float kLog2Thousand = 9.96578428f;                    // 60 dB is 10^-3
    static constexpr float kFollowGain = 2.14f;                            // see the header
    static constexpr float kTiny = 1e-6f;                                  // -120 dB: the envelope is over

    static double rateOf(double rate) {
        return rate >= kMinRateBeats ? std::min(rate, kMaxRateBeats) : (rate < kMinRateBeats ? kMinRateBeats : 1.0);
    }

    float step(const Params& p, const ef::Transport& t, int n, const float* keyL, const float* keyR, bool shared, int onset, float level) {
        n = std::clamp(n, 1, ef::kChunk);
        // Playing, the song position is MPC's (it moves for a locate, a loop, a tempo change). Otherwise
        // it runs on at the tempo from where it was.
        const double bpm = ef::tempoOf(t);
        if (t.playing && t.valid && std::isfinite(t.beats)) pos_ = t.beats;
        const double start = pos_;
        pos_ += n * bpm / (60.0 * ef::kRate);

        const int mode = p.mode >= kLfo && p.mode <= kFollow ? p.mode : kOff;
        if (mode != mode_) pick(mode);
        if (mode == kOff) return 0.0f;
        if (mode == kLfo) return lfo(p, start);

        if (shared) {
            if (mode == kEnv) return env(p, bpm, onset >= 0 && onset < n ? onset : -1, n);
            return std::min((level >= 0.0f ? level : 0.0f) * kFollowGain, 1.0f);   // (a NaN is 0)
        }
        // The key: null is silence.
        static const float silence[ef::kChunk] = {};
        const float* L = keyL ? keyL : silence;
        const float* R = keyR ? keyR : silence;
        if (mode == kFollow) return std::min(fol_.process(L, R, n) * kFollowGain, 1.0f);
        const float sens = ef::clampParam(p.sensitivity, 0.0f, 1.0f, 0.5f);
        if (sens != sens_) {
            sens_ = sens;
            det_.setSensitivity(sens);
        }
        return env(p, bpm, det_.process(L, R, n), n);
    }

    void pick(int mode) {
        mode_ = mode;
        env_ = 0.0f;
        fol_.reset();
        if (mode == kEnv) mute_ = kWarmup;
    }

    float lfo(const Params& p, double start) const {
        const int shape = std::clamp(p.shape, 0, numShapes() - 1);
        const float v = shapeAt(shape, toStraight(start, p.swing) / rateOf(p.rateBeats));
        return v >= 0.0f ? (v <= 1.0f ? v : 1.0f) : 0.0f;   // (and a NaN is 0)
    }

    // at: the offset of the detector's first onset in the chunk, or -1.
    float env(const Params& p, double bpm, int at, int n) {
        if (mute_ > 0) {   // still warming up: what it finds is the audio starting, not a hit
            mute_ = std::max(mute_ - n, 0);
            at = -1;
        }
        // -60 dB over rateBeats: the factor for the samples left in the chunk (all of it, if no hit).
        const double fall = rateOf(p.rateBeats) * 60.0 / bpm * ef::kRate;   // samples
        float out = env_;
        if (at >= 0) {
            out = 1.0f;
            env_ = ef::exp2Fast(-kLog2Thousand * static_cast<float>((n - at) / fall));
        } else {
            env_ *= ef::exp2Fast(-kLog2Thousand * static_cast<float>(n / fall));
        }
        if (env_ < kTiny) env_ = 0.0f;   // no denormals while it decays
        return out;
    }

    TransientDetector det_;
    KeyFollower fol_;
    double pos_ = 0.0;        // the song position at the chunk's start, quarter-note beats
    float env_ = 0.0f;        // Env: the value now
    float sens_ = 0.5f;       // what the detector is set to (its default)
    int mode_ = kOff;         // the mode the last chunk ran
    int mute_ = 0;            // samples the detector's onsets are not taken, warming up
};

} // namespace gf
