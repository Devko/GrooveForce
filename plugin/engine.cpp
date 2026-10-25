#include "engine.h"

#include "groove_map.h"

#include <algorithm>

namespace gf {

Engine::Engine() {
    float n[P_COUNT];
    for (int i = 0; i < P_COUNT; ++i) n[i] = PARAM_INFO[i].def;
    params_ = grooveParams(n);   // the first snapshot from MPC is taken as it is: the chain starts fresh and jumps to it
}

void Engine::setParams(const float* norm) { params_ = grooveParams(norm); }

void Engine::reset() { groove_.reset(); }

void Engine::render(float* L, float* R, int n, const ef::Transport& t) {
    const double beatsPerSample = t.bpm / 60.0 / ef::kRate;
    for (int pos = 0; pos < n; pos += ef::kChunk) {
        const int len = std::min(ef::kChunk, n - pos);
        ef::Transport c = t;   // MPC says where the block starts; the chunk starts `pos` samples on
        if (t.playing && t.valid) c.beats = t.beats + pos * beatsPerSample;
        groove_.set(params_, c);
        groove_.process(L + pos, R + pos, len);
    }
}

} // namespace gf
