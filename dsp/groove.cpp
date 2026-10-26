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

// How much input Time's ring must have recorded before its groove is asked for: the groove's longest lag, a crossfade and a
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
    sendMix_ = 0.0f;
    listening_ = false;
    running_ = 0;
}

bool Groove::allocate(const Params& p) {
    bool ok = true;
    if (p.echoOn) ok = echo_.allocate() && ok;
    if (p.timeOn) ok = time_.allocate() && ok;
    return ok;
}

// What a part does in this chunk: where its fade is going, and whether it runs at all. Back on after resting it
// starts clean.
template <class M>
bool Groove::arm(M& m, Part& s, bool want) {
    s.goal = want ? 1.0f : 0.0f;
    if (fresh_) s.fade = s.goal;
    s.woke = s.goal > 0.0f && s.fade == 0.0f;   // coming on from rest
    if (s.woke && s.dirty) {
        m.reset();
        s.dirty = false;
    }
    s.runs = s.audible();
    return s.runs;
}

// An instance that begins while its twin on the other bus has been running begins as a copy of it (ruling R48): the
// two duck, or move, alike from the first sample, whatever the trigger and the transport. The audio state is not
// copied (see copyStateFrom()). When it is the Echo that begins (the wet bus starts with it), the instance starts as
// far in as its twin; when a target moves, it fades in.
template <class M>
void Groove::join(M& dry, const Part& dryPart, M& wet, Part& wetPart) {
    if (wetPart.woke && dryPart.runs && !dryPart.woke) {
        wet.copyStateFrom(dry);
        if (echoPart_.woke && dryPart.goal > 0.0f) wetPart.fade = dryPart.fade;
    } else if (dryPart.woke && wetPart.runs && !wetPart.woke) {
        dry.copyStateFrom(wet);
    }
}

