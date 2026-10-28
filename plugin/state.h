#pragma once
// The plugin's state text, shared by projects (effGetChunk / effSetChunk) and preset files.
//
// "groovewerk 1": key=value lines for every sound parameter: REAL values (dB, ms, Hz, 0..1) and
// options by name ("pump_curve=Hard", "vol_shape=Offbeat Accent"; an index is read too). A project adds
// the preset it came from. Survives parameters being added or reordered AND ranges changing: a key it
// doesn't know is skipped, and a project changes only what it lists. The selected Move (`move`) is saved like any
// other sound parameter; loading it only selects it, and ends the Move that plays (Surface::noteLoad).
#include "surface.h"

#include <string>

namespace gf {

constexpr int kStateVersion = 1;

std::string saveState(const Surface& s, bool asPreset);
bool isStateText(const std::string& text);   // "groovewerk <version >= 1>" (a UTF-8 BOM allowed)
// A preset starts from the defaults (what it doesn't say is the default); a project's state only
// overrides what it lists. False if it isn't GrooveWerk state.
bool loadState(Surface& s, const std::string& text, bool asPreset);

} // namespace gf
