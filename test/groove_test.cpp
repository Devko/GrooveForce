// gf::Groove (dsp/groove.h): the chain. At rest it changes nothing, bit for bit; Shape and Pump act on
// the bus their target names (the Echo's wet bus is silent until plan 2, so a part aimed at it has
// nothing to work on); Shape comes before Pump; both swing from the one global Swing; a part switched
// on or off, or aimed at another bus, fades over 10 ms without a click, and a part that is off costs
// no work; input and output gain and mix. Then the contract: extremes, reset, the tail.
//
// The fades are measured on a sine that the part silences (the Pump on a Hold curve just after the
// bar's start, the Shape's Volume lane on a Square's low half): then out = in (1 - g), and g, the
// fade, can be read off sample by sample.
#include "signal.h"
#include "../dsp/groove.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {

using namespace eft;
using ef::kChunk;
using ef::kRate;
using ef::Transport;
using gf::Groove;
using gf::Pump;
using gf::Shape;
using gf::Swing;
using P = Groove::Params;

constexpr int kBeat = 22050;   // a beat at 120 BPM, in samples
constexpr double kEdge = 1.0 / 44.1;   // the 1 ms limit: a full scale a sample

Transport transport(double bpm, double beats, bool playing) {
    Transport t;
    t.bpm = bpm;
    t.beats = beats;
    t.playing = playing;
    t.valid = true;
    return t;
}
Transport playingAt(double beats) { return transport(120.0, beats, true); }

int shapeIdx(const char* name) {
    for (int i = 0; i < gf::numShapes(); ++i)
        if (std::string(gf::shapeName(i)) == name) return i;
    std::printf("  no shape named %s\n", name);
    CHECK(false);
    return 0;
}

bool same(const Buf& a, const Buf& b) { return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0; }
double maxDiff(const Buf& a, const Buf& b) {
    double m = 0.0;
    for (size_t i = 0; i < a.size(); ++i) m = std::max(m, static_cast<double>(std::fabs(a[i] - b[i])));
    return m;
}
Buf part(const Buf& x, size_t from, size_t to) { return Buf(x.begin() + static_cast<long>(from), x.begin() + static_cast<long>(to)); }
Buf dc(int n, float v) { return Buf(static_cast<size_t>(n), v); }

// The Pump on a Hold curve over a whole bar's step: silent from the bar's start (the bottom) until
// 60% of the bar (beat 2.4), whatever the sample: play it from beat 0.2 on.
void silencePump(P& p) {
    p.pump.depth = 1.0f;
    p.pump.curve = gf::PC_HOLD;
    p.pump.rateBeats = 4.0;
    p.pump.length = 1.0f;
}
// The Volume lane on a Square (high for the first half of the bar, low for the second): silent from
// beat 2 to beat 4. Play it from beat 2.2 on.
void silenceShape(P& p) {
    Shape::LaneParams& q = p.shape.lane[Shape::kVolume];
    q.on = true;
    q.shape = shapeIdx("Square");
    q.rateBeats = 4.0;
    q.depth = 1.0f;
}

// What play() hands back: the left output, and how many parts ran in each chunk.
struct Played {
    Buf out;
    std::vector<int> running;
};

// `in` through a Groove in chunks of `chunk` samples; edit(p, at) changes the parameters before the
// chunk that starts at sample `at`. R carries the same signal.
template <class F>
Played play(const Buf& in, P p, Transport t, F edit, int chunk = kChunk) {
    Groove g;
    Played r;
    r.out = in;
    Buf R = in;
    for (size_t pos = 0; pos < in.size(); pos += static_cast<size_t>(chunk)) {
        const int n = static_cast<int>(std::min(static_cast<size_t>(chunk), in.size() - pos));
        edit(p, pos);
        g.set(p, t);
        g.process(&r.out[pos], &R[pos], n);
        r.running.push_back(g.running());
        if (t.playing) t.beats += n / static_cast<double>(kRate) * t.bpm / 60.0;
    }
    return r;
}

Buf render(const P& p, const Buf& in, Transport t = playingAt(0.0), int chunk = kChunk) {
    Buf L = in, R = in;
    Groove g;
    run(g, p, L, R, t, chunk);
    return L;
}

