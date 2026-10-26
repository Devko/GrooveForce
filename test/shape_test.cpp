// Shape (dsp/shape.h) against its laws: at rest it changes nothing (bit for bit); the Volume lane
// opens the steps of a gate and follows a sine; the Filter lane puts the cutoff where the law says
// (-3 dB of a Butterworth at the cutoff, Q 12 at resonance 1); the Pan lane keeps L^2 + R^2 at the
// input's power; the shapes sit on the beat grid, through swing, phase, stopped transport, locates,
// short loops and tempo changes; no gain edge is steeper than 1 ms, and a gate on the cutoff takes
// 1 ms too. Then the contract: lanes switched on and off and parameters thrown about without a
// click, the extremes and NaN input, reset, the tail.
#include "signal.h"
#include "../dsp/shape.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace eft;
using ef::kChunk;
using ef::kRate;
using ef::Transport;
using gf::Shape;
using P = Shape::Params;

constexpr double kSlope = 1.0 / (0.001 * 44100.0);   // the 1 ms limit: a full-scale change per sample
constexpr double k16th = 44100.0 * 60.0 / 120.0 / 4.0;   // a 16th at 120 BPM, in samples (5512.5)

int idx(const char* name) {
    for (int i = 0; i < gf::numShapes(); ++i)
        if (std::string(gf::shapeName(i)) == name) return i;
    std::printf("  no shape named %s\n", name);
    CHECK(false);
    return 0;
}

bool same(const Buf& a, const Buf& b) { return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0; }
double maxDiff(const Buf& a, const Buf& b, size_t from = 0, size_t to = 0) {
    if (to == 0) to = a.size();
    double m = 0.0;
    for (size_t i = from; i < to; ++i) m = std::max(m, static_cast<double>(std::fabs(a[i] - b[i])));
    return m;
}
// The steepest change between neighbouring samples (EffectForce's helper; here it stays in this file).
double steepest(const Buf& x, size_t from = 0, size_t to = 0) { return maxStep(x, from, to); }

Transport transport(double bpm, double beats, bool playing) {
    Transport t;
    t.bpm = bpm;
    t.beats = beats;
    t.playing = playing;
    t.valid = true;
    return t;
}

// Parameters with one lane on: the shape by name, the rate in beats, the depth.
void lane(P& p, int l, const char* shape, double rate, float depth, float phase = 0.0f) {
    Shape::LaneParams& q = p.lane[l];
    q.on = true;
    q.shape = idx(shape);
    q.rateBeats = rate;
    q.depth = depth;
    q.phase = phase;
}
P with(int l, const char* shape, double rate, float depth, float phase = 0.0f) {
    P p;
    lane(p, l, shape, rate, depth, phase);
    return p;
}

void render(const P& p, Buf& L, Buf& R, Transport t = {}, int chunk = kChunk) {
    Shape m;
    run(m, p, L, R, t, chunk);
}

// One chunk of a played-back timeline: what MPC says, the song position the output should follow
// (it differs while stopped), and whether the chunk is still settling after a jump.
struct Step {
    Transport t;
    double expect = 0.0;
    bool settling = false;
};

// Runs L / R through a Shape chunk by chunk along `steps` (whole chunks).
void play(const P& p, const std::vector<Step>& steps, Buf& L, Buf& R) {
    Shape m;
    for (size_t c = 0; c < steps.size(); ++c) {
        m.set(p, steps[c].t);
        m.process(&L[c * kChunk], &R[c * kChunk], kChunk);
    }
}

// A timeline of `chunks` chunks: at(c, step) fills in the transport of chunk c and what it expects.
template <class F>
std::vector<Step> timeline(size_t chunks, F at) {
    std::vector<Step> steps(chunks);
    for (size_t c = 0; c < chunks; ++c) at(c, steps[c]);
    return steps;
}

// The output at each chunk's first sample against the shape's value at the song position there,
// wherever that value is the same 3 ms either side (not near an edge) and the chunk is not
// settling after a jump. Returns how many chunks were checked and how many were wrong.
struct Followed {
    int checked = 0, wrong = 0;
    double worst = 0.0;
};
Followed follows(const Buf& out, const std::vector<Step>& steps, int shape, double rate) {
    Followed f;
    for (size_t c = 0; c < steps.size(); ++c) {
        if (steps[c].settling) continue;
        const double span = steps[c].t.bpm / 60.0 * 0.003;   // 3 ms in beats
        const float v = gf::shapeAt(shape, steps[c].expect / rate);
        bool steady = true;
        for (int k = -6; k <= 6; ++k) steady = steady && gf::shapeAt(shape, (steps[c].expect + span * k / 6.0) / rate) == v;
        if (!steady) continue;
        ++f.checked;
        const double err = std::fabs(out[c * kChunk] - v);
        f.worst = std::max(f.worst, err);
        if (err > 1e-4) ++f.wrong;
    }
    return f;
}

// --- tests -------------------------------------------------------------------------------------

void shapeRates() {   // the rates are the LFO divisions' first twelve: the surface shows the same names
    CHECK(gf::kNumShapeRates == 12);
    for (int i = 0; i < gf::kNumShapeRates; ++i) {
        CHECK(std::string(gf::kShapeRates[i].name) == ef::kLfoDivs[i].name);
        CHECK(gf::kShapeRates[i].beats == ef::kLfoDivs[i].beats);
    }
    CHECK(gf::kShapeRates[0].beats == 0.25 && gf::kShapeRates[gf::kNumShapeRates - 1].beats == 16.0);
}

// Default Params, lanes off, and lanes on with nothing to do: the input, bit for bit.
void shapeRests() {
    const Buf inL = whiteNoise(20000, 0.5f, 1), inR = whiteNoise(20000, 0.5f, 2);
    const Transport ts[3] = {transport(120.0, 0.0, false), transport(120.0, 5.3, true), transport(97.0, 11.7, true)};
    auto passes = [&](const P& p) {
        bool ok = true;
        for (const Transport& t : ts)
            for (int chunk : {kChunk, 7}) {
                Buf L = inL, R = inR;
                render(p, L, R, t, chunk);
                ok = ok && same(L, inL) && same(R, inR);
            }
        return ok;
    };
    CHECK(passes(P{}));

    P off;   // every lane off, whatever the rest says
    for (int l = 0; l < Shape::kLanes; ++l) {
        off.lane[l].shape = 20 + l;
        off.lane[l].rateBeats = 0.25;
        off.lane[l].depth = l == Shape::kVolume ? 1.0f : -1.0f;
        off.lane[l].phase = 0.3f;
    }
    off.filterType = Shape::kHP;
    off.cutoffHz = 300.0f;
    off.resonance = 1.0f;
    off.swing = gf::Swing{0.66f, 0.25};
    CHECK(passes(off));

    for (int l : {static_cast<int>(Shape::kVolume), static_cast<int>(Shape::kPan)}) {
        CHECK(passes(with(l, "Sine", 1.0, 0.0f)));                  // on with depth 0
        CHECK(passes(with(l, "Trance 1", 4.0, 0.0f)));
        P zero = with(l, "Square", 0.25, 1.0f);                     // depth 1 times amount 0
        zero.amount = 0.0f;
        CHECK(passes(zero));
        P dry = with(l, "Square", 0.25, 1.0f);                      // depth 1 at mix 0
        dry.mix = 0.0f;
        CHECK(passes(dry));
    }
    P dry = with(Shape::kFilter, "Sine", 1.0, 1.0f);                // the filter at mix 0
    dry.cutoffHz = 200.0f;
    dry.mix = 0.0f;
    CHECK(passes(dry));
    P all = with(Shape::kVolume, "Sine", 1.0, 0.0f);                // all three together, each with nothing to do
    lane(all, Shape::kPan, "Triangle", 2.0, 0.0f);
    lane(all, Shape::kFilter, "Saw Up", 1.0, 1.0f);
    all.mix = 0.0f;
    CHECK(passes(all));

    Shape m;
    CHECK(m.tailSamples() == 0);
}

