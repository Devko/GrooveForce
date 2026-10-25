#pragma once
// The engine: the plugin's parameters played on the audio, chunk by chunk.
//
// It holds the chain (dsp/groove.h: Shape and Pump, the grid's swing, the targets, the levels and the mix)
// and the parameters as the chain takes them (groove_map.h's grooveParams). render() gives the chain one
// chunk at a time, 32 samples, which is what every module expects, and with each MPC's transport at that
// chunk's first sample: while MPC plays, the song position is moved along the block by the tempo. The Pump
// re-anchors on the position it is given, so a loop, a locate or a tempo change leaves it on the grid.
// Stopped, the transport's position is not used: the grid runs on at the tempo from where it was.
//
// reset() clears every tail, and the next chunk jumps to its targets (Groove::reset): a level, a part's
// switch and the Pump's depth are where they are told to be, with no glide. The glue calls it only when
// the plugin was suspended for longer than a Stop (250 ms); a short suspend keeps every state, and nothing
// here may reset while audio is running. With the defaults the block is handed back untouched, bit for bit.
//
// Audio thread: setParams, render, reset, tailSamples, running.
#include "../dsp/common.h"
#include "../dsp/groove.h"
#include "param_ids.h"

namespace gf {

class Engine {
public:
    static constexpr int kParts = 2;   // Shape and Pump: what running() counts against

    Engine();

    void setParams(const float* norm);   // a new snapshot of every parameter (0..1), norm[P_COUNT]
    // In place, any n: chunk by chunk (the transport moved along the block while MPC plays).
    void render(float* L, float* R, int n, const ef::Transport& t);
    void reset();                        // every tail cleared (a long suspend)
    int tailSamples() const { return groove_.tailSamples(); }   // how long it rings after the input stops (0: none)
    int running() const { return groove_.running(); }           // parts run in the last block, of kParts

private:
    Groove groove_;
    Groove::Params params_;
};

} // namespace gf
