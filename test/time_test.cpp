// Time's engine and its patterns (dsp/time.h) against the data surface.py generates: the library (15
// patterns, every source curve at or behind now, the volume within 0..1, a one-shot ending where it
// rejoins now or in silence), the evaluator's laws, then the playhead: at rest it passes the input bit
// for bit while the ring records; a lag reads the past (an impulse 125 ms later); a jump's 5 ms
// raised-cosine crossfade ends exactly on its line; a stutter repeats its first slice, half-time
// plays an octave down, reverse plays the beat before backwards, a tape stop falls in pitch to
// silence and rejoins now; gates and the break open and close in 1 ms edges that end on their lines.
// Then the contract: no sample depends on a later one (zero latency), no click through any pattern, a
// locate or a loop, the same output whatever the chunk sizes, a stopped transport, nothing allocated
// yet, wild parameters, reset. Then Time (the engine with a groove): swing on the audio, Drunk and Push, a
// pattern played over a groove, loops; a click put on the grid is read back where it is heard (its centroid).
#include "signal.h"
#include "../dsp/time.h"
#include "time_data.h"   // generated: surface/build

#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace {

using namespace eft;
using ef::kChunk;
using ef::kRate;
using ef::Transport;
using gf::Time;
using gf::TimeCore;

constexpr double kBeat = 22050.0;                   // a beat at 120 BPM, in samples
constexpr int kFade = TimeCore::kFade;              // a jump's crossfade: 5 ms, 220 samples
constexpr int kEdge = TimeCore::kEdge;              // the volume's quickest edge: 48 samples, a hair over 1 ms
constexpr double kStep = 1.0 / (0.001 * 44100.0);   // the 1 ms rule: a full scale per sample at the quickest

int idx(const char* name) {
    for (int i = 0; i < gf::numTimePatterns(); ++i)
        if (std::string(gf::timePatternName(i)) == name) return i;
    std::printf("  no time pattern named %s\n", name);
    CHECK(false);
    return 0;
}

bool same(const Buf& a, const Buf& b) { return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0; }
size_t at(double beats) { return static_cast<size_t>(std::llround(beats * kBeat)); }
bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

using Beats = std::function<double(size_t)>;

// Plays samples [from, to) of L and R in place through `core`, in chunks of at most `chunk`. The
// chunk that starts at sample i is at song position beats(i); the tempo is bpm.
void playAt(TimeCore& core, Buf& L, Buf& R, size_t from, size_t to, int pattern, double start, double lag, double bpm,
            const Beats& beats, bool playing = true, int chunk = kChunk) {
    for (size_t i = from; i < to;) {
        const int n = static_cast<int>(std::min(static_cast<size_t>(chunk), to - i));
        Transport t;
        t.bpm = bpm;
        t.beats = beats(i);
        t.playing = playing;
        t.valid = true;
        core.process(&L[i], &R[i], n, t, pattern, start, lag);
        i += static_cast<size_t>(n);
    }
}
// At 120 BPM, playing from beat 0: sample i is at beat i / 22050.
void play(TimeCore& core, Buf& L, Buf& R, size_t from, size_t to, int pattern, double start, double lag = 0.0, int chunk = kChunk) {
    playAt(core, L, R, from, to, pattern, start, lag, 120.0, [](size_t i) { return static_cast<double>(i) / kBeat; }, true, chunk);
}
// A fresh, allocated TimeCore over `in` (left and right alike) for its whole length.
Buf render(const Buf& in, int pattern, double start, double lag = 0.0, int chunk = kChunk) {
    TimeCore c;
    CHECK(c.allocate());
    Buf L = in, R = in;
    play(c, L, R, 0, in.size(), pattern, start, lag, chunk);
    return L;
}

// The 4-point Hermite read of x at a fractional position, as the engine reads the ring.
float interp(const Buf& x, double pos) {
    const double fl = std::floor(pos);
    const long k = static_cast<long>(fl);
    const auto g = [&x](long i) { return i >= 0 && i < static_cast<long>(x.size()) ? x[static_cast<size_t>(i)] : 0.0f; };
    return ef::hermite(g(k - 1), g(k), g(k + 1), g(k + 2), static_cast<float>(pos - fl));
}

// Four tones that no slice of a bar repeats, under a slow swell: the material the slices are told apart on.
Buf mixture(int n, float amp = 0.1f) {
    Buf x(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const double t = i / static_cast<double>(kRate);
        double v = 0.0;
        for (double f : {313.0, 771.0, 1733.0, 2417.0}) v += std::sin(2.0 * kPi * f * t + f);
        x[static_cast<size_t>(i)] = amp * static_cast<float>(v * (0.6 + 0.4 * std::sin(2.0 * kPi * 1.7 * t + 0.3)));
    }
    return x;
}

double corr(const Buf& a, size_t a0, const Buf& b, size_t b0, size_t n) {
    double ab = 0.0, aa = 0.0, bb = 0.0;
    for (size_t i = 0; i < n; ++i) {
        ab += static_cast<double>(a[a0 + i]) * b[b0 + i];
        aa += static_cast<double>(a[a0 + i]) * a[a0 + i];
        bb += static_cast<double>(b[b0 + i]) * b[b0 + i];
    }
    return ab / std::sqrt(aa * bb + 1e-30);
}

// Rising zero crossings in x[from..to), as Hz.
double crossingHz(const Buf& x, size_t from, size_t to) {
    int n = 0;
    for (size_t i = from + 1; i < to; ++i)
        if (x[i - 1] < 0.0f && x[i] >= 0.0f) ++n;
    return n * kRate / static_cast<double>(to - from);
}

// The steepest speed of a pattern's source (frames per sample), from the data.
double topSpeed(int p) {
    const double len = gf::timePatternDef(p).lengthBeats;
    double v = 1.0;
    for (int k = 0; k < 4000; ++k) v = std::max(v, std::fabs(gf::timeSpeedAt(p, len * k / 4000.0)));
    return v;
}
// What a sine of amplitude 0.5 at `hz` may step by through a pattern: the playhead's speed times its own
// step, 10% for the Hermite reads, the crossfade (a raised cosine over 220 samples across two tones
// of 0.5: 0.5 pi / 220 a sample at most) and the gain's 1 ms edge (0.5 / 48 at most).
double clickBound(double hz, double speed) { return 1.1 * speed * 0.5 * 2.0 * kPi * hz / kRate + 0.02; }

// --- the library -----------------------------------------------------------------------------

void timeData() {      // the generated library: order, names, lengths, and the laws surface.py checks
    static const char* const names[] = {"Stutter 1/8", "Stutter 1/16", "Stutter 1/32", "Reverse Stutter", "Roll Build 1 Bar", "Roll Build 4 Bars",
                                        "Half-Time Bar", "Tape Stop", "Slow Down", "Reverse Beat", "Spin-Back", "Rewind",
                                        "Gate 1/16", "Gate 1/8", "Break Silence"};
    static const double lens[] = {4, 4, 4, 4, 4, 16, 4, 4, 4, 1, 0.5, 1, 4, 4, 4};
    static const bool loops[] = {true, true, true, true, false, false, false, false, false, false, false, false, true, true, false};
    CHECK(gf::numTimePatterns() == 15 && gf::kNumTimePatterns == 15);
    for (int p = 0; p < 15; ++p) {
        CHECK(std::string(gf::timePatternName(p)) == names[p]);
        CHECK(gf::timePatternDef(p).lengthBeats == static_cast<float>(lens[p]));
        CHECK(gf::timePatternDef(p).loopable == loops[p]);
        CHECK(std::string(gf::timePatternName(p)).size() <= 18);
        for (int q = p + 1; q < 15; ++q) CHECK(std::string(gf::timePatternName(p)) != gf::timePatternName(q));
    }
    // an index out of range (never from the surface) is the nearest pattern
    CHECK(std::string(gf::timePatternName(-4)) == "Stutter 1/8" && std::string(gf::timePatternName(99)) == "Break Silence");

    for (int p = 0; p < 15; ++p) {
        const gf::TimePatternDef& d = gf::timePatternDef(p);
        const double len = d.lengthBeats;
        bool behind = true, volume = true, ordered = true;
        // At 4000 places along it, and on both sides of every knot: never ahead of now, the volume within 0..1.
        const auto look = [&](double x) {
            if (!(gf::timeSourceAt(p, x) <= x + 1e-9)) behind = false;
            const float v = gf::timeVolumeAt(p, x);
            if (!(v >= 0.0f && v <= 1.0f)) volume = false;
        };
        for (int k = 0; k <= 4000; ++k) look(len * k / 4000.0);
        for (int c = 0; c < 2; ++c) {
            const int first = c ? d.volFirst : d.srcFirst, count = c ? d.volCount : d.srcCount;
            const gf::TPoint* pts = gf::kTimePoints + first;
            if (pts[0].x != 0.0f || pts[count - 1].x != d.lengthBeats) ordered = false;
            for (int i = 0; i < count; ++i) {
                look(pts[i].x);
                if (pts[i].x < len) look(pts[i].x + 1e-9);
                if (pts[i].x > 0.0f) look(pts[i].x - 1e-9);
                if (i && pts[i].x < pts[i - 1].x) ordered = false;
                // equal x only across a Jump (the vertical segment), and a Jump only across equal x
                if (i + 1 < count && (pts[i + 1].x == pts[i].x) != (pts[i].seg == gf::TSeg::Jump)) ordered = false;
            }
        }
        CHECK(gf::timeSourceAt(p, 0.0) <= 0.0);
        CHECK(behind);
        CHECK(volume);
        CHECK(ordered);
        // A one-shot rejoins now at its end, or is silent there.
        if (!d.loopable) CHECK(near(gf::timeSourceAt(p, len), len, 1e-9) || gf::timeVolumeAt(p, len) == 0.0f);
        if (!behind) std::printf("  pattern %s is ahead of now somewhere\n", gf::timePatternName(p));
    }
}

void timeEvaluator() { // the evaluator's laws on patterns whose curves are known by heart
    const int half = idx("Half-Time Bar"), st = idx("Stutter 1/16"), spin = idx("Spin-Back"), stop = idx("Tape Stop");
    const int rev = idx("Reverse Beat"), gate = idx("Gate 1/16"), brk = idx("Break Silence"), roll = idx("Roll Build 1 Bar");
    // a Line is a constant speed
    CHECK(near(gf::timeSourceAt(half, 2.0), 1.0, 1e-12) && near(gf::timeSpeedAt(half, 1.0), 0.5, 1e-12));
    // a Jump is right-continuous: the line is on the far side, and just before it the source is still running
    CHECK(near(gf::timeSourceAt(st, 0.2), 0.2, 1e-12) && near(gf::timeSourceAt(st, 0.24999999), 0.24999999, 1e-9));
    CHECK(gf::timeSourceAt(st, 0.25) == 0.0 && near(gf::timeSourceAt(st, 0.3), 0.05, 1e-12));
    CHECK(near(gf::timeSpeedAt(st, 0.3), 1.0, 1e-12));
    // the next Jump within (x0, x1]: strictly after x0, up to and including x1
    CHECK(near(gf::timeNextJump(st, 0.0, 4.0), 0.25, 1e-12) && near(gf::timeNextJump(st, 0.25, 4.0), 0.5, 1e-12));
    CHECK(near(gf::timeNextJump(st, 0.0, 0.25), 0.25, 1e-12) && gf::timeNextJump(st, 0.0, 0.2499) > 1e300);
    CHECK(gf::timeNextJump(half, 0.0, 3.99) > 1e300 && near(gf::timeNextJump(half, 0.0, 4.0), 4.0, 1e-12));
    CHECK(near(gf::timeNextJump(rev, 0.0, 1.0), 1.0, 1e-12) && gf::timeNextJump(rev, 1.0, 5.0) > 1e300);
    CHECK(near(gf::timeNextJump(roll, 1.5, 4.0), 2.0, 1e-12) && near(gf::timeNextJump(roll, 2.0, 4.0), 2.5, 1e-12));
    // a Smooth segment is a raised cosine: flat at both ends, the middle halfway, its speed a sine (no library pattern has one
    // now: a curve of its own, bound as the evaluator binds the library's)
    {
        const gf::TPoint pts[] = {{0.0f, 0.0f, gf::TSeg::Smooth}, {0.5f, -1.0f, gf::TSeg::Line}};
        gf::TimeCurve c;
        c.bind(pts, 2);
        c.seek(0.25);
        CHECK(near(c.value(0.25), -0.5, 1e-9) && near(c.value(0.5), -1.0, 1e-9));
        CHECK(near(c.slope(0.0), 0.0, 1e-9) && near(c.slope(0.25), -kPi, 1e-6));
        c.seek(0.1);
        CHECK(near(c.value(0.1), -0.5 * (1.0 - std::cos(kPi * 0.2)), 1e-9));
    }
    // the spin-back is already going backwards at its first sample (speed -1: a speed that started at 0 would mute it, the level
    // follows the speed), faster and faster to -3, a beat back at its end
    CHECK(near(gf::timeSpeedAt(spin, 0.0), -1.0, 0.03) && near(gf::timeSpeedAt(spin, 0.49), -3.0, 0.1));   // 48 lines on the parabola
    CHECK(near(gf::timeSourceAt(spin, 0.5), -1.0, 1e-9) && near(gf::timeSourceAt(spin, 0.25), -0.375, 1e-3));
    // the tape stop: the speed 1 - x / 4, the source x - x^2 / 8
    CHECK(near(gf::timeSourceAt(stop, 2.0), 1.5, 1e-9) && near(gf::timeSourceAt(stop, 4.0), 2.0, 1e-9));
    CHECK(near(gf::timeSpeedAt(stop, 2.0), 0.5, 0.01) && near(gf::timeSpeedAt(stop, 3.9), 0.025, 0.01));
    // the volume: the gate is 1 for 60% of each 16th and 0 for the rest, steps made of Jumps
    CHECK(gf::timeVolumeAt(gate, 0.1) == 1.0f && gf::timeVolumeAt(gate, 0.16) == 0.0f && gf::timeVolumeAt(gate, 0.26) == 1.0f);
    CHECK(gf::timeVolumeAt(gate, 0.15f) == 0.0f);   // the line itself is on the far side
    CHECK(gf::timeVolumeAt(brk, 1.0) == 0.0f && gf::timeVolumeAt(st, 1.0) == 1.0f);
    // before the start and past the end: the first and the last point's value
    CHECK(gf::timeSourceAt(rev, -1.0) == 0.0 && near(gf::timeSourceAt(rev, 3.0), 1.0, 1e-12));
}

// The crossfade's weight comes from a table (a cosine of a double in every sample of a fade was the Move's costliest line):
// the raised cosine to 1.2e-7 (a float's own rounding is 6e-8 at 1), falling from 1 to 0, held to 0..1.
void timeFadeWeights() {
    double worst = 0.0;
    bool falling = true;
    float prev = 2.0f;
    for (int k = 0; k <= 100000; ++k) {
        const double r = k / 100000.0;
        const float w = gf::timeFadeWeight(r);
        worst = std::max(worst, std::fabs(static_cast<double>(w) - (0.5 + 0.5 * std::cos(kPi * r))));
        falling = falling && w <= prev + 1e-7f;
        prev = w;
    }
    CHECK(worst < 1.2e-7);
    CHECK(falling);
    CHECK(gf::timeFadeWeight(0.0) == 1.0f && gf::timeFadeWeight(1.0) == 0.0f && near(gf::timeFadeWeight(0.5), 0.5, 1e-7));
    CHECK(gf::timeFadeWeight(-0.5) == 1.0f && gf::timeFadeWeight(7.0) == 0.0f && gf::timeFadeWeight(std::nan("")) == 1.0f);
}

