// Groove: see groove.h.
#include "groove.h"

#include <algorithm>
#include <cstring>

namespace gf {

namespace {

constexpr float kFadeStep = 1.0f / (0.010f * ef::kRate);   // a part on or off: 10 ms
constexpr float kSlope = 1.0f / (0.001f * ef::kRate);      // the gains' fastest move a sample: a full scale in 1 ms
constexpr float kChunkSlope = kSlope * ef::kChunk;
constexpr float kMaxDb = 24.0f;

// 0 dB is exactly 1: the defaults pass the input through bit for bit. NaN is 0 dB.
float gainOf(float db) {
    db = ef::clampParam(db, -kMaxDb, kMaxDb, 0.0f);
    return db == 0.0f ? 1.0f : ef::dbToGain(db);
}

float toward(float v, float target, float step) { return v < target ? std::min(v + step, target) : std::max(v - step, target); }

// The bus a target works on: 0 the dry, 1 the wet. All is the sum of the two, which is the dry while
// the wet is silent: the same bus, so moving a part between All and Dry is no change at all.
int busOf(int target) { return target == Groove::kEcho ? 1 : 0; }

// Whether a bus has anything on it. The wet is silent until plan 2's Echo, so a part aimed at it
// has nothing to work on.
bool busSounds(int bus) { return bus == 0; }

} // namespace

Groove::Groove() = default;

void Groove::reset() {
    shape_.reset();
    pump_.reset();
    shapePart_ = pumpPart_ = Part();
    fresh_ = true;
    running_ = 0;
}

// What a part does in this chunk: where its fade is going, and whether it runs at all. A part aimed
// at another bus keeps its old one until it has faded out. Back on after resting it starts clean.
template <class M>
bool Groove::arm(M& m, Part& s, bool on, int target) {
    const int bus = busOf(std::clamp(target, static_cast<int>(kAll), static_cast<int>(kEcho)));
    if (fresh_ || s.fade == 0.0f) s.bus = bus;
    s.goal = on && bus == s.bus && busSounds(bus) ? 1.0f : 0.0f;
    if (fresh_) s.fade = s.goal;
    if (s.goal > 0.0f && s.fade == 0.0f && s.dirty) {
        m.reset();
        s.dirty = false;
    }
    s.runs = s.audible();
    return s.runs;
}

void Groove::set(const Params& p, const ef::Transport& t) {
    const float in = gainOf(p.inputDb), out = gainOf(p.outputDb), mix = ef::clampParam(p.mix, 0.0f, 1.0f, 1.0f);
    if (fresh_) {
        in_.jump(in);
        out_.jump(out);
        mix_.jump(mix);
    } else {   // a full scale a millisecond at most, from where each is
        in_.to(toward(in_.value(), in, kChunkSlope), ef::kChunk);
        out_.to(toward(out_.value(), out, kChunkSlope), ef::kChunk);
        mix_.to(toward(mix_.value(), mix, kChunkSlope), ef::kChunk);
    }

    const Swing swing = stepSwing(p);
    if (arm(shape_, shapePart_, p.shapeOn, p.shapeTarget)) {
        Shape::Params sp = p.shape;
        sp.swing = swing;
        shape_.set(sp, t);
    }
    if (arm(pump_, pumpPart_, p.pumpOn, p.pumpTarget)) {
        Pump::Params pp = p.pump;
        pp.swing = swing;
        pump_.set(pp, t);
    }
    fresh_ = false;
}

// The part works on L / R in place; while it fades, what it made is crossfaded with what it was given.
template <class M>
void Groove::runPart(M& m, Part& s, float* L, float* R, int n) {
    s.dirty = true;
    const bool fading = s.fade != s.goal;
    if (fading) {
        std::memcpy(givenL_, L, sizeof(float) * static_cast<size_t>(n));
        std::memcpy(givenR_, R, sizeof(float) * static_cast<size_t>(n));
    }
    m.process(L, R, n);
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

void Groove::process(float* L, float* R, int n) {
    n = std::min(n, ef::kChunk);
    if (n <= 0) return;
    const bool mixing = mix_.moving() || mix_.value() != 1.0f;   // else the output is all there is
    if (mixing) {
        std::memcpy(inputL_, L, sizeof(float) * static_cast<size_t>(n));
        std::memcpy(inputR_, R, sizeof(float) * static_cast<size_t>(n));
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

    running_ = 0;
    if (shapePart_.runs) {
        runPart(shape_, shapePart_, L, R, n);
        ++running_;
    }
    if (pumpPart_.runs) {
        runPart(pump_, pumpPart_, L, R, n);
        ++running_;
    }

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
            std::memcpy(L, inputL_, sizeof(float) * static_cast<size_t>(n));
            std::memcpy(R, inputR_, sizeof(float) * static_cast<size_t>(n));
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

int Groove::tailSamples() const {
    int t = 0;
    if (shapePart_.audible()) t = std::max(t, shape_.tailSamples());
    if (pumpPart_.audible()) t = std::max(t, pump_.tailSamples());
    return t;
}

} // namespace gf
