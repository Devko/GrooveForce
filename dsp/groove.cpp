// Groove: see groove.h.
#include "groove.h"
#include "slew.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace gf {

namespace {

constexpr float kFadeStep = 1.0f / (0.010f * ef::kRate);   // a part on or off: 10 ms
constexpr float kSlope = 1.0f / (0.001f * ef::kRate);      // the gains' fastest move a sample: a full scale in 1 ms
constexpr float kChunkSlope = kSlope * ef::kChunk;
constexpr float kMaxDb = 24.0f;
constexpr int64_t kForever = 1 << 30;                      // the Echo's tail when it holds (frozen): the cap
constexpr int kSettled = 1 << 30;                          // a part's age when nothing waits on it any more

// 0 dB is exactly 1: the defaults pass the input through bit for bit. NaN is 0 dB.
float gainOf(float db) {
    db = ef::clampParam(db, -kMaxDb, kMaxDb, 0.0f);
    return db == 0.0f ? 1.0f : ef::dbToGain(db);
}

// How much input Time must have heard before its groove is asked for: the groove's longest lag, a crossfade and a
// chunk. Until then the ring holds nothing where the lag would read.
int warmSamples(const Time::Params& tp, const ef::Transport& t) {
    const double bpm = t.bpm >= 20.0 ? std::min(t.bpm, 999.0) : (t.bpm < 20.0 ? 20.0 : 120.0);   // as Time takes it
    double lag = 0.0;   // samples
    switch (tp.groove) {
    case Time::kSwing: {
        const double split = swingSplit(tp.swing);
        if (split > 0.0) lag = (split - tp.swing.unitBeats) * 60.0 / bpm * ef::kRate;
        break;
    }
    case Time::kDrunk: lag = ef::clampParam(tp.amount, 0.0f, 1.0f, 0.5f) * Time::kDrunkMs * 0.001 * ef::kRate; break;
    case Time::kPush: lag = ef::clampParam(tp.amount, 0.0f, 1.0f, 0.5f) * Time::kPushMs * 0.001 * ef::kRate; break;
    default: break;
    }
    return lag > 0.0 ? static_cast<int>(std::ceil(lag)) + TimeCore::kFade + ef::kChunk : 0;
}

// A target as the part should act on it: out of range is the nearest one.
int targetOf(int target) { return std::clamp(target, static_cast<int>(Groove::kAll), static_cast<int>(Groove::kEcho)); }

// Whether a part acts on the dry, and on the wet. The dry unless the target is the Echo; the wet unless the target
// is the Dry, and only while the wet sounds. Echo last has only the dry to move: the targets are ignored.
bool onDry(int target, bool echoLast) { return echoLast || target != Groove::kEcho; }
bool onWet(int target, bool echoLast, bool wetSounds) { return !echoLast && target != Groove::kDry && wetSounds; }

} // namespace

Groove::Groove() = default;

void Groove::reset() {
    shape_.reset();
    shapeWet_.reset();
    pump_.reset();
    pumpWet_.reset();
    echo_.reset();
    time_.reset();
    shapePart_ = shapeWetPart_ = pumpPart_ = pumpWetPart_ = echoPart_ = timePart_ = Part();
    fresh_ = true;
    echoLast_ = false;
    running_ = 0;
}

bool Groove::allocate(const Params& p) {
    bool ok = true;
    if (p.echoOn) ok = echo_.allocate() && ok;
    if (p.timeOn) ok = time_.allocate() && ok;
    return ok;
}

// What a part does in this chunk: where its fade is going, and whether it runs at all. A part wanted at another place
// keeps its old one until it has faded out. Back on after resting it starts clean.
template <class M>
bool Groove::arm(M& m, Part& s, bool want, int where) {
    if (fresh_ || s.fade == 0.0f) s.where = where;
    s.goal = want && where == s.where ? 1.0f : 0.0f;
    if (fresh_) {
        s.fade = s.goal;
        s.age = kSettled;
    }
    if (s.goal > 0.0f && s.fade == 0.0f) {   // coming on from rest
        if (s.dirty) {
            m.reset();
            s.dirty = false;
        }
        s.age = 0;
    }
    s.runs = s.audible();
    return s.runs;
}

