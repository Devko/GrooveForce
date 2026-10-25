#pragma once
// The grid (docs/DESIGN.md, "The signal path and combining", 4): swing as a map between MPC's
// song position and straight time, so Pump and Shape swing the same way. A module runs its steps
// on straight time (toStraight of the song position): on that clock the 16ths (or 8ths) are evenly
// spaced, and toSwung says where each one lands in the audio.
//
// Swing delays every second unit: of each pair of units (2u beats, from a multiple of 2u) the
// second starts at 2u * amount instead of u, so it comes (amount - 0.5) * 2 units late (at 120 BPM
// on 1/16, 58% is 20 ms and 66% is 40 ms). Inside the pair the map is two straight lines, one
// through each half, so the first unit stretches and the second shrinks to meet the next pair,
// whose start doesn't move: the pair boundaries are fixed points. The same rule holds before
// beat 0 (floor, not a cast). Both directions only ever go forward, up to rounding: the Force's
// build reorders float math (-funsafe-math-optimizations), which can move a result by the last
// bit or two, so positions closer than 1e-12 beats are not ordered by their images.
//
// An amount that isn't finite or is at most 0.5 (and a unit that isn't a length) gives the position
// back unchanged, bit for bit: that is what keeps Init changing nothing. Past kFarBeats a double
// can't tell one 16th from the next; there the position is passed on as it is.
#include <algorithm>
#include <cmath>

namespace gf {

struct Swing {
    float amount = 0.5f;      // 0.5 straight .. 0.75 (clamped)
    double unitBeats = 0.25;  // the swung unit: 0.25 (1/16) or 0.5 (1/8)
};

inline constexpr float kMaxSwing = 0.75f;
inline constexpr double kFarBeats = 1e15;

// Where the second unit starts in swung time, in beats from the start of its pair: 2u * amount.
// 0 for a Swing that doesn't swing.
inline double swingSplit(const Swing& s) {
    if (!(s.amount > 0.5f) || !std::isfinite(s.amount) || !(s.unitBeats > 0.0)) return 0.0;   // NaN fails each test
    const double split = 2.0 * s.unitBeats * std::min(s.amount, kMaxSwing);
    return std::isfinite(split) ? split : 0.0;
}

// The song position (quarter-note beats) as straight time: where it would be on an unswung grid.
// Piecewise linear in each pair of units; pair boundaries are fixed points.
inline double toStraight(double beats, const Swing& s) {
    const double split = swingSplit(s);
    if (split <= 0.0 || !(std::fabs(beats) < kFarBeats)) return beats;
    const double u = s.unitBeats, pair = 2.0 * u;
    const double first = std::floor(beats / pair) * pair;   // the pair's start
    const double x = beats - first;
    return first + (x < split ? x * u / split : u + (x - split) * u / (pair - split));
}

// The inverse: where a straight position lands in the swung audio.
inline double toSwung(double straightBeats, const Swing& s) {
    const double split = swingSplit(s);
    if (split <= 0.0 || !(std::fabs(straightBeats) < kFarBeats)) return straightBeats;
    const double u = s.unitBeats, pair = 2.0 * u;
    const double first = std::floor(straightBeats / pair) * pair;
    const double y = straightBeats - first;
    return first + (y < u ? y * split / u : split + (y - u) * (pair - split) / u);
}

} // namespace gf