// Volume, Offbeat, 1 bar, depth 1 on DC at 120 BPM: open on the 16ths 2, 6, 10, 14 and shut on the
// rest, by the song position, whether played from the top, from the middle of a bar, stopped, or in
// chunks that aren't the rack's.
void shapeVolumeSteps() {
    const P p = with(Shape::kVolume, "Offbeat", 4.0, 1.0f);
    const int n = static_cast<int>(32 * k16th);   // two bars
    struct Case { Transport t; int firstStep, chunk; };
    for (const Case c : {Case{transport(120.0, 0.0, true), 0, kChunk}, Case{transport(120.0, 0.0, true), 0, 7},
                         Case{transport(120.0, 1.25, true), 5, kChunk}, Case{transport(120.0, 99.0, false), 0, kChunk}}) {
        Buf L(n, 0.5f), R(n, 0.5f);
        render(p, L, R, c.t, c.chunk);
        bool ok = same(L, R);
        for (int k = 0; k + c.firstStep < 32; ++k) {
            const int step = (c.firstStep + k) % 16;
            const size_t from = static_cast<size_t>(k * k16th) + 100, to = static_cast<size_t>((k + 1) * k16th) - 100;
            const float want = step % 4 == 2 ? 0.5f : 0.0f;
            for (size_t i = from; i < to && i < L.size(); ++i)
                if (std::fabs(L[i] - want) > 1e-6f) {
                    ok = false;
                    std::printf("  step %d of the bar, sample %zu: %g, want %g\n", step, i, L[i], want);
                    break;
                }
        }
        CHECK(ok);
    }
}

// Volume, Sine, 1/4, depth 0.5: the gain follows the raised cosine between 0.5 and 1.
void shapeVolumeSine() {
    const P p = with(Shape::kVolume, "Sine", 1.0, 0.5f);
    const int n = 2 * 44100;
    Buf L(n, 1.0f), R(n, 1.0f);
    render(p, L, R, transport(120.0, 0.0, true));
    double err = 0.0, lo = 2.0, hi = 0.0;
    for (int i = 0; i < n; ++i) {
        const double beats = i * 2.0 / kRate, v = 0.5 - 0.5 * std::cos(2.0 * kPi * beats);
        err = std::max(err, std::fabs(L[i] - (1.0 - 0.5 * (1.0 - v))));
        lo = std::min(lo, static_cast<double>(L[i]));
        hi = std::max(hi, static_cast<double>(L[i]));
    }
    CHECK(err < 0.01);
    CHECK(err < 1e-4);   // in fact: a straight line between 32 samples of a curve this slow
    CHECK(lo >= 0.5 - 1e-6 && lo < 0.5 + 1e-4 && hi <= 1.0 + 1e-6 && hi > 1.0 - 1e-4);
    CHECK(same(L, R));
}

// The Filter lane: the cutoff is cutoffHz 2^(4 d (v - 1)); at the cutoff a Butterworth is 3 dB down.
void shapeFilter() {
    // dB of the `hz` sine through the Filter lane, in a window around sample `at` of a bar started at beat 0.
    auto gainDb = [](const P& p, double hz, int at, int n = 90000) {
        Buf in = sine(hz, n, 0.25f), L = in, R = in;
        render(p, L, R, transport(120.0, 0.0, true));
        const size_t from = static_cast<size_t>(at - 1024), to = static_cast<size_t>(at + 1024);
        return db(magnitude(L, hz, from, to) / magnitude(in, hz, from, to));
    };
    P lp = with(Shape::kFilter, "Saw Down", 4.0, 1.0f);   // v falls from 1 to 0 across the bar
    lp.cutoffHz = 8000.0f;
    // Half a bar: v 0.5, 8 kHz x 2^-2 = 2 kHz, where the Butterworth is -3 dB.
    const double half = gainDb(lp, 2000.0, 44100);
    CHECK(std::fabs(half + 3.01) < 0.5);
    if (!(std::fabs(half + 3.01) < 0.5)) std::printf("  LP at half a bar: %.2f dB\n", half);
    CHECK(std::fabs(gainDb(lp, 2000.0, 3000)) < 0.3);               // near the top: 7 kHz, 2 kHz passes
    const double low = gainDb(lp, 2000.0, 79380);                   // 0.9 bar: v 0.1, 8k x 2^-3.6 = 660 Hz
    CHECK(low < -16.0 && low > -22.0);
    // The cutoff of a -1 depth rises from cutoffHz: 1 kHz x 2^(4 (1 - 0.5)) = 4 kHz at half a bar.
    P up = with(Shape::kFilter, "Saw Down", 4.0, -1.0f);
    up.cutoffHz = 1000.0f;
    CHECK(std::fabs(gainDb(up, 4000.0, 44100) + 3.01) < 0.5);
    // Amount scales the depth: 8k x 2^(4 x 0.5 x -0.5) = 4 kHz.
    P half2 = lp;
    half2.amount = 0.5f;
    CHECK(std::fabs(gainDb(half2, 4000.0, 44100) + 3.01) < 0.5);
    // Types: HP is -3 dB at its cutoff too; BP is normalised to 0 dB there.
    P hp = lp;
    hp.filterType = Shape::kHP;
    CHECK(std::fabs(gainDb(hp, 2000.0, 44100) + 3.01) < 0.5);
    P bp = lp;
    bp.filterType = Shape::kBP;
    CHECK(std::fabs(gainDb(bp, 2000.0, 44100)) < 0.5);
    // Resonance: depth 0 holds the cutoff at 2 kHz; Q 0.7071 is -3 dB there, Q 12 is +21.6 dB.
    P res = with(Shape::kFilter, "Sine", 1.0, 0.0f);
    res.cutoffHz = 2000.0f;
    res.resonance = 0.0f;
    CHECK(std::fabs(gainDb(res, 2000.0, 20000, 40000) + 3.01) < 0.3);
    res.resonance = 1.0f;
    const double peak12 = gainDb(res, 2000.0, 20000, 40000);
    CHECK(std::fabs(peak12 - 20.0 * std::log10(12.0)) < 0.5);
    if (!(std::fabs(peak12 - 21.58) < 0.5)) std::printf("  resonance 1: %.2f dB at the cutoff\n", peak12);
    // Mix blends the filtered signal with the dry: half of each, 90 degrees apart at the cutoff.
    P mixed = with(Shape::kFilter, "Sine", 1.0, 0.0f);
    mixed.cutoffHz = 2000.0f;
    mixed.mix = 0.5f;
    CHECK(std::fabs(gainDb(mixed, 2000.0, 20000, 40000) - db(std::sqrt(0.25 + 0.125))) < 0.3);
}

