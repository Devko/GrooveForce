// Time's engine: see time.h. The patterns themselves are surface/build/time_data.h.
#include "time.h"

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

const TimePatternDef& defOf(int i) { return kTimePatterns[std::clamp(i, 0, kNumTimePatterns - 1)]; }

void bindCurve(TimeCurve& c, int pattern, bool volume) {
    const TimePatternDef& d = defOf(pattern);
    c.bind(kTimePoints + (volume ? d.volFirst : d.srcFirst), volume ? d.volCount : d.srcCount);
}

// `v` moved toward `target` by at most `step`; it lands on the target exactly.
float toward(float v, float target, float step) { return v < target ? std::min(v + step, target) : std::max(v - step, target); }

// The level a head plays at for its speed: the looper's, a tape stop ends in silence.
float speedGain(double speed) { return std::min(std::fabs(static_cast<float>(speed)) * 8.0f, 1.0f); }

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
}

void TimeCurve::seek(double x) {
    if (!(x >= static_cast<double>(p_[i_].x))) {   // backwards (a locate, a lap), or not a number: from the start
        i_ = 0;
        jq_ = -1;
    }
    while (i_ + 2 < n_ && x >= static_cast<double>(p_[i_ + 1].x)) ++i_;
}

double TimeCurve::value(double x) const {
    const TPoint& a = p_[i_];
    const TPoint& b = p_[i_ + 1];
    if (x >= b.x) return b.y;      // the end
    if (!(x > a.x)) return a.y;    // the start, or before it
    const double u = (x - a.x) / (static_cast<double>(b.x) - a.x);
    const double dy = static_cast<double>(b.y) - a.y;
    return a.y + dy * (a.seg == TSeg::Smooth ? 0.5 * (1.0 - std::cos(kPi * u)) : u);
}

double TimeCurve::slope(double x) const {
    const TPoint& a = p_[i_];
    const TPoint& b = p_[i_ + 1];
    if (x >= b.x || !(b.x > a.x)) return 0.0;   // past the end
    const double w = static_cast<double>(b.x) - a.x, dy = static_cast<double>(b.y) - a.y;
    if (a.seg == TSeg::Smooth) return dy * 0.5 * kPi * std::sin(kPi * std::clamp((x - a.x) / w, 0.0, 1.0)) / w;
    return a.seg == TSeg::Line ? dy / w : 0.0;
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
        if (jq_ >= n_ - 1) jq_ = n_;
    }
    if (jq_ >= n_) return HUGE_VAL;
    landsAt = p_[jq_ + 1].y;
    slopeAfter = slopeOf(jq_ + 1);
    return p_[jq_].x;
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
    g_ = 1.0f;
}

