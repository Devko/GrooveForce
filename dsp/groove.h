#pragma once
// Groove: the chain, the plugin's whole sound (docs/DESIGN.md, "The signal path and combining").
// The path, with the Echo first (the default) and Time Post (the default); the key is a copy of the signal
// where it is taken, for the Pump's Input trigger and the Echo's duck and modulators:
//
//   input -> input gain -> [Time, Pre] -+--> key
//                                       +-- dry ------------------ Shape -> Pump -- dry level --+
//                                       '-- send -> Echo -> wet -- Shape -> Pump ---------------+-> [Time, Post]
//                                                                                                  -> output gain -> mix
//
// and with the Echo last: Shape and Pump come first, on the dry, and the send is what they made (the
// targets are ignored, since there is only the dry to move):
//
//   input -> input gain -> [Time, Pre] -+--> key
//                                       '-- dry -> Shape -> Pump -+-- dry level ----------------+-> [Time, Post]
//                                                                 '-- send -> Echo -> wet ------'    -> output gain -> mix
//
// Two buses run through it, the dry (the input) and the wet (the Echo's output). Shape and Pump each act
// on a target: Dry, Echo (the wet only) or All. All means the part acts on the dry and on the wet
// identically, as if on their sum: there are two instances of it, one on each bus, with the same
// parameters and the same grid, and the two Pumps are fed the same key, so their gains are equal. While
// the wet is silent (the Echo is off, or its memory has not arrived) All is the dry, the wet's instance is
// not run, and a part aimed at the Echo has nothing to work on: it rests and costs nothing. The order is
// the path's: Shape moves what comes in, Pump moves what Shape made. An instance that has no business on
// its bus any more (a target moved, the order changed) fades out of it, and the one that has fades in: a
// target is never a jump.
//
// The key (ruling R26) is the chain's input: what comes in after the input gain, and after Time when Time
// is Pre (it bends the input first, so the key hears the hits where the dry will play them: an Input
// trigger on swung audio ducks where the hits are, not where they were before the swing), before any other
// part. The Pump's Input trigger and the Echo's duck, Env and Follow listen to it, whatever the target:
// Rumble, a Pump on the Echo with the Input trigger, hears the dry kick and not the echo it ducks.
//
// `dry` scales the dry bus where it joins the wet (R29). The send, the key and the mix's input side are
// not scaled, so the echo goes on ringing with the dry turned off (Break); the wet is not scaled.
//
// Time is one part with a place. Post, it bends the sum of the buses, so a tape stop stops the echoes
// too; Pre, it bends the input only, and the echo goes on ringing through a stop. A move of the place
// fades Time out where it was and in where it goes, from cleared state (its ring holds another signal at
// the other place).
//
// Every part can be switched on and off. A switch is a 10 ms crossfade between the part's input and its
// output (the Echo's is a fade of the wet), and a part that is off and faded out is neither set nor run:
// it costs no work (running() counts the parts that ran in the last chunk: Shape, Pump, Echo, Time; Time's
// ring excepted, below). It comes back from cleared state, as a rack module does (the Pump's input detector
// warms up for 60 ms, as at start-up), unless it comes back before its fade-out is over.
//
// Two instances begin alike (ruling R48). Both instances of a part that comes on together come on in the same
// chunk, from cleared state. An instance that begins while its twin on the other bus has been running (the Echo
// coming on under a Pump on All, or a target moving onto the bus the part was not on) begins as a copy of it
// (copyStateFrom() of Pump and Shape): the grid's phase, the Pump's detector with its low-pass and warm-up, the
// slews and the lanes are the twin's, so the two duck and move alike from the first sample, whatever the trigger
// and the transport. What the audio left in the twin's crossover or filter is not copied (it is the other bus's
// signal): both are linear, so the twin keeps its own ring, the copy starts with none, and together they are
// one crossover or filter on the sum. When the Echo itself comes on, the copy starts as far in as the twin (the
// wet is silent and fades in on its own); when a target moves, it fades in over 10 ms like any part. A filter
// that starts on a wet that is already sounding rings in for a while: that is what a filter does on its first
// samples.
//
// Time's ring listens (ruling R42). While it is allocated and Time is off, the audio at Time's place goes into
// it all the same, on a copy and with no groove or pattern, so the output is untouched, bit for bit, and a Move
// that reads the past (Reverse Beat, Rewind, Spin-Back, Tape Stop) always finds it. That is the one thing a part
// that is off costs: a set() and a write of each sample (listening() says so; running() does not count it). A
// ring that was never allocated is not fed. The ring is cleared by reset(), and when Time's place moves while
// it rests (it holds the other place's signal); a Time that comes on keeps what it has.
//
// Time warms up. Its groove asks for a lag, which reads the ring behind now, and a ring that has just begun
// to record holds nothing there: the output would drop out for the length of the lag and then step to the
// first sample recorded. So a Time whose ring has recorded less than the groove's longest lag and a crossfade
// passes its input (recording) until it has, and only then engages the groove, by its own crossfade. With the
// ring listening that is only the first moments after it was allocated or cleared. A Move's pattern is not
// held back, since it has its own start on the grid. A Time that is on from the first chunk (the chain was
// just reset) has nothing to wait for and does not warm up.
//
// Memory. The Echo's lines and Time's ring are allocated on the UI thread by allocate() (the plugin calls
// it when a switch goes on) and handed over to the audio thread through an atomic pointer inside each
// part. A part that is switched on before its memory has arrived stays silent and costs nothing, and fades
// in once it has: nothing is read before then, and nothing is uninitialised after.
//
// Input gain, output gain, the dry level and the mix glide, each a straight line across a chunk and no
// faster than a full scale in 1 ms (a jump from -24 to +24 dB takes 16 ms): no click, whatever the knob
// does. At 0 dB, dry 1 and mix 1 they are exactly 1 and do no work, so Params{} passes the input through
// bit for bit; so do an Echo with no line on (the wet is exactly 0) and a Time with no groove and no
// pattern (it only records). The mix blends the output with the chain's input as it came in (before the
// input gain); at 0 it is that input, bit for bit.
//
// Swing is applied once along the path, and stepSwing() is the one place that says how: the swing Shape's
// and Pump's steps (and the Echo's LFOs) follow. It is the global Swing, unless Time's Groove is Swing and
// Time is Post: Time then swings the audio they have changed, and their steps stay straight (Time moves
// their accents along with the hits). set() overwrites the Swing in Pump::Params, Shape::Params and
// Echo::Params with it, and gives Time the global Swing itself.
#include "common.h"
#include "echo.h"
#include "grid.h"
#include "pump.h"
#include "shape.h"
#include "time.h"

