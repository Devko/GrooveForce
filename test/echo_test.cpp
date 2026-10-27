// The Echo (dsp/echo.h) against its laws: four lines wired five ways (an impulse comes back where the
// routing says, at each line's level and pan), the polyrhythm that was asked for (3/16 against 5/16,
// panned apart), the ring held bounded with feedback 1 and drive 1 for 60 s, the duck under the key,
// the freeze (the lines hold, the send closes over 20 ms), the tone and feedback offsets on every
// line, a modulator opening a line's cutoff after each kick, and the contract: nothing runs before
// allocate(), nothing clicks (a routing, a mode, a time, a switch), a line that is off costs nothing
// and comes back clean, NaN never gets into the loop, any parameters and any chunk length stay finite.
#include "signal.h"
#include "../dsp/echo.h"
#include "../dsp/shapes.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace eft;
using gf::Echo;
using P = Echo::Params;

constexpr int kSr = 44100;
constexpr int kChunk = ef::kChunk;

// An Echo with its lines (the chain allocates them the first time the Echo is switched on).
struct Rig : Echo {
    Rig() { CHECK(allocate()); }
};

// At 100 BPM a beat is 26460 samples, so every division the tests use is a whole number of samples:
// 1/16 6615, 1/8 13230, 3/16 19845, 1/4 26460, 5/16 33075.
constexpr double k16 = 0.25, k8 = 0.5, k316 = 0.75, k4 = 1.0, k516 = 1.25;
constexpr double kH0 = 0.9979;   // what an impulse keeps of its size in the lines' 30 Hz low cut

ef::Transport tempo(double bpm = 100.0) {   // stopped
    ef::Transport t;
    t.bpm = bpm;
    return t;
}
ef::Transport playingAt(double bpm, double beats) {
    ef::Transport t;
    t.bpm = bpm;
    t.beats = beats;
    t.playing = t.valid = true;
    return t;
}

// Line i on, synced to `beats`, the filter off, no wow or drive.
Echo::Line& on(P& p, int i, double beats, float fb = 0.0f) {
    Echo::Line& l = p.line[i];
    l.on = true;
    l.p.sync = true;
    l.p.divBeats = beats;
    l.p.feedback = fb;
    return l;
}
Echo::Line& onFree(P& p, int i, float ms, float fb = 0.0f) {
    Echo::Line& l = on(p, i, 0.5, fb);
    l.p.sync = false;
    l.p.timeMs = ms;
    return l;
}

struct Wet {
    Buf l, r;
};

// Runs the Echo over [from, to) of the inputs in chunks of `chunk` (set, then process); the wet goes
// to the same places in w.
void go(Echo& e, const P& p, const Buf& inL, const Buf& inR, const Buf& keyL, const Buf& keyR, Wet& w, size_t from, size_t to,
        ef::Transport t, int chunk = kChunk) {
    if (w.l.size() < inL.size()) {
        w.l.assign(inL.size(), 0.0f);
        w.r.assign(inL.size(), 0.0f);
    }
    for (size_t pos = from; pos < to; pos += static_cast<size_t>(chunk)) {
        const int n = static_cast<int>(std::min(static_cast<size_t>(chunk), to - pos));
        e.set(p, t);
        e.process(&inL[pos], &inR[pos], &keyL[pos], &keyR[pos], &w.l[pos], &w.r[pos], n);
        if (t.playing) t.beats += n / static_cast<double>(ef::kRate) * t.bpm / 60.0;
    }
}
// The key is the input (the chain's input, here the send's source as well).
Wet run(Echo& e, const P& p, const Buf& inL, const Buf& inR, ef::Transport t = tempo(), int chunk = kChunk) {
    Wet w;
    go(e, p, inL, inR, inL, inR, w, 0, inL.size(), t, chunk);
    return w;
}
Wet run(Echo& e, const P& p, const Buf& in, ef::Transport t = tempo()) { return run(e, p, in, in, t); }

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
bool same(const Buf& a, const Buf& b) { return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0; }
double maxDiff(const Buf& a, const Buf& b, size_t from = 0, size_t to = 0) {
    if (to == 0 || to > a.size()) to = a.size();
    double m = 0.0;
    for (size_t i = from; i < to; ++i) m = std::max(m, static_cast<double>(std::fabs(a[i] - b[i])));
    return m;
}
Buf silence(int n) { return Buf(static_cast<size_t>(n), 0.0f); }
// The biggest magnitude in [at - tol, at + tol].
float around(const Buf& x, size_t at, size_t tol = 1) {
    float m = 0.0f;
    for (size_t i = at - tol; i <= at + tol && i < x.size(); ++i) m = std::max(m, std::fabs(x[i]));
    return m;
}
// The biggest magnitude outside a radius of every place in `at`.
float quietExcept(const Buf& x, const std::vector<size_t>& at, size_t radius) {
    float m = 0.0f;
    for (size_t i = 0; i < x.size(); ++i) {
        bool near = false;
        for (size_t a : at) near = near || (i + radius >= a && i <= a + radius);
        if (!near) m = std::max(m, std::fabs(x[i]));
    }
    return m;
}
// The centre of the energy in [from, to).
double centroid(const Buf& x, size_t from, size_t to) {
    double e = 0.0, c = 0.0;
    for (size_t i = from; i < to; ++i) {
        const double v = static_cast<double>(x[i]) * x[i];
        e += v;
        c += v * static_cast<double>(i);
    }
    return e > 0.0 ? c / e : 0.0;
}
// A tone's gain through the wet, in dB: the wet's `hz` component against the input's.
double gainDb(const Buf& wet, const Buf& in, double hz, size_t from, size_t to) { return db(magnitude(wet, hz, from, to) / magnitude(in, hz, from, to)); }

// The pan law, as Shape's Pan lane has it: a = pi/4 pan, left cos a - sin a, right cos a + sin a (1 and 1 in the middle).
void panGains(double pan, double& l, double& r) {
    const double a = kPi / 4.0 * pan;
    l = std::cos(a) - std::sin(a);
    r = std::cos(a) + std::sin(a);
}

int shapeIndex(const char* name) {
    for (int i = 0; i < gf::numShapes(); ++i)
        if (std::string(gf::shapeName(i)) == name) return i;
    std::printf("  no shape named %s\n", name);
    CHECK(false);
    return 0;
}

// A bright blip: 3 kHz, 20 ms, decaying, starting at once at `at`: a hit the transient detector finds.
void addBlip(Buf& x, int at, float amp) {
    const double tau = 0.008 * kSr;
    for (int i = 0; at + i < static_cast<int>(x.size()) && i < static_cast<int>(0.020 * kSr); ++i)
        x[static_cast<size_t>(at + i)] += amp * static_cast<float>(std::sin(2.0 * kPi * 3000.0 * i / kSr) * std::exp(-i / tau));
}

// --- the routings ------------------------------------------------------------------------------------

void echoRests() {       // defaults: the wet is silent, and nothing runs
    {
        Rig e;
        P p;
        const Wet w = run(e, p, whiteNoise(kSr / 2, 0.5f, 1), whiteNoise(kSr / 2, 0.5f, 2));
        CHECK(peak(w.l) == 0.0f && peak(w.r) == 0.0f);
        CHECK(e.running() == 0 && e.tailSamples() == 0 && e.peakStored() == 0.0f);
    }
    {   // the wet is written, not added to
        Rig e;
        P p;
        Buf in = whiteNoise(kChunk, 0.5f, 3), L(kChunk, 1.0f), R(kChunk, -1.0f);
        e.set(p, tempo());
        e.process(in.data(), in.data(), in.data(), in.data(), L.data(), R.data(), kChunk);
        CHECK(peak(L) == 0.0f && peak(R) == 0.0f);
    }
    {   // a line that is on and then off: after its 10 ms fade it is neither read nor written
        Rig e;
        P p;
        on(p, 0, k8, 0.5f);
        const Buf in = whiteNoise(kSr, 0.5f, 4);
        Wet w;
        go(e, p, in, in, in, in, w, 0, kSr / 2, tempo());
        CHECK(e.running() == 1);
        p.line[0].on = false;
        go(e, p, in, in, in, in, w, kSr / 2, kSr / 2 + 441 + kChunk, tempo());
        CHECK(e.running() == 0);
        go(e, p, in, in, in, in, w, kSr / 2 + 441 + kChunk, kSr, tempo());
        CHECK(peak(w.l, kSr / 2 + 441 + kChunk) == 0.0f && peak(w.r, kSr / 2 + 441 + kChunk) == 0.0f && e.running() == 0);
    }
}

void echoOneLine() {     // one line, everything else at its default, is the line (bit for bit)
    Rig e;
    gf::EchoLine line;
    CHECK(line.allocate());
    P p;
    Echo::Line& l = on(p, 0, k316, 0.5f);
    l.p.mode = gf::EchoLine::kPingPong;
    l.p.filterType = gf::EchoLine::kBP;
    l.p.cutoffHz = 1500.0f;
    l.p.resonance = 0.4f;
    l.p.drive = 0.3f;
    l.p.wow = 0.5f;
    const Buf inL = whiteNoise(kSr, 0.4f, 5), inR = whiteNoise(kSr, 0.4f, 6);
    const Wet w = run(e, p, inL, inR);
    Buf oL(inL.size()), oR(inL.size());
    for (size_t pos = 0; pos < inL.size(); pos += kChunk) {
        const int m = static_cast<int>(std::min(static_cast<size_t>(kChunk), inL.size() - pos));
        line.set(l.p, tempo());
        line.read(&oL[pos], &oR[pos], m);
        line.write(&inL[pos], &inR[pos], m);
    }
    CHECK(same(w.l, oL) && same(w.r, oR));
}

void echoParallel() {    // A at 1/8 and B at 5/16: an impulse comes back at both times, from each line alone
    Rig e;
    P p;
    on(p, 0, k8);
    on(p, 1, k516);
    const int n = 60000, at = 100;
    const Buf in = impulseAt(n, at);
    const Wet w = run(e, p, in);
    CHECK(e.running() == 2);
    for (const Buf* ch : {&w.l, &w.r}) {
        CHECK(near((*ch)[at + 13230], kH0, 0.01) && near((*ch)[at + 33075], kH0, 0.01));
        CHECK(around(*ch, at + 13230 - 1, 0) < 0.01f && around(*ch, at + 13230 + 1, 0) < 0.01f);
        CHECK(quietExcept(*ch, {at + 13230, at + 33075}, 2000) < 0.003f);
    }
    // Each line on its own makes only its own repeat.
    for (int i = 0; i < 2; ++i) {
        Rig f;
        P q;
        on(q, i, i ? k516 : k8);
        const Wet v = run(f, q, in);
        CHECK(near(v.l[at + (i ? 33075 : 13230)], kH0, 0.01) && quietExcept(v.l, {at + static_cast<size_t>(i ? 33075 : 13230)}, 2000) < 0.003f);
    }
    // A line that is off hears nothing, whatever it is set to.
    P q = p;
    q.line[1].on = false;
    Rig f;
    const Wet v = run(f, q, in);
    CHECK(quietExcept(v.l, {at + 13230}, 2000) < 0.003f && f.running() == 1);
}

void echoSerial() {      // A <- x, B <- yA: B's first repeat comes at A + B, and B alone makes none
    Rig e;
    P p;
    on(p, 0, k8);
    on(p, 1, k316);
    p.routing = Echo::kSerial;
    const int n = 50000, at = 100;
    const Wet w = run(e, p, impulseAt(n, at));
    CHECK(near(w.l[at + 13230], kH0, 0.01));                      // A, heard
    CHECK(near(w.l[at + 13230 + 19845], kH0 * kH0, 0.01));        // B, heard: A's repeat a second time
    CHECK(quietExcept(w.l, {at + 13230, at + 13230 + 19845}, 2000) < 0.003f);   // nothing at B's own time
    // C and D after B, each fed by the one before.
    Rig f;
    P q = p;
    on(q, 2, k16);
    on(q, 3, k4);
    const Wet v = run(f, q, impulseAt(n + 40000, at));
    const size_t a = at + 13230, b = a + 19845, c = b + 6615, d = c + 26460;
    CHECK(near(v.l[a], kH0, 0.01) && near(v.l[b], std::pow(kH0, 2), 0.01) && near(v.l[c], std::pow(kH0, 3), 0.01) && near(v.l[d], std::pow(kH0, 4), 0.01));
    CHECK(quietExcept(v.l, {a, b, c, d}, 2000) < 0.003f);
    // The chain closes over the lines that are on (R63): B off, so C takes A's repeat and D C's.
    q.line[1].on = false;
    Rig g;
    const Wet u = run(g, q, impulseAt(n + 40000, at));
    const size_t c2 = a + 6615, d2 = c2 + 26460;
    CHECK(near(u.l[a], kH0, 0.01) && near(u.l[c2], kH0 * kH0, 0.01) && near(u.l[d2], std::pow(kH0, 3), 0.01));
    CHECK(quietExcept(u.l, {a, c2, d2}, 2000) < 0.003f && g.running() == 3);
    // A off: B is the first line that is on, so it takes the send.
    P r;
    on(r, 1, k316);
    r.routing = Echo::kSerial;
    Rig h;
    const Wet t = run(h, r, impulseAt(n, at));
    CHECK(near(t.l[at + 19845], kH0, 0.01) && quietExcept(t.l, {at + 19845}, 2000) < 0.003f);
}