// A curve evaluated as the engine did before it kept the numbers of the segment it stands on: a division at every call.
double refValue(const gf::TPoint* p, int n, double x) {
    if (!(x >= p[0].x)) return p[0].y;
    int i = 0;
    while (i + 2 < n && x >= p[i + 1].x) ++i;
    const gf::TPoint &a = p[i], &b = p[i + 1];
    if (x >= b.x) return b.y;
    if (!(x > a.x)) return a.y;
    return a.y + (static_cast<double>(b.y) - a.y) * ((x - a.x) / (static_cast<double>(b.x) - a.x));
}
double refSlope(const gf::TPoint* p, int n, double x) {
    int i = 0;   // (before the start the cursor stands on the first segment)
    while (i + 2 < n && x >= p[i + 1].x) ++i;
    const gf::TPoint &a = p[i], &b = p[i + 1];
    if (x >= b.x || !(b.x > a.x)) return 0.0;
    return a.seg == gf::TSeg::Line ? (static_cast<double>(b.y) - a.y) / (static_cast<double>(b.x) - a.x) : 0.0;
}

// The numbers a curve keeps for its segment (its width's reciprocal, its slope, the Jump it heads for) give what dividing at every call
// gave, to rounding, for every pattern of the library, going forward and jumping about: a locate sends the cursor back.
void timeCurveNumbers() {
    uint32_t seed = 7u;
    const auto rnd = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<double>(seed >> 8) / 16777216.0;
    };
    double worst = 0.0, worstSlope = 0.0;
    for (int p = 0; p < gf::numTimePatterns(); ++p) {
        const gf::TimePatternDef& d = gf::timePatternDef(p);
        for (int vol = 0; vol < 2; ++vol) {
            const gf::TPoint* pts = gf::kTimePoints + (vol ? d.volFirst : d.srcFirst);
            const int n = vol ? d.volCount : d.srcCount;
            gf::TimeCurve c;
            c.bind(pts, n);
            const double len = d.lengthBeats;
            double x = -0.05;
            for (int k = 0; k < 4000; ++k) {
                x = k % 7 == 6 ? (rnd() * 1.2 - 0.1) * len : x + len / 3000.0;   // forward, with a locate now and then
                c.seek(x);
                worst = std::max(worst, std::fabs(c.value(x) - refValue(pts, n, x)));
                if (!vol) worstSlope = std::max(worstSlope, std::fabs(c.slope(x) - refSlope(pts, n, x)));
            }
        }
    }
    CHECK(worst < 1e-12);
    CHECK(worstSlope < 1e-12);
}

// What a sample finds in the run an earlier sample left (the formulas of its segment, ready) is what it would have found by looking it
// up: sample for sample, bit for bit. Binding the pattern again before every sample leaves no run to find.
void timeRuns() {
    for (const char* name : {"Roll Build 1 Bar", "Stutter 1/16", "Reverse Beat", "Gate 1/16", "Tape Stop", "Spin-Back"})
        for (const double bpm : {120.0, 174.0}) {
            const int p = idx(name);
            const double spb = kRate * 60.0 / bpm;
            const Buf in = mixture(static_cast<int>(1.6 * spb));
            const auto render1 = [&](bool lookEach) {
                TimeCore c;
                CHECK(c.allocate());
                Buf L = in, R = in;
                for (size_t i = 0; i < in.size(); ++i) {
                    if (lookEach) c.useLibrary(gf::kTimePatterns, gf::kNumTimePatterns, gf::kTimePoints);
                    Transport t;
                    t.bpm = bpm;
                    t.beats = static_cast<double>(i) / spb;
                    t.playing = t.valid = true;
                    c.process(&L[i], &R[i], 1, t, p, 0.5, 0.0);
                }
                return L;
            };
            CHECK(same(render1(false), render1(true)));
        }
}

// --- the playhead ------------------------------------------------------------------------------

void timeRest() {      // no pattern, lag 0: output == input bit for bit, whatever the chunks; and the ring records
    for (int chunk : {32, 7, 1, 128}) {
        TimeCore c;
        CHECK(c.allocate());
        Buf in = whiteNoise(30000, 0.8f, 5), inR = whiteNoise(30000, 0.8f, 6);
        Buf L = in, R = inR;
        play(c, L, R, 0, 30000, -1, 0.0, 0.0, chunk);
        CHECK(same(L, in) && same(R, inR));
    }
    // while it only recorded, the past was kept: a lag after it reads that
    TimeCore c;
    CHECK(c.allocate());
    const Buf in = mixture(60000, 0.3f);
    Buf L = in, R = in;
    play(c, L, R, 0, 30016, -1, 0.0, 0.0);
    CHECK(std::memcmp(L.data(), in.data(), 30016 * sizeof(float)) == 0);
    play(c, L, R, 30016, 60000, -1, 0.0, 0.25);   // 1/16 at 120 BPM: 5512.5 samples
    bool ok = true;
    for (size_t i = 30016 + kFade + 40; i < 60000; ++i)
        if (!near(L[i], interp(in, static_cast<double>(i) - 5512.5), 2e-6)) ok = false;
    CHECK(ok);
    // left and right are read alike
    CHECK(same(L, R));
}

void timeLag() {       // lag 1/16 at 120 BPM: an impulse 125 ms later (+-1 sample); a change of lag is crossfaded
    {
        Buf in(40000, 0.0f);
        in[20000] = 1.0f;
        const Buf out = render(in, -1, 0.0, 0.25);
        size_t peak = 0;
        for (size_t i = 0; i < out.size(); ++i)
            if (std::fabs(out[i]) > std::fabs(out[peak])) peak = i;
        CHECK(peak == 25512 || peak == 25513);
        // 5512.5 samples: half a sample between 25512 and 25513, read with the Hermite weights at 1/2
        CHECK(near(out[25512], 0.5625, 1e-6) && near(out[25513], 0.5625, 1e-6));
        CHECK(near(out[25511], -0.0625, 1e-6) && near(out[25514], -0.0625, 1e-6));
        bool silent = true;
        for (size_t i = 0; i < 25510; ++i)
            if (out[i] != 0.0f) silent = false;
        CHECK(silent);
        // the tempo sets the lag: 1/16 at 60 BPM is 250 ms
        Buf slow = in, slowR = in;
        TimeCore c;
        CHECK(c.allocate());
        playAt(c, slow, slowR, 0, in.size(), -1, 0.0, 0.25, 60.0, [](size_t i) { return static_cast<double>(i) / (kRate * 60.0 / 60.0); });
        size_t p2 = 0;
        for (size_t i = 0; i < slow.size(); ++i)
            if (std::fabs(slow[i]) > std::fabs(slow[p2])) p2 = i;
        CHECK(p2 == 31025 || p2 == 31024);   // 20000 + 11025
    }
    {   // the switch from 0 to 1/16: a sine whose delayed copy is out of phase, crossfaded over 5 ms
        const double f = 457.0;
        const Buf in = sine(f, 80000, 0.5f);
        TimeCore c;
        CHECK(c.allocate());
        Buf L = in, R = in;
        const size_t sw = 937 * 32;   // a chunk edge
        play(c, L, R, 0, sw, -1, 0.0, 0.0);
        play(c, L, R, sw, 80000, -1, 0.0, 0.25);
        CHECK(std::memcmp(L.data(), in.data(), sw * sizeof(float)) == 0);
        const double bound = clickBound(f, 1.0);
        CHECK(maxStep(L, 2, 80000) <= bound);
        if (!(maxStep(L, 2, 80000) <= bound)) std::printf("  a step of %.4f, the bound %.4f\n", maxStep(L, 2, 80000), bound);
        // it did cross over: at the end of the 5 ms the delayed copy plays alone, and the first sample still the old head
        CHECK(near(L[sw + kFade + 1], interp(in, static_cast<double>(sw + kFade + 1) - 5512.5), 1e-5));
        CHECK(near(L[sw], in[sw], 1e-6));
        // the delayed copy is far from the live one here (5512.5 samples is 57.12 cycles of 457 Hz)
        CHECK(std::fabs(in[sw + 100] - interp(in, static_cast<double>(sw + 100) - 5512.5)) > 0.1);
        // and back again
        Buf L2 = in, R2 = in;
        TimeCore c2;
        CHECK(c2.allocate());
        play(c2, L2, R2, 0, sw, -1, 0.0, 0.25);
        play(c2, L2, R2, sw, 80000, -1, 0.0, 0.0);
        CHECK(maxStep(L2, 5600, 80000) <= bound);
        CHECK(near(L2[sw + kFade + 40], in[sw + kFade + 40], 1e-6));
    }
}

void timeJumpEndsOnLine() { // Reverse Beat's jump at x = 1 beat: the 5 ms crossfade ends exactly on the beat
    const int p = idx("Reverse Beat");
    const double start = 3.0;
    const size_t S = at(start), line = S + at(1.0);
    const Buf in = whiteNoise(static_cast<int>(line + 3000), 0.5f, 21);
    const Buf out = render(in, p, start);
    // before the pattern: the input; from its start the beat before, backwards (whole samples: exact)
    bool before = true, back = true, win = true, after = true;
    for (size_t i = 0; i < S - 2; ++i)
        if (out[i] != in[i]) before = false;
    for (size_t i = S; i + kFade < line; ++i)
        if (!near(out[i], in[2 * S - i], 1e-6)) back = false;
    // across the 220 samples before the line: (1 - w) the reversed head + w the live one, w a raised cosine that is 0 at the
    // first and 1 at the line
    for (size_t i = line - kFade; i < line; ++i) {
        const double rem = static_cast<double>(line - i) / kFade;
        const double w = 0.5 + 0.5 * std::cos(kPi * rem);
        const double want = (1.0 - w) * in[2 * S - i] + w * in[i];
        if (!near(out[i], want, 3e-6)) win = false;
    }
    // from the line, the old head is silent: the live input and nothing else
    for (size_t i = line; i < line + 3000; ++i)
        if (!near(out[i], in[i], 1e-6)) after = false;
    CHECK(before);
    CHECK(back);
    CHECK(win);
    CHECK(after);
    // the first sample of the crossfade is the old head alone, the last before the line nearly the new one alone
    CHECK(near(out[line - kFade], in[2 * S - (line - kFade)], 1e-6));
    CHECK(std::fabs(out[line - 1] - in[line - 1]) < 0.02 * std::fabs(in[2 * S - (line - 1)] - in[line - 1]) + 1e-5);
}

void timeStutter() {   // each slice of the bar repeats the bar's first slice
    for (int which = 0; which < 2; ++which) {
        const int p = idx(which ? "Stutter 1/8" : "Stutter 1/16");
        const double d = which ? 11025.0 : 5512.5;   // a slice in samples
        const int slices = which ? 8 : 16;
        const double start = 3.0;
        const size_t S = at(start);
        const Buf in = mixture(static_cast<int>(S + 4 * kBeat + 3000));
        const Buf out = render(in, p, start);
        const size_t len = static_cast<size_t>(d);
        // the first slice is the live bar
        bool live = true;
        for (size_t i = S; i + kFade + 2 < S + len; ++i)   // (not the 5 ms before its end: that crossfades into the repeat)
            if (!near(out[i], in[i], 1e-5)) live = false;
        CHECK(live);
        double liveCorr = 0.0;
        for (int k = 1; k < slices; ++k) {
            const double line = static_cast<double>(S) + k * d;
            const size_t from = static_cast<size_t>(std::ceil(line)) + 2, to = static_cast<size_t>(std::floor(line + d)) - kFade - 2;
            // against the first slice itself, where the slice is a whole number of samples
            if (k % 2 == 0 || which == 1) {
                const double c = corr(out, from, out, S + (from - static_cast<size_t>(line)), to - from);
                CHECK(c > 0.99);
                if (!(c > 0.99)) std::printf("  %s slice %d: correlation %.4f\n", gf::timePatternName(p), k, c);
            }
            // and exactly: the input read k slices behind
            bool exact = true;
            for (size_t i = from; i < to; ++i)
                if (!near(out[i], interp(in, static_cast<double>(i) - k * d), 3e-5)) exact = false;
            CHECK(exact);
            liveCorr += corr(in, from, in, S + (from - static_cast<size_t>(line)), to - from);
        }
        // the slices are not the live input over again: the live bar's own slices are unlike the first
        CHECK(liveCorr / (slices - 1) < 0.6);
    }
    {   // the 1/32 buzz: eight 1/32 slices to the 1/4
        const int p = idx("Stutter 1/32");
        const size_t S = at(2.0);
        const Buf in = mixture(static_cast<int>(S + 4 * kBeat + 3000));
        const Buf out = render(in, p, 2.0);
        bool exact = true;
        const double d = 2756.25;
        for (int k = 1; k < 32; ++k) {
            const double line = static_cast<double>(S) + k * d;
            for (size_t i = static_cast<size_t>(std::ceil(line)) + 2; i < static_cast<size_t>(std::floor(line + d)) - kFade - 2; ++i)
                if (!near(out[i], interp(in, static_cast<double>(i) - k * d), 3e-5)) exact = false;
        }
        CHECK(exact);
    }
}

void timeLoop() {      // a loopable pattern goes on while it is asked for: bar 2 stutters on bar 2's own first slice
    const int p = idx("Stutter 1/16");
    const size_t S = at(2.0), S2 = S + at(4.0);
    const Buf in = mixture(static_cast<int>(S2 + at(4.0) + 3000));
    const Buf out = render(in, p, 2.0);
    bool live = true, exact = true;
    for (size_t i = S2 + 4; i + kFade + 2 < S2 + 5512; ++i)
        if (!near(out[i], in[i], 1e-5)) live = false;
    for (int k = 1; k < 16; ++k) {
        const double line = static_cast<double>(S2) + k * 5512.5;
        for (size_t i = static_cast<size_t>(std::ceil(line)) + 2; i < static_cast<size_t>(std::floor(line + 5512.5)) - kFade - 2; ++i)
            if (!near(out[i], interp(in, static_cast<double>(i) - k * 5512.5), 3e-5)) exact = false;
    }
    CHECK(live);
    CHECK(exact);
}

void timeReverseStutter() { // Reverse Stutter: the first 1/8 live, then backwards over and over; each jump lands in a backward run
    const int p = idx("Reverse Stutter");
    const size_t S = at(3.0), d = at(0.5);
    const Buf in = whiteNoise(static_cast<int>(S + 4 * d * 2 + 3000), 0.5f, 77);
    const Buf out = render(in, p, 3.0);
    bool live = true, back = true, win = true;
    for (size_t i = S; i + kFade + 2 < S + d; ++i)
        if (!near(out[i], in[i], 1e-6)) live = false;
    // slice k (1 .. 7) runs from the slice's end down to its start: position 2 S + d - (i - S - k d) ... in whole samples
    for (size_t k = 1; k < 8; ++k) {
        const size_t line = S + k * d;
        for (size_t i = line + (k > 1 ? 2 : 0); i + kFade + 2 < line + d; ++i)
            if (!near(out[i], in[2 * S + (k + 1) * d - i], 1e-6)) back = false;
        // the 5 ms before the next jump: the slice still running back, and the next one run back from its line (it lands
        // at the slice's end, going down at speed -1, so before the line it was a little higher still)
        if (k < 7)
            for (size_t i = line + d - kFade; i < line + d; ++i) {
                const double w = 0.5 + 0.5 * std::cos(kPi * static_cast<double>(line + d - i) / kFade);
                const double want = (1.0 - w) * in[2 * S + (k + 1) * d - i] + w * in[2 * S + (k + 2) * d - i];
                if (!near(out[i], want, 3e-6)) win = false;
            }
    }
    CHECK(live);
    CHECK(back);
    CHECK(win);
}

