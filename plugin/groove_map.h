#pragma once
// MPC's 0..1 parameter values <-> real values and display text. Ranges, curves, names and options come
// from surface/surface.py via build/param_ids.h. The chain's parameters from these (grooveParams) arrive
// with Task 11's second half, once the chain exists.
#include "param_ids.h"

#include <string>

namespace gf {

float paramValue(int id, float norm);    // real value (dB, ms, Hz, 0..1, option index...)
float paramNorm(int id, float value);    // inverse, for state text, tests and the bench
std::string paramDisplay(int id, float norm);   // what the knob's value label shows

} // namespace gf