void echoPairs() {       // A -> B and C -> D, the pairs side by side
    Rig e;
    P p;
    on(p, 0, k8);
    on(p, 1, k316);
    on(p, 2, k4);
    on(p, 3, k8);
    p.routing = Echo::kPairs;
    const size_t at = 100, a = at + 13230, b = a + 19845, c = at + 26460, d = c + 13230;
    const Wet w = run(e, p, impulseAt(60000, static_cast<int>(at)));
    CHECK(near(w.l[a], kH0, 0.01) && near(w.l[b], kH0 * kH0, 0.01) && near(w.l[c], kH0, 0.01) && near(w.l[d], kH0 * kH0, 0.01));
    CHECK(quietExcept(w.l, {a, b, c, d}, 2000) < 0.003f);
    // A pair closes over its lines that are on (R63): with A off, B takes the send; with C off, D does.
    for (int alone : {1, 3}) {
        P q;
        on(q, alone, alone == 1 ? k316 : k8);
        q.routing = Echo::kPairs;
        Rig f;
        const Wet v = run(f, q, impulseAt(60000, static_cast<int>(at)));
        const size_t hit = at + (alone == 1 ? 19845 : 13230);
        CHECK(near(v.l[hit], kH0, 0.01) && quietExcept(v.l, {hit}, 2000) < 0.003f);
    }
}

void echoSpread() {      // A <- x, then B, C and D <- yA: one hit, then a cloud
    Rig e;
    P p;
    on(p, 0, k8);
    on(p, 1, k16);
    on(p, 2, k316);
    on(p, 3, k4);
    p.routing = Echo::kSpread;
    const size_t at = 100, a = at + 13230, b = a + 6615, c = a + 19845, d = a + 26460;
    const Wet w = run(e, p, impulseAt(60000, static_cast<int>(at)));
    CHECK(near(w.l[a], kH0, 0.01) && near(w.l[b], kH0 * kH0, 0.01) && near(w.l[c], kH0 * kH0, 0.01) && near(w.l[d], kH0 * kH0, 0.01));
    CHECK(quietExcept(w.l, {a, b, c, d}, 2000) < 0.003f);
    // With A off the spread closes over the lines that are on (R63): B, C and D each take the send.
    P q = p;
    q.line[0].on = false;
    Rig f;
    const Wet v = run(f, q, impulseAt(60000, static_cast<int>(at)));
    const size_t b1 = at + 6615, c1 = at + 19845, d1 = at + 26460;
    CHECK(near(v.l[b1], kH0, 0.01) && near(v.l[c1], kH0, 0.01) && near(v.l[d1], kH0, 0.01));
    CHECK(quietExcept(v.l, {b1, c1, d1}, 2000) < 0.003f);
}

void echoRing() {        // A <- x + yD, B <- yA, C <- yB, D <- yC: the hit goes round
    Rig e;
    P p;
    on(p, 0, k8, 1.0f);   // (the feedbacks are the send round the ring: R39)
    on(p, 1, k16, 1.0f);
    on(p, 2, k16, 1.0f);
    on(p, 3, k16, 1.0f);
    p.routing = Echo::kRing;
    const size_t at = 100;
    const size_t delays[4] = {13230, 6615, 6615, 6615};   // A, B, C, D: 33075 round
    std::vector<size_t> hits;
    size_t t = at;
    for (size_t k = 0; k < 12; ++k) {
        t += delays[k % 4];
        hits.push_back(t);
    }
    const Wet w = run(e, p, impulseAt(110000, static_cast<int>(at)));
    for (size_t k = 0; k < 12; ++k) {   // each pass through a line is the low cut once more
        const double want = std::pow(kH0, static_cast<double>(k + 1));
        if (!near(w.l[hits[k]], want, 0.01)) std::printf("  ring pass %zu: %g, wanted %g\n", k + 1, static_cast<double>(w.l[hits[k]]), want);
        CHECK(near(w.l[hits[k]], want, 0.01));
    }
    CHECK(quietExcept(w.l, hits, 2000) < 0.003f);
    // With a line off the ring closes over the others (R46): D off, the hit goes A, B, C, A, B, C ... (a trip is 26460).
    P q = p;
    q.line[3].on = false;
    Rig f;
    const size_t delays3[3] = {13230, 6615, 6615};
    std::vector<size_t> hits3;
    t = at;
    for (size_t k = 0; k < 12; ++k) {
        t += delays3[k % 3];
        hits3.push_back(t);
    }
    const Wet v = run(f, q, impulseAt(110000, static_cast<int>(at)));
    for (size_t k = 0; k < hits3.size(); ++k) CHECK(near(v.l[hits3[k]], std::pow(kH0, static_cast<double>(k + 1)), 0.01));
    CHECK(quietExcept(v.l, hits3, 2000) < 0.003f);
    // The send goes to the first line that is on: A off, the ring is B, C, D and the hit enters at B.
    P r = p;
    r.line[0].on = false;
    Rig g;
    const Wet u = run(g, r, impulseAt(110000, static_cast<int>(at)));
    std::vector<size_t> hitsBCD;
    for (size_t k = 1; k <= 16; ++k) hitsBCD.push_back(at + 6615 * k);   // B, C, D, B ... all 6615 apart
    for (size_t k = 0; k < hitsBCD.size(); ++k) CHECK(near(u.l[hitsBCD[k]], std::pow(kH0, static_cast<double>(k + 1)), 0.01));
    CHECK(quietExcept(u.l, hitsBCD, 2000) < 0.003f);
}

void echoLoopScale() {   // R36: what a routing passes on is scaled by the source's loopScale(); what is heard is not
    // A ring of four resonant low-passes at 1 kHz (Q 12), feedback 1, a 1 kHz burst going in: every pass through a line
    // gives the burst 12 times (the filter's peak), the routing gives it back 1/12, so the burst goes round at the size it
    // came in (less the low cut's 0.001 dB a pass) and does not build. What is heard is the filter's output.
    Rig e;
    P p;
    p.routing = Echo::kRing;
    for (int i = 0; i < 4; ++i) {
        Echo::Line& l = onFree(p, i, 100.0f, 1.0f);
        l.p.filterType = gf::EchoLine::kLP;
        l.p.cutoffHz = 1000.0f;
        l.p.resonance = 1.0f;
        l.p.place = gf::EchoLine::kInLoop;
    }
    Buf in = silence(5 * kSr);
    {
        const Buf b = sine(1000.0, 2205, 0.05f);   // 50 ms
        std::copy(b.begin(), b.end(), in.begin() + 1000);
    }
    const Wet w = run(e, p, in);
    auto pass = [&](int k) { return rms(w.l, static_cast<size_t>(1000 + k * 4410 + 1100), static_cast<size_t>(1000 + k * 4410 + 2200)); };
    const double first = pass(1), second = pass(5), later = pass(21);   // A's first read, A's second (round once), five times round
    // (it does lose a little each trip: four narrow filters and the low cut take the burst's edges)
    if (!(second < 1.05 * first && second > 0.7 * first && later < 1.05 * first && later > 0.05 * first))
        std::printf("  loop scale: %g, %g, %g\n", first, second, later);
    CHECK(second < 1.05 * first && second > 0.7 * first && later < 1.05 * first && later > 0.05 * first);
    CHECK(first > 0.4 * 0.05 * 12 * 0.707 * 0.5);   // heard unscaled: about 12 times the burst (rms of a sine, a little under its peak gain)
    CHECK(peak(w.l) < 1.5f);
}

// R39: in a Ring the feedbacks are the send round the ring, and the lines have none of their own.
void echoRingFeedback() {
    {   // the hit goes round at the feedbacks it has met: 0.9 after A, 0.5 after B, 0.7 after C, 0.8 after D
        Rig e;
        P p;
        const double beats[4] = {k8, k16, k8, k16};
        const float fb[4] = {0.9f, 0.5f, 0.7f, 0.8f};
        const size_t delays[4] = {13230, 6615, 13230, 6615};
        for (int i = 0; i < 4; ++i) on(p, i, beats[i], fb[i]);
        p.routing = Echo::kRing;
        const size_t at = 100;
        std::vector<size_t> hits;
        std::vector<double> want;
        size_t t = at;
        double a = 1.0;
        for (size_t k = 0; k < 10; ++k) {
            t += delays[k % 4];
            hits.push_back(t);
            a *= kH0 * (k ? static_cast<double>(fb[(k - 1) % 4]) : 1.0);   // the low cut of the line, the feedback it was passed on at
            want.push_back(a);
        }
        const Wet w = run(e, p, impulseAt(110000, static_cast<int>(at)));
        for (size_t k = 0; k < hits.size(); ++k) {
            if (!near(w.l[hits[k]], want[k], 0.003 + 0.01 * want[k])) std::printf("  ring pass %zu: %g, wanted %g\n", k + 1, static_cast<double>(w.l[hits[k]]), want[k]);
            CHECK(near(w.l[hits[k]], want[k], 0.003 + 0.01 * want[k]));
        }
        CHECK(quietExcept(w.l, hits, 2000) < 0.003f);
    }
    {   // a line alone in a Ring feeds itself at its feedback (R46): it is a line with that feedback
        Rig e;
        P p;
        on(p, 0, k8, 0.8f);
        p.routing = Echo::kRing;
        const Wet w = run(e, p, impulseAt(100000, 100));
        std::vector<size_t> alone;
        for (size_t k = 1; k <= 7; ++k) {
            alone.push_back(100 + 13230 * k);
            const double want = std::pow(kH0, static_cast<double>(k)) * std::pow(0.8, static_cast<double>(k - 1));
            CHECK(near(w.l[alone.back()], want, 0.003 + 0.01 * want));
        }
        CHECK(quietExcept(w.l, alone, 2000) < 0.01f);
        // and the same as the line with that feedback in a Parallel (noise in, filter off)
        const Buf noise = whiteNoise(3 * kSr, 0.3f, 60);
        Rig f, g;
        P par = p;
        par.routing = Echo::kParallel;
        const Wet a = run(f, p, noise), b = run(g, par, noise);
        if (maxDiff(a.l, b.l) > 1e-5) std::printf("  a lone line in a Ring against a line: differs by %g\n", maxDiff(a.l, b.l));
        CHECK(maxDiff(a.l, b.l) < 1e-5 && maxDiff(a.r, b.r) < 1e-5);
    }
    const double ms[4] = {100.0, 150.0, 80.0, 120.0};   // 450 ms round
    // The ring closes over the lines that are on (R46): all four, A B D, B C and D alone, each at 0.9: it dies away (0.9 a trip a line)
    // and never reaches a limiter, so it is linear in the input.
    const unsigned masks[4] = {0xF, 0xB, 0x6, 0x8};
    for (unsigned mask : masks) {
        Wet w[2];
        for (int k = 0; k < 2; ++k) {
            Rig e;
            P p;
            p.routing = Echo::kRing;
            for (int i = 0; i < 4; ++i)
                if (mask >> i & 1) onFree(p, i, static_cast<float>(ms[i]), 0.9f);
            Buf x = silence(15 * kSr);
            const Buf noise = whiteNoise(kSr, k ? 0.1f : 0.05f, 40);
            std::copy(noise.begin(), noise.end(), x.begin());
            w[k] = run(e, p, x);
        }
        double worst = 0.0;
        for (size_t i = 0; i < w[0].l.size(); ++i) worst = std::max(worst, static_cast<double>(std::fabs(w[1].l[i] - 2.0f * w[0].l[i])));
        // (linear to the last bit: the 0.2 run is twice the 0.1 run, so no limiter ever acted)
        if (!(worst < 1e-5 && rms(w[0].l, 14 * kSr, 15 * kSr) < 1e-3 * rms(w[0].l, kSr / 2, kSr)))
            std::printf("  ring of mask %x at 0.9: linearity %g\n", mask, worst);
        CHECK(worst < 1e-5);
        CHECK(rms(w[0].l, kSr / 2, kSr) > 0.01);                                      // it rings
        CHECK(rms(w[0].l, 14 * kSr, 15 * kSr) < 1e-3 * rms(w[0].l, kSr / 2, kSr));   // and dies
    }
    // 1 on all the lines that are on holds without growing (the low cut takes a little a pass), 60 s with four, 30 s with 3, 2 and 1;
    // the energy late against early within 1 dB, never over the limiter.
    const struct { unsigned mask; int blocks; } holds[4] = {{0xF, 600}, {0xB, 300}, {0x5, 300}, {0x2, 300}};
    for (const auto& h : holds) {
        Rig e;
        P p;
        p.routing = Echo::kRing;
        for (int i = 0; i < 4; ++i)
            if (h.mask >> i & 1) onFree(p, i, static_cast<float>(ms[i]), 1.0f);
        const int block = 4410;
        double early = 0.0, late = 0.0;
        float top = 0.0f;
        bool finite = true;
        Wet w;
        for (int b = 0; b < h.blocks; ++b) {
            const Buf L = b < 5 ? whiteNoise(block, 0.3f, 100 + static_cast<uint32_t>(2 * b)) : silence(block);
            const Buf R = b < 5 ? whiteNoise(block, 0.3f, 101 + static_cast<uint32_t>(2 * b)) : silence(block);
            go(e, p, L, R, L, R, w, 0, static_cast<size_t>(block), tempo());
            finite = finite && allFinite(w.l) && allFinite(w.r);
            top = std::max({top, peak(w.l), peak(w.r)});
            if (b >= 100 && b < 110) early += rms(w.l) * rms(w.l);
            if (b >= h.blocks - 30 && b < h.blocks - 20) late += rms(w.l) * rms(w.l);
        }
        if (!(late <= 1.001 * early && late > 0.79 * early)) std::printf("  ring held (mask %x): %g late against %g early\n", h.mask, late, early);
        CHECK(finite && top <= 8.0f && e.peakStored() <= 1.00001f);
        CHECK(late <= 1.001 * early && late > 0.79 * early);   // (within 1 dB)
    }
    {   // the freeze takes the feedbacks to 1: the passes after it hold their size
        Rig e;
        P p;
        p.routing = Echo::kRing;
        for (int i = 0; i < 4; ++i) onFree(p, i, 100.0f, 0.5f);
        const size_t F = 9216;   // after the second pass (8920), before the third (13330)
        const Buf hit = impulseAt(60000, 100);
        Wet w;
        go(e, p, hit, hit, hit, hit, w, 0, F, tempo());
        p.freeze = true;
        go(e, p, hit, hit, hit, hit, w, F, hit.size(), tempo());
        auto pass = [&](size_t k) { return std::fabs(w.l[100 + 4410 * k]); };
        CHECK(near(pass(1), kH0, 0.01) && near(pass(2), 0.5 * kH0 * kH0, 0.01) && near(pass(3), 0.25 * std::pow(kH0, 3), 0.01));
        for (size_t k = 4; k <= 10; ++k) CHECK(near(db(pass(k) / pass(3)), 0.0, 0.3));
    }
}

