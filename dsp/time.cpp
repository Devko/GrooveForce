// Time: the engine (TimeCore) and the part with its grooves (Time): see time.h. The patterns themselves are surface/build/time_data.h.
#include "time.h"

#include "slew.h"
#include "svf.h"   // clampParam
#include "time_data.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <new>

namespace gf {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int64_t kMask = TimeCore::kRingFrames - 1;
constexpr float kGainStep = 1.0f / TimeCore::kEdge;   // the gain's quickest move a sample
constexpr double kHalfFrame = 0.5;                    // a playhead that lands within this of where it was heading did not jump
constexpr double kFar = 1e15;                         // a position this large has no fraction left to be a place in a pattern
const float kSilence[2] = {0.0f, 0.0f};

// The old head's weight at each step of a crossfade nothing foresaw (fade_ = kFade .. 1): a raised cosine,
// worked out once, as the looper does.
struct FadeWeights {
    float w[TimeCore::kFade + 1];
    FadeWeights() {
        for (int f = 0; f <= TimeCore::kFade; ++f) w[f] = 0.5f - 0.5f * std::cos(3.14159265f * static_cast<float>(f) / TimeCore::kFade);
    }
};
const FadeWeights kFadeWeights;

// timeFadeWeight()'s table: kWeightSteps steps of `remaining` over 0..1 and one more, to interpolate in the last.
constexpr int kWeightSteps = 4096;
struct RaisedCosine {
    float v[kWeightSteps + 2];
    RaisedCosine() {
        for (int k = 0; k <= kWeightSteps + 1; ++k) v[k] = static_cast<float>(0.5 + 0.5 * std::cos(kPi * k / kWeightSteps));
    }
};
const RaisedCosine kRaisedCosine;

const TimePatternDef& defOf(int i) { return kTimePatterns[std::clamp(i, 0, kNumTimePatterns - 1)]; }

void bindCurve(TimeCurve& c, int pattern, bool volume) {
    const TimePatternDef& d = defOf(pattern);
    c.bind(kTimePoints + (volume ? d.volFirst : d.srcFirst), volume ? d.volCount : d.srcCount);
}

// A frame of the ring, left and right, into one vector.
inline ef::f2 pair(const float* p) {
#if EF_NEON
    return vld1_f32(p);
#else
    return ef::f2{p[0], p[1]};
#endif
}

// The level a head plays at for its speed: the looper's, a tape stop ends in silence.
float speedGain(double speed) { return std::min(std::fabs(static_cast<float>(speed)) * 8.0f, 1.0f); }

// Whether the heads now (pos0, with p1 coming in at weight wNew) go on the sound the heads were making (e0, with e1 coming in at wOld): each
// head that counts, in either, is where it was heading, and a weight that counts has not been moved. A head with next to no weight is not heard.
bool carriesOn(double pos0, double p1, double wNew, double e0, double e1, double wOld) {
    constexpr double kUnheard = 1e-3;
    if ((1.0 - wNew > kUnheard || 1.0 - wOld > kUnheard) && std::fabs(pos0 - e0) > kHalfFrame) return false;
    if ((wNew > kUnheard || wOld > kUnheard) && std::fabs(p1 - e1) > kHalfFrame) return false;
    return !(std::fabs(e0 - e1) > kHalfFrame && std::fabs(wNew - wOld) > 0.01);
}

} // namespace

// --- the patterns ---------------------------------------------------------------------------------

int numTimePatterns() { return kNumTimePatterns; }
const char* timePatternName(int i) { return defOf(i).name; }
const TimePatternDef& timePatternDef(int i) { return defOf(i); }

void TimeCurve::bind(const TPoint* points, int count) {
    p_ = points;
    n_ = count;
    i_ = 0;
    jq_ = -1;
    crossed_ = false;
    land();
}

void TimeCurve::land() {
    const TPoint& a = p_[i_];
    const TPoint& b = p_[i_ + 1];
    x0_ = a.x;
    x1_ = b.x;
    y0_ = a.y;
    y1_ = b.y;
    rise_ = x1_ > x0_ ? (y1_ - y0_) / (x1_ - x0_) : 0.0;
    line_ = a.seg == TSeg::Line ? rise_ : 0.0;
}

void TimeCurve::seek(double x) {
    crossed_ = false;
    if (x >= x0_ && (x < x1_ || i_ + 2 >= n_)) return;   // the cursor is where x is (or on the last segment, which has nowhere to go)
    if (!(x >= x0_)) {   // backwards (a locate, a lap), or not a number: from the start
        i_ = 0;
        jq_ = -1;
    }
    while (i_ + 2 < n_ && x >= static_cast<double>(p_[i_ + 1].x)) {
        if (p_[i_].seg == TSeg::Jump) {   // over the line: from the Jump's point to where it lands
            crossed_ = true;
            crossGap_ = gapOf(i_);
        }
        ++i_;
    }
    land();
}

double TimeCurve::gapOf(int jq) const {
    int k = jq - 1;
    while (k > 0 && p_[k].seg != TSeg::Jump) --k;
    return static_cast<double>(p_[jq].x) - (k >= 0 && p_[k].seg == TSeg::Jump ? static_cast<double>(p_[k].x) : static_cast<double>(p_[0].x));
}