namespace gf {

class Groove {
public:
    enum Target : int { kAll, kDry, kEcho };
    enum Order : int { kEchoFirst, kEchoLast };
    enum Place : int { kPost, kPre };
    struct Params {
        bool pumpOn = false, shapeOn = false;
        Pump::Params pump;   int pumpTarget = kAll;
        Shape::Params shape; int shapeTarget = kAll;
        bool echoOn = false; Echo::Params echo;   int order = kEchoFirst;
        bool timeOn = false; Time::Params time;   int timePlace = kPost;
        Swing swing;         // the global Swing
        float inputDb = 0.0f, outputDb = 0.0f;   // -24..24
        float mix = 1.0f;                        // 0..1: the chain's output against its input
        float dry = 1.0f;                        // 0..1: the dry bus where it joins the wet (R29)
    };

    Groove();
    void reset();
    void set(const Params& p, const ef::Transport& t);
    void process(float* L, float* R, int n);
    int tailSamples() const;   // the longest ring of a part that is heard or fading
    int running() const { return running_; }   // parts run in the last process(): 0 when all four are off
    bool listening() const { return listening_; }   // Time's ring is being fed while Time is off (R42); not a part that runs

    // UI thread: the memory of the parts the parameters switch on (the Echo's lines, Time's ring), once.
    // False if there was none to be had; the part then stays silent. Call it before the audio thread sees
    // the switch; if it comes after, the part fades in when the memory has arrived.
    bool allocate(const Params& p);
    bool allocateEcho() { return echo_.allocate(); }
    bool allocateTime() { return time_.allocate(); }   // also for Moves, which play their patterns from Time's ring

    // Swing applied once along the path (docs/DESIGN.md "The signal path and combining", 4): the swing
    // Shape's and Pump's steps follow. Straight when Time swings the audio they have changed.
    static Swing stepSwing(const Params& p) {
        if (p.timeOn && p.time.groove == Time::kSwing && placeOf(p.timePlace) == kPost) {
            Swing s = p.swing;
            s.amount = 0.5f;
            return s;
        }
        return p.swing;
    }

private:
    // The order and the place as the chain takes them: out of range is the nearest.
    static int orderOf(int order) { return order > kEchoFirst ? kEchoLast : kEchoFirst; }
    static int placeOf(int place) { return place > kPost ? kPre : kPost; }

    // A part, or one instance of it, as the chain sees it.
    struct Part {
        int where = 0;          // where it works (Time's place); the parameter's, once it has faded out
        float fade = 0.0f;      // 0..1: the share of its output that is heard
        float goal = 0.0f;      // where the fade is going
        bool dirty = false;     // has run since it was last reset
        bool woke = false;      // comes on from rest in this chunk
        int age = 0;            // Time's: samples its ring has recorded since it was cleared (Time warms up)
        bool runs = false;      // runs in this chunk
        bool audible() const { return fade > 0.0f || goal > 0.0f; }
    };

    template <class M> bool arm(M& m, Part& s, bool want, int where);
    template <class M> void join(M& dry, const Part& dryPart, M& wet, Part& wetPart);
    void armTime(const Params& p, const ef::Transport& t);
    template <class F> void runPart(Part& s, float* L, float* R, int n, F&& process);
    void listen(float* L, float* R, int n);
    void makeWet(const float* L, const float* R, int n);

    Shape shape_, shapeWet_;    // the instances for the dry and for the wet
    Pump pump_, pumpWet_;
    Echo echo_;
    Time time_;
    Part shapePart_, shapeWetPart_, pumpPart_, pumpWetPart_, echoPart_, timePart_;
    ef::Ramp in_, out_, mix_, dry_;   // gains, the mix and the dry level, as linear factors
    bool fresh_ = true;         // the next set() jumps to its targets
    bool echoLast_ = false;     // the Echo comes after Shape and Pump in this chunk
    bool listening_ = false;    // Time is off, its ring is allocated, and it records the audio at its place
    int running_ = 0;
    float inputL_[ef::kChunk] = {}, inputR_[ef::kChunk] = {};   // the chain's input, for the mix
    float givenL_[ef::kChunk] = {}, givenR_[ef::kChunk] = {};   // what a fading part was given
    float keyL_[ef::kChunk] = {}, keyR_[ef::kChunk] = {};       // the key: the chain's input (after Time when Pre)
    float wetL_[ef::kChunk] = {}, wetR_[ef::kChunk] = {};       // the wet bus
};

} // namespace gf