// R44: frozen, the lines hold what they have in every routing: the feeds between them stop, as the send does, and nothing grows.
void echoFreezeRoutings() {
    const int n = 25 * kSr;
    Buf in = silence(n);
    for (int k = 0; k < 16; ++k) {   // a hat pattern: sixteenths of a decaying noise burst, two seconds
        const Buf b = whiteNoise(1500, 0.6f, 50 + static_cast<uint32_t>(k));
        for (int i = 0; i < 1500; ++i) in[static_cast<size_t>(k * 5512 + i)] = b[static_cast<size_t>(i)] * static_cast<float>(std::exp(-i / 300.0));
    }
    const size_t F = 3 * kSr - (3 * kSr) % kChunk;
    const double beats[4] = {0.75, 0.5, 0.375, 1.25};
    for (int routing : {Echo::kParallel, Echo::kSerial, Echo::kPairs, Echo::kSpread})
        for (int filter = 0; filter < 3; ++filter) {   // none, a band-pass at 5 kHz, and a resonant one
            Rig e;
            P p;
            p.routing = routing;
            for (int i = 0; i < 4; ++i) {
                Echo::Line& l = on(p, i, beats[i], 0.45f);
                l.pan = i % 2 ? 0.6f : -0.6f;
                if (filter) {
                    l.p.filterType = gf::EchoLine::kBP;
                    l.p.cutoffHz = 5000.0f;
                    l.p.resonance = filter == 2 ? 0.6f : 0.0f;
                }
            }
            Wet w;
            go(e, p, in, in, in, in, w, 0, F, tempo(120.0));
            P f = p;
            f.freeze = true;
            go(e, f, in, in, in, in, w, F, in.size(), tempo(120.0));
            const double early = rms(w.l, 4 * kSr, 6 * kSr) + rms(w.r, 4 * kSr, 6 * kSr);
            const double late = rms(w.l, 22 * kSr, 24 * kSr) + rms(w.r, 22 * kSr, 24 * kSr);
            const double drift = db(late / early);
            if (!(drift < 1.0 && drift > -12.0 && e.peakStored() < 0.95f))
                std::printf("  frozen, routing %d, filter %d: %.1f dB over 18 s, stored %g\n", routing, filter, drift, static_cast<double>(e.peakStored()));
            CHECK(drift < 1.0 && drift > -12.0);       // it holds (the low cuts take a little a pass), it does not swell
            CHECK(e.peakStored() < 0.95f);             // and no limiter had to catch it
        }
}

// R45: a ping-pong line in a Ring runs as Stereo inside and passes its feed on with the sides swapped: the ping-pong goes round the ring.
void echoRingPingPong() {
    for (int which = 0; which < 2; ++which) {   // A alone, then all four
        Rig e;
        P p;
        p.routing = Echo::kRing;
        for (int i = 0; i < 4; ++i) {
            Echo::Line& l = onFree(p, i, 100.0f, 1.0f);
            if (which == 1 || i == 0) l.p.mode = gf::EchoLine::kPingPong;
        }
        const int n = 11 * kSr;
        Buf hit = silence(n);
        {
            const Buf b = sine(1000.0, 882, 0.5f);
            for (size_t i = 0; i < b.size(); ++i) hit[100 + i] = b[i] * static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / 882.0));
        }
        const Wet w = run(e, p, hit);
        auto pass = [&](int k, double& l, double& r) {
            const size_t at = 100 + 4410 * static_cast<size_t>(k);
            l = rms(w.l, at, at + 882);
            r = rms(w.r, at, at + 882);
        };
        double l1, r1;
        pass(1, l1, r1);
        const double first = l1 * l1 + r1 * r1;
        CHECK(first > 0.01);
        for (int k = 1; k <= 100; ++k) {
            double l, r;
            pass(k, l, r);
            // the side: A is on the left in the first trip (the send comes in mono in L), then B, C and D on the right; with one
            // ping-pong line the sides alternate trip by trip, with four pass by pass
            const bool left = which == 0 ? ((k - 1) / 4 % 2 == 0) == ((k - 1) % 4 == 0) : (k % 2 == 1);
            const bool ok = left ? (l > 20.0 * r) : (r > 20.0 * l);
            if (!ok && k <= 12) std::printf("  ping-pong ring (%s), pass %d: L %g R %g\n", which ? "four" : "A", k, l, r);
            if (k <= 12 || k == 100) CHECK(ok);
        }
        double l100, r100;
        pass(100, l100, r100);
        const double last = l100 * l100 + r100 * r100;
        if (!near(db(last / first), 0.0, 1.0)) std::printf("  ping-pong ring (%s): %g dB after 10 s\n", which ? "four" : "A", db(last / first));
        CHECK(near(db(last / first), 0.0, 1.0));   // holds its level (within 1 dB over 10 s)
    }
}

// R45, the send: a ping-pong line in a Ring takes it as its mono sum in L only, so a send out of phase in the two sides gives nothing.
void echoRingPingPongSend() {
    Rig e, f;
    P p;
    p.routing = Echo::kRing;
    for (int i = 0; i < 4; ++i) {
        Echo::Line& l = onFree(p, i, 100.0f, 1.0f);
        if (i == 0) l.p.mode = gf::EchoLine::kPingPong;
    }
    Buf b = sine(1000.0, 882, 0.5f);
    for (size_t i = 0; i < b.size(); ++i) b[i] *= static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / 882.0));
    Buf inL = silence(20000), inR = silence(20000);
    std::copy(b.begin(), b.end(), inL.begin() + 100);
    for (size_t i = 0; i < b.size(); ++i) inR[100 + i] = -b[i];
    const Wet w = run(e, p, inL, inR);
    CHECK(rms(w.l, 4510, 4510 + 882) < 1e-3 && rms(w.r, 4510, 4510 + 882) < 1e-3);   // (0.5 (L + R) = 0)
    // and the same send into a stereo line goes in as it is
    P q = p;
    q.line[0].p.mode = gf::EchoLine::kStereo;
    const Wet v = run(f, q, inL, inR);
    CHECK(rms(v.l, 4510, 4510 + 882) > 0.1 && rms(v.r, 4510, 4510 + 882) > 0.1);
}

// R46: a line leaving the Ring (switched off) is out of it at once: what is passing goes on to the next line that is on.
void echoRingLeaves() {
    Rig e;
    P p;
    p.routing = Echo::kRing;
    for (int i = 0; i < 3; ++i) {
        Echo::Line& l = onFree(p, i, 100.0f, 1.0f);
        l.pan = i == 2 ? 1.0f : -1.0f;   // the right side is C alone
    }
    Buf b = sine(1000.0, 882, 0.5f);
    for (size_t i = 0; i < b.size(); ++i) b[i] *= static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / 882.0));
    Buf in = silence(30000);
    std::copy(b.begin(), b.end(), in.begin() + 100);
    Wet w;
    go(e, p, in, in, in, in, w, 0, 4608, tempo());   // A reads the burst at 4510 and passes it to B
    p.line[1].on = false;
    go(e, p, in, in, in, in, w, 4608, in.size(), tempo());
    // The rest of the burst (from 4608) goes A -> C now and C plays it 100 ms on, at 9018: it is there at once, not 10 ms later,
    // when B would have finished fading out.
    const double rest = rms(w.r, 9018, 9018 + 300);
    if (!(rest > 0.05)) std::printf("  a line leaving the ring: %g\n", rest);
    CHECK(rest > 0.05);
}

// R47: the shared follower starts from nothing when it resumes after chunks nobody used it (a stale one holds the key of a while ago).
void echoFollowResumes() {
    const int n = 3 * kSr;
    const Buf tone = sine(1000.0, n, 0.3f);
    Buf key = sine(500.0, n, 0.5f);
    std::fill(key.begin() + kSr, key.end(), 0.0f);   // the key falls silent at 1 s
    P p;
    Echo::Line& l = onFree(p, 0, 5.0f);
    l.mod.mode = gf::EchoMod::kFollow;
    l.modLevel = 1.0f;
    P off = p;
    off.line[0].mod.mode = gf::EchoMod::kOff;
    Rig e;
    Wet w;
    go(e, p, tone, tone, key, key, w, 0, kSr, tempo());                 // Follow on a loud key
    go(e, off, tone, tone, key, key, w, kSr, 2 * kSr, tempo());         // Off: nobody follows, the key is silent now
    go(e, p, tone, tone, key, key, w, 2 * kSr, n, tempo());             // Follow again on the silent key: the level is 0, the line closed
    CHECK(rms(w.l, kSr / 2, kSr) > 0.05);                              // (it did follow the loud key)
    if (!(rms(w.l, 2 * kSr, 2 * kSr + 2205) < 1e-3)) std::printf("  follow resumes: heard %g in the first 50 ms\n", rms(w.l, 2 * kSr, 2 * kSr + 2205));
    CHECK(rms(w.l, 2 * kSr, 2 * kSr + 2205) < 1e-3);
}

// R41: one detector and one follower for the lines are what one each would find.
void echoSharedKey() {
    const int n = 5 * kSr;
    Buf hits = silence(n);
    for (int a : {13230, 60000, 100000, 140000, 180000}) addBlip(hits, a, 0.5f);
    for (size_t i = 0; i < hits.size(); ++i) hits[i] += 0.002f * static_cast<float>(std::sin(0.01 * static_cast<double>(i)));   // a floor under them
    P all;
    all.sensitivity = 0.7f;
    for (int i = 0; i < 4; ++i) {
        Echo::Line& l = on(all, i, i % 2 ? k8 : k16);
        l.p.filterType = gf::EchoLine::kLP;
        l.p.cutoffHz = 800.0f;
        l.pan = 0.3f * static_cast<float>(i) - 0.45f;
        l.mod.mode = i == 2 ? gf::EchoMod::kFollow : (i == 3 ? gf::EchoMod::kEnv : gf::EchoMod::kEnv);
        l.mod.rateBeats = i == 0 ? 1.0 : (i == 1 ? 3.0 : 0.5);
        l.modLevel = 0.8f;
        l.modCutoff = 0.6f;
        l.modTime = 0.4f;
    }
    Rig together;
    const Wet sum = run(together, all, hits);
    Buf wantL(static_cast<size_t>(n), 0.0f), wantR(static_cast<size_t>(n), 0.0f);
    for (int i = 0; i < 4; ++i) {
        P one = all;
        for (int j = 0; j < 4; ++j) one.line[j].on = j == i;
        Rig alone;
        const Wet w = run(alone, one, hits);
        for (size_t k = 0; k < wantL.size(); ++k) {
            wantL[k] += w.l[k];
            wantR[k] += w.r[k];
        }
    }
    // (the sum in the same order, so the same but for the device's fused multiply-adds)
    if (maxDiff(sum.l, wantL) > 1e-6) std::printf("  shared key: differs by %g\n", maxDiff(sum.l, wantL));
    CHECK(maxDiff(sum.l, wantL) < 1e-6 && maxDiff(sum.r, wantR) < 1e-6);
    CHECK(peak(sum.l) > 0.05f);
    // Each line's Env falls at its own rate: A (1 beat) is down 7.5 dB further than B (3 beats) a quarter of a beat on... heard through
    // their levels alone, panned to the two sides.
    P two;
    two.sensitivity = 0.7f;
    for (int i = 0; i < 2; ++i) {
        Echo::Line& l = on(two, i, k16);
        l.mod.mode = gf::EchoMod::kEnv;
        l.mod.rateBeats = i ? 4.0 : 1.0;
        l.modLevel = 1.0f;
        l.pan = i ? 1.0f : -1.0f;
    }
    Rig e;
    const Wet w = run(e, two, hits);
    const size_t at = 13230 + 6615;   // the hit's repeat, 1/16 on: a sixteenth of the way down the 4-beat fall, a quarter of the 1-beat
    const double left = rms(w.l, at, at + 900), right = rms(w.r, at, at + 900);
    CHECK(left > 0.0 && right > 0.0 && left < 0.4 * right);
}