double TimeCurve::value(double x) const {
    if (x >= x1_) return y1_;      // the end
    if (!(x > x0_)) return y0_;    // the start, or before it
    if (p_[i_].seg == TSeg::Smooth) {
        const double u = (x - x0_) / (x1_ - x0_);
        return y0_ + (y1_ - y0_) * (0.5 * (1.0 - std::cos(kPi * u)));
    }
    return y0_ + rise_ * (x - x0_);
}

double TimeCurve::slope(double x) const {
    if (x >= x1_ || !(x1_ > x0_)) return 0.0;   // past the end
    if (p_[i_].seg == TSeg::Smooth) {
        const double w = x1_ - x0_, dy = y1_ - y0_;
        return dy * 0.5 * kPi * std::sin(kPi * std::clamp((x - x0_) / w, 0.0, 1.0)) / w;
    }
    return line_;
}

double TimeCurve::slopeOf(int k) const {
    if (k + 1 >= n_) return 1.0;   // past the last point: the live signal's
    const TPoint& a = p_[k];
    const TPoint& b = p_[k + 1];
    if (a.seg != TSeg::Line || !(b.x > a.x)) return 0.0;   // a Smooth segment starts flat
    return (static_cast<double>(b.y) - a.y) / (static_cast<double>(b.x) - a.x);
}

double TimeCurve::nextJump(double& landsAt, double& slopeAfter) {
    if (jq_ <= i_) {   // none looked for since the cursor moved past the last one
        jq_ = i_ + 1;
        while (jq_ < n_ - 1 && p_[jq_].seg != TSeg::Jump) ++jq_;
        if (jq_ >= n_ - 1) {
            jq_ = n_;
        } else {
            gap_ = gapOf(jq_);
            jumpX_ = p_[jq_].x;
            lands_ = p_[jq_ + 1].y;
            slopeAfter_ = slopeOf(jq_ + 1);
        }
    }
    if (jq_ >= n_) return HUGE_VAL;
    landsAt = lands_;
    slopeAfter = slopeAfter_;
    return jumpX_;
}

double timeSourceAt(int pattern, double x) {
    TimeCurve c;
    bindCurve(c, pattern, false);
    c.seek(x);
    return c.value(x);
}

double timeSpeedAt(int pattern, double x) {
    TimeCurve c;
    bindCurve(c, pattern, false);
    c.seek(x);
    return c.slope(x);
}

float timeVolumeAt(int pattern, double x) {
    TimeCurve c;
    bindCurve(c, pattern, true);
    c.seek(x);
    return static_cast<float>(std::clamp(c.value(x), 0.0, 1.0));
}

double timeNextJump(int pattern, double x0, double x1) {
    TimeCurve c;
    bindCurve(c, pattern, false);
    c.seek(x0);
    double landsAt, slopeAfter;
    const double j = c.nextJump(landsAt, slopeAfter);
    return j <= x1 ? j : HUGE_VAL;
}

float timeFadeWeight(double remaining) {
    const double p = (remaining > 0.0 ? (remaining < 1.0 ? remaining : 1.0) : 0.0) * kWeightSteps;   // (a NaN: 0)
    const int k = std::min(static_cast<int>(p), kWeightSteps);
    const float f = static_cast<float>(p - k);
    return kRaisedCosine.v[k] + (kRaisedCosine.v[k + 1] - kRaisedCosine.v[k]) * f;
}

// --- the playhead ---------------------------------------------------------------------------------

TimeCore::~TimeCore() { delete store_.load(std::memory_order_acquire); }

