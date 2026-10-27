// Shape: see shape.h.
#include "shape.h"
#include "slew.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <type_traits>

namespace gf {

// copyStateFrom() is a plain copy of the object: no allocation, nothing to fix up.
static_assert(std::is_trivially_copyable<Shape>::value, "Shape is copied bit for bit");

namespace {

constexpr float kQ0 = 0.707106781f;      // Butterworth: no peak at resonance 0
constexpr float kQRange = 16.9705627f;   // 12 / kQ0: Q at resonance 1 is 12
constexpr float kMinHz = 10.0f;          // the cutoff's floor (20 Hz with the shape four octaves down would be 1.25)
constexpr float kMaxHz = 0.45f * ef::kRate;
constexpr double kMinRate = 0.0625, kMaxRate = 64.0;   // beats a cycle

// The most a value moves a sample: the shape's value a full scale in 1 ms (so no edge is steeper than
// 1 ms). The pan law turns the value's step into a gain's up to sqrt2 pi/2 = 2.22 times as big (a = pi/4 p,
// p = 2 y - 1, g = cos a -+ sin a), so the Pan lane's value moves that much slower: its gains obey the 1 ms
// rule too, and a sweep from one side to the other takes 2.2 ms. In a chunk: the controls a full scale in
// 10 ms, a cutoff 10 octaves in 10 ms.
constexpr float kShapeSlope = 1.0f / (0.001f * ef::kRate);
constexpr float kPanSlope = kShapeSlope / 2.22144147f;
constexpr float kFadeStep = ef::kChunk / (0.010f * ef::kRate);
constexpr float kOctaveStep = 10.0f * kFadeStep;

constexpr float kQuarterPi = 0.785398163f;

// cos(a) for |a| <= pi/4 as a short series (ef::sinQuarter does the sin): the pan's angle. Sin is exactly 0
// and cos exactly 1 at 0, so a pan in the middle is unity gain to the bit.
inline float cosSmall(float a) {
    const float a2 = a * a;
    return 1.0f + a2 * (-0.5f + a2 * (4.1666667e-2f + a2 * (-1.3888889e-3f + a2 * 2.4801587e-5f)));
}

// The parameters in range: out-of-range values clamped, NaN the parameter's default.
Shape::Params sanitized(const Shape::Params& in) {
    Shape::Params p = in;
    for (int l = 0; l < Shape::kLanes; ++l) {
        Shape::LaneParams& q = p.lane[l];
        q.shape = std::clamp(q.shape, 0, numShapes() - 1);
        q.rateBeats = q.rateBeats >= kMinRate ? std::min(q.rateBeats, kMaxRate) : (q.rateBeats < kMinRate ? kMinRate : 1.0);
        q.depth = ef::clampParam(q.depth, l == Shape::kVolume ? 0.0f : -1.0f, 1.0f, 0.0f);
        q.phase = ef::clampParam(q.phase, 0.0f, 1.0f, 0.0f);
    }
    p.amount = ef::clampParam(p.amount, 0.0f, 1.0f, 1.0f);
    p.filterType = std::clamp(p.filterType, 0, 2);
    p.cutoffHz = ef::clampParam(p.cutoffHz, 20.0f, 20000.0f, 20000.0f);
    p.resonance = ef::clampParam(p.resonance, 0.0f, 1.0f, 0.0f);
    p.mix = ef::clampParam(p.mix, 0.0f, 1.0f, 1.0f);
    return p;
}

} // namespace

Shape::Shape() { reset(); }

void Shape::reset() {
    vol_ = GainLane{};
    pan_ = GainLane{};
    for (Eval& e : last_) e = Eval{};
    fRunning_ = false;
    fY_ = fOct_ = fDepth_ = fRes_ = fWet_ = 0.0f;
    fType_[0] = fType_[1] = fType_[2] = 0.0f;
    derived_ = Derived{};
    gl_ = ef::Glide<kValues, 1>{};
    a_ = ef::SvfUpdate{};
    s_.clear();
    pos_ = 0.0;
    perSample_ = 120.0 / (60.0 * ef::kRate);
    fresh_ = true;
    tail_ = 0;
}

// A lane's shape at the chunk's start and end (positions on straight time, in beats). The start is
// the value the last chunk found at its end, when the song position has gone on from there.
void Shape::valuesOf(int lane, const LaneParams& q, double start, double end, float& v0, float& v1) {
    const double c0 = start / q.rateBeats + q.phase, c1 = end / q.rateBeats + q.phase;
    Eval& e = last_[lane];
    v0 = e.valid && e.shape == q.shape && std::fabs(c0 - e.cycles) < 1e-9 ? e.value : shapeAt(q.shape, c0);
    v1 = shapeAt(q.shape, c1);
    e = Eval{true, q.shape, c1, v1};
}

void Shape::set(const Params& in, const ef::Transport& t) {
    const Params p = sanitized(in);
    const double bpm = ef::tempoOf(t);
    perSample_ = bpm / (60.0 * ef::kRate);
    // Playing, the song position is MPC's (it moves for a locate, a loop, a tempo change). Otherwise
    // the position runs on at the tempo from where it was (process() moves it): from MPC's, the first
    // chunk after reset(), if it says where, else from 0.
    if (t.valid && std::isfinite(t.beats) && (t.playing || fresh_)) pos_ = t.beats;
    const double start = toStraight(pos_, p.swing);
    const double end = toStraight(pos_ + ef::kChunk * perSample_, p.swing);

    const LaneParams& qv = p.lane[kVolume];
    const LaneParams& qp = p.lane[kPan];
    setGainLane(kVolume, vol_, qv, qv.on ? p.mix * qv.depth * p.amount : 0.0f, 1.0f, start, end);
    setGainLane(kPan, pan_, qp, qp.on ? qp.depth * p.amount : 0.0f, p.mix, start, end);
    setFilter(p, start, end);
    fresh_ = false;
}

// Volume and Pan. The lane runs while it is on and while its controls still fade out. Starting, its
// value is the shape's where it is (the controls fade in from nothing, so that is no step); then each
// chunk the shape goes in a line from its value at the chunk's start to its value at the end (process()
// limits the lane's value to it), and the controls move toward theirs by at most the 10 ms limit.
void Shape::setGainLane(int lane, GainLane& s, const LaneParams& q, float cTarget, float mTarget, double start, double end) {
    const bool was = s.running;
    s.running = q.on || s.c.cur != 0.0f || s.c.target != 0.0f;
    if (!s.running) {
        last_[lane].valid = false;
        return;
    }
    float v0, v1;
    valuesOf(lane, q, start, end, v0, v1);
    if (!was) {
        s.y = v0;
        s.m.jump(mTarget);
        if (fresh_) s.c.jump(cTarget);   // (else c is 0, and fades in)
    }
    s.shape.between(v0, v1);
    s.c.to(toward(s.c.cur, cTarget, kFadeStep));
    s.m.to(toward(s.m.cur, mTarget, kFadeStep));
}

// The glide's targets for the filter's state now.
void Shape::filterTargets(ef::f2* t) const {
    const float hz = ef::clampf(std::exp2(fOct_ + 4.0f * fDepth_ * (fY_ - 1.0f)), kMinHz, kMaxHz);
    const float k = 1.0f / (kQ0 * std::pow(kQRange, fRes_));
    t[G] = ef::splat2(ef::svfG(hz));
    t[K] = ef::splat2(k);
    // LP is (0, 0, 1), BP (0, k, 0) (0 dB at the cutoff), HP (1, -k, -1); the weights blend them.
    t[M0] = ef::splat2(fType_[kHP]);
    t[M1] = ef::splat2((fType_[kBP] - fType_[kHP]) * k);
    t[M2] = ef::splat2(fType_[kLP] - fType_[kHP]);
    t[WET] = ef::splat2(fWet_);
}

// The filter lane. It runs while it is on and while its wet share fades out; starting, its state is
// empty (nothing was running) and its wet share fades in from 0.
void Shape::setFilter(const Params& p, double start, double end) {
    const LaneParams& q = p.lane[kFilter];
    const bool was = fRunning_;
    fRunning_ = q.on || fWet_ != 0.0f || gl_.cur()[WET][0] != 0.0f;
    if (!fRunning_) {
        tail_ = 0;
        last_[kFilter].valid = false;
        return;
    }
    float v0, v1;
    valuesOf(kFilter, q, start, end, v0, v1);
    const float d = q.depth * p.amount, wet = q.on ? p.mix : 0.0f;
    // The cutoff in octaves and the ring follow the parameters, which seldom move: libm only when they do.
    if (p.cutoffHz != derived_.hz || p.resonance != derived_.res || d != derived_.d) {
        derived_.hz = p.cutoffHz, derived_.res = p.resonance, derived_.d = d;
        derived_.oct = std::log2(p.cutoffHz);
        // The ring of the sharpest setting down to -60 dB: ln(1000) 2 Q / w at the lowest cutoff the shape reaches.
        const float lowHz = ef::clampf(p.cutoffHz * std::exp2(-4.0f * std::max(d, 0.0f)), kMinHz, kMaxHz);
        derived_.tail = static_cast<int>(6.9078f * kQ0 * std::pow(kQRange, p.resonance) * ef::kRate / (ef::kPi * lowHz)) + 1;
    }
    tail_ = derived_.tail;
    const float type[3] = {p.filterType == kLP ? 1.0f : 0.0f, p.filterType == kBP ? 1.0f : 0.0f, p.filterType == kHP ? 1.0f : 0.0f};
    if (!was) {
        s_.clear();
        fY_ = v0;
        fOct_ = derived_.oct;
        fDepth_ = d;
        fRes_ = p.resonance;
        for (int i = 0; i < 3; ++i) fType_[i] = type[i];
        fWet_ = fresh_ ? wet : 0.0f;
        ef::f2 t[kValues];
        filterTargets(t);
        gl_.jump(t);
    }
    // The value's limiter over the chunk (as process() runs it for Volume and Pan): where it gets to by the end.
    if (v0 != fY_ || v1 != fY_) {
        const float dv = (v1 - v0) * (1.0f / ef::kChunk);
        for (int i = 0; i < ef::kChunk; ++i) fY_ = toward(fY_, v0 + dv * static_cast<float>(i), kShapeSlope);
    }
    fOct_ = toward(fOct_, derived_.oct, kOctaveStep);
    fDepth_ = toward(fDepth_, d, kFadeStep);
    fRes_ = toward(fRes_, p.resonance, kFadeStep);
    for (int i = 0; i < 3; ++i) fType_[i] = toward(fType_[i], type[i], kFadeStep);
    fWet_ = toward(fWet_, wet, kFadeStep);

    // Where the glide goes: nothing to do if nothing has moved since the last chunk.
    const float state[8] = {fOct_, fDepth_, fY_, fRes_, fType_[0], fType_[1], fType_[2], fWet_};
    if (!was || std::memcmp(state, fState_, sizeof state) != 0) {
        std::memcpy(fState_, state, sizeof state);
        ef::f2 t[kValues];
        filterTargets(t);
        a_ = ef::SvfUpdate::of(t[G], t[K]);
        gl_.to(t);
    }
}

void Shape::process(float* L, float* R, int n) {
    n = std::min(n, ef::kChunk);
    if (n <= 0) return;
    pos_ += n * perSample_;
    const bool filt = fRunning_, vol = vol_.running, pan = pan_.running;
    if (!filt && !vol && !pan) return;   // nothing to do: the input as it is

    // Locals: the compiler can't keep members in registers across the stores to L and R.
    ef::Glide<kValues, 1> g = gl_;
    ef::SvfState s = s_;
    const ef::SvfUpdate settled = a_;
    const Line vs = vol_.shape, vc = vol_.c, ps = pan_.shape, pc = pan_.c, pm = pan_.m;
    float vy = vol_.y, py = pan_.y;
    const ef::f2 one = ef::splat2(1.0f);
    for (int i = 0; i < n; ++i) {
        ef::f2 x = ef::safeIn(ef::load2(L + i, R + i));
        if (filt) {
            const bool gliding = g.moving();
            g.next();
            const ef::f2* v = g.cur();
            const ef::SvfUpdate a = gliding ? ef::SvfUpdate::fast(v[G], v[K]) : settled;
            const ef::f2 mix[3] = {v[M0], v[M1], v[M2]};
            x = x * (one - v[WET]) + s.tick(x, a, mix) * v[WET];   // exact at wet 0 and 1
        }
        if (vol) {
            vy = toward(vy, vs.at(i), kShapeSlope);
            x = x * ef::splat2(1.0f - vc.at(i) * (1.0f - vy));
        }
        if (pan) {
            py = toward(py, ps.at(i), kPanSlope);
            const float a = kQuarterPi * (pc.at(i) * (2.0f * py - 1.0f));   // the angle from the middle
            const float sn = ef::sinQuarter(a), cs = cosSmall(a), m = pm.at(i);
            // sqrt2 cos(pi/4 + a) = cos a - sin a, sqrt2 sin(pi/4 + a) = cos a + sin a
            x = x * ef::f2{1.0f + m * (cs - sn - 1.0f), 1.0f + m * (cs + sn - 1.0f)};
        }
        ef::store2(L + i, R + i, x);
    }
    if (filt) {
        s.flushTiny();
        gl_ = g;
        s_ = s;
    }
    if (vol) {
        vol_.y = vy;
        vol_.c.advance(n);
    }
    if (pan) {
        pan_.y = py;
        pan_.c.advance(n);
        pan_.m.advance(n);
    }
}

} // namespace gf