void timeHalfTime() {  // Half-Time Bar: a 1 kHz tone plays at 500 Hz, for the bar, then rejoins now
    const int p = idx("Half-Time Bar");
    const size_t S = at(3.0), E = S + at(4.0);
    const Buf in = sine(1000.0, static_cast<int>(E + 20000), 0.5f);
    const Buf out = render(in, p, 3.0);
    const double hz = crossingHz(out, S + 2000, E - 2000);
    CHECK(std::fabs(hz - 500.0) <= 2.0);
    if (!(std::fabs(hz - 500.0) <= 2.0)) std::printf("  half-time: %.2f Hz\n", hz);
    CHECK(magnitude(out, 500.0, S + 4000, E - 3000) > 0.45);
    CHECK(magnitude(out, 1000.0, S + 4000, E - 3000) < 0.02);
    // before: the input as it was; after: the live tone again, rejoined without a click
    for (size_t i = 0; i + 300 < S; i += 97) CHECK(out[i] == in[i]);
    CHECK(near(crossingHz(out, E + 2000, E + 18000), 1000.0, 3.0));
    for (size_t i = E + 4; i < E + 8000; i += 31) CHECK(near(out[i], in[i], 1e-6));
    CHECK(maxStep(out, 2, out.size()) <= clickBound(1000.0, 1.0));
}

void timeReverse() {   // Reverse Beat: the ramp of the beat before comes out falling, then the live input again
    const int p = idx("Reverse Beat");
    const size_t S = at(3.0), B = at(1.0);
    Buf in(S + 3 * B, 1.0f);
    for (size_t i = 0; i < S; ++i) in[i] = i + B >= S ? static_cast<float>(i + B - S) / static_cast<float>(B) : 0.0f;   // rises through the beat before
    const Buf out = render(in, p, 3.0);
    bool falling = true, matches = true;
    for (size_t j = 0; j + kFade + 2 < B; ++j) {
        if (!near(out[S + j], 1.0 - static_cast<double>(j) / B, 1e-5)) matches = false;
        if (j && out[S + j] > out[S + j - 1] + 1e-6f) falling = false;
    }
    CHECK(matches);
    CHECK(falling);
    CHECK(out[S] > 0.99f && out[S + B - kFade - 4] < 0.02f);
    // the beat after it: the live input (the 1.0 held)
    for (size_t i = S + B + 2; i < S + 2 * B; i += 17) CHECK(near(out[i], 1.0, 1e-6));
}

void timeTapeStop() {  // Tape Stop: the pitch falls continuously, silent at the end, then rejoins now with no click
    const int p = idx("Tape Stop");
    const size_t S = at(3.0), E = S + at(4.0);
    const Buf in = sine(1000.0, static_cast<int>(E + 30000), 0.5f);
    const Buf out = render(in, p, 3.0);
    double last = 1e9;
    for (double xc : {0.5, 1.0, 1.5, 2.0, 2.5, 3.0}) {
        const size_t c = S + at(xc);
        const double hz = crossingHz(out, c - 4096, c + 4096), want = 1000.0 * (1.0 - xc / 4.0);
        CHECK(std::fabs(hz - want) < 0.03 * want + 6.0);
        if (!(std::fabs(hz - want) < 0.03 * want + 6.0)) std::printf("  tape stop at %.1f: %.1f Hz, wanted %.1f\n", xc, hz, want);
        CHECK(hz < last);   // it only ever falls
        last = hz;
    }
    // silent at the end: the last 2000 samples fade, the last 400 are nearly nothing
    CHECK(peak(out, E - 2000, E - 60) < 0.02f);
    CHECK(peak(out, E - 441, E - 60) < 0.003f);
    // then it rejoins now: from the line on, the live tone, and no click anywhere
    bool live = true;
    for (size_t i = E + 3; i < E + 20000; ++i)
        if (!near(out[i], in[i], 1e-6)) live = false;
    CHECK(live);
    CHECK(maxStep(out, 2, out.size()) <= clickBound(1000.0, 1.0));
    for (size_t i = 0; i + 300 < S; i += 89) CHECK(out[i] == in[i]);
}

void timeSlowDown() {  // Slow Down: the speed falls to a half over the bar, then the bar rejoins now
    const int p = idx("Slow Down");
    const size_t S = at(3.0), E = S + at(4.0);
    const Buf in = sine(1000.0, static_cast<int>(E + 20000), 0.5f);
    const Buf out = render(in, p, 3.0);
    CHECK(near(crossingHz(out, S + at(0.2) - 2048, S + at(0.2) + 2048), 1000.0 * (1.0 - 0.2 / 8.0), 25.0));   // speed 1 - x / 8
    CHECK(near(crossingHz(out, E - at(0.5) - 2048, E - at(0.5) + 2048), 1000.0 * (1.0 - 3.5 / 8.0), 25.0));
    CHECK(near(crossingHz(out, E - at(0.2) - 2048, E - at(0.2) + 2048), 1000.0 * (1.0 - 3.8 / 8.0), 25.0));
    CHECK(peak(out, E - 6000, E - 100) > 0.45f);   // it keeps its level: no fade at half speed
    for (size_t i = E + 3; i < E + 8000; i += 13) CHECK(near(out[i], in[i], 1e-6));
    CHECK(maxStep(out, 2, out.size()) <= clickBound(1000.0, 1.0));
}

void timeSpinRewind() {// Spin-Back and Rewind play backwards, then rejoin
    {   // Rewind: two beats of the past in one, backwards (a falling ramp from a rising one)
        const int p = idx("Rewind");
        const size_t S = at(3.0), B = at(1.0);
        Buf in(S + 3 * B, 1.0f);
        for (size_t i = 0; i < S; ++i) in[i] = i + 2 * B >= S ? static_cast<float>(i + 2 * B - S) / static_cast<float>(2 * B) : 0.0f;
        const Buf out = render(in, p, 3.0);
        bool ok = true;
        for (size_t j = 0; j + kFade + 2 < B; ++j)
            if (!near(out[S + j], 1.0 - 2.0 * static_cast<double>(j) / (2.0 * B), 1e-5)) ok = false;   // at twice the speed
        CHECK(ok);
        for (size_t i = S + B + 2; i < S + 2 * B; i += 19) CHECK(near(out[i], 1.0, 1e-6));
    }
    {   // Spin-Back: backwards from the first sample and faster, fading out; silent at the end
        const int p = idx("Spin-Back");
        const size_t S = at(3.0), H = at(0.5);
        const Buf in = sine(300.0, static_cast<int>(S + 3 * H), 0.5f);
        const Buf out = render(in, p, 3.0);
        const Buf flat(S + 3 * H, 0.5f);                              // it sounds at once: no hole at the start (the level
        const Buf held = render(flat, p, 3.0);                        // follows the speed: from 0 it would have taken ~8 ms)
        CHECK(held[S] > 0.49f && held[S + 20] > 0.49f && held[S + 200] > 0.49f);
        CHECK(peak(out, S + at(0.05), S + at(0.15)) > 0.2f);          // it sounds
        CHECK(peak(out, S + H - 300, S + H - 60) < 0.02f);            // and fades out
        CHECK(maxStep(out, 2, out.size()) <= clickBound(300.0, topSpeed(p)));
        // the playhead went back: the tone in the middle is higher than 300 Hz (speed 1.6 to 2.4 there)
        CHECK(crossingHz(out, S + at(0.15), S + at(0.35)) > 450.0);
    }
}

// --- gates and the break -------------------------------------------------------------------------

void timeGate() {      // Gate 1/16: 60% open of each 16th, edges of 1 ms that end on their lines
    for (int which = 0; which < 2; ++which) {
        const int p = idx(which ? "Gate 1/8" : "Gate 1/16");
        const double step = which ? 11025.0 : 5512.5;
        const int steps = which ? 8 : 16;
        const size_t S = at(3.0);
        const Buf in(S + at(4.0) + 4000, 0.5f);
        const Buf out = render(in, p, 3.0);
        bool shape = true;
        for (int k = 0; k < steps; ++k) {
            const double closeAt = S + k * step + 0.6 * step, openAt = S + (k + 1) * step;
            // open before the closing edge's 48 samples, closed after the line
            if (!near(out[static_cast<size_t>(std::floor(closeAt)) - kEdge - 3], 0.5, 1e-6)) shape = false;
            if (!near(out[static_cast<size_t>(std::floor(closeAt)) + 2], 0.0, 1e-6)) shape = false;
            // closed before the opening edge, open on the line (the last opening is the bar's end, back to the live input)
            if (!near(out[static_cast<size_t>(std::floor(openAt)) - kEdge - 3], 0.0, 1e-6)) shape = false;
            if (!near(out[static_cast<size_t>(std::floor(openAt)) + 2], 0.5, 1e-6)) shape = false;
            // on the line itself the edge has just finished: at most a step short
            if (!(out[static_cast<size_t>(std::ceil(closeAt))] <= 0.5 * kStep * 1.01)) shape = false;
            if (!(out[static_cast<size_t>(std::ceil(openAt))] >= 0.5 - 0.5 * kStep * 1.01)) shape = false;
        }
        CHECK(shape);
        // the edges are the 1 ms rule: no steeper, and as steep as it allows
        const double top = maxStep(out, 2, out.size());
        CHECK(top <= 0.5 * kStep + 1e-6);
        CHECK(top >= 0.5 * (1.0 / kEdge) * 0.95);
    }
}

void timeBreak() {     // Break Silence: the volume 0 for the bar, closing on the start and opening on the end
    const int p = idx("Break Silence");
    const size_t S = at(3.0), E = S + at(4.0);
    const Buf in(E + 4000, 0.5f);
    const Buf out = render(in, p, 3.0);
    CHECK(near(out[S - kEdge - 3], 0.5, 1e-6));
    CHECK(near(out[S + 2], 0.0, 1e-6) && near(out[S + 1000], 0.0, 1e-6) && near(out[E - kEdge - 3], 0.0, 1e-6));
    CHECK(near(out[E + 2], 0.5, 1e-6) && near(out[E + 2000], 0.5, 1e-6));
    CHECK(out[S] <= 0.5f * static_cast<float>(kStep) * 1.01f && out[E] >= 0.5f - 0.5f * static_cast<float>(kStep) * 1.01f);
    CHECK(maxStep(out, 2, out.size()) <= 0.5 * kStep + 1e-6);
}

void timeRolls() {     // Roll Build: slices that shorten, each repeating its first, then the live input
    {
        const int p = idx("Roll Build 1 Bar");
        const size_t S = at(3.0), E = S + at(4.0);
        const Buf in = mixture(static_cast<int>(E + 4000));
        const Buf out = render(in, p, 3.0);
        struct Phase { double start, end, d; };
        bool ok = true;
        for (const Phase& ph : {Phase{0, 2, 1}, Phase{2, 3, 0.5}, Phase{3, 3.5, 0.25}, Phase{3.5, 4, 0.125}}) {
            const int n = static_cast<int>(std::lround((ph.end - ph.start) / ph.d));
            for (int k = 1; k < n; ++k) {   // slice k of the phase: the phase's first slice, k slices behind
                const double line = static_cast<double>(S) + (ph.start + k * ph.d) * kBeat, back = k * ph.d * kBeat;
                for (size_t i = static_cast<size_t>(std::ceil(line)) + 2; i + kFade + 2 < static_cast<size_t>(std::floor(line + ph.d * kBeat)); ++i)
                    if (!near(out[i], interp(in, static_cast<double>(i) - back), 3e-5)) ok = false;
            }
            // the phase's first slice is live
            for (size_t i = S + at(ph.start) + 2; i + kFade + 2 < S + at(ph.start + ph.d) && i < E; ++i)
                if (!near(out[i], in[i], 1e-5)) ok = false;
        }
        CHECK(ok);
        for (size_t i = E + 3; i < E + 4000; i += 7) CHECK(near(out[i], in[i], 1e-6));
    }
    {   // four bars: 1/4, 1/8, 1/16, 1/32, a division to the bar
        const int p = idx("Roll Build 4 Bars");
        const size_t S = at(2.0), E = S + at(16.0);
        const Buf in = mixture(static_cast<int>(E + 4000));
        const Buf out = render(in, p, 2.0);
        bool ok = true;
        for (int bar = 0; bar < 4; ++bar) {
            const double d = 1.0 / (1 << bar);   // beats
            for (int k = 1; k < static_cast<int>(4.0 / d); ++k) {
                const double line = static_cast<double>(S) + (4.0 * bar + k * d) * kBeat, back = k * d * kBeat;
                for (size_t i = static_cast<size_t>(std::ceil(line)) + 2; i + kFade + 2 < static_cast<size_t>(std::floor(line + d * kBeat)); ++i)
                    if (!near(out[i], interp(in, static_cast<double>(i) - back), 3e-5)) ok = false;
            }
        }
        CHECK(ok);
        for (size_t i = E + 3; i < E + 4000; i += 7) CHECK(near(out[i], in[i], 1e-6));
    }
}

// --- the contract ----------------------------------------------------------------------------------

void timeNeverAhead() {// zero latency: no output sample depends on a later input sample, in any pattern
    const double start = 0.1;
    const size_t S = at(start);
    for (int p = 0; p < gf::numTimePatterns(); ++p) {
        const size_t len = at(gf::timePatternDef(p).lengthBeats);
        const Buf in = mixture(static_cast<int>(S + len + 4000), 0.4f);
        bool causal = true, shows = false;
        // near the start (a live slice), near the end (the rejoin's crossfade), and after the end
        for (size_t M : {S + 100, S + len / 2, S + len - 3, S + len + 100}) {
            Buf a = in, b = in;
            b[M] += 3.0f;   // a burst the later output may carry but the earlier must not
            Buf aR = a, bR = b;
            TimeCore ca, cb;
            CHECK(ca.allocate() && cb.allocate());
            play(ca, a, aR, 0, M + 2000, p, start);
            play(cb, b, bR, 0, M + 2000, p, start);
            for (size_t i = 0; i < M; ++i)
                if (a[i] != b[i]) causal = false;
            if (M == S + len + 100 && a[M] != b[M]) shows = true;   // and the test can see it: live after the end
        }
        CHECK(causal);
        CHECK(shows);
        if (!causal) std::printf("  pattern %s reads ahead of now\n", gf::timePatternName(p));
    }
}

void timeNoClicks() {  // every pattern on a sine: no step beyond the playhead's own speed and the edges
    const double hz = 440.0;
    for (int p = 0; p < gf::numTimePatterns(); ++p) {
        const gf::TimePatternDef& d = gf::timePatternDef(p);
        const size_t S = at(3.0), len = at(d.lengthBeats), total = S + (d.loopable ? 2 : 1) * len + at(1.0);   // a loop goes round twice
        const Buf in = sine(hz, static_cast<int>(total), 0.5f);
        const Buf out = render(in, p, 3.0);
        const double top = maxStep(out, 2, out.size()), bound = clickBound(hz, topSpeed(p));
        CHECK(top <= bound);
        CHECK(allFinite(out));
        if (!(top <= bound)) std::printf("  %s: a step of %.4f, the bound is %.4f\n", gf::timePatternName(p), top, bound);
    }
}

