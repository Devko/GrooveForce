#pragma once
// Crossover: splits a stereo signal into lows and highs at one frequency (60-300 Hz), so the banded
// Pump can duck one side and let the other through.
//
// A Linkwitz-Riley filter of the 4th order: each side is two Butterworth sections in series (dsp/svf.h's
// SVF, k = sqrt 2), the lows low-pass twice and the highs high-pass twice. Both sides are 6 dB down at
// the crossover and in phase there, and low + high is exactly an allpass: the magnitude is flat from
// DC to Nyquist, so a band that is not ducked sums back without a notch or a bump at the crossover.
// The slopes are 24 dB per octave. No latency; the allpass rings, at 60 Hz about 500 samples to -60 dB
// and 1000 to -80 dB (a third of that at 300 Hz).
//
// Both sides' first sections take the same input at the same cutoff, so they are one filter: its
// low-pass output feeds the lows' second section, x - k v1 - v2 (the high-pass) the highs'. Three SVFs
// run, not four, as EffectWerk's OTT does. L and R are the two lanes of an f2.
//
// Moving: g glides across a chunk, evenly in octaves (ef::Glide, as the SVF modules do), and a1..a3
// follow it sample by sample while it moves, so a swept cutoff makes no click; svf.h shows why that
// can't blow the filter up. A leap across the whole range in one set() is bounded too, but it turns the
// allpass's phase within a chunk (a tone near the crossover can swell to twice its level for a moment):
// whoever feeds set() from a knob smooths it. Settled, a1..a3 are fixed. The input goes through
// safeIn(), so a NaN or an infinity never gets into the state.
#include "svf.h"

namespace gf {

class Crossover {
public:
    static constexpr float kMinHz = 60.0f, kMaxHz = 300.0f, kDefaultHz = 120.0f;

    // Clears the filters; the next set() jumps to its cutoff instead of gliding to it.
    void reset() {
        s1_.clear();
        sLow_.clear();
        sHigh_.clear();
        fresh_ = true;
    }

    // The cutoff for the next chunk, 60..300 Hz (clamped; NaN is 120).
    void set(float hz) {
        const ef::f2 t[1] = {ef::splat2(ef::svfG(ef::clampParam(hz, kMinHz, kMaxHz, kDefaultHz)))};
        a_ = ef::SvfUpdate::of(t[0], k());
        if (fresh_) {
            g_.jump(t);
            fresh_ = false;
        } else {
            g_.to(t);
        }
    }

    // Splits n samples (n <= kChunk) of L / R into lows and highs. An output may be the input it
    // belongs to (each sample is read before its place is written).
    void process(const float* L, const float* R, float* lowL, float* lowR, float* highL, float* highR, int n) {
        // Locals: the compiler can't keep members in registers across the stores to the outputs.
        ef::Glide<1, 1> g = g_;
        ef::SvfState s1 = s1_, sLow = sLow_, sHigh = sHigh_;
        const ef::f2 k0 = k(), zero = ef::splat2(0.0f), one = ef::splat2(1.0f);
        const ef::f2 lowMix[3] = {zero, zero, one};         // low-pass
        const ef::f2 highMix[3] = {one, -k0, -one};         // high-pass: x - k v1 - v2
        const auto sample = [&](int i, const ef::SvfUpdate& a) {
            const ef::f2 x = ef::safeIn(ef::load2(L + i, R + i));
            // The shared first section: v1 the band-pass, v2 the low-pass.
            const ef::f2 v3 = x - s1.ic2;
            const ef::f2 v1 = a.a1 * s1.ic1 + a.a2 * v3;
            const ef::f2 v2 = s1.ic2 + a.a2 * s1.ic1 + a.a3 * v3;
            s1.ic1 = v1 + v1 - s1.ic1;
            s1.ic2 = v2 + v2 - s1.ic2;
            ef::store2(lowL + i, lowR + i, sLow.tick(v2, a, lowMix));
            ef::store2(highL + i, highR + i, sHigh.tick(x - k0 * v1 - v2, a, highMix));
        };
        int i = 0;
        for (; i < n && g.moving(); ++i) {
            g.next();
            sample(i, ef::SvfUpdate::fast(g.cur()[0], k0));
        }
        const ef::SvfUpdate a = a_;
        for (; i < n; ++i) sample(i, a);
        s1.flushTiny();
        sLow.flushTiny();
        sHigh.flushTiny();
        g_ = g;
        s1_ = s1;
        sLow_ = sLow;
        sHigh_ = sHigh;
    }

private:
    static ef::f2 k() { return ef::splat2(1.41421356f); }   // 1 / Q of a Butterworth section

    ef::Glide<1, 1> g_;            // g, both lanes
    ef::SvfUpdate a_ = {};         // a1..a3 at the glide's target
    ef::SvfState s1_, sLow_, sHigh_;   // the shared first section, the lows' second, the highs' second
    bool fresh_ = true;
};

} // namespace gf