// Out = in (1 - g) with g a fade from `start`: g = 1 - out / in at each sample the sine isn't too
// small to say. True if g is a straight line over 10 ms, within a millisecond either way (between
// the 11 and the 9 ms line), and never turns back. up: 0 to 1; else 1 to 0.
bool tenMsFade(const Buf& in, const Buf& out, size_t start, bool up) {
    bool ok = true;
    int told = 0;
    double last = up ? 0.0 : 1.0;
    for (size_t i = start; i < start + 700 && i < in.size(); ++i) {
        if (std::fabs(in[i]) < 1e-3f) continue;
        const double g = 1.0 - static_cast<double>(out[i]) / in[i];
        const double k = static_cast<double>(i - start + 1);
        double lo = std::min(k / 486.0, 1.0), hi = std::min(k / 397.0, 1.0);
        if (!up) {
            const double l = 1.0 - hi, h = 1.0 - lo;
            lo = l;
            hi = h;
        }
        ok = ok && g >= lo - 0.01 && g <= hi + 0.01;
        ok = ok && (up ? g >= last - 1e-4 : g <= last + 1e-4);
        last = g;
        ++told;
    }
    return ok && told > 500;
}

// No click across [from, from + 0.1 s): the steepest step stays under max(0.01, 1.25 x the settled
// steepest before and after) (EffectForce's rule for the gain modules).
bool clickFree(const Buf& out, size_t before, size_t from, size_t after) {
    const double settled = std::max(steepest(out, before, from), steepest(out, after, after + 2000));
    return steepest(out, from, from + 4410) <= std::max(0.01, 1.25 * settled);
}

// --- tests -------------------------------------------------------------------------------------

// Default Params, and parts that are on with nothing to do: the input, bit for bit, whatever the
// transport and the chunk size. A gain that went away and came back is exactly 1 again.
void grooveRests() {
    const Buf inL = whiteNoise(8192, 0.5f, 1), inR = whiteNoise(8192, 0.5f, 2);
    const Transport ts[3] = {transport(120.0, 0.0, false), transport(120.0, 5.3, true), transport(97.0, 11.7, true)};
    for (const Transport& t : ts)
        for (int chunk : {kChunk, 7, 1}) {
            Buf L = inL, R = inR;
            Groove g;
            P p;
            run(g, p, L, R, t, chunk);
            CHECK(same(L, inL) && same(R, inR));
            CHECK(g.running() == 0);
            CHECK(g.tailSamples() == 0);

            P q;   // on, depth 0 and every lane off
            q.pumpOn = q.shapeOn = true;
            L = inL;
            R = inR;
            Groove h;
            run(h, q, L, R, t, chunk);
            CHECK(same(L, inL) && same(R, inR));
            CHECK(h.running() == 2);
        }

    // +6 dB in, -6 dB out and mix 0.3 for a while, then back to the defaults: from there on the
    // input again, bit for bit.
    Buf L = inL, R = inR;
    Groove g;
    P p;
    for (size_t pos = 0; pos < L.size(); pos += kChunk) {
        if (pos < 2048) p.inputDb = 6.0f, p.outputDb = -6.0f, p.mix = 0.3f;
        else p = P();
        g.set(p, playingAt(1.0));
        g.process(&L[pos], &R[pos], kChunk);
    }
    CHECK(same(part(L, 4096, 8192), part(inL, 4096, 8192)));
    CHECK(same(part(R, 4096, 8192), part(inR, 4096, 8192)));
}