void echoRoutingChange() {   // a routing change is a crossfade: afterwards the new routing is exactly what a fresh Echo has
    for (int to = 0; to < 5; ++to) {
        P p;
        on(p, 0, k8);
        on(p, 1, k316);
        on(p, 2, k16);
        on(p, 3, k4);
        p.routing = (to + 2) % 5;
        P q = p;
        q.routing = to;
        const size_t n = 3 * kSr;
        const Buf quiet = silence(static_cast<int>(n));
        const Buf hit = impulseAt(static_cast<int>(n), 2 * kSr);   // long after the change
        Rig e, f;
        Wet w, v;
        go(e, p, quiet, quiet, quiet, quiet, w, 0, kSr, tempo());
        go(e, q, hit, hit, hit, hit, w, kSr, n, tempo());
        // (the first second of f is silence, so all its state is fresh and the same)
        go(f, q, quiet, quiet, quiet, quiet, v, 0, kSr, tempo());
        go(f, q, hit, hit, hit, hit, v, kSr, n, tempo());
        // (the tiny constant under everything the lines write, 1e-18, carries the history of the first second: it is all that differs)
        if (!(maxDiff(w.l, v.l) < 1e-9)) std::printf("  routing %d: differs from a fresh Echo by %g\n", to, maxDiff(w.l, v.l));
        CHECK(maxDiff(w.l, v.l) < 1e-9 && maxDiff(w.r, v.r) < 1e-9);
        CHECK(peak(w.l, 2 * kSr, 2 * kSr + 20000) > 0.5f * 0.5f);
    }
}

void echoOff() {         // a line that is off is not read or written, and comes back from nothing
    Rig e;
    P p;
    onFree(p, 1, 100.0f, 0.6f).mod.mode = gf::EchoMod::kLfo;
    p.line[1].mod.shape = shapeIndex("Saw Up");
    p.line[1].mod.rateBeats = 4.0;
    p.line[1].modLevel = 1.0f;
    const Buf noise = whiteNoise(kSr, 0.4f, 7);
    const Buf quiet = silence(kSr / 2);
    // B on for a second, off for half a second, on again: from here it must be what a fresh B is.
    Wet w;
    const size_t n = noise.size() + 2 * quiet.size() + noise.size();
    Buf in = noise;
    in.insert(in.end(), quiet.begin(), quiet.end());
    in.insert(in.end(), quiet.begin(), quiet.end());
    in.insert(in.end(), noise.begin(), noise.end());
    CHECK(in.size() == n);
    const size_t on1 = noise.size() + quiet.size();
    go(e, p, in, in, in, in, w, 0, noise.size(), tempo());
    P off = p;
    off.line[1].on = false;
    go(e, off, in, in, in, in, w, noise.size(), on1, tempo());
    CHECK(e.running() == 0);
    go(e, p, in, in, in, in, w, on1, n, tempo());
    // The reference: B off all the time before, and the same input from there on.
    Rig f;
    Buf in2 = silence(static_cast<int>(on1));
    in2.insert(in2.end(), in.begin() + static_cast<long>(on1), in.end());
    Wet v;
    go(f, off, in2, in2, in2, in2, v, 0, on1, tempo());
    go(f, p, in2, in2, in2, in2, v, on1, n, tempo());
    // (the modulator restarts too: the saw is at its start again, as in the fresh one)
    if (maxDiff(w.l, v.l, on1) > 1e-6) std::printf("  after coming back on: differs from a fresh line by %g\n", maxDiff(w.l, v.l, on1));
    CHECK(maxDiff(w.l, v.l, on1) < 1e-6 && maxDiff(w.r, v.r, on1) < 1e-6);
}

void echoFades() {       // a line switching on or off fades over 10 ms, and is not run once it has
    Rig e;
    P p;
    onFree(p, 0, 5.0f);
    const Buf in = sine(400.0, kSr, 0.5f);
    Wet w;
    const size_t t0 = 8192;   // a chunk edge
    P q = p;
    q.line[0].on = false;
    go(e, q, in, in, in, in, w, 0, t0, tempo());
    CHECK(e.running() == 0);
    go(e, p, in, in, in, in, w, t0, 20000, tempo());
    CHECK(e.running() == 1);
    // Five milliseconds of nothing (the line's time), then a rise: input and output both fade, so t samples
    // on it is about (t / 441) of the tone times (1 - ...): 0.09 of 0.5 at t = 110; by 10 ms + 5 ms it is all there.
    CHECK(peak(w.l, t0, t0 + 216) < 2e-3f);
    CHECK(peak(w.l, t0 + 220, t0 + 330) < 0.13f);
    CHECK(peak(w.l, t0 + 441 + 220 + 100, t0 + 441 + 220 + 700) > 0.9f * 0.5f);
    // Off: the tone dies away over about 10 ms (and the line is then gone).
    const size_t t1 = 20000 - 20000 % kChunk;
    go(e, q, in, in, in, in, w, 20000, t1 + 4000, tempo());
    CHECK(e.running() == 0);
    CHECK(peak(w.l, t1 + 900, t1 + 4000) < 1e-6f);
    CHECK(maxStep(w.l, 20000, t1 + 4000) < 0.5f * 0.0227f * 2.0f + 0.06f);   // (the tone's own slope is 0.029 a sample)
}

// --- the polyrhythm ------------------------------------------------------------------------------------

void echoPolyrhythm() {  // 3/16 and 5/16 panned -0.7 and +0.7: the left repeat at 3/16, the right at 5/16
    Rig e;
    P p;
    on(p, 0, k316).pan = -0.7f;
    on(p, 1, k516).pan = 0.7f;
    const size_t at = 100, a = at + 19845, b = at + 33075;
    const Wet w = run(e, p, impulseAt(60000, static_cast<int>(at)));
    double gl, gr;
    panGains(-0.7, gl, gr);   // A: loud on the left, a third on the right
    CHECK(near(w.l[a], gl * kH0, 0.02) && near(w.r[a], gr * kH0, 0.02));
    panGains(0.7, gl, gr);    // B the other way round
    CHECK(near(w.l[b], gl * kH0, 0.02) && near(w.r[b], gr * kH0, 0.02));
    CHECK(w.l[a] > 3.0f * w.l[b] && w.r[b] > 3.0f * w.r[a]);
    CHECK(quietExcept(w.l, {a, b}, 2000) < 0.003f && quietExcept(w.r, {a, b}, 2000) < 0.003f);
}

void echoLevelPan() {    // level scales a line, pan moves it by the law of Shape's Pan lane, the send and the wet level scale all
    const int n = 20000;
    const size_t at = 100, a = at + 13230;
    for (double pan : {-1.0, -0.5, 0.0, 0.5, 1.0}) {
        Rig e;
        P p;
        Echo::Line& l = on(p, 0, k8);
        l.pan = static_cast<float>(pan);
        l.level = 0.5f;
        const Wet w = run(e, p, impulseAt(n, at));
        double gl, gr;
        panGains(pan, gl, gr);
        CHECK(near(w.l[a], 0.5 * gl * kH0, 0.01) && near(w.r[a], 0.5 * gr * kH0, 0.01));
        CHECK(near(w.l[a] * w.l[a] + w.r[a] * w.r[a], 0.25 * 2.0 * kH0 * kH0, 0.01));   // constant power
    }
    {
        Rig e;
        P p;
        on(p, 0, k8);
        p.send = 0.5f;
        p.level = 0.5f;
        const Wet w = run(e, p, impulseAt(n, at));
        CHECK(near(w.l[a], 0.25 * kH0, 0.005));
        p.send = 0.0f;
        Rig f;
        CHECK(peak(run(f, p, impulseAt(n, at)).l) < 1e-12f);   // (the 1e-18 under the lines)
    }
}

void echoWidth() {       // width 1 leaves the wet as it is, 0 makes it mono, 1.5 widens the side by half
    const int n = 20000;
    const Buf inL = sine(300.0, n, 0.4f), inR = sine(300.0, n, 0.3f, 1.0);
    P p;
    onFree(p, 0, 20.0f);
    Rig e1, e0, e15;
    const Wet w1 = run(e1, p, inL, inR);
    p.width = 0.0f;
    const Wet w0 = run(e0, p, inL, inR);
    p.width = 1.5f;
    const Wet w15 = run(e15, p, inL, inR);
    CHECK(same(w0.l, w0.r));
    double worst = 0.0, worstM = 0.0;
    for (size_t i = 0; i < w1.l.size(); ++i) {
        const double m1 = 0.5 * (w1.l[i] + w1.r[i]), s1 = 0.5 * (w1.l[i] - w1.r[i]);
        const double m0 = 0.5 * (w0.l[i] + w0.r[i]);
        const double m15 = 0.5 * (w15.l[i] + w15.r[i]), s15 = 0.5 * (w15.l[i] - w15.r[i]);
        worst = std::max(worst, std::fabs(s15 - 1.5 * s1));
        worstM = std::max({worstM, std::fabs(m0 - m1), std::fabs(m15 - m1)});
    }
    CHECK(worst < 1e-5 && worstM < 1e-5);
    CHECK(peak(w1.l, 2000) > 0.2f);
}

// --- bounded ------------------------------------------------------------------------------------------

struct Stats {
    float peak = 0.0f;
    float stored = 0.0f;
    bool finite = true;
};
// The wet of `seconds` of noise, in blocks: the biggest sample, and whether all stayed numbers.
Stats stream(Echo& e, const P& p, double seconds, float amp, ef::Transport t, uint32_t seed = 1) {
    Stats s;
    const int block = 4410;
    const int blocks = static_cast<int>(seconds * kSr / block);
    Wet w;
    for (int b = 0; b < blocks; ++b) {
        const Buf L = whiteNoise(block, amp, seed + static_cast<uint32_t>(2 * b)), R = whiteNoise(block, amp, seed + static_cast<uint32_t>(2 * b + 1));
        w.l.assign(block, 0.0f);
        w.r.assign(block, 0.0f);
        go(e, p, L, R, L, R, w, 0, static_cast<size_t>(block), t);
        s.finite = s.finite && allFinite(w.l) && allFinite(w.r) && std::isfinite(e.peakStored());
        s.peak = std::max({s.peak, peak(w.l), peak(w.r)});
        s.stored = std::max(s.stored, e.peakStored());
    }
    return s;
}

void echoRingBounded() { // Ring, feedback 1 everywhere, drive 1, full-scale noise for 60 s: finite, bounded (four lines at 0 dBFS, the pan law and the width's headroom: 8)
    for (float drive : {1.0f, 0.0f}) {   // (drive 1 holds each line to a third; with none the limiter alone keeps it at 1)
        Rig e;
        P p;
        p.routing = Echo::kRing;
        const double beats[4] = {k16, k8, k316, k4};
        for (int i = 0; i < 4; ++i) {
            Echo::Line& l = on(p, i, beats[i], 1.0f);
            l.p.drive = drive;
        }
        const Stats s = stream(e, p, drive > 0.0f ? 60.0 : 20.0, 1.0f, tempo(120.0));
        if (!s.finite || s.peak > 8.0f) std::printf("  ring, drive %g: peak %g, stored %g, finite %d\n", static_cast<double>(drive), s.peak, s.stored, s.finite);
        CHECK(s.finite && s.peak <= 8.0f && s.stored <= 1.00001f);
        CHECK(s.peak > 0.5f);   // and it is loud: the limiter holds the lines, it does not mute them
    }
}