void timeLocate() {    // the song position jumps mid-pattern: the old head plays on and crossfades, no click, back at now
    const int p = idx("Tape Stop");
    const double hz = 1000.0;
    const size_t S = at(3.0);
    const Buf in = sine(hz, static_cast<int>(S + at(9.0)), 0.5f);
    for (int run = 0; run < 3; ++run) {
        TimeCore c;
        CHECK(c.allocate());
        Buf L = in, R = in;
        const size_t at2 = S + at(2.0);
        const double jump = run == 0 ? -1.5 : (run == 1 ? 3.0 : -10.0);   // back into the pattern; past its end; before its start
        playAt(c, L, R, 0, L.size(), p, 3.0, 0.0, 120.0, [&](size_t i) { return static_cast<double>(i) / kBeat + (i >= at2 ? jump : 0.0); });
        CHECK(allFinite(L));
        CHECK(maxStep(L, 2, L.size()) <= clickBound(hz, 1.0));
        if (!(maxStep(L, 2, L.size()) <= clickBound(hz, 1.0))) std::printf("  locate %d: a step of %.4f\n", run, maxStep(L, 2, L.size()));
        // it is the live input again by the end
        bool live = true;
        for (size_t i = L.size() - 3000; i < L.size(); ++i)
            if (!near(L[i], in[i], 1e-5)) live = false;
        CHECK(live);
    }
    {   // a tempo change mid-pattern: finite, no click, back to live
        TimeCore c;
        CHECK(c.allocate());
        Buf L = in, R = in;
        double beats = 0.0;
        for (size_t i = 0; i < L.size(); i += kChunk) {
            Transport t;
            t.bpm = i < at(5.0) ? 120.0 : 128.0;
            t.beats = beats;
            t.playing = t.valid = true;
            c.process(&L[i], &R[i], static_cast<int>(std::min<size_t>(kChunk, L.size() - i)), t, p, 3.0, 0.0);
            beats += kChunk * t.bpm / 60.0 / kRate;
        }
        CHECK(allFinite(L));
        CHECK(maxStep(L, 2, L.size()) <= clickBound(hz, 1.0));
    }
}

void timeChunking() {  // the same output whatever the chunks the host's block is cut into
    // (A start that is not a rational number of samples: a knot of a curve that falls exactly on a sample is
    // decided by the last bit of the song position, and chunks cut that position differently.)
    const double bpm = 133.0, start = 3.00137;
    const Buf in = mixture(120000);
    const auto beats = [bpm](size_t i) { return 1.0 + static_cast<double>(i) * bpm / 60.0 / kRate; };
    for (const char* name : {"Stutter 1/16", "Roll Build 1 Bar", "Tape Stop", "Reverse Beat", "Gate 1/16"}) {
        const int p = idx(name);
        Buf ref = in, refR = in;
        TimeCore c;
        CHECK(c.allocate());
        playAt(c, ref, refR, 0, ref.size(), p, start, 0.0, bpm, beats);
        for (int chunk : {1, 5, 31, 64, 128}) {
            TimeCore c2;
            CHECK(c2.allocate());
            Buf L = in, R = in;
            playAt(c2, L, R, 0, L.size(), p, start, 0.0, bpm, beats, true, chunk);
            double worst = 0.0;
            for (size_t i = 0; i < L.size(); ++i) worst = std::max(worst, static_cast<double>(std::fabs(L[i] - ref[i])));
            CHECK(worst < 1e-4);
            if (!(worst < 1e-4)) std::printf("  %s in chunks of %d: differs by %g\n", name, chunk, worst);
        }
    }
}

void timeStopped() {   // stopped, the clock runs on at the tempo: a pattern anchored on it plays
    const int p = idx("Reverse Beat");
    const size_t B = at(1.0);
    const Buf in = whiteNoise(static_cast<int>(3 * B), 0.5f, 33);
    for (int valid = 0; valid < 2; ++valid) {
        TimeCore c;
        CHECK(c.allocate());
        Buf L = in, R = in;
        for (size_t i = 0; i < L.size(); i += kChunk) {
            Transport t;
            t.bpm = 120.0;
            t.beats = 123.456;   // nonsense: not playing, so not used
            t.playing = false;
            t.valid = valid != 0;
            c.process(&L[i], &R[i], static_cast<int>(std::min<size_t>(kChunk, L.size() - i)), t, p, 1.0, 0.0);
        }
        bool ok = true;
        for (size_t j = 0; j + kFade + 2 < B; ++j)
            if (!near(L[B + j], in[B - j], 1e-6)) ok = false;
        CHECK(ok);
        CHECK(near(c.nextBeat(), static_cast<double>(L.size()) / kBeat, 1e-6));
    }
    {   // playing and valid: MPC's song position wins, wherever the free clock was
        TimeCore c;
        CHECK(c.allocate());
        Buf L = in, R = in;
        playAt(c, L, R, 0, L.size(), p, 1000.0 + 1.0, 0.0, 120.0, [](size_t i) { return 1000.0 + static_cast<double>(i) / kBeat; });
        bool ok = true;
        for (size_t j = 0; j + kFade + 2 < B; ++j)
            if (!near(L[B + j], in[B - j], 1e-6)) ok = false;
        CHECK(ok);
    }
}

void timeAllocate() {  // before allocate(): the input passes untouched, no reads; after it, an empty past is silence
    TimeCore c;
    CHECK(!c.allocated());
    const Buf in = whiteNoise(20000, 0.5f, 3);
    Buf L = in, R = in;
    play(c, L, R, 0, 10000, idx("Stutter 1/16"), 0.0, 0.25);   // a pattern and a lag asked for, nothing to read from
    CHECK(std::memcmp(L.data(), in.data(), 10000 * sizeof(float)) == 0);
    CHECK(!c.allocated());
    CHECK(c.allocate() && c.allocated() && c.allocate());
    play(c, L, R, 10000, 20000, -1, 0.0, 0.25);   // the ring starts empty at the hand-over
    bool silent = true, delayed = true;
    for (size_t i = 10000; i < 10000 + 5500; ++i)
        if (L[i] != 0.0f) silent = false;
    for (size_t i = 10000 + 5600; i < 20000; ++i)
        if (!near(L[i], interp(in, static_cast<double>(i) - 5512.5), 2e-6)) delayed = false;
    CHECK(silent);
    CHECK(delayed);
}

void timeReset() {     // reset() forgets the recording and any pattern in flight: the next chunk starts clean
    TimeCore c;
    CHECK(c.allocate());
    const Buf in = mixture(40000, 0.3f);
    Buf L = in, R = in;
    play(c, L, R, 0, 20000, idx("Tape Stop"), 0.1);
    c.reset();
    L = in;
    R = in;
    play(c, L, R, 20000, 40000, -1, 0.0, 0.0);   // idle again: bit for bit
    CHECK(std::memcmp(L.data() + 20000, in.data() + 20000, 20000 * sizeof(float)) == 0);
    Buf M = in, MR = in;
    TimeCore d;
    CHECK(d.allocate());
    play(d, M, MR, 0, 20000, -1, 0.0, 0.0);
    c.reset();
    d.reset();
    Buf a = in, aR = in, b = in, bR = in;
    play(c, a, aR, 0, 40000, -1, 0.0, 0.25);
    play(d, b, bR, 0, 40000, -1, 0.0, 0.25);
    CHECK(same(a, b));
    bool silent = true;
    for (size_t i = 0; i < 5500; ++i)
        if (a[i] != 0.0f) silent = false;
    CHECK(silent);   // nothing of the old past is left to read
}

void timeRobust() {    // wild parameters and transports: finite, in range, nothing undefined (UBSan)
    TimeCore c;
    CHECK(c.allocate());
    uint32_t seed = 12345;
    const auto rnd = [&seed]() { return ef::xorshift(seed); };
    const double bpms[] = {120.0, std::nan(""), 0.0, -5.0, 1.0, 19.0, 20.0, 133.7, 999.0, 5000.0, 1e9, HUGE_VAL, -HUGE_VAL};
    const double songs[] = {0.0, 17.25, std::nan(""), HUGE_VAL, -HUGE_VAL, -1e9, 1e12, 1e300, 3.0};
    const double starts[] = {0.0, 1.0, 17.0, std::nan(""), HUGE_VAL, -HUGE_VAL, 1e300, -1e300, 1e15, -1e15, 16.99};
    const double lags[] = {0.0, 0.25, std::nan(""), -1.0, 1e9, HUGE_VAL, 2.0, 15.999};
    const int pats[] = {-1, -7, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 99, 2147483647, -2147483647 - 1};
    Buf L = whiteNoise(400000, 0.8f, 9), R = whiteNoise(400000, 0.8f, 10);
    L[1000] = 0.8f;
    bool finite = true;
    double top = 0.0;
    double song = 0.0;
    for (size_t i = 0; i < L.size();) {
        if (rnd() % 8 == 0) song = songs[rnd() % 9];
        else if (std::isfinite(song) && std::fabs(song) < 1e9) song += 0.01;
        Transport t;
        t.bpm = bpms[rnd() % 13];
        t.beats = song;
        t.playing = rnd() % 4 != 0;
        t.valid = rnd() % 8 != 0;
        const int n = 1 + static_cast<int>(rnd() % 128);
        const size_t m = std::min(static_cast<size_t>(n), L.size() - i);
        const int pat = pats[rnd() % 20];
        const double start = rnd() % 3 ? (std::isfinite(song) ? song - 0.37 : 0.0) : starts[rnd() % 11];
        c.process(&L[i], &R[i], static_cast<int>(m), t, pat, start, lags[rnd() % 8]);
        for (size_t k = i; k < i + m; ++k) {
            if (!std::isfinite(L[k]) || !std::isfinite(R[k])) finite = false;
            top = std::max(top, static_cast<double>(std::max(std::fabs(L[k]), std::fabs(R[k]))));
        }
        i += m;
    }
    CHECK(finite);
    CHECK(top < 3.0);   // Hermite overshoot and a crossfade of two heads, nothing near a runaway
    // a NaN or infinity in the input never gets into the ring: later reads are finite
    TimeCore d;
    CHECK(d.allocate());
    Buf a = whiteNoise(40000, 0.5f, 1), aR = a;
    a[500] = std::nan("");
    aR[700] = HUGE_VAL;
    Buf b = a, bR = aR;
    play(d, b, bR, 0, 40000, -1, 0.0, 0.25);
    bool ok = true;
    for (size_t i = 0; i < 40000; ++i)
        if (!std::isfinite(b[i]) || !std::isfinite(bR[i])) ok = false;
    CHECK(ok);
}

// --- a jump inside a fade waits for it ---------------------------------------------------------------

// Two jumps in a row must not step: the second waits for the fade the first started (at most 5 ms late), then fades from where the
// head was held. The tone is 458 Hz: half a beat at 120 BPM (11025 samples) is 114.5 cycles of it, so the heads that are an odd number
// of half beats apart are in antiphase, and a fade restarted from the wrong place steps by nearly the tone's whole swing.
void timeJumpInFade() {
    const double hz = 458.0;
    const size_t S0 = 60000;
    const Buf in = sine(hz, 120000, 0.5f);
    const double bound = clickBound(hz, 1.0);
    for (size_t d : {static_cast<size_t>(44), static_cast<size_t>(88), static_cast<size_t>(176)}) {   // 1, 2 and 4 ms into the fade
        {   // a lag asked for (half a beat: in antiphase with the live input) starts a fade; a pattern comes in d samples later
            const int p = idx("Half-Time Bar");
            TimeCore c;
            CHECK(c.allocate());
            Buf L = in, R = in;
            play(c, L, R, 0, S0, -1, 0.0, 0.0);
            play(c, L, R, S0, S0 + d, -1, 0.0, 0.5);
            // the pattern is at x = 3 beats (a lag of 1.5 beats) from here, at beat S0 + d
            const double start = static_cast<double>(S0 + d) / kBeat - 3.0;
            play(c, L, R, S0 + d, 80000, p, start, 0.5);
            const double top = maxStep(L, S0 - 2, 80000);
            CHECK(top <= bound);
            if (!(top <= bound)) std::printf("  a pattern d=%zu into a fade: a step of %.4f, the bound %.4f\n", d, top, bound);
            // while the first fade runs the second jump is not heard: the output is the first fade's, the live input to the lag, to its end
            bool waited = true;
            for (size_t i = S0; i < S0 + kFade; ++i) {
                const double live = 0.5 + 0.5 * std::cos(kPi * static_cast<double>(i - S0) / kFade);   // the old head's share
                if (!near(L[i], live * in[i] + (1.0 - live) * interp(in, static_cast<double>(i) - 0.5 * kBeat), 3e-6)) waited = false;
            }
            CHECK(waited);
            // it is late by no more than 5 ms: the lag asked is the pattern's head (lag x / 2) from 2 fades on, i.e. from S0 + 440
            bool pure = true;
            for (size_t i = S0 + 2 * kFade + 3; i < 79000; ++i) {
                const double x = 3.0 + static_cast<double>(i - (S0 + d)) / kBeat;
                if (!near(L[i], interp(in, static_cast<double>(i) - 0.5 * x * kBeat), 2e-5)) pure = false;
            }
            CHECK(pure);
        }
        {   // a locate (a beat on) starts a fade in a pattern; another beat on, d samples later
            const int p = idx("Half-Time Bar");
            TimeCore c;
            CHECK(c.allocate());
            Buf L = in, R = in;
            const double start = static_cast<double>(S0) / kBeat - 0.5;   // x is half a beat at S0 before the locates, 1.5 and 2.5 after
            const Beats beats = [&](size_t i) { return static_cast<double>(i) / kBeat + (i >= S0 ? 1.0 : 0.0) + (i >= S0 + d ? 1.0 : 0.0); };
            playAt(c, L, R, 0, S0, p, start, 0.0, 120.0, beats);
            playAt(c, L, R, S0, S0 + d, p, start, 0.0, 120.0, beats);
            playAt(c, L, R, S0 + d, 80000, p, start, 0.0, 120.0, beats);
            const double top = maxStep(L, S0 - 2, 80000);
            CHECK(top <= bound);
            if (!(top <= bound)) std::printf("  a locate d=%zu into a fade: a step of %.4f, the bound %.4f\n", d, top, bound);
            // and the first fade's own, as if the second locate were not: the old head on its way, the new where the first locate put it
            bool waited = true;
            for (size_t i = S0; i < S0 + kFade; ++i) {
                const double old = 0.5 + 0.5 * std::cos(kPi * static_cast<double>(i - S0) / kFade);
                const double headOld = static_cast<double>(i) - 0.5 * (static_cast<double>(i) / kBeat - start) * kBeat;
                const double headNew = static_cast<double>(i) - 0.5 * (static_cast<double>(i) / kBeat + 1.0 - start) * kBeat;
                if (!near(L[i], old * interp(in, headOld) + (1.0 - old) * interp(in, headNew), 3e-6)) waited = false;
            }
            CHECK(waited);
            bool pure = true;
            for (size_t i = S0 + 2 * kFade + 3; i < 79000; ++i)
                if (!near(L[i], interp(in, static_cast<double>(i) - 0.5 * (beats(i) - start) * kBeat), 2e-5)) pure = false;
            CHECK(pure);
        }
    }
    {   // three in a row, and a fade longer than they are: the last one wins, and nothing steps
        const int p = idx("Half-Time Bar");
        TimeCore c;
        CHECK(c.allocate());
        Buf L = in, R = in;
        const double start = static_cast<double>(S0) / kBeat - 0.5;
        const Beats beats = [&](size_t i) { return static_cast<double>(i) / kBeat + (i >= S0 ? 1.0 : 0.0) + (i >= S0 + 30 ? 1.0 : 0.0) + (i >= S0 + 70 ? 1.0 : 0.0); };
        playAt(c, L, R, 0, S0, p, start, 0.0, 120.0, beats);
        playAt(c, L, R, S0, S0 + 30, p, start, 0.0, 120.0, beats);
        playAt(c, L, R, S0 + 30, S0 + 70, p, start, 0.0, 120.0, beats);
        playAt(c, L, R, S0 + 70, 80000, p, start, 0.0, 120.0, beats);
        CHECK(maxStep(L, S0 - 2, 80000) <= bound);
        CHECK(allFinite(L));
    }
}

