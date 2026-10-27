#pragma once
// MPC's 0..1 parameter values <-> real values and display text, and the chain's parameters from them.
// Ranges, curves, names and options come from surface/surface.py via build/param_ids.h; grooveParams()
// is where each of them becomes a field of dsp/groove.h's Params, and groove_map.cpp is where surface.py's
// option lists are held to the DSP's (static_asserts).
#include "../dsp/groove.h"
#include "param_ids.h"

#include <string>

namespace gf {

float paramValue(int id, float norm);    // real value (dB, ms, Hz, 0..1, option index...)
float paramNorm(int id, float value);    // inverse, for state text, tests and the bench
std::string paramDisplay(int id, float norm);   // what the knob's value label shows

// Every parameter as the chain takes it; norm[P_COUNT] are MPC's 0..1 values. A value within a hair of its
// rest (a depth at 0, a mix at 100%, 0 dB, a straight swing, an Echo at its send of 1 with nothing filtered,
// driven or modulated) is its rest exactly, so Init stays the input bit for bit however the knob got there.
// Any norm is usable: NaN and out-of-range values land on the ends of the range. The Echo's four lines are
// read through one table (groove_map.cpp's kLineIds); the Move parameters are not the chain's (plugin/moves.h
// plays them) and map to nothing, and Time's pattern is the engine's to set.
Groove::Params grooveParams(const float* norm);

} // namespace gf
