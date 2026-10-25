#pragma once
// The engine: the plugin's parameters played on the audio, chunk by chunk.
//
// This is the interim engine of Task 11's first half. It applies only the three levels, Input, Output
// and Mix (the dry signal against the processed one), each gliding to a new value over 10 ms. With all
// three at their defaults it hands the block back untouched, bit for bit, as Init must. The chain
// (dsp/groove.h: Pump, Shape, the grid's swing, the targets) arrives with Task 11's second half, which
// replaces what render() does with each chunk; the interface below stays.
//
// Audio thread: setParams, render, reset, tailSamples.
#include "../dsp/common.h"
#include "param_ids.h"

namespace gf {

class Engine {
public:
    Engine();

    void setParams(const float* norm);   // a new snapshot of every parameter (0..1), norm[P_COUNT]
    // In place, any n: chunk by chunk (the chain takes one chunk at a time, with MPC's transport moved
    // along it while it plays).
    void render(float* L, float* R, int n, const ef::Transport& t);
    void reset();                        // every tail cleared (a long suspend)
    int tailSamples() const { return 0; }   // how long it rings after the input stops (0: none)

private:
    void chunk(float* L, float* R, int n);

    ef::Ramp in_, out_, mix_;   // linear gains and the share of the processed signal
    bool fresh_ = true;         // nothing played yet: the first snapshot is taken as it is, not glided to
};

} // namespace gf
