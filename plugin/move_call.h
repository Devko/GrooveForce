#pragma once
// What the UI thread tells the audio thread about the Moves (docs/DESIGN.md, "Moves"; ruling R35). A tile tap, GO, a
// change of the Move parameter and a preset or project load happen on MPC's UI side; Moves::fire(), stop() and reset() run
// on the audio thread, so the surface (plugin/surface.h) queues these and the plugin hands them to the engine, one block at
// a time, in the order they were made.

namespace gf {

struct MoveCall {
    enum Kind : int {
        kFire,   // a tile was tapped, or the Move parameter was changed to a Move: select `move` (0..15) and fire it at the quantize
        kGo,     // GO: fire the selected Move again
        kStop,   // the Move parameter was changed to None: end the Move that plays, cancel the one that waits
        kLoad,   // a preset or a project was loaded: the Move ends and what it kept on goes at once, nothing is fired
    };
    int kind = kFire;
    int move = 0;
};

} // namespace gf
