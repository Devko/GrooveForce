#pragma once
// Time's engine (docs/DESIGN.md, "Time"): Flex Beat's and Gross Beat's time patterns at zero latency. A
// recorder ring holds the last 6 s or more of the input and the output is read from a playhead that is at
// now or behind it, never ahead, so nothing is delayed: a pattern that seems to need the future (double
// speed) plays a captured slice twice instead. The ring, the 4-point Hermite reads, the 5 ms raised-cosine
// crossfade and the level that fades with the speed are EffectForce's looper's (looper.cpp), put to a
// different use: its heads follow a pattern's curve instead of a loop's edges.
//
// The playhead. It reads `lag` beats behind now (a lag of 0 is the input itself). A pattern says where it
// reads by a source curve s(x), in beats from the pattern's start: s(x) = x is now, less is behind it, and it
// may go negative (audio from before the pattern began, as far back as the ring reaches). s(x) <= x always.
// The lag x - s(x) is turned into frames at the tempo, so the playhead stays on MPC's grid through
// a tempo change, a loop or a locate: everything is a function of the song position.
//
// The patterns are data: surface/surface.py holds TIME_PATTERNS (checked there) and writes
// surface/build/time_data.h, which dsp/time.cpp reads. A curve is a list of points (x, y, seg), y the
// source (beats) or the volume (0..1); a point's `seg` is the way to the next one:
//   Line    a constant speed
//   Smooth  a raised cosine (flat at both ends)
//   Jump    vertical: the next point has the same x and the curve jumps there. A curve is right-continuous:
//           at the x of a Jump it has the value after it.
//
// A source Jump is crossfaded, and the crossfade ENDS on the line. The curve is known ahead of time, so
// the new head starts fading in kFade (220 samples, 5 ms) before the line: it reads where the segment
// after the jump would have read, run back from the line (a Line backwards at its speed, a Smooth held
// at its start), while the old head plays on to the line. The weight is 0.5 + 0.5 cos(pi r), r the time left to
// the line over the 5 ms: at the line the old head is silent, so a hit on the grid line is never heard early.
// Neither head ever reads ahead of now: a lag that would go negative is 0. The same holds where a loopable
// pattern goes round (the next lap's first segment, run back) and where a one-shot ends (back to the lag
// asked for, at volume 1), whether or not the data says so with a last Jump.
//
// The volume is looked at kEdge samples ahead and the gain slews toward it by at most 1/kEdge a sample: every
// edge, a gate's included, takes 1 ms or more for a full scale (the suite's rule) and ends on its line. The
// gain also follows the speed, min(8 |speed|, 1) as the looper's: a tape stop ends in silence.
//
// Anything the curves cannot foresee is a different kind of jump: the lag asked for changes (a groove's
// swing), the pattern is changed or the song position locates or the tempo changes under a pattern. The playhead
// then lands somewhere else than it was heading, and the old head plays on at its last speed for kFade
// samples while it crossfades into the new one, starting at that sample (see process() for how a caller makes such a
// change end on a line). A change that moves the playhead by less than half a
// frame needs no crossfade.
//
// A jump inside a fade waits for it (at most 5 ms late). A crossfade is never restarted, which would step: if the
// playhead jumps again while one is going, the head goes on where it was heading until the fade is over, and the jump is
// made then, from there, however many came meanwhile. The same goes for a pattern's own crossfade (head 1 coming in over
// head 0, the 5 ms before a Jump or the end): a jump that lands in it, even one that leaves the head that goes on where it
// was (a cancel in the last Jump's slice, a locate by a whole slice), holds both heads, with the fade's weights, to the line.
// (A jump is told from the heads, not from the one that goes on alone: each head that is heard, before or now, has to be
// where it was heading.)
// A pattern's own jump is scheduled (its fade ends on its line) when it has 5 ms to itself, that is, when the line before
// it (the previous Jump, or the pattern's start) is 5 ms or more back; the library's are (its closest are 331 samples apart
// at 999 BPM, the fastest tempo). One that has not is made at its line, as a jump nothing foresaw, and waits like one; so
// does an end or a lap's wrap with a Jump less than 5 ms before it.
//
// With no pattern in range, a lag of 0 and nothing fading, process() leaves the input alone: bit for bit,
// while the ring records.
//
// Time, the part (class Time, below), is this engine with a groove: it decides the lag asked for, per sample, from the
// song position, and cuts each chunk where the lag has to change so that the change ENDS on its grid line.
//
// Memory. allocate() (UI thread; the plugin calls it the first time Time is switched on) makes the ring and
// hands it over through an atomic pointer, as the looper does; until then process() passes its input through
// untouched. reset() forgets the recording (nothing is cleared: reads before what was recorded are silence).
// The position is MPC's song position while it plays, else a clock that runs on at the tempo from where the last
// chunk ended: a pattern's start is in beats on that clock (nextBeat() says where it is).
#include "common.h"
#include "grid.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace gf {