// Pump depth 1 on All and on Dry: the same ducked output (and the Pump's alone); on Echo, where there
// is no echo yet, the input. The same for the Shape. A target that changes is a fade, not a jump.
void grooveTargets() {
    const Buf in = dc(2 * kBeat, 0.5f);
    P base;
    base.pumpOn = true;
    base.pump.depth = 1.0f;

    Buf alone = in, aloneR = in;
    {
        Pump m;
        Pump::Params pp = base.pump;
        run(m, pp, alone, aloneR, playingAt(0.0));
    }
    P a = base, d = base, e = base;
    a.pumpTarget = Groove::kAll;
    d.pumpTarget = Groove::kDry;
    e.pumpTarget = Groove::kEcho;
    const Buf ba = render(a, in), bd = render(d, in), be = render(e, in);
    CHECK(same(ba, bd));
    CHECK(same(ba, alone));
    CHECK(peak(ba, 100, ba.size()) == 0.5f && *std::min_element(ba.begin(), ba.end()) < 0.01f);   // ducked, and back up
    CHECK(same(be, in));

    P s;   // the Shape: a Square on Volume, depth 1
    s.shapeOn = true;
    Shape::LaneParams& q = s.shape.lane[Shape::kVolume];
    q.on = true;
    q.shape = shapeIdx("Square");
    q.rateBeats = 1.0;
    q.depth = 1.0f;
    Buf shapeAlone = in, shapeAloneR = in;
    {
        Shape m;
        Shape::Params sp = s.shape;
        run(m, sp, shapeAlone, shapeAloneR, playingAt(0.0));
    }
    P sa = s, sd = s, se = s;
    sa.shapeTarget = Groove::kAll;
    sd.shapeTarget = Groove::kDry;
    se.shapeTarget = Groove::kEcho;
    const Buf ta = render(sa, in), td = render(sd, in), te = render(se, in);
    CHECK(same(ta, td));
    CHECK(same(ta, shapeAlone));
    CHECK(maxDiff(ta, in) > 0.4);
    CHECK(same(te, in));

    // Each part on its own bus: the Pump on the Echo leaves the Shape alone to do its work, and the other way round.
    P mixed = s;
    mixed.pumpOn = true;
    mixed.pump.depth = 1.0f;
    mixed.pumpTarget = Groove::kEcho;
    CHECK(same(render(mixed, in), shapeAlone));
    mixed.pumpTarget = Groove::kAll;
    mixed.shapeTarget = Groove::kEcho;
    CHECK(same(render(mixed, in), alone));

    // A target out of range is the nearest one.
    P lo = base, hi = base;
    lo.pumpTarget = -3;
    hi.pumpTarget = 17;
    CHECK(same(render(lo, in), ba));
    CHECK(same(render(hi, in), in));

    // A part moved to another bus fades out where it was, rests, and fades in on the new one: 10 ms
    // each way, no click, and nothing runs while it is aimed at the silent bus.
    const Buf sine1 = sine(110.0, 20480, 0.9f);
    P sil;
    silencePump(sil);
    sil.pumpOn = true;
    sil.pumpTarget = Groove::kDry;
    auto edit = [](P& x, size_t at) {
        x.pumpTarget = at >= 12288 ? Groove::kDry : (at >= 6144 ? Groove::kEcho : Groove::kDry);
    };
    const Played r = play(sine1, sil, playingAt(0.2), edit);
    CHECK(peak(r.out, 441, 6144) == 0.0f);   // on from the start (fresh): silent
    CHECK(tenMsFade(sine1, r.out, 6144, false));
    CHECK(same(part(r.out, 6144 + 500, 12288), part(sine1, 6144 + 500, 12288)));
    CHECK(r.running[(6144 + 500) / kChunk + 1] == 0 && r.running[12000 / kChunk] == 0);
    CHECK(tenMsFade(sine1, r.out, 12288, true));
    CHECK(peak(r.out, 12288 + 500, r.out.size()) == 0.0f);
    CHECK(r.running[14000 / kChunk] == 1);
    CHECK(clickFree(r.out, 4096, 6144, 8192) && clickFree(r.out, 10000, 12288, 14336));

    // All and Dry are one bus while the wet is silent: a part moved between them does not notice
    // (no fade, no reset, and it runs throughout).
    auto flip = [](P& x, size_t at) { x.pumpTarget = (at / 1024) % 2 ? Groove::kDry : Groove::kAll; };
    auto stay = [](P&, size_t) {};
    const Played f = play(in, base, playingAt(0.0), flip), still = play(in, base, playingAt(0.0), stay);
    CHECK(same(f.out, still.out) && same(still.out, ba));
    CHECK(*std::min_element(f.running.begin(), f.running.end()) == 1);
}

