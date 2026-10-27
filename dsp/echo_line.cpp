// EchoLine: see echo_line.h.
#include "echo_line.h"

#include <cstring>
#include <new>

namespace gf {

namespace {

// L and R side by side in svf.h's two-lane vector: NEON's 64-bit registers on the device, GCC's generic
// vectors elsewhere (the x86 tests run the same arithmetic lane by lane). Comparisons and min / max
// stay in the vector unit: VFP's scalar compares stall on the transfer of their flags.
using Lr = ef::f2;
#if EF_NEON
using LrMask = uint32x2_t;
#else
typedef int32_t LrMask __attribute__((vector_size(8)));
#endif

EF_INLINE Lr both(float x) { return ef::splat2(x); }
EF_INLINE Lr pairLr(float l, float r) {
#if EF_NEON
    return vset_lane_f32(r, vdup_n_f32(l), 1);
#else
    return Lr{l, r};
#endif
}
EF_INLINE Lr absLr(Lr a) {
#if EF_NEON
    return vabs_f32(a);
#else
    return a < Lr{} ? -a : a;
#endif
}
EF_INLINE Lr swapLr(Lr a) {   // (R, L)
#if EF_NEON
    return vrev64_f32(a);
#else
    return Lr{a[1], a[0]};
#endif
}
EF_INLINE LrMask greaterLr(Lr a, Lr b) {
#if EF_NEON
    return vcgt_f32(a, b);
#else
    return a > b;
#endif
}
EF_INLINE Lr pickLr(LrMask m, Lr yes, Lr no) {
#if EF_NEON
    return vbsl_f32(m, yes, no);
#else
    return m ? yes : no;
#endif
}
// NaN and infinities become 0, by their bits (an exponent of all ones).
EF_INLINE Lr finiteLr(Lr x) {
#if EF_NEON
    const uint32x2_t bits = vand_u32(vreinterpret_u32_f32(x), vdup_n_u32(0x7fffffffu));
    return vbsl_f32(vclt_u32(bits, vdup_n_u32(0x7f800000u)), x, vdup_n_f32(0.0f));
#else
    LrMask bits;
    std::memcpy(&bits, &x, sizeof bits);
    return (bits & 0x7fffffff) < 0x7f800000 ? x : Lr{};
#endif
}
// common.h's hermite() for both lanes.
EF_INLINE Lr hermiteLr(Lr xm1, Lr x0, Lr x1, Lr x2, Lr t) {
    const Lr c1 = both(0.5f) * (x1 - xm1);
    const Lr c2 = xm1 - both(2.5f) * x0 + both(2.0f) * x1 - both(0.5f) * x2;
    const Lr c3 = both(0.5f) * (x2 - xm1) + both(1.5f) * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}

// A parameter clamped to its range; NaN (which no comparison catches) becomes `nan`.
template <class T>
T clampOr(T x, T lo, T hi, T nan) {
    return x >= lo ? (x <= hi ? x : hi) : (x < lo ? lo : nan);
}

constexpr int kSeg = 32;                          // the glide, the wow and the fades step per segment
constexpr double kMaxTime = 3.0 * ef::kRate;      // 1 bar at 80 BPM
constexpr double kMinTime = 0.001 * ef::kRate;    // 1 ms
// A chunk is written after it is read, so a tap must lie more than a chunk behind: the Hermite kernel
// reaches one sample nearer than the integer age, and that must still be written. The time itself is
// never under 44.1 samples; only the wow can take a read nearer, and it gives way at kMinRead (a sample
// past kMinAge, so the kernel always interpolates and never reaches back past what is written).
constexpr int kMinAge = ef::kChunk + 1;
constexpr double kMinRead = kMinAge + 1.0;
constexpr float kWowDepth = 0.003f * ef::kRate;   // +-3 ms at wow 1
constexpr float kFlutter = 0.2f / 3.0f;           // the flutter's depth against the wow's: +-0.2 ms
constexpr float kWowHz = 0.5f, kFlutterHzL = 6.0f, kFlutterHzR = 6.6f;
constexpr float kWowLagR = 0.25f;                 // cycles
// The lines: the longest time plus the wow's depth, and the cubic read's two samples on either side.
constexpr int kLen = static_cast<int>(kMaxTime) + static_cast<int>(kWowDepth * (1.0f + kFlutter)) + 8;
constexpr int kGuard = 3;                         // the first samples again past the end: a read never wraps
constexpr int kMaxAge = kLen - 3;                 // the oldest whole delay a read may ask for
constexpr float kClamp = 4.0f;                    // +12 dBFS: with the low cut's overshoot, +18 at most reaches the limiter
constexpr float kTiny = 1e-18f;                   // a DC offset (-360 dB) under everything in the loop: no denormals
constexpr float kMonoStep = 1.0f / 16.0f;         // Mono's share of R's wow, per segment: 12 ms to switch
constexpr int kTailForever = 1 << 30;             // feedback 1: 6.8 hours
constexpr int kFadeSegs = 69;                     // Fade's crossfade: 69 segments, 50 ms

// What the controls do in a chunk: a full scale in 10 ms (so no gain edge is steeper than 1 ms), a cutoff
// 10 octaves in 10 ms.
constexpr float kFadeStep = ef::kChunk / (0.010f * ef::kRate);
constexpr float kOctaveStep = 10.0f * kFadeStep;

// The filter's Q runs from 0.7071 (a Butterworth: no peak at resonance 0) to 12 at resonance 1, evenly
// in octaves of Q: Shape's.
constexpr float kInvQ0 = 1.41421356f;             // 1 / 0.7071
constexpr float kLogQRange = 4.0849625f;          // log2 (12 / 0.7071)
constexpr float kOffHz = 20000.0f;                // a low-pass here or above is off

// A trapezoidal one-pole's step, g / (1 + g) for g = tan (pi hz / rate): its low-pass follows its input
// by this much of the way a sample, and the high-pass is what is left (unity gain at Nyquist).
float onePoleStep(float hz) {
    const float g = std::tan(ef::kPi * hz / ef::kRate);
    return g / (1.0f + g);
}

// Set when the plugin loads (no guard to take on the audio thread).
const float kWowSmooth = 1.0f - std::exp(-kSeg / (0.1f * ef::kRate));   // the wow amount glides
const float kRelease = std::exp(1.0f / (0.1f * ef::kRate));             // the limiter's, per sample
const float kLowCut = onePoleStep(30.0f);                               // the low cut's, at 30 Hz
const double kGlide = 1.0 - std::exp(-kSeg / (0.060 * ef::kRate));      // Tape's step per segment
const float kMinOct = std::log2(10.0f), kMaxOct = std::log2(0.45f * ef::kRate);   // the cutoff's range

// R36: the loop never gains. The filter's gain is at most (1 - wet) + wet (wLP P + wBP + wHP P), where
// w are the type weights (a blend of types peaks at no more than the weighted sum of their peaks) and P
// = 1 / (k sqrt(1 - k^2 / 4)) is the low-pass's and the high-pass's peak at k = 1 / Q (1 up to Q = 0.7071,
// where k is 1.4142: the Butterworth, exactly 1). What is fed back is scaled by 1 over that, never raised.
float filterScale(const float* type, float k, float wet) {
    const float p = k >= kInvQ0 ? 1.0f : std::max(1.0f, 1.0f / (k * std::sqrt(std::max(1.0f - 0.25f * k * k, 0.5f))));
    const float peak = (1.0f - wet) + wet * ((type[EchoLine::kLP] + type[EchoLine::kHP]) * p + type[EchoLine::kBP]);
    return 1.0f / std::max(1.0f, peak);
}

// Both lines read at their times: the taps at ages i - 1 .. i + 2, four samples in a row, through the
// Hermite kernel. `w` is where the sample being read would be written.
EF_INLINE Lr readLr(const float* bl, const float* br, int w, double teL, double teR) {
    const int il = std::clamp(static_cast<int>(teL), kMinAge, kMaxAge), ir = std::clamp(static_cast<int>(teR), kMinAge, kMaxAge);
    const Lr frac = pairLr(static_cast<float>(teL - il), static_cast<float>(teR - ir));
    int ql = w - il - 2, qr = w - ir - 2;
    if (ql < 0) ql += kLen;
    if (qr < 0) qr += kLen;
    const float* pl = bl + ql;
    const float* pr = br + qr;
    return hermiteLr(ef::load2(pl + 3, pr + 3), ef::load2(pl + 2, pr + 2), ef::load2(pl + 1, pr + 1), ef::load2(pl, pr), frac);
}

// Fade's equal-power gains for the old and the new head at the end of segment `fade` of the
// crossfade (none running: 1 and 0), exact at both ends.
void fadeGains(int fade, float& a, float& b) {
    const float x = fade < 0 ? 0.0f : static_cast<float>(fade + 1) / kFadeSegs;
    a = x <= 0.0f ? 1.0f : (x >= 1.0f ? 0.0f : ef::sinQuarter(0.5f * ef::kPi * (1.0f - x)));
    b = x <= 0.0f ? 0.0f : (x >= 1.0f ? 1.0f : ef::sinQuarter(0.5f * ef::kPi * x));
}

// Tape: the time one segment on, a fraction of the way to the target. The last millionth snaps, so
// it lands exactly. In double: in float the step drops under the precision of a time near 22050
// samples and the glide stalls a few samples short of its target, for good.
double glideTime(double current, double target) {
    const double d = target - current;
    return std::fabs(d) < 1e-6 ? target : current + d * kGlide;
}

// A smoothed amount one segment on; snaps the last bit so it lands (wow 0 is exactly no wow).
float follow(float cur, float target, float coef) {
    const float d = target - cur;
    return std::fabs(d) < 1e-5f ? target : cur + d * coef;
}
// `cur` moved toward `target` by at most `step`; it lands on the target exactly.
float slew(float cur, float target, float step) {
    const float d = target - cur;
    return d > step ? cur + step : (d < -step ? cur - step : target);
}
float wrap1(float x) { return x >= 1.0f ? x - 1.0f : x; }

// Within a millionth of a sample of a whole number: whole (t > 0).
double wholeIfClose(double t) {
    const double r = ef::floorFast(t + 0.5);
    return std::fabs(t - r) < 1e-6 ? r : t;
}

// A read time with the wow added, never nearer than kMinRead.
double wowTime(double t, float wow) { return std::max(t + wow, kMinRead); }

// Until a line has been written all the way round since reset(), it still holds what came before.
// Before a run of reads that ends n samples into the chunk (w is where the chunk's first sample will
// be written), at times te + d .. te + n d, zero what of that its taps can reach (ages past `written`;
// those written since stay): a few dozen samples, and only the first time round, instead of 530 KB at
// once or a check on every tap.
void hideOld(float* line, int w, int written, double te, double d, int n) {
    const double lo = std::min(te + d, te + n * d), hi = std::max(te + d, te + n * d);
    // At sample k of the chunk a tap of age a reads what is k samples younger than at the chunk's start
    // (where w is); all of those are older than the chunk, written before it.
    const int from = std::max({written + 1, std::clamp(static_cast<int>(lo), kMinAge, kMaxAge) - n, 1});
    const int to = std::clamp(static_cast<int>(hi), kMinAge, kMaxAge) + 2;
    for (int a = from; a <= to; ++a) {
        const int j = w - a < 0 ? w - a + kLen : w - a;
        line[j] = 0.0f;
        if (j < kGuard) line[kLen + j] = 0.0f;
    }
}

int wrapped(int w) { return w >= kLen ? w - kLen : w; }

} // namespace

EchoLine::EchoLine() { reset(); }

bool EchoLine::allocate() {
    if (allocated()) return true;
    try {
        lineL_.assign(static_cast<size_t>(kLen + kGuard), 0.0f);
        lineR_.assign(static_cast<size_t>(kLen + kGuard), 0.0f);
    } catch (const std::bad_alloc&) {
        std::vector<float>().swap(lineL_);
        std::vector<float>().swap(lineR_);
        return false;
    }
    reset();
    return true;
}

void EchoLine::reset() {
    w_ = 0;
    written_ = 0;
    readN_ = 0;
    low_ = ef::f2{};
    gain_ = ef::f2{1.0f, 1.0f};
    phWow_ = phFlutL_ = phFlutR_ = 0.0f;
    segLeft_ = 0;
    fade_ = -1;
    fading_ = false;
    fRunning_ = false;
    fOct_ = fRes_ = fWet_ = 0.0f;
    fLoop_ = 1.0f;
    fType_[0] = fType_[1] = fType_[2] = 0.0f;
    gl_ = ef::Glide<kValues, 1>{};
    a_ = ef::SvfUpdate{};
    s_.clear();
    fresh_ = true;
}

float EchoLine::peakStored() const {
    float m = 0.0f;
    for (const std::vector<float>* line : {&lineL_, &lineR_})
        for (float v : *line) {
            if (!std::isfinite(v)) return INFINITY;
            m = std::max(m, std::fabs(v));
        }
    return m;
}

void EchoLine::set(const Params& in, const ef::Transport& tr) {
    const int mode = std::clamp(in.mode, 0, 2);
    glideType_ = std::clamp(in.glide, 0, 1);   // a fade under way finishes first
    double base;
    if (in.sync) {
        const double beats = clampOr(in.divBeats, kEchoDivs[0].beats, kEchoDivs[kNumEchoDivs - 1].beats, 0.75);
        base = ef::divSeconds(beats, clampOr(tr.bpm, 1.0, 1000.0, 120.0)) * ef::kRate;
    } else {
        base = static_cast<double>(clampOr(in.timeMs, 1.0f, 2000.0f, 375.0f)) * ef::kRate / 1000.0;
    }
    base *= static_cast<double>(clampOr(in.timeMod, 0.001f, 1000.0f, 1.0f));
    tgtT_ = wholeIfClose(std::clamp(base, kMinTime, kMaxTime));

    const float fb = clampOr(in.feedback, 0.0f, 1.0f, 0.0f);
    float want[kRamps];
    want[FB] = fb;
    want[DRIVE] = clampOr(in.drive, 0.0f, 1.0f, 0.0f);
    want[MONO_IN] = mode == kStereo ? 0.0f : 1.0f;
    want[CROSS] = mode == kPingPong ? 1.0f : 0.0f;
    want[R_IN] = mode == kPingPong ? 0.0f : 1.0f;
    wowTgt_ = clampOr(in.wow, 0.0f, 1.0f, 0.0f);
    monoTgt_ = mode == kMono ? 1.0f : 0.0f;

    if (fresh_) {   // jump: no ramps, no glide, the wow where it would be by now
        for (int k = 0; k < kRamps; ++k) cur_[k] = goal_[k] = want[k];
        t_ = tgtT_;
        tB_ = t_;
        wowAmt_ = wowTgt_;
        mono_ = monoTgt_;
        float ml, mr;
        wowNow(t_, ml, mr);
        teL_ = teEndL_ = wowTime(t_, ml);
        teR_ = teEndR_ = wowTime(t_, mr);
        teStepL_ = teStepR_ = 0.0;
        teBL_ = teBEndL_ = teL_;
        teBR_ = teBEndR_ = teR_;
        teBStepL_ = teBStepR_ = 0.0;
        gA_ = gAEnd_ = 1.0f;
        gB_ = gBEnd_ = 0.0f;
        gAStep_ = gBStep_ = 0.0f;
    } else {
        for (int k = 0; k < kRamps; ++k) goal_[k] = slew(goal_[k], want[k], kFadeStep);
    }

    setFilter(in);
    fresh_ = false;

    // Down to -60 dB: repeat k is fb^(k-1) of the first (at the passband), at most k times the time
    // away. Plus the low cut's ring and the filter's. Only when the feedback or the times change (the
    // logs only for the feedback).
    const double longest = std::max({tgtT_, t_, tB_}) + wowTgt_ * kWowDepth * (1.0f + kFlutter);
    if (fb != tailFb_ || longest != tailLongest_ || fRing_ != tailRing_) {
        if (fb != tailFb_) {
            tailFb_ = fb;
            repeats_ = fb > 1e-6f && fb < 1.0f ? std::floor(std::log(1e-3) / std::log(static_cast<double>(fb))) + 1.0 : 1.0;
        }
        tailLongest_ = longest;
        tailRing_ = fRing_;
        tail_ = fb >= 1.0f ? kTailForever
                           : static_cast<int>(std::min(longest * repeats_ + 0.06 * ef::kRate + fRing_, static_cast<double>(kTailForever)));
    }
}

// The glide's targets for the filter's state now.
void EchoLine::filterTargets(ef::f2* t) const {
    const float hz = ef::exp2Fast(fOct_);
    const float k = kInvQ0 * ef::exp2Fast(-kLogQRange * fRes_);
    t[G] = ef::splat2(ef::tanFast(ef::kPi * hz / ef::kRate));
    t[K] = ef::splat2(k);
    // LP is (0, 0, 1), BP (0, k, 0) (0 dB at the cutoff), HP (1, -k, -1); the weights blend them.
    t[M0] = ef::splat2(fType_[kHP]);
    t[M1] = ef::splat2((fType_[kBP] - fType_[kHP]) * k);
    t[M2] = ef::splat2(fType_[kLP] - fType_[kHP]);
    t[WET] = ef::splat2(fWet_);
    t[LOOP] = ef::splat2(fLoop_);
    t[SCALE] = ef::splat2(filterScale(fType_, k, fWet_));
}

float EchoLine::loopScale() const { return fRunning_ ? gl_.cur()[SCALE][0] : 1.0f; }

// The filter runs while it is on and while its wet share fades out; starting, its state is empty
// (nothing was running) and its wet share fades in from 0.
void EchoLine::setFilter(const Params& p) {
    const int type = std::clamp(p.filterType, 0, 2);
    const float res = clampOr(p.resonance, 0.0f, 1.0f, 0.0f);
    const float hz = clampOr(p.cutoffHz, 20.0f, 20000.0f, 20000.0f);
    const float mod = clampOr(p.cutoffModOct, -10.0f, 10.0f, 0.0f);
    const bool off = type == kLP && hz * ef::exp2Fast(mod) >= kOffHz;
    const float wet = off ? 0.0f : 1.0f;
    const float loop = std::clamp(p.place, 0, 1) == kInLoop ? 1.0f : 0.0f;
    const float oct = ef::clampf(ef::log2Fast(hz) + mod, kMinOct, kMaxOct);
    const float tp[3] = {type == kLP ? 1.0f : 0.0f, type == kBP ? 1.0f : 0.0f, type == kHP ? 1.0f : 0.0f};

    const bool was = fRunning_;
    fRunning_ = !off || fWet_ != 0.0f || gl_.cur()[WET][0] != 0.0f;
    if (!fRunning_) {
        fRing_ = 0.0f;
        return;
    }
    // The ring of the sharpest setting down to -60 dB: ln(1000) 2 Q / w at the cutoff.
    fRing_ = off ? 0.0f : 6.9078f * ef::exp2Fast(-0.5f + kLogQRange * res) * ef::kRate / (ef::kPi * ef::exp2Fast(oct));
    if (!was) {
        s_.clear();
        fOct_ = oct;
        fRes_ = res;
        for (int i = 0; i < 3; ++i) fType_[i] = tp[i];
        fLoop_ = loop;
        fWet_ = fresh_ ? wet : 0.0f;
        ef::f2 t[kValues];
        filterTargets(t);
        gl_.jump(t);
    }
    fOct_ = slew(fOct_, oct, kOctaveStep);
    fRes_ = slew(fRes_, res, kFadeStep);
    for (int i = 0; i < 3; ++i) fType_[i] = slew(fType_[i], tp[i], kFadeStep);
    fLoop_ = slew(fLoop_, loop, kFadeStep);
    fWet_ = slew(fWet_, wet, kFadeStep);

    // Where the glide goes: nothing to do if nothing has moved since the last chunk.
    const float state[7] = {fOct_, fRes_, fType_[0], fType_[1], fType_[2], fWet_, fLoop_};
    if (!was || std::memcmp(state, fState_, sizeof state) != 0) {
        std::memcpy(fState_, state, sizeof state);
        ef::f2 t[kValues];
        filterTargets(t);
        a_ = ef::SvfUpdate::of(t[G], t[K]);
        gl_.to(t);
    }
}

// The wow's offsets (samples) for a head at time t, at the phases and amounts now.
void EchoLine::wowNow(double t, float& l, float& r) const {
    const float depth = wowAmt_ * std::min(kWowDepth, 0.25f * static_cast<float>(t));
    l = depth * (ef::sinCycle(phWow_) + kFlutter * ef::sinCycle(phFlutL_));
    const float own = depth * (ef::sinCycle(wrap1(phWow_ + kWowLagR)) + kFlutter * ef::sinCycle(phFlutR_));
    r = (1.0f - mono_) * own + mono_ * l;   // exact at 0 and 1: Mono's R is L
}

// Every 32 samples: the glide, the wow and the fade at the segment's end, and their steps.
void EchoLine::segment() {
    constexpr float kWowInc = kSeg * kWowHz / ef::kRate, kFlutIncL = kSeg * kFlutterHzL / ef::kRate,
                    kFlutIncR = kSeg * kFlutterHzR / ef::kRate;

    teL_ = teEndL_;   // the last segment lands exactly where it was aimed
    teR_ = teEndR_;
    teBL_ = teBEndL_;
    teBR_ = teBEndR_;
    gA_ = gAEnd_;
    gB_ = gBEnd_;

    wowAmt_ = follow(wowAmt_, wowTgt_, kWowSmooth);
    mono_ = mono_ < monoTgt_ ? std::min(monoTgt_, mono_ + kMonoStep) : std::max(monoTgt_, mono_ - kMonoStep);
    phWow_ = wrap1(phWow_ + kWowInc);
    phFlutL_ = wrap1(phFlutL_ + kFlutIncL);
    phFlutR_ = wrap1(phFlutR_ + kFlutIncR);

    // The heads. A fade that is over hands the reading to its new head. Then Tape glides; Fade snaps a
    // change under half a sample, or starts a fade to the target.
    if (fade_ >= 0 && ++fade_ == kFadeSegs) {
        t_ = tB_;
        teL_ = teBL_;
        teR_ = teBR_;
        gA_ = 1.0f;
        gB_ = 0.0f;
        fade_ = -1;
    }
    bool started = false;
    if (fade_ < 0) {
        if (glideType_ == kTape) {
            t_ = glideTime(t_, tgtT_);
            tB_ = t_;
        } else if (std::fabs(tgtT_ - t_) < 0.5) {
            t_ = tgtT_;
            tB_ = t_;
        } else {
            tB_ = tgtT_;
            fade_ = 0;
            started = true;
        }
    }

    float ml, mr;
    wowNow(t_, ml, mr);
    teEndL_ = wowTime(t_, ml);
    teEndR_ = wowTime(t_, mr);
    teStepL_ = (teEndL_ - teL_) * (1.0 / kSeg);
    teStepR_ = (teEndR_ - teR_) * (1.0 / kSeg);

    fading_ = fade_ >= 0;
    if (fading_) {
        wowNow(tB_, ml, mr);
        teBEndL_ = wowTime(tB_, ml);
        teBEndR_ = wowTime(tB_, mr);
        if (started) {   // a new head starts where it will be: its gain is still near 0
            teBL_ = teBEndL_;
            teBR_ = teBEndR_;
        }
    } else {   // B reads what A does, at gain 0
        teBL_ = teL_;
        teBR_ = teR_;
        teBEndL_ = teEndL_;
        teBEndR_ = teEndR_;
    }
    teBStepL_ = (teBEndL_ - teBL_) * (1.0 / kSeg);
    teBStepR_ = (teBEndR_ - teBR_) * (1.0 / kSeg);
    fadeGains(fade_, gAEnd_, gBEnd_);
    gAStep_ = (gAEnd_ - gA_) * (1.0f / kSeg);
    gBStep_ = (gBEnd_ - gB_) * (1.0f / kSeg);
    segLeft_ = kSeg;
}

void EchoLine::read(float* outL, float* outR, int n) {
    n = std::min(n, ef::kChunk);
    readN_ = 0;
    if (n <= 0) return;
    if (!allocated()) {   // no lines yet: silence
        std::fill(outL, outL + n, 0.0f);
        std::fill(outR, outR + n, 0.0f);
        return;
    }
    for (int i = 0; i < n;) {
        if (segLeft_ == 0) segment();
        const int m = std::min(n - i, segLeft_);
        const int w = wrapped(w_ + i);
        if (fRunning_) {
            if (fading_) readRun<true, true>(outL + i, outR + i, w, i, m);
            else readRun<false, true>(outL + i, outR + i, w, i, m);
        } else {
            if (fading_) readRun<true, false>(outL + i, outR + i, w, i, m);
            else readRun<false, false>(outL + i, outR + i, w, i, m);
        }
        segLeft_ -= m;
        i += m;
    }
    readN_ = n;
}

// n reads inside one segment, the first at sample `offset` of the chunk (the write point `w` is
// where its sample will go). Locals: the compiler can't keep members in registers across the stores
// to the outputs.
template <bool Fading, bool Filt>
void EchoLine::readRun(float* outL, float* outR, int w, int offset, int n) {
    float* const bl = lineL_.data();
    float* const br = lineR_.data();
    double teL = teL_, teR = teR_;
    const double dl = teStepL_, dr = teStepR_;
    double teBL = teBL_, teBR = teBR_;
    const double dbl = teBStepL_, dbr = teBStepR_;
    Lr gA = both(gA_), gB = both(gB_);
    const Lr sGA = both(gAStep_), sGB = both(gBStep_);
    ef::Glide<kValues, 1> g = gl_;
    ef::SvfState s = s_;
    const ef::SvfUpdate settled = a_;
    const Lr one = both(1.0f);
    if (written_ < kLen) {
        // The reads are of what was written before this chunk: ages from the chunk's start (w_) on.
        hideOld(bl, w_, written_, teL, dl, offset + n);
        hideOld(br, w_, written_, teR, dr, offset + n);
        if (Fading) {
            hideOld(bl, w_, written_, teBL, dbl, offset + n);
            hideOld(br, w_, written_, teBR, dbr, offset + n);
        }
    }

    for (int k = 0; k < n; ++k) {
        teL += dl;
        teR += dr;
        Lr r = readLr(bl, br, w, teL, teR);
        if (Fading) {   // and the new head, crossfaded in
            teBL += dbl;
            teBR += dbr;
            gA += sGA;
            gB += sGB;
            r = r * gA + readLr(bl, br, w, teBL, teBR) * gB;
        }
        w = w + 1 == kLen ? 0 : w + 1;

        Lr y = r, src = r;
        if (Filt) {
            const bool gliding = g.moving();
            g.next();
            const Lr* v = g.cur();
            const ef::SvfUpdate a = gliding ? ef::SvfUpdate::fast(v[G], v[K]) : settled;
            const Lr mix[3] = {v[M0], v[M1], v[M2]};
            y = r * (one - v[WET]) + s.tick(ef::safeIn(r), a, mix) * v[WET];   // exact at wet 0 and 1
            src = r * (one - v[LOOP]) + y * (v[LOOP] * v[SCALE]);               // the loop carries y x scale In loop, r On output
        }
        ef::store2(outL + k, outR + k, y);
        back_[offset + k] = src;
    }

    teL_ = teL;
    teR_ = teR;
    if (Fading) {
        teBL_ = teBL;
        teBR_ = teBR;
        gA_ = gA[0];
        gB_ = gB[0];
    }
    if (Filt) {
        s.flushTiny();
        gl_ = g;
        s_ = s;
    }
}

void EchoLine::write(const float* inL, const float* inR, int n) {
    n = std::min(n, readN_);
    if (n <= 0) return;
    const float inv = 1.0f / static_cast<float>(n);
    bool moving = false;
    for (int k = 0; k < kRamps; ++k) {
        step_[k] = (goal_[k] - cur_[k]) * inv;
        moving = moving || goal_[k] != cur_[k];
    }
    if (moving) writeRun<true>(inL, inR, n);
    else writeRun<false>(inL, inR, n);
    for (int k = 0; k < kRamps; ++k) cur_[k] = goal_[k];   // lands exactly
    w_ = wrapped(w_ + n);
    written_ = std::min(written_ + n, kLen);
    readN_ = 0;
}

template <bool Moving>
void EchoLine::writeRun(const float* inL, const float* inR, int n) {
    float* const bl = lineL_.data();
    float* const br = lineR_.data();
    int w = w_;
    Lr low = low_, gain = gain_;
    const Lr aLow = both(kLowCut), release = both(kRelease), one = both(1.0f), half = both(0.5f);
    Lr fb = both(cur_[FB]), drive = both(cur_[DRIVE]), monoIn = both(cur_[MONO_IN]), cross = both(cur_[CROSS]),
       rIn = Lr{1.0f, cur_[R_IN]};
    const Lr sFb = both(step_[FB]), sDrive = both(step_[DRIVE]), sMonoIn = both(step_[MONO_IN]), sCross = both(step_[CROSS]),
             sRIn = Lr{0.0f, step_[R_IN]};

    for (int k = 0; k < n; ++k) {
        if (Moving) {
            fb += sFb;
            drive += sDrive;
            monoIn += sMonoIn;
            cross += sCross;
            rIn += sRIn;
        }
        // NaN and infinity are silence; anything wild but finite is held to +-kClamp here, so that no sum below
        // can overflow (the compiler is free to add L and R before halving them).
        const Lr x = ef::min2(ef::max2(finiteLr(ef::load2(inL + k, inR + k)), both(-kClamp)), both(kClamp));

        // The input (or its mono sum; none into R in Ping-Pong) and the feedback (from the other side in
        // Ping-Pong).
        const Lr sum = half * x + half * swapLr(x);
        const Lr in = (x * (one - monoIn) + sum * monoIn) * rIn;
        const Lr back = back_[k] * (one - cross) + swapLr(back_[k]) * cross;
        Lr u = ef::min2(ef::max2(in + fb * back + both(kTiny), both(-kClamp)), both(kClamp));

        // The low cut (a trapezoidal one-pole: its low-pass is v + low), and the tiny offset again under the signal.
        const Lr vLow = (u - low) * aLow;
        const Lr lpLow = vLow + low;
        low = lpLow + vLow;
        u = u - lpLow + both(kTiny);

        const Lr c = ef::min2(ef::max2(u, both(-0.5f)), both(0.5f));
        Lr v = u + drive * (c - both(4.0f / 3.0f) * c * c * c - u);

        // The limiter: the gain recovers, then drops at once to what keeps this sample at 1.
        const Lr a = absLr(v);
        gain = ef::min2(gain * release, one);
        gain = pickLr(greaterLr(a * gain, one), ef::recip2(ef::max2(a, one)), gain);
        v *= gain;

        ef::store2(bl + w, br + w, v);
        if (w < kGuard) ef::store2(bl + kLen + w, br + kLen + w, v);
        w = w + 1 == kLen ? 0 : w + 1;
    }

    low_ = low;
    gain_ = gain;
}

} // namespace gf
