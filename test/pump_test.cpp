// gf::Pump (dsp/pump.h): Kickstart on MPC's grid. On a steady input (DC 0.5 on both sides, so the
// gain shows sample by sample) the ducks fall where the grid says: on the beats, a push before or
// after them, the late 16ths of a swing, through a locate, a loop, a tempo change and a stopped
// transport, never faster than the 1 ms edge. Then the input trigger (a kick per duck, no duck on a
// hat), the bands, the contract: defaults change nothing, extremes, block sizes, no clicks.
#include "signal.h"
#include "../dsp/pump.h"

#include <cmath>
#include <cstdio>
#include <limits>

namespace {

using namespace eft;
using gf::Pump;
using P = Pump::Params;

constexpr int kBeat = 22050;               // a beat at 120 BPM
constexpr double kMaxStep = 1.0 / 44.1;    // the 1 ms edge: a full scale takes 44.1 samples at the quickest
constexpr double kTol = 1e-6;              // the gain's rounding
constexpr int kMs = 44;                    // samples in a millisecond, rounded down

ef::Transport transport(double bpm, double beats, bool playing, bool valid = true) {
    ef::Transport t;
    t.bpm = bpm;
    t.beats = beats;
    t.playing = playing;
    t.valid = valid;
    return t;
}
ef::Transport playingAt(double bpm, double beats) { return transport(bpm, beats, true); }
ef::Transport stopped(double bpm) { return transport(bpm, 7.3, false); }   // MPC says where it stopped; the Pump runs on from 0

float val(const Buf& x, int i) { return x[static_cast<size_t>(i)]; }

bool same(const Buf& a, const Buf& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (!(a[i] == b[i])) return false;
    return true;
}
double maxDiff(const Buf& a, const Buf& b) {
    double m = 0.0;
    for (size_t i = 0; i < a.size(); ++i) m = std::max(m, static_cast<double>(std::fabs(a[i] - b[i])));
    return m;
}

// The module over L / R, the transport from transportAt(first sample of the chunk).
template <class F>
void renderWith(const P& p, Buf& L, Buf& R, F transportAt, int chunk = ef::kChunk) {
    Pump m;
    for (size_t pos = 0; pos < L.size(); pos += static_cast<size_t>(chunk)) {
        const int n = static_cast<int>(std::min(static_cast<size_t>(chunk), L.size() - pos));
        m.set(p, transportAt(pos));
        m.process(&L[pos], &R[pos], n);
    }
}
void render(const P& p, Buf& L, Buf& R, ef::Transport t, int chunk = ef::kChunk) {
    Pump m;
    run(m, p, L, R, t, chunk);
}
// DC 0.5 through the Pump, as the gain (0.5 scales exactly: the gain has every bit of the output).
Buf dcOut(const P& p, int n, ef::Transport t, int chunk = ef::kChunk) {
    Buf L(static_cast<size_t>(n), 0.5f), R = L;
    render(p, L, R, t, chunk);
    return L;
}
Buf toGain(Buf out) {
    for (float& v : out) v *= 2.0f;
    return out;
}
Buf dcGain(const P& p, int n, ef::Transport t, int chunk = ef::kChunk) { return toGain(dcOut(p, n, t, chunk)); }

float minOver(const Buf& g, int from, int to) {
    float m = 1e9f;
    for (int i = std::max(from, 0); i < std::min(to, static_cast<int>(g.size())); ++i) m = std::min(m, val(g, i));
    return m;
}
// True if the gain is `want` (within tol) on [from, to).
bool holds(const Buf& g, int from, int to, float want, float tol = 1e-6f) {
    for (int i = std::max(from, 0); i < std::min(to, static_cast<int>(g.size())); ++i)
        if (!(std::fabs(val(g, i) - want) <= tol)) return false;
    return true;
}
double biggestStep(const Buf& g) { return maxStep(g); }

P hardPump() {   // 1/4, Hard, depth 1, recovery half a beat
    P p;
    p.rateBeats = 1.0;
    p.curve = gf::PC_HARD;
    p.depth = 1.0f;
    p.length = 0.5f;
    return p;
}

// --- hits ----------------------------------------------------------------------------------------

// x += a kick at `at`: 55 Hz, 60 ms decay (the detector's own test material).
void addKick(Buf& x, int at, float amp) {
    const double tau = 0.060 * ef::kRate;
    for (int i = 0; at + i < static_cast<int>(x.size()) && i < 10 * tau; ++i)
        x[static_cast<size_t>(at + i)] += amp * static_cast<float>(std::sin(2.0 * kPi * 55.0 * i / ef::kRate) * std::exp(-i / tau));
}
// x += a hat: noise with its lows taken out (a second difference: a hat has nothing under 5 kHz),
// 20 ms decay, `amp` at the loudest.
void addHat(Buf& x, int at, float amp, uint32_t seed) {
    const Buf n = whiteNoise(2048, 1.0f, seed);
    const double tau = 0.020 * ef::kRate;
    for (int i = 2; i < 2048 && at + i < static_cast<int>(x.size()); ++i)
        x[static_cast<size_t>(at + i)] += amp * 0.25f * (n[static_cast<size_t>(i)] - 2.0f * n[static_cast<size_t>(i - 1)] + n[static_cast<size_t>(i - 2)]) *
                                         static_cast<float>(std::exp(-i / tau));
}
// The same with white noise: lows and all.
void addNoiseHat(Buf& x, int at, float amp, uint32_t seed) {
    const Buf n = whiteNoise(2048, 1.0f, seed);
    const double tau = 0.020 * ef::kRate;
    for (int i = 0; i < 2048 && at + i < static_cast<int>(x.size()); ++i)
        x[static_cast<size_t>(at + i)] += amp * n[static_cast<size_t>(i)] * static_cast<float>(std::exp(-i / tau));
}

// out / in where the input is big enough to tell (a hit is a sine: it crosses zero); elsewhere the
// last value. 1 before the first.
Buf gainFrom(const Buf& in, const Buf& out, float floor = 0.02f) {
    Buf g(in.size(), 1.0f);
    float last = 1.0f;
    for (size_t i = 0; i < in.size(); ++i) {
        if (std::fabs(in[i]) >= floor) last = out[i] / in[i];
        g[i] = last;
    }
    return g;
}
// The first sample of every dive of the gain under `level` (a dive less than 5 ms after the last
// sample under it is the same dive).
std::vector<int> dives(const Buf& g, float level = 0.2f) {
    std::vector<int> at;
    int last = -100000;
    for (int i = 0; i < static_cast<int>(g.size()); ++i) {
        if (val(g, i) < level) {
            if (i - last > 5 * kMs) at.push_back(i);
            last = i;
        }
    }
    return at;
}

// --- tests ---------------------------------------------------------------------------------------

void pumpRests() {   // default Params: output == input bit for bit, any band and trigger, playing or stopped
    const Buf inL = whiteNoise(20000, 0.5f, 1), inR = whiteNoise(20000, 0.5f, 2);
    for (int band = 0; band < 3; ++band)
        for (int trigger = 0; trigger < 2; ++trigger)
            for (int playing = 0; playing < 2; ++playing) {
                P p;
                p.band = band;
                p.trigger = trigger;
                Buf L = inL, R = inR;
                render(p, L, R, playing ? playingAt(120.0, 1.3) : stopped(120.0));
                CHECK(same(L, inL) && same(R, inR));
                // And with depth 0 whatever else is set (curve, push, swing...): still nothing happens.
                P q = p;
                q.curve = gf::PC_SNAP;
                q.length = 0.1f;
                q.pushMs = 12.0f;
                q.crossoverHz = 250.0f;
                q.swing = {0.7f, 0.25};
                Buf L2 = inL, R2 = inR;
                render(q, L2, R2, playing ? playingAt(120.0, 1.3) : stopped(120.0));
                CHECK(same(L2, inL) && same(R2, inR));
            }
    // mix 0 is the same as depth 0.
    P p = hardPump();
    p.mix = 0.0f;
    p.band = Pump::kLows;
    Buf L = inL, R = inR;
    render(p, L, R, playingAt(120.0, 0.0));
    CHECK(same(L, inL) && same(R, inR));
}

void pumpGrid() {   // 1/4, Hard, depth 1, length 0.5: a duck on every beat, recovered from 250 ms on
    const P p = hardPump();
    const Buf g = dcGain(p, 8 * kBeat, playingAt(120.0, 0.0));
    bool bottoms = true, recovered = true, early = true;
    for (int k = 0; k < 7; ++k) {
        const int at = k * kBeat;
        bottoms = bottoms && minOver(g, at - kMs, at + kMs) < 0.005f;
        recovered = recovered && holds(g, at + kBeat / 2, at + kBeat - 2 * kMs, 1.0f);   // 250 ms to 2 ms before the next
        if (k > 0) {   // the drop starts 1 ms early: halfway down 22 samples before the beat, still at unity 46 before
            early = early && std::fabs(val(g, at - 22) - 0.5f) < 0.02f && val(g, at - 46) == 1.0f;
        }
    }
    CHECK(bottoms);
    CHECK(recovered);
    CHECK(early);
    CHECK(biggestStep(g) <= kMaxStep + kTol);
    // Both sides alike, and the same off the grid's start: beat 2.5 in, the next bottom is the 3rd beat.
    const Buf g2 = dcGain(p, 3 * kBeat, playingAt(120.0, 2.5));
    CHECK(minOver(g2, kBeat / 2 - kMs, kBeat / 2 + kMs) < 0.005f);
    CHECK(holds(g2, 0, kBeat / 2 - 2 * kMs, 1.0f));   // 2.5 beats in: 0.5 beat after the bottom: recovered
}

void pumpPush() {   // push +20: the bottoms 20 ms after the beats; push -10: 10 ms before; +30
    for (const float push : {20.0f, -10.0f, 30.0f}) {
        P p = hardPump();
        p.pushMs = push;
        const Buf g = dcGain(p, 8 * kBeat, playingAt(120.0, 0.0));
        const int shift = static_cast<int>(std::lround(push * 44.1f));
        bool bottoms = true, off = true;
        for (int k = 1; k < 7; ++k) {
            const int at = k * kBeat;
            bottoms = bottoms && minOver(g, at + shift - kMs, at + shift + kMs) < 0.005f;
            // Not on the beat itself: after a push it is still at unity (or, before it, coming back).
            off = off && (push > 0.0f ? val(g, at) == 1.0f : val(g, at) > 0.05f);
        }
        CHECK(bottoms);
        CHECK(off);
        CHECK(biggestStep(g) <= kMaxStep + kTol);
    }
}

void pumpCurveShape() {   // between bottom and recovery the gain is 1 - depth (1 - pumpLevel(curve, x)) (x: how far through)
    for (const int curve : {static_cast<int>(gf::PC_SMOOTH), static_cast<int>(gf::PC_LONG), static_cast<int>(gf::PC_PUNCH), static_cast<int>(gf::PC_HOLD)}) {
        P p;
        p.rateBeats = 1.0;
        p.curve = curve;
        p.depth = 0.8f;
        p.length = 0.5f;
        const Buf g = dcGain(p, 8 * kBeat, playingAt(120.0, 0.0));
        const double recovery = 0.5 * kBeat;
        double worst = 0.0;
        for (int k = 1; k < 7; ++k)
            for (int i = 0; i <= static_cast<int>(recovery); ++i) {
                const float x = static_cast<float>(i / recovery);
                const double want = 1.0 - 0.8 * (1.0 - gf::pumpLevel(curve, x));
                worst = std::max(worst, std::fabs(val(g, k * kBeat + i) - want));
            }
        CHECK(worst < 0.01);
        if (!(worst < 0.01)) std::printf("  curve %s: the gain is off the curve by %g\n", gf::kPumpCurveNames[curve], worst);
        CHECK(std::fabs(minOver(g, kBeat - kMs, kBeat + kMs) - 0.2f) < 0.005f);   // depth 0.8: a fifth is left at the bottom
    }
}

void pumpSwing() {   // 1/16 at swing 66% (unit 1/16): the odd 16ths' bottoms 40 ms late, the even ones on time
    P p = hardPump();
    p.rateBeats = 0.25;
    p.length = 0.4f;   // 50 ms
    p.swing = {0.66f, 0.25};
    const Buf g = dcGain(p, 4 * kBeat, playingAt(120.0, 0.0));
    const double sixteenth = 0.125 * ef::kRate;   // 5512.5 samples
    bool onTime = true, late = true, clear = true;
    for (int k = 1; k < 14; ++k) {
        const int nominal = static_cast<int>(std::lround(k * sixteenth));
        if (k % 2 == 0) {
            onTime = onTime && minOver(g, nominal - kMs, nominal + kMs) < 0.02f;
        } else {
            late = late && minOver(g, nominal + 40 * 44 + 1 - kMs, nominal + 40 * 44 + 1 + kMs) < 0.02f;   // 40 ms = 1764 samples
            // and on the nominal beat the gain is up (the previous bottom is 125 ms back, the next 40 ms on)
            clear = clear && val(g, nominal) > 0.99f;
        }
    }
    CHECK(onTime);
    CHECK(late);
    CHECK(clear);
    CHECK(biggestStep(g) <= kMaxStep + kTol);
    // On 1/8 the unit is the 8th: 66% puts the second 8th of a beat 80 ms late. The 16ths between move with it.
    P q = p;
    q.rateBeats = 0.5;
    q.swing = {0.66f, 0.5};
    const Buf h = dcGain(q, 4 * kBeat, playingAt(120.0, 0.0));
    CHECK(minOver(h, kBeat / 2 + 80 * 44 + 3 - kMs, kBeat / 2 + 80 * 44 + 3 + kMs) < 0.02f);
    CHECK(minOver(h, kBeat - kMs, kBeat + kMs) < 0.005f);
    // 50% is straight.
    P s = p;
    s.swing = {0.5f, 0.25};
    const Buf straight = dcGain(s, 4 * kBeat, playingAt(120.0, 0.0));
    CHECK(minOver(straight, static_cast<int>(sixteenth) - kMs, static_cast<int>(sixteenth) + kMs) < 0.02f);
}

void pumpEdges() {   // no gain edge is faster than 1 ms: whatever the curve, length, rate, depth
    for (int curve = 0; curve < gf::PC_COUNT; ++curve)
        for (const float length : {0.05f, 0.3f, 1.0f})
            for (const double rate : {0.25, 1.0}) {
                P p;
                p.rateBeats = rate;
                p.curve = curve;
                p.depth = 1.0f;
                p.length = length;
                const Buf g = dcGain(p, 3 * kBeat, playingAt(150.0, 0.0));
                CHECK(biggestStep(g) <= kMaxStep + kTol);
                if (!(biggestStep(g) <= kMaxStep + kTol))
                    std::printf("  %s, length %.2f, rate %.2f: a gain step of %g\n", gf::kPumpCurveNames[curve], length, rate, biggestStep(g));
            }
    // The edge is the 1 ms, not much slower: Hold at length 1 on 1/16 at 150 BPM drops 1 -> 0 in 44 samples.
    P p;
    p.rateBeats = 0.25;
    p.curve = gf::PC_HOLD;
    p.depth = 1.0f;
    p.length = 1.0f;
    const Buf g = dcGain(p, 4 * kBeat, playingAt(150.0, 0.0));
    CHECK(biggestStep(g) >= 0.95 * kMaxStep);
    CHECK(minOver(g, 0, static_cast<int>(g.size())) < 0.001f);
}

int firstUnder(const Buf& g, int from, int to, float level) {
    for (int i = std::max(from, 0); i < std::min(to, static_cast<int>(g.size())); ++i)
        if (val(g, i) < level) return i;
    return -1;
}

void pumpInput() {   // Input trigger: kicks at random, off-grid times: a bottom a few ms after each one's start, and a push later
    // Gaps of 150 to 400 ms, 12 kicks; the Pump is told nothing about the grid (stopped, a tempo at odds with the kicks).
    uint32_t seed = 3;
    std::vector<int> starts;
    int at = 9000;
    for (int k = 0; k < 12; ++k) {
        starts.push_back(at);
        seed = seed * 1664525u + 1013904223u;
        at += 6615 + static_cast<int>((seed >> 8) % 11025u);
    }
    Buf in(static_cast<size_t>(at + 20000), 0.0f);
    for (int s : starts) addKick(in, s, 0.8f);
    P p = hardPump();
    p.trigger = Pump::kInput;
    p.length = 0.3f;   // 150 ms
    std::vector<int> bottom[2];
    int pass = 0;
    for (const float push : {0.0f, 15.0f}) {
        p.pushMs = push;
        Buf L = in, R = in;
        render(p, L, R, stopped(93.0));
        const Buf g = gainFrom(in, L);
        const std::vector<int> found = dives(g);
        CHECK(found.size() == starts.size());   // one duck per kick: no more, none missed
        for (size_t k = 0; k < starts.size() && k < found.size(); ++k) {
            const int s = starts[k];
            CHECK(found[k] >= s);   // no look-ahead: never before the kick
            // The bottom: the detector's find (2 ms, a kick right after another takes longer: the slow envelope still
            // remembers it), the 150 Hz low-pass in front of it (1.5 ms) and the 1 ms drop.
            const int b = firstUnder(g, s, s + (static_cast<int>(push) + 6) * kMs, 0.01f);
            CHECK(b >= 0);
            bottom[pass].push_back(b);
            if (push == 0.0f) {
                CHECK(b >= 0 && b - s <= 6 * kMs);
            } else if (k == 0 || s - starts[k - 1] >= 9700) {   // the bottom `push` later; up to the drop the gain is 1
                CHECK(val(g, s + 10 * kMs) > 0.99f);
            }
            // The drop is the 1 ms edge: from 95% down to 1% in 41.5 samples (give or take where the hit is too small to read the gain).
            const int top = firstUnder(g, s, s + (static_cast<int>(push) + 6) * kMs, 0.95f);
            CHECK(b - top >= 39 && b - top <= 48);
        }
        // Every edge is the 1 ms edge or slower, where the gain can be read (neighbouring samples both loud enough).
        double steepestGain = 0.0;
        for (size_t i = 1; i < in.size(); ++i)
            if (std::fabs(in[i]) >= 0.1f && std::fabs(in[i - 1]) >= 0.1f) steepestGain = std::max(steepestGain, static_cast<double>(std::fabs(g[i] - g[i - 1])));
        CHECK(steepestGain <= kMaxStep + 1e-5);
        ++pass;
    }
    // The same onsets, so the push moves each bottom by itself less the drop it replaces: 15 ms - 1 ms = 617 samples.
    for (size_t k = 0; k < starts.size() && k < bottom[0].size() && k < bottom[1].size(); ++k)
        CHECK(bottom[1][k] - bottom[0][k] >= 614 && bottom[1][k] - bottom[0][k] <= 624);
    // Played in blocks of 1, 7 and 32 samples: the same ducks.
    p.pushMs = 5.0f;
    Buf a = in, b = in, c = in, ar = in, br = in, cr = in;
    render(p, a, ar, stopped(93.0), 32);
    render(p, b, br, stopped(93.0), 7);
    render(p, c, cr, stopped(93.0), 1);
    CHECK(maxDiff(a, b) <= 1e-6 && maxDiff(a, c) <= 1e-6);
}

void pumpHats() {   // Input trigger on a drum bus: kicks on the beat, hats 6 dB under them on the offbeats: a duck per kick, none on a hat
    const int lead = 8820, beats = 8;
    Buf in(static_cast<size_t>(lead + beats * kBeat), 0.0f);
    for (int k = 0; k < beats; ++k) {
        addKick(in, lead + k * kBeat, 0.8f);
        addHat(in, lead + k * kBeat + kBeat / 2, 0.4f, 10 + static_cast<uint32_t>(k));
    }
    P p = hardPump();
    p.trigger = Pump::kInput;
    p.length = 0.3f;
    Buf L = in, R = in;
    render(p, L, R, stopped(120.0));
    const Buf g = gainFrom(in, L);
    const std::vector<int> found = dives(g);
    CHECK(static_cast<int>(found.size()) == beats);
    for (int k = 0; k < beats && k < static_cast<int>(found.size()); ++k) {
        CHECK(found[static_cast<size_t>(k)] >= lead + k * kBeat && found[static_cast<size_t>(k)] <= lead + k * kBeat + 4 * kMs);
        // The hat goes through whole: its first 20 ms are at unity.
        const int hat = lead + k * kBeat + kBeat / 2;
        CHECK(minOver(g, hat, hat + 20 * kMs) > 0.99f);
    }
    // The audio is the input's, ducked: the hat is untouched, sample for sample.
    bool hatsWhole = true;
    for (int k = 0; k < beats; ++k) {
        const int hat = lead + k * kBeat + kBeat / 2;
        for (int i = hat; i < hat + 20 * kMs; ++i) hatsWhole = hatsWhole && val(L, i) == val(in, i);
    }
    CHECK(hatsWhole);
    // Hats of white noise (the lows of noise are in range of the filter): still no duck on one.
    Buf in2(in.size(), 0.0f);
    for (int k = 0; k < beats; ++k) {
        addKick(in2, lead + k * kBeat, 0.8f);
        addNoiseHat(in2, lead + k * kBeat + kBeat / 2, 0.4f, 30 + static_cast<uint32_t>(k));
    }
    Buf L2 = in2, R2 = in2;
    render(p, L2, R2, stopped(120.0));
    CHECK(static_cast<int>(dives(gainFrom(in2, L2)).size()) == beats);
}

// A mix of two sines through the Pump parked at a long bottom (1 bar, Hold, length 1: 1.2 s down).
void pumpBand() {
    const int n = 52000;
    const Buf low = sine(50.0, n, 0.25f), high = sine(5000.0, n, 0.25f);
    Buf mixed(low.size());
    for (size_t i = 0; i < mixed.size(); ++i) mixed[i] = low[i] + high[i];
    struct Case {
        int band;
        double lowDb, highDb;   // limits: <= for the ducked band, within 1 dB of 0 for the other (NaN: either)
        bool lowDucked, highDucked;
    };
    for (const Case c : {Case{Pump::kLows, 0, 0, true, false}, Case{Pump::kHighs, 0, 0, false, true}, Case{Pump::kFull, 0, 0, true, true}}) {
        P p;
        p.rateBeats = 4.0;
        p.curve = gf::PC_HOLD;
        p.depth = 1.0f;
        p.length = 1.0f;
        p.band = c.band;
        Buf L = mixed, R = mixed;
        render(p, L, R, playingAt(120.0, 0.0));
        const double lowDb = db(magnitude(L, 50.0, 13230, 44100) / 0.25), highDb = db(magnitude(L, 5000.0, 13230, 44100) / 0.25);
        CHECK(c.lowDucked ? lowDb <= -30.0 : std::fabs(lowDb) <= 1.0);
        CHECK(c.highDucked ? highDb <= -30.0 : std::fabs(highDb) <= 1.0);
        if (!(c.lowDucked ? lowDb <= -30.0 : std::fabs(lowDb) <= 1.0) || !(c.highDucked ? highDb <= -30.0 : std::fabs(highDb) <= 1.0))
            std::printf("  band %d: 50 Hz %.2f dB, 5 kHz %.2f dB\n", c.band, lowDb, highDb);
        CHECK(same(L, R));
        // Back up after the recovery (1.2 s to 2 s): the whole signal returns within a dB, whichever band it was.
        Buf big(static_cast<size_t>(4 * kBeat), 0.0f);
        const Buf a = sine(50.0, 4 * kBeat, 0.25f), b = sine(5000.0, 4 * kBeat, 0.25f);
        for (size_t i = 0; i < big.size(); ++i) big[i] = a[i] + b[i];
        Buf BL = big, BR = big;
        render(p, BL, BR, playingAt(120.0, 0.0));
        CHECK(std::fabs(db(magnitude(BL, 50.0, 80000, 88000) / 0.25)) <= 1.0);
        CHECK(std::fabs(db(magnitude(BL, 5000.0, 80000, 88000) / 0.25)) <= 1.0);
    }
}

void pumpMix() {   // mix 0.5, depth 1: the bottom's gain is 0.5
    P p = hardPump();
    p.mix = 0.5f;
    const Buf g = dcGain(p, 8 * kBeat, playingAt(120.0, 0.0));
    bool ok = true;
    for (int k = 0; k < 7; ++k) {
        const float low = minOver(g, k * kBeat - kMs, k * kBeat + kMs);
        ok = ok && std::fabs(low - 0.5f) < 0.005f && holds(g, k * kBeat + kBeat / 2, k * kBeat + kBeat - 2 * kMs, 1.0f);
    }
    CHECK(ok);
    CHECK(biggestStep(g) <= 0.5 * kMaxStep + kTol);
    // Depth 0.5 and mix 0.5 together: a quarter.
    p.depth = 0.5f;
    CHECK(std::fabs(minOver(dcGain(p, 3 * kBeat, playingAt(120.0, 0.0)), kBeat - kMs, kBeat + kMs) - 0.75f) < 0.005f);
}

void pumpStopped() {   // MPC stopped: the ducks keep coming every 500 ms at 120 BPM; starting to play doesn't click
    const P p = hardPump();
    const Buf g = dcGain(p, 6 * kBeat, stopped(120.0));
    bool every = true, between = true;
    for (int k = 0; k < 6; ++k) {
        every = every && minOver(g, k * kBeat - kMs, k * kBeat + kMs) < 0.005f;
        if (k < 5) between = between && holds(g, k * kBeat + kBeat / 2, (k + 1) * kBeat - 2 * kMs, 1.0f);   // and recovered in between
    }
    CHECK(every);
    CHECK(between);
    CHECK(biggestStep(g) <= kMaxStep + kTol);
    // Playing, then stopped: the ducks carry on from where the song was (here beat 2.4 at the stop), whatever MPC says the position is.
    const int stop = 32 * 1378;   // 1.0 s, on a chunk boundary
    Buf L0(static_cast<size_t>(5 * kBeat), 0.5f), R0 = L0;
    renderWith(p, L0, R0, [&](size_t pos) { return static_cast<int>(pos) < stop ? playingAt(120.0, 2.4 + static_cast<double>(pos) / kBeat) : stopped(120.0); });
    const Buf g0 = toGain(L0);
    bool carries = true;
    for (int k = 0; k < 4; ++k) {   // beats 3, 4, 5, 6 of the song: 0.6 beat in, then every beat
        const int at = static_cast<int>(std::lround((3.0 + k - 2.4) * kBeat));
        carries = carries && minOver(g0, at - kMs, at + kMs) < 0.005f;
        if (at + kBeat < static_cast<int>(g0.size())) carries = carries && holds(g0, at + kBeat / 2, at + kBeat - 2 * kMs, 1.0f);
    }
    CHECK(carries);
    CHECK(biggestStep(g0) <= kMaxStep + kTol);

    // At 90 BPM they come every 666.7 ms.
    const Buf g90 = dcGain(p, 5 * kBeat, stopped(90.0));
    const int beat90 = static_cast<int>(std::lround(60.0 / 90.0 * ef::kRate));
    CHECK(minOver(g90, 2 * beat90 - kMs, 2 * beat90 + kMs) < 0.005f && minOver(g90, 3 * beat90 - kMs, 3 * beat90 + kMs) < 0.005f);

    // Stopped for 1.52 s (just past the 4th bottom, 3.04 beats), then playing from beat 3.3.
    const int total = 4 * kBeat, play = 32 * 2098;   // 1.52 s, on a chunk boundary
    Buf L(static_cast<size_t>(total), 0.5f), R = L;
    renderWith(p, L, R, [&](size_t pos) { return static_cast<int>(pos) < play ? stopped(120.0) : playingAt(120.0, 3.3 + (static_cast<double>(pos) - play) / kBeat); });
    const double before = steepest(L, static_cast<size_t>(play) - kBeat / 2, static_cast<size_t>(play));
    const double after = steepest(L, static_cast<size_t>(play) + 4410, static_cast<size_t>(play) + 4410 + kBeat);
    const double now = steepest(L, static_cast<size_t>(play) - 1, static_cast<size_t>(play) + 4410);
    CHECK(now < std::max(0.01, 1.25 * std::max(before, after)));
    // And then on the song position's beats: beat 4 is (4 - 3.3) beats after the start.
    const int beat4 = play + static_cast<int>(std::lround((4.0 - 3.3) * kBeat));
    CHECK(minOver(toGain(L), beat4 - kMs, beat4 + kMs) < 0.005f);
}

// The first sample (rounded) at which the song position crosses each whole beat, per chunk: where
// a bottom is due. transportAt(pos) gives the transport at the chunk's first sample.
template <class F>
std::vector<int> beatSamples(F transportAt, size_t total, double bpm) {
    std::vector<int> at;
    for (size_t pos = 0; pos + ef::kChunk <= total; pos += ef::kChunk) {
        const double b0 = transportAt(pos).beats, b1 = b0 + ef::kChunk * bpm / 60.0 / ef::kRate;
        for (double m = std::ceil(b0); m < b1; m += 1.0) at.push_back(static_cast<int>(pos) + static_cast<int>(std::lround((m - b0) * 60.0 / bpm * ef::kRate)));
    }
    return at;
}

void pumpFollowsJumps() {   // a locate from beat 3.3 to 9.8, then a loop of 1.5 beats: the next bottom on the grid, no click
    const P p = hardPump();
    const size_t locate = 32 * 2274, total = 16 * kBeat;   // the locate at beat 3.30013 (120 BPM), 3.3 s in
    const double loopFrom = 12.0, loopLen = 1.5;
    const auto transportAt = [&](size_t pos) {
        if (pos < locate) return playingAt(120.0, static_cast<double>(pos) / kBeat);
        double b = 9.8 + (static_cast<double>(pos) - static_cast<double>(locate)) / kBeat;
        if (b >= loopFrom + loopLen) b = loopFrom + std::fmod(b - loopFrom, loopLen);
        return playingAt(120.0, b);
    };
    Buf out(total, 0.5f), outR = out;
    renderWith(p, out, outR, transportAt);
    const Buf g = toGain(out);
    // Every beat the song position crosses (but the loop's start, where the jump itself is) has its bottom within a sample or two.
    const std::vector<int> due = beatSamples(transportAt, total, 120.0);
    int checked = 0;
    bool onGrid = true;
    for (const int at : due) {
        if (at < 2 * kBeat || (at > static_cast<int>(locate) - 100 && at < static_cast<int>(locate) + 100)) continue;
        // A jump back to the loop's start lands on a bottom (beat 12): 1 ms to ramp down, not a bottom to place.
        const double beats = transportAt(static_cast<size_t>(at - at % 32)).beats;
        if (std::fabs(beats - loopFrom) < 0.01) continue;
        onGrid = onGrid && minOver(g, at - kMs, at + kMs + 1) < 0.015f;
        ++checked;
    }
    CHECK(onGrid);
    CHECK(checked >= 6);
    // No click: after the locate and after each wrap, the steepest 0.1 s is within the rule.
    const double settled = steepest(out, static_cast<size_t>(kBeat), locate - 1);
    CHECK(settled > 0.5 * 0.5 * kMaxStep);   // the reference does have its edges
    std::vector<size_t> events = {locate};
    for (size_t pos = locate + 32; pos < total; pos += 32)
        if (transportAt(pos).beats < transportAt(pos - 32).beats) events.push_back(pos);
    CHECK(events.size() >= 4);
    bool clean = true;
    for (const size_t e : events) {
        const double now = steepest(out, e - 1, std::min(e + 4410, total));
        const bool ok = now <= std::max(0.01, 1.25 * settled);
        clean = clean && ok;
        if (!ok) std::printf("  jump at %zu: %.5f a sample, settled %.5f\n", e, now, settled);
    }
    CHECK(clean);
    // The locate lands on the grid: bottom of beat 10 is (10 - 9.8) beats after it.
    CHECK(minOver(g, static_cast<int>(locate) + 4410 - kMs, static_cast<int>(locate) + 4410 + kMs) < 0.005f);
}

void pumpTempoChange() {   // 120 -> 128 BPM while playing: the bottoms stay on the song position's beats, no click
    const P p = hardPump();
    const size_t change = 32 * 1400, total = 8 * kBeat;   // beat 2.03 at 120 BPM
    const double b1 = static_cast<double>(change) / kBeat;
    const auto transportAt = [&](size_t pos) {
        if (pos < change) return playingAt(120.0, static_cast<double>(pos) / kBeat);
        return playingAt(128.0, b1 + (static_cast<double>(pos) - static_cast<double>(change)) * 128.0 / 60.0 / ef::kRate);
    };
    Buf out(total, 0.5f), outR = out;
    renderWith(p, out, outR, transportAt);
    const Buf g = toGain(out);
    bool onGrid = true;
    int checked = 0;
    for (const int at : beatSamples(transportAt, total, 124.0)) {
        if (at < static_cast<int>(kBeat / 2)) continue;
        onGrid = onGrid && minOver(g, at - kMs, at + kMs + 1) < 0.015f;
        ++checked;
    }
    CHECK(onGrid);
    CHECK(checked >= 6);
    const double before = steepest(out, change - kBeat, change), after = steepest(out, change + 4410, change + 4410 + kBeat);
    CHECK(steepest(out, change - 1, change + 4410) <= std::max(0.01, 1.25 * std::max(before, after)));
    CHECK(biggestStep(g) <= kMaxStep + kTol);
}

void pumpShortestSteps() {   // 1/16, 150 BPM, swing 75%, push -10, length 0.05: a bottom per 16th in order, the 1 ms edge holds
    P p;
    p.rateBeats = 0.25;
    p.curve = gf::PC_SMOOTH;
    p.depth = 1.0f;
    p.length = 0.05f;
    p.pushMs = -10.0f;
    p.swing = {0.75f, 0.25};
    const int steps = 24;
    const Buf g = dcGain(p, 12 * kBeat / 2, playingAt(150.0, 0.0));   // 2.4 s: 24 sixteenths of 100 ms
    // 150 BPM: a 16th is 4410 samples; at 75% the first of each pair lasts 150 ms, the second 50 ms.
    // Starts at 0, 6615, 8820, 15435, ...; the bottoms 10 ms (441) before.
    std::vector<int> want;
    for (int k = 0; k <= steps; ++k) want.push_back((k / 2) * 8820 + (k % 2) * 6615 - 441);
    const std::vector<int> found = dives(g, 0.05f);
    CHECK(static_cast<int>(found.size()) >= steps - 1);   // step 0's bottom is before the start: the next 23 are all there
    bool inOrder = true, placed = true;
    for (int k = 1; k < steps; ++k) {
        placed = placed && minOver(g, want[static_cast<size_t>(k)] - kMs, want[static_cast<size_t>(k)] + kMs) < 0.005f;
        // each in its own window: between this bottom and the next nothing goes under 0.05 but this one's own dive
        const int to = want[static_cast<size_t>(k + 1)] - 3 * kMs;
        inOrder = inOrder && holds(g, want[static_cast<size_t>(k)] + 2 * 44 * 3, to, 1.0f);   // up again 12 ms on (5 ms recovery), until the next drop
    }
    CHECK(placed);
    CHECK(inOrder);
    CHECK(biggestStep(g) <= kMaxStep + kTol);
}

// A tone on the crossover is the worst case for the bands: the split's allpass turns a 120 Hz sine
// by half a cycle there, so a switch that is not crossfaded would flip it (a step of 1.6). On a long
// bottom (1 bar, Hold: 1.2 s down) the band, then the depth, change every 0.15 s.
void pumpBandSwitch() {
    P p;
    p.rateBeats = 4.0;
    p.curve = gf::PC_HOLD;
    p.depth = 0.5f;
    p.length = 1.0f;
    p.crossoverHz = 120.0f;
    const size_t seg = 207 * ef::kChunk;   // 0.15 s
    const int changes = 7;
    Buf L = sine(120.0, static_cast<int>(changes * seg), 0.8f), R = L;
    Pump m;
    double settledBefore = 0.0;
    bool clean = true;
    for (int s = 0; s < changes; ++s) {
        switch (s) {   // the change at the start of segment s
        case 1: p.band = Pump::kLows; break;
        case 2: p.band = Pump::kHighs; break;
        case 3: p.band = Pump::kLows; break;
        case 4: p.depth = 0.0f; break;   // the split goes out
        case 5: p.depth = 0.5f; break;   // and comes back
        case 6: p.band = Pump::kFull; break;
        default: break;
        }
        const size_t from = static_cast<size_t>(s) * seg;
        for (size_t i = from; i < from + seg; i += ef::kChunk) {
            m.set(p, playingAt(120.0, static_cast<double>(i) / kBeat));
            m.process(&L[i], &R[i], ef::kChunk);
        }
        const double settled = steepest(L, from + seg - 2205, from + seg);
        if (s > 0) {
            const double first = steepest(L, from - 1, from + 4410);
            const bool ok = first < std::max(0.01, 1.25 * std::max(settled, settledBefore));
            clean = clean && ok;
            if (!ok) std::printf("  band change %d: %.5f a sample after it, settled %.5f before, %.5f after\n", s, first, settledBefore, settled);
        }
        settledBefore = settled;
    }
    CHECK(clean);
    CHECK(allFinite(L));
}

// Input picked, or turned up, while a tone is playing: the detector was not running, or not heard
// from; it must not take the tone for a hit (the pump stays at unity), and a hit after that ducks.
void pumpInputSwitch() {
    const int n = 32 * 1000;
    Buf tone = sine(100.0, n, 0.1f);
    addKick(tone, 32 * 800, 0.8f);
    for (int how = 0; how < 3; ++how) {
        P p = hardPump();
        p.length = 0.3f;
        Buf L = tone, R = tone;
        Pump m;
        const size_t at = 32 * 400;   // 0.29 s in
        for (size_t i = 0; i < static_cast<size_t>(n); i += ef::kChunk) {
            if (how == 0) {   // Grid at depth 0 until then: the detector never ran
                p.trigger = i >= at ? Pump::kInput : Pump::kGrid;
                p.depth = i >= at ? 1.0f : 0.0f;
            } else if (how == 1) {   // Input all along, the depth turned up
                p.trigger = Pump::kInput;
                p.depth = i >= at ? 1.0f : 0.0f;
            } else {          // Input for a while (the detector warm), Grid, Input again: it holds a stale state, not a reset one
                p.trigger = i < 32 * 100 || i >= at ? Pump::kInput : Pump::kGrid;
                p.depth = i >= at ? 1.0f : 0.0f;
            }
            m.set(p, stopped(120.0));
            m.process(&L[i], &R[i], ef::kChunk);
        }
        const Buf g = gainFrom(tone, L);
        bool quiet = true;
        for (size_t i = at; i < 32 * 790; ++i) quiet = quiet && L[i] == tone[i];   // not one sample touched until the hit
        CHECK(quiet);
        CHECK(minOver(g, 32 * 800, 32 * 800 + 7 * kMs) < 0.05f);   // and the hit does duck
    }
}

// Random numbers.
struct Rng {
    uint32_t s = 20241025;
    uint32_t next() { return ef::xorshift(s); }
    bool coin() { return next() & 1u; }
};

void pumpExtremes() {   // every Params field at both ends and past them, NaN and 1e30 in the input: finite, never above the input's level
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    Rng r;
    bool finite = true, bounded = true;
    int banded = 0;
    for (int trial = 0; trial < 300; ++trial) {
        auto pick = [&r](float a, float b) { return r.coin() ? a : b; };
        P p;
        p.rateBeats = r.coin() ? 0.25 : 4.0;
        p.curve = static_cast<int>(r.next() % gf::PC_COUNT);
        p.depth = pick(0.0f, 1.0f);
        p.length = pick(0.05f, 1.0f);
        p.pushMs = pick(-10.0f, 30.0f);
        p.trigger = static_cast<int>(r.next() % 2);
        p.sensitivity = pick(0.0f, 1.0f);
        p.band = static_cast<int>(r.next() % 3);
        p.crossoverHz = pick(60.0f, 300.0f);
        p.mix = pick(0.0f, 1.0f);
        p.swing = {pick(0.5f, 0.75f), r.coin() ? 0.25 : 0.5};
        if (trial % 4 == 0) {   // past the ends
            p.rateBeats = r.coin() ? -1.0 : (r.coin() ? 1e9 : static_cast<double>(nan));
            p.curve = r.coin() ? -5 : 99;
            p.depth = pick(-1.0f, 7.0f);
            p.length = pick(0.0f, 9.0f);
            p.pushMs = pick(-1e6f, 1e6f);
            p.trigger = static_cast<int>(r.next() % 7) - 2;
            p.sensitivity = pick(-3.0f, 9.0f);
            p.band = static_cast<int>(r.next() % 9) - 3;
            p.crossoverHz = pick(0.0f, 1e6f);
            p.mix = pick(-2.0f, 5.0f);
            p.swing = {pick(-3.0f, 9.0f), r.coin() ? -1.0 : 1e300};
        }
        if (trial % 7 == 3) {   // not a number, infinite
            p.depth = nan;
            p.length = nan;
            p.pushMs = inf;
            p.sensitivity = nan;
            p.crossoverHz = nan;
            p.mix = nan;
            p.swing.amount = nan;
        }
        Buf inL = whiteNoise(8192, 100.0f, static_cast<uint32_t>(2 * trial + 1)), inR = whiteNoise(8192, 100.0f, static_cast<uint32_t>(2 * trial + 2));
        for (size_t i : {100u, 2000u, 3001u, 5000u}) {
            inL[i] = i == 100 ? nan : (i == 2000 ? inf : (i == 3001 ? -inf : 1e30f));
            inR[i + 7] = i == 100 ? -inf : (i == 2000 ? nan : (i == 3001 ? 1e30f : inf));
        }
        ef::Transport t;
        const double bpms[] = {20.0, 999.0, 120.0, static_cast<double>(nan), 0.0, -5.0, 1e9};
        const double beatss[] = {3.7, -100.0, 1e9, 1e300, static_cast<double>(nan), static_cast<double>(inf), 0.0};
        t.bpm = bpms[r.next() % 7];
        t.beats = beatss[r.next() % 7];
        t.playing = r.coin();
        t.valid = r.next() % 4 != 0;
        Buf L = inL, R = inR;
        render(p, L, R, t);
        finite = finite && allFinite(L) && allFinite(R);
        const bool full = p.band != Pump::kLows && p.band != Pump::kHighs;
        float peakIn = 0.0f;
        for (const Buf* x : {&inL, &inR})
            for (float v : *x)
                if (std::isfinite(v)) peakIn = std::max(peakIn, std::fabs(v));
        if (full) {   // sample by sample, never louder than the input
            for (size_t i = 0; i < L.size(); ++i) {
                if (std::isfinite(inL[i])) bounded = bounded && std::fabs(L[i]) <= std::fabs(inL[i]);
                if (std::isfinite(inR[i])) bounded = bounded && std::fabs(R[i]) <= std::fabs(inR[i]);
            }
        } else {   // an allpass moves the waveform: 2x the input's peak
            ++banded;
            bounded = bounded && peak(L) <= 2.0f * peakIn && peak(R) <= 2.0f * peakIn;
        }
    }
    CHECK(finite);
    CHECK(bounded);
    CHECK(banded > 50);
}

void pumpBlockSizes() {   // chunks of 1, 7, 32: the same output as 32 (within 1e-6)
    Buf kicks(60000, 0.0f);
    for (int s : {9000, 21000, 34000, 47000}) addKick(kicks, s, 0.8f);
    struct Case {
        P p;
        ef::Transport t;
        bool useKicks;
    };
    std::vector<Case> cases;
    {
        P p;
        p.curve = gf::PC_SMOOTH;
        p.rateBeats = 0.25;
        p.depth = 0.8f;
        p.pushMs = 7.0f;
        p.swing = {0.6f, 0.25};
        p.mix = 0.9f;
        cases.push_back({p, playingAt(123.0, 3.1), false});
        p.band = Pump::kLows;
        p.crossoverHz = 140.0f;
        p.depth = 1.0f;
        cases.push_back({p, playingAt(123.0, 3.1), false});
        p.band = Pump::kHighs;
        cases.push_back({p, stopped(133.0), false});
        p.trigger = Pump::kInput;
        p.pushMs = 5.0f;
        cases.push_back({p, stopped(120.0), true});
        p.band = Pump::kFull;
        cases.push_back({p, playingAt(120.0, 0.0), true});
    }
    for (const Case& c : cases) {
        const Buf inL = c.useKicks ? kicks : whiteNoise(60000, 0.5f, 11), inR = c.useKicks ? kicks : whiteNoise(60000, 0.5f, 12);
        Buf L32 = inL, R32 = inR, L7 = inL, R7 = inR, L1 = inL, R1 = inR;
        render(c.p, L32, R32, c.t, 32);
        render(c.p, L7, R7, c.t, 7);
        render(c.p, L1, R1, c.t, 1);
        const double d = std::max(std::max(maxDiff(L32, L7), maxDiff(R32, R7)), std::max(maxDiff(L32, L1), maxDiff(R32, R1)));
        CHECK(d <= 1e-6);
        if (!(d <= 1e-6)) std::printf("  band %d trigger %d: blocks differ by %g\n", c.p.band, c.p.trigger, d);
        CHECK(!same(L32, inL));   // and it did something
    }
}

// Abrupt changes of every parameter at chunk edges, on DC 0.5 at 150 BPM: after each change, the
// first 0.1 s moves no faster than the settings before or after it do when settled (the edges at the
// most), or under 0.01 a sample; and in the full band no step is over the 1 ms edge at all.
void pumpNoClicks() {
    const size_t seg = 413 * ef::kChunk;   // 0.3 s
    const int changes = 22;
    Buf L(static_cast<size_t>(changes) * seg, 0.5f), R = L;
    Pump m;
    P p;
    p.rateBeats = 0.25;
    p.curve = gf::PC_HARD;
    p.depth = 0.5f;
    p.length = 0.5f;
    bool clean = true, edge = true;
    double settledBefore = 0.0;
    ef::Transport t = playingAt(150.0, 0.0);
    for (int s = 0; s < changes; ++s) {
        switch (s) {   // the change at the start of segment s
        case 1: p.depth = 1.0f; break;
        case 2: p.curve = gf::PC_SNAP; break;
        case 3: p.length = 0.1f; break;
        case 4: p.length = 1.0f; break;
        case 5: p.pushMs = 20.0f; break;
        case 6: p.pushMs = -10.0f; break;
        case 7: p.rateBeats = 1.0; break;
        case 8: p.rateBeats = 0.25; break;
        case 9: p.swing = {0.66f, 0.25}; break;
        case 10: p.mix = 0.5f; break;
        case 11: p.mix = 1.0f; break;
        case 12: p.trigger = Pump::kInput; break;   // DC has no hits: the ducks stop
        case 13: p.trigger = Pump::kGrid; break;
        case 14: p.band = Pump::kLows; break;
        case 15: p.band = Pump::kHighs; break;
        case 16: p.crossoverHz = 300.0f; break;
        case 17: p.crossoverHz = 60.0f; break;
        case 18: p.band = Pump::kFull; break;
        case 19: p.depth = 0.0f; break;
        case 20: p.depth = 1.0f; break;
        case 21: p.curve = gf::PC_LONG; break;
        default: break;
        }
        const size_t from = static_cast<size_t>(s) * seg;
        for (size_t i = from; i < from + seg; i += ef::kChunk) {
            m.set(p, t);
            m.process(&L[i], &R[i], ef::kChunk);
            t.beats += ef::kChunk * 150.0 / 60.0 / ef::kRate;
        }
        const double settled = steepest(L, from + seg - 6615, from + seg);
        if (s > 0) {
            const double first = steepest(L, from - 1, from + 4410);
            const bool ok = first < std::max(0.01, 1.25 * std::max(settled, settledBefore));
            clean = clean && ok;
            if (!ok) std::printf("  change %d: %.5f a sample after it, settled %.5f before, %.5f after\n", s, first, settledBefore, settled);
            if (s < 14 || s > 17) {   // the full band: the 1 ms edge, whatever changes (half of it: DC 0.5)
                const double worst = steepest(L, from - 1, from + seg);
                edge = edge && worst <= 0.5 * kMaxStep + kTol;
                if (!(worst <= 0.5 * kMaxStep + kTol)) std::printf("  change %d: a step of %.5f\n", s, worst);
            }
        }
        settledBefore = settled;
    }
    CHECK(clean);
    CHECK(edge);
}

// The crossover moves with a knob: 60 -> 300 -> 60 Hz in one chunk, on a sine at 120 Hz, in the
// middle of a long bottom with the lows ducked by half. The Pump smooths it (the crossover alone,
// leaping, would swell a tone near it): no click by the rule.
void pumpCrossoverLeap() {
    P p;
    p.rateBeats = 4.0;
    p.curve = gf::PC_HOLD;
    p.depth = 0.5f;
    p.length = 1.0f;
    p.band = Pump::kLows;
    const size_t n = 32 * 2000;   // 1.45 s of the 1.2 s bottom... plus
    const size_t up = 32 * 400, down = 32 * 800;
    const double sineStep = 0.8 * 2.0 * kPi * 120.0 / ef::kRate;
    Buf a = sine(120.0, static_cast<int>(n), 0.8f), b = a;
    Pump m;
    for (size_t pos = 0; pos < n; pos += ef::kChunk) {
        P q = p;
        q.crossoverHz = pos >= up && pos < down ? 300.0f : 60.0f;
        m.set(q, playingAt(120.0, static_cast<double>(pos) / kBeat));
        m.process(&a[pos], &b[pos], ef::kChunk);
    }
    CHECK(allFinite(a));
    const double settled = steepest(a, 32 * 150, 32 * 380);
    CHECK(settled > 0.3 * sineStep);   // there is a signal to click
    bool clean = true;
    for (const size_t at : {up, down}) {
        const double now = steepest(a, at - 1, at + 4410);
        const bool ok = now <= std::max(0.01, 1.25 * std::max(settled, steepest(a, 32 * 1000, 32 * 1200)));
        clean = clean && ok;
        if (!ok) std::printf("  crossover leap at %zu: %.5f a sample, settled %.5f\n", at, now, settled);
    }
    CHECK(clean);
}

void pumpReset() {   // reset() forgets everything: afterwards it plays exactly as a new one; the tail is 0 in Full
    for (const int band : {static_cast<int>(Pump::kFull), static_cast<int>(Pump::kLows)})
        for (const int trigger : {static_cast<int>(Pump::kGrid), static_cast<int>(Pump::kInput)}) {
            P p = hardPump();
            p.band = band;
            p.trigger = trigger;
            Buf kicks(30000, 0.0f);
            for (int s : {9000, 17000}) addKick(kicks, s, 0.8f);
            Pump used;
            Buf L = whiteNoise(30000, 0.8f, 13), R = whiteNoise(30000, 0.8f, 14);
            run(used, p, L, R, playingAt(97.0, 5.5));
            P q = p;
            q.depth = 0.3f;
            q.curve = gf::PC_LATE;
            used.set(q, stopped(120.0));
            used.process(L.data(), R.data(), 17);
            used.reset();
            Buf a = kicks, b = kicks, c = kicks, d = kicks;
            run(used, p, a, b, playingAt(120.0, 0.0));
            Pump fresh;
            run(fresh, p, c, d, playingAt(120.0, 0.0));
            CHECK(same(a, c) && same(b, d));
            CHECK(fresh.tailSamples() == (band == Pump::kFull ? 0 : 1000));
        }
}

void pumpNanInput() {   // a NaN or an infinity in the input comes out as silence there and changes nothing else
    for (const int band : {static_cast<int>(Pump::kFull), static_cast<int>(Pump::kLows)})
        for (const int trigger : {static_cast<int>(Pump::kGrid), static_cast<int>(Pump::kInput)}) {
            P p = hardPump();
            p.band = band;
            p.trigger = trigger;
            Buf L = whiteNoise(20000, 0.5f, 9), R = whiteNoise(20000, 0.5f, 10);
            Buf zl = L, zr = R;
            L[1000] = std::nanf("");
            R[2000] = INFINITY;
            L[3001] = R[3001] = -INFINITY;
            zl[1000] = zr[2000] = zl[3001] = zr[3001] = 0.0f;
            render(p, L, R, playingAt(120.0, 0.0));
            render(p, zl, zr, playingAt(120.0, 0.0));
            CHECK(allFinite(L) && allFinite(R) && same(L, zl) && same(R, zr));
        }
}

} // namespace

void pumpTests() {
    pumpRests();
    pumpGrid();
    pumpPush();
    pumpCurveShape();
    pumpSwing();
    pumpEdges();
    pumpInput();
    pumpHats();
    pumpBand();
    pumpMix();
    pumpStopped();
    pumpFollowsJumps();
    pumpTempoChange();
    pumpShortestSteps();
    pumpExtremes();
    pumpBlockSizes();
    pumpNoClicks();
    pumpBandSwitch();
    pumpInputSwitch();
    pumpCrossoverLeap();
    pumpReset();
    pumpNanInput();
}