// A gate on the Filter lane cannot move the cutoff four octaves inside a chunk: its value goes through
// the same 1 ms limit as the gains. An HP filter at 500 Hz whose cutoff rises to 8 kHz when the Square
// falls, on a 2 kHz sine; the edges fall on chunk boundaries (86 chunks to a half cycle). Right at the
// edge the sine has lost 11 dB in the first 11 samples (a cutoff that went the whole way within the
// chunk would have taken 24), and a few ms later the filter has closed.
void shapeFilterEdge() {
    P p = with(Shape::kFilter, "Square", 0.25, -1.0f);
    p.filterType = Shape::kHP;
    p.cutoffHz = 500.0f;
    const int half = 86 * kChunk;
    const double bpm = 0.125 * 60.0 * 44100.0 / half;   // a half cycle (0.125 beats) is `half` samples
    Buf L = sine(2000.0, 12 * half, 0.5f), R = L;
    render(p, L, R, transport(bpm, 0.0, true));
    bool slowEnough = true, closes = true;
    for (int k = 2; k < 5; ++k) {
        const size_t edge = static_cast<size_t>(half) * (2 * k + 1);   // the Square falls: v 1 to 0
        const double open = rms(L, edge - 200, edge - 1);
        slowEnough = slowEnough && db(rms(L, edge, edge + 11) / open) > -17.5;
        closes = closes && db(rms(L, edge + 200, edge + 400) / open) < -20.0;
    }
    CHECK(slowEnough);
    CHECK(closes);
}

// The Pan lane: constant power. A mono input keeps L^2 + R^2 = 2 x^2, a shape at 0 is full left,
// at 1 full right (a negative depth the other way), depth 0.5 half way.
void shapePan() {
    const int n = 3 * 44100;
    const Buf in = sine(440.0, n, 0.5f);
    for (const char* shape : {"Sine", "Square", "Random Steps"}) {
        P p = with(Shape::kPan, shape, 4.0, 1.0f);
        Buf L = in, R = in;
        render(p, L, R, transport(120.0, 0.0, true));
        double power = 0.0;
        for (int i = 0; i < n; ++i) power = std::max(power, std::fabs(static_cast<double>(L[i]) * L[i] + static_cast<double>(R[i]) * R[i] - 2.0 * in[i] * in[i]));
        CHECK(power < 1e-5);   // (the brief asks for 1e-4)
        if (!(power < 1e-5)) std::printf("  pan %s: L^2 + R^2 off by %g\n", shape, power);
    }
    // v 0 at the start of the Sine: full left, with the sine's own sqrt2 gain.
    const float root2 = std::sqrt(2.0f);
    P p = with(Shape::kPan, "Sine", 4.0, 1.0f);
    Buf L = in, R = in;
    render(p, L, R, transport(120.0, 0.0, true));
    bool left = true, right = true;
    for (int i = 20; i < 40; ++i) left = left && std::fabs(L[i] - root2 * in[i]) < 1e-3f && std::fabs(R[i]) < 1e-3f;
    for (int i = 44100 - 20; i < 44100 + 20; ++i) right = right && std::fabs(R[i] - root2 * in[i]) < 1e-3f && std::fabs(L[i]) < 1e-3f;   // v 1 at half a bar
    CHECK(left && right);
    p.lane[Shape::kPan].depth = -1.0f;
    L = in;
    R = in;
    render(p, L, R, transport(120.0, 0.0, true));
    bool flipped = true;
    for (int i = 20; i < 40; ++i) flipped = flipped && std::fabs(R[i] - root2 * in[i]) < 1e-3f && std::fabs(L[i]) < 1e-3f;
    CHECK(flipped);
    // Half the depth: p = -0.5, a = -pi/8: L = cos a - sin a, R = cos a + sin a.
    p.lane[Shape::kPan].depth = 0.5f;
    L = in;
    R = in;
    render(p, L, R, transport(120.0, 0.0, true));
    const double a = -kPi / 8.0;
    bool halved = true;
    for (int i = 20; i < 40; ++i)
        halved = halved && std::fabs(L[i] - (std::cos(a) - std::sin(a)) * in[i]) < 1e-4 && std::fabs(R[i] - (std::cos(a) + std::sin(a)) * in[i]) < 1e-4;
    CHECK(halved);
    // The mix blends the gains with unity: gain' = 1 + mix (gain - 1).
    p.lane[Shape::kPan].depth = 1.0f;
    p.mix = 0.5f;
    L = in;
    R = in;
    render(p, L, R, transport(120.0, 0.0, true));
    bool mixed = true;
    for (int i = 20; i < 40; ++i)
        mixed = mixed && std::fabs(L[i] - (1.0 + 0.5 * (root2 - 1.0)) * in[i]) < 1e-4 && std::fabs(R[i] - 0.5 * in[i]) < 1e-4;
    CHECK(mixed);
}

// The first sample below `level` (falling) or above it (rising) from `from`, in ms.
double crossing(const Buf& x, size_t from, float level, bool falling) {
    for (size_t i = from; i < x.size(); ++i)
        if (falling ? x[i] < level : x[i] > level) return i / 44.1;
    return -1.0;
}

// Rolling 16ths (1, 0.55, 0.75, 0.55) at one bar on DC: the change from the 16th 0 to the 16th 1 comes
// (swing - 50%) x 2 units late, 40 ms at 66% on 1/16 at 120 BPM (125 ms straight).
void shapeSwing() {
    auto render16 = [](float amount, double unit) {
        P p = with(Shape::kVolume, "Rolling 16ths", 4.0, 1.0f);
        p.swing = gf::Swing{amount, unit};
        Buf L(44100, 1.0f), R(44100, 1.0f);
        render(p, L, R, transport(120.0, 0.0, true));
        return L;
    };
    const Buf straight = render16(0.5f, 0.25);
    const double at0 = crossing(straight, 0, 0.775f, true);
    CHECK(std::fabs(at0 - 125.0) < 1.0);
    const double at66 = crossing(render16(0.66f, 0.25), 0, 0.775f, true);
    CHECK(std::fabs(at66 - 165.0) < 1.0);
    if (!(std::fabs(at66 - 165.0) < 1.0)) std::printf("  66%%: the change lands at %.1f ms\n", at66);
    CHECK(std::fabs(crossing(render16(0.58f, 0.25), 0, 0.775f, true) - 145.0) < 1.0);   // 58%: 20 ms late
    CHECK(same(straight, render16(0.3f, 0.25)));   // at most 50% is straight, bit for bit
    // On 1/8: the 16th 1 (straight at 0.25 beats) lands at 0.25 x 0.66 / 0.5 = 0.33 beats, the 16th 2 at 0.66.
    const Buf eighths = render16(0.66f, 0.5);
    CHECK(std::fabs(crossing(eighths, 0, 0.775f, true) - 165.0) < 1.0);
    CHECK(std::fabs(crossing(eighths, 8000, 0.65f, false) - 330.0) < 1.0);
    // The late 16th is the straight one pushed on: the swung output is the straight one's levels at other times.
    CHECK(std::fabs(render16(0.66f, 0.25)[static_cast<size_t>(0.150 * 44100)] - 1.0f) < 1e-6f);   // still the 16th 0 at 150 ms
}