// A part switched on or off mid-sine: a 10 ms straight line (+-1 ms), no click; and a part that is off
// and faded out costs no work: it is not run (running() is the number of parts run in the chunk), the
// output is the input bit for bit, and a part switched on again starts from cleared state.
void grooveFades() {
    const Buf in = sine(110.0, 16384, 0.9f);
    for (int which = 0; which < 2; ++which) {   // 0: the Pump, 1: the Shape
        P p;
        if (which == 0) silencePump(p);
        else silenceShape(p);
        const Transport t = playingAt(which == 0 ? 0.2 : 2.2);
        auto edit = [which](P& x, size_t at) {
            const bool on = at >= 4096 && at < 10240;
            (which == 0 ? x.pumpOn : x.shapeOn) = on;
        };
        const Played r = play(in, p, t, edit);
        CHECK(same(part(r.out, 0, 4096), part(in, 0, 4096)));       // off: untouched
        CHECK(tenMsFade(in, r.out, 4096, true));
        CHECK(peak(r.out, 4096 + 500, 10240) == 0.0f);              // on: the part's own work (silence)
        CHECK(tenMsFade(in, r.out, 10240, false));
        CHECK(same(part(r.out, 10240 + 500, 16384), part(in, 10240 + 500, 16384)));
        CHECK(clickFree(r.out, 2048, 4096, 6144));
        CHECK(clickFree(r.out, 6144, 10240, 12288));

        // Off costs no work: not run before it is switched on, run through its fade-in, on and its
        // fade-out, and not run again once the fade is over (a chunk after the 441 samples).
        bool idleBefore = true, busy = true, idleAfter = true;
        for (size_t c = 0; c < r.running.size(); ++c) {
            const size_t at = c * kChunk;
            if (at < 4096) idleBefore = idleBefore && r.running[c] == 0;
            else if (at < 10240 + 441) busy = busy && r.running[c] == 1;
            else if (at >= 10240 + 441 + 2 * kChunk) idleAfter = idleAfter && r.running[c] == 0;
        }
        CHECK(idleBefore && busy && idleAfter);
    }

    // Switched on and off faster than the fade: g turns round in the middle, out stays between 0 and
    // the input, and nothing clicks.
    {
        P p;
        silencePump(p);
        auto edit = [](P& x, size_t at) { x.pumpOn = at >= 4096 && at < 8192 && (at / (4 * kChunk)) % 2 == 0; };
        const Played r = play(in, p, playingAt(0.2), edit);
        bool between = true;
        for (size_t i = 0; i < r.out.size(); ++i)
            between = between && r.out[i] * in[i] >= 0.0f && std::fabs(r.out[i]) <= std::fabs(in[i]) + 1e-6f;
        CHECK(between);
        CHECK(allFinite(r.out));
        CHECK(clickFree(r.out, 2048, 4096, 10240));
        CHECK(maxDiff(part(r.out, 4096, 8192), part(in, 4096, 8192)) > 0.2);   // it did work (g gets to 0.29)
    }

    // A part that was off comes back from cleared state: the same output from there on as a copy that
    // never ran (a resonant filter's ring, the Pump's grid on a stopped transport).
    {
        const Buf noise = whiteNoise(24576, 0.5f, 5);
        P p;
        p.pump.depth = 1.0f;
        p.pump.rateBeats = 0.5;
        Shape::LaneParams& q = p.shape.lane[Shape::kFilter];
        q.on = true;
        q.shape = shapeIdx("Sine");
        q.rateBeats = 1.0;
        q.depth = 1.0f;
        p.shape.cutoffHz = 800.0f;
        p.shape.resonance = 1.0f;
        const Transport t = transport(120.0, 3.0, false);   // stopped: the grid runs on from where the part started
        auto again = [](P& x, size_t at) { x.pumpOn = x.shapeOn = (at < 4096) || at >= 16384; };
        auto never = [](P& x, size_t at) { x.pumpOn = x.shapeOn = at >= 16384; };
        const Played a = play(noise, p, t, again), b = play(noise, p, t, never);
        CHECK(same(part(a.out, 16384 + 2 * 441, 24576), part(b.out, 16384 + 2 * 441, 24576)));
        CHECK(maxDiff(part(a.out, 16384, 24576), part(noise, 16384, 24576)) > 0.01);
    }
}

