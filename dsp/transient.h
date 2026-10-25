#pragma once
// Onsets in the input, for Pump's Input trigger (and later the Echo's modulators): the hits a kick
// or a drum bus is made of, found in the louder of the two sides.
//
// Two envelopes follow the level: a quick one (attack 0.1 ms, release 8 ms: on a hit within a few
// samples, off it between two) and a slow one (attack 15 ms, release 120 ms: the level the quick
// one is measured against, and what remembers the last loud hit). An onset is the quick envelope
// standing well over the slow one, and over -50 dBFS so noise under the music never counts. A
// steady tone or noise, or a swell, keeps the two together; a hit lifts the quick one first, and the
// slow one's memory of an earlier, louder hit is what keeps its tail from counting as a new hit.
//
// After an onset the detector waits kHoldoffMs, so one hit is one onset whatever it does inside (a
// kick's second wave, a flam). A hit still standing over the slow envelope when the wait is over
// counts then.
//
// Sensitivity sets how far over the slow envelope the quick one has to rise: a ratio from 4 (0, only
// hits that stand out) to 1.4 (1, quiet hits too), in equal steps of log.
//
// No look-ahead: the onset is the sample at which the quick envelope crosses, found as the audio
// goes by. A kick over noise 30 dB under it is found 0.2 to 0.5 ms after it starts (the first rise
// out of the noise).
#include "common.h"   // sanitize, smoothCoef, kRate; simd.h for NEON

#include <cfloat>

namespace gf {

class TransientDetector {
public:
    static constexpr float kHoldoffMs = 40.0f;

    TransientDetector() { setSensitivity(0.5f); }

    void reset() {
        env_[0] = env_[1] = 0.0f;
        holdoff_ = 0;
    }

    // 0..1: higher catches quieter hits (default 0.5). Not a number counts as 0.
    void setSensitivity(float s) {
        s = !(s > 0.0f) ? 0.0f : (s < 1.0f ? s : 1.0f);
        ratio_ = kRatioDull * ef::exp2Fast(s * kLog2KeenOverDull);
    }

    // Follows n samples (n <= kChunk) of the louder side; the offset of the first onset in them, or
    // -1. Two onsets are at least kHoldoffMs apart. On NEON the two envelopes are the lanes of one
    // vector (as in EffectForce's EnvFollower: VFP has no min or max, and its compares stall on the
    // flags); the decision is one more compare of that vector, its lane 0 read as an integer.
    int process(const float* L, const float* R, int n) {
        int first = -1;
#if EF_NEON
        float32x2_t e = vld1_f32(env_);   // lane 0 quick, lane 1 slow
        const float32x2_t att = vld1_f32(att_), rel = vld1_f32(rel_), big = vdup_n_f32(FLT_MAX);
        const float32x2_t ratio = vset_lane_f32(ratio_, vdup_n_f32(0.0f), 0), flo = vset_lane_f32(kFloor, vdup_n_f32(0.0f), 0);
        for (int i = 0; i < n; ++i) {
            float32x2_t v = vld1_lane_f32(R + i, vld1_dup_f32(L + i), 1);
            v = vreinterpret_f32_u32(vand_u32(vreinterpret_u32_f32(v), vcale_f32(v, big)));   // sanitize()
            v = vabs_f32(v);
            const float32x2_t x = vpmax_f32(v, v);
            e = vfma_f32(e, vsub_f32(x, e), vbsl_f32(vcgt_f32(x, e), att, rel));
            if (holdoff_ > 0 && --holdoff_ > 0) continue;   // waiting; the sample that counts it to 0 may fire
            // quick > max(slow * ratio, floor), in lane 0
            const float32x2_t over = vmax_f32(vmul_f32(vrev64_f32(e), ratio), flo);
            if (!vget_lane_u32(vcgt_f32(e, over), 0)) continue;
            holdoff_ = kHoldoffSamples;
            if (first < 0) first = i;
        }
        const uint32x2_t keep = vcgt_f32(e, vdup_n_f32(kTiny));   // no denormals while it decays
        e = vreinterpret_f32_u32(vand_u32(vreinterpret_u32_f32(e), keep));
        vst1_f32(env_, e);
#else
        float quick = env_[0], slow = env_[1];
        for (int i = 0; i < n; ++i) {
            const float x = std::max(std::fabs(ef::sanitize(L[i])), std::fabs(ef::sanitize(R[i])));
            quick += (x - quick) * (x > quick ? att_[0] : rel_[0]);
            slow += (x - slow) * (x > slow ? att_[1] : rel_[1]);
            if (holdoff_ > 0 && --holdoff_ > 0) continue;   // waiting; the sample that counts it to 0 may fire
            if (!(quick > slow * ratio_ && quick > kFloor)) continue;
            holdoff_ = kHoldoffSamples;
            if (first < 0) first = i;
        }
        env_[0] = quick < kTiny ? 0.0f : quick;   // no denormals while it decays
        env_[1] = slow < kTiny ? 0.0f : slow;
#endif
        return first;
    }

private:
    static constexpr int kHoldoffSamples = static_cast<int>(kHoldoffMs * 0.001f * ef::kRate + 0.5f);
    static constexpr float kFloor = 3.1622777e-3f;          // -50 dBFS
    static constexpr float kTiny = 1e-9f;
    static constexpr float kRatioDull = 4.0f;               // sensitivity 0
    static constexpr float kLog2KeenOverDull = -1.5145732f; // log2(1.4 / 4): sensitivity 1 is a ratio of 1.4

    float env_[2] = {0.0f, 0.0f};   // quick, slow
    float att_[2] = {ef::smoothCoef(0.1e-3f), ef::smoothCoef(15e-3f)};
    float rel_[2] = {ef::smoothCoef(8e-3f), ef::smoothCoef(120e-3f)};
    float ratio_ = kRatioDull;
    int holdoff_ = 0;               // samples left before the next onset may count
};

} // namespace gf