// Phase 0.25 on a 1/4 Sine moves it 125 ms earlier at 120 BPM (a quarter of its 500 ms cycle).
void shapePhase() {
    const int n = 3 * 44100;
    Buf a(n, 1.0f), ar(n, 1.0f), b(n, 1.0f), br(n, 1.0f);
    render(with(Shape::kVolume, "Sine", 1.0, 1.0f), a, ar, transport(120.0, 0.0, true));
    render(with(Shape::kVolume, "Sine", 1.0, 1.0f, 0.25f), b, br, transport(120.0, 0.0, true));
    double err = 0.0, ref = 0.0;
    for (int i = 0; i < n - 6000; ++i) {
        err = std::max(err, static_cast<double>(std::fabs(b[i] - a[i + 5512])));   // 125 ms = 5512.5 samples
        const double v = 0.5 - 0.5 * std::cos(2.0 * kPi * (i * 2.0 / kRate + 0.25));
        ref = std::max(ref, std::fabs(b[i] - v));
    }
    CHECK(err < 3e-4);
    CHECK(ref < 1e-4);
    CHECK(std::fabs(a[0]) < 1e-6f && std::fabs(b[0] - 0.5f) < 1e-6f);   // 0 at the start; a quarter on, half way up
}

// Volume Hold steps (a gate, a pump's fall, a square) never change the gain faster than 1 ms for a
// full scale: the shapes' edges go through the slope limit.
void shapeEdges() {
    double hardest = 0.0;
    for (int s = 0; s < gf::numShapes(); ++s)
        for (double rate : {0.25, 4.0}) {
            P p;
            Shape::LaneParams& q = p.lane[Shape::kVolume];
            q.on = true;
            q.shape = s;
            q.rateBeats = rate;
            q.depth = 1.0f;
            Buf L(3 * 44100, 1.0f), R(3 * 44100, 1.0f);
            render(p, L, R, transport(120.0, 0.0, true));
            const double step = steepest(L);
            hardest = std::max(hardest, step);
            CHECK(step <= kSlope * 1.001 + 1e-6);
            if (!(step <= kSlope * 1.001 + 1e-6)) std::printf("  %s at %g beats: %.5f a sample, the limit is %.5f\n", gf::shapeName(s), rate, step, kSlope);
        }
    CHECK(hardest >= kSlope * 0.99);   // and the edges do use it: a gate takes 1 ms
    // Depth below 1 scales the gain's steps down with it.
    P half = with(Shape::kVolume, "Square", 0.25, 0.5f);
    Buf L(44100, 1.0f), R(44100, 1.0f);
    render(half, L, R, transport(120.0, 0.0, true));
    CHECK(steepest(L) <= 0.5 * kSlope * 1.001 + 1e-6 && steepest(L) >= 0.5 * kSlope * 0.99);

    // The Pan lane's gains (L and R on DC 1) obey the same rule: the pan law steepens the value by up to
    // sqrt2 pi/2 = 2.22, so the lane limits its value that much harder (a full sweep takes 2.2 ms).
    double hardestPan = 0.0;
    for (int s = 0; s < gf::numShapes(); ++s)
        for (double rate : {0.25, 4.0})
            for (float depth : {1.0f, -1.0f}) {
                P p;
                Shape::LaneParams& q = p.lane[Shape::kPan];
                q.on = true;
                q.shape = s;
                q.rateBeats = rate;
                q.depth = depth;
                Buf pl(3 * 44100, 1.0f), pr(3 * 44100, 1.0f);
                render(p, pl, pr, transport(120.0, 0.0, true));
                const double step = std::max(steepest(pl), steepest(pr));
                hardestPan = std::max(hardestPan, step);
                CHECK(step <= kSlope * 1.001 + 1e-6);
                if (!(step <= kSlope * 1.001 + 1e-6)) std::printf("  pan %s at %g beats: %.5f a sample, the limit is %.5f\n", gf::shapeName(s), rate, step, kSlope);
            }
    CHECK(hardestPan >= kSlope * 0.9);   // and a hard pan does use it
}

// Stopped, the cycle runs on at the tempo from where it was; starting to play doesn't click.
void shapeStopped() {
    const P p = with(Shape::kVolume, "Offbeat", 4.0, 1.0f);
    auto stepsOk = [](const Buf& out, double stepSamples, int firstStep) {
        bool ok = true;
        for (int k = 0; (k + 1) * stepSamples < out.size(); ++k) {
            const float want = (firstStep + k) % 4 == 2 ? 1.0f : 0.0f;
            for (size_t i = static_cast<size_t>(k * stepSamples) + 100; i < static_cast<size_t>((k + 1) * stepSamples) - 100; ++i)
                if (std::fabs(out[i] - want) > 1e-6f) ok = false;
        }
        return ok;
    };
    // MPC reports beat 80 (the start of a bar) while stopped: it starts there, and runs at the tempo.
    Buf L(3 * 44100, 1.0f), R = L;
    render(p, L, R, transport(120.0, 80.0, false));
    CHECK(stepsOk(L, k16th, 0));
    Buf L2(3 * 44100, 1.0f), R2 = L2;
    render(p, L2, R2, transport(90.0, 80.0, false));   // slower: a 16th is 7350 samples
    CHECK(stepsOk(L2, 7350.0, 0));
    CHECK(!same(L, L2));
    // Playing but without a valid position runs on too (from 0, the same place in the bar).
    Transport lost = transport(120.0, 77.7, true);
    lost.valid = false;
    Buf L3(3 * 44100, 1.0f), R3 = L3;
    render(p, L3, R3, lost);
    CHECK(same(L, L3));

    // Playing for 1.2 s, then stopped (MPC freezes the song position): the cycle goes on where it was.
    // Then playing from elsewhere: a jump, but the gain still never steps faster than 1 ms.
    const double perChunk = kChunk * 2.0 / 44100.0;   // beats a chunk at 120 BPM
    const size_t stopAt = static_cast<size_t>(1.2 * 44100 / kChunk), startAt = static_cast<size_t>(3.9 * 44100 / kChunk);
    auto steps = timeline(static_cast<size_t>(6 * 44100 / kChunk), [&](size_t c, Step& st) {
        if (c < stopAt) {
            st.t = transport(120.0, 0.3 + c * perChunk, true);
            st.expect = st.t.beats;
        } else if (c < startAt) {
            st.t = transport(120.0, 0.3 + stopAt * perChunk, false);
            st.expect = 0.3 + c * perChunk;   // the cycle runs on
        } else {
            st.t = transport(120.0, 5.3 + (c - startAt) * perChunk, true);
            st.expect = st.t.beats;
            st.settling = c < startAt + 8;
        }
    });
    Buf L4(steps.size() * kChunk, 1.0f), R4 = L4;
    play(p, steps, L4, R4);
    CHECK(steepest(L4) <= kSlope * 1.001 + 1e-6);
    const Followed f = follows(L4, steps, idx("Offbeat"), 4.0);
    CHECK(f.wrong == 0 && f.checked > 300);
    if (f.wrong || f.checked <= 300) std::printf("  stopped and started: %d of %d chunks off the cycle, worst by %g\n", f.wrong, f.checked, f.worst);
}

