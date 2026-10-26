#pragma once
// The shape library: the named curves Shape's lanes move volume, filter and pan along. Nothing is
// drawn on the device, so a shape is data: surface/surface.py holds the 38 of them (SHAPES, checked
// there) and writes surface/build/shape_data.h, which dsp/shapes.cpp evaluates. Waves, the Pump
// curves, gates and accents, random.
//
// A shape is one cycle of a value 0..1. shapeAt(shape, cycles) takes a position in cycles (the song
// position over the lane's rate, plus its phase): the fraction is where we are in the cycle, the whole
// number says which cycle. The shapes repeat every cycle; only the two random ones use the whole number,
// as their seed, so they play the same wherever the song position repeats (a loop, a locate) and need no
// state of their own.
//
// The kinds:
//   Points        breakpoints x (ascending from 0, y 0..1); each point's `seg` is the way to the next
//                 point, the last running on to the first at x = 1. A shape that ends where it began
//                 wraps without a step; one whose last segment is Hold (the square, the saws) ends
//                 in an edge at the wrap.
//   Steps         16 levels in kShapePoints, one per sixteenth of the cycle (x = i/16): a gate is 1 and 0,
//                 an accent has levels between.
//   Pump          0 at phase 0 and pumpLevel(curve, phase) from there: the duck falls at the wrap and
//                 recovers across the cycle, along the curve.
//   RandomSteps   a level per sixteenth, drawn from a hash of (shape, cycle, step).
//   RandomSmooth  four levels per cycle drawn the same way, joined by smooth segments; the last
//                 runs on to the next cycle's first level, so the shape never steps.
//
// Everything here is a pure function of its arguments: no allocation, no state, safe from any thread.
#include <cstdint>

namespace gf {

enum class Seg : uint8_t { Linear, Smooth, Hold, Curve };   // from a point to the next
enum class ShapeKind : uint8_t { Points, Steps, Pump, RandomSteps, RandomSmooth };

struct ShapePoint { float x, y; Seg seg; float tension; };   // x ascending in [0, 1), y 0..1
struct ShapeDef {
    const char* name;
    ShapeKind kind;
    int first, count;    // Points: kShapePoints[first..first+count); Steps: 16 levels there (x = i/16)
    int pumpCurve;       // Pump: the PumpCurve
};

int numShapes();
const char* shapeName(int shape);   // an index out of range (never from the surface) is the nearest shape

// The shape's value 0..1 at `cycles`: the fraction is the phase. The whole number seeds the random
// shapes, so they repeat wherever the song position repeats. A position that is not a number, or too
// large to have a fraction, is cycle 0, phase 0.
float shapeAt(int shape, double cycles);

// One segment: the value u (0..1) of the way from a to b.
//   Linear   a + (b - a) u
//   Smooth   a + (b - a) (1 - cos(pi u)) / 2, a raised cosine: flat at both ends
//   Hold     a, the whole way (b is where the next segment starts)
//   Curve    a + (b - a) (exp(6 t u) - 1) / (exp(6 t) - 1) for the tension t (-1..1): positive starts slow and
//            ends fast, negative the other way round, 0 is the straight line
float segmentValue(Seg seg, float a, float b, float tension, double u);

} // namespace gf
