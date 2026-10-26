#pragma once
// What the UI thread tells the audio thread about the Moves (docs/DESIGN.md, "Moves"; ruling R35). A tile tap, GO, a
// change of the Move parameter and a preset or project load happen on MPC's UI side; Moves::fire(), stop() and reset() run
// on the audio thread, so the surface (plugin/surface.h) holds these and the plugin hands them to the engine before each
// block. They coalesce (R54): the latest fire, GO or stop is the call, and a load, which comes first, is never lost.

namespace gf {

struct MoveCall {
    enum Kind : int {
        kFire,   // a tile was tapped, or the Move parameter was changed to a Move: select `move` (0..15) and fire it at the quantize
        kGo,     // GO: fire the selected Move again (`move` is the one the UI had selected when it was pressed)
        kStop,   // the Move parameter was changed to None: end the Move that plays, cancel the one that waits
        kLoad,   // a preset or a project was loaded: the Move ends and what it kept on goes at once, nothing is fired
    };
    int kind = kFire;
    int move = 0;
};

} // namespace gf