// Reset while MPC is stopped, and says where: the lane starts from that song position (as it does when
// playing) and then runs on at the tempo (plan 1's ruling R21; it used to start from beat 0). Only the first
// set() after reset() seeds it, and only when MPC's position is valid.
void shapeSeededStopped() {
    const P p = with(Shape::kVolume, "Offbeat", 4.0, 1.0f);   // 16 steps a bar, every fourth one open from the third
    // The gain a step at a time: `step` samples each, the first being step `first` of the bar.
    auto stepsOk = [](const Buf& out, double step, int first) {
        bool ok = true;
        for (int k = 0; (k + 1) * step < static_cast<double>(out.size()); ++k) {
            const float want = (first + k) % 4 == 2 ? 1.0f : 0.0f;
            for (size_t i = static_cast<size_t>(k * step) + 100; i < static_cast<size_t>((k + 1) * step) - 100; ++i)
                if (std::fabs(out[i] - want) > 1e-6f) ok = false;
        }
        return ok;
    };
    // Stopped at beat 9.5 (step 6 of its bar): the first 16th is open. From 0 it would be shut.
    Buf L(3 * 44100, 1.0f), R = L;
    render(p, L, R, transport(120.0, 9.5, false));
    CHECK(stepsOk(L, k16th, 6));
    // Slower, and from a step that is not the first of its beat: 5.25 beats is step 21 (90 BPM: 7350 samples a 16th).
    Buf L2(3 * 44100, 1.0f), R2 = L2;
    render(p, L2, R2, transport(90.0, 5.25, false));
    CHECK(stepsOk(L2, 7350.0, 21));
    CHECK(!stepsOk(L2, 7350.0, 0));
    // MPC keeps saying 9.5, or says something else later: the position is a seed, not a source. It runs on at the tempo.
    Buf L3(3 * 44100, 1.0f), R3 = L3;
    {
        Shape m;
        for (size_t pos = 0; pos < L3.size(); pos += kChunk) {
            m.set(p, transport(120.0, pos < 44100 ? 9.5 : 77.7, false));
            m.process(&L3[pos], &R3[pos], static_cast<int>(std::min<size_t>(kChunk, L3.size() - pos)));
        }
    }
    CHECK(same(L, L3));
    // MPC doesn't say where (not valid): from the top, as before.
    Transport lost = transport(120.0, 9.5, false);
    lost.valid = false;
    Buf L4(3 * 44100, 1.0f), R4 = L4;
    render(p, L4, R4, lost);
    CHECK(stepsOk(L4, k16th, 0));
    // Not a number is not a position either.
    Transport nan = transport(120.0, std::nan(""), false);
    Buf L5(3 * 44100, 1.0f), R5 = L5;
    render(p, L5, R5, nan);
    CHECK(same(L4, L5));
    // A used Shape, reset, and stopped at 9.5 again: the same as a new one.
    Shape used;
    Buf warm(44100, 1.0f), warmR = warm;
    run(used, p, warm, warmR, transport(120.0, 1.0, true));
    used.reset();
    Buf L6(3 * 44100, 1.0f), R6 = L6;
    run(used, p, L6, R6, transport(120.0, 9.5, false));
    CHECK(same(L, L6));
}

// Locates (forwards and back), a stop and a start elsewhere, and short loops: the gain is on the
// right step within a few ms of each, and never steps faster than the limit.
void shapeFollowsJumps() {
    const P p = with(Shape::kVolume, "Trance 1", 4.0, 1.0f);
    const double perChunk = kChunk * 2.0 / 44100.0;   // beats a chunk at 120 BPM
    struct Seg { size_t chunks; double start; bool playing; double loop; };
    const Seg segs[] = {
        {300, 0.3, true, 0.0},      // playing on
        {300, 7.1, true, 0.0},      // a locate forwards
        {300, 2.6, true, 0.0},      // and back
        {200, 99.0, false, 0.0},    // stopped (the position MPC reports then doesn't matter)
        {300, 10.4, true, 0.0},     // played from elsewhere
        {1000, 3.0, true, 0.5},     // a loop of 1/8 note: [3, 3.5), step 12 (shut) then 13 (open)
        {1000, 3.125, true, 0.25},  // a loop of a 16th across the edge between step 12 and step 13
    };
    std::vector<Step> steps;
    size_t settleUntil = 0;
    for (const Seg& sg : segs)
        for (size_t j = 0; j < sg.chunks; ++j) {
            Step st;
            const double pos = sg.loop > 0.0 ? sg.start + std::fmod(j * perChunk, sg.loop) : sg.start + j * perChunk;
            const bool wrapped = sg.loop > 0.0 && j > 0 && pos < steps.back().t.beats;
            st.t = transport(120.0, pos, sg.playing);
            st.expect = pos;
            // A chunk is settling for 8 chunks (5.8 ms) after the transport jumps; a stopped one is not on the song position.
            if (j == 0 || wrapped) settleUntil = steps.size() + 8;
            st.settling = !sg.playing || steps.size() < settleUntil;
            steps.push_back(st);
        }
    Buf L(steps.size() * kChunk, 1.0f), R = L;
    play(p, steps, L, R);
    CHECK(steepest(L) <= kSlope * 1.001 + 1e-6);
    const Followed f = follows(L, steps, idx("Trance 1"), 4.0);
    CHECK(f.wrong == 0 && f.checked > 1500);
    if (f.wrong || f.checked <= 1500) std::printf("  jumps: %d of %d chunks off the step, worst by %g\n", f.wrong, f.checked, f.worst);
    // The loops really are loops of the shape: in the 16th loop the gain alternates 0 and 1 every 62.5 ms.
    const size_t loopFrom = (300 + 300 + 300 + 200 + 300 + 1000) * kChunk;
    int edges = 0;
    for (size_t i = loopFrom + 400; i + 1 < L.size(); ++i) edges += (L[i] < 0.5f) != (L[i + 1] < 0.5f);
    CHECK(edges >= 2 * static_cast<int>((L.size() - loopFrom - 400) / 5512.5) - 2);
}