// Shape, then Pump. Volume on the Offbeat gate with depth 1 under a 1/4 pump is the product of each
// alone; and with parts that don't commute (a filter that rings, a pump) the chain is the Pump after
// the Shape, bit for bit, and not the other way round.
void grooveOrder() {
    const Buf one = dc(2 * kBeat, 1.0f);
    P p;
    p.shapeOn = p.pumpOn = true;
    Shape::LaneParams& v = p.shape.lane[Shape::kVolume];
    v.on = true;
    v.shape = shapeIdx("Offbeat");
    v.rateBeats = 4.0;
    v.depth = 1.0f;
    p.pump.depth = 1.0f;
    p.pump.rateBeats = 1.0;
    P ps = p, pp = p;
    ps.pumpOn = false;
    pp.shapeOn = false;
    const Buf both = render(p, one), gs = render(ps, one), gp = render(pp, one);
    double worst = 0.0;
    for (size_t i = 0; i < one.size(); ++i) worst = std::max(worst, std::fabs(static_cast<double>(both[i]) - static_cast<double>(gs[i]) * gp[i]));
    CHECK(worst < 1e-5);
    CHECK(maxDiff(gs, one) > 0.9 && maxDiff(gp, one) > 0.9 && *std::max_element(both.begin(), both.end()) > 0.9f);

    const Buf inL = whiteNoise(16384, 0.5f, 1), inR = whiteNoise(16384, 0.5f, 2);
    P q;
    q.shapeOn = q.pumpOn = true;
    Shape::LaneParams& f = q.shape.lane[Shape::kFilter];
    f.on = true;
    f.shape = shapeIdx("Sine");
    f.rateBeats = 1.0;
    f.depth = 1.0f;
    q.shape.cutoffHz = 3000.0f;
    q.shape.resonance = 0.8f;
    q.pump.depth = 1.0f;
    q.pump.rateBeats = 0.5;
    q.pump.curve = gf::PC_HARD;
    q.swing.amount = 0.6f;

    auto chain = [&](bool shapeFirst, Buf& L, Buf& R) {
        Shape s;
        Pump m;
        Shape::Params sp = q.shape;
        Pump::Params mp = q.pump;
        sp.swing = mp.swing = Groove::stepSwing(q);
        Transport t = playingAt(0.0);
        for (size_t pos = 0; pos < L.size(); pos += kChunk) {
            s.set(sp, t);
            m.set(mp, t);
            if (shapeFirst) {
                s.process(&L[pos], &R[pos], kChunk);
                m.process(&L[pos], &R[pos], kChunk);
            } else {
                m.process(&L[pos], &R[pos], kChunk);
                s.process(&L[pos], &R[pos], kChunk);
            }
            t.beats += kChunk / static_cast<double>(kRate) * t.bpm / 60.0;
        }
    };
    Buf gl = inL, gr = inR, sl = inL, sr = inR, pl = inL, pr = inR;
    Groove g;
    run(g, q, gl, gr, playingAt(0.0));
    chain(true, sl, sr);
    chain(false, pl, pr);
    CHECK(same(gl, sl) && same(gr, sr));
    CHECK(maxDiff(gl, pl) > 1e-3);   // (the other order is another sound)
}

