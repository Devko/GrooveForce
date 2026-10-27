#pragma once
// The engine: the plugin's parameters played on the audio, chunk by chunk.
//
// It holds the chain (dsp/groove.h: Shape, Pump, the Echo and Time, the grid's swing, the targets, the levels and the mix),
// the Moves (plugin/moves.h) and the parameters as the chain takes them (groove_map.h's grooveParams). render() gives the
// chain one chunk at a time, 32 samples, which is what every module expects.
//
// The grid. Every part and the Moves are given one position per chunk: while MPC plays, its song position at that chunk's first
// sample (the block's start moved along by the tempo); stopped, or with no position to give, a clock of the engine's that
// runs on at the tempo from where the last chunk ended (seeded from MPC's held position at the first chunk after a reset()).
// The Pump, Shape and Time re-anchor on the position they are given, so a loop, a locate or a tempo change leaves them on the
// grid; and because all of them are given the same clock, a Move's pattern (its start is a line of this clock) and Time (which
// follows it: Time::nextBeat() is this clock) are on the same line, however late a part woke up while MPC was stopped.
//
// The Moves. Once a chunk, Moves::apply() lays the Move that plays on a copy of the snapshot (ramps on the knobs, the parts it
// needs switched on) and says which Time pattern to play and where it started. The copy becomes the chain's parameters (only
// when it differs from the last one's, and a ramp's steps at most once in 128 frames: while nothing moves, the Moves cost
// nothing, and a ramp that moves in every chunk costs one conversion a block; the two are not even compared in a chunk where
// nothing was laid on the copy and the chain's parameters were made from the snapshot itself, R60a). A tile, GO, a change of the Move
// parameter and a preset or project load reach the engine as MoveCalls from the UI thread (plugin/surface.h holds them, coalesced; the
// plugin passes them in at the start of a block): Moves::fire(), stop() and reset() run on the audio thread only. A preset or
// project load ends the Move and what it had on (it fires nothing, whatever Move the new sound selects); a reset() clears the
// Moves with everything else, Time's ring too.
//
// A part a Move switched on stays on after the Move until its tail has rung out (ruling R38): the Echo a Throw opened keeps
// ringing, for as long as the chain's tail says (Groove::tailSamples() when the Move let go, at most four lines' worth), and
// then goes off as a switch does, over 10 ms. Time, Shape and Pump have no tail to keep. A part the knobs have on is theirs.
//
// reset() clears every tail, and the next chunk jumps to its targets (Groove::reset): a level, a part's switch and the Pump's
// depth are where they are told to be, with no glide. The glue calls it only when the plugin was suspended for longer than a
// Stop (250 ms); a short suspend keeps every state, and nothing here may reset while audio is running. With the defaults the
// block is handed back untouched, bit for bit.
//
// Threads. Audio: setParams, render, moveCall, reset, tailSamples, running, litMoves. UI: allocateEcho and allocateTime, once
// (the first time the Echo or Time is switched on, or a Move that needs them is selected, R42; the audio thread takes the
// memory up when it has arrived, and until then the part is silent and costs nothing).
#include "../dsp/common.h"
#include "../dsp/groove.h"
#include "move_call.h"
#include "moves.h"
#include "param_ids.h"

#include <cstdint>

namespace gf {

class Engine {
public:
    static constexpr int kParts = 4;   // Shape, Pump, the Echo and Time: what running() counts against
    // How long a part a Move switched on is kept for at most (R38): four lines of three seconds, one after the other.
    static constexpr int kMaxHoldSamples = 12 * 44100;

    Engine();

    void setParams(const float* norm);   // a new snapshot of every parameter (0..1), norm[P_COUNT]
    // In place, any n: chunk by chunk (the transport moved along the block while MPC plays).
    void render(float* L, float* R, int n, const ef::Transport& t);
    void moveCall(const MoveCall& call);   // a tile, GO, the Move parameter or a load, from the UI thread: before the next render()
    void reset();                        // every tail cleared, the Moves and Time's ring too (a long suspend)
    int tailSamples() const { return groove_.tailSamples(); }   // how long it rings after the input stops (0: none)
    int running() const { return groove_.running(); }           // parts run in the last block, of kParts
    uint32_t litMoves() const;           // the Moves that play or wait for their line (bit k: Move k), for the tiles
    uint32_t conversions() const { return conversions_; }   // (for tests) how often the chain's parameters were worked out
    uint32_t compares() const { return compares_; }         // (for tests) how often a chunk's snapshot was compared with the one in use
    void compareAlways(bool on) { compareAlways_ = on; }    // (for tests) compare in every chunk, as before an idle chunk skipped it

    // UI thread: the memory of the Echo's lines and of Time's ring, once. False if there was none to be had.
    bool allocateEcho() { return groove_.allocateEcho(); }
    bool allocateTime() { return groove_.allocateTime(); }

private:
    ef::Transport gridFor(const ef::Transport& t, int pos, int len);
    bool keepEcho(float* norm, int len);   // whether it wrote into norm
    void fire(int move);
    int pickOf(int id) const;

    Groove groove_;
    Groove::Params params_;       // the chain's parameters for `used_`, the pattern fields of the chunk
    Moves moves_;
    float norm_[P_COUNT];         // the snapshot
    float used_[P_COUNT];         // what params_ was last made from (the snapshot with the Moves on it)
    bool haveUsed_ = false;
    static constexpr int kConvertGap = 128;   // frames between two workings out of the chain's parameters, a Move's ramps moving
    int sinceConvert_ = kConvertGap;          // frames since the last one
    uint32_t conversions_ = 0;
    uint32_t compares_ = 0;
    bool usedLaid_ = false;       // used_ is a snapshot that a Move or the Echo hold was laid on: it may differ from norm_
    bool compareAlways_ = false;

    double clock_ = 0.0;          // the grid's position at the next chunk, if MPC does not say
    bool fresh_ = true;           // the next chunk is the first since the start or a reset()

    bool echoMoved_ = false;      // a Move has the Echo on, and the knobs have it off
    int echoHold_ = 0;            // samples to keep it on after the Move let go
};

} // namespace gf