void echoRingWorst() {   // the worst I can make of it: resonant filters in the loop, wow, the widest pan and width, all three modulators, 30 s
    Rig e;
    P p;
    p.routing = Echo::kRing;
    p.width = 1.5f;
    p.toneOct = 1.0f;
    p.feedbackOffset = 0.5f;
    const double beats[4] = {k16, k8, k316, k4};
    const int types[4] = {gf::EchoLine::kLP, gf::EchoLine::kBP, gf::EchoLine::kHP, gf::EchoLine::kBP};
    for (int i = 0; i < 4; ++i) {
        Echo::Line& l = on(p, i, beats[i], 1.0f);
        l.p.drive = i % 2 ? 1.0f : 0.5f;
        l.p.wow = 1.0f;
        l.p.filterType = types[i];
        l.p.cutoffHz = 300.0f * static_cast<float>(i + 1);
        l.p.resonance = 1.0f;
        l.p.place = gf::EchoLine::kInLoop;
        l.pan = i % 2 ? 1.0f : -1.0f;
        l.modCutoff = 1.0f;
        l.modTime = 1.0f;
        l.modLevel = 0.5f;
        l.mod.mode = i == 0 ? gf::EchoMod::kLfo : (i == 1 ? gf::EchoMod::kEnv : (i == 2 ? gf::EchoMod::kFollow : gf::EchoMod::kLfo));
        l.mod.shape = shapeIndex(i == 3 ? "Square" : "Sine");
        l.mod.rateBeats = 0.5;
    }
    const Stats s = stream(e, p, 30.0, 1.0f, playingAt(120.0, 0.0), 77);
    if (!s.finite || s.peak > 100.0f) std::printf("  worst ring: peak %g, stored %g, finite %d\n", s.peak, s.stored, s.finite);
    CHECK(s.finite && s.stored <= 1.00001f && s.peak <= 100.0f);
}

// --- the duck, the freeze, the offsets ------------------------------------------------------------------

void echoDuck() {        // the wet ducks under the key (EffectForce's law: 1 / (1 + 16 duck env)), not under its own input
    const int n = 5 * kSr / 2;
    const Buf tone = sine(1000.0, n, 0.2f);
    // The key: silent, then 0.2 s of loud noise at 0.5 s.
    Buf key = silence(n);
    {
        const Buf burst = whiteNoise(kSr / 5, 0.5f, 9);
        std::copy(burst.begin(), burst.end(), key.begin() + kSr / 2);
    }
    Wet w[3];
    const float amounts[3] = {0.0f, 0.5f, 1.0f};
    for (int k = 0; k < 3; ++k) {
        Rig e;
        P p;
        onFree(p, 0, 20.0f);
        p.duck = amounts[k];
        go(e, p, tone, tone, key, key, w[k], 0, static_cast<size_t>(n), tempo());
    }
    const size_t from = kSr / 2 + 2205, to = kSr / 2 + 8000;   // inside the burst, after the attack
    const double r0 = rms(w[0].l, from, to), r5 = rms(w[1].l, from, to), r1 = rms(w[2].l, from, to);
    if (!(r1 < 0.2 * r0)) std::printf("  duck: %.1f dB at 1, %.1f dB at 0.5\n", db(r1 / r0), db(r5 / r0));
    CHECK(r1 < 0.2 * r0 && r5 < 0.4 * r0 && r5 > r1 * 1.2);
    // Before the key: untouched, bit for bit. Long after it: back.
    CHECK(maxDiff(w[0].l, w[2].l, 0, kSr / 2 - 100) == 0.0);
    CHECK(near(db(rms(w[2].l, n - 5000, n) / rms(w[0].l, n - 5000, n)), 0.0, 0.5));

    // The law, on a steady key: 0.25 gives 1 / (1 + 16 0.25) = 0.2 at duck 1, 1/3 at duck 0.5.
    const Buf dc(static_cast<size_t>(n), 0.25f);
    for (int k = 1; k < 3; ++k) {
        Rig e;
        P p;
        onFree(p, 0, 20.0f);
        p.duck = amounts[k];
        Wet v;
        go(e, p, tone, tone, dc, dc, v, 0, static_cast<size_t>(n), tempo());
        const double want = 1.0 / (1.0 + 16.0 * amounts[k] * 0.25);
        const double got = rms(v.l, n - 8000, n) / rms(w[0].l, n - 8000, n);
        if (!near(got, want, 0.005)) std::printf("  duck %g on a key of 0.25: gain %g, wanted %g\n", amounts[k], got, want);
        CHECK(near(got, want, 0.005));
    }
    // Its times: 5 ms attack and 250 ms release. A key that steps from 0 to 0.25 and back, the gain a millisecond at a time.
    {
        const size_t on = 13230, off = 44100;
        Buf step = silence(n);
        std::fill(step.begin() + static_cast<long>(on), step.begin() + static_cast<long>(off), 0.25f);
        Rig e;
        P p;
        onFree(p, 0, 20.0f);
        p.duck = 1.0f;
        Wet v;
        go(e, p, tone, tone, step, step, v, 0, static_cast<size_t>(n), tempo());
        auto gain = [&](size_t at) { return rms(v.l, at, at + 44) / rms(w[0].l, at, at + 44); };
        // down: 1 - e^(-t / 5 ms) of 0.25 at the window's middle; up: e^(-t / 250 ms) of what it had reached
        auto gainAt = [](double env) { return 1.0 / (1.0 + 16.0 * env); };
        for (double ms : {2.0, 5.0, 10.0, 20.0}) {
            const size_t at = on + static_cast<size_t>(ms * 44.1) - 22;
            const double want = gainAt(0.25 * (1.0 - std::exp(-ms / 5.0)));
            if (!near(gain(at), want, 0.04)) std::printf("  duck attack at %g ms: %g, wanted %g\n", ms, gain(at), want);
            CHECK(near(gain(at), want, 0.04));
        }
        for (double ms : {50.0, 250.0, 500.0}) {
            const size_t at = off + static_cast<size_t>(ms * 44.1) - 22;
            const double want = gainAt(0.25 * std::exp(-ms / 250.0));
            if (!near(gain(at), want, 0.03)) std::printf("  duck release at %g ms: %g, wanted %g\n", ms, gain(at), want);
            CHECK(near(gain(at), want, 0.03));
        }
    }
    // Its own input does not duck it: loud input, a silent key.
    {
        const Buf loud = whiteNoise(n, 0.5f, 10), none = silence(n);
        Rig e, f;
        P p;
        onFree(p, 0, 20.0f);
        Wet a, b;
        go(e, p, loud, loud, none, none, a, 0, static_cast<size_t>(n), tempo());
        p.duck = 1.0f;
        go(f, p, loud, loud, none, none, b, 0, static_cast<size_t>(n), tempo());
        CHECK(same(a.l, b.l) && same(a.r, b.r));
    }
    // The lines are not ducked, only the heard wet: a repeat that sounds under the key is ducked, and goes on
    // round at its full size (the next one is not ducked).
    {
        const int T = 66150;   // 1.5 s
        Buf hit = silence(3 * T);
        {
            const Buf b = sine(1000.0, 882, 0.5f);
            for (size_t i = 0; i < b.size(); ++i) hit[1000 + i] = b[i] * static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / 882.0));
        }
        Buf k2 = silence(3 * T);
        for (int i = 0; i < 2200; ++i) k2[static_cast<size_t>(T + 700 + i)] = 0.5f;   // loud just before the first repeat and through it
        Wet v[2];
        for (int k = 0; k < 2; ++k) {
            Rig e;
            P p;
            onFree(p, 0, 1500.0f, 0.5f);
            p.duck = static_cast<float>(k);
            go(e, p, hit, hit, k2, k2, v[k], 0, hit.size(), tempo());
        }
        auto rep = [&](int k, int r) { return magnitude(v[k].l, 1000.0, static_cast<size_t>(1000 + r * T - 20), static_cast<size_t>(1000 + r * T + 902)); };
        CHECK(rep(1, 1) < 0.3 * rep(0, 1));
        CHECK(rep(1, 2) > 0.97 * rep(0, 2) && rep(0, 2) > 0.0);
    }
}

void echoFreeze() {      // frozen: the lines hold, the send is closed; both glide over 20 ms
    const int T = 13230;   // 1/8 at 100 BPM
    const size_t F = 16000 - 16000 % kChunk;
    auto make = [&](bool lateBurst) {
        Buf x = silence(8 * T);
        const Buf b = sine(1000.0, 882, 0.5f);
        for (size_t i = 0; i < b.size(); ++i) x[1000 + i] = b[i] * static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / 882.0));
        if (lateBurst)
            for (size_t i = 0; i < b.size(); ++i) x[static_cast<size_t>(5 * T) + i] = b[i] * static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / 882.0));
        return x;
    };
    P p;
    on(p, 0, k8, 0.5f);
    P fr = p;
    fr.freeze = true;
    auto trial = [&](bool freeze, bool lateBurst) {
        Rig e;
        const Buf x = make(lateBurst);
        Wet w;
        go(e, p, x, x, x, x, w, 0, F, tempo());
        go(e, freeze ? fr : p, x, x, x, x, w, F, x.size(), tempo());
        return w;
    };
    const Wet held = trial(true, false), free = trial(false, false), heldLate = trial(true, true);
    // The repeats: 1 at T, 2 at 2T, ...; the burst was written before the freeze, so the repeats from 3 on are held at repeat 2's size.
    auto rep = [&](const Wet& w, int k) { return magnitude(w.l, 1000.0, static_cast<size_t>(1000 + k * T - 20), static_cast<size_t>(1000 + k * T + 902)); };
    CHECK(near(db(rep(free, 3) / rep(free, 2)), -6.0, 0.5));                                      // free: each repeat half the last
    for (int k = 3; k <= 7; ++k) {
        if (!near(db(rep(held, k) / rep(held, 2)), 0.0, 0.3)) std::printf("  frozen repeat %d: %.2f dB against repeat 2\n", k, db(rep(held, k) / rep(held, 2)));
        CHECK(near(db(rep(held, k) / rep(held, 2)), 0.0, 0.3));
    }
    // The send is closed: a burst after the freeze does not get in.
    CHECK(maxDiff(held.l, heldLate.l) == 0.0 && maxDiff(held.r, heldLate.r) == 0.0);

    // The glides, seen through impulses into a silent line: the send's gain at the freeze (a straight line to 0 over 20 ms),
    // and the feedback that follows it to 1 (so what came in at the start of the freeze is held).
    {
        Rig e;
        P q;
        onFree(q, 0, 100.0f, 0.0f);
        P qf = q;
        qf.freeze = true;
        const size_t G = 8192;   // a chunk edge
        Buf x = silence(40000);
        const size_t hits[3] = {G, G + 441, G + 946};
        for (size_t h : hits) x[h] = 0.5f;
        Wet w;
        go(e, q, x, x, x, x, w, 0, G, tempo());
        go(e, qf, x, x, x, x, w, G, x.size(), tempo());
        // 441 samples: half-way (10 ms of the 20); 946: closed. Each comes back 4410 later.
        CHECK(near(w.l[G + 4410] / 0.5, kH0 * (1.0 - 1.0 / 882.0), 0.02));
        CHECK(near(w.l[G + 441 + 4410] / 0.5, kH0 * 0.5, 0.03));
        CHECK(std::fabs(w.l[G + 946 + 4410]) < 1e-3f);
    }
    // Unfreezing opens the send again ...
    {
        Rig e;
        const Buf x = make(true);
        P q = fr;
        Wet w;
        go(e, q, x, x, x, x, w, 0, static_cast<size_t>(4 * T), tempo());
        q.freeze = false;
        go(e, q, x, x, x, x, w, static_cast<size_t>(4 * T) - static_cast<size_t>(4 * T) % kChunk, x.size(), tempo());
        const double late = magnitude(w.l, 1000.0, static_cast<size_t>(5 * T) + T - 20, static_cast<size_t>(5 * T) + T + 902);
        CHECK(late > 0.01);   // the burst at 5T, now let in, is heard one repeat later
    }
    // ... and lets what the lines hold decay again at their feedback: held to 4T, then half a repeat each time.
    {
        Rig e;
        const Buf x = make(false);
        Wet w;
        go(e, p, x, x, x, x, w, 0, F, tempo());
        go(e, fr, x, x, x, x, w, F, static_cast<size_t>(4 * T), tempo());
        go(e, p, x, x, x, x, w, static_cast<size_t>(4 * T), x.size(), tempo());
        const double held = rep(w, 4);
        CHECK(near(db(rep(w, 3) / held), 0.0, 0.3));               // frozen: repeats 3 and 4 the same
        CHECK(near(db(rep(w, 5) / held), -6.0, 0.6) && near(db(rep(w, 6) / rep(w, 5)), -6.0, 0.6) && near(db(rep(w, 7) / rep(w, 6)), -6.0, 0.6));
    }
}