// stepSwing is the one place that says which swing the parts' steps follow; here the global Swing.
// The Pump's and the Shape's own swing fields are not heard, and the audio is not swung a second time.
void grooveStepSwing() {
    P p;
    CHECK(Groove::stepSwing(p).amount == 0.5f && Groove::stepSwing(p).unitBeats == 0.25);
    p.swing.amount = 0.66f;
    p.swing.unitBeats = 0.5;
    CHECK(Groove::stepSwing(p).amount == 0.66f && Groove::stepSwing(p).unitBeats == 0.5);

    const Buf in = dc(2 * kBeat, 0.5f);
    const Swing global{0.66f, 0.25}, decoy{0.75f, 0.5}, straight{0.5f, 0.25};
    P q;
    q.swing = global;
    q.pumpOn = true;
    q.pump.depth = 1.0f;
    q.pump.rateBeats = 0.25;
    q.pump.swing = decoy;
    CHECK(Groove::stepSwing(q).amount == 0.66f);

    auto pumpWith = [&](const Swing& s) {
        Pump::Params pp = q.pump;
        pp.swing = s;
        Buf L = in, R = in;
        Pump m;
        run(m, pp, L, R, playingAt(0.0));
        return L;
    };
    const Buf g = render(q, in);
    CHECK(same(g, pumpWith(global)));
    CHECK(maxDiff(g, pumpWith(straight)) > 0.1 && maxDiff(g, pumpWith(decoy)) > 0.1);   // (the swing is heard)
    q.swing = straight;
    CHECK(same(render(q, in), pumpWith(straight)));

    P s;   // the Shape: the Off 16ths gate on Volume
    s.swing = global;
    s.shapeOn = true;
    Shape::LaneParams& l = s.shape.lane[Shape::kVolume];
    l.on = true;
    l.shape = shapeIdx("Off 16ths");
    l.rateBeats = 4.0;
    l.depth = 1.0f;
    s.shape.swing = decoy;
    auto shapeWith = [&](const Swing& w) {
        Shape::Params sp = s.shape;
        sp.swing = w;
        Buf L = in, R = in;
        Shape m;
        run(m, sp, L, R, playingAt(0.0));
        return L;
    };
    const Buf h = render(s, in);
    CHECK(same(h, shapeWith(global)));
    CHECK(maxDiff(h, shapeWith(straight)) > 0.1 && maxDiff(h, shapeWith(decoy)) > 0.1);
    s.swing = straight;
    CHECK(same(render(s, in), shapeWith(straight)));

    // Both parts, once: each swung by the global Swing, and the audio itself not at all.
    P both = q;
    both.swing = global;
    both.shapeOn = true;
    both.shape = s.shape;
    Buf bl = in, br = in;
    {
        Shape sh;
        Pump pm;
        Shape::Params sp = both.shape;
        Pump::Params mp = both.pump;
        sp.swing = mp.swing = global;
        Transport t = playingAt(0.0);
        for (size_t pos = 0; pos < bl.size(); pos += kChunk) {
            const int n = static_cast<int>(std::min(static_cast<size_t>(kChunk), bl.size() - pos));
            sh.set(sp, t);
            sh.process(&bl[pos], &br[pos], n);
            pm.set(mp, t);
            pm.process(&bl[pos], &br[pos], n);
            t.beats += n / static_cast<double>(kRate) * t.bpm / 60.0;
        }
    }
    CHECK(same(render(both, in), bl));
}

