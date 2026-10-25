#pragma once
// Groove: the chain, the plugin's whole sound (docs/DESIGN.md, "The signal path and combining").
// The path of this plan:
//
//   input -> input gain -> Shape -> Pump -> output gain -> mix against the input
//
// Two buses run through it, the dry (the input) and the wet (the Echo's output, plan 2). The wet
// is silent until then and has no buffer yet; busOf() and busSounds() in groove.cpp are where the
// chain will learn that it can ring. Shape and Pump each act on a target: All (the sum of the
// buses), Dry or Echo (the wet only). While the wet is silent All is the dry, so All and Dry are
// one bus, and a part aimed at Echo has nothing to work on: it rests and costs nothing. The order
// is the path's: Shape moves what comes in, Pump moves what Shape made.
//
// Every part can be switched on and off. A switch is a 10 ms crossfade between the part's input
// and its output (EffectForce's rack fades), and a part that is off and faded out is neither set
// nor run: it costs no work (running() counts the parts that ran in the last chunk). It comes
// back from cleared state, as a rack module does (the Pump's input detector warms up for 60 ms,
// as at start-up), unless it comes back before its fade-out is over. A part moved to another bus
// fades out on the old one and in on the new one: a target is never a jump.
//
// Input gain, output gain and mix glide, each a straight line across a chunk and no faster than
// a full scale in 1 ms (a jump from -24 to +24 dB takes 16 ms): no click, whatever the knob does.
// At 0 dB and mix 1 they are exactly 1 and do no work, so Params{} passes the input through bit
// for bit. The mix blends the output with the chain's input as it came in (before the input
// gain); at 0 it is that input, bit for bit.
//
// Swing is applied once along the path, and stepSwing() is the one place that says how: the
// swing Shape's and Pump's steps follow. Until Time exists (plan 3) nothing else swings the
// audio, so it is the global Swing, and set() overwrites the Swing in Pump::Params and
// Shape::Params with it.
#include "common.h"
#include "grid.h"
#include "pump.h"
#include "shape.h"

namespace gf {

class Groove {
public:
    enum Target : int { kAll, kDry, kEcho };
    struct Params {
        bool pumpOn = false, shapeOn = false;
        Pump::Params pump;   int pumpTarget = kAll;
        Shape::Params shape; int shapeTarget = kAll;
        Swing swing;         // the global Swing
        float inputDb = 0.0f, outputDb = 0.0f;   // -24..24
        float mix = 1.0f;                        // 0..1: the chain's output against its input
    };

    Groove();
    void reset();
    void set(const Params& p, const ef::Transport& t);
    void process(float* L, float* R, int n);
    int tailSamples() const;   // the longest ring of a part that is heard or fading
    int running() const { return running_; }   // parts run in the last process(): 0 when both are off

    // Swing applied once along the path (docs/DESIGN.md "The signal path and combining", 4): the
    // swing Shape's and Pump's steps follow. Until Time exists (plan 3) it is the global Swing.
    static Swing stepSwing(const Params& p) { return p.swing; }

private:
    // A part as the chain sees it.
    struct Part {
        int bus = 0;            // the bus it works on; the parameter's, once it has faded out
        float fade = 0.0f;      // 0..1: the share of its output that is heard
        float goal = 0.0f;      // where the fade is going
        bool dirty = false;     // has run since it was last reset
        bool runs = false;      // runs in this chunk
        bool audible() const { return fade > 0.0f || goal > 0.0f; }
    };

    template <class M> bool arm(M& m, Part& s, bool on, int target);
    template <class M> void runPart(M& m, Part& s, float* L, float* R, int n);

    Shape shape_;
    Pump pump_;
    Part shapePart_, pumpPart_;
    ef::Ramp in_, out_, mix_;   // gains, and the mix, as linear factors
    bool fresh_ = true;         // the next set() jumps to its targets
    int running_ = 0;
    float inputL_[ef::kChunk] = {}, inputR_[ef::kChunk] = {};   // the chain's input, for the mix
    float givenL_[ef::kChunk] = {}, givenR_[ef::kChunk] = {};   // what a fading part was given
};

} // namespace gf
