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
// samples while it crossfades into the new one, starting at that sample (a caller that wants such a change to end on a
// line starts it 5 ms earlier, with a chunk cut to the sample). A change that moves the playhead by less than half a
// frame needs no crossfade.
//
// With no pattern in range, a lag of 0 and nothing fading, process() leaves the input alone: bit for bit,
// while the ring records.
//
// Memory. allocate() (UI thread; the plugin calls it the first time Time is switched on) makes the ring and
// hands it over through an atomic pointer, as the looper does; until then process() passes its input through
// untouched. reset() forgets the recording (nothing is cleared: reads before what was recorded are silence).
// The position is MPC's song position while it plays, else a clock that runs on at the tempo from where the last
// chunk ended: a pattern's start is in beats on that clock (nextBeat() says where it is).
#include "common.h"

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

private:
    double slopeOf(int k) const;   // the segment k -> k + 1, from its start
    const TPoint* p_ = nullptr;
    int n_ = 0, i_ = 0;            // the segment: p_[i_].x <= x < p_[i_ + 1].x
    int jq_ = -1;                  // the first Jump's point after i_ (n_: none), or -1 for not looked for yet
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
    // range the lag asked for applies. Jumps crossfade over 5 ms and end on the line where they happen.
    void process(float* L, float* R, int n, const ef::Transport& t, int pattern, double patternStart, double lagBeats);

    // Where the next chunk starts on the clock a pattern's start is measured on while the transport is stopped.
    double nextBeat() const { return clock_; }

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
        float vol = 1.0f;                // the pattern's volume, kEdge samples ahead
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

    // The pattern bound, and what it says about its start.
    int pat_ = -1;
    double len_ = 0.0;
    bool loop_ = false;
    double s0_ = 0.0, d0_ = 1.0;    // the source at the start, and its slope going on
    TimeCurve src_, vol_;
    double tfb_ = 0.0, leadB_ = 0.0;   // the crossfade and the volume's look-ahead, in beats at this chunk's tempo

    // What the last chunk ended on, to tell a chunk that follows on from one that does not.
    bool fresh_ = true;
    int prevEff_ = -1;
    double prevStart_ = 0.0, prevLam_ = 0.0, prevSpb_ = 0.0, prevX_ = 0.0;
    double prevPos_ = 0.0;          // where the carrying head read at the last sample (frames)
    float prevSpeed_ = 1.0f;
    bool calm_ = true;              // the playhead is at now, the gain is 1, nothing is fading

    // The crossfade for a jump nothing foresaw: the old head plays on at its speed.
    double u_ = 0.0;
    float uSpeed_ = 1.0f;
    int fade_ = 0;

    float g_ = 1.0f;                // the gain
};

} // namespace gf