// Input +6 dB and output -6 dB are unity; mix 0 is the input bit for bit, mix 0.5 halfway (against the
// input as it came in, not as it was gained); no gain edge is faster than 1 ms.
void grooveGains() {
    const Buf in = whiteNoise(4096, 0.5f, 3);
    P p;
    p.inputDb = 6.0f;
    p.outputDb = -6.0f;
    CHECK(maxDiff(render(p, in), in) < 2e-6);

    P up;
    up.inputDb = 6.0f;
    const Buf gained = render(up, in);
    double worst = 0.0;
    for (size_t i = 0; i < in.size(); ++i) worst = std::max(worst, std::fabs(static_cast<double>(gained[i]) - static_cast<double>(in[i]) * ef::dbToGain(6.0f)));
    CHECK(worst < 1e-6);
    P down;
    down.outputDb = -12.0f;
    const Buf quiet = render(down, in);
    worst = 0.0;
    for (size_t i = 0; i < in.size(); ++i) worst = std::max(worst, std::fabs(static_cast<double>(quiet[i]) - static_cast<double>(in[i]) * ef::dbToGain(-12.0f)));
    CHECK(worst < 1e-6);

    // Mix 0: the input, with the gains and both parts doing all they can.
    P m0;
    m0.inputDb = 6.0f;
    m0.outputDb = 12.0f;
    m0.mix = 0.0f;
    m0.pumpOn = true;
    m0.pump.depth = 1.0f;
    CHECK(same(render(m0, in), in));
    // ... and a mix that gets there: from 1, in a few milliseconds, then the input again.
    {
        P q = m0;
        auto edit = [](P& x, size_t at) { x.mix = at >= 1024 ? 0.0f : 1.0f; };
        const Played r = play(in, q, playingAt(0.0), edit);
        CHECK(same(part(r.out, 1024 + 96, 4096), part(in, 1024 + 96, 4096)));
    }

    // Mix 0.5 halfway between the input and the chain's output.
    P half;
    half.inputDb = 6.0f;
    half.mix = 0.5f;
    const Buf mixed = render(half, in);
    worst = 0.0;
    for (size_t i = 0; i < in.size(); ++i)
        worst = std::max(worst, std::fabs(static_cast<double>(mixed[i]) - 0.5 * (static_cast<double>(in[i]) + static_cast<double>(in[i]) * ef::dbToGain(6.0f))));
    CHECK(worst < 1e-6);
    // The dry side of the mix is the input before the input gain: the Pump silences the wet side.
    P silent = half;
    silent.pumpOn = true;
    silent.pump.depth = 1.0f;
    silent.pump.curve = gf::PC_HOLD;
    silent.pump.rateBeats = 4.0;
    silent.pump.length = 1.0f;
    const Buf dry = render(silent, in, playingAt(0.2));
    worst = 0.0;
    for (size_t i = 0; i < in.size(); ++i) worst = std::max(worst, std::fabs(static_cast<double>(dry[i]) - 0.5 * in[i]));
    CHECK(worst < 1e-6);

    // No gain edge faster than 1 ms: on a steady input the output is the gain itself. A jump of the
    // input gain (to +24 dB), of the output gain (to -24 dB) and of the mix takes at least 1 ms for
    // a full scale, and gets there.
    const Buf steady = dc(8192, 1.0f);
    {
        P q;
        auto edit = [](P& x, size_t at) { x.inputDb = at >= 2048 ? 24.0f : 0.0f; };
        const Played r = play(steady, q, playingAt(0.0), edit);
        CHECK(steepest(r.out, 0, r.out.size()) <= kEdge + 1e-5);
        CHECK(std::fabs(r.out.back() - ef::dbToGain(24.0f)) < 2e-6);   // (the compiler's own rounding of the gain, on the Force's build)
        CHECK(r.out[2048 + 4 * kChunk] < r.out.back());   // (it is a glide, not a jump)
    }
    {
        P q;
        auto edit = [](P& x, size_t at) { x.outputDb = at >= 2048 ? -24.0f : 0.0f; };
        const Played r = play(steady, q, playingAt(0.0), edit);
        CHECK(steepest(r.out, 0, r.out.size()) <= kEdge + 1e-5);
        CHECK(std::fabs(r.out.back() - ef::dbToGain(-24.0f)) < 2e-6);
    }
    {
        P q;
        q.outputDb = -6.0206f;   // half: the mix moves the output between 1 and 0.5
        auto edit = [](P& x, size_t at) { x.mix = at >= 2048 ? 0.0f : 1.0f; };
        const Played r = play(steady, q, playingAt(0.0), edit);
        CHECK(steepest(r.out, 0, r.out.size()) <= 0.5 * kEdge + 1e-5);
        CHECK(r.out.back() == 1.0f);
    }
    {   // a sine, gain from 0 to +24 dB: no click
        const Buf s = sine(110.0, 16384, 0.05f);
        P q;
        auto edit = [](P& x, size_t at) { x.inputDb = at >= 4096 ? 24.0f : 0.0f; };
        const Played r = play(s, q, playingAt(0.0), edit);
        CHECK(clickFree(r.out, 2048, 4096, 10240));
    }
}

