#pragma once
// Moves (docs/DESIGN.md, "Moves"): Flex Beat's pads on the Force. A Move is a named one-tap action on the grid that
// drives several parts at once over its length, then returns: a Time pattern, up to four parameter ramps on top of the
// knobs, and the parts it switches on. The sixteen factory Moves are data (surface/surface.py MOVES, checked there,
// written to build/move_data.h) in the order of the MOVES page's tiles. This is EffectWerk's scene moves put on the
// grid's pads: a ramp is a change of a parameter's value relative to where the knob is, and back at the end.
//
// The plugin's engine sees a tile pressed, GO pressed or the Move parameter changed and calls fire() on the audio
// thread (the wiring is the surface's: Task 11); Moves does the rest, chunk by chunk, in apply():
//
//   Firing. fire() arms a Move for the next line of the launch quantize (a 1/16, a beat or a bar of the grid): the first
//   apply() after it looks where the grid is and fixes the line. A press that came up to 10 ms after a line (a sixteenth of
//   the quantize, if that is less) counts as on it: the UI thread's lag. The Move starts in the chunk the line falls in. Another Move fired while one plays waits for
//   its line the same way, and replaces it there; None (or a preset load) stops it: stop().
//
//   Playing. A One Shot plays its length once from the line and ends. A Loop goes round, the same length again, until
//   another Move, stop() or a preset load. Time is counted in beats played, not in song positions: a locate, a loop
//   point or a tempo change under a Move leaves it where it was in its length, as the scene moves are.
//
//   Ramps. A ramp is an `Add` of a change to the knob's 0..1 value (held within 0..1) shaped over its window of the
//   Move's length (Rise 0 to 1, Hold 1, Fall 1 to 0), or a `Set`: an option held while the window is on. Outside its
//   window and when the Move ends an Add glides back to the knob's value over 10 ms (and in over 1 ms, the project's
//   quickest edge); a Set returns at once (an option has no between: the part it belongs to fades itself).
//
//   Parts. A Move switches on the parts it needs (Throw the Echo, a Time pattern Time) from 20 ms before its line (twice
//   a part's fade, so it is in before the Move is) until it ends. A part that is on already stays on. apply() switches
//   them on for the Move only: the Engine keeps one running until its tail has rung out (an Echo after a Throw).
//
//   Time. apply() says what Time should play: the pattern, where it started on the grid (the line; in a Loop, the start
//   of the lap) and whether the Move goes round. The pattern is handed over 5 ms before the line (TimeCore's crossfade
//   ends on the line), and a Loop hands over the next lap the same way, so Time's own joins are the pattern's.
//
// Threads: fire, stop, reset and apply run on the audio thread, one after the other. playing() and armed() are
// atomics, for the surface to light a tile. No allocation, lock or exception anywhere.
#include "../dsp/common.h"

#include <atomic>
#include <cstdint>

namespace gf {

enum MoveKind : uint8_t { kMoveAdd, kMoveSet };                   // Add (to the knob's 0..1) | Set (an option)
enum MoveShape : uint8_t { kMoveRise, kMoveHold, kMoveFall };     // 0 to 1 over the window | 1 | 1 to 0
enum MovePart : int { kPartPump = 1, kPartShape = 2, kPartEcho = 4, kPartTime = 8 };   // MoveDef.partsOn's bits

// A ramp on one parameter. `amount`: an Add's change of the 0..1 value, a Set's option as its 0..1 value (index / (n - 1)).
// The window is as fractions of the Move's length: the ramp acts from `from` to `to` (the whole Move unless it says).
struct MoveRamp {
    int param;
    uint8_t kind;
    float amount;
    uint8_t shape;
    float from = 0.0f, to = 1.0f;
};

struct MoveDef {
    const char* name;
    int timePattern;     // an index into Time's patterns, -1: none
    float lengthBeats;   // a pattern's own length when there is one
    int rampFirst, rampCount;   // kMoveRamps[rampFirst ..]
    int partsOn;         // MovePart bits: on for the Move; the Engine keeps a part on until its tail rings out (R38)
};

constexpr int kMaxMoveRamps = 4;

int numMoves();
const MoveDef& moveDef(int i);             // an index out of range (never from the surface) is the nearest Move
const MoveRamp& moveRamp(int i);           // kMoveRamps[i], likewise

class Moves {
public:
    enum Mode : int { kOneShot, kLoop };               // surface.py MOVE_MODES (Hold waits for the device probe, R24)
    enum Quant : int { kSixteenth, kBeat, kBar };      // surface.py MOVE_QUANTS