// A pattern's own crossfade is a fade too: the 5 ms before one of its Jumps (or its end) the output is a blend of two heads, and a jump that lands in it
// (the pattern cancelled or switched, a locate or a nudge, a tempo change) must hold both to the line, then make its jump from there. The step after the
// event is held against the playing signal's own steepness: the input's, 1.1 times it, with the room of two crossfades at once.
enum class Hit { None, Cancel, Switch, Locate, Nudge, Tempo };

size_t around(double lineSample, int off) { return static_cast<size_t>(static_cast<long>(lineSample) + off); }

// Samples from a line to hit at: every `step`th from before its 5 ms fade to after it, and every one at the fade's first and last samples and the line's.
std::vector<int> sweepOffsets(int step) {
    std::vector<int> v;
    for (int off = -260; off <= 30; ++off)
        if ((off + 260) % step == 0 || (off >= -226 && off <= -212) || (off >= -6 && off <= 6)) v.push_back(off);
    return v;
}

// Plays `pat` (from beat `start`) over `in` from `lead` beats before it starts, in chunks of 32, with one thing done at sample `hitAt` (a call begins
// there), up to 700 samples after it.
Buf playHit(const Buf& in, int pat, bool loop, double start, double lead, size_t hitAt, Hit hit) {
    const double spb = 22050.0;
    size_t i = static_cast<size_t>((start - lead) * spb);
    Time time;
    if (!time.allocate()) CHECK(false);   // (not a check a trial: there are thousands)
    Buf L = in, R = in;
    double beats = static_cast<double>(i) / spb, bpm = 120.0;
    Time::Params p;
    p.pattern = pat;
    p.patternStart = start;
    p.loop = loop;
    const size_t end = hitAt + 700;
    while (i < end) {
        size_t n = std::min<size_t>(kChunk, end - i);
        if (i < hitAt) n = std::min(n, hitAt - i);
        if (i == hitAt) {
            switch (hit) {
            case Hit::None: break;
            case Hit::Cancel: p.pattern = -1; break;
            case Hit::Switch: p.pattern = idx("Half-Time Bar"); p.patternStart = beats; p.loop = false; break;
            case Hit::Locate: beats += 0.5; break;
            case Hit::Nudge: beats += 0.004; break;   // 88 samples: the same slice, the line that much nearer
            case Hit::Tempo: bpm = 150.0; break;
            }
        }
        Transport t;
        t.bpm = bpm;
        t.beats = beats;
        t.playing = t.valid = true;
        time.set(p, t);
        time.process(&L[i], &R[i], static_cast<int>(n));
        beats += static_cast<double>(n) * bpm / 60.0 / kRate;
        i += n;
    }
    L.resize(end);
    return L;
}

// The largest step from two samples before the hit to the end.
double stepAfter(const Buf& in, int pat, bool loop, double start, double lead, size_t hitAt, Hit hit) {
    return maxStep(playHit(in, pat, loop, start, lead, hitAt, hit), hitAt - 2, hitAt + 700);
}

void timeJumpInScheduledFade() { // a cancel, a switch, a locate, a nudge and a tempo change across the 5 ms before a Jump, and before a one shot's end
    const double start = 2.0;
    const char* const names[] = {"Stutter 1/8", "Stutter 1/16", "Stutter 1/32", "Reverse Stutter", "Roll Build 1 Bar"};
    const Hit hits[] = {Hit::Cancel, Hit::Switch, Hit::Locate, Hit::Nudge, Hit::Tempo};
    const auto firstLine = [](int p) {   // the first Jump a tenth of a beat in or later, in beats from the start
        const gf::TimePatternDef& d = gf::timePatternDef(p);
        for (int k = 0; k < d.srcCount; ++k)
            if (gf::kTimePoints[d.srcFirst + k].seg == gf::TSeg::Jump && gf::kTimePoints[d.srcFirst + k].x >= 0.1) return static_cast<double>(gf::kTimePoints[d.srcFirst + k].x);
        return 0.0;
    };
    {   // broadband: five tones, so the heads of different lags differ by about as much as they can
        Buf in(static_cast<size_t>(5 * kBeat));
        for (size_t i = 0; i < in.size(); ++i) {
            double v = 0.0;
            for (double f : {300.0, 523.0, 811.0, 1217.0, 1777.0}) v += 0.1 * std::sin(2.0 * kPi * f * static_cast<double>(i) / kRate + f);
            in[i] = static_cast<float>(v);
        }
        const double bound = 1.1 * maxStep(in, 1, in.size()) + 0.04;
        for (const char* name : names) {
            const int p = idx(name);
            const double line = firstLine(p);
            CHECK(line > 0.0);
            const double lineSample = (start + line) * kBeat;
            for (const Hit hit : hits) {
                double worst = 0.0;
                int worstOff = 0;
                for (const int off : sweepOffsets(5)) {   // before the fade, in it from its first sample to its last, and after the line
                    const double s = stepAfter(in, p, true, start, 0.1, around(lineSample, off), hit);
                    if (s > worst) {
                        worst = s;
                        worstOff = off;
                    }
                }
                CHECK(worst <= bound);
                if (!(worst <= bound))
                    std::printf("  %s, hit %d: a step of %.4f at %+d samples from the line, the bound %.4f\n", name, static_cast<int>(hit), worst, worstOff, bound);
            }
            // And the jump waits: what is heard from the hit to the line is the fade as it would have gone, both heads on their way and its weights
            // (a Line goes on as it was going; nothing but the timing of the jump was changed).
            bool held = true;
            for (const int off : {-200, -120, -40}) {
                const size_t hitAt = around(lineSample, off);
                const Buf ref = playHit(in, p, true, start, 0.1, hitAt, Hit::None);
                for (const Hit hit : hits) {
                    const Buf out = playHit(in, p, true, start, 0.1, hitAt, hit);
                    for (size_t i = hitAt; i + 2 < static_cast<size_t>(lineSample); ++i)
                        if (!near(out[i], ref[i], 2e-5)) held = false;
                }
            }
            CHECK(held);
            if (!held) std::printf("  %s: a jump in its fade changed what is heard before the line\n", name);
        }
    }
    {   // a tone whose heads are in antiphase: Stutter 1/32's slice (2756.25 samples) is 29.5 cycles of 472 Hz
        const double hz = 472.0;
        const Buf in = sine(hz, static_cast<int>(5 * kBeat), 0.5f);
        const double bound = clickBound(hz, 1.0);
        const double lineSample = (start + 0.125) * kBeat;
        for (const Hit hit : hits) {
            double worst = 0.0;
            for (const int off : sweepOffsets(6))
                worst = std::max(worst, stepAfter(in, idx("Stutter 1/32"), true, start, 0.1, around(lineSample, off), hit));
            CHECK(worst <= bound);
            if (!(worst <= bound)) std::printf("  antiphase, hit %d: a step of %.4f, the bound %.4f\n", static_cast<int>(hit), worst, bound);
        }
    }
    {   // a one shot's end is the same kind of line: Reverse Beat, across the 5 ms before its end
        const double hz = 472.0;
        const Buf in = sine(hz, static_cast<int>(5 * kBeat), 0.5f);
        const double bound = clickBound(hz, 1.0);
        const double lineSample = (start + 1.0) * kBeat;
        for (const Hit hit : {Hit::Cancel, Hit::Locate, Hit::Nudge, Hit::Tempo}) {
            double worst = 0.0;
            for (const int off : sweepOffsets(6))
                worst = std::max(worst, stepAfter(in, idx("Reverse Beat"), false, start, 1.1, around(lineSample, off), hit));
            CHECK(worst <= bound);
            if (!(worst <= bound)) std::printf("  the end of Reverse Beat, hit %d: a step of %.4f, the bound %.4f\n", static_cast<int>(hit), worst, bound);
        }
    }
}

// Patterns whose lines are closer than the library lets any be (the library's own are 331 samples apart at the fastest tempo, 999 BPM):
// a jump with no 5 ms to itself since the line before it (or the pattern's start) is not scheduled but made at its line, and waits
// if a fade is going. Four of them, at 120 BPM (a beat is 22050 samples, g is 60 of them, h is 80).
void timeCloseJumps() {
    using gf::TPoint;
    using gf::TSeg;
    const float g = 60.0f / 22050.0f, h = 80.0f / 22050.0f;
    static const TPoint pts[] = {
        // 0 Close Jumps, 2 beats: live for half a beat, then three jumps 60 samples apart, each back to an earlier place, then at speed 1
        {0.0f, 0.0f, TSeg::Line}, {0.5f, 0.5f, TSeg::Jump}, {0.5f, 0.2f, TSeg::Line}, {0.5f + g, 0.2f + g, TSeg::Jump},
        {0.5f + g, 0.0f, TSeg::Line}, {0.5f + 2 * g, g, TSeg::Jump}, {0.5f + 2 * g, 0.35f, TSeg::Line}, {2.0f, 1.85f - 2 * g, TSeg::Line},
        {0.0f, 1.0f, TSeg::Line}, {2.0f, 1.0f, TSeg::Line},
        // 1 Jump Before End, 1 beat: a jump 80 samples before the end
        {0.0f, 0.0f, TSeg::Line}, {1.0f - h, 1.0f - h, TSeg::Jump}, {1.0f - h, 0.3f, TSeg::Line}, {1.0f, 0.3f + h, TSeg::Line},
        {0.0f, 1.0f, TSeg::Line}, {1.0f, 1.0f, TSeg::Line},
        // 2 Jump Before Wrap: the same, going round
        {0.0f, 0.0f, TSeg::Line}, {1.0f - h, 1.0f - h, TSeg::Jump}, {1.0f - h, 0.3f, TSeg::Line}, {1.0f, 0.3f + h, TSeg::Line},
        {0.0f, 1.0f, TSeg::Line}, {1.0f, 1.0f, TSeg::Line},
        // 3 Jump After Start, 1 beat: a jump 60 samples after the start
        {0.0f, 0.0f, TSeg::Line}, {g, g, TSeg::Jump}, {g, 0.0f, TSeg::Line}, {1.0f, 1.0f - g, TSeg::Line},
        {0.0f, 1.0f, TSeg::Line}, {1.0f, 1.0f, TSeg::Line},
    };
    static const gf::TimePatternDef defs[] = {
        {"Close Jumps", 2.0f, false, 0, 8, 8, 2},
        {"Jump Before End", 1.0f, false, 10, 4, 14, 2},
        {"Jump Before Wrap", 1.0f, true, 16, 4, 20, 2},
        {"Jump After Start", 1.0f, false, 22, 4, 26, 2},
    };
    {   // the library gives every line its 5 ms at the fastest tempo (999 BPM), so none of its own is made this way
        double closest = 1e9;
        for (int p = 0; p < gf::numTimePatterns(); ++p) {
            const gf::TimePatternDef& d = gf::timePatternDef(p);
            const gf::TPoint* pts0 = gf::kTimePoints + d.srcFirst;
            double line = 0.0;
            for (int k = 0; k < d.srcCount; ++k) {
                if (pts0[k].seg != gf::TSeg::Jump || pts0[k].x >= d.lengthBeats - 1e-9) continue;
                closest = std::min(closest, pts0[k].x - line);
                line = pts0[k].x;
            }
            closest = std::min(closest, d.lengthBeats - line);
        }
        CHECK(closest >= static_cast<double>(kFade) / kBeat * (999.0 / 120.0));   // 220 samples at 999 BPM, in beats
    }
    const double hz = 457.0, bound = clickBound(hz, 1.0);
    const size_t S = at(3.0);
    const Buf in = sine(hz, static_cast<int>(S + at(4.0)), 0.5f);
    const auto run = [&](int p, size_t to) {
        TimeCore c;
        CHECK(c.allocate());
        c.useLibrary(defs, 4, pts);
        Buf L = in, R = in;
        play(c, L, R, 0, to, p, 3.0);
        L.resize(to);   // (the rest is the input, untouched)
        return L;
    };
    const double sg = 60.0;   // g in samples
    {   // 0: the three jumps; the head ends on the last one's place, one 220-sample fade after the fade the first of the two waited for
        const Buf out = run(0, S + at(2.5));
        const size_t j1 = S + at(0.5);
        const double top = maxStep(out, 2, out.size());
        CHECK(top <= bound);
        if (!(top <= bound)) std::printf("  close jumps: a step of %.4f, the bound %.4f\n", top, bound);
        bool live = true, last = true;
        for (size_t i = 0; i + kFade + 2 < j1; ++i)
            if (!near(out[i], in[i], 1e-5)) live = false;
        // after the last jump: lag 0.15 + 2 g beats; the second jump's fade began on its line (j1 + 60), the third waited for its end
        const double lag = (0.15 + 2.0 * sg / kBeat) * kBeat;
        for (size_t i = j1 + static_cast<size_t>(sg) + 2 * kFade + 4; i + kFade + 2 < S + at(2.0); ++i)
            if (!near(out[i], interp(in, static_cast<double>(i) - lag), 1e-4)) last = false;   // (the points are floats: 4e-5 off)
        CHECK(live);
        CHECK(last);
    }
    {   // 1: the end 80 samples after a jump: a fade to the live input from the end, which the jump does not disturb
        const Buf out = run(1, S + at(2.0));
        const size_t E = S + at(1.0);
        const double top = maxStep(out, 2, out.size());
        CHECK(top <= bound);
        if (!(top <= bound)) std::printf("  jump before the end: a step of %.4f, the bound %.4f\n", top, bound);
        bool live = true;
        for (size_t i = E + kFade + 2; i < E + at(0.5); ++i)
            if (!near(out[i], in[i], 1e-5)) live = false;
        CHECK(live);
    }
    {   // 2: the same going round: the wrap is a fade to the next lap's start, and the lap goes on
        const Buf out = run(2, S + at(3.0));
        const size_t E = S + at(1.0);
        const double top = maxStep(out, 2, out.size());
        CHECK(top <= bound);
        if (!(top <= bound)) std::printf("  jump before the wrap: a step of %.4f, the bound %.4f\n", top, bound);
        bool live = true;
        for (size_t i = E + kFade + 2; i + kFade + 2 < E + at(1.0) - 100; ++i)   // the second lap's live stretch
            if (!near(out[i], in[i], 1e-5)) live = false;
        CHECK(live);
    }
    {   // 3: a jump 60 samples after the start, as the pre-roll ends: the head lands 60 samples behind
        const Buf out = run(3, S + at(2.0));
        const double top = maxStep(out, 2, out.size());
        CHECK(top <= bound);
        if (!(top <= bound)) std::printf("  jump after the start: a step of %.4f, the bound %.4f\n", top, bound);
        bool back = true;
        for (size_t i = S + static_cast<size_t>(sg) + kFade + 4; i + kFade + 2 < S + at(1.0); ++i)
            if (!near(out[i], interp(in, static_cast<double>(i) - sg), 2e-5)) back = false;
        CHECK(back);
    }
}

// --- the grooves (Time) --------------------------------------------------------------------------
// Time is the engine with a lag that follows the groove. Its tests put a click on the grid and read
// where it is heard: a click through the 4-point Hermite read keeps its weight (1) and its first moment, so
// the centroid of what is heard around it is where it landed, to a hundredth of a sample.

using gf::Time;
using ParamsAt = std::function<Time::Params(size_t)>;

constexpr double kMs = kRate / 1000.0;   // a millisecond, in samples