void Groove::set(const Params& p, const ef::Transport& t) {
    const float in = gainOf(p.inputDb), out = gainOf(p.outputDb), mix = ef::clampParam(p.mix, 0.0f, 1.0f, 1.0f);
    const float dry = ef::clampParam(p.dry, 0.0f, 1.0f, 1.0f);
    if (fresh_) {
        in_.jump(in);
        out_.jump(out);
        mix_.jump(mix);
        dry_.jump(dry);
    } else {   // a full scale a millisecond at most, from where each is
        in_.to(toward(in_.value(), in, kChunkSlope), ef::kChunk);
        out_.to(toward(out_.value(), out, kChunkSlope), ef::kChunk);
        mix_.to(toward(mix_.value(), mix, kChunkSlope), ef::kChunk);
        dry_.to(toward(dry_.value(), dry, kChunkSlope), ef::kChunk);
    }

    echoLast_ = orderOf(p.order) == kEchoLast;
    const Swing swing = stepSwing(p);

    // The Echo and Time first: where the buses sound and where Time works depend on them. A part switched on before
    // its memory has arrived waits for it (silent, and it costs nothing).
    if (arm(echo_, echoPart_, p.echoOn && echo_.allocated(), 0)) {
        Echo::Params ep = p.echo;
        ep.swing = swing;
        echo_.set(ep, t);
    }
    if (arm(time_, timePart_, p.timeOn && time_.allocated(), placeOf(p.timePlace))) {
        Time::Params tp = p.time;
        tp.swing = p.swing;   // Time swings the audio by the global Swing itself
        if (timePart_.age < warmSamples(tp, t)) tp.groove = Time::kOff;   // warming up: it only records
        time_.set(tp, t);
    }

    // Shape and Pump, each on the buses its target names. The wet's instance begins together with the Echo, and with
    // the dry's whenever both begin at once; both are given the same parameters, the same grid and (the Pumps) the
    // same key.
    const bool wetSounds = echoPart_.audible();
    const int shapeTarget = targetOf(p.shapeTarget), pumpTarget = targetOf(p.pumpTarget);
    const bool sd = arm(shape_, shapePart_, p.shapeOn && onDry(shapeTarget, echoLast_), 0);
    const bool sw = arm(shapeWet_, shapeWetPart_, p.shapeOn && onWet(shapeTarget, echoLast_, wetSounds), 0);
    if (sd || sw) {
        Shape::Params sp = p.shape;
        sp.swing = swing;
        if (sd) shape_.set(sp, t);
        if (sw) shapeWet_.set(sp, t);
    }
    const bool pd = arm(pump_, pumpPart_, p.pumpOn && onDry(pumpTarget, echoLast_), 0);
    const bool pw = arm(pumpWet_, pumpWetPart_, p.pumpOn && onWet(pumpTarget, echoLast_, wetSounds), 0);
    if (pd || pw) {
        Pump::Params pp = p.pump;
        pp.swing = swing;
        if (pd) pump_.set(pp, t);
        if (pw) pumpWet_.set(pp, t);
    }
    fresh_ = false;
}

// The part works on L / R in place; while it fades, what it made is crossfaded with what it was given.
template <class F>
void Groove::runPart(Part& s, float* L, float* R, int n, F&& process) {
    s.dirty = true;
    const bool fading = s.fade != s.goal;
    if (fading) {
        std::memcpy(givenL_, L, sizeof(float) * static_cast<size_t>(n));
        std::memcpy(givenR_, R, sizeof(float) * static_cast<size_t>(n));
    }
    process(L, R, n);
    s.age = static_cast<int>(std::min<int64_t>(static_cast<int64_t>(s.age) + n, kSettled));
    if (!fading) return;
    float g = s.fade;
    const float step = s.goal > g ? kFadeStep : -kFadeStep;
    for (int i = 0; i < n; ++i) {
        g = std::clamp(g + step, 0.0f, 1.0f);
        L[i] = givenL_[i] + (L[i] - givenL_[i]) * g;
        R[i] = givenR_[i] + (R[i] - givenR_[i]) * g;
    }
    s.fade = g;
}

// The wet bus: the Echo hears L / R (the send's source) and the key, and what it makes is faded in or out as the
// Echo is switched.
void Groove::makeWet(const float* L, const float* R, int n) {
    echo_.process(L, R, keyL_, keyR_, wetL_, wetR_, n);
    Part& s = echoPart_;
    s.dirty = true;
    if (s.fade == s.goal) return;
    float g = s.fade;
    const float step = s.goal > g ? kFadeStep : -kFadeStep;
    for (int i = 0; i < n; ++i) {
        g = std::clamp(g + step, 0.0f, 1.0f);
        wetL_[i] *= g;
        wetR_[i] *= g;
    }
    s.fade = g;
}