enum class TSeg : uint8_t { Line, Smooth, Jump };   // to the next point; Jump: the curve jumps there (vertical, crossfaded, ending at x)
struct TPoint { float x, y; TSeg seg; };            // x in beats from the pattern's start; y: source (beats) or volume (0..1)
struct TimePatternDef {
    const char* name;
    float lengthBeats;
    bool loopable;                // goes round while it is asked for
    int srcFirst, srcCount;       // kTimePoints[srcFirst ..): the source curve
    int volFirst, volCount;       // and the volume curve
};

int numTimePatterns();
const char* timePatternName(int i);              // an index out of range (never from the surface) is the nearest pattern
const TimePatternDef& timePatternDef(int i);

// The evaluator: pure functions of a pattern and x (no state; the engine uses the same curves).
//   timeSourceAt    s(x), beats from the start (the first point's y before it, the last point's after the end)
//   timeSpeedAt     ds/dx, the speed the playhead moves at (frames per sample); the right-hand slope at a Jump
//   timeVolumeAt    the volume 0..1
//   timeNextJump    the first Jump in (x0, x1] in the pattern's data, else HUGE_VAL
double timeSourceAt(int pattern, double x);
double timeSpeedAt(int pattern, double x);
float timeVolumeAt(int pattern, double x);
double timeNextJump(int pattern, double x0, double x1);

// One curve and a cursor into it: x mostly grows from one call to the next, so the segment it was in is
// where to look. seek(x) first; value / slope / nextJump read the segment seek found.
class TimeCurve {
public:
    void bind(const TPoint* points, int count);
    void seek(double x);
    double value(double x) const;                                  // right-continuous
    double slope(double x) const;                                  // dy/dx going on from x
    // The first Jump after x: its x (HUGE_VAL if none), where it lands and the slope from there.
    double nextJump(double& landsAt, double& slopeAfter);
    // How far that Jump is from the line before it (the one before, or the start): the room a crossfade ending on it has.
    double nextGap() const { return gap_; }
    // Whether the last seek() went over a Jump (a line between the last x and this), and that line's gap.
    bool crossed() const { return crossed_; }
    double crossGap() const { return crossGap_; }

private:
    double slopeOf(int k) const;   // the segment k -> k + 1, from its start
    double gapOf(int jq) const;    // the Jump at point jq: its x less the x of the Jump before it, or of the start
    const TPoint* p_ = nullptr;
    int n_ = 0, i_ = 0;            // the segment: p_[i_].x <= x < p_[i_ + 1].x
    int jq_ = -1;                  // the first Jump's point after i_ (n_: none), or -1 for not looked for yet
    double gap_ = HUGE_VAL;        // of that Jump
    bool crossed_ = false;
    double crossGap_ = HUGE_VAL;
};

class TimeCore {
public:
    static constexpr int kRingFrames = 1 << 19;   // 11.9 s: the next power of two over 2 bars at 80 BPM (6 s)
    static constexpr int kFade = 220;             // a jump's crossfade, 5 ms
    static constexpr int kEdge = 48;              // the volume's quickest edge: a full scale in 48 samples, a hair over 1 ms
    static constexpr double kMaxLagBeats = 16.0;  // the lag asked for is held to this (the ring is shorter at slow tempos)