// set() then process() per chunk, as the rack does. The chunk at sample i is at song position beats(i); a chunk
// never crosses a multiple of `cut` (where a loop wraps), if it is not 0.
void playTime(Time& time, const ParamsAt& params, Buf& L, Buf& R, size_t from, size_t to, double bpm, const Beats& beats,
              bool playing = true, int chunk = kChunk, size_t cut = 0) {
    for (size_t i = from; i < to;) {
        size_t m = std::min(static_cast<size_t>(chunk), to - i);
        if (cut) m = std::min(m, cut - i % cut);
        Transport t;
        t.bpm = bpm;
        t.beats = beats(i);
        t.playing = playing;
        t.valid = true;
        time.set(params(i), t);
        time.process(&L[i], &R[i], static_cast<int>(m));
        i += m;
    }
}
ParamsAt fixed(const Time::Params& p) { return [p](size_t) { return p; }; }
const Beats at120_ = [](size_t i) { return static_cast<double>(i) / kBeat; };   // 120 BPM from beat 0

// A fresh, allocated Time over `in` (left and right alike): the left output. A loop of `cut` samples wraps the song position.
Buf renderTime(const Buf& in, const ParamsAt& params, double bpm = 120.0, int chunk = kChunk, size_t cut = 0) {
    Time time;
    CHECK(time.allocate());
    Buf L = in, R = in;
    const double spb = kRate * 60.0 / bpm;
    playTime(time, params, L, R, 0, in.size(), bpm, [spb, cut](size_t i) { return static_cast<double>(cut ? i % cut : i) / spb; }, true, chunk, cut);
    CHECK(same(L, R));
    return L;
}

Time::Params swingParams(double amount, double unit) {
    Time::Params p;
    p.groove = Time::kSwing;
    p.swing.amount = static_cast<float>(amount);
    p.swing.unitBeats = unit;
    return p;
}
Time::Params grooveParams(int groove, float amount) {
    Time::Params p;
    p.groove = groove;
    p.amount = amount;
    return p;
}

// The first sample at or after a position in samples: where a click put "on a line" goes.
size_t lineSample(double samples) { return static_cast<size_t>(std::ceil(samples - 1e-6)); }

Buf clicksAt(size_t len, const std::vector<size_t>& where) {
    Buf x(len, 0.0f);
    for (size_t s : where)
        if (s < len) x[s] = 1.0f;
    return x;
}

// The output in [from, to) is these clicks and nothing else: each arrival keeps its weight (1 +- 1e-3) and lands within a
// tenth of a sample of where it was said to, and every sample away from them (+-6) is below 1e-5.
bool expectClicks(const Buf& out, const std::vector<double>& arrivals, size_t from, size_t to, const char* what) {
    bool ok = true;
    to = std::min(to, out.size());
    std::vector<char> mark(out.size(), 0);
    const auto window = [&](double a, const std::function<void(size_t)>& f) {
        const long c = std::lround(a);
        for (long i = c - 6; i <= c + 6; ++i)
            if (i >= static_cast<long>(from) && i < static_cast<long>(to)) f(static_cast<size_t>(i));
    };
    for (double a : arrivals) {
        window(a, [&](size_t i) { mark[i] = 1; });
        if (a < static_cast<double>(from) || a > static_cast<double>(to) - 7.0) continue;
        double w = 0.0, m = 0.0;
        window(a, [&](size_t i) {
            w += out[i];
            m += static_cast<double>(i) * out[i];
        });
        const bool good = std::fabs(w - 1.0) < 1e-3 && std::fabs(m / w - a) < 0.1;
        if (!good) {
            std::printf("  %s: the click for %.2f has weight %.4f at %.3f\n", what, a, w, w != 0.0 ? m / w : -1.0);
            ok = false;
        }
    }
    for (size_t i = from; i < to; ++i)
        if (!mark[i] && std::fabs(out[i]) > 1e-5) {
            std::printf("  %s: %.6f heard at sample %zu, where nothing should be\n", what, static_cast<double>(out[i]), i);
            ok = false;
            break;
        }
    CHECK(ok);
    return ok;
}

// How late the click put at `line` is heard: the centroid of what comes in [line - 6, line + span), less the line.
double delayOf(const Buf& out, size_t line, size_t span) {
    double w = 0.0, m = 0.0;
    for (size_t i = line > 6 ? line - 6 : 0; i < std::min(out.size(), line + span); ++i) {
        w += out[i];
        m += static_cast<double>(i) * out[i];
    }
    return w > 0.5 ? m / w - static_cast<double>(line) : -1e9;
}

// A click on each of `count` units: the odd ones come (2 amount - 1) units late, the even ones on time, and none early.
bool swingCase(double amount, double unit, double bpm, int count, int chunk) {
    const double spb = kRate * 60.0 / bpm, u = unit * spb;
    const double late = (2.0 * static_cast<double>(static_cast<float>(amount)) - 1.0) * u;
    std::vector<size_t> lines;
    std::vector<double> want;
    for (int k = 0; k < count; ++k) {
        lines.push_back(lineSample(k * u));
        want.push_back(static_cast<double>(lines.back()) + (k & 1 ? late : 0.0));
    }
    const size_t len = lineSample(count * u) + 3000;
    const Buf out = renderTime(clicksAt(len, lines), fixed(swingParams(amount, unit)), bpm, chunk);
    return expectClicks(out, want, 0, len - 100, "swing");
}

void timeSwing() {     // 66% on 1/16 at 120 BPM: the odd 16ths come out 40 ms late (+-1 sample), the even ones on time, none early
    {
        std::vector<size_t> lines;
        for (int k = 0; k < 32; ++k) lines.push_back(lineSample(k * 5512.5));
        const Buf out = renderTime(clicksAt(lineSample(32 * 5512.5) + 3000, lines), fixed(swingParams(0.66, 0.25)));
        bool timing = true;
        for (int k = 0; k < 32; ++k) {
            const double d = delayOf(out, lines[static_cast<size_t>(k)], 4000);
            if (!(k & 1 ? near(d, 40.0 * kMs, 1.0) : near(d, 0.0, 1.0))) timing = false;
        }
        CHECK(timing);
        CHECK(near(40.0 * kMs, 1764.0, 1e-9));
    }
    CHECK(swingCase(0.66, 0.25, 120.0, 40, kChunk));
    CHECK(swingCase(0.58, 0.25, 120.0, 40, kChunk));          // 20 ms: 882 samples
    CHECK(swingCase(0.75, 0.5, 100.0, 24, kChunk));           // 1/8, the most: half a unit, 150 ms
    CHECK(swingCase(0.66, 0.25, 133.7, 40, kChunk));          // a tempo that is not a whole number of samples
    CHECK(swingCase(0.66, 0.25, 300.0, 60, kChunk));          // fast: a 16th is 2205 samples
    CHECK(swingCase(0.66, 0.25, 120.0, 40, 7));               // whatever the chunks
    CHECK(swingCase(0.66, 0.25, 120.0, 12, 1));
    // straight is straight: 50% is on time everywhere
    CHECK(swingCase(0.5, 0.25, 120.0, 20, kChunk));
}

void timeSwingCrossfade() { // each change is a raised-cosine crossfade of 5 ms that ENDS on its line, forwards and back
    const Buf in = whiteNoise(60000, 0.5f, 31);
    const Buf out = renderTime(in, fixed(swingParams(0.66, 0.25)));
    const double lag = (2.0 * static_cast<double>(0.66f) - 1.0) * 5512.5;
    // the pairs [0, 11025) and [11025, 22050): the late unit's line is sample 5513 (5512.5), the pair's end 11025
    const size_t l1 = 5513, l2 = 11025, l3 = 16538, l4 = 22050;
    bool live = true, cross1 = true, late1 = true, cross2 = true, live2 = true, cross3 = true, late2 = true;
    for (size_t i = 0; i < l1 - kFade; ++i)
        if (out[i] != in[i]) live = false;
    for (size_t i = l1 - kFade; i < l1; ++i) {   // the old head (live) to the delayed: w the delayed one's share, 0 at first, 1 on the line
        const double w = 0.5 + 0.5 * std::cos(kPi * static_cast<double>(l1 - i) / kFade);
        if (!near(out[i], (1.0 - w) * in[i] + w * interp(in, static_cast<double>(i) - lag), 3e-6)) cross1 = false;
    }
    for (size_t i = l1; i < l2 - kFade; ++i)
        if (!near(out[i], interp(in, static_cast<double>(i) - lag), 3e-6)) late1 = false;
    for (size_t i = l2 - kFade; i < l2; ++i) {   // and back
        const double w = 0.5 + 0.5 * std::cos(kPi * static_cast<double>(l2 - i) / kFade);
        if (!near(out[i], (1.0 - w) * interp(in, static_cast<double>(i) - lag) + w * in[i], 3e-6)) cross2 = false;
    }
    for (size_t i = l2; i < l3 - kFade; ++i)
        if (!near(out[i], in[i], 3e-6)) live2 = false;
    for (size_t i = l3 - kFade; i < l3; ++i) {
        const double w = 0.5 + 0.5 * std::cos(kPi * static_cast<double>(l3 - i) / kFade);
        if (!near(out[i], (1.0 - w) * in[i] + w * interp(in, static_cast<double>(i) - lag), 3e-6)) cross3 = false;
    }
    for (size_t i = l3; i < l4 - kFade; ++i)
        if (!near(out[i], interp(in, static_cast<double>(i) - lag), 3e-6)) late2 = false;
    CHECK(live);
    CHECK(cross1);
    CHECK(late1);
    CHECK(cross2);
    CHECK(live2);
    CHECK(cross3);
    CHECK(late2);
    // on each line the old head is silent: the first sample of the new lag IS the new head
    CHECK(near(out[l1], interp(in, static_cast<double>(l1) - lag), 3e-6) && near(out[l2], in[l2], 3e-6));
}

void timeDrunk() {     // a lag per 16th from a hash of the 16th's index: 0..amount 15 ms, the same on every bar, each change ending on its 16th
    for (float amount : {1.0f, 0.5f}) {
        for (size_t loop : {static_cast<size_t>(88200), static_cast<size_t>(0)}) {   // under a 1-bar loop, and on through the bars
            const size_t bar = 88200;
            std::vector<size_t> lines;
            for (int k = 0; k < 64; ++k) lines.push_back(lineSample(k * 5512.5));
            const size_t len = 4 * bar + 3000;
            const Buf out = renderTime(clicksAt(len, lines), fixed(grooveParams(Time::kDrunk, amount)), 120.0, kChunk, loop);
            std::vector<double> delay, arrivals;
            bool range = true, early = true;
            double lo = 1e9, hi = -1e9;
            for (size_t k = 0; k < 64; ++k) {
                const double d = delayOf(out, lines[k], 5000);
                delay.push_back(d);
                arrivals.push_back(static_cast<double>(lines[k]) + d);
                if (!(d >= -0.1 && d <= amount * 15.0 * kMs + 0.1)) range = false;
                for (size_t i = lines[k]; i + 6 < static_cast<size_t>(static_cast<double>(lines[k]) + d); ++i)
                    if (std::fabs(out[i]) > 1e-5) early = false;   // nothing at the line itself: the new lag is already in
                lo = std::min(lo, d);
                hi = std::max(hi, d);
            }
            CHECK(range);
            CHECK(early);
            CHECK(hi - lo > 0.4 * amount * 15.0 * kMs);   // it does move around: the 16 lags of a bar are not alike
            bool repeats = true;
            for (size_t k = 16; k < 64; ++k)
                if (!near(delay[k], delay[k % 16], 1e-3)) repeats = false;
            CHECK(repeats);   // bar 2 equals bar 1 (and 3, 4)
            expectClicks(out, arrivals, 0, len - 100, "drunk");   // one click each, nothing in between
        }
    }
    {   // noise: a bar that repeats comes out the same every time round, each bar the same as the one before
        const size_t bar = 88200;
        const Buf one = whiteNoise(static_cast<int>(bar), 0.5f, 77);
        Buf in;
        for (int b = 0; b < 4; ++b) in.insert(in.end(), one.begin(), one.end());
        const Buf out = renderTime(in, fixed(grooveParams(Time::kDrunk, 1.0f)), 120.0, kChunk, bar);
        double d12 = 0.0, d23 = 0.0, d34 = 0.0;
        for (size_t i = 0; i < bar; ++i) {
            if (i >= 1200) d12 = std::max(d12, static_cast<double>(std::fabs(out[bar + i] - out[i])));   // (bar 1 starts on an empty ring)
            d23 = std::max(d23, static_cast<double>(std::fabs(out[2 * bar + i] - out[bar + i])));
            d34 = std::max(d34, static_cast<double>(std::fabs(out[3 * bar + i] - out[2 * bar + i])));
        }
        CHECK(d12 < 1e-6 && d23 < 1e-6 && d34 < 1e-6);
        // and it is not the input: the lags did something
        double moved = 0.0;
        for (size_t i = bar; i < 2 * bar; ++i) moved = std::max(moved, static_cast<double>(std::fabs(out[i] - in[i])));
        CHECK(moved > 0.1);
    }
}

void timePush() {      // a constant lag of amount 20 ms, in ms whatever the tempo
    for (double bpm : {120.0, 90.0, 171.0}) {
        const double spb = kRate * 60.0 / bpm;
        std::vector<size_t> lines;
        for (int k = 0; k < 24; ++k) lines.push_back(lineSample(k * spb / 4.0));
        const size_t len = lineSample(24 * spb / 4.0) + 3000;
        const Buf in = clicksAt(len, lines);
        for (float amount : {1.0f, 0.5f, 0.0f}) {
            const Buf out = renderTime(in, fixed(grooveParams(Time::kPush, amount)), bpm);
            std::vector<double> want;
            for (size_t l : lines) want.push_back(static_cast<double>(l) + amount * 20.0 * kMs);
            expectClicks(out, want, 0, len - 100, "push");
            if (amount == 1.0f) CHECK(near(delayOf(out, lines[5], 3000), 882.0, 1.0));   // 20 ms
        }
    }
}

void timeGrooveOff() { // Off is the input, bit for bit, while the ring records; so is a groove that does nothing
    const Buf in = whiteNoise(40000, 0.8f, 5), inR = whiteNoise(40000, 0.8f, 6);
    Time::Params straight = swingParams(0.5, 0.25), straight8 = swingParams(0.5, 0.5), nodrunk = grooveParams(Time::kDrunk, 0.0f),
                 nopush = grooveParams(Time::kPush, 0.0f);
    for (const Time::Params& p : {Time::Params{}, straight, straight8, nodrunk, nopush}) {
        for (int chunk : {32, 7, 1}) {
            Time time;
            CHECK(time.allocate());
            Buf L = in, R = inR;
            playTime(time, fixed(p), L, R, 0, L.size(), 120.0, at120_, true, chunk);
            CHECK(same(L, in) && same(R, inR));
        }
    }
    {   // it recorded: a Push switched on after the Off reads what Off heard
        Time time;
        CHECK(time.allocate());
        Buf L = in, R = in;
        const size_t sw = 20000;
        playTime(time, fixed(Time::Params{}), L, R, 0, sw, 120.0, at120_);
        playTime(time, fixed(grooveParams(Time::kPush, 1.0f)), L, R, sw, L.size(), 120.0, at120_);
        CHECK(std::memcmp(L.data(), in.data(), sw * sizeof(float)) == 0);
        bool ok = true;
        for (size_t i = sw + kFade + 2; i < L.size(); ++i)
            if (!near(L[i], in[i - 882], 1e-6)) ok = false;
        CHECK(ok);
    }
    {   // Swing, then straight again: the lag crossfades away and the input is back bit for bit
        Time time;
        CHECK(time.allocate());
        Buf L = in, R = in;
        playTime(time, fixed(swingParams(0.66, 0.25)), L, R, 0, 20000, 120.0, at120_);
        playTime(time, fixed(Time::Params{}), L, R, 20000, L.size(), 120.0, at120_);
        bool ok = true;
        for (size_t i = 20000 + kFade + 2; i < L.size(); ++i)
            if (L[i] != in[i]) ok = false;
        CHECK(ok);
    }
}

// --- a pattern over a groove -------------------------------------------------------------------------

