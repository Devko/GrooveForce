#include "pump_curves.h"

#include <cmath>

namespace gf {

// ---------------------------------------------------------------------------------------------
// The ten recoveries. A duck falls to its bottom and comes back. x runs from 0 at the bottom to 1
// when the input is back at its own level, and each curve is one way of getting from 0 to 1. The
// formula decides the feel: a high power is over early, a low one lasts, and a late start keeps
// the duck down longer. Together they cover the feels a duck is asked for: the default ease, fast
// recoveries that punch through, slow ones that breathe, ducks that hold and come back late, and
// two plain references (Linear, S-Curve).
//
// The ten, in the order of PumpCurve, with how each one feels:
//   Smooth   1 - (1 - x)^2. The default sidechain feel: an ease-out, back into the input without
//            a snap.
//   Soft     (3x - x^2) / 2. Gentler than Smooth and lower early on: the breath of a compressor
//            on a pad, nothing grabs.
//   Punch    1 - (1 - x)^4. Fast: the duck is mostly over early, a kick that pushes through.
//   Hard     1 - (1 - x)^3. Between Smooth and Punch: a duck with a definite edge.
//   Snap     1 - (1 - x)^6. The fastest: nearly all the way back in the first quarter.
//   Long     x^2. Slow: the duck lasts across most of the window and lets go at the end.
//   Late     0 up to x = 0.3, then Smooth over the rest (u = (x - 0.3) / 0.7, 1 - (1 - u)^2).
//            The duck stays down, then comes back late.
//   Linear   x. A straight line from the bottom to the input's level.
//   S-Curve  3x^2 - 2x^3, a smoothstep: slow at both ends, quick through the middle.
//   Hold     0 up to x = 0.6, then a smoothstep over the last 40% (u = (x - 0.6) / 0.4,
//            3u^2 - 2u^3). Held down for most of the window, then a clear rise at the end.
//
// Smooth, Punch, Hard and Snap are one ease-out at four powers, the one family in the set. Within
// it Punch and Hard come closest, about 0.105 apart at x = 0.25. The other six are different
// shapes: Soft and Long are quadratics, Linear is a line, S-Curve is a smoothstep, and Late and
// Hold stay at 0 for part of the window. The closest pair overall is Linear and S-Curve, about
// 0.096 apart at x = 0.21. test/pump_curves_test.cpp requires every pair of the ten to differ by
// more than 0.05 somewhere.
//
// These are a first version. The user judges them by ear on the Force and may retune any of them,
// as long as the rules in the test still hold. The ends are handled before the switch (x <= 0 is
// 0; x >= 1 and NaN are 1), so the cases only see 0 < x < 1. Pump never moves the gain faster
// than 1 ms per edge, so a steep start (Snap, Punch) is safe as written. Called per sample, so
// each case is a few multiplications.
// ---------------------------------------------------------------------------------------------
float pumpLevel(int curve, float x) {
    if (std::isnan(x)) return 1.0f;   // no position is a recovered duck, not a stuck one
    if (x <= 0.0f) return 0.0f;       // the bottom
    if (x >= 1.0f) return 1.0f;       // recovered

    switch (curve) {
    case PC_SMOOTH:
    default: {
        const float r = 1.0f - x;
        return 1.0f - r * r;
    }
    case PC_SOFT:
        return (3.0f * x - x * x) / 2.0f;
    case PC_PUNCH: {
        const float r = 1.0f - x;
        return 1.0f - r * r * r * r;
    }
    case PC_HARD: {
        const float r = 1.0f - x;
        return 1.0f - r * r * r;
    }
    case PC_SNAP: {
        const float r = 1.0f - x;
        const float r3 = r * r * r;
        return 1.0f - r3 * r3;
    }
    case PC_LONG:
        return x * x;
    case PC_LATE: {
        if (x <= 0.3f) return 0.0f;
        const float r = 1.0f - (x - 0.3f) / 0.7f;
        return 1.0f - r * r;
    }
    case PC_LINEAR:
        return x;
    case PC_SCURVE:
        return x * x * (3.0f - 2.0f * x);
    case PC_HOLD: {
        if (x <= 0.6f) return 0.0f;
        const float u = (x - 0.6f) / 0.4f;
        return u * u * (3.0f - 2.0f * u);
    }
    }
}

} // namespace gf