void echoTone() {        // toneOct darkens (or brightens) the cutoff of every line
    const int n = kSr / 2;
    for (int i = 0; i < 4; ++i) {
        const Buf hi = sine(10000.0, n, 0.5f), lo = sine(200.0, n, 0.5f);
        P p;
        onFree(p, i, 5.0f);
        Rig a, b, c, d;
        const Wet flatHi = run(a, p, hi);
        const Wet flatLo = run(b, p, lo);
        p.toneOct = -2.0f;
        const Wet darkHi = run(c, p, hi);
        const Wet darkLo = run(d, p, lo);
        const double dh = gainDb(darkHi.l, hi, 10000.0, 10000, n) - gainDb(flatHi.l, hi, 10000.0, 10000, n);
        const double dl = gainDb(darkLo.l, lo, 200.0, 10000, n) - gainDb(flatLo.l, lo, 200.0, 10000, n);
        if (dh > -10.0 || std::fabs(dl) > 0.3) std::printf("  line %d: 10 kHz %.2f dB, 200 Hz %.2f dB\n", i, dh, dl);
        CHECK(dh < -10.0 && std::fabs(dl) < 0.3);   // a 5 kHz low-pass: 12 dB down at 10 kHz
    }
    {   // The offset is held to ±4 octaves.
        const Buf noise = whiteNoise(kSr / 2, 0.5f, 31);
        P p;
        onFree(p, 0, 5.0f);
        p.toneOct = 100.0f;
        Rig a, b;
        const Wet wild = run(a, p, noise);
        p.toneOct = 4.0f;
        const Wet four = run(b, p, noise);
        CHECK(same(wild.l, four.l));
        p.toneOct = -100.0f;
        Rig c, d;
        const Wet wildLow = run(c, p, noise);
        p.toneOct = -4.0f;
        const Wet minusFour = run(d, p, noise);
        CHECK(same(wildLow.l, minusFour.l));
    }
    {   // And up: a line at 2 kHz opens to 8 kHz with +2.
        const Buf mid = sine(5000.0, n, 0.5f);
        P p;
        onFree(p, 0, 5.0f).p.cutoffHz = 2000.0f;
        Rig a, b;
        const Wet shut = run(a, p, mid);
        p.toneOct = 2.0f;
        const Wet open = run(b, p, mid);
        const double g0 = gainDb(shut.l, mid, 5000.0, 10000, n), g2 = gainDb(open.l, mid, 5000.0, 10000, n);
        CHECK(g0 < -10.0 && g2 > -1.5);
    }
}

void echoFeedbackOffset() {  // added to every line's feedback, then held to 0..1
    const size_t at = 100, T = 4410;
    auto ratio = [&](int line, float fb, float offset) {
        Rig e;
        P p;
        onFree(p, line, 100.0f, fb);
        p.feedbackOffset = offset;
        const Wet w = run(e, p, impulseAt(5 * static_cast<int>(T), static_cast<int>(at)));
        return std::make_pair(std::fabs(w.l[at + 2 * T]) / std::fabs(w.l[at + T]), std::fabs(w.l[at + 3 * T]) / std::fabs(w.l[at + T]));
    };
    for (int line = 0; line < 4; ++line) {
        CHECK(near(ratio(line, 0.4f, 0.0f).first, 0.4 * kH0, 0.01));
        CHECK(near(ratio(line, 0.4f, 0.3f).first, 0.7 * kH0, 0.01));
        CHECK(ratio(line, 0.4f, -0.4f).first < 1e-3);
        CHECK(ratio(line, 0.4f, -1.0f).first < 1e-3);
        CHECK(near(ratio(line, 0.8f, 0.5f).first, kH0, 0.005) && near(ratio(line, 0.8f, 0.5f).second, kH0 * kH0, 0.01));   // up to 1 and no further
    }
}

// --- the modulators ----------------------------------------------------------------------------------

void echoModEnv() {      // Env on the cutoff: after each hit the line opens, so the repeat is bright; a steady tone never opens it
    const size_t T = 6615;   // 1/16 at 100 BPM
    const int n = 10 * kSr;
    Buf hits = silence(n);
    const int at[3] = {13230, 3 * kSr + 13230, 6 * kSr + 13230};
    for (int a : at) addBlip(hits, a, 0.5f);
    P p;
    Echo::Line& l = on(p, 0, k16);
    l.p.filterType = gf::EchoLine::kLP;
    l.p.cutoffHz = 800.0f;
    l.mod.mode = gf::EchoMod::kEnv;
    l.mod.rateBeats = 4.0;
    P flat = p;
    l.modCutoff = 1.0f;
    Rig e, f;
    const Wet open = run(e, p, hits), shut = run(f, flat, hits);
    for (int a : at) {
        const double bright = magnitude(open.l, 3000.0, a + T - 50, a + T + 1500), dark = magnitude(shut.l, 3000.0, a + T - 50, a + T + 1500);
        if (!(bright > 6.0 * dark)) std::printf("  hit at %d: repeat %g against %g at 3 kHz\n", a, bright, dark);
        CHECK(bright > 6.0 * dark && dark > 0.0);
    }
    // A tone that was already going when the line came on is no hit: the repeat is as dark as without the modulator.
    const Buf tone = sine(3000.0, 3 * kSr, 0.5f);
    Rig g, h;
    const Wet a = run(g, p, tone), b = run(h, flat, tone);
    CHECK(near(db(rms(a.l, 2 * kSr, 3 * kSr) / rms(b.l, 2 * kSr, 3 * kSr)), 0.0, 0.5));
    // The Env listens to the key (the chain's input), not to what the Echo is sent: a steady tone in, the hits only in the key.
    {
        const Buf quiet = silence(n);
        Wet viaKey, viaInput;
        Rig k, m;
        go(k, p, tone, tone, hits, hits, viaKey, 0, 3 * kSr, tempo());
        go(m, p, hits, hits, quiet, quiet, viaInput, 0, 3 * kSr, tempo());
        // (the tone's third second has no hit in it; its first hit is at 13230)
        const double opened = magnitude(viaKey.l, 3000.0, 13230 + T, 13230 + T + 4000), dark = magnitude(b.l, 3000.0, 2 * kSr, 3 * kSr - 1);
        CHECK(opened > 4.0 * dark && dark > 0.0);
        // and the hits as the input with a silent key open nothing: the repeat is as dark as with no modulator
        CHECK(magnitude(viaInput.l, 3000.0, 13230 + T - 50, 13230 + T + 1500) < 2.0 * magnitude(shut.l, 3000.0, 13230 + T - 50, 13230 + T + 1500));
    }
    // Another line's Env listens to the same key and is on its own cutoff: each line has its own modulator.
    P two = p;
    Echo::Line& l2 = on(two, 1, k8);
    l2 = two.line[1];
    l2.p.filterType = gf::EchoLine::kLP;
    l2.p.cutoffHz = 800.0f;
    l2.mod.mode = gf::EchoMod::kEnv;
    l2.mod.rateBeats = 4.0;
    l2.modCutoff = 0.0f;   // B is not modulated
    Rig m;
    const Wet both = run(m, two, hits);
    CHECK(magnitude(both.l, 3000.0, at[1] + T - 50, at[1] + T + 1500) > 6.0 * magnitude(shut.l, 3000.0, at[1] + T - 50, at[1] + T + 1500));
}

void echoModLevel() {    // modLevel: the level dips to 1 - modLevel at the modulator's 0
    const size_t T = 6615;
    const int n = 3 * kSr;
    Buf hits = silence(n);
    addBlip(hits, 13230, 0.5f);
    P p;
    Echo::Line& l = on(p, 0, k16);
    l.mod.mode = gf::EchoMod::kEnv;
    l.mod.rateBeats = 4.0;
    P flat = p;
    l.modLevel = 1.0f;
    Rig e, f;
    const Wet gated = run(e, p, hits), plain = run(f, flat, hits);
    // The repeat of the hit comes while the envelope is still high (0.65 a sixteenth later at a 4-beat fall).
    const double r = rms(gated.l, 13230 + T, 13230 + T + 900) / rms(plain.l, 13230 + T, 13230 + T + 900);
    CHECK(r > 0.5 && r < 0.8);
    // A tone with no hit in it never opens the gate; without the modulator it is heard.
    const Buf tone = sine(1000.0, n, 0.4f);
    Rig g, h;
    const Wet a = run(g, p, tone), b = run(h, flat, tone);
    CHECK(rms(a.l, 2 * kSr, 3 * kSr) < 0.01 * rms(b.l, 2 * kSr, 3 * kSr));
    // Half the amount: the level dips to a half.
    p.line[0].modLevel = 0.5f;
    Rig i;
    const Wet half = run(i, p, tone);
    CHECK(near(rms(half.l, 2 * kSr, 3 * kSr) / rms(b.l, 2 * kSr, 3 * kSr), 0.5, 0.02));
}

void echoModShared() {   // R41: the one follower and the one detector, heard through the lines
    // Follow on the level: a 1 kHz tone at 0.05 and then at 0.5 under the key; the line's level follows it (2.14 times the level,
    // cut at 1), so the wet is far more than the 10 to 1 of the tone.
    {
        P p;
        Echo::Line& l = onFree(p, 0, 5.0f);
        l.mod.mode = gf::EchoMod::kFollow;
        l.modLevel = 1.0f;
        Buf tone = sine(1000.0, 3 * kSr, 0.5f);
        for (size_t i = 0; i < 3 * kSr / 2; ++i) tone[i] *= 0.1f;
        Rig e;
        const Wet w = run(e, p, tone);
        const double quiet = rms(w.l, kSr, 3 * kSr / 2), loud = rms(w.l, 5 * kSr / 2, 3 * kSr);
        if (!(loud > 50.0 * quiet)) std::printf("  follow: %g loud against %g quiet\n", loud, quiet);
        CHECK(loud > 50.0 * quiet && quiet > 0.0);
        CHECK(near(loud, 0.5 * 0.7071, 0.03));   // the follower reads 1 for a 0.5 tone (2.14 times its level, cut at 1)
    }
    // The detector's sensitivity is the Echo's: a tone that steps up 1.6 times is a hit only at the keenest setting.
    {
        Buf tone = sine(1000.0, 3 * kSr, 0.2f);
        for (size_t i = kSr; i < tone.size(); ++i) tone[i] *= 1.6f;
        double heard[2];
        for (int k = 0; k < 2; ++k) {
            P p;
            Echo::Line& l = onFree(p, 0, 5.0f);
            l.mod.mode = gf::EchoMod::kEnv;
            l.mod.rateBeats = 4.0;
            l.modLevel = 1.0f;
            p.sensitivity = static_cast<float>(k);
            Rig e;
            const Wet w = run(e, p, tone);
            heard[k] = rms(w.l, kSr + 500, kSr + 4000);
        }
        if (!(heard[1] > 20.0 * heard[0])) std::printf("  sensitivity: %g at 1, %g at 0\n", heard[1], heard[0]);
        CHECK(heard[1] > 20.0 * heard[0] && heard[1] > 0.05);
    }
}

void echoModTime() {     // modTime: up to 2% of the time at the modulator's 1, glided
    const size_t T = 6615;
    const int n = 3 * kSr;
    Buf hits = silence(n);
    addBlip(hits, 13230, 0.5f);
    P p;
    Echo::Line& l = on(p, 0, k16);
    l.mod.mode = gf::EchoMod::kEnv;
    l.mod.rateBeats = 4.0;
    P flat = p;
    l.modTime = 1.0f;
    Rig e, f;
    const Wet moved = run(e, p, hits), still = run(f, flat, hits);
    const double d = centroid(moved.l, 13230 + T - 200, 13230 + T + 900) - centroid(still.l, 13230 + T - 200, 13230 + T + 900);
    if (!(d > 20.0 && d < 140.0)) std::printf("  modTime: the repeat is %g samples later\n", d);
    CHECK(d > 20.0 && d < 140.0);   // 2% of 6615 is 132, and the envelope has fallen to 0.65 by then
    p.line[0].modTime = -1.0f;
    Rig g;
    const Wet earlier = run(g, p, hits);
    CHECK(centroid(earlier.l, 13230 + T - 400, 13230 + T + 900) < centroid(still.l, 13230 + T - 400, 13230 + T + 900) - 20.0);
}

void echoModSlewed() {   // a modulator's step reaches the heard level no faster than a full scale in 1 ms
    P p;
    Echo::Line& l = onFree(p, 0, 2.0f);
    l.mod.mode = gf::EchoMod::kLfo;
    l.mod.shape = shapeIndex("Square");
    l.mod.rateBeats = 1.0;
    l.modLevel = 1.0f;
    const Buf dc(kSr, 0.5f);
    Rig e;
    const Wet w = run(e, p, dc, playingAt(100.0, 0.0));
    float worst = 0.0f;
    for (size_t i = 6000; i + 1 < w.l.size(); ++i) worst = std::max(worst, std::fabs(w.l[i + 1] - w.l[i]));
    // A DC input goes through the 30 Hz low cut, so the wet is the edges alone: 0.5 a full scale in 1 ms is 0.0113 a sample.
    if (worst > 0.5f / 44.1f * 1.05f) std::printf("  square on the level: steepest %g\n", worst);
    CHECK(worst <= 0.5f / 44.1f * 1.05f);
}

// --- allocation, chunks, the extremes ---------------------------------------------------------------------