    TimeCore() = default;
    ~TimeCore();
    TimeCore(const TimeCore&) = delete;
    TimeCore& operator=(const TimeCore&) = delete;

    // The ring, once (UI thread). False if there is no memory for it: process() then passes its input through.
    bool allocate();
    bool allocated() const { return store_.load(std::memory_order_acquire) != nullptr; }

    void reset();

    // In place, any n >= 1. The playhead's source for this chunk: `lagBeats` behind now (>= 0), or, with
    // `pattern` >= 0, the pattern at x = (the song position - patternStart) beats from its start. A pattern
    // is in range from 5 ms before its start to its end (a loopable one for as long as it is asked for); out of
    // range the lag asked for applies. A pattern's own jumps crossfade over 5 ms and end on the line where they happen.
    //
    // For a caller (Time is the first):
    //  - A change of lagBeats between calls is not scheduled: it crossfades over the kFade samples from this call's
    //    first sample. To end on a line, start a call at line - kFade (cut the chunk there) with the new lag. If a
    //    crossfade is going when it comes, it (like any jump: a pattern change, a locate) waits for its end, at most 5 ms.
    //  - Keep passing `pattern` through the chunk that contains its end: the crossfade back to lagBeats is inside it.
    //    A loopable pattern repeats for as long as it is passed, so stop passing it at its length for a one shot.
    //  - patternStart is in the song position's beats while the transport plays, and in nextBeat()'s clock while it is
    //    stopped.
    void process(float* L, float* R, int n, const ef::Transport& t, int pattern, double patternStart, double lagBeats);

    // Where the next chunk starts on the clock a pattern's start is measured on while the transport is stopped.
    double nextBeat() const { return clock_; }

    // The patterns the core plays: the generated library, unless a test points it at its own (idle calls only; `defs` and
    // `points` outlive the use; nullptr goes back to the generated one). The surface checks the generated library;
    // this is how the engine is tried on curves the library would not let through.
    void useLibrary(const TimePatternDef* defs, int count, const TPoint* points);

private:
    struct Store {
        std::vector<float> ring;   // kRingFrames interleaved L R frames
    };
    // Where the heads read for one sample: head 0 plays, head 1 is fading in (w, 0 when it is not).
    struct Heads {
        double lag0 = 0.0, lag1 = 0.0;   // beats behind now
        float w = 0.0f;                  // head 1's weight
        float gain = 1.0f;               // the speed's: min(8 |speed|, 1), the two heads' alike
        float speed = 1.0f;              // frames a sample, of the head that carries on
        float speed0 = 1.0f;             // and of head 0
        double left = 0.0;               // samples to the line, while head 1 is fading in
        float vol = 1.0f;                // the pattern's volume, kEdge samples ahead
        bool cut = false;                // a line with no 5 ms of its own to fade in went by at this sample: the head may have jumped
    };

    void bind(int pattern);
    void headsAt(bool inPattern, double x, double lag, Heads& h);
    float volumeAt(double x);
    void read(double pos, int64_t last, float& l, float& r) const;
    const float* tap(int64_t a, int64_t last) const;

    std::atomic<Store*> store_{nullptr};
    const float* ring_ = nullptr;   // the audio thread's copy of the ring
    int64_t abs_ = 0;               // frames recorded since the ring was handed over
    int64_t written_ = 0;           // how many of the last ones count (reset() forgets them)
    int64_t oldest_ = 0;            // the first frame still to be read

    double clock_ = 0.0;            // the song position the next chunk starts at, if the transport does not say

    // The patterns: the generated ones unless a test brought its own.
    const TimePatternDef* defs_ = nullptr;
    const TPoint* points_ = nullptr;
    int numDefs_ = 0;

    // The pattern bound, and what it says about its start.
    int pat_ = -1;
    double len_ = 0.0;
    bool loop_ = false;
    double s0_ = 0.0, d0_ = 1.0;    // the source at the start, and its slope going on
    TimeCurve src_, vol_;
    double endGap_ = 0.0;           // how far the end is from the line before it (the last Jump, or the start)
    double tfb_ = 0.0, leadB_ = 0.0, bps_ = 0.0;   // the crossfade and the volume's look-ahead, and a sample, in beats at this chunk's tempo