void Groove::process(float* L, float* R, int n) {
    n = std::min(n, ef::kChunk);
    if (n <= 0) return;
    const size_t bytes = sizeof(float) * static_cast<size_t>(n);
    const bool mixing = mix_.moving() || mix_.value() != 1.0f;   // else the output is all there is
    if (mixing) {
        std::memcpy(inputL_, L, bytes);
        std::memcpy(inputR_, R, bytes);
    }
    if (in_.moving()) {
        for (int i = 0; i < n; ++i) {
            const float g = in_.next();
            L[i] *= g;
            R[i] *= g;
        }
    } else if (in_.value() != 1.0f) {
        const float g = in_.value();
        for (int i = 0; i < n; ++i) {
            L[i] *= g;
            R[i] *= g;
        }
    }

    // Time, Pre: it bends the input before anything hears it, the key included.
    auto timeProcess = [this](float* l, float* r, int k) { time_.process(l, r, k); };
    if (timePart_.runs && timePart_.where == kPre) runPart(timePart_, L, R, n, timeProcess);

    // The key: the chain's input, for the Pump's Input trigger (both buses) and the Echo's duck and modulators.
    const bool echoRuns = echoPart_.runs;
    if (echoRuns || pumpPart_.runs || pumpWetPart_.runs) {
        std::memcpy(keyL_, L, bytes);
        std::memcpy(keyR_, R, bytes);
    }

    // The dry: the Echo hears it first (Echo first) or after Shape and Pump (Echo last).
    if (echoRuns && !echoLast_) makeWet(L, R, n);
    if (shapePart_.runs) runPart(shapePart_, L, R, n, [this](float* l, float* r, int k) { shape_.process(l, r, k); });
    if (pumpPart_.runs) runPart(pumpPart_, L, R, n, [this](float* l, float* r, int k) { pump_.process(l, r, k, keyL_, keyR_); });
    if (echoRuns && echoLast_) makeWet(L, R, n);

    // The wet: Shape and Pump on it, for as long as they are on it (an instance fading out of a bus runs on silence).
    const bool wetParts = shapeWetPart_.runs || pumpWetPart_.runs;
    const bool wetLive = echoRuns || wetParts;
    if (wetLive && !echoRuns) {
        std::fill(wetL_, wetL_ + n, 0.0f);
        std::fill(wetR_, wetR_ + n, 0.0f);
    }
    if (shapeWetPart_.runs) runPart(shapeWetPart_, wetL_, wetR_, n, [this](float* l, float* r, int k) { shapeWet_.process(l, r, k); });
    if (pumpWetPart_.runs) runPart(pumpWetPart_, wetL_, wetR_, n, [this](float* l, float* r, int k) { pumpWet_.process(l, r, k, keyL_, keyR_); });

    // The buses join: the dry at its level, and the wet.
    const bool scaling = dry_.moving() || dry_.value() != 1.0f;
    if (wetLive) {
        if (scaling) {
            for (int i = 0; i < n; ++i) {
                const float d = dry_.next();
                L[i] = L[i] * d + wetL_[i];
                R[i] = R[i] * d + wetR_[i];
            }
        } else {
            for (int i = 0; i < n; ++i) {
                L[i] += wetL_[i];
                R[i] += wetR_[i];
            }
        }
    } else if (scaling) {
        for (int i = 0; i < n; ++i) {
            const float d = dry_.next();
            L[i] *= d;
            R[i] *= d;
        }
    }

    // Time, Post: it bends the sum of the buses.
    if (timePart_.runs && timePart_.where == kPost) runPart(timePart_, L, R, n, timeProcess);

    running_ = (shapePart_.runs || shapeWetPart_.runs ? 1 : 0) + (pumpPart_.runs || pumpWetPart_.runs ? 1 : 0) + (echoRuns ? 1 : 0) +
               (timePart_.runs ? 1 : 0);

    if (out_.moving() || mix_.moving()) {
        for (int i = 0; i < n; ++i) {
            const float o = out_.next();
            if (mixing) {
                const float w = mix_.next();
                L[i] = inputL_[i] + (L[i] * o - inputL_[i]) * w;
                R[i] = inputR_[i] + (R[i] * o - inputR_[i]) * w;
            } else {
                L[i] *= o;
                R[i] *= o;
            }
        }
    } else if (mixing) {
        const float o = out_.value(), w = mix_.value();
        if (w == 0.0f) {   // the input, bit for bit
            std::memcpy(L, inputL_, bytes);
            std::memcpy(R, inputR_, bytes);
        } else {
            for (int i = 0; i < n; ++i) {
                L[i] = inputL_[i] + (L[i] * o - inputL_[i]) * w;
                R[i] = inputR_[i] + (R[i] * o - inputR_[i]) * w;
            }
        }
    } else if (out_.value() != 1.0f) {
        const float o = out_.value();
        for (int i = 0; i < n; ++i) {
            L[i] *= o;
            R[i] *= o;
        }
    }
}

// The longest ring of what is heard or fading. The parts act in series with the Echo (after it on the wet, before it
// in Echo last), and Time bends all of it, so the rings add: the longest of the parts', the Echo's, Time's.
int Groove::tailSamples() const {
    int64_t parts = 0;
    if (shapePart_.audible()) parts = std::max<int64_t>(parts, shape_.tailSamples());
    if (shapeWetPart_.audible()) parts = std::max<int64_t>(parts, shapeWet_.tailSamples());
    if (pumpPart_.audible()) parts = std::max<int64_t>(parts, pump_.tailSamples());
    if (pumpWetPart_.audible()) parts = std::max<int64_t>(parts, pumpWet_.tailSamples());
    int64_t t = parts;
    if (echoPart_.audible()) t += echo_.tailSamples();
    if (timePart_.audible()) t += time_.tailSamples();
    return static_cast<int>(std::min(t, kForever));
}

} // namespace gf