// Time. Its ring records whenever it is allocated (ruling R42), so a Move that reads the past (Reverse Beat, Rewind,
// Spin-Back, Tape Stop) finds it: on, Time plays; off, it listens (set and run on a copy of the audio at its place, the
// output untouched). Coming on from rest therefore keeps what the ring holds, and `age` counts what it has recorded
// since it was cleared. It is cleared when its place moves while it rests (the ring then holds another signal), and by
// reset(). A groove waits for the ring to hold what it reads back (see warmSamples()).
void Groove::armTime(const Params& p, const ef::Transport& t) {
    Part& s = timePart_;
    const bool allocated = time_.allocated();
    const int place = placeOf(p.timePlace);
    if (fresh_ || s.fade == 0.0f) {
        if (!fresh_ && s.where != place) {   // it recorded at the other place
            time_.reset();
            s.age = 0;
        }
        s.where = place;
    }
    s.goal = p.timeOn && allocated && place == s.where ? 1.0f : 0.0f;
    if (fresh_) {
        s.fade = s.goal;
        s.age = s.goal > 0.0f ? kSettled : 0;   // on from the first chunk: nothing to wait for
    }
    s.runs = s.audible();
    listening_ = allocated && !s.runs;
    Time::Params tp = p.time;
    tp.swing = p.swing;   // Time swings the audio by the global Swing itself
    if (s.runs) {
        if (s.age < warmSamples(tp, t)) tp.groove = Time::kOff;   // warming up: it only records
    } else {
        tp.groove = Time::kOff;   // listening: nothing to play
        tp.pattern = -1;
        tp.loop = false;
    }
    if (s.runs || listening_) time_.set(tp, t);
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
    const bool echoWasRunning = echoPart_.runs;

    // The Echo and Time first: where the buses sound and where Time works depend on them. A part switched on before
    // its memory has arrived waits for it (silent, and it costs nothing).
    if (arm(echo_, echoPart_, p.echoOn && echo_.allocated())) {
        Echo::Params ep = p.echo;
        ep.swing = swing;
        echo_.set(ep, t);
    }
    // What the Echo hears crosses over when the order changes (R50): nothing hears it while the Echo rests.
    if (fresh_ || echoPart_.woke || !echoWasRunning) sendMix_ = echoLast_ ? 1.0f : 0.0f;
    armTime(p, t);

    // Shape and Pump, each on the buses its target names. The wet's instance begins together with the Echo, and with
    // the dry's whenever both begin at once; both are given the same parameters, the same grid and (the Pumps) the
    // same key.
    const bool wetSounds = echoPart_.audible();
    const int shapeTarget = targetOf(p.shapeTarget), pumpTarget = targetOf(p.pumpTarget);
    const bool sd = arm(shape_, shapePart_, p.shapeOn && onDry(shapeTarget, echoLast_));
    const bool sw = arm(shapeWet_, shapeWetPart_, p.shapeOn && onWet(shapeTarget, echoLast_, wetSounds));
    join(shape_, shapePart_, shapeWet_, shapeWetPart_);
    if (sd || sw) {
        Shape::Params sp = p.shape;
        sp.swing = swing;
        if (sd) shape_.set(sp, t);
        if (sw) shapeWet_.set(sp, t);
    }
    const bool pd = arm(pump_, pumpPart_, p.pumpOn && onDry(pumpTarget, echoLast_));
    const bool pw = arm(pumpWet_, pumpWetPart_, p.pumpOn && onWet(pumpTarget, echoLast_, wetSounds));
    join(pump_, pumpPart_, pumpWet_, pumpWetPart_);
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

// Time off: its ring records L / R and what it would play goes nowhere. At rest it leaves its input alone, but it may
// still be settling from the last thing it played, so it is given a copy.
void Groove::listen(float* L, float* R, int n) {
    std::memcpy(givenL_, L, sizeof(float) * static_cast<size_t>(n));
    std::memcpy(givenR_, R, sizeof(float) * static_cast<size_t>(n));
    timeProcess(givenL_, givenR_, n);
}

// Time over k samples; its ring has recorded that much more (Part::age, which only Time's warm-up reads).
void Groove::timeProcess(float* l, float* r, int k) {
    time_.process(l, r, k);
    timePart_.age = static_cast<int>(std::min<int64_t>(static_cast<int64_t>(timePart_.age) + k, kSettled));
}

// The wet bus. L / R is the dry after Shape and Pump; the key is a copy of it from before them. The Echo hears one
// or the other, as the order says, and a change of the order crosses from the one to the other over 10 ms (R50), so
// that no switch steps the line: a part that moves the dry with a gain other than 1 would make a step one delay
// before it is heard. Settled, the source is the one or the other itself, bit for bit. What the Echo makes is then
// faded in or out as the Echo is switched.
void Groove::makeWet(const float* L, const float* R, int n) {
    const float target = echoLast_ ? 1.0f : 0.0f;
    const float* srcL = sendMix_ >= 1.0f ? L : keyL_;
    const float* srcR = sendMix_ >= 1.0f ? R : keyR_;
    if (sendMix_ != target) {
        float o = sendMix_;
        const float step = target > o ? kFadeStep : -kFadeStep;
        for (int i = 0; i < n; ++i) {
            o = std::clamp(o + step, 0.0f, 1.0f);
            givenL_[i] = keyL_[i] + (L[i] - keyL_[i]) * o;
            givenR_[i] = keyR_[i] + (R[i] - keyR_[i]) * o;
        }
        sendMix_ = o;
        srcL = givenL_;
        srcR = givenR_;
    }
    echo_.process(srcL, srcR, keyL_, keyR_, wetL_, wetR_, n);
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
    auto timeRun = [this](float* l, float* r, int k) { timeProcess(l, r, k); };
    if (timePart_.where == kPre) {
        if (timePart_.runs) runPart(timePart_, L, R, n, timeRun);
        else if (listening_) listen(L, R, n);
    }

    // The key: the chain's input, for the Pump's Input trigger (both buses) and the Echo's duck and modulators.
    const bool echoRuns = echoPart_.runs;
    if (echoRuns || pumpPart_.runs || pumpWetPart_.runs) {
        std::memcpy(keyL_, L, bytes);
        std::memcpy(keyR_, R, bytes);
    }

    // The dry: Shape and Pump, then the Echo (which hears the dry from before them or from after, see makeWet()).
    if (shapePart_.runs) runPart(shapePart_, L, R, n, [this](float* l, float* r, int k) { shape_.process(l, r, k); });
    if (pumpPart_.runs) runPart(pumpPart_, L, R, n, [this](float* l, float* r, int k) { pump_.process(l, r, k, keyL_, keyR_); });
    if (echoRuns) makeWet(L, R, n);

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
    if (timePart_.where == kPost) {
        if (timePart_.runs) runPart(timePart_, L, R, n, timeRun);
        else if (listening_) listen(L, R, n);
    }

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