// Where a click put at `y` beats is heard under a swing of unit `u` whose late lag is `lateBeats`, with the groove suspended on
// [zFrom, zTo): a click in the second unit of a pair comes late, and is lost if that takes it past the pair's end; a click in the
// last stretch before a second unit's line (the late lag long) is heard twice, live and again after the line, because the late
// unit starts again from where it was. Returns how many times it is heard, in at[].
int heardAt(double y, double u, double lateBeats, double zFrom, double zTo, double at[2]) {
    const double unitNo = std::floor(y / u + 1e-9), line = (unitNo + 1.0) * u;
    const bool waiting = y >= zFrom && y < zTo;
    if (waiting) {
        at[0] = y;
        return 1;
    }
    if (std::fmod(unitNo, 2.0) == 1.0) {
        if (y + lateBeats >= line - 1e-9) return 0;
        at[0] = y + lateBeats;
        return 1;
    }
    at[0] = y;
    if (y >= line - lateBeats - 1e-9 && !(line >= zFrom && line < zTo)) {
        at[1] = y + lateBeats;
        return 2;
    }
    return 1;
}

// One pattern played over a swing, from `start` beats. The input is a click every `step` beats up to `last` beats, so:
//  - before the pattern the swing's clicks come out where heardAt says, and after it too, except that the groove stays
//    suspended on [zFrom, zTo) beats (a pattern ends, the groove resumes on the next unit line);
//  - the same whether the caller keeps passing the pattern after its end or stops (`keep`);
//  - over noise, inside the pattern the output is the bare pattern's, with no groove (the end's last 5 ms aside).
// Returns the click output, to compare two runs.
Buf overCase(double unit, double amount, const char* pattern, double start, double step, double last, double zFrom, double zTo, bool keep,
             bool loop = false) {
    const int p = idx(pattern);
    const double lateBeats = (2.0 * static_cast<double>(static_cast<float>(amount)) - 1.0) * unit;
    const double len = gf::timePatternDef(p).lengthBeats;
    const size_t S = lineSample(start * kBeat), E = lineSample((start + len) * kBeat);
    const size_t total = lineSample((last + 1.0) * kBeat);
    Time::Params base = swingParams(amount, unit);
    base.pattern = p;
    base.patternStart = start;
    base.loop = loop;
    const ParamsAt params = [=](size_t i) {
        Time::Params q = base;
        if (!keep && i >= E + 100) q.pattern = -1;   // the Move is over: the caller stops asking
        return q;
    };
    std::vector<size_t> lines;
    std::vector<double> pre, post;
    for (int j = 0; j * step <= last + 1e-9; ++j) {
        const double y = j * step;
        lines.push_back(lineSample(y * kBeat));
        double at[2] = {0.0, 0.0};
        const int heard = heardAt(y, unit, lateBeats, zFrom, zTo, at);
        for (int h = 0; h < heard; ++h) {
            const double arrival = static_cast<double>(lines.back()) + (at[h] - y) * kBeat;
            if (y < start && arrival < static_cast<double>(S) - 8.0) pre.push_back(arrival);
            if (y >= start + len - 1e-9) post.push_back(arrival);
        }
    }
    const Buf out = renderTime(clicksAt(total, lines), params);
    expectClicks(out, pre, 0, S - 8, "before the pattern");
    if (!loop) expectClicks(out, post, E, total - 400, "after the pattern");

    // inside the pattern: the bare pattern, over noise
    const Buf noise = whiteNoise(static_cast<int>(total), 0.5f, 12);
    const Buf over = renderTime(noise, params);
    Buf bare = noise, bareR = noise;
    TimeCore c;
    CHECK(c.allocate());
    play(c, bare, bareR, 0, bare.size(), p, start, 0.0);
    bool same_ = true;
    double worst = 0.0;
    const size_t to = loop ? S + (E - S) * 2 : E - kFade - 2;   // a loop goes round, seams included
    for (size_t i = S; i < to; ++i) {
        worst = std::max(worst, static_cast<double>(std::fabs(over[i] - bare[i])));
        if (!near(over[i], bare[i], 2e-5)) same_ = false;
    }
    CHECK(same_);
    if (!same_) std::printf("  %s over a swing: differs from the bare pattern by %g\n", pattern, worst);
    return out;
}

void timePatternOverGroove() { // a pattern plays over a swing and the swing resumes on the next unit line
    const double u = 0.25, a = 0.66;
    // A: Reverse Beat from beat 2 (a pair's start) to 3: it starts from the late lag the 7th 16th left, ends on a pair's start
    const Buf a1 = overCase(u, a, "Reverse Beat", 2.0, 0.25, 6.0, 0.0, 0.0, true);
    const Buf a2 = overCase(u, a, "Reverse Beat", 2.0, 0.25, 6.0, 0.0, 0.0, false);
    CHECK(same(a1, a2));   // the caller's stopping after the end changes nothing
    // B: from the 9th 16th (a late line, the groove was on time) to the 13th (a late line): it ends back at the late lag
    const Buf b1 = overCase(u, a, "Reverse Beat", 2.25, 0.25, 6.0, 0.0, 0.0, true);
    const Buf b2 = overCase(u, a, "Reverse Beat", 2.25, 0.25, 6.0, 0.0, 0.0, false);
    CHECK(same(b1, b2));
    // C: 1/8 swing at 75%: from the middle of the second unit (a late lag of half a unit, 5512.5 samples) to the middle of the
    // second unit of the next pair. The groove resumes on the next unit line, the pair's: the clicks in between come on time.
    const double l = 0.5, ze = 3.75;
    const Buf c1 = overCase(l, 0.75, "Reverse Beat", 2.75, 0.125, 4.625, ze, 4.0, true);
    const Buf c2 = overCase(l, 0.75, "Reverse Beat", 2.75, 0.125, 4.625, ze, 4.0, false);
    CHECK(same(c1, c2));
    // D: a pattern that goes round by itself, one shot, from a late line to a late line: the groove resumes after the line,
    // on the next one (the end of such a pattern is a crossfade to the live input, not to the lag)
    const Buf d1 = overCase(u, a, "Stutter 1/16", 2.25, 0.25, 8.0, 6.25, 6.5, true);
    const Buf d2 = overCase(u, a, "Stutter 1/16", 2.25, 0.25, 8.0, 6.25, 6.5, false);
    CHECK(same(d1, d2));
    // E: Half-Time Bar (it ends in a jump to now), 4 beats from the middle of a second unit to the middle of another, under
    // 66% on 1/8 (a late lag of 0.16 beat). The click at 7.875 would be taken past its pair's end by the lag: it is lost, as it
    // is without a pattern.
    const Buf e1 = overCase(l, 0.66, "Half-Time Bar", 2.75, 0.125, 8.0, 6.75, 7.0, true);
    const Buf e2 = overCase(l, 0.66, "Half-Time Bar", 2.75, 0.125, 8.0, 6.75, 7.0, false);
    CHECK(same(e1, e2));
}

void timePatternLag() { // a pattern over a lag > 0: it starts out of the lag (a pre-roll) and ends back into it, 5 ms each, on its lines
    // 105 BPM: a beat is 25200 samples, a 16th 6300, and the late lag of 66% on 1/16 is 2016.0003 samples
    const double bpm = 105.0, spb = 25200.0, lag = (2.0 * static_cast<double>(0.66f) - 1.0) * 6300.0;
    const int p = idx("Reverse Beat");
    const Buf in = whiteNoise(static_cast<int>(7 * spb), 0.5f, 61);
    const Beats beats = [spb](size_t i) { return static_cast<double>(i) / spb; };
    const auto render105 = [&](double start) {
        Time::Params q = swingParams(0.66, 0.25);
        q.pattern = p;
        q.patternStart = start;
        Time time;
        CHECK(time.allocate());
        Buf L = in, R = in;
        playTime(time, fixed(q), L, R, 0, L.size(), bpm, beats);
        return L;
    };
    {   // from beat 2 (a pair's start): the groove was in its late unit, lag 2016; the pattern starts at the line, from the lag
        const Buf out = render105(2.0);
        const size_t S = 50400;
        bool pre = true, inside = true;
        for (size_t i = S - kFade; i < S; ++i) {   // the lag's head to the live one, w the live one's share: 0 at first, 1 on the line
            const double w = 0.5 + 0.5 * std::cos(kPi * static_cast<double>(S - i) / kFade);
            if (!near(out[i], (1.0 - w) * interp(in, static_cast<double>(i) - lag) + w * in[i], 3e-6)) pre = false;
        }
        for (size_t i = S; i + kFade < S + 25200; ++i)
            if (!near(out[i], in[2 * S - i], 1e-6)) inside = false;
        CHECK(pre);
        CHECK(inside);
        bool held = true;   // the lag until then
        for (size_t i = S - 3000; i < S - kFade; ++i)
            if (!near(out[i], interp(in, static_cast<double>(i) - lag), 3e-6)) held = false;
        CHECK(held);
    }
    {   // beat 2.25 to 3.25, two late lines: the groove was on time, and the pattern ends back in the late lag
        const Buf out = render105(2.25);
        const size_t S = 56700, E = 81900;
        bool pre = true, post = true, end = true;
        for (size_t i = S - kFade; i < S; ++i)   // out of straight time: the live signal all through
            if (!near(out[i], in[i], 1e-6)) pre = false;
        for (size_t i = E - kFade; i < E; ++i) {   // the reversed head to the lag's head, on the line
            const double w = 0.5 + 0.5 * std::cos(kPi * static_cast<double>(E - i) / kFade);
            if (!near(out[i], (1.0 - w) * in[2 * S - i] + w * interp(in, static_cast<double>(i) - lag), 3e-6)) end = false;
        }
        for (size_t i = E; i + kFade < E + 6300; ++i)   // and the late unit goes on in the lag, up to its own change back
            if (!near(out[i], interp(in, static_cast<double>(i) - lag), 3e-6)) post = false;
        CHECK(pre);
        CHECK(end);
        CHECK(post);
    }
}

void timePatternLoops() { // loop: a pattern repeats until the caller stops asking, on any pattern
    {   // a loopable pattern is the engine's own wrap: the same as the bare engine for two laps
        overCase(0.25, 0.66, "Stutter 1/16", 2.0, 0.25, 12.0, 0.0, 0.0, true, true);
    }
    {   // a pattern that does not go round by itself, Reverse Beat: a lap a beat, each the beat before backwards; the caller
        // stops asking just before the third lap ends (inside a swung unit), and the swing is back on the next unit line
        const int p = idx("Reverse Beat");
        const double start = 2.0;
        const size_t S = at(start), B = at(1.0);
        const Buf in = whiteNoise(static_cast<int>(S + 6 * B), 0.5f, 90);
        Time::Params q = swingParams(0.66, 0.25);
        q.pattern = p;
        q.patternStart = start;
        q.loop = true;
        const size_t stop = (S + 3 * B - 400) / kChunk * kChunk;   // a chunk start, 400 samples before the third lap ends
        const Buf out = renderTime(in, [=](size_t i) {
            Time::Params r = q;
            if (i >= stop) r.pattern = -1;
            return r;
        });
        bool laps = true;
        for (size_t k = 0; k < 3; ++k) {
            const size_t s = S + k * B;
            for (size_t i = s; i + kFade < std::min(s + B, stop); ++i)
                if (!near(out[i], in[2 * s - i], 1e-6)) laps = false;
        }
        CHECK(laps);
        // then the groove is back, from the next unit line (5.0, whose unit is on time like the one after it)
        bool live = true;
        for (size_t i = stop + kFade + 2; i + kFade + 2 < lineSample(5.25 * kBeat); ++i)
            if (!near(out[i], in[i], 1e-6)) live = false;
        CHECK(live);
    }
    {   // the caller takes `loop` away (the pattern still named): it stops where it is, in a swung unit, and the groove waits for the next line
        const double start = 2.0;
        const size_t S = at(start), B = at(1.0);
        const Buf in = whiteNoise(static_cast<int>(S + 6 * B), 0.5f, 91);
        Time::Params q = swingParams(0.66, 0.25);
        q.pattern = idx("Reverse Beat");
        q.patternStart = start;
        q.loop = true;
        const size_t stop = at(4.8) / kChunk * kChunk;   // inside the late unit [4.75, 5.0)
        const Buf out = renderTime(in, [=](size_t i) {
            Time::Params r = q;
            if (i >= stop) r.loop = false;
            return r;
        });
        bool live = true;
        for (size_t i = stop + kFade + 2; i + kFade + 2 < lineSample(5.25 * kBeat); ++i)
            if (!near(out[i], in[i], 1e-6)) live = false;
        CHECK(live);
    }
    {   // seams: laps of a reversed tone without a click
        const int p = idx("Reverse Beat");
        const double hz = 457.0;
        const Buf in = sine(hz, static_cast<int>(at(10.0)), 0.5f);
        Time::Params q;
        q.pattern = p;
        q.patternStart = 2.0;
        q.loop = true;
        const Buf out = renderTime(in, fixed(q));
        CHECK(maxStep(out, 2, out.size()) <= clickBound(hz, 1.0));
        CHECK(allFinite(out));
        // still looping at the end: lap k+1 starts the beat after lap k
        const size_t s = at(2.0) + 5 * at(1.0);
        bool lap = true;
        for (size_t i = s; i + kFade < s + at(1.0); ++i)
            if (!near(out[i], interp(in, 2.0 * static_cast<double>(s) - static_cast<double>(i)), 2e-6)) lap = false;
        CHECK(lap);
    }
    {   // not looping: a loopable pattern stops at its length (the engine itself would go round)
        const int p = idx("Stutter 1/16");
        const size_t S = at(2.0), E = S + at(4.0);
        const Buf in = mixture(static_cast<int>(E + at(2.0)));
        Time::Params q;
        q.pattern = p;
        q.patternStart = 2.0;
        const Buf out = renderTime(in, fixed(q));
        bool live = true;
        for (size_t i = E + 2; i < E + at(1.0); ++i)
            if (!near(out[i], in[i], 1e-6)) live = false;
        CHECK(live);
        q.loop = true;
        const Buf again = renderTime(in, fixed(q));
        bool goes = false;
        for (size_t i = E + at(0.5); i < E + at(1.0); ++i)
            if (!near(again[i], in[i], 1e-3)) goes = true;
        CHECK(goes);   // the loop is still stuttering where the one shot has long been live
    }
}

// --- the contract ------------------------------------------------------------------------------------

void timeZoneLapses() { // the wait after a pattern is over when the position has gone by it: an MPC loop over the place again sees the groove
    const Buf in = whiteNoise(static_cast<int>(4 * 88200), 0.5f, 7);
    struct Case { const char* what; size_t loop; double cancel; };   // the loop's length in samples, where in it the pattern is cancelled (beats)
    const Case cases[] = {
        {"cancelled in a late unit, the loop goes on past the wait", 88200, 0.85},
        {"cancelled late in the loop", 88200, 2.9},
        {"cancelled in a late unit, the loop wraps inside the wait", 19845, 0.85},   // 0.9 beat: the position goes back before the wait is over
    };
    for (const Case& c : cases) {
        const Buf ref = renderTime(in, fixed(swingParams(0.66, 0.25)), 120.0, kChunk, c.loop);   // a run that never had a pattern
        const Buf out = renderTime(in, [&](size_t i) {
            Time::Params p = swingParams(0.66, 0.25);
            if (i < c.loop && static_cast<double>(i) / kBeat < c.cancel) {   // the first pass only: Stutter 1/16, looping, from the start of the loop
                p.pattern = idx("Stutter 1/16");
                p.patternStart = 0.0;
                p.loop = true;
            }
            return p;
        }, 120.0, kChunk, c.loop);
        double worst = 0.0;
        for (size_t pass = 1; pass * c.loop + 3000 < in.size(); ++pass)
            for (size_t i = pass * c.loop + 3000; i < std::min(in.size(), (pass + 1) * c.loop); ++i)
                worst = std::max(worst, static_cast<double>(std::fabs(out[i] - ref[i])));
        CHECK(worst < 1e-5);   // every pass after the first is the groove, as if no pattern had ever been
        if (!(worst < 1e-5)) std::printf("  %s: %.4f from a run that never had a pattern\n", c.what, worst);
    }
}

