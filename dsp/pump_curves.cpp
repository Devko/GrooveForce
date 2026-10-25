#include "pump_curves.h"

#include <cmath>

namespace gf {

// ---------------------------------------------------------------------------------------------
// FOR YOU: the ten recoveries. This is the decision the design names: what makes a duck feel like
// Kickstart. The grid, the ducks and the gain are built around it; this is the part that decides
// how a duck sounds.
//
// What pumpLevel(curve, x) does. A duck has fallen to its bottom and now comes back. x is how far
// through the recovery we are: 0 at the bottom, 1 when it is over. Return the level there, 0
// (still fully ducked) to 1 (back at the input's own level). The gain is
// 1 - depth (1 - pumpLevel(curve, x)), so a curve that is high early gives a duck that is over
// quickly (a punch), one that stays low gives a duck that lasts (a long breath). Pump calls it
// for every sample while a duck is recovering, so keep to a few multiplications: no tables, no
// state, no allocation. (x * x * x is far cheaper than a power; ef::exp2Fast in dsp/fastmath.h
// is there for an exponential.)
//
// The rules the tests enforce (test/pump_curves_test.cpp; they are the only hard rules):
//   - curvesEnds:      every curve is exactly 0 at x = 0 and exactly 1 at x = 1. The code below
//                      already handles x <= 0, x >= 1 and a NaN x before your switch, so you only
//                      write what happens for 0 < x < 1, but your formula must not overshoot.
//   - curvesRise:      never falls and stays within 0..1 (checked at 4096 steps of x). A formula
//                      that can come out a rounding error over 1 wants a std::min(.., 1.0f).
//                      No bounce or overshoot: a curve that dips back is a failure here.
//   - curvesDiffer:    any two of the ten differ by more than 0.05 somewhere in 0..1. The pairs
//                      likely to be close are the neighbours: Smooth / Soft, Punch / Hard / Snap
//                      and Long / Late. Give them clearly different shapes, not the same curve
//                      with a slightly different power.
//   - curvesCharacter: Linear is exactly x (0.5 at 0.5); Hold is below 0.05 at x = 0.5 (still
//                      down half way); Late is lower than Smooth at 0.5 (comes back later); Snap
//                      is higher than Smooth at 0.25 (is back sooner).
//
// What each name should feel like (the first five are the plan's guidance; Soft and Hard are my
// reading of their names, yours to overrule):
//   - Smooth   the default sidechain feel, a duck that eases back out; for example an ease-out
//              such as 1 - (1 - x)^2.
//   - Soft     gentler than Smooth: the breath of a compressor on a pad, nothing grabs.
//   - Punch    comes back fast, a high power or an exponential: the duck is mostly over early.
//   - Hard     firmer than Smooth, between Smooth and Snap: a duck with a definite edge.
//   - Snap     the fastest of them: snaps back early (see curvesCharacter).
//   - Long     comes back slowly, so the duck lasts across most of the recovery window.
//   - Late     starts late: stays low, then comes back (see curvesCharacter).
//   - Linear   a straight line from 0 to 1: just x.
//   - S-Curve  a smoothstep: slow, quick through the middle, slow again.
//   - Hold     stays down for most of the window, then rises at the end (see curvesCharacter).
//
// Pump limits every gain edge to 1 ms or slower, so a very steep start or end is safe, but it
// also smooths a curve that is really a step: give Hold's rise a visible ramp.
//
// Run make test-module M=pump_curves to see the checks; 0 failed when every rule holds.
// The body is about 10-15 lines: a switch (curve) with one shape per case.
// ---------------------------------------------------------------------------------------------
float pumpLevel(int curve, float x) {
    if (std::isnan(x)) return 1.0f;   // no position is a recovered duck, not a stuck one
    if (x <= 0.0f) return 0.0f;       // the bottom
    if (x >= 1.0f) return 1.0f;       // recovered
    (void)curve;
    // TODO(you): the ten recoveries for 0 < x < 1, one case per PumpCurve (PC_SMOOTH .. PC_HOLD);
    // a default: falls back to Smooth, so a curve index out of range (never from the surface) can't
    // read anything it shouldn't. The placeholder is a straight line for every curve.
    return x;
}

} // namespace gf