void echoAllocate() {    // before allocate(): silent, nothing read; after: the lines come in
    Echo e;
    CHECK(!e.allocated());
    P p;
    on(p, 0, k8);
    on(p, 1, k316, 0.5f);
    const Buf in = whiteNoise(kSr / 2, 0.5f, 11);
    Wet w;
    go(e, p, in, in, in, in, w, 0, in.size(), tempo());
    CHECK(peak(w.l) == 0.0f && peak(w.r) == 0.0f);
    CHECK(e.running() == 0 && e.tailSamples() == 0 && e.peakStored() == 0.0f);
    CHECK(e.allocate() && e.allocated());
    // The next chunk takes them up: they fade in (no step), and then it is the Echo.
    const Buf tone = sine(400.0, kSr, 0.5f);
    go(e, p, tone, tone, tone, tone, w, 0, tone.size(), tempo());
    CHECK(e.running() == 2);
    CHECK(peak(w.l, 20000, 40000) > 0.3f);
    CHECK(maxStep(w.l, 0, 20000) < 0.1f);
    // A second allocate() leaves it alone (a repeat in flight survives it).
    Echo f;
    CHECK(f.allocate());
    P q;
    on(q, 0, k8);
    const Buf hit = impulseAt(30000, 100);
    Wet v;
    go(f, q, hit, hit, hit, hit, v, 0, 10000, tempo());
    CHECK(f.allocate());
    go(f, q, hit, hit, hit, hit, v, 10000, 30000, tempo());
    CHECK(near(v.l[100 + 13230], kH0, 0.01));
    CHECK(sizeof(Echo) < 16384);
}

void echoChunks() {      // static parameters: the chunk length makes no difference
    P p;
    p.routing = Echo::kRing;
    p.width = 1.2f;
    p.send = 0.8f;
    p.level = 0.9f;
    p.toneOct = -1.0f;
    const double beats[4] = {k16, k8, k316, k4};
    for (int i = 0; i < 4; ++i) {
        Echo::Line& l = on(p, i, beats[i], 0.6f);
        l.pan = 0.5f * static_cast<float>(i) - 0.7f;
        l.level = 0.6f;
        l.p.filterType = gf::EchoLine::kBP;
        l.p.cutoffHz = 1200.0f;
        l.p.resonance = 0.3f;
        l.p.drive = 0.2f;
    }
    const Buf L = whiteNoise(2 * kSr, 0.3f, 12), R = whiteNoise(2 * kSr, 0.3f, 13);
    Rig a, b, c;
    const Wet w32 = run(a, p, L, R, tempo(), 32);
    const Wet w20 = run(b, p, L, R, tempo(), 20);
    const Wet w7 = run(c, p, L, R, tempo(), 7);
    if (maxDiff(w32.l, w20.l) > 1e-4 || maxDiff(w32.l, w7.l) > 1e-4) std::printf("  chunks: %g, %g\n", maxDiff(w32.l, w20.l), maxDiff(w32.l, w7.l));
    CHECK(maxDiff(w32.l, w20.l) < 1e-4 && maxDiff(w32.r, w20.r) < 1e-4 && maxDiff(w32.l, w7.l) < 1e-4 && maxDiff(w32.r, w7.r) < 1e-4);
}

void echoAliasing() {    // the wet may be one of the inputs
    P p;
    p.routing = Echo::kSerial;
    on(p, 0, k16, 0.4f);
    on(p, 1, k8, 0.4f);
    p.duck = 0.5f;
    const Buf L = whiteNoise(kSr, 0.4f, 14), R = whiteNoise(kSr, 0.4f, 15);
    Rig a, b;
    const Wet ref = run(a, p, L, R);
    Buf iL = L, iR = R;
    for (size_t pos = 0; pos < L.size(); pos += kChunk) {
        const int m = static_cast<int>(std::min(static_cast<size_t>(kChunk), L.size() - pos));
        b.set(p, tempo());
        b.process(&iL[pos], &iR[pos], &iL[pos], &iR[pos], &iL[pos], &iR[pos], m);
    }
    CHECK(same(ref.l, iL) && same(ref.r, iR));
}

void echoNan() {         // NaN and infinity in the input are silence; in the key too; the loop stays clean
    P p;
    p.routing = Echo::kRing;
    p.duck = 0.5f;
    const double beats[4] = {k16, k8, k316, k4};
    for (int i = 0; i < 4; ++i) {
        Echo::Line& l = on(p, i, beats[i], 0.9f);
        l.mod.mode = i % 2 ? gf::EchoMod::kEnv : gf::EchoMod::kFollow;
        l.modLevel = 0.5f;
        l.modCutoff = 0.5f;
    }
    Buf L = whiteNoise(3 * kSr, 0.4f, 16), R = whiteNoise(3 * kSr, 0.4f, 17);
    Buf cL = L, cR = R;   // the same with the bad samples made silent
    const float bad[] = {std::nanf(""), INFINITY, -INFINITY, std::nanf("")};
    for (int k = 0; k < 40; ++k) {
        const size_t i = 80000 + 700 * static_cast<size_t>(k);   // (once the repeats are going round the ring)
        L[i] = bad[k % 4];
        cL[i] = 0.0f;
        R[i + 5] = bad[(k + 1) % 4];
        cR[i + 5] = 0.0f;
    }
    Rig a, b;
    Wet w, v;
    go(a, p, L, R, L, R, w, 0, L.size(), tempo());
    go(b, p, cL, cR, cL, cR, v, 0, L.size(), tempo());
    CHECK(allFinite(w.l) && allFinite(w.r) && std::isfinite(a.peakStored()));
    CHECK(same(w.l, v.l) && same(w.r, v.r));
    // Huge but finite: bounded.
    Buf hL(kSr, 3e38f), hR(kSr, -3e38f);
    Rig c;
    Wet h;
    go(c, p, hL, hR, hL, hR, h, 0, hL.size(), tempo());
    CHECK(allFinite(h.l) && allFinite(h.r) && c.peakStored() <= 1.00001f && peak(h.l) <= 100.0f);
}

void echoExtremes() {    // any parameters, any chunk length: finite, bounded, the lines held under 0 dBFS
    uint32_t seed = 12345;
    auto rnd = [&]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<double>(seed >> 8) / 16777216.0;
    };
    auto wild = [&](double lo, double hi) {
        const double r = rnd();
        if (r < 0.05) return static_cast<double>(std::nanf(""));
        if (r < 0.08) return static_cast<double>(INFINITY);
        if (r < 0.10) return -static_cast<double>(INFINITY);
        if (r < 0.2) return lo - 5.0;
        if (r < 0.3) return hi + 5.0;
        return lo + (hi - lo) * rnd();
    };
    for (int trial = 0; trial < 40; ++trial) {
        Rig e;
        P p;
        p.routing = static_cast<int>(wild(-2.0, 7.0));
        p.send = static_cast<float>(wild(0.0, 1.0));
        p.duck = static_cast<float>(wild(0.0, 1.0));
        p.freeze = rnd() < 0.3;
        p.glide = static_cast<int>(wild(-1.0, 2.0));
        p.width = static_cast<float>(wild(0.0, 1.5));
        p.level = static_cast<float>(wild(0.0, 1.0));
        p.feedbackOffset = static_cast<float>(wild(-1.0, 1.0));
        p.toneOct = static_cast<float>(wild(-4.0, 4.0));
        p.sensitivity = static_cast<float>(wild(0.0, 1.0));
        p.swing.amount = static_cast<float>(wild(0.5, 0.75));
        for (Echo::Line& l : p.line) {
            l.on = rnd() < 0.8;
            l.level = static_cast<float>(wild(0.0, 1.0));
            l.pan = static_cast<float>(wild(-1.0, 1.0));
            l.p.sync = rnd() < 0.5;
            l.p.divBeats = wild(0.05, 5.0);
            l.p.timeMs = static_cast<float>(wild(1.0, 2000.0));
            l.p.feedback = static_cast<float>(wild(0.0, 1.0));
            l.p.mode = static_cast<int>(wild(-1.0, 4.0));
            l.p.filterType = static_cast<int>(wild(-1.0, 4.0));
            l.p.cutoffHz = static_cast<float>(wild(20.0, 20000.0));
            l.p.resonance = static_cast<float>(wild(0.0, 1.0));
            l.p.place = static_cast<int>(wild(-1.0, 3.0));
            l.p.drive = static_cast<float>(wild(0.0, 1.0));
            l.p.wow = static_cast<float>(wild(0.0, 1.0));
            l.modCutoff = static_cast<float>(wild(-1.0, 1.0));
            l.modTime = static_cast<float>(wild(-1.0, 1.0));
            l.modLevel = static_cast<float>(wild(0.0, 1.0));
            l.mod.mode = static_cast<int>(wild(-1.0, 4.0));
            l.mod.shape = static_cast<int>(wild(-3.0, 60.0));
            l.mod.rateBeats = wild(0.01, 100.0);
        }
        const Buf L = whiteNoise(kSr, 0.8f, 100 + static_cast<uint32_t>(trial)), R = whiteNoise(kSr, 0.8f, 200 + static_cast<uint32_t>(trial));
        Wet w;
        w.l.assign(L.size(), 0.0f);
        w.r.assign(L.size(), 0.0f);
        ef::Transport t = rnd() < 0.5 ? tempo(wild(10.0, 400.0)) : playingAt(wild(10.0, 400.0), wild(0.0, 1000.0));
        size_t pos = 0;
        while (pos < L.size()) {
            const int n = 1 + static_cast<int>(rnd() * (kChunk - 1) + 0.5);
            const size_t m = std::min(static_cast<size_t>(n), L.size() - pos);
            e.set(p, t);
            e.process(&L[pos], &R[pos], &L[pos], &R[pos], &w.l[pos], &w.r[pos], static_cast<int>(m));
            if (t.playing) t.beats += static_cast<double>(m) / kSr * t.bpm / 60.0;
            pos += m;
        }
        const bool ok = allFinite(w.l) && allFinite(w.r) && std::isfinite(e.peakStored()) && e.peakStored() <= 1.0001f && peak(w.l) <= 200.0f &&
                        peak(w.r) <= 200.0f && e.tailSamples() >= 0;
        if (!ok) std::printf("  trial %d: peak %g %g, stored %g\n", trial, peak(w.l), peak(w.r), e.peakStored());
        CHECK(ok);
    }
}

void echoTail() {        // how long it rings: the lines' tails along the routing, forever if frozen or ringing round
    const double bpm = 100.0;
    auto tail = [&](const P& p) {
        Rig e;
        const Buf in = silence(kChunk);
        Wet w;
        go(e, p, in, in, in, in, w, 0, kChunk, tempo(bpm));
        return e.tailSamples();
    };
    P p;
    CHECK(tail(p) == 0);
    on(p, 0, k8, 0.5f);
    const int one = tail(p);
    CHECK(one >= 13230 && one < 5 * kSr);
    on(p, 1, k8, 0.5f);
    CHECK(tail(p) == one);   // in parallel: the longer
    p.routing = Echo::kSerial;
    CHECK(tail(p) >= 2 * one - 100);   // in a row: the sum
    p.routing = Echo::kRing;
    const int two = tail(p);
    CHECK(two < (1 << 29) && two >= 2 * 13230);   // a ring of the two lines that are on, 0.25 a trip: it dies
    p.line[0].p.feedback = p.line[1].p.feedback = 1.0f;
    CHECK(tail(p) >= (1 << 29));       // at 1 on both of them it never does (R46)
    p.line[0].p.feedback = p.line[1].p.feedback = 0.5f;
    for (int i = 2; i < 4; ++i) on(p, i, k8, 0.5f);
    const int round = tail(p);
    CHECK(round < (1 << 29) && round >= 3 * 4 * 13230);   // all four round at 0.5 each: it dies, in 3 trips to -60 dB
    for (Echo::Line& l : p.line) l.p.feedback = 1.0f;
    CHECK(tail(p) >= (1 << 29));       // at 1 on all four it never does (R39)
    p.line[2].p.feedback = 0.0f;
    CHECK(tail(p) < (1 << 29));        // a 0 anywhere opens the ring
    p.routing = Echo::kParallel;
    p.freeze = true;
    CHECK(tail(p) >= (1 << 29));
    // Frozen from the chunk it is switched on (the feedback is still on its way to 1, the tail already endless).
    {
        Rig e;
        P q;
        on(q, 0, k8, 0.5f);
        const Buf in = silence(2 * kChunk);
        Wet w;
        go(e, q, in, in, in, in, w, 0, kChunk, tempo(bpm));
        CHECK(e.tailSamples() < (1 << 29));
        q.freeze = true;
        go(e, q, in, in, in, in, w, kChunk, 2 * kChunk, tempo(bpm));
        CHECK(e.tailSamples() >= (1 << 29));
        q.freeze = false;
        q.line[0].on = false;
        const Buf more = silence(1000);   // off: it fades in 10 ms, and then nothing runs
        Wet v;
        go(e, q, more, more, more, more, v, 0, more.size(), tempo(bpm));
        CHECK(e.tailSamples() == 0);
    }
}

void echoReset() {       // reset: the same as a new Echo
    P p;
    on(p, 0, k8, 0.6f);
    on(p, 1, k316, 0.6f);
    p.routing = Echo::kSerial;
    const Buf L = whiteNoise(kSr, 0.4f, 18), R = whiteNoise(kSr, 0.4f, 19);
    Rig a, b;
    run(a, p, whiteNoise(kSr, 0.5f, 20), whiteNoise(kSr, 0.5f, 21));
    a.reset();
    const Wet w = run(a, p, L, R), v = run(b, p, L, R);
    CHECK(same(w.l, v.l) && same(w.r, v.r));
}

// --- no clicks ----------------------------------------------------------------------------------------