// A tempo change while playing: the song position goes on and the gain stays on it.
void shapeTempoChange() {
    const P p = with(Shape::kVolume, "Trance 1", 4.0, 1.0f);
    double beats = 0.0;
    auto steps = timeline(3200, [&](size_t c, Step& st) {
        const double bpm = c < 800 ? 120.0 : (c < 1600 ? 150.0 : (c < 2400 ? 70.0 : 140.0));
        st.t = transport(bpm, beats, true);
        st.expect = beats;
        st.settling = false;
        beats += kChunk * bpm / 60.0 / 44100.0;
    });
    Buf L(steps.size() * kChunk, 1.0f), R = L;
    play(p, steps, L, R);
    CHECK(steepest(L) <= kSlope * 1.001 + 1e-6);
    const Followed f = follows(L, steps, idx("Trance 1"), 4.0);
    CHECK(f.wrong == 0 && f.checked > 1200);
    if (f.wrong) std::printf("  tempo change: %d of %d chunks off the step, worst by %g\n", f.wrong, f.checked, f.worst);
    // A tempo of 999 and of 20: a 16th at 20 BPM is 33075 samples, at 999 BPM 662.
    for (double bpm : {20.0, 999.0, 0.0, -5.0, std::nan("")}) {
        Buf a(44100, 1.0f), b = a;
        render(p, a, b, transport(bpm, 0.0, false));
        CHECK(allFinite(a) && peak(a) <= 1.0f);
    }
}

// Loud noise through every lane at the ends of every field, past them, and NaN in: finite, and no
// louder than 17 times the input (resonance 1 is a Q of 12, and the pan's sqrt2 on top: 16.97, which a
// sine at the cutoff reaches). With no lane on nothing runs and the buffers are left alone, NaN and all.
void shapeExtremes() {
    uint32_t s = 5;
    auto flip = [&s] { return (ef::xorshift(s) & 1u) != 0; };
    double worst = 0.0;
    bool finite = true, untouched = true;
    for (int trial = 0; trial < 300; ++trial) {
        P p;
        for (int l = 0; l < Shape::kLanes; ++l) {
            Shape::LaneParams& q = p.lane[l];
            q.on = trial < 40 || flip();
            q.shape = static_cast<int>(ef::xorshift(s) % static_cast<uint32_t>(gf::numShapes()));
            q.rateBeats = flip() ? gf::kShapeRates[0].beats : gf::kShapeRates[gf::kNumShapeRates - 1].beats;
            q.depth = flip() ? (l == Shape::kVolume ? 0.0f : -1.0f) : 1.0f;
            q.phase = flip() ? 0.0f : 1.0f;
        }
        p.amount = trial < 40 ? 1.0f : (flip() ? 0.0f : 1.0f);
        p.filterType = static_cast<int>(ef::xorshift(s) % 3u);
        p.cutoffHz = flip() ? 20.0f : 20000.0f;
        p.resonance = trial < 40 ? 1.0f : (flip() ? 0.0f : 1.0f);
        p.mix = trial < 40 ? 1.0f : (flip() ? 0.0f : 1.0f);
        p.swing = gf::Swing{flip() ? 0.5f : 0.75f, flip() ? 0.25 : 0.5};
        if (trial % 7 == 0) {   // past the ends, and not numbers
            for (int l = 0; l < Shape::kLanes; ++l) {
                Shape::LaneParams& q = p.lane[l];
                q.shape = flip() ? -9 : 999;
                q.rateBeats = flip() ? -3.0 : (flip() ? 1e9 : std::nan(""));
                q.depth = flip() ? -7.0f : (flip() ? 9.0f : std::nanf(""));
                q.phase = flip() ? -4.0f : (flip() ? 11.0f : std::nanf(""));
            }
            p.amount = flip() ? -2.0f : (flip() ? 5.0f : std::nanf(""));
            p.filterType = flip() ? -3 : 17;
            p.cutoffHz = flip() ? -1.0f : (flip() ? 1e9f : std::nanf(""));
            p.resonance = flip() ? -1.0f : (flip() ? 9.0f : std::nanf(""));
            p.mix = flip() ? -1.0f : (flip() ? 3.0f : std::nanf(""));
            p.swing = gf::Swing{flip() ? std::nanf("") : 9.0f, flip() ? std::nan("") : -1.0};
        }
        const Buf cleanL = whiteNoise(8192, 100.0f, 2 * trial + 1), cleanR = whiteNoise(8192, 100.0f, 2 * trial + 2);
        Buf L = cleanL, R = cleanR;
        L[100] = std::nanf("");
        R[2000] = INFINITY;
        L[3001] = R[3001] = -INFINITY;
        Transport t = transport(flip() ? 20.0 : 999.0, flip() ? 3.7 : 1e5, flip() || trial % 17 == 0 || trial % 19 == 0);
        if (trial % 11 == 0) t.beats = std::nan("");
        if (trial % 17 == 0) t.beats = 1e300;   // too far out to have a fraction
        if (trial % 19 == 0) t.beats = -2.5;     // a count-in
        if (trial % 13 == 0) t.valid = false;
        const Buf sentL = L, sentR = R;
        render(p, L, R, t);
        if (!(p.lane[0].on || p.lane[1].on || p.lane[2].on)) {   // nothing running: the buffers are left alone, NaN and all
            untouched = untouched && same(L, sentL) && same(R, sentR);
            continue;
        }
        finite = finite && allFinite(L) && allFinite(R);
        const double in = std::max(peak(cleanL), peak(cleanR)), out = std::max(peak(L), peak(R));
        worst = std::max(worst, out / in);
    }
    CHECK(finite);
    CHECK(untouched);
    CHECK(worst <= 17.0);
    if (!(worst <= 17.0)) std::printf("  loudest: %.2f times the input\n", worst);

    // The worst case there is: a 2 kHz sine at the cutoff of an LP at resonance 1 (gain Q = 12) with the Pan
    // lane holding a full side (sqrt2). Its peak is 16.97 times the input's: more than the 16 of noise.
    P loud = with(Shape::kFilter, "Sine", 1.0, 0.0f);
    loud.cutoffHz = 2000.0f;
    loud.resonance = 1.0f;
    lane(loud, Shape::kPan, "Square", 16.0, 1.0f);   // v 1 for the first 8 beats: p +1
    const Buf tone = sine(2000.0, 44100, 0.25f);
    Buf a = tone, b = tone;
    render(loud, a, b, transport(120.0, 0.0, true));
    const double ratio = std::max(peak(a), peak(b)) / peak(tone);
    CHECK(ratio <= 17.0 && ratio > 16.0);
    if (!(ratio <= 17.0 && ratio > 16.0)) std::printf("  a sine at the cutoff: %.3f times the input\n", ratio);
}