bool TimeCore::allocate() {
    if (store_.load(std::memory_order_acquire)) return true;
    try {
        // Owned until handed over: two callers at once keep one.
        auto s = std::make_unique<Store>();
        s->ring.assign(2 * static_cast<size_t>(kRingFrames), 0.0f);
        Store* none = nullptr;
        if (store_.compare_exchange_strong(none, s.get(), std::memory_order_acq_rel)) s.release();
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

void TimeCore::reset() {
    written_ = 0;
    oldest_ = abs_;   // nothing is cleared: what was recorded is not read
    clock_ = 0.0;
    pat_ = -1;
    fresh_ = true;
    prevEff_ = -1;
    calm_ = true;
    fade_ = 0;
    held_ = false;
    hl_ = 0.0;
    prevLeft_ = 0.0;
    g_ = 1.0f;
}

void TimeCore::useLibrary(const TimePatternDef* defs, int count, const TPoint* points) {
    defs_ = defs && points && count > 0 ? defs : nullptr;
    points_ = defs_ ? points : nullptr;
    numDefs_ = defs_ ? count : 0;
    pat_ = -1;   // whatever was bound is another pattern now
}

void TimeCore::bind(int pattern) {
    pat_ = pattern;
    run_.lo = run_.hi = 0.0;
    const TimePatternDef& d = defs_ ? defs_[pattern] : kTimePatterns[pattern];
    const TPoint* points = points_ ? points_ : kTimePoints;
    len_ = d.lengthBeats;
    loop_ = d.loopable;
    src_.bind(points + d.srcFirst, d.srcCount);
    vol_.bind(points + d.volFirst, d.volCount);
    double last = 0.0;   // the line before the end: the last Jump short of it
    for (int k = 0; k < d.srcCount; ++k) {
        const TPoint& pt = points[d.srcFirst + k];
        if (pt.seg == TSeg::Jump && static_cast<double>(pt.x) < len_ - 1e-9) last = pt.x;
    }
    endGap_ = len_ - last;
    src_.seek(0.0);
    s0_ = src_.value(0.0);
    d0_ = src_.slope(0.0);
}

float TimeCore::volumeAt(double x) {
    vol_.seek(x);
    return static_cast<float>(std::clamp(vol_.value(x), 0.0, 1.0));
}

// Where the heads read for the sample at x (beats from the pattern's start) and what the volume asks.
// Outside the pattern the lag asked for plays. Inside, head 0 follows the source curve; within 5 ms of
// a line (a Jump, the pattern's end, a lap) head 1 fades in on what comes after it, read back from the line.
//
// Most samples are one more of a run in the same segment of the curves, nowhere near a line unless it is the fade's; the
// first one of a run looks everything up and leaves the formulas (run_), and the others, until the segment, the volume's
// piece or the lap ends, or the fade begins, only evaluate them: the same arithmetic as the lookup's, so a sample has the
// same answer whichever way it was found.
EF_INLINE void TimeCore::headsAt(bool inPattern, double x, double lag, Heads& h) {
    if (!inPattern || !(x >= run_.lo && x < run_.hi)) {
        headsLookup(inPattern, x, lag, h);
        return;
    }
    // A run an earlier sample found: the formulas, with Heads' defaults set one by one (this runs every sample).
    const Stretch& r = run_;
    const double xl = x - r.off;
    h.lag0 = h.lag1 = std::clamp(xl - (r.y0 + r.rise * (xl - r.x0)), 0.0, TimeCore::kMaxLagBeats);
    h.w = 0.0f;
    h.gain = r.gain;
    h.speed = h.speed0 = r.speed;
    h.left = 0.0;
    h.vol = r.vrise == 0.0 ? r.vol : static_cast<float>(std::clamp(r.vy0 + r.vrise * (xl + leadB_ - r.vx0), 0.0, 1.0));
    h.cut = false;
    if (r.fade != 0) {
        const double remaining = (r.line - xl) * invTfb_;
        const double lag1 = std::clamp(r.fade == 1   ? (r.line - r.lands) + (1.0 - r.slopeAfter) * (xl - r.line)
                                       : r.fade == 2 ? (1.0 - d0_) * (xl - len_) - s0_
                                                     : lag,
                                       0.0, TimeCore::kMaxLagBeats);
        const float w = timeFadeWeight(remaining);
        h.lag1 = lag1;
        h.left = remaining * TimeCore::kFade;
        h.w = lag1 != h.lag0 ? w : 0.0f;
        h.gain += (r.gain1 - h.gain) * w;
        h.speed = r.speed1;
    }
}

void TimeCore::headsLookup(bool inPattern, double x, double lag, Heads& h) {
    h.lag0 = h.lag1 = lag;
    h.w = 0.0f;
    h.gain = h.speed = h.speed0 = h.vol = 1.0f;
    h.left = 0.0;
    h.cut = false;
    if (!inPattern) return;

    // Head 1 on `lag1` beats behind now, going at `speed1`, `remaining` (0..1) of the crossfade still to go.
    // Never ahead of now: a source run back from its line may be, and then it waits at now.
    const auto fadeIn = [&h](double lag1, double speed1, double remaining) {
        lag1 = std::clamp(lag1, 0.0, TimeCore::kMaxLagBeats);
        const float w = timeFadeWeight(remaining);
        h.lag1 = lag1;
        h.left = remaining * TimeCore::kFade;
        h.w = lag1 != h.lag0 ? w : 0.0f;
        h.gain += (speedGain(speed1) - h.gain) * w;
        h.speed = static_cast<float>(speed1);
    };

    if (x < 0.0) {   // before the start: the start is the line ahead
        h.gain = 1.0f;
        if (x >= -tfb_) fadeIn((1.0 - d0_) * x - s0_, d0_, -x * invTfb_);
        if (x + leadB_ >= 0.0) h.vol = volumeAt(x + leadB_);
        return;
    }
    if (!loop_ && x >= len_) {   // over: back at the lag asked for, at volume 1
        h.cut = endGap_ < tfb_ && x - bps_ < len_;   // (an end with no 5 ms of its own is made here, as a jump nothing foresaw)
        return;
    }

    double xl = x;   // in this lap
    double off = 0.0;
    bool plain = true;   // the lap's start is a multiple of its length, and xl the distance from it: a run can say so
    if (loop_) {
        off = ef::floorFast(x / len_) * len_;
        xl = x - off;
        if (xl < 0.0) {
            xl += len_;
            plain = false;
        }
        if (xl >= len_) {
            xl -= len_;
            plain = false;
        }
        if (!(xl >= 0.0 && xl < len_)) {
            xl = 0.0;
            plain = false;
        }
    }
    src_.seek(xl);
    if (src_.crossed() && src_.crossGap() < tfb_) h.cut = true;   // a Jump too close behind the one before it to have been scheduled
    if (loop_ && x >= len_ && xl < bps_ && endGap_ < tfb_) h.cut = true;   // and a wrap too close behind the last Jump
    const double speed = src_.slope(xl);
    h.lag0 = std::clamp(xl - src_.value(xl), 0.0, TimeCore::kMaxLagBeats);
    h.lag1 = h.lag0;
    h.speed = h.speed0 = static_cast<float>(speed);
    h.gain = speedGain(speed);

    double landsAt = 0.0, slopeAfter = 1.0;
    double line = src_.nextJump(landsAt, slopeAfter);
    double room = src_.nextGap();      // the stretch since the line before this one: the fade may have it, if it is 5 ms or more
    const bool end = !(line < len_);   // the pattern's end (a last Jump at len_ is the same line)
    if (end) {
        line = len_;
        room = endGap_;
    }
    int fade = 0;
    double speed1 = 1.0;
    if (line - xl <= tfb_ && room >= tfb_) {
        const double remaining = (line - xl) * invTfb_;
        if (!end) {   // the segment after the Jump, run back
            fade = 1;
            speed1 = slopeAfter;
            fadeIn((line - landsAt) + (1.0 - slopeAfter) * (xl - line), speed1, remaining);
        } else if (loop_) {   // the next lap's start, run back
            fade = 2;
            speed1 = d0_;
            fadeIn((1.0 - d0_) * (xl - len_) - s0_, speed1, remaining);
        } else {   // the lag asked for
            fade = 3;
            fadeIn(lag, speed1, remaining);
        }
    }

    const double xv = xl + leadB_;   // the volume, kEdge samples ahead: its edges end on their lines
    bool volRun = false;
    if (xv < len_) {
        h.vol = volumeAt(xv);
        volRun = true;
    } else if (loop_) {
        h.vol = volumeAt(xv - len_);
    }

    // The run this sample starts, if it can be said in formulas: a straight piece of the source and of the volume, and
    // nothing special at this sample (no jump too close to be scheduled, no lap rounding), up to where the segment, the
    // piece, the lap or the state of the fade changes (a hair short of it: the edge itself is looked up).
    run_.lo = run_.hi = 0.0;
    if (runs_ && plain && volRun && !h.cut && src_.isLine() && vol_.isLine()) {
        double hiXl = std::min({src_.segEnd(), vol_.segEnd() - leadB_, len_ - leadB_, fade != 0 ? line : line - tfb_});
        hiXl -= 1e-9;
        if (hiXl > xl) {
            run_.lo = x;
            run_.hi = off + hiXl;
            run_.off = off;
            run_.x0 = src_.segStart();
            run_.y0 = src_.segY0();
            run_.rise = src_.segRise();
            run_.speed = h.speed0;
            run_.gain = speedGain(speed);
            run_.vol = h.vol;
            run_.vx0 = vol_.segStart();
            run_.vy0 = vol_.segY0();
            run_.vrise = vol_.segRise();
            run_.fade = fade;
            run_.line = line;
            run_.lands = landsAt;
            run_.slopeAfter = slopeAfter;
            run_.speed1 = static_cast<float>(speed1);
            run_.gain1 = speedGain(speed1);
        }
    }
}

const float* TimeCore::tap(int64_t a, int64_t last) const {
    if (a > last) a = last;   // never a sample after the one being played
    if (a < oldest_) return kSilence;
    return ring_ + 2 * static_cast<size_t>(a & kMask);
}

EF_INLINE void TimeCore::read(double back, double nowD, double availD, uint32_t now32, float& l, float& r) const {
    if (!(back >= 0.0 && back <= availD)) {
        readEdge(back, nowD, l, r);
        return;
    }
    // The usual: the four frames are recorded, and where one is after the sample being played (a head at now, or a frame
    // behind it) that frame is now's. The frame at or before the position is `ceil(back)` behind now; the ring is indexed in 32
    // bits (its length divides 2^32), because a 64-bit conversion is a library call on the device.
    constexpr uint32_t kM = static_cast<uint32_t>(kMask);
    const uint32_t whole = static_cast<uint32_t>(back);   // (truncates)
    const double down = static_cast<double>(whole);
    const bool frac = down < back;
    const uint32_t b = whole + (frac ? 1u : 0u);
    const float t = frac ? static_cast<float>(down + 1.0 - back) : 0.0f;
    const uint32_t k = (now32 - b) & kM;
    const float *p0, *p1, *p2, *p3;
    if (b >= 2 && k >= 1 && k + 2 <= kM) {   // four frames in a row
        p0 = ring_ + 2 * static_cast<size_t>(k - 1);
        p1 = p0 + 2;
        p2 = p0 + 4;
        p3 = p0 + 6;
    } else {
        p0 = ring_ + 2 * static_cast<size_t>((k - 1) & kM);
        p1 = ring_ + 2 * static_cast<size_t>(k);
        p2 = ring_ + 2 * static_cast<size_t>((k + (b < 1 ? b : 1)) & kM);
        p3 = ring_ + 2 * static_cast<size_t>((k + (b < 2 ? b : 2)) & kM);
    }
    const ef::f2 a = pair(p0), bb = pair(p1), c = pair(p2), d = pair(p3);
    // ef::hermite's arithmetic, left and right in one vector.
    const ef::f2 c1 = 0.5f * (c - a);
    const ef::f2 c2 = a - 2.5f * bb + 2.0f * c - 0.5f * d;
    const ef::f2 c3 = 0.5f * (d - a) + 1.5f * (bb - c);
    const ef::f2 o = ((c3 * t + c2) * t + c1) * t + bb;
    l = o[0];
    r = o[1];
}

void TimeCore::readEdge(double back, double nowD, float& l, float& r) const {
    const double pos = nowD - back;
    const double fl = ef::floorFast(pos);
    const float t = static_cast<float>(pos - fl);
    const int64_t last = static_cast<int64_t>(nowD);
    const int64_t k = static_cast<int64_t>(fl);
    const float* f0 = tap(k - 1, last);
    const float* f1 = tap(k, last);
    const float* f2 = tap(k + 1, last);
    const float* f3 = tap(k + 2, last);
    l = ef::hermite(f0[0], f1[0], f2[0], f3[0], t);
    r = ef::hermite(f0[1], f1[1], f2[1], f3[1], t);
}

void TimeCore::process(float* L, float* R, int n, const ef::Transport& t, int pattern, double patternStart, double lagBeats) {
    if (n <= 0) return;
    const double bpm = ef::tempoOf(t);
    const double bps = bpm / 60.0 / ef::kRate;     // beats a sample
    const double spb = ef::kRate * 60.0 / bpm;     // samples a beat
    // Where the chunk starts: MPC's song position while it plays, else where the last chunk ended.
    const double beat0 = t.playing && t.valid && std::isfinite(t.beats) ? t.beats : clock_;
    clock_ = beat0 + n * bps;

    Store* st = store_.load(std::memory_order_acquire);
    if (!st) return;   // nothing allocated: the input passes

    // Record, before anything is read: a head at now reads this chunk's own samples.
    float* ring = st->ring.data();
    const int64_t a0 = abs_;
    for (int i = 0; i < n; ++i) {
        float* f = ring + 2 * static_cast<size_t>((a0 + i) & kMask);
        f[0] = ef::sanitize(L[i]);
        f[1] = ef::sanitize(R[i]);
    }
    abs_ += n;
    written_ = std::min<int64_t>(written_ + n, kRingFrames);
    oldest_ = abs_ - written_;
    ring_ = ring;

    // What to play: the pattern, if it is asked for and its place is a place; the lag asked for.
    int pat = pattern >= 0 && pattern < (defs_ ? numDefs_ : kNumTimePatterns) && std::isfinite(patternStart) ? pattern : -1;
    const double lam = std::isfinite(lagBeats) ? std::clamp(lagBeats, 0.0, kMaxLagBeats) : 0.0;
    double x0 = 0.0;
    if (pat >= 0) {
        x0 = beat0 - patternStart;
        if (!(std::fabs(x0) < kFar)) pat = -1;
    }
    if (pat >= 0 && pat != pat_) bind(pat);
    if (bps != bps_) run_.lo = run_.hi = 0.0;   // a run's fade window is in beats at the tempo it was found at
    tfb_ = TimeCore::kFade * bps;
    invTfb_ = 1.0 / tfb_;
    leadB_ = TimeCore::kEdge * bps;
    bps_ = bps;
    const double x1 = x0 + n * bps;
    const bool inRange = pat >= 0 && x1 >= -tfb_ && (loop_ || x0 < len_);   // within 5 ms of the start up to the end

    if (!inRange && lam == 0.0 && calm_) {   // at rest: the input untouched
        fresh_ = false;
        prevEff_ = -1;
        prevLam_ = 0.0;
        prevSpb_ = spb;
        prevPos_ = static_cast<double>(abs_ - 1);
        prevSpeed_ = 1.0f;
        prevLeft_ = 0.0;
        return;
    }

    // A chunk that follows on from the last (the same thing asked for, the song position where the last ended)
    // goes where the curves say. Any other may land the playhead elsewhere than it was heading.
    const bool cont = !fresh_ && inRange == (prevEff_ >= 0) && lam == prevLam_ && spb == prevSpb_ &&
                      (!inRange || (pat == prevEff_ && patternStart == prevStart_ && std::fabs(x0 - prevX_) <= 0.5 * bps));
    bool checked = cont || fresh_;
    bool have = !fresh_;   // there is a last sample to go on from
    fresh_ = false;

    Heads h;
    double carry = prevPos_;    // where the head that goes on read at the sample before: the one that is heard past any line
    float speed = prevSpeed_;   // and how fast it was going
    double carry0 = prev0Pos_;  // where head 0 read, how fast, and how far from the line the pattern's own fade was
    float speed0 = prev0Speed_;
    double left = prevLeft_;
    const double a0d = static_cast<double>(a0);   // (the sample numbers go through doubles: an int64 to a double is a library call on the device)
    const uint32_t a032 = static_cast<uint32_t>(a0);
    const double avail0 = a0d - static_cast<double>(oldest_) - 1.0;   // how far back the Hermite may read from the chunk's first sample
    for (int i = 0; i < n; ++i) {
        const double now = a0d + i;
        const uint32_t now32 = a032 + static_cast<uint32_t>(i);
        const double avail = avail0 + i;
        headsAt(inRange, x0 + i * bps, lam, h);
        const double back0 = h.lag0 * spb;
        const double pos0 = now - back0;
        const bool two = h.w > 0.0f;
        const double back1 = h.lag1 * spb;
        const double pos1 = now - back1;
        double here = two ? pos1 : pos0;   // the head that goes on past the line
        const double wNew = two ? static_cast<double>(h.w) : 0.0;
        if (held_ && fade_ == 0 && hl_ <= 0.0) {
            // The fades a jump waited for are over: the jump is made now, from where the head was held.
            held_ = false;
            hl_ = 0.0;
            if (!carriesOn(pos0, here, wNew, hp_, hp_, 0.0)) {
                u_ = hp_;
                uSpeed_ = hs_;
                fade_ = TimeCore::kFade;
            }
        } else if (!held_ && have && (!checked || h.cut)) {
            // Where the heads would be but for the jump: the head that goes on (e1), and head 0 if the pattern's own fade the sample before
            // (toGo samples from its line, now) was not over (e0, and that fade's weight on e1).
            const double toGo = left - 1.0;
            const bool fadingOld = toGo > 0.0;
            const double e1 = carry + static_cast<double>(speed);
            const double e0 = fadingOld ? carry0 + static_cast<double>(speed0) : e1;
            const double wOld = fadingOld ? static_cast<double>(timeFadeWeight(toGo / TimeCore::kFade)) : 0.0;
            if (!carriesOn(pos0, here, wNew, e0, e1, wOld)) {   // it jumped: the old head(s) play on and crossfade
                if (fade_ > 0 || fadingOld) {   // a fade is going: the jump waits for it, the head(s) going on where they were heading
                    held_ = true;
                    hp_ = e1;
                    hs_ = speed;
                    hl_ = fadingOld ? toGo : 0.0;
                    hq_ = e0;
                    hqs_ = speed0;
                } else {
                    u_ = e1;
                    uSpeed_ = speed;
                    fade_ = TimeCore::kFade;
                }
            }
        }
        checked = true;
        float l, r;
        if (held_) {
            read(now - hp_, now, avail, now32, l, r);
            if (hl_ > 0.0) {   // head 1 still coming in over head 0, on the pattern's own weights
                float l0, r0;
                read(now - hq_, now, avail, now32, l0, r0);
                const float w = timeFadeWeight(hl_ / TimeCore::kFade);
                l = l0 * (1.0f - w) + l * w;
                r = r0 * (1.0f - w) + r * w;
                hq_ += static_cast<double>(hqs_);
                hl_ -= 1.0;
            }
            here = hp_;
            hp_ += static_cast<double>(hs_);
        } else {
            read(back0, now, avail, now32, l, r);
            if (two) {
                float l1, r1;
                read(back1, now, avail, now32, l1, r1);
                l = l * (1.0f - h.w) + l1 * h.w;
                r = r * (1.0f - h.w) + r1 * h.w;
            }
        }
        if (fade_ > 0) {
            float lu, ru;
            read(now - u_, now, avail, now32, lu, ru);
            const float wo = kFadeWeights.w[fade_];
            l = l * (1.0f - wo) + lu * wo;
            r = r * (1.0f - wo) + ru * wo;
            u_ += static_cast<double>(uSpeed_);
            --fade_;
        }
        g_ = toward(g_, h.vol * h.gain, kGainStep);
        L[i] = l * g_;
        R[i] = r * g_;
        carry = here;
        speed = held_ ? hs_ : h.speed;
        carry0 = pos0;
        speed0 = h.speed0;
        left = two ? h.left : 0.0;
        have = true;
    }

    prevEff_ = inRange ? pat : -1;
    prevStart_ = patternStart;
    prevLam_ = lam;
    prevSpb_ = spb;
    prevX_ = x1;
    prevPos_ = carry;
    prevSpeed_ = speed;
    prev0Pos_ = carry0;
    prev0Speed_ = speed0;
    prevLeft_ = left;
    calm_ = h.lag0 == 0.0 && h.w == 0.0f && g_ == 1.0f && fade_ == 0 && !held_;
}


// --- Time: the grooves ----------------------------------------------------------------------------

namespace {

constexpr double kSlack = 1e-3;                  // samples: a line this close after a sample counts as reached by it
constexpr double kSixteenth = 0.25;              // Drunk's unit, in beats
constexpr double kMsSamples = ef::kRate / 1000.0;
constexpr double kUnitTolerance = 1e-6;          // in units: a pattern's end this close to a line is on it

// Drunk's lag for the 16th numbered k in its bar, 0..1 of the longest: a hash of the index, so it is the same on every
// bar, every loop and every load (a seeded random, not a random).
float drunkShare(int k) {
    uint32_t x = static_cast<uint32_t>(k) * 0x9E3779B1u + 0x7F4A7C15u;
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return static_cast<float>(x >> 8) * (1.0f / 16777216.0f);
}

} // namespace

Time::Time() = default;

void Time::reset() {
    core_.reset();
    fresh_ = true;
    lastLag_ = 0.0;
    armed_ = false;
    zoneOn_ = false;
    latchUnit_ = INT64_MIN;
    tail_ = 0;
}

double Time::grooveLagSamples(const Params& p, const ef::Transport& t) {
    switch (p.groove) {
    case kSwing: {
        const double split = swingSplit(p.swing);
        return split > 0.0 ? (split - p.swing.unitBeats) / (ef::tempoOf(t) / 60.0 / ef::kRate) : 0.0;
    }
    case kDrunk: return static_cast<double>(ef::clampParam(p.amount, 0.0f, 1.0f, 0.5f) * kDrunkMs) * kMsSamples;
    case kPush: return static_cast<double>(ef::clampParam(p.amount, 0.0f, 1.0f, 0.5f) * kPushMs) * kMsSamples;
    default: return 0.0;
    }
}

// The groove's lag, in beats, for the sample whose position 5 ms on is q (so a change is asked for 5 ms before its
// line and ends on it), and the position where it next changes (HUGE_VAL: never).
double Time::grooveLag(double q, double& until) {
    until = HUGE_VAL;
    switch (p_.groove) {
    case kSwing: {
        const double split = swingSplit(p_.swing);
        if (split <= 0.0) return 0.0;   // straight
        const double u = p_.swing.unitBeats;
        const double unitNo = ef::floorFast(q / u);
        if (!(std::fabs(unitNo) < kFar)) return 0.0;
        until = (unitNo + 1.0) * u;
        // the second unit of a pair (an odd unit) is the late one: the lag is how much later it starts than its straight line,
        // fixed in samples when the unit first comes to be asked for (as Drunk's and Push's are), whatever the tempo does after
        const int64_t unit = static_cast<int64_t>(unitNo);
        if (!(static_cast<uint64_t>(unit) & 1u)) return 0.0;
        if (unit != latchUnit_ || split - u != latchNominal_) {
            latchUnit_ = unit;
            latchNominal_ = split - u;
            latchSamples_ = latchNominal_ / bps_;
        }
        return latchSamples_ * bps_;
    }
    case kDrunk: {
        if (!(p_.amount > 0.0f)) return 0.0;
        const double sixteenth = ef::floorFast(q / kSixteenth);
        if (!(std::fabs(sixteenth) < kFar)) return 0.0;
        until = (sixteenth + 1.0) * kSixteenth;
        const int k = static_cast<int>(static_cast<uint64_t>(static_cast<int64_t>(sixteenth)) & 15u);   // its place in the bar
        return static_cast<double>(drunkShare(k) * p_.amount * kDrunkMs) * kMsSamples * bps_;
    }
    case kPush:
        return static_cast<double>(p_.amount * kPushMs) * kMsSamples * bps_;
    default:
        return 0.0;
    }
}

// What is asked of the playhead when no pattern holds it: the groove, except while it waits for its next line after a
// pattern (lag 0 on [zFrom_, zTo_)).
double Time::baseLag(double q, double& until) {
    if (zoneOn_ && q >= zFrom_ && q < zTo_) {
        until = zTo_;
        return 0.0;
    }
    const double g = grooveLag(q, until);
    if (zoneOn_ && q < zFrom_) until = std::min(until, zFrom_);
    return g;
}

// Where the groove resumes after a pattern that ended at `e`: the first unit line at or after it (strictly after, for a
// pattern whose end is a crossfade to the live input, not to the lag). A groove with no lines resumes at once.
double Time::resumeLine(double e, bool strict) const {
    double u = 0.0;
    if (p_.groove == kSwing) {
        if (swingSplit(p_.swing) > 0.0) u = p_.swing.unitBeats;
    } else if (p_.groove == kDrunk) {
        if (p_.amount > 0.0f) u = kSixteenth;
    }
    if (!(u > 0.0)) return e;
    const double r = e / u;
    if (!(std::fabs(r) < kFar)) return e;
    return (strict ? ef::floorFast(r + kUnitTolerance) + 1.0 : std::ceil(r - kUnitTolerance)) * u;
}

void Time::setZone(double e, bool strict) {
    const double r = resumeLine(e, strict);
    zoneOn_ = r > e;
    zFrom_ = e;
    zTo_ = r;
}

void Time::set(const Params& in, const ef::Transport& t) {
    p_ = in;
    p_.groove = in.groove >= kOff && in.groove <= kPush ? in.groove : kOff;
    p_.amount = ef::clampParam(in.amount, 0.0f, 1.0f, 0.5f);
    p_.pattern = in.pattern >= 0 && in.pattern < numTimePatterns() && std::isfinite(in.patternStart) ? in.pattern : -1;

    bpm_ = ef::tempoOf(t);
    bps_ = bpm_ / 60.0 / ef::kRate;
    tfb_ = TimeCore::kFade * bps_;
    // Playing, the song position is MPC's (it moves for a locate, a loop, a tempo change). Otherwise it runs on at the
    // tempo from where it was (process() moves it): from MPC's, the first chunk after reset(), if it says where.
    if (t.valid && std::isfinite(t.beats) && (t.playing || fresh_)) pos_ = t.beats;
    fresh_ = false;
    qBase_ = pos_ + tfb_ + kSlack * bps_;

    // Where a pattern ends, the groove waits for its next unit line. A pattern that is asked for says where it ends; one
    // that is no longer asked for ended where it said it would, or where the asking stopped, if that came first.
    double reach = 0.0;   // beats a pattern can still read back
    if (p_.pattern >= 0) {
        const TimePatternDef& d = timePatternDef(p_.pattern);
        const bool wasLooping = armed_ && armedEnd_ == HUGE_VAL;
        double end = HUGE_VAL;
        if (!p_.loop) {
            end = p_.patternStart + d.lengthBeats;
            if (end + tfb_ > pos_) setZone(end, d.loopable);
            else if (wasLooping) setZone(pos_, false);   // it was going round and is told to stop: it stops where it is
        }
        armed_ = true;
        armedEnd_ = end;
        armedStrict_ = d.loopable;
        if (p_.loop || end + tfb_ > pos_) reach = kReachBeats;
    } else if (armed_) {
        armed_ = false;
        const double e = std::min(armedEnd_, pos_);
        setZone(e, armedStrict_ && armedEnd_ <= pos_);
    }

    // A wait for a line that the position has gone by (an MPC loop took it back, or on past) is over, if no pattern is asked for.
    if (p_.pattern < 0 && zoneOn_ && (pos_ >= zTo_ || pos_ < zFrom_ - tfb_)) zoneOn_ = false;

    // The tail: the groove's longest lag, or a pattern's reach back, and a crossfade.
    const double samples = std::max(grooveLagSamples(p_, t), reach / bps_);
    tail_ = samples > 0.0 && core_.allocated()
                ? static_cast<int>(std::min(std::ceil(samples) + TimeCore::kFade, static_cast<double>(TimeCore::kRingFrames)))
                : 0;
}

// The next piece of the chunk, from sample i of n: what to ask of the core, and for how many samples. A piece ends where
// the ask changes: a unit line of the groove (5 ms early: q looks 5 ms on), the start or the end of a pattern or of one of
// its laps, the end of the wait after a pattern.
Time::Piece Time::pieceAt(int i, int n) {
    const double q = qBase_ + i * bps_;
    Piece pc{0.0, -1, 0.0, n - i};
    double until = HUGE_VAL;
    bool hold = false;   // a pattern has the playhead: the groove's lines are not followed

    if (p_.pattern >= 0) {
        const double start = p_.patternStart;
        const TimePatternDef& d = timePatternDef(p_.pattern);
        const double len = d.lengthBeats;
        if (std::fabs(q - start) < kFar) {
            if (q < start) {
                until = start;   // not yet; TimeCore's pre-roll begins here
            } else if (p_.loop) {
                pc.pattern = p_.pattern;
                pc.start = start;
                hold = true;
                if (!d.loopable) {   // it does not go round by itself: a lap at a time, each lap's pre-roll replacing the last's end
                    pc.start = start + ef::floorFast((q - start) / len) * len;
                    until = pc.start + len;
                }
            } else if (q < start + len) {
                pc.pattern = p_.pattern;
                pc.start = start;
                hold = true;
                until = start + len;
            } else if (q < start + len + tfb_) {   // the end's crossfade, back to the lag the groove has from the line
                pc.pattern = p_.pattern;
                pc.start = start;
                until = start + len + tfb_;
            }
        }
    }

    if (hold) {
        pc.lag = lastLag_ * bps_;
    } else {
        double next = HUGE_VAL;
        pc.lag = baseLag(q, next);
        until = std::min(until, next);
    }

    // The first sample at or after `until`, at least the next one.
    const double x = (until - qBase_) / bps_;
    int j = n;
    if (x < n) j = x > i ? std::max(i + 1, static_cast<int>(std::ceil(x))) : i + 1;
    pc.m = std::min(j, n) - i;
    return pc;
}

void Time::process(float* L, float* R, int n) {
    if (n <= 0) return;
    ef::Transport t;   // the core is always told the position: this one's, which is MPC's while it plays
    t.bpm = bpm_;
    t.playing = t.valid = true;
    for (int i = 0; i < n;) {
        const Piece pc = pieceAt(i, n);
        t.beats = pos_ + i * bps_;
        core_.process(L + i, R + i, pc.m, t, pc.pattern, pc.start, pc.lag);
        lastLag_ = pc.lag / bps_;
        i += pc.m;
    }
    pos_ += n * bps_;
}

} // namespace gf