P clickBase() {          // A and B on, C and D set but off
    P p;
    Echo::Line& a = on(p, 0, k8, 0.4f);
    a.pan = -0.3f;
    Echo::Line& b = on(p, 1, k316, 0.4f);
    b.pan = 0.3f;
    on(p, 2, k16, 0.3f).on = false;
    on(p, 3, k4, 0.3f).on = false;
    return p;
}
// All four on, with times whose sum is no whole number of 400 Hz cycles, so a ring of them settles instead of building.
P ringBase(float fb = 0.5f) {
    P p;
    const float ms[4] = {69.2f, 89.2f, 64.9f, 82.5f};   // (no sum of some of them is near a whole number of 400 Hz cycles)
    for (int i = 0; i < 4; ++i) onFree(p, i, ms[i], fb).pan = 0.25f * static_cast<float>(i) - 0.4f;
    p.routing = Echo::kRing;
    return p;
}

// The steepest step of x[from, to) against the size of the sound there: each step over the peak of the three 110-sample
// blocks (a period of the 400 Hz tone) round it. A tone that swells or fades, as the wet does for a while after a routing
// has changed what goes round, is not taken for a click, and a step in a quiet place is not made big by the quiet (the
// peak is held at 0.05 at least). A pure sine gives 2 pi 400 / 44100 = 0.057.
double relSteepest(const Buf& x, size_t from, size_t to) {
    const size_t blk = 110;
    std::vector<float> peaks(x.size() / blk + 3, 0.0f);
    for (size_t i = 0; i < x.size(); ++i) peaks[i / blk] = std::max(peaks[i / blk], std::fabs(x[i]));
    double m = 0.0;
    for (size_t i = std::max<size_t>(from, 1); i < to && i < x.size(); ++i) {
        const size_t b = i / blk;
        const float a = std::max({b ? peaks[b - 1] : 0.0f, peaks[b], peaks[b + 1], 0.05f});
        m = std::max(m, static_cast<double>(std::fabs(x[i] - x[i - 1])) / a);
    }
    return m;
}

// A 400 Hz tone in both channels, a little different in each; at 3 s `after` takes over from `before`.
// The steepest step in the wet around the change (against the sound it is in) stays within `limit` of the steeper of the two settled states
// (or, with settled == false, of the state before it, for a modulator that keeps moving the wet's own edges).
// `lead`, if given, changes `before` for the first second and a half (a freeze is held after the lines have filled).
void click(const char* name, P before, void (*change)(P&), double limit = 1.25, bool settled = true, void (*lead)(P&) = nullptr) {
    const size_t n = 6 * kSr, at = 3 * kSr - 3 * kSr % kChunk, early = 3 * kSr / 2 - 3 * kSr / 2 % kChunk;
    const Buf L = sine(400.0, static_cast<int>(n), 0.4f), R = sine(400.0, static_cast<int>(n), 0.28f, 1.0);
    P after = before;
    change(after);
    Rig e;
    Wet w;
    P first = before;
    if (lead) lead(first);
    go(e, first, L, R, L, R, w, 0, early, playingAt(100.0, 0.0));
    go(e, before, L, R, L, R, w, early, at, playingAt(100.0, static_cast<double>(early) / kSr * 100.0 / 60.0));
    go(e, after, L, R, L, R, w, at, n, playingAt(100.0, static_cast<double>(at) / kSr * 100.0 / 60.0));
    for (const Buf* ch : {&w.l, &w.r}) {
        const double was = relSteepest(*ch, at - kSr / 2, at), will = relSteepest(*ch, n - kSr / 2, n);
        const double during = relSteepest(*ch, at, at + 2 * kSr);
        const double bound = std::max(0.01, limit * std::max(was, settled ? will : 0.0));
        if (!(during <= bound)) std::printf("  click: %s: steepest %g, before %g, after %g\n", name, during, was, will);
        CHECK(during <= bound);
    }
    CHECK(allFinite(w.l) && allFinite(w.r));
}

// A crossfade of 1 ms between two signals that are not the same (a Ring's lines come to carry what others carried, at other delays,
// so the phases of the old and the new have nothing to do with each other) adds up to |old - new| / 45 to the slope of a sample:
// up to 1.4 times a sine's at equal amplitudes, 1.8 at most. A jump would be 10 times that. The same-phase cases hold 1.25.
constexpr double kUnrelated = 1.6;

void echoNoClicks() {    // a routing, a mode, the freeze, a time, every switch and every control: a step no steeper than the tone's own
    // Every routing to every other, with all four lines on.
    {
        P all = clickBase();
        all.line[2].on = all.line[3].on = true;
        const char* names[5] = {"parallel", "serial", "pairs", "ring", "spread"};
        (void)names;
        for (int from = 0; from < 5; ++from)
            for (int to = 0; to < 5; ++to) {
                if (from == to) continue;
                P a = from == Echo::kRing ? ringBase() : all;
                a.routing = from;
                static int target;
                target = to;
                const std::string name = "routing " + std::to_string(from) + " -> " + std::to_string(to);
                const double limit = from == Echo::kRing || to == Echo::kRing ? kUnrelated : 1.25;
                if (to == Echo::kRing) {   // into the ring: the lines are ring-safe ones
                    P r = ringBase();
                    r.routing = from;
                    click(name.c_str(), r, [](P& p) { p.routing = Echo::kRing; }, limit);
                } else {
                    click(name.c_str(), a, [](P& p) { p.routing = target; }, limit);
                }
            }
    }
    // Modes.
    click("mode: stereo -> ping-pong", clickBase(), [](P& p) { p.line[0].p.mode = gf::EchoLine::kPingPong; });
    click("mode: ping-pong -> mono", [] { P p = clickBase(); p.line[1].p.mode = gf::EchoLine::kPingPong; return p; }(),
          [](P& p) { p.line[1].p.mode = gf::EchoLine::kMono; });
    click("mode: mono -> stereo", [] { P p = clickBase(); p.line[0].p.mode = gf::EchoLine::kMono; return p; }(),
          [](P& p) { p.line[0].p.mode = gf::EchoLine::kStereo; });
    // Freeze, on and off.
    click("freeze on", clickBase(), [](P& p) { p.freeze = true; });
    click("freeze off", [] { P p = clickBase(); p.freeze = true; return p; }(), [](P& p) { p.freeze = false; }, 1.25, true,
          [](P& p) { p.freeze = false; });
    // Time: a division (a crossfade), a free time (the tape bends), the glide mode.
    click("time: 1/8 -> 5/16, fade", [] { P p = clickBase(); p.glide = gf::EchoLine::kFade; return p; }(),
          [](P& p) { p.line[0].p.divBeats = k516; }, 1.5);
    click("time: 1/8 -> 3/16, tape", clickBase(), [](P& p) { p.line[0].p.divBeats = 0.53; }, 1.5);
    click("time: free 300 -> 315 ms, tape", [] { P p = clickBase(); p.line[0].p.sync = false; p.line[0].p.timeMs = 300.0f; return p; }(),
          [](P& p) { p.line[0].p.timeMs = 315.0f; }, 1.5);
    click("time: free 300 -> 315 ms, fade", [] { P p = clickBase(); p.glide = gf::EchoLine::kFade; p.line[0].p.sync = false; p.line[0].p.timeMs = 300.0f; return p; }(),
          [](P& p) { p.line[0].p.timeMs = 315.0f; }, 1.5);
    click("glide: tape -> fade", clickBase(), [](P& p) { p.glide = gf::EchoLine::kFade; });
    // Every switch: a line on and off, in each routing that has something to feed.
    click("line A off", clickBase(), [](P& p) { p.line[0].on = false; });
    click("line B on -> off, serial", [] { P p = clickBase(); p.routing = Echo::kSerial; return p; }(), [](P& p) { p.line[1].on = false; });
    click("line A on -> off, serial (it feeds B)", [] { P p = clickBase(); p.routing = Echo::kSerial; return p; }(), [](P& p) { p.line[0].on = false; });
    click("line A off -> on, serial (it feeds B)", [] { P p = clickBase(); p.routing = Echo::kSerial; p.line[0].on = false; return p; }(),
          [](P& p) { p.line[0].on = true; });
    click("line C on", clickBase(), [](P& p) { p.line[2].on = true; });
    click("line D on, spread", [] { P p = clickBase(); p.routing = Echo::kSpread; return p; }(), [](P& p) { p.line[3].on = true; });
    click("line D on, ring", [] { P p = ringBase(); p.line[3].on = false; return p; }(), [](P& p) { p.line[3].on = true; });
    click("ring at 1: D off", ringBase(1.0f), [](P& p) { p.line[3].on = false; }, kUnrelated);   // the ring closes over the lines that are on (R46)
    click("ring at 1: B off (a middle line)", ringBase(1.0f), [](P& p) { p.line[1].on = false; }, kUnrelated);
    click("ring at 1: A off (the one with the send)", ringBase(1.0f), [](P& p) { p.line[0].on = false; }, kUnrelated);
    click("ring at 1: B on", [] { P p = ringBase(1.0f); p.line[1].on = false; return p; }(), [](P& p) { p.line[1].on = true; }, kUnrelated);
    click("ring: A stereo -> ping-pong", ringBase(), [](P& p) { p.line[0].p.mode = gf::EchoLine::kPingPong; }, kUnrelated);
    click("ring (A ping-pong) -> serial", [] { P p = ringBase(); p.line[0].p.mode = gf::EchoLine::kPingPong; return p; }(),
          [](P& p) { p.routing = Echo::kSerial; }, kUnrelated);
    click("serial -> ring (A ping-pong)", [] { P p = ringBase(); p.routing = Echo::kSerial; p.line[0].p.mode = gf::EchoLine::kPingPong; return p; }(),
          [](P& p) { p.routing = Echo::kRing; }, kUnrelated);
    click("frozen: serial -> parallel", [] { P p = clickBase(); p.routing = Echo::kSerial; p.freeze = true; return p; }(), [](P& p) { p.routing = Echo::kParallel; },
          1.25, true, [](P& p) { p.freeze = false; });
    click("all lines off", clickBase(), [](P& p) { for (Echo::Line& l : p.line) l.on = false; });
    // The Echo's controls.
    click("send 1 -> 0", clickBase(), [](P& p) { p.send = 0.0f; });
    click("send 0 -> 1", [] { P p = clickBase(); p.send = 0.0f; return p; }(), [](P& p) { p.send = 1.0f; });
    click("duck 0 -> 1", clickBase(), [](P& p) { p.duck = 1.0f; });
    click("width 1 -> 0", clickBase(), [](P& p) { p.width = 0.0f; });
    click("width 1 -> 1.5", clickBase(), [](P& p) { p.width = 1.5f; });
    click("level 1 -> 0", clickBase(), [](P& p) { p.level = 0.0f; });
    click("pan -1 -> 1", [] { P p = clickBase(); p.line[0].pan = -1.0f; return p; }(), [](P& p) { p.line[0].pan = 1.0f; });
    click("line level 1 -> 0", clickBase(), [](P& p) { p.line[1].level = 0.0f; });
    click("feedback offset -1 -> 1", [] { P p = clickBase(); p.feedbackOffset = -1.0f; return p; }(), [](P& p) { p.feedbackOffset = 1.0f; }, 1.5);
    click("tone 0 -> -4", clickBase(), [](P& p) { p.toneOct = -4.0f; });
    click("tone -4 -> 4", [] { P p = clickBase(); p.toneOct = -4.0f; return p; }(), [](P& p) { p.toneOct = 4.0f; });
    // The lines' own filter and modulators.
    click("filter: type", [] { P p = clickBase(); p.line[0].p.cutoffHz = 700.0f; return p; }(), [](P& p) { p.line[0].p.filterType = gf::EchoLine::kHP; });
    click("filter: in loop -> on output", [] { P p = clickBase(); p.line[0].p.cutoffHz = 700.0f; return p; }(),
          [](P& p) { p.line[0].p.place = gf::EchoLine::kOnOutput; });
    click("modulator on: square on the level", clickBase(),
          [](P& p) {
              p.line[0].mod.mode = gf::EchoMod::kLfo;
              p.line[0].mod.shape = 0;
              p.line[0].mod.rateBeats = 1.0;
              p.line[0].modLevel = 1.0f;
          },
          1.25, false);
}

} // namespace

void echoTests() {
    echoRests();
    echoOneLine();
    echoParallel();
    echoSerial();
    echoPairs();
    echoSpread();
    echoRing();
    echoLoopScale();
    echoRingFeedback();
    echoRingPingPong();
    echoRingPingPongSend();
    echoRingLeaves();
    echoFreezeRoutings();
    echoRoutingChange();
    echoOff();
    echoFades();
    echoPolyrhythm();
    echoLevelPan();
    echoWidth();
    echoRingBounded();
    echoRingWorst();
    echoDuck();
    echoFreeze();
    echoTone();
    echoFeedbackOffset();
    echoModEnv();
    echoSharedKey();
    echoModShared();
    echoFollowResumes();
    echoModLevel();
    echoModTime();
    echoModSlewed();
    echoAllocate();
    echoChunks();
    echoAliasing();
    echoNan();
    echoExtremes();
    echoTail();
    echoReset();
    echoNoClicks();
}