// New, random parameters (out of range, NaN) and transport every chunk: finite, and bounded.
void shapeRandomJumps() {
    uint32_t s = 77;
    auto r = [&s] { return 0.5f + 0.5f * ef::randBipolar(s); };
    const Buf inL = whiteNoise(5 * 44100, 1.0f, 7), inR = whiteNoise(5 * 44100, 1.0f, 8);
    Buf L = inL, R = inR;
    Shape m;
    for (size_t i = 0; i < L.size(); i += kChunk) {
        P p;
        for (int l = 0; l < Shape::kLanes; ++l) {
            Shape::LaneParams& q = p.lane[l];
            q.on = r() < 0.7f;
            q.shape = static_cast<int>(r() * 44.0f) - 3;
            q.rateBeats = r() * 20.0 - 2.0;
            q.depth = r() * 2.6f - 1.3f;
            q.phase = r() * 1.6f - 0.3f;
            if (r() < 0.02f) q.depth = std::nanf("");
            if (r() < 0.02f) q.rateBeats = std::nan("");
        }
        p.amount = r() * 1.4f - 0.2f;
        p.filterType = static_cast<int>(r() * 5.0f) - 1;
        p.cutoffHz = r() * 25000.0f - 2000.0f;
        p.resonance = r() * 1.4f - 0.2f;
        p.mix = r() * 1.4f - 0.2f;
        p.swing = gf::Swing{r() * 0.5f + 0.4f, r() < 0.5f ? 0.25 : 0.5};
        if (r() < 0.02f) p.cutoffHz = std::nanf("");
        if (r() < 0.02f) p.resonance = std::nanf("");
        if (r() < 0.02f) p.mix = std::nanf("");
        if (r() < 0.02f) p.amount = std::nanf("");
        Transport t;
        t.playing = r() < 0.7f;
        t.valid = r() < 0.9f;
        t.bpm = r() * 1200.0;
        t.beats = r() * 1e4 - 100.0;
        if (r() < 0.02f) t.bpm = std::nan("");
        if (r() < 0.02f) t.beats = INFINITY;
        if (r() < 0.02f) t.beats = std::nan("");
        m.set(p, t);
        m.process(&L[i], &R[i], static_cast<int>(std::min<size_t>(kChunk, L.size() - i)));
    }
    CHECK(allFinite(L) && allFinite(R));
    CHECK(std::max(peak(L), peak(R)) <= 17.0f * std::max(peak(inL), peak(inR)));
}

// Random Steps under a 1-bar MPC loop: every bar plays the bar's pattern, the first one's. (At 122.5
// BPM a bar is 2700 chunks exactly, so each bar's chunks fall on the same samples.)
void shapeRandomRepeats() {
    const int bar = 2700 * kChunk;
    for (const char* name : {"Random Steps", "Random Smooth"}) {
        const P p = with(Shape::kVolume, name, 4.0, 1.0f);
        Buf L(3 * bar, 1.0f), R = L;
        Shape m;
        for (size_t c = 0; c < static_cast<size_t>(3 * 2700); ++c) {
            m.set(p, transport(122.5, 4.0 * static_cast<double>(c % 2700) / 2700.0, true));
            m.process(&L[c * kChunk], &R[c * kChunk], kChunk);
        }
        Buf b1(L.begin(), L.begin() + bar), b2(L.begin() + bar, L.begin() + 2 * bar), b3(L.begin() + 2 * bar, L.end());
        // (a bar's first 3 ms come from the end of the bar before: the gain moves to the pattern's first level)
        CHECK(maxDiff(b1, b2, 200) == 0.0);
        CHECK(same(b2, b3));
        // Not a loop, the bars play their own patterns: bar 2 of the song is not bar 1.
        Buf M(3 * bar, 1.0f), N = M;
        Shape song;
        for (size_t c = 0; c < static_cast<size_t>(3 * 2700); ++c) {
            song.set(p, transport(122.5, 4.0 * static_cast<double>(c) / 2700.0, true));
            song.process(&M[c * kChunk], &N[c * kChunk], kChunk);
        }
        Buf s1(M.begin(), M.begin() + bar), s2(M.begin() + bar, M.begin() + 2 * bar);
        CHECK(maxDiff(s1, s2, 200) > 0.2);
        CHECK(maxDiff(s1, b1) == 0.0);   // the first bar is the same either way
    }
}

// Switching lanes on and off, and turning every control hard, makes no click: the gain stays
// within the limit (plus the 10 ms fade), the sine's steps within its own.
void shapeNoClicks() {
    // Volume and Pan: DC through a Square, the lane switched on and off every 0.2 s.
    for (int l : {static_cast<int>(Shape::kVolume), static_cast<int>(Shape::kPan)}) {
        P p = with(l, "Square", 1.0, 1.0f);
        p.lane[l].on = false;
        Buf L(4 * 44100 / kChunk * kChunk, 1.0f), R = L;   // whole chunks
        Shape m;
        for (size_t i = 0; i < L.size(); i += kChunk) {
            p.lane[l].on = (i / 8820) % 2 == 1;
            m.set(p, transport(120.0, i * 2.0 / kRate, true));
            m.process(&L[i], &R[i], kChunk);
        }
        // The limit, plus the 10 ms fade's slope (at most 0.11 of it, for the Pan's sqrt2 pi/4 too).
        const double bound = 1.12 * kSlope;
        CHECK(steepest(L) <= bound && steepest(R) <= bound);
        if (!(steepest(L) <= bound)) std::printf("  lane %d on and off: %.5f a sample\n", l, steepest(L));
    }
    // The Filter: a 1 kHz sine, switched on and off, and the controls thrown about in between.
    const Buf in = sine(1000.0, 8 * 44100, 0.5f);
    Buf L = in, R = in;
    P p = with(Shape::kFilter, "Square", 1.0, 1.0f);
    p.cutoffHz = 600.0f;   // (resonance 0: no setting makes the sine louder than it was)
    Shape m;
    const double inStep = steepest(in);
    for (size_t i = 0; i < L.size(); i += kChunk) {
        const size_t seg = i / 11025;   // every 0.25 s another change
        p.lane[Shape::kFilter].on = seg % 5 != 2;
        p.filterType = static_cast<int>(seg / 3) % 3;
        p.cutoffHz = seg % 2 ? 20000.0f : 400.0f;
        p.lane[Shape::kFilter].depth = seg % 4 == 1 ? -1.0f : 1.0f;
        p.amount = seg % 6 == 5 ? 0.0f : 1.0f;
        p.mix = seg % 7 == 6 ? 0.3f : 1.0f;
        m.set(p, transport(120.0, i * 2.0 / kRate, true));
        m.process(&L[i], &R[i], kChunk);
    }
    CHECK(allFinite(L));
    // The sine's own steps are 0.071; a type, cutoff or depth change cross-fades or glides, so no step is
    // much more than that (a hard switch of the filter's output would step by up to its whole amplitude).
    const double st = steepest(L);
    CHECK(st < 1.35 * inStep);
    if (!(st < 1.35 * inStep)) std::printf("  filter: %.4f a sample, the sine's own are %.4f\n", st, inStep);
    // Mixed in, a gate on the volume lane and a pan lane together: finite and in range.
    P q = with(Shape::kVolume, "Offbeat", 4.0, 1.0f);
    lane(q, Shape::kPan, "Trance 2", 4.0, 1.0f);
    lane(q, Shape::kFilter, "Random Smooth", 1.0, 1.0f);
    Buf a = in, b = in;
    render(q, a, b, transport(120.0, 0.0, true));
    CHECK(allFinite(a) && peak(a) <= 6.0f);
}

