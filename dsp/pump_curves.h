#pragma once
// Pump curves: the named ways a duck comes back. Pump (the part that ducks on every step of the
// grid, or on every hit of the input) and the Pump shapes in Shape's library both use them; a curve
// is picked by name, never drawn.
//
// A duck falls to its bottom and then recovers. A pump curve is that recovery as a level:
// pumpLevel(curve, x) takes x, how far through the recovery we are (0 at the duck's bottom, 1 when
// it is over and the input is back at its own level), and returns where the level is, 0 (fully
// ducked) to 1 (the input's level). Every curve starts at 0 and ends at 1 and never falls; what
// differs is the route between: Smooth eases back, Punch and Snap leap early, Long and Late hold
// back, Hold stays down for most of the window.
//
// The gain is 1 - depth (1 - pumpLevel(curve, x)): depth 1 ducks to silence and recovers along the
// curve, depth 0.5 ducks half way and recovers along the same shape, depth 0 never moves. x is how
// far the recovery has got, i.e. the time since the bottom divided by the recovery's length (Pump's
// Length control, as a share of the step), so the curve is the same whatever the rate or the length.
//
// pumpLevel is called per sample in the audio path: no allocation, no state, cheap.
namespace gf {

enum PumpCurve : int { PC_SMOOTH, PC_SOFT, PC_PUNCH, PC_HARD, PC_SNAP, PC_LONG, PC_LATE,
                       PC_LINEAR, PC_SCURVE, PC_HOLD, PC_COUNT };

// The names the options surface shows (surface.py's PUMP_CURVES, same order).
inline constexpr const char* kPumpCurveNames[PC_COUNT] = {
    "Smooth", "Soft", "Punch", "Hard", "Snap", "Long", "Late", "Linear", "S-Curve", "Hold"};

// The level through a duck's recovery: x = 0 at the duck's bottom, 1 recovered. Returns 0..1
// (0 fully ducked, 1 the input's level). The gain is 1 - depth (1 - pumpLevel(curve, x)).
float pumpLevel(int curve, float x);

} // namespace gf