// The contract: finite output for any finite input and any parameters, reset forgets, and the tail.
void grooveContract() {
    const Buf inL = whiteNoise(16384, 1.0f, 7), inR = whiteNoise(16384, 1.0f, 8);

    // Everything on at its extremes, and parameters that are not numbers or not in range.
    const float inf = std::numeric_limits<float>::infinity(), nan = std::numeric_limits<float>::quiet_NaN();
    for (int variant = 0; variant < 4; ++variant) {
        P p;
        p.pumpOn = p.shapeOn = true;
        p.pump.depth = 1.0f;
        p.pump.rateBeats = 0.25;
        p.pump.length = 0.05f;
        p.pump.pushMs = 30.0f;
        p.pump.trigger = Pump::kInput;
        p.pump.sensitivity = 1.0f;
        p.pump.band = Pump::kLows;
        p.shape.cutoffHz = 20.0f;
        p.shape.resonance = 1.0f;
        for (int l = 0; l < Shape::kLanes; ++l) {
            p.shape.lane[l].on = true;
            p.shape.lane[l].shape = 3 * l + variant;
            p.shape.lane[l].rateBeats = 0.25;
            p.shape.lane[l].depth = l == Shape::kVolume ? 1.0f : -1.0f;
        }
        p.swing.amount = 0.75f;
        p.inputDb = 24.0f;
        p.outputDb = 24.0f;
        p.mix = 0.5f;
        if (variant == 1) p.pumpTarget = -5, p.shapeTarget = 99;
        if (variant == 2) p.inputDb = 1e30f, p.outputDb = -1e30f, p.mix = 2.0f;
        if (variant == 3) p.inputDb = nan, p.outputDb = inf, p.mix = nan, p.swing.amount = nan;
        for (int chunk : {kChunk, 13, 1}) {
            Buf L = inL, R = inR;
            Groove g;
            run(g, p, L, R, playingAt(1.3), chunk);
            CHECK(allFinite(L) && allFinite(R));
            CHECK(g.tailSamples() >= 0);
        }
    }

    // A NaN or an infinity in the input is not kept: the parts take it for silence.
    {
        P p;
        p.pumpOn = p.shapeOn = true;
        p.pump.depth = 0.5f;
        p.shape.lane[Shape::kFilter].on = true;
        p.shape.lane[Shape::kFilter].shape = shapeIdx("Sine");
        p.shape.lane[Shape::kFilter].depth = 1.0f;
        p.shape.cutoffHz = 500.0f;
        Buf L = inL, R = inR;
        L[1000] = nan;
        R[3000] = inf;
        Groove g;
        run(g, p, L, R, playingAt(0.0));
        CHECK(allFinite(L) && allFinite(R));
    }

    // Reset forgets everything: the same run again from the start.
    {
        P p;
        p.pumpOn = p.shapeOn = true;
        p.pump.depth = 0.8f;
        p.pump.trigger = Pump::kInput;
        Shape::LaneParams& q = p.shape.lane[Shape::kFilter];
        q.on = true;
        q.shape = shapeIdx("Triangle");
        q.rateBeats = 1.0;
        q.depth = 1.0f;
        p.shape.cutoffHz = 2000.0f;
        p.shape.resonance = 0.9f;
        Groove g;
        Buf aL = inL, aR = inR, bL = inL, bR = inR;
        run(g, p, aL, aR, playingAt(0.0));
        P other;   // something else in between, so that state is left to forget
        other.inputDb = -5.0f;
        Buf xL = inL, xR = inR;
        run(g, other, xL, xR, playingAt(2.0));
        g.reset();
        run(g, p, bL, bR, playingAt(0.0));
        CHECK(same(aL, bL) && same(aR, bR));
    }

    // The tail: none at rest; the Pump's crossover while it is banded, the Shape's filter while it
    // rings; and none again once a part is faded out, or aimed at the bus with nothing on it.
    {
        Groove g;
        P p;
        Buf L = inL, R = inR;
        run(g, p, L, R, playingAt(0.0));
        CHECK(g.tailSamples() == 0);

        p.pumpOn = true;
        p.pump.depth = 1.0f;
        p.pump.band = Pump::kLows;
        L = inL, R = inR;
        run(g, p, L, R, playingAt(0.0));
        CHECK(g.tailSamples() >= Pump::kTail);
        p.pumpOn = false;
        Buf L2 = part(inL, 0, 2048), R2 = part(inR, 0, 2048);
        run(g, p, L2, R2, playingAt(0.0));
        CHECK(g.tailSamples() == 0);

        p = P();
        p.shapeOn = true;
        Shape::LaneParams& q = p.shape.lane[Shape::kFilter];
        q.on = true;
        q.depth = 0.5f;
        p.shape.cutoffHz = 200.0f;
        p.shape.resonance = 1.0f;
        L2 = part(inL, 0, 2048), R2 = part(inR, 0, 2048);
        run(g, p, L2, R2, playingAt(0.0));
        CHECK(g.tailSamples() > 1000);
        p.shapeTarget = Groove::kEcho;
        L2 = part(inL, 0, 2048), R2 = part(inR, 0, 2048);
        run(g, p, L2, R2, playingAt(0.0));
        CHECK(g.tailSamples() == 0);
    }
}

} // namespace

void grooveTests() {
    grooveRests();
    grooveTargets();
    grooveFades();
    grooveOrder();
    grooveStepSwing();
    grooveGains();
    grooveContract();
}