// reset() forgets everything: afterwards it plays exactly as a new one. And a tail while the
// filter lane is on, none otherwise.
void shapeReset() {
    P p = with(Shape::kVolume, "Trance 3", 2.0, 0.8f);
    lane(p, Shape::kFilter, "Sine", 1.0, 1.0f);
    lane(p, Shape::kPan, "Triangle", 1.0, 0.6f);
    p.cutoffHz = 3000.0f;
    p.resonance = 0.7f;
    Shape used;
    Buf L = whiteNoise(30000, 0.8f, 13), R = whiteNoise(30000, 0.8f, 14);
    run(used, p, L, R, transport(133.0, 4.2, true));
    P q = p;   // leave it mid-glide, mid-fade
    q.lane[Shape::kVolume].on = false;
    q.filterType = Shape::kHP;
    used.set(q, Transport{});
    used.process(L.data(), R.data(), 17);
    used.reset();
    const Buf inL = whiteNoise(10000, 0.5f, 15), inR = whiteNoise(10000, 0.5f, 16);
    Buf a = inL, b = inR, c = inL, d = inR;
    run(used, p, a, b, transport(120.0, 1.1, true));
    render(p, c, d, transport(120.0, 1.1, true));
    CHECK(same(a, c) && same(b, d));
    used.reset();
    CHECK(used.tailSamples() == 0);
}

void shapeTail() {
    Shape m;
    P p = with(Shape::kFilter, "Sine", 1.0, 0.0f);
    p.cutoffHz = 1000.0f;
    const Transport t = transport(120.0, 0.0, true);
    Buf z(kChunk, 0.0f), z2(kChunk, 0.0f);
    m.set(p, t);
    m.process(z.data(), z2.data(), kChunk);
    const int soft = m.tailSamples();   // Q 0.7071 at 1 kHz: ln(1000) 2 Q / w = 69 samples
    CHECK(soft > 30 && soft < 200);
    p.resonance = 1.0f;
    m.set(p, t);
    const int sharp = m.tailSamples();   // Q 12 at 1 kHz: 1164 samples
    CHECK(sharp > 8 * soft && sharp < 2500);
    p.cutoffHz = 20.0f;
    p.lane[Shape::kFilter].depth = 1.0f;   // down to 1.25 Hz: held at 10 Hz, 116 000 samples
    m.set(p, t);
    CHECK(m.tailSamples() > 100000 && m.tailSamples() < 150000);
    // Switched off: nothing rings on once the 10 ms fade is over.
    p.lane[Shape::kFilter].on = false;
    Buf L(4410, 0.5f), R = L;
    run(m, p, L, R, t);
    CHECK(m.tailSamples() == 0);
    // Volume and Pan alone: no tail.
    P v = with(Shape::kVolume, "Sine", 1.0, 1.0f);
    lane(v, Shape::kPan, "Sine", 1.0, 1.0f);
    Shape n;
    n.set(v, t);
    CHECK(n.tailSamples() == 0);
}

// copyStateFrom() (ruling R48): a Shape that begins while its twin has been running begins as a copy of it: the lanes
// are where the twin's are, on a stopped transport too, so both move alike from the next sample whatever the audio.
// What the audio left in the filter is not copied: the copy has no ring where the twin has one.
void shapeCopyState() {
    P p;
    lane(p, Shape::kVolume, "Square", 1.0, 0.8f);
    lane(p, Shape::kPan, "Triangle", 2.0, 0.5f);
    lane(p, Shape::kFilter, "Sine", 1.0, 1.0f);
    p.cutoffHz = 300.0f;
    p.resonance = 0.9f;
    p.filterType = Shape::kLP;
    const size_t cut = 8192 + 96, total = 40000;
    const Transport t = transport(120.0, 8.0, false);   // stopped: a fresh one starts its grid over
    const Buf x = whiteNoise(static_cast<int>(total), 0.5f, 5);
    // Without the filter lane the copy and the twin move in step on other audio: the gains are the same.
    P gains = p;
    gains.lane[Shape::kFilter].on = false;
    Shape a, b, fresh;
    Buf La = x, Ra = x, Lb(total, 0.5f), Rb = Lb, Lf = Lb, Rf = Lb;
    for (size_t pos = 0; pos < total; pos += kChunk) {
        const int n = static_cast<int>(std::min(static_cast<size_t>(kChunk), total - pos));
        if (pos == cut) b.copyStateFrom(a);
        a.set(gains, t);
        a.process(&La[pos], &Ra[pos], n);
        if (pos >= cut) {
            b.set(gains, t);
            b.process(&Lb[pos], &Rb[pos], n);
            fresh.set(gains, t);
            fresh.process(&Lf[pos], &Rf[pos], n);
        }
    }
    // (La is the twin on noise, Lb the copy on DC 0.5: compare the gain, the twin's output over its input)
    double worst = 0.0, apart = 0.0;
    for (size_t i = cut + 64; i < total; ++i) {
        if (std::fabs(x[i]) > 0.05f) worst = std::max(worst, static_cast<double>(std::fabs(La[i] / x[i] - Lb[i] / 0.5f)));
        apart = std::max(apart, static_cast<double>(std::fabs(Lb[i] - Lf[i])));
    }
    CHECK(worst < 1e-5);
    CHECK(apart > 0.1);

    // With the filter: silence in both, and only the twin rings.
    Shape c, d;
    Buf Lx = x, Rx = x;
    for (size_t pos = 0; pos < 16384; pos += kChunk) {
        c.set(p, t);
        c.process(&Lx[pos], &Rx[pos], kChunk);
    }
    d.copyStateFrom(c);
    Buf zc(600, 0.0f), zd(600, 0.0f), zcr = zc, zdr = zd;
    for (size_t pos = 0; pos < 600; pos += kChunk) {
        const int n = static_cast<int>(std::min<size_t>(kChunk, 600 - pos));
        c.set(p, t);
        c.process(&zc[pos], &zcr[pos], n);
        d.set(p, t);
        d.process(&zd[pos], &zdr[pos], n);
    }
    CHECK(peak(zc) > 1e-3f);    // the twin's filter rings on
    CHECK(peak(zd) == 0.0f);    // the copy's has nothing in it
}

} // namespace

void shapeTests() {
    shapeRates();
    shapeRests();
    shapeVolumeSteps();
    shapeVolumeSine();
    shapeFilter();
    shapeFilterEdge();
    shapePan();
    shapeSwing();
    shapePhase();
    shapeEdges();
    shapeStopped();
    shapeSeededStopped();
    shapeFollowsJumps();
    shapeTempoChange();
    shapeExtremes();
    shapeRandomJumps();
    shapeRandomRepeats();
    shapeNoClicks();
    shapeReset();
    shapeTail();
    shapeCopyState();
}