void timeGrooveNoClicks() { // a sine through every groove, and through every switch of groove and of amount: no step beyond a crossfade's
    const double hz = 457.0;
    const Buf in = sine(hz, static_cast<int>(at(16.0)), 0.5f);
    const double bound = clickBound(hz, 1.0);
    for (const Time::Params& p : {swingParams(0.66, 0.25), swingParams(0.75, 0.5), swingParams(0.58, 0.25), grooveParams(Time::kDrunk, 1.0f),
                                  grooveParams(Time::kPush, 1.0f)}) {
        const Buf out = renderTime(in, fixed(p));
        const double top = maxStep(out, 2, out.size());
        CHECK(top <= bound);
        if (!(top <= bound)) std::printf("  groove %d: a step of %.4f, the bound %.4f\n", p.groove, top, bound);
    }
    {   // the groove changes every 7000 samples; the amount sweeps; a pattern comes and goes in the middle
        const Time::Params list[] = {swingParams(0.66, 0.25), Time::Params{}, grooveParams(Time::kDrunk, 1.0f), grooveParams(Time::kPush, 0.7f),
                                     swingParams(0.58, 0.5), grooveParams(Time::kPush, 0.1f)};
        const ParamsAt params = [&](size_t i) {
            Time::Params p = list[(i / 7000) % 6];
            p.amount = p.groove == Time::kPush || p.groove == Time::kDrunk ? static_cast<float>(0.2 + 0.8 * std::fabs(std::sin(i * 1e-5))) : p.amount;
            if (i >= at(6.0) && i < at(6.0) + 9000) {
                p.pattern = idx("Half-Time Bar");
                p.patternStart = 6.0 - 0.01;
            }
            return p;
        };
        const Buf out = renderTime(in, params);
        const double top = maxStep(out, 2, out.size()), b = clickBound(hz, 1.0);
        CHECK(top <= b);
        CHECK(allFinite(out));
        if (!(top <= b)) std::printf("  switching: a step of %.4f, the bound %.4f\n", top, b);
    }
    {   // a pattern stopped half way over a swing: a crossfade, not a click
        Time::Params q = swingParams(0.66, 0.25);
        q.pattern = idx("Reverse Beat");
        q.patternStart = 2.0;
        q.loop = true;
        const Buf out = renderTime(in, [=](size_t i) {
            Time::Params r = q;
            if (i >= at(4.31)) r.pattern = -1;
            return r;
        });
        CHECK(maxStep(out, 2, out.size()) <= bound);
    }
}

void timeTempoRamp() { // the tempo moves under a groove: every line still ends on its unit, each click is heard once, never early
    const size_t total = 4 * 44100;
    struct Chunk { size_t at; double bpm, beats; int n; };
    std::vector<Chunk> chunks;
    double beats = 0.0;
    for (size_t i = 0; i < total; i += kChunk) {
        const double bpm = 100.0 + 40.0 * static_cast<double>(i) / total;
        chunks.push_back({i, bpm, beats, static_cast<int>(std::min<size_t>(kChunk, total - i))});
        beats += chunks.back().n * bpm / 60.0 / kRate;
    }
    for (int which = 0; which < 2; ++which) {   // Drunk (a lag in ms), Swing (a lag in beats)
        const double unit = 0.25;
        const Time::Params p = which ? swingParams(0.66, unit) : grooveParams(Time::kDrunk, 1.0f);
        // a click on the first sample at or after each 16th, found by the same chunk-linear position the module uses
        std::vector<size_t> lines;
        double next = unit;
        for (const Chunk& c : chunks) {
            const double bps = c.bpm / 60.0 / kRate;
            while (next <= c.beats + c.n * bps - 1e-9) {
                lines.push_back(c.at + static_cast<size_t>(std::ceil((next - c.beats) / bps - 1e-6)));
                next += unit;
            }
        }
        const Buf in = clicksAt(total, lines);
        Time time;
        CHECK(time.allocate());
        Buf L = in, R = in;
        for (const Chunk& c : chunks) {
            Transport t;
            t.bpm = c.bpm;
            t.beats = c.beats;
            t.playing = t.valid = true;
            time.set(p, t);
            time.process(&L[c.at], &R[c.at], c.n);
        }
        bool once = true, early = true, range = true;
        for (size_t k = 0; k + 1 < lines.size() && lines[k + 1] + 100 < total; ++k) {
            double w = 0.0, m = 0.0;
            for (size_t i = lines[k]; i < lines[k + 1]; ++i) {   // the interval after the line: this click, late by its lag
                w += L[i];
                m += static_cast<double>(i) * L[i];
            }
            if (!(std::fabs(w - 1.0) < 1e-3)) {
                if (once) std::printf("  interval %zu (sample %zu): weight %.5f\n", k, lines[k], w);
                once = false;
            }
            const double d = m / w - static_cast<double>(lines[k]);
            const double most = which ? 0.34 * static_cast<double>(lines[k + 1] - lines[k]) : 15.0 * kMs;
            if (!(d >= -0.1 && d <= most + 1.0)) range = false;
            for (size_t i = lines[k]; i + 6 < static_cast<size_t>(static_cast<double>(lines[k]) + d); ++i)
                if (std::fabs(L[i]) > 1e-5) early = false;
        }
        CHECK(once);
        CHECK(early);
        CHECK(range);
        if (!(once && early && range)) std::printf("  tempo ramp, groove %d: once %d early %d range %d\n", p.groove, once, early, range);
    }
}

void timeGrooveChunking() { // the same output whatever the chunks
    const Buf in = whiteNoise(110000, 0.5f, 55);
    for (const Time::Params& p : {swingParams(0.66, 0.25), grooveParams(Time::kDrunk, 1.0f), grooveParams(Time::kPush, 0.8f)}) {
        const Buf ref = renderTime(in, fixed(p), 133.0);
        for (int chunk : {1, 5, 17, 31}) {
            const Buf out = renderTime(in, fixed(p), 133.0, chunk);
            double worst = 0.0;
            for (size_t i = 0; i < out.size(); ++i) worst = std::max(worst, static_cast<double>(std::fabs(out[i] - ref[i])));
            CHECK(worst < 1e-4);
            if (!(worst < 1e-4)) std::printf("  groove %d in chunks of %d: differs by %g\n", p.groove, chunk, worst);
        }
    }
}

void timeGrooveStopped() { // stopped: the grid is seeded from MPC's position on the first chunk, and runs on at the tempo
    const double b0 = 0.25;   // an odd 16th: the swing's late unit
    std::vector<size_t> lines;
    std::vector<double> want;
    const double u = 5512.5, late = (2.0 * static_cast<double>(0.66f) - 1.0) * u;
    for (int j = 0; j < 20; ++j) {   // sample 0 is the 16th numbered 1
        lines.push_back(lineSample(j * u));
        want.push_back(static_cast<double>(lines.back()) + ((j + 1) & 1 ? late : 0.0));
    }
    const size_t len = lineSample(20 * u) + 3000;
    const Buf in = clicksAt(len, lines);
    Time time;
    CHECK(time.allocate());
    Buf L = in, R = in;
    Time::Params p = swingParams(0.66, 0.25);
    for (size_t i = 0; i < len; i += kChunk) {
        Transport t;
        t.bpm = 120.0;
        t.beats = i == 0 ? b0 : 123.456;   // the first says where; the rest are nonsense that a stopped transport does not use
        t.playing = false;
        t.valid = true;
        time.set(p, t);
        time.process(&L[i], &R[i], static_cast<int>(std::min<size_t>(kChunk, len - i)));
    }
    expectClicks(L, want, 0, len - 100, "stopped");
    // playing, the song position is MPC's every chunk: the same
    Time time2;
    CHECK(time2.allocate());
    Buf L2 = in, R2 = in;
    playTime(time2, fixed(p), L2, R2, 0, len, 120.0, [b0](size_t i) { return b0 + static_cast<double>(i) / kBeat; });
    bool same_ = true;
    for (size_t i = 0; i < len; ++i)
        if (!near(L[i], L2[i], 1e-6)) same_ = false;
    CHECK(same_);
}

void timeGrooveAllocate() { // before allocate(): the input passes untouched and nothing is read; after it the groove works
    Time time;
    CHECK(!time.allocated());
    const Buf in = whiteNoise(30000, 0.5f, 3);
    Buf L = in, R = in;
    Time::Params p = swingParams(0.66, 0.25);
    p.pattern = idx("Stutter 1/16");
    p.patternStart = 0.0;
    playTime(time, fixed(p), L, R, 0, 10000, 120.0, at120_);
    CHECK(std::memcmp(L.data(), in.data(), 10000 * sizeof(float)) == 0);
    CHECK(time.tailSamples() == 0);
    CHECK(time.allocate() && time.allocated() && time.allocate());
    playTime(time, fixed(grooveParams(Time::kPush, 1.0f)), L, R, 10000, 30000, 120.0, at120_);
    bool silent = true, delayed = true;
    for (size_t i = 10000; i < 10000 + 870; ++i)
        if (L[i] != 0.0f) silent = false;
    for (size_t i = 10000 + 882 + kFade + 2; i < 30000; ++i)
        if (!near(L[i], in[i - 882], 1e-6)) delayed = false;
    CHECK(silent);
    CHECK(delayed);
}

void timeGrooveReset() { // reset() forgets the recording, the pattern and the groove's state
    const Buf in = whiteNoise(60000, 0.5f, 8);
    Time::Params p = swingParams(0.66, 0.25);
    p.pattern = idx("Tape Stop");
    p.patternStart = 0.5;
    Time a, b;
    CHECK(a.allocate() && b.allocate());
    Buf L = in, R = in;
    playTime(a, fixed(p), L, R, 0, 30000, 120.0, at120_);
    a.reset();
    b.reset();
    p.pattern = -1;
    Buf x = in, xR = in, y = in, yR = in;
    playTime(a, fixed(p), x, xR, 0, 60000, 120.0, at120_);
    playTime(b, fixed(p), y, yR, 0, 60000, 120.0, at120_);
    CHECK(same(x, y));
}

void timeGrooveTail() { // how long it sounds after the input stops
    Time time;
    Transport t;
    CHECK(time.allocate());
    time.set(Time::Params{}, t);
    CHECK(time.tailSamples() == 0);
    time.set(swingParams(0.66, 0.25), t);
    CHECK(time.tailSamples() >= 1764 + kFade && time.tailSamples() < 4000);
    time.set(grooveParams(Time::kDrunk, 1.0f), t);
    CHECK(time.tailSamples() >= 661 + kFade && time.tailSamples() < 4000);
    time.set(grooveParams(Time::kPush, 1.0f), t);
    CHECK(time.tailSamples() >= 882 + kFade && time.tailSamples() < 4000);
    Time::Params p;
    p.pattern = idx("Rewind");
    p.patternStart = 0.0;
    time.set(p, t);
    CHECK(time.tailSamples() >= static_cast<int>(4 * kBeat) && time.tailSamples() <= TimeCore::kRingFrames);   // the longest reach back is 4 beats
}

void timeGrooveRobust() { // wild parameters and transports: finite, in range, nothing undefined (UBSan)
    Time time;
    CHECK(time.allocate());
    uint32_t seed = 4242;
    const auto rnd = [&seed]() { return ef::xorshift(seed); };
    const double bpms[] = {120.0, std::nan(""), 0.0, -5.0, 19.0, 133.7, 999.0, 5000.0, 1e9, HUGE_VAL, -HUGE_VAL};
    const double songs[] = {0.0, 17.25, std::nan(""), HUGE_VAL, -HUGE_VAL, -1e9, 1e12, 1e300, 3.0};
    const double starts[] = {0.0, 1.0, 17.0, std::nan(""), HUGE_VAL, -HUGE_VAL, 1e300, -1e300, 1e15, -1e15, 16.99};
    const float amounts[] = {0.0f, 0.3f, 0.5f, 0.66f, 0.75f, 1.0f, 2.0f, -1.0f, std::nanf(""), HUGE_VALF, 1e30f};
    const double units[] = {0.25, 0.5, 0.0, -1.0, std::nan(""), HUGE_VAL, 1e300, 1e-9, 1e-300, 3.0};
    const int grooves[] = {0, 1, 2, 3, -1, 4, 99, 2147483647, -2147483647 - 1};
    const int pats[] = {-1, -7, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 99, 2147483647, -2147483647 - 1};
    Buf L = whiteNoise(300000, 0.8f, 9), R = whiteNoise(300000, 0.8f, 10);
    L[1000] = 0.8f;
    bool finite = true;
    double top = 0.0, song = 0.0;
    for (size_t i = 0; i < L.size();) {
        if (rnd() % 8 == 0) song = songs[rnd() % 9];
        else if (std::isfinite(song) && std::fabs(song) < 1e9) song += 0.01;
        Transport t;
        t.bpm = bpms[rnd() % 11];
        t.beats = song;
        t.playing = rnd() % 4 != 0;
        t.valid = rnd() % 8 != 0;
        const size_t m = std::min<size_t>(1 + rnd() % 128, L.size() - i);
        Time::Params p;
        p.groove = grooves[rnd() % 9];
        p.amount = amounts[rnd() % 11];
        p.swing.amount = amounts[rnd() % 11];
        p.swing.unitBeats = units[rnd() % 10];
        p.pattern = pats[rnd() % 20];
        p.patternStart = rnd() % 3 ? (std::isfinite(song) ? song - 0.37 : 0.0) : starts[rnd() % 11];
        p.loop = rnd() % 2 != 0;
        time.set(p, t);
        time.process(&L[i], &R[i], static_cast<int>(m));
        for (size_t k = i; k < i + m; ++k) {
            if (!std::isfinite(L[k]) || !std::isfinite(R[k])) finite = false;
            top = std::max(top, static_cast<double>(std::max(std::fabs(L[k]), std::fabs(R[k]))));
        }
        i += m;
    }
    CHECK(finite);
    CHECK(top < 3.0);
    CHECK(time.tailSamples() >= 0 && time.tailSamples() <= TimeCore::kRingFrames);
}

} // namespace

void timeTests() {
    timeData();
    timeEvaluator();
    timeFadeWeights();
    timeCurveNumbers();
    timeRuns();
    timeRest();
    timeLag();
    timeJumpEndsOnLine();
    timeStutter();
    timeLoop();
    timeReverseStutter();
    timeHalfTime();
    timeReverse();
    timeTapeStop();
    timeSlowDown();
    timeSpinRewind();
    timeGate();
    timeBreak();
    timeRolls();
    timeNeverAhead();
    timeNoClicks();
    timeLocate();
    timeChunking();
    timeStopped();
    timeAllocate();
    timeReset();
    timeRobust();
    timeJumpInFade();
    timeJumpInScheduledFade();
    timeCloseJumps();
    timeSwing();
    timeSwingCrossfade();
    timeDrunk();
    timePush();
    timeGrooveOff();
    timePatternOverGroove();
    timePatternLag();
    timePatternLoops();
    timeZoneLapses();
    timeGrooveNoClicks();
    timeTempoRamp();
    timeGrooveChunking();
    timeGrooveStopped();
    timeGrooveAllocate();
    timeGrooveReset();
    timeGrooveTail();
    timeGrooveRobust();
}