    // What the last chunk ended on, to tell a chunk that follows on from one that does not.
    bool fresh_ = true;
    int prevEff_ = -1;
    double prevStart_ = 0.0, prevLam_ = 0.0, prevSpb_ = 0.0, prevX_ = 0.0;
    double prevPos_ = 0.0;          // where the carrying head read at the last sample (frames)
    float prevSpeed_ = 1.0f;
    double prev0Pos_ = 0.0;         // where head 0 read, its speed, and how many samples the pattern's own fade (head 1 in) had to go
    float prev0Speed_ = 1.0f;
    double prevLeft_ = 0.0;
    bool calm_ = true;              // the playhead is at now, the gain is 1, nothing is fading

    // The crossfade for a jump nothing foresaw: the old head plays on at its speed.
    double u_ = 0.0;
    float uSpeed_ = 1.0f;
    int fade_ = 0;
    // A jump that comes while a crossfade is going waits for its end: the head goes on where it was heading (hp_, at hs_)
    // for what is left of the fade, and then the jump is made, from there. A pattern's own crossfade, head 1 coming in over
    // head 0, is one: if the jump comes in it, both heads go on (hq_ is head 0, at hqs_) with the fade's weight, hl_ samples to the line.
    bool held_ = false;
    double hp_ = 0.0;
    float hs_ = 1.0f;
    double hq_ = 0.0;
    float hqs_ = 1.0f;
    double hl_ = 0.0;

    float g_ = 1.0f;                // the gain
};

// Time: the engine with its grooves (docs/DESIGN.md, "Time") and the Moves' patterns over them.
//
// Swing on the audio (ruling R25). Pump and Shape swing their steps; Time swings the audio itself, so straight
// hats groove as they do on an MPC. Each pair of units (the Swing's unit, 1/16 or 1/8: u beats) is [c, c + 2u) from a
// multiple of 2u, and the second unit starts at c + 2u * amount instead of c + u. Before the second unit's straight line
// c + u the lag is 0; from it, the lag is 2u * amount - u; at c + 2u it is 0 again. Each of those changes is a jump
// whose 5 ms crossfade ENDS on its line, so a hit on the line is never heard early, and no pitch bends: the hit that was
// on c + u comes out on c + 2u * amount (at 120 BPM on 1/16, 58% is 20 ms late and 66% is 40 ms).
//
// How a change ends on its line. TimeCore crossfades a changed lag over kFade samples from the first sample of the call
// that changes it. The lines are known ahead, so Time asks for each lag kFade samples early: the lag asked at a sample is the
// groove's at the position 5 ms on. A line that falls inside a chunk cuts the chunk there, as many pieces as there are
// lines in it (usually one), and the piece after the cut asks the new lag. A line is reached by the first sample at or
// after it (a thousandth of a sample's slack, so a line that falls exactly on a sample is that sample's).
// A change of lag the groove did not schedule (a knob, a groove switched, a pattern's end close behind another change)
// that comes while a crossfade is going waits for its end: TimeCore defers a jump inside a fade (see its header comment).
// The groove's lines are far enough apart not to wait for each other (a unit is 15 ms at the fastest tempo).
//
// A swing's late lag is fixed in samples when its unit first comes to be asked for (as Drunk's and Push's are, in ms), at
// the tempo of that moment: a tempo ramp inside the unit does not feed it sub-frame steps. It is the next late unit's to
// follow the tempo.
//
// Drunk: a lag per 16th, from a hash of the 16th's index in its bar (0..amount * 15 ms), so it is the same on every bar and
// wherever MPC loops; each change ends on its 16th's line. Push: a constant lag of amount * 20 ms. Off: no lag, and with
// no pattern the input passes bit for bit (the ring only records).
//
// A pattern (`pattern` >= 0) overrides the groove while it plays: the groove's lines inside it are not followed (the lag
// asked is held, and TimeCore's pre-roll starts from it), and its end crossfades back to the lag the groove has at that
// line. When the pattern ends off a line (after a one shot, or a stop), the groove stays off, the lag 0, until the next
// unit line, and resumes there; if the position has gone by the wait by then (it ran out, or an MPC loop took the song back
// before it), no pattern being asked for, it is over. A pattern that goes round by itself ends in a crossfade to the live input, not to the
// lag, so one that is not looping and ends on a line resumes on the line after. `loop` repeats the pattern until the
// caller stops asking (pattern -1): a loopable one by TimeCore's own wrap, any other as a new lap each time, the next lap's
// 5 ms pre-roll replacing the last lap's end.
//
// The song position is MPC's while it plays; stopped, the grid runs on at the tempo from where it was, seeded from
// MPC's position on the first chunk after reset() (as Pump and Shape do), and patternStart is on that clock.
//
// Memory is TimeCore's: allocate() on the UI thread; until then the input passes and nothing is read.
class Time {
public:
    enum Groove : int { kOff, kSwing, kDrunk, kPush };
    struct Params {
        int groove = kOff;
        float amount = 0.5f;        // Drunk: up to 15 ms late per 16th; Push: up to 20 ms late
        Swing swing;                // Swing's amount and unit (the global Swing)
        int pattern = -1;           // a Move's pattern, -1 none
        double patternStart = 0.0;  // beats where it started (a grid line)
        bool loop = false;          // repeat the pattern until told otherwise
    };

