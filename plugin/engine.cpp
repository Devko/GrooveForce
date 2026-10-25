#include "engine.h"

#include "groove_map.h"

#include <algorithm>
#include <cmath>

namespace gf {

namespace {

constexpr int kGlide = static_cast<int>(0.010f * ef::kRate);   // a level moves to its new value in 10 ms

// 0 dB is exactly 1: the defaults must not touch the audio, and the approximation behind dbToGain isn't exact there.
float gainOf(float db) { return std::fabs(db) < 1e-3f ? 1.0f : ef::dbToGain(db); }
float share(float mix) { return mix > 0.9999f ? 1.0f : mix < 0.0f ? 0.0f : mix; }

} // namespace

Engine::Engine() {
    float n[P_COUNT];
    for (int i = 0; i < P_COUNT; ++i) n[i] = PARAM_INFO[i].def;
    setParams(n);
    fresh_ = true;   // the defaults were only a start: the first snapshot from MPC is taken as it is
}

void Engine::setParams(const float* norm) {
    const float in = gainOf(paramValue(P_INPUT, norm[P_INPUT]));
    const float out = gainOf(paramValue(P_OUTPUT, norm[P_OUTPUT]));
    const float mix = share(paramValue(P_MIX, norm[P_MIX]));
    if (fresh_) {
        in_.jump(in);
        out_.jump(out);
        mix_.jump(mix);
        fresh_ = false;
        return;
    }
    in_.to(in, kGlide);
    out_.to(out, kGlide);
    mix_.to(mix, kGlide);
}

void Engine::reset() {
    in_.jump(in_.target());
    out_.jump(out_.target());
    mix_.jump(mix_.target());
}

void Engine::render(float* L, float* R, int n, const ef::Transport& /*t*/) {
    for (int pos = 0; pos < n; pos += ef::kChunk) chunk(L + pos, R + pos, std::min(ef::kChunk, n - pos));
}

void Engine::chunk(float* L, float* R, int n) {
    const bool rest = !in_.moving() && !out_.moving() && !mix_.moving() && in_.value() == 1.0f &&
                      out_.value() == 1.0f && mix_.value() == 1.0f;
    if (rest) return;   // Init: nothing to do, nothing touched
    for (int i = 0; i < n; ++i) {
        const float g = in_.next(), o = out_.next(), m = mix_.next();
        const float dl = L[i], dr = R[i];
        L[i] = (dl * (1.0f - m) + dl * g * m) * o;
        R[i] = (dr * (1.0f - m) + dr * g * m) * o;
    }
}

} // namespace gf
