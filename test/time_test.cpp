// Time's engine and its patterns (dsp/time.h) against the data surface.py generates: the library (15
// patterns, every source curve at or behind now, the volume within 0..1, a one-shot ending where it
// rejoins now or in silence), the evaluator's laws, then the playhead: at rest it passes the input bit
// for bit while the ring records; a lag reads the past (an impulse 125 ms later); a jump's 5 ms
// raised-cosine crossfade ends exactly on its line; a stutter repeats its first slice, half-time
// plays an octave down, reverse plays the beat before backwards, a tape stop falls in pitch to
// silence and rejoins now; gates and the break open and close in 1 ms edges that end on their lines.
// Then the contract: no sample depends on a later one (zero latency), no click through any pattern, a
// locate or a loop, the same output whatever the chunk sizes, a stopped transport, nothing allocated
// yet, wild parameters, reset.
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
    // a Smooth segment is a raised cosine: flat at both ends, the middle halfway, its speed a sine
    CHECK(near(gf::timeSourceAt(spin, 0.25), -0.5, 1e-9) && near(gf::timeSourceAt(spin, 0.5), -1.0, 1e-9));
    CHECK(near(gf::timeSpeedAt(spin, 0.0), 0.0, 1e-9) && near(gf::timeSpeedAt(spin, 0.25), -kPi, 1e-6));
    CHECK(near(gf::timeSourceAt(spin, 0.1), -0.5 * (1.0 - std::cos(kPi * 0.2)), 1e-9));
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
    {   // Spin-Back: backwards with a raised cosine's speed, fading out; silent at the end
        const int p = idx("Spin-Back");
        const size_t S = at(3.0), H = at(0.5);
        const Buf in = sine(300.0, static_cast<int>(S + 3 * H), 0.5f);
        const Buf out = render(in, p, 3.0);
        CHECK(peak(out, S + at(0.05), S + at(0.15)) > 0.2f);          // it sounds
        CHECK(peak(out, S + H - 300, S + H - 60) < 0.02f);            // and fades out
        CHECK(maxStep(out, 2, out.size()) <= clickBound(300.0, topSpeed(p)));
        // the playhead went back: the tone in the middle is higher than 300 Hz (speed up to pi)
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

} // namespace

void timeTests() {
    timeData();
    timeEvaluator();
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
}