void TimeCore::bind(int pattern) {
    pat_ = pattern;
    const TimePatternDef& d = kTimePatterns[pattern];
    len_ = d.lengthBeats;
    loop_ = d.loopable;
    src_.bind(kTimePoints + d.srcFirst, d.srcCount);
    vol_.bind(kTimePoints + d.volFirst, d.volCount);
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
void TimeCore::headsAt(bool inPattern, double x, double lag, Heads& h) {
    h = Heads{};
    h.lag0 = h.lag1 = lag;
    if (!inPattern) return;

    // Head 1 on `lag1` beats behind now, going at `speed1`, `remaining` (0..1) of the crossfade still to go.
    // Never ahead of now: a source run back from its line may be, and then it waits at now.
    const auto fadeIn = [&h](double lag1, double speed1, double remaining) {
        lag1 = std::clamp(lag1, 0.0, TimeCore::kMaxLagBeats);
        const float w = static_cast<float>(0.5 + 0.5 * std::cos(kPi * remaining));
        h.lag1 = lag1;
        h.w = lag1 != h.lag0 ? w : 0.0f;
        h.gain += (speedGain(speed1) - h.gain) * w;
        h.speed = static_cast<float>(speed1);
    };

    if (x < 0.0) {   // before the start: the start is the line ahead
        h.gain = 1.0f;
        if (x >= -tfb_) fadeIn((1.0 - d0_) * x - s0_, d0_, -x / tfb_);
        if (x + leadB_ >= 0.0) h.vol = volumeAt(x + leadB_);
        return;
    }
    if (!loop_ && x >= len_) return;   // over: back at the lag asked for, at volume 1

    double xl = x;   // in this lap
    if (loop_) {
        xl = x - ef::floorFast(x / len_) * len_;
        if (xl < 0.0) xl += len_;
        if (xl >= len_) xl -= len_;
        if (!(xl >= 0.0 && xl < len_)) xl = 0.0;
    }
    src_.seek(xl);
    const double speed = src_.slope(xl);
    h.lag0 = std::clamp(xl - src_.value(xl), 0.0, TimeCore::kMaxLagBeats);
    h.lag1 = h.lag0;
    h.speed = static_cast<float>(speed);
    h.gain = speedGain(speed);

    double landsAt = 0.0, slopeAfter = 1.0;
    double line = src_.nextJump(landsAt, slopeAfter);
    const bool end = !(line < len_);   // the pattern's end (a last Jump at len_ is the same line)
    if (end) line = len_;
    if (line - xl <= tfb_) {
        const double remaining = (line - xl) / tfb_;
        if (!end) fadeIn((line - landsAt) + (1.0 - slopeAfter) * (xl - line), slopeAfter, remaining);   // the segment after the Jump, run back
        else if (loop_) fadeIn((1.0 - d0_) * (xl - len_) - s0_, d0_, remaining);                        // the next lap's start, run back
        else fadeIn(lag, 1.0, remaining);                                                              // the lag asked for
    }

    const double xv = xl + leadB_;   // the volume, kEdge samples ahead: its edges end on their lines
    if (xv < len_) h.vol = volumeAt(xv);
    else if (loop_) h.vol = volumeAt(xv - len_);
}

const float* TimeCore::tap(int64_t a, int64_t last) const {
    if (a > last) a = last;   // never a sample after the one being played
    if (a < oldest_) return kSilence;
    return ring_ + 2 * static_cast<size_t>(a & kMask);
}

void TimeCore::read(double pos, int64_t last, float& l, float& r) const {
    const double fl = ef::floorFast(pos);
    const int64_t k = static_cast<int64_t>(fl);
    const float t = static_cast<float>(pos - fl);
    const float* f0 = tap(k - 1, last);
    const float* f1 = tap(k, last);
    const float* f2 = tap(k + 1, last);
    const float* f3 = tap(k + 2, last);
    l = ef::hermite(f0[0], f1[0], f2[0], f3[0], t);
    r = ef::hermite(f0[1], f1[1], f2[1], f3[1], t);
}

void TimeCore::process(float* L, float* R, int n, const ef::Transport& t, int pattern, double patternStart, double lagBeats) {
    if (n <= 0) return;
    const double bpm = t.bpm >= 20.0 ? std::min(t.bpm, 999.0) : (t.bpm < 20.0 ? 20.0 : 120.0);
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
    int pat = pattern >= 0 && pattern < kNumTimePatterns && std::isfinite(patternStart) ? pattern : -1;
    const double lam = std::isfinite(lagBeats) ? std::clamp(lagBeats, 0.0, kMaxLagBeats) : 0.0;
    double x0 = 0.0;
    if (pat >= 0) {
        x0 = beat0 - patternStart;
        if (!(std::fabs(x0) < kFar)) pat = -1;
    }
    if (pat >= 0 && pat != pat_) bind(pat);
    tfb_ = TimeCore::kFade * bps;
    leadB_ = TimeCore::kEdge * bps;
    const double x1 = x0 + n * bps;
    const bool inRange = pat >= 0 && x1 >= -tfb_ && (loop_ || x0 < len_);   // within 5 ms of the start up to the end

    if (!inRange && lam == 0.0 && calm_) {   // at rest: the input untouched
        fresh_ = false;
        prevEff_ = -1;
        prevLam_ = 0.0;
        prevSpb_ = spb;
        prevPos_ = static_cast<double>(abs_ - 1);
        prevSpeed_ = 1.0f;
        return;
    }

    // A chunk that follows on from the last (the same thing asked for, the song position where the last ended)
    // goes where the curves say. Any other may land the playhead elsewhere than it was heading.
    const bool cont = !fresh_ && inRange == (prevEff_ >= 0) && lam == prevLam_ && spb == prevSpb_ &&
                      (!inRange || (pat == prevEff_ && patternStart == prevStart_ && std::fabs(x0 - prevX_) <= 0.5 * bps));
    bool checked = cont || fresh_;
    fresh_ = false;

    Heads h;
    double carry = 0.0;
    for (int i = 0; i < n; ++i) {
        const int64_t now = a0 + i;
        headsAt(inRange, x0 + i * bps, lam, h);
        const double pos0 = static_cast<double>(now) - h.lag0 * spb;
        const bool two = h.w > 0.0f;
        const double pos1 = static_cast<double>(now) - h.lag1 * spb;
        carry = two ? pos1 : pos0;   // the head that goes on past the line
        if (!checked) {
            checked = true;
            const double expected = prevPos_ + static_cast<double>(prevSpeed_);
            if (std::fabs(carry - expected) > kHalfFrame) {   // it jumped: the old head plays on and crossfades
                u_ = expected;
                uSpeed_ = prevSpeed_;
                fade_ = TimeCore::kFade;
            }
        }
        float l, r;
        read(pos0, now, l, r);
        if (two) {
            float l1, r1;
            read(pos1, now, l1, r1);
            l = l * (1.0f - h.w) + l1 * h.w;
            r = r * (1.0f - h.w) + r1 * h.w;
        }
        if (fade_ > 0) {
            float lu, ru;
            read(u_, now, lu, ru);
            const float wo = kFadeWeights.w[fade_];
            l = l * (1.0f - wo) + lu * wo;
            r = r * (1.0f - wo) + ru * wo;
            u_ += static_cast<double>(uSpeed_);
            --fade_;
        }
        g_ = toward(g_, h.vol * h.gain, kGainStep);
        L[i] = l * g_;
        R[i] = r * g_;
    }

    prevEff_ = inRange ? pat : -1;
    prevStart_ = patternStart;
    prevLam_ = lam;
    prevSpb_ = spb;
    prevX_ = x1;
    prevPos_ = carry;
    prevSpeed_ = h.speed;
    calm_ = h.lag0 == 0.0 && h.w == 0.0f && g_ == 1.0f && fade_ == 0;
}

} // namespace gf
