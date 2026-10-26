// Pump: see pump.h.
#include "pump.h"
#include "slew.h"

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace gf {

// copyStateFrom() is a plain copy of the object: no allocation, nothing to fix up.
static_assert(std::is_trivially_copyable<Pump>::value, "Pump is copied bit for bit");

namespace {

constexpr double kMinRate = 0.25, kMaxRate = 4.0;     // 1/16 .. 1 bar
constexpr float kMinLength = 0.05f;
constexpr double kFar = 1e9;                          // offsets in samples are held to this: far enough, and a float holds them
constexpr float kAmountRate = 1.442695041f / (0.005f * ef::kRate);   // log2(e) a sample: a 5 ms one-pole
constexpr float kCutoffRate = 1.442695041f / (0.020f * ef::kRate);   // and a 20 ms one

double clampD(double x, double lo, double hi, double nan) { return x >= lo ? (x <= hi ? x : hi) : (x < lo ? lo : nan); }

// A level held to 0..1; not a number is 1 (a curve that gives up leaves the input alone).
float unit(float x) { return x >= 0.0f ? (x <= 1.0f ? x : 1.0f) : (x < 0.0f ? 0.0f : 1.0f); }

} // namespace

Pump::Pump() {
    lpA_ = ef::SvfUpdate::of(ef::splat2(ef::svfG(kDetectorHz)), ef::splat2(1.41421356f));   // 2 poles, Butterworth
    reset();
}

void Pump::reset() {
    fresh_ = true;
    pos_ = 0.0;
    clock_ = 0.0;
    level_ = 1.0f;
    sync_ = true;
    amount1_ = amount_ = 0.0f;
    fade_ = 0.0f;
    low_ = high_ = 1.0f;
    splitLive_ = false;
    bandT_ = false;
    band_ = kFull;
    trigger_ = kGrid;
    numPending_ = 0;
    last_ = kNever;
    det_.reset();   // here and only here: a reset while audio runs makes the next sample an onset
    lp_.clear();
    xo_.reset();
    mute_ = kDetectorWarmup;
}

void Pump::set(const Params& p, const ef::Transport& t) {
    rate_ = clampD(p.rateBeats, kMinRate, kMaxRate, 1.0);
    curve_ = p.curve < 0 ? 0 : (p.curve >= PC_COUNT ? PC_COUNT - 1 : p.curve);
    amountT_ = ef::clampParam(p.depth, 0.0f, 1.0f, 0.0f) * ef::clampParam(p.mix, 0.0f, 1.0f, 1.0f);
    lengthBeats_ = static_cast<double>(ef::clampParam(p.length, kMinLength, 1.0f, 0.6f)) * rate_;
    const int trigger = p.trigger == kInput ? kInput : kGrid;
    pushMs_ = ef::clampParam(p.pushMs, trigger == kInput ? 0.0f : -10.0f, 30.0f, 0.0f);
    det_.setSensitivity(ef::clampParam(p.sensitivity, 0.0f, 1.0f, 0.5f));
    swing_ = p.swing;

    // Where the chunk starts: MPC's song position while it plays, else where the last chunk ended. The
    // first chunk after reset() starts from MPC's position whether it plays or not: stopped, the grid
    // runs on from there at the tempo.
    const double bpm = t.bpm >= 20.0 ? std::min(t.bpm, 999.0) : (t.bpm < 20.0 ? 20.0 : 120.0);
    bps_ = bpm / 60.0 / ef::kRate;
    const bool known = t.valid && std::isfinite(t.beats);
    b0_ = known && (t.playing || fresh_) ? t.beats : pos_;

    if (trigger != trigger_) {   // Input picked: its low-pass starts clean and the detector warms up on the audio (never reset)
        trigger_ = trigger;
        numPending_ = 0;
        last_ = kNever;
        if (trigger == kInput) {
            lp_.clear();
            mute_ = kDetectorWarmup;
        }
    }

    band_ = p.band == kLows || p.band == kHighs ? p.band : kFull;
    lowT_ = band_ == kHighs ? 0.0f : 1.0f;
    highT_ = band_ == kLows ? 0.0f : 1.0f;
    xoTarget_ = ef::log2Fast(ef::clampParam(p.crossoverHz, Crossover::kMinHz, Crossover::kMaxHz, Crossover::kDefaultHz));
    bandT_ = band_ != kFull && (amountT_ > 0.0f || amount_ > 0.0f);

    if (fresh_) {   // the first set() after reset(): everything is where it is told to be
        amount1_ = amount_ = amountT_;
        fade_ = bandT_ ? 1.0f : 0.0f;
        fresh_ = false;
    }
    if (bandT_ && !splitLive_) {   // the split starts from nothing: it fades in over the plain path
        xo_.reset();
        splitLive_ = true;
        low_ = lowT_;
        high_ = highT_;
        xoLog_ = xoTarget_;
    }
}

void Pump::glideAmount(int n) {
    if (amount_ == amountT_ && amount1_ == amountT_) return;
    const float k = 1.0f - ef::exp2Fast(-static_cast<float>(n) * kAmountRate);
    amount1_ += (amountT_ - amount1_) * k;
    amount_ += (amount1_ - amount_) * k;
    if (std::fabs(amountT_ - amount_) < 1e-5f && std::fabs(amountT_ - amount1_) < 1e-5f) amount1_ = amount_ = amountT_;
}

// The bottoms of the grid near this chunk, as offsets in samples from its first: the one at or before
// it, then those after, up to a drop's length past the chunk's end. Step k of the grid is straight
// time k r; swung it is toSwung(k r), and the push comes on top, in time.
int Pump::gridBottoms(double* d, int n) const {
    const double pushBeats = static_cast<double>(pushMs_) * 0.001 * ef::kRate * bps_;
    const double limit = n + kEdgeSamples + 1.0;
    double k = std::floor(toStraight(b0_ - pushBeats, swing_) / rate_);   // (a double: nothing here is cast to an int)
    int m = 0;
    while (m < kMaxBottoms) {
        const double at = (toSwung(k * rate_, swing_) + pushBeats - b0_) / bps_;
        if (!std::isfinite(at)) break;
        const double v = std::min(std::max(at, -kFar), kFar);
        if (m > 0 && !(v > d[m - 1])) break;   // past what a double can tell apart
        d[m++] = v;
        if (v >= limit) break;
        k += 1.0;
    }
    if (m == 0) d[m++] = -kFar;
    return m;
}

// The bottoms Input has found: the last one reached, then those scheduled (all after the chunk's start).
int Pump::inputBottoms(double* d, int) const {
    int m = 0;
    d[m++] = std::max(last_ - clock_, -kFar);
    for (int k = 0; k < numPending_ && m < kMaxBottoms; ++k) d[m++] = pending_[k] - clock_;
    return m;
}

// The detector hears the input through a low-pass (a kick, not the hats over it). An onset becomes a
// bottom `push` later, and no sooner than the drop takes.
void Pump::listen(const float* L, const float* R, int n) {
    float lpL[ef::kChunk], lpR[ef::kChunk];
    const ef::f2 zero = ef::splat2(0.0f), one = ef::splat2(1.0f);
    const ef::f2 lowpass[3] = {zero, zero, one};
    for (int i = 0; i < n; ++i) ef::store2(lpL + i, lpR + i, lp_.tick(ef::safeIn(ef::load2(L + i, R + i)), lpA_, lowpass));
    lp_.flushTiny();
    const int at = det_.process(lpL, lpR, n);
    if (mute_ > 0) {   // still warming up: what it finds is the audio starting, not a hit
        mute_ = std::max(mute_ - n, 0);
        return;
    }
    if (at < 0) return;
    const double bottom = clock_ + at + std::max(static_cast<double>(pushMs_) * 0.001 * ef::kRate, static_cast<double>(kEdgeSamples));
    if (numPending_ == kMaxPending) --numPending_;   // (not reached: hits are 40 ms apart and a push is 30)
    int k = numPending_++;
    while (k > 0 && pending_[k - 1] > bottom) {
        pending_[k] = pending_[k - 1];
        --k;
    }
    pending_[k] = bottom;
}

// The chunk is over: the song position runs on, the bottoms reached are behind us.
void Pump::advance(int n) {
    pos_ = b0_ + n * bps_;
    clock_ += n;
    int gone = 0;
    while (gone < numPending_ && pending_[gone] < clock_) last_ = pending_[gone++];
    for (int k = gone; k < numPending_; ++k) pending_[k - gone] = pending_[k];
    numPending_ -= gone;
}

void Pump::process(float* L, float* R, int n, const float* keyL, const float* keyR) {
    n = std::min(n, ef::kChunk);
    if (n <= 0) return;
    if (trigger_ == kInput) {
        const bool keyed = keyL && keyR;
        listen(keyed ? keyL : L, keyed ? keyR : R, n);   // (before the audio is touched: the key may be the audio)
    }

    const float a0 = amount_;
    glideAmount(n);
    const float a1 = amount_;
    if (a0 == 0.0f && a1 == 0.0f && !splitLive_) {   // nothing ducks: the input as it is (a NaN or an infinity excepted)
        for (int i = 0; i < n; ++i) {
            L[i] = ef::sanitize(L[i]);
            R[i] = ef::sanitize(R[i]);
        }
        sync_ = true;
        advance(n);
        return;
    }

    // The level, a sample at a time: the lower of the recovery from the last bottom and the drop to
    // the next, slope-limited. duck is how much of the input is taken away.
    double d[kMaxBottoms];
    const int m = trigger_ == kGrid ? gridBottoms(d, n) : inputBottoms(d, n);
    const float perSample = static_cast<float>(bps_ / lengthBeats_);   // the recovery's x, a sample
    const float da = (a1 - a0) / static_cast<float>(n);
    float duck[ef::kChunk];
    float level = level_;
    int j = 0;
    for (int i = 0; i < n; ++i) {
        while (j + 1 < m && d[j + 1] <= i) ++j;
        const float x = static_cast<float>(i - d[j]) * perSample;
        float target = x >= 1.0f ? 1.0f : pumpLevel(curve_, x);
        if (j + 1 < m) target = std::min(target, static_cast<float>(d[j + 1] - i) * kSlope);
        target = unit(target);
        level = sync_ ? target : ef::clampf(target, level - kSlope, level + kSlope);
        sync_ = false;
        duck[i] = ef::clampf((a0 + da * static_cast<float>(i + 1)) * (1.0f - level), 0.0f, 1.0f);   // (a hair under 0 or over 1 from rounding)
    }
    level_ = level;

    if (!splitLive_) {
        for (int i = 0; i < n; ++i) {
            const float gain = 1.0f - duck[i];
            L[i] = ef::sanitize(L[i]) * gain;
            R[i] = ef::sanitize(R[i]) * gain;
        }
        advance(n);
        return;
    }

    // Banded: the split, one band ducked, the sum; crossfaded with the plain path while it comes or goes.
    float xl[ef::kChunk], xr[ef::kChunk], lowL[ef::kChunk], lowR[ef::kChunk], highL[ef::kChunk], highR[ef::kChunk];
    for (int i = 0; i < n; ++i) {
        xl[i] = ef::sanitize(L[i]);
        xr[i] = ef::sanitize(R[i]);
    }
    xoLog_ += (xoTarget_ - xoLog_) * (1.0f - ef::exp2Fast(-static_cast<float>(n) * kCutoffRate));
    xo_.set(ef::exp2Fast(xoLog_));
    xo_.process(xl, xr, lowL, lowR, highL, highR, n);
    const float fadeT = bandT_ ? 1.0f : 0.0f;
    for (int i = 0; i < n; ++i) {
        fade_ = toward(fade_, fadeT, kFadeStep);
        low_ = toward(low_, lowT_, kFadeStep);
        high_ = toward(high_, highT_, kFadeStep);
        const float plain = 1.0f - duck[i], gl = 1.0f - low_ * duck[i], gh = 1.0f - high_ * duck[i];
        L[i] = (1.0f - fade_) * (xl[i] * plain) + fade_ * (lowL[i] * gl + highL[i] * gh);
        R[i] = (1.0f - fade_) * (xr[i] * plain) + fade_ * (lowR[i] * gl + highR[i] * gh);
    }
    if (!bandT_ && fade_ <= 0.0f) splitLive_ = false;
    advance(n);
}

} // namespace gf