    static constexpr double kGraceMs = 10.0;     // a press this late after a line counts as on it (or 1/16 of the quantize)
    static constexpr double kLeadMs = 20.0;      // parts are in this long before a Move's line
    static constexpr double kGlideInMs = 1.0;    // a ramp comes in over this (a full scale: the quickest edge)
    static constexpr double kGlideOutMs = 10.0;  // and goes back over this

    Moves();

    // From the engine, when a press or a change of the Move parameter is seen. `move` 0..15 (anything else is no Move
    // and does nothing), `mode` a Mode, `quant` a Quant (out of range: the nearest).
    void fire(int move, int mode, int quant);
    // A preset or project load, the Move parameter at None: cancels a Move that waits and ends the one that plays
    // (its ramps glide back).
    void stop();
    // Everything forgotten, the clock too: the plugin was suspended for longer than a Stop (the engine's reset()).
    void reset();

    // Applies the playing Move to norm[P_COUNT] in place (ramps on top of the knobs, held within 0..1; the parts it
    // switches on) and says what Time should play: the pattern (-1: none), its start on the grid in beats and whether the
    // Move goes round (a Loop). `frames` is the chunk's length; the default is the control chunk. The grid is MPC's song
    // position while it plays, else a clock that runs on at the tempo from where the last chunk ended: the same clock
    // Time's pattern start is measured on (TimeCore::process).
    // Returns whether it may have written into norm (a ramp, a Set or a part switched on): false, and norm is as it was given.
    bool apply(float* norm, const ef::Transport& t, int* timePattern, double* patternStart, bool* loop, int frames = ef::kChunk);

    // The grid position the last apply() started at: MPC's song position while it plays, else the clock. Time, when it does
    // not run every chunk, takes this as its song position, so that a pattern's start and its clock are never apart.
    double gridBeat() const { return beat0_; }

    int playing() const { return playing_.load(std::memory_order_relaxed); }   // the Move that plays, -1 none
    int armed() const { return armed_.load(std::memory_order_relaxed); }       // the Move that waits for its line, -1 none

    // What the Moves are, for the plugin: how many, how they are named, the parts one switches on.
    static int count() { return numMoves(); }
    static const char* name(int move) { return moveDef(move).name; }
    static int partsOf(int move) { return moveDef(move).partsOn; }

private:
    // A Move that plays, or is gliding out. Its ramps' weights are how much of each is in (0..1).
    struct Run {
        int move = -1;
        bool live = false;       // still plays (else gliding out: its u is where it stopped)
        int mode = kOneShot;
        double start = 0.0;      // its line on the grid
        double u = 0.0;          // where it is in its length (this lap), 0..1
        float w[kMaxMoveRamps] = {};
    };
    static constexpr int kRuns = 4;   // the one that plays and those gliding out (a fifth would be 10 ms of retriggers)

    void begin(int move, int mode, double line);
    void retire(Run& r);
    Run* freeRun();
    void follow(double jump);

    Run runs_[kRuns];
    std::atomic<int> playing_{-1}, armed_{-1};
    int armMove_ = -1, armMode_ = kOneShot, armQuant_ = kBeat;
    bool lineSet_ = false;       // the armed Move's line is known
    double line_ = 0.0;
    double clock_ = 0.0;         // where the next chunk starts, if the transport does not say
    double beat0_ = 0.0;         // where the last chunk started
    bool fresh_ = true;          // no chunk yet
};

} // namespace gf
