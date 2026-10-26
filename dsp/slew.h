#pragma once
// The one slew helper: a value moved toward its target by at most `step`, landing on it exactly.
// Every limited edge in the plugin is this: the Pump's crossfades, Shape's controls and its limited
// shape values, the Groove's gains. (Pump, Shape and Groove each had a copy of it.) The arguments
// are finite: a target that is not a number moves the value down by `step`, and a value that is not
// a number stays one; the callers hold both to a range first.
#include <algorithm>

namespace gf {

inline float toward(float cur, float target, float step) {
    return cur < target ? std::min(cur + step, target) : std::max(cur - step, target);
}

} // namespace gf