    static constexpr float kDrunkMs = 15.0f;    // Drunk's longest lag, at amount 1
    static constexpr float kPushMs = 20.0f;     // Push's, at amount 1
    static constexpr double kReachBeats = 8.0;  // how far back a pattern can read (the library holds to this): its tail

    Time();
    bool allocate() { return core_.allocate(); }
    bool allocated() const { return core_.allocated(); }

    void reset();
    void set(const Params& p, const ef::Transport& t);   // once per chunk, before process()
    void process(float* L, float* R, int n);             // in place; n >= 1, at most ef::kChunk as the contract says (any n works)

    // How long it sounds after the input stops: the lag of the groove, a pattern's reach back, and a crossfade.
    int tailSamples() const { return tail_; }

    // Where the next chunk starts on the clock a pattern's start is measured on while the transport is stopped.
    double nextBeat() const { return pos_; }

private:
    // One piece of a chunk: what TimeCore is asked for the next `m` samples.
    struct Piece {
        double lag;      // beats behind now
        int pattern;     // -1 none
        double start;    // the pattern's start (a lap's, when it goes round a lap at a time)
        int m;
    };

    Piece pieceAt(int i, int n);
    double baseLag(double q, double& until);     // the groove's lag, or none while it waits for a line
    double grooveLag(double q, double& until);   // the groove's lag at q beats (5 ms on) and where it next changes
    double resumeLine(double e, bool strict) const;
    void setZone(double e, bool strict);

    TimeCore core_;
    Params p_;
    double bpm_ = 120.0, bps_ = 120.0 / 60.0 / ef::kRate;   // the tempo; beats a sample
    double tfb_ = 0.0;           // the crossfade (5 ms), in beats
    double pos_ = 0.0;           // the song position of the next chunk's first sample
    double qBase_ = 0.0;         // pos_ + the crossfade + the slack: where the chunk's first sample asks the groove
    bool fresh_ = true;          // the next set() seeds the position from MPC's
    double lastLag_ = 0.0;       // the lag asked in the last piece, in samples (a tempo change leaves it be): held while a pattern plays

    // What ended a pattern: the groove waits (lag 0) from there to the next unit line.
    bool armed_ = false;         // the last set() had a pattern
    double armedEnd_ = 0.0;      // where it ends (HUGE_VAL looping)
    bool armedStrict_ = false;   // it goes round by itself, so its end is a crossfade to live
    bool zoneOn_ = false;
    double zFrom_ = 0.0, zTo_ = 0.0;

    // Swing's late lag, in samples, as it was when its unit first came to be asked for: a tempo ramp inside the unit does not move it.
    int64_t latchUnit_ = INT64_MIN;
    double latchNominal_ = 0.0, latchSamples_ = 0.0;

    int tail_ = 0;
};

} // namespace gf
