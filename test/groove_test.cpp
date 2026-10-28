// gf::Groove (dsp/groove.h): the chain. At rest it changes nothing, bit for bit; Shape and Pump act on
// the bus their target names (a part aimed at the Echo has nothing to work on while the wet is silent);
// Shape comes before Pump; both swing from the one global Swing; a part switched on or off, or aimed at
// another bus, fades over 10 ms without a click, and a part that is off costs no work; input and output
// gain and mix. Then the contract: extremes, reset, the tail.
//
// Plan 2 grew the chain: the Echo's live wet bus (a part on the Echo ducks only the repeats; on All it
// acts on the sum of the buses; Echo first or last), Time's place (Post bends the echoes too, Pre only
// the input), the key (Rumble: the Pump on the Echo hears the dry kick), the dry level, the swing-once
// rule with Time, and the Echo's and Time's first switch-on while playing (the memory arrives on the UI
// thread).
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
using gf::Echo;
using gf::Groove;
using gf::Pump;
using gf::Shape;
using gf::Swing;
using gf::Time;
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
    Buf out;    // the left output
    Buf outR;   // the right, from play()
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
        g.allocate(p);   // the UI thread sees a switch before the audio thread does
        g.set(p, t);
        g.process(&r.out[pos], &R[pos], n);
        r.running.push_back(g.running());
        if (t.playing) t.beats += n / static_cast<double>(kRate) * t.bpm / 60.0;
    }
    r.outR = R;
    return r;
}

Buf render(const P& p, const Buf& in, Transport t = playingAt(0.0), int chunk = kChunk) {
    Buf L = in, R = in;
    Groove g;
    CHECK(g.allocate(p));
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
// steepest before and after) (EffectWerk's rule for the gain modules).
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

    // The Echo and Time, allocated or not, off or on with nothing to do (no line on; no groove and no
    // pattern, when Time only records): the input, bit for bit. Parts that have no memory yet cost nothing.
    for (const Transport& t : ts)
        for (int chunk : {kChunk, 7, 1})
            for (int allocated = 0; allocated < 2; ++allocated) {
                P on;
                on.echoOn = on.timeOn = true;
                Buf L = inL, R = inR;
                Groove g;
                if (allocated) CHECK(g.allocate(on));
                run(g, on, L, R, t, chunk);
                CHECK(same(L, inL) && same(R, inR));
                CHECK(g.running() == (allocated ? 2 : 0));
                CHECK(g.tailSamples() == 0);

                L = inL;
                R = inR;
                Groove h;   // allocated, and off
                CHECK(h.allocate(on));
                run(h, P(), L, R, t, chunk);
                CHECK(same(L, inL) && same(R, inR));
                CHECK(h.running() == 0 && h.tailSamples() == 0);
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

// --- plan 2: the Echo's bus, Time's place, the key ----------------------------------------------------------

// The Echo on, its line A synced to `beats` with `fb` feedback and the other lines off: the wet is the input
// again, a repeat at a time. At 120 BPM a beat is kBeat samples.
void echoA(P& p, double beats, float fb = 0.0f) {
    p.echoOn = true;
    Echo::Line& l = p.echo.line[0];
    l.on = true;
    l.p.sync = true;
    l.p.divBeats = beats;
    l.p.feedback = fb;
}
// ... or free-running at `ms`.
void echoFree(P& p, float ms) {
    echoA(p, 0.5);
    p.echo.line[0].p.sync = false;
    p.echo.line[0].p.timeMs = ms;
}
int timePat(const char* name) {
    for (int i = 0; i < gf::numTimePatterns(); ++i)
        if (std::string(gf::timePatternName(i)) == name) return i;
    std::printf("  no time pattern named %s\n", name);
    CHECK(false);
    return 0;
}

// x += a kick at `at`: 55 Hz, 60 ms decay (the Pump's own test material).
void addKick(Buf& x, int at, float amp) {
    const double tau = 0.060 * kRate;
    for (int i = 0; at + i < static_cast<int>(x.size()) && i < 10 * tau; ++i)
        x[static_cast<size_t>(at + i)] += amp * static_cast<float>(std::sin(2.0 * kPi * 55.0 * i / kRate) * std::exp(-i / tau));
}
// A pad (330 Hz) with a kick on every beat from `lead` on.
Buf kickPad(int beats, int lead) {
    Buf x = sine(330.0, lead + beats * kBeat, 0.25f);
    for (int k = 0; k < beats; ++k) addKick(x, lead + k * kBeat, 0.8f);
    return x;
}

// A Pump on its own over x (both sides alike), in chunks, listening to `key` (null: to x itself).
Buf pumpAlone(const Pump::Params& pp, const Buf& x, const Buf* key, Transport t) {
    Buf L = x, R = x;
    Pump m;
    for (size_t pos = 0; pos < L.size(); pos += kChunk) {
        const int n = static_cast<int>(std::min(static_cast<size_t>(kChunk), L.size() - pos));
        m.set(pp, t);
        if (key) m.process(&L[pos], &R[pos], n, &(*key)[pos], &(*key)[pos]);
        else m.process(&L[pos], &R[pos], n);
        if (t.playing) t.beats += n / static_cast<double>(kRate) * t.bpm / 60.0;
    }
    return L;
}
Buf shapeAlone(const Shape::Params& sp, const Buf& x, Transport t) {
    Buf L = x, R = x;
    Shape m;
    run(m, sp, L, R, t);
    return L;
}
// a - b, and a + b, sample by sample.
Buf minus(const Buf& a, const Buf& b) {
    Buf d(a.size());
    for (size_t i = 0; i < a.size(); ++i) d[i] = a[i] - b[i];
    return d;
}
Buf plus(const Buf& a, const Buf& b) {
    Buf d(a.size());
    for (size_t i = 0; i < a.size(); ++i) d[i] = a[i] + b[i];
    return d;
}

// The same as play(), but the memory (the Echo's lines, Time's ring) is made when the chunk that starts at
// `allocAt` comes up: the UI thread, between two chunks.
template <class F>
Played playLate(const Buf& in, P p, Transport t, F edit, size_t allocAt) {
    Groove g;
    Played r;
    r.out = in;
    Buf R = in;
    for (size_t pos = 0; pos < in.size(); pos += kChunk) {
        const int n = static_cast<int>(std::min(static_cast<size_t>(kChunk), in.size() - pos));
        edit(p, pos);
        if (pos == allocAt) {
            CHECK(g.allocateEcho());
            CHECK(g.allocateTime());
        }
        g.set(p, t);
        g.process(&r.out[pos], &R[pos], n);
        r.running.push_back(g.running());
        if (t.playing) t.beats += n / static_cast<double>(kRate) * t.bpm / 60.0;
    }
    return r;
}

// A fade of the wet: out - in is a delayed copy of the sine `in` (hz, amp), `delay` samples late, at a gain g that
// goes along a straight line over 10 ms, up from `start` or down. g is found a period at a time (a correlation
// with the copy: the Echo's own 30 Hz cut moves the phase a little), and checked against the lines 11 and 9 ms
// long, with a few percent to spare. Up, the wet is nothing until the lines have been written, so the first
// `delay` samples are not asked.
bool wetFade(const Buf& in, const Buf& out, double hz, float amp, double delay, size_t start, bool up) {
    const size_t win = static_cast<size_t>(kRate / hz + 0.5);
    bool ok = true;
    int told = 0;
    double last = up ? 0.0 : 1.0;
    for (size_t a = start + (up ? static_cast<size_t>(delay) + 8 : 0); a + win <= start + 600 && a + win <= in.size(); a += win) {
        double num = 0.0, den = 0.0;
        for (size_t i = a; i < a + win; ++i) {
            const double ref = amp * std::sin(2.0 * kPi * hz * (static_cast<double>(i) - delay) / kRate);
            num += (static_cast<double>(out[i]) - in[i]) * ref;
            den += ref * ref;
        }
        const double g = num / den, k = static_cast<double>(a + win / 2 - start + 1);
        double lo = std::min(k / 486.0, 1.0), hi = std::min(k / 397.0, 1.0);
        if (!up) {
            const double l = 1.0 - hi, h = 1.0 - lo;
            lo = l;
            hi = h;
        }
        ok = ok && g >= lo - 0.04 && g <= hi + 0.04;
        ok = ok && (up ? g >= last - 0.04 : g <= last + 0.04);
        last = g;
        ++told;
    }
    return ok && told >= 8;
}

// A Pump on the Echo ducks only the repeats; the dry passes bit for bit. On the Dry the other way round: the
// repeats hear the input as it came, not as the Pump left it.
void grooveEchoBus() {
    const Buf in = sine(220.0, 4 * kBeat, 0.4f);
    P echo;
    echoA(echo, 0.5, 0.5f);   // 1/8: a repeat every half beat, each half the one before
    const Transport t = playingAt(0.2);   // the Hold curve (silencePump) is silent from here until beat 2.4: sample 48510
    const Buf plain = render(echo, in, t);
    const size_t quiet = 47000;
    CHECK(maxDiff(part(plain, 12000, quiet), part(in, 12000, quiet)) > 0.3);   // (the echo is there)

    P duck = echo;
    silencePump(duck);
    duck.pumpOn = true;
    duck.pumpTarget = Groove::kEcho;
    const Buf d = render(duck, in, t);
    CHECK(same(part(d, 0, quiet), part(in, 0, quiet)));   // the dry, bit for bit, with nothing from the echo
    const Buf wet = minus(plain, in);
    CHECK(maxDiff(d, plus(in, pumpAlone(duck.pump, wet, nullptr, t))) < 2e-5);   // the repeats are the Pump's alone, the dry untouched, throughout
    CHECK(maxDiff(d, plain) > 0.3);   // (it did duck them)

    P dry = duck;
    dry.pumpTarget = Groove::kDry;
    const Buf dd = render(dry, in, t);
    CHECK(peak(dd, 0, 11000) == 0.0f);   // the dry is silenced
    double worst = 0.0;
    for (size_t i = 12000; i < quiet; ++i) worst = std::max(worst, std::fabs(static_cast<double>(dd[i]) - (static_cast<double>(plain[i]) - in[i])));
    CHECK(worst < 1e-6);   // and the repeats are the ones of the unducked input
    P all = duck;
    all.pumpTarget = Groove::kAll;
    CHECK(peak(render(all, in, t), 0, quiet) == 0.0f);   // on both: nothing at all

    // The Shape the same: a Square on Volume closes the second half of a bar's cycle (beats 2 to 4).
    P gate = echo;
    gate.shapeOn = true;
    Shape::LaneParams& q = gate.shape.lane[Shape::kVolume];
    q.on = true;
    q.shape = shapeIdx("Square");
    q.rateBeats = 4.0;
    q.depth = 1.0f;
    gate.shapeTarget = Groove::kEcho;
    const Buf g = render(gate, in, playingAt(2.2));
    CHECK(same(part(g, 0, 20000), part(in, 0, 20000)));
    gate.shapeTarget = Groove::kDry;
    const Buf gd = render(gate, in, playingAt(2.2));
    CHECK(peak(gd, 0, 11000) == 0.0f);
}

// A part on All is that part on the sum of the two buses: the Groove with the Echo on and a Pump (or a Shape)
// on All gives what the part gives alone on dry + wet. The Input trigger listens to the chain's input in both.
void grooveAllBothBuses() {
    const Buf in = kickPad(6, 4410);
    const Transport t = playingAt(0.0);
    P echo;
    echoA(echo, 0.5, 0.6f);
    const Buf plain = render(echo, in, t);   // dry + wet
    CHECK(maxDiff(plain, in) > 0.2);
    const Buf wet = minus(plain, in);

    Pump::Params grid;
    grid.depth = 0.9f;
    grid.rateBeats = 0.5;
    grid.length = 0.5f;
    Pump::Params input;
    input.trigger = Pump::kInput;
    input.curve = gf::PC_HARD;
    input.depth = 1.0f;
    input.length = 0.3f;
    Pump::Params lows;
    lows.band = Pump::kLows;
    lows.crossoverHz = 150.0f;
    lows.curve = gf::PC_HARD;
    lows.depth = 1.0f;
    lows.rateBeats = 1.0;
    for (const Pump::Params* pp : {&grid, &input, &lows}) {
        const Buf* key = pp->trigger == Pump::kInput ? &in : nullptr;   // the chain's input
        P all = echo;
        all.pumpOn = true;
        all.pump = *pp;
        all.pumpTarget = Groove::kAll;
        const Buf got = render(all, in, t);
        CHECK(maxDiff(got, pumpAlone(*pp, plain, key, t)) < 2e-5);   // as if on the sum
        CHECK(maxDiff(got, plain) > 0.05);                           // (it ducked)
        CHECK(maxDiff(got, plus(pumpAlone(*pp, in, key, t), wet)) > 0.05);   // (and not the dry alone)

        P onDry = all;
        onDry.pumpTarget = Groove::kDry;
        CHECK(maxDiff(render(onDry, in, t), plus(pumpAlone(*pp, in, key, t), wet)) < 2e-5);   // the dry ducked, the wet as it was

        P onEcho = all;
        onEcho.pumpTarget = Groove::kEcho;
        CHECK(maxDiff(render(onEcho, in, t), plus(in, pumpAlone(*pp, wet, key, t))) < 2e-5);  // the wet ducked, the dry as it was
    }

    // The Shape, every lane: the same.
    Shape::Params sp;
    sp.lane[Shape::kVolume] = {true, shapeIdx("Square"), 1.0, 0.7f, 0.0f};
    sp.lane[Shape::kFilter] = {true, shapeIdx("Sine"), 2.0, 1.0f, 0.0f};
    sp.lane[Shape::kPan] = {true, shapeIdx("Triangle"), 4.0, 0.5f, 0.0f};
    sp.cutoffHz = 800.0f;
    sp.resonance = 0.6f;
    P sh = echo;
    sh.shapeOn = true;
    sh.shape = sp;
    sh.shapeTarget = Groove::kAll;
    CHECK(maxDiff(render(sh, in, t), shapeAlone(sp, plain, t)) < 2e-5);
    CHECK(maxDiff(render(sh, in, t), plain) > 0.05);
    P shDry = sh, shEcho = sh;
    shDry.shapeTarget = Groove::kDry;
    shEcho.shapeTarget = Groove::kEcho;
    CHECK(maxDiff(render(shDry, in, t), plus(shapeAlone(sp, in, t), wet)) < 2e-5);
    CHECK(maxDiff(render(shEcho, in, t), plus(in, shapeAlone(sp, wet, t))) < 2e-5);

    // Both, Shape then Pump, on All.
    P both = sh;
    both.pumpOn = true;
    both.pump = grid;
    both.pumpTarget = Groove::kAll;
    CHECK(maxDiff(render(both, in, t), pumpAlone(grid, shapeAlone(sp, plain, t), nullptr, t)) < 4e-5);
}

// Both Pumps (the dry's and the wet's) come on in the same chunk when the part and the Echo are switched on
// together: with the Input trigger, which takes 60 ms to warm up, they begin together and duck alike, as one
// Pump begun there would on the sum. An Echo switched on later joins a Pump that has been running (the grid
// is MPC's, so the wet's Pump is on it from its first chunk).
void groovePumpWake() {
    const Buf in = kickPad(6, 4410);
    const size_t X = 16384;   // a chunk start
    const Transport t = playingAt(0.0);
    Pump::Params input;
    input.trigger = Pump::kInput;
    input.curve = gf::PC_HARD;
    input.depth = 1.0f;
    input.length = 0.3f;

    P p;
    echoA(p, 0.5, 0.6f);
    p.pump = input;
    auto together = [&](P& x, size_t at) { x.echoOn = x.pumpOn = at >= X; };
    auto echoOnly = [&](P& x, size_t at) { x.echoOn = at >= X; };
    const Played r = play(in, p, t, together);
    const Played sum = play(in, p, t, echoOnly);   // (the Pump is off in p)
    const double beatsAtX = static_cast<double>(X) / kBeat;
    const Buf keyFromX = part(in, X, in.size());
    const Buf want = pumpAlone(input, part(sum.out, X, sum.out.size()), &keyFromX, playingAt(beatsAtX));
    CHECK(maxDiff(part(r.out, X + 500, r.out.size()), part(want, 500, want.size())) < 2e-5);
    CHECK(maxDiff(part(r.out, X + 500, r.out.size()), part(sum.out, X + 500, sum.out.size())) > 0.05);   // (it ducked)

    // The Pump switched on while the Echo already sounds: the wet's instance begins with the dry's, in that chunk.
    {
        P e = p;
        e.echoOn = true;
        const Played onLater = play(in, e, t, [&](P& x, size_t at) { x.pumpOn = at >= X; });
        const Played all = play(in, e, t, [](P&, size_t) {});   // (the Pump off)
        const Buf wantLater = pumpAlone(input, part(all.out, X, all.out.size()), &keyFromX, playingAt(beatsAtX));
        CHECK(maxDiff(part(onLater.out, X + 500, onLater.out.size()), part(wantLater, 500, wantLater.size())) < 2e-5);
        CHECK(same(part(onLater.out, 0, X), part(all.out, 0, X)));
    }

    // The grid: the Pump on from the start, the Echo switched on at X.
    Pump::Params grid;
    grid.depth = 0.9f;
    grid.rateBeats = 0.5;
    grid.length = 0.5f;
    P g;
    echoA(g, 0.5, 0.6f);
    g.pump = grid;
    const Played gsum = play(in, g, t, echoOnly);
    g.pumpOn = true;
    const Played late = play(in, g, t, echoOnly);
    CHECK(maxDiff(part(late.out, X + 500, late.out.size()), part(pumpAlone(grid, gsum.out, nullptr, t), X + 500, late.out.size())) < 2e-5);
    CHECK(maxDiff(part(late.out, X + 500, late.out.size()), part(gsum.out, X + 500, gsum.out.size())) > 0.05);
}

// The same key, different audio: the Pump's gains depend on the key and the grid only. (Two instances, the
// dry's and the wet's, rely on it.)
void grooveSameKeyGains() {
    const int total = 4410 + 6 * kBeat;
    Buf key(static_cast<size_t>(total), 0.0f), other(static_cast<size_t>(total), 0.0f);
    for (int k = 0; k < 6; ++k) {
        addKick(key, 4410 + k * kBeat, 0.8f);
        addKick(other, 4410 + k * kBeat + 3000, 0.8f);
    }
    Pump::Params pp;
    pp.trigger = Pump::kInput;
    pp.curve = gf::PC_HARD;
    pp.depth = 1.0f;
    pp.length = 0.3f;
    const Buf dcA = dc(total, 1.0f), dcB = dc(total, -0.37f);
    const Buf tone = sine(330.0, total, 0.9f);
    auto gains = [&](const Buf& audio, const Buf& k) {
        const Buf out = pumpAlone(pp, audio, &k, transport(120.0, 0.0, false));
        Buf g(audio.size(), -1.0f);
        for (size_t i = 0; i < audio.size(); ++i)
            if (std::fabs(audio[i]) > 0.05f) g[i] = out[i] / audio[i];
        return g;
    };
    const Buf ga = gains(dcA, key), gb = gains(dcB, key), gc = gains(tone, key);
    double worstB = 0.0, worstC = 0.0;
    int ducked = 0;
    for (size_t i = 0; i < ga.size(); ++i) {
        if (ga[i] < 0.5f) ++ducked;
        worstB = std::max(worstB, static_cast<double>(std::fabs(ga[i] - gb[i])));
        if (gc[i] >= 0.0f) worstC = std::max(worstC, static_cast<double>(std::fabs(ga[i] - gc[i])));
    }
    CHECK(worstB < 3e-7 && worstC < 3e-5);
    CHECK(ducked > 6 * 400);   // (it ducked, on every kick)
    // Another key is another duck.
    const Buf go = gains(dcA, other);
    double apart = 0.0;
    for (size_t i = 0; i < ga.size(); ++i) apart = std::max(apart, static_cast<double>(std::fabs(ga[i] - go[i])));
    CHECK(apart > 0.5);
}

// Echo last: the echo hears what Shape and Pump made (a gated signal echoes as a rhythm), and the targets are
// ignored, there being only the dry to move. Without parts the order changes nothing.
void grooveEchoOrder() {
    const Buf in = sine(220.0, 5 * kBeat, 0.4f);
    P first;
    echoA(first, 1.0, 0.0f);   // 1/4: one repeat, a beat later
    P last = first;
    last.order = Groove::kEchoLast;
    const Transport t = playingAt(0.2);   // the Pump's Hold is silent until sample 48510
    CHECK(same(render(first, in, t), render(last, in, t)));

    first.pumpOn = last.pumpOn = true;
    silencePump(first);
    silencePump(last);
    first.pumpTarget = Groove::kDry;    // the dry gated, the echo not
    last.pumpTarget = Groove::kEcho;    // (ignored: the dry is gated in Echo last)
    const Buf a = render(first, in, t), b = render(last, in, t);
    CHECK(peak(a, 0, 21000) == 0.0f);
    CHECK(peak(a, 23000, 47000) > 0.3f);       // the echo of the input as it was: not gated
    CHECK(peak(b, 0, 47000) < 1e-6f);          // the echo of the gated input: nothing
    // The echo of the gated dry is the Echo alone on what the Pump left: Echo last is the Pump, then the Echo.
    P echoOnly = first;
    echoOnly.pumpOn = false;
    CHECK(maxDiff(b, render(echoOnly, pumpAlone(last.pump, in, nullptr, t), t)) < 2e-5);
    // ... and Echo first (the Pump on the Dry) is the Pump on the dry and the echo of the input as it came.
    CHECK(maxDiff(a, plus(pumpAlone(first.pump, in, nullptr, t), minus(render(echoOnly, in, t), in))) < 2e-5);
    CHECK(maxDiff(part(b, 72000, 83000), part(a, 72000, 83000)) > 0.2);   // (they are not the same sound)
    // The Pump on the Echo, the order flipped while playing: the Pump's dry instance fades in as its wet instance
    // fades out (10 ms), and nothing clicks in the 4410 samples after it. (What the Echo hears when the order
    // flips is grooveOrderFlip's: the repeat of this 1/4 line comes after this window.)
    P gatedWet = first;
    gatedWet.pumpTarget = Groove::kEcho;   // first: the dry sounds and the echo is gated; last: the other way round
    auto flip = [](P& x, size_t at) { x.order = at >= 24576 ? Groove::kEchoLast : Groove::kEchoFirst; };
    const Played f = play(in, gatedWet, t, flip);
    CHECK(clickFree(f.out, 20000, 24576, 30000));
    CHECK(peak(f.out, 20000, 24000) > 0.3f);   // (the dry was heard; the echo gated)
    // After the flip the dry is gated and what the lines held is heard: a Pump that acts before the Echo does not reach back into it.
    CHECK(peak(f.out, 25100, 26000) > 0.3f);
    CHECK(allFinite(f.out));
}

// Ruling R50: an order change crossfades what the Echo hears over 10 ms. The Echo's source is the dry before Shape
// and Pump (Echo first) or after them (Echo last); with a part on the Dry or on All whose gain is not 1 when the
// order flips, a jump from one to the other steps the line, and the step is heard one delay later. A short line
// (5 ms) and a 1/16 one, a sine at 220 Hz held by a Shape gate or a Pump's Hold on the Dry and on All, flipped both
// ways: nothing in the window that covers the repeat is steeper than a sine of the amplitude and a 1 ms edge allow.
void grooveOrderFlip() {
    const size_t N = 40000, X = 8192;   // the flip
    const Buf in = sine(220.0, static_cast<int>(N), 0.4f);
    // The sine itself reaches a slope of 0.0125 and a gain edge of 1 ms on 0.4 is 0.009: more than 0.02 is a step.
    // (The steps this test was written against were 0.17 on the Dry and 0.09 on All; now none is over the sine's.)
    constexpr double kBound = 0.02;
    for (int variant = 0; variant < 4; ++variant)
        for (int line = 0; line < 2; ++line)
            for (int dir = 0; dir < 2; ++dir) {
                P p;
                if (line == 0) echoFree(p, 5.0f);
                else echoA(p, 0.25, 0.0f);
                const int target = (variant % 2) == 0 ? Groove::kDry : Groove::kAll;
                double beat = 2.2;
                if (variant < 2) {   // a Square on Volume: silent from beat 2 to 4
                    p.shapeOn = true;
                    Shape::LaneParams& q = p.shape.lane[Shape::kVolume];
                    q.on = true;
                    q.shape = shapeIdx("Square");
                    q.rateBeats = 4.0;
                    q.depth = 1.0f;
                    p.shapeTarget = target;
                } else {   // a Hold: silent from beat 0.2 to 2.4
                    silencePump(p);
                    p.pumpOn = true;
                    p.pumpTarget = target;
                    beat = 0.2;
                }
                const int from = dir == 0 ? Groove::kEchoFirst : Groove::kEchoLast, to = dir == 0 ? Groove::kEchoLast : Groove::kEchoFirst;
                auto flip = [&](P& x, size_t at) { x.order = at >= X ? to : from; };
                const Played r = play(in, p, playingAt(beat), flip);
                const size_t window = X + (line == 0 ? 6000 : 9000);   // the flip, the repeat, and the blend that follows it
                CHECK(steepest(r.out, X, window) <= kBound);
                CHECK(steepest(r.outR, X, window) <= kBound);   // (the right side is blended too)
                CHECK(allFinite(r.out));
            }
    // With no part to move the dry, the order changes nothing the Echo hears: the flip is invisible, bit for bit.
    {
        P p;
        echoFree(p, 5.0f);
        const Played still = play(in, p, playingAt(0.0), [](P&, size_t) {});
        for (int dir = 0; dir < 2; ++dir) {
            auto flip = [&](P& x, size_t at) { x.order = (at >= X) == (dir == 0) ? Groove::kEchoLast : Groove::kEchoFirst; };
            CHECK(same(play(in, p, playingAt(0.0), flip).out, still.out));
        }
    }
}

// Time Post bends the sum, so a tape stop stops the echoes too; Pre bends the input only and the echo rings on.
void grooveTimePlace() {
    const int stop = timePat("Tape Stop");
    const size_t S = kBeat, E = S + 4 * kBeat;   // the pattern starts on beat 1 and is a bar long
    const Buf in = sine(1000.0, static_cast<int>(E + 20000), 0.5f);
    P base;
    echoA(base, 1.0, 0.0f);   // one repeat, a beat later
    const Buf plain = render(base, in);
    base.timeOn = true;
    base.time.pattern = stop;
    base.time.patternStart = 1.0;
    P post = base, pre = base;
    post.timePlace = Groove::kPost;
    pre.timePlace = Groove::kPre;
    const Buf a = render(post, in), b = render(pre, in);
    CHECK(same(part(a, 0, S - 400), part(plain, 0, S - 400)) && same(part(b, 0, S - 400), part(plain, 0, S - 400)));   // before it: nothing
    CHECK(peak(a, E - 441, E - 60) < 0.01f);   // Post: the stop is the whole sound's, the echo included
    CHECK(peak(b, E - 441, E - 60) > 0.3f);    // Pre: the dry has stopped; the echo of what came before it rings on
    CHECK(allFinite(a) && allFinite(b));
}

// Swing once: straight steps when Time swings the audio after them, else the global Swing. The four cases of
// the rule, and the chains they stand for.
void grooveStepSwingRule() {
    P p;
    p.swing.amount = 0.66f;
    p.swing.unitBeats = 0.5;
    p.timeOn = true;
    p.time.groove = Time::kSwing;
    p.timePlace = Groove::kPost;
    CHECK(Groove::stepSwing(p).amount == 0.5f);                                  // Time on, Swing, Post: straight
    P pre = p;
    pre.timePlace = Groove::kPre;
    CHECK(Groove::stepSwing(pre).amount == 0.66f && Groove::stepSwing(pre).unitBeats == 0.5);    // Pre: the audio is swung first
    P drunk = p;
    drunk.time.groove = Time::kDrunk;
    CHECK(Groove::stepSwing(drunk).amount == 0.66f);                             // another groove: nothing swings the audio
    P off = p;
    off.timeOn = false;
    CHECK(Groove::stepSwing(off).amount == 0.66f);                               // Time off: the global Swing
    CHECK(Groove::stepSwing(P()).amount == 0.5f);

    // The chains. A Pump on a steady input at 1/16, the Swing 66% on 1/16; Time's Groove swings the audio.
    const Buf in = dc(2 * kBeat, 0.5f);
    Pump::Params pp;
    pp.depth = 1.0f;
    pp.rateBeats = 0.25;
    pp.length = 0.3f;
    Time::Params tp;
    tp.groove = Time::kSwing;
    tp.swing = Swing{0.66f, 0.25};
    const Swing straight{0.5f, 0.25}, swung{0.66f, 0.25};
    // By hand: the Pump's steps swung by pumpSwing, Time before or after it.
    auto chain = [&](const Swing& pumpSwing, bool timeFirst) {
        Buf L = in, R = in;
        Pump m;
        Time tm;
        CHECK(tm.allocate());
        Pump::Params q = pp;
        q.swing = pumpSwing;
        Transport tr = playingAt(0.0);
        for (size_t pos = 0; pos < L.size(); pos += kChunk) {
            const int n = static_cast<int>(std::min(static_cast<size_t>(kChunk), L.size() - pos));
            m.set(q, tr);
            tm.set(tp, tr);
            if (timeFirst) {
                tm.process(&L[pos], &R[pos], n);
                m.process(&L[pos], &R[pos], n);
            } else {
                m.process(&L[pos], &R[pos], n);
                tm.process(&L[pos], &R[pos], n);
            }
            tr.beats += n / static_cast<double>(kRate) * tr.bpm / 60.0;
        }
        return L;
    };
    P q;
    q.pumpOn = true;
    q.pump = pp;
    q.timeOn = true;
    q.time = tp;
    q.swing = swung;
    q.timePlace = Groove::kPost;
    const Buf post = render(q, in);
    CHECK(same(post, chain(straight, false)));                   // Post: straight steps, then Time swings the lot
    CHECK(maxDiff(post, chain(swung, false)) > 0.1);              // (swung twice is another sound)
    q.timePlace = Groove::kPre;
    const Buf prev = render(q, in);
    CHECK(same(prev, chain(swung, true)));                       // Pre: Time swings the input, the steps swing to meet it
    CHECK(maxDiff(prev, chain(straight, true)) > 0.1);
}

// R26: the key is the chain's input after Time when Time is Pre. The Pump's Input trigger hears the kicks
// where the dry plays them.
void grooveKeyAfterTime() {
    const Buf in = kickPad(6, 4410);
    const Transport t = playingAt(0.0);
    Pump::Params pp;
    pp.trigger = Pump::kInput;
    pp.curve = gf::PC_HARD;
    pp.depth = 1.0f;
    pp.length = 0.3f;
    Time::Params tp;
    tp.groove = Time::kPush;
    tp.amount = 1.0f;   // 20 ms late
    P p;
    p.pumpOn = true;
    p.pump = pp;
    p.pumpTarget = Groove::kDry;
    p.timeOn = true;
    p.time = tp;
    p.timePlace = Groove::kPre;
    const Buf got = render(p, in, t);

    auto byHand = [&](bool keyAfterTime) {
        Buf L = in, R = in;
        Time tm;
        CHECK(tm.allocate());
        Pump m;
        Transport tr = t;
        for (size_t pos = 0; pos < L.size(); pos += kChunk) {
            const int n = static_cast<int>(std::min(static_cast<size_t>(kChunk), L.size() - pos));
            const size_t bytes = sizeof(float) * static_cast<size_t>(n);
            float kl[kChunk], kr[kChunk];
            if (!keyAfterTime) {
                std::memcpy(kl, &in[pos], bytes);
                std::memcpy(kr, &in[pos], bytes);
            }
            tm.set(tp, tr);
            tm.process(&L[pos], &R[pos], n);
            if (keyAfterTime) {
                std::memcpy(kl, &L[pos], bytes);
                std::memcpy(kr, &R[pos], bytes);
            }
            m.set(pp, tr);
            m.process(&L[pos], &R[pos], n, kl, kr);
            tr.beats += n / static_cast<double>(kRate) * tr.bpm / 60.0;
        }
        return L;
    };
    // (R61: after a reset or at the start, Time warms up: the input passes for the lag, a crossfade and a chunk. The hand-built Time
    // asks for its lag at once, so the two are compared from where the groove is in.)
    const size_t warm = 882 + 2 * (gf::TimeCore::kFade + kChunk);   // the warm-up, then Time's own crossfade into the groove
    CHECK(same(part(got, warm, got.size()), part(byHand(true), warm, got.size())));
    CHECK(maxDiff(got, byHand(false)) > 0.1);   // (hearing the kick 20 ms early is another duck)
    // The key is taken before every part, Shape's too: a kick that Shape's gate is holding back is still heard
    // by the Pump, and its duck is still going when the gate opens (bar 2 begins at beat 4 = sample 88200).
    {
        Buf kicked = sine(330.0, 6 * kBeat, 0.25f);
        addKick(kicked, 87097, 0.8f);   // beat 3.95, in the gate's closed half
        P q;
        q.shapeOn = true;
        Shape::LaneParams& v = q.shape.lane[Shape::kVolume];
        v.on = true;
        v.shape = shapeIdx("Square");
        v.rateBeats = 4.0;
        v.depth = 1.0f;
        q.shapeTarget = Groove::kDry;
        q.pumpOn = true;
        q.pump = pp;
        q.pump.length = 0.5f;
        q.pumpTarget = Groove::kDry;
        const Buf g = render(q, kicked, t);
        auto byHandShape = [&](bool keyBeforeShape) {
            Buf L = kicked, R = kicked;
            Shape sh;
            Pump m;
            Transport tr = t;
            for (size_t pos = 0; pos < L.size(); pos += kChunk) {
                const int n = static_cast<int>(std::min(static_cast<size_t>(kChunk), L.size() - pos));
                float kl[kChunk];
                if (keyBeforeShape) std::memcpy(kl, &kicked[pos], sizeof(float) * static_cast<size_t>(n));
                sh.set(q.shape, tr);
                sh.process(&L[pos], &R[pos], n);
                if (!keyBeforeShape) std::memcpy(kl, &L[pos], sizeof(float) * static_cast<size_t>(n));
                m.set(q.pump, tr);
                m.process(&L[pos], &R[pos], n, kl, kl);
                tr.beats += n / static_cast<double>(kRate) * tr.bpm / 60.0;
            }
            return L;
        };
        CHECK(same(g, byHandShape(true)));
        CHECK(maxDiff(g, byHandShape(false)) > 0.05);   // (a Pump that heard only what Shape let through would duck elsewhere)
    }
    // Time Post: the key is the input, there is nothing before it.
    P post = p;
    post.timePlace = Groove::kPost;
    P none = post;
    none.timeOn = false;
    CHECK(maxDiff(render(post, in, t), render(none, in, t)) > 0.1);   // (Time moves the ducked audio)
    CHECK(allFinite(render(post, in, t)));
}

// The Echo's LFO steps are Echo steps like Shape's and Pump's: they follow stepSwing, so with Time swinging the sum
// after them (Post) they stay straight and Time moves them with the hits.
void grooveStepSwingEcho() {
    const Buf in = sine(220.0, 3 * kBeat, 0.4f);
    const Swing swung{0.66f, 0.25}, straight{0.5f, 0.25};
    Time::Params tp;
    tp.groove = Time::kSwing;
    tp.swing = swung;
    P p;
    echoA(p, 0.5, 0.5f);
    Echo::Line& l = p.echo.line[0];
    l.mod.mode = gf::EchoMod::kLfo;
    l.mod.shape = shapeIdx("Off 16ths");
    l.mod.rateBeats = 4.0;
    l.modLevel = 1.0f;
    p.swing = swung;
    p.timeOn = true;
    p.time = tp;
    p.timePlace = Groove::kPost;
    const Buf got = render(p, in);

    // By hand: the Echo with the steps' swing given, then the sum through Time.
    auto byHand = [&](const Swing& echoSwing) {
        Buf L = in, R = in;
        Echo e;
        CHECK(e.allocate());
        Time tm;
        CHECK(tm.allocate());
        Echo::Params ep = p.echo;
        ep.swing = echoSwing;
        Transport tr = playingAt(0.0);
        float wl[kChunk], wr[kChunk];
        for (size_t pos = 0; pos < L.size(); pos += kChunk) {
            const int n = static_cast<int>(std::min(static_cast<size_t>(kChunk), L.size() - pos));
            e.set(ep, tr);
            e.process(&L[pos], &R[pos], &L[pos], &R[pos], wl, wr, n);   // (the key is the input)
            for (int i = 0; i < n; ++i) {
                L[pos + static_cast<size_t>(i)] += wl[i];
                R[pos + static_cast<size_t>(i)] += wr[i];
            }
            tm.set(tp, tr);
            tm.process(&L[pos], &R[pos], n);
            tr.beats += n / static_cast<double>(kRate) * tr.bpm / 60.0;
        }
        return L;
    };
    CHECK(same(got, byHand(straight)));
    CHECK(maxDiff(got, byHand(swung)) > 0.05);   // (the swing is heard)
    // Time off: the steps swing by the global Swing.
    P off = p;
    off.timeOn = false;
    Buf L = in, R = in;
    {
        Echo e;
        CHECK(e.allocate());
        Echo::Params ep = p.echo;
        ep.swing = swung;
        Transport tr = playingAt(0.0);
        float wl[kChunk], wr[kChunk];
        for (size_t pos = 0; pos < L.size(); pos += kChunk) {
            const int n = static_cast<int>(std::min(static_cast<size_t>(kChunk), L.size() - pos));
            e.set(ep, tr);
            e.process(&L[pos], &R[pos], &L[pos], &R[pos], wl, wr, n);
            for (int i = 0; i < n; ++i) L[pos + static_cast<size_t>(i)] += wl[i];
            tr.beats += n / static_cast<double>(kRate) * tr.bpm / 60.0;
        }
    }
    CHECK(same(render(off, in), L));
}

// Ruling R48: an instance that begins while its twin on the other bus has been running begins as a copy of it, so
// the two duck (or move) alike from the first sample, whatever the trigger and the transport. The Echo is switched
// on in the middle of a duck: the wet's Pump is where the dry's is, and the Groove is the part alone on the sum.
void grooveWetStartsWhereDryIs() {
    const Buf in = kickPad(6, 4410);
    const size_t X = 26560, N = in.size();   // a chunk start, just after the kick at 26460: the duck is going on
    P base;
    echoFree(base, 3.0f);
    base.echo.line[0].p.feedback = 0.6f;
    auto echoLate = [&](P& x, size_t at) { x.echoOn = at >= X; };
    Pump::Params input;
    input.trigger = Pump::kInput;
    input.curve = gf::PC_HARD;
    input.depth = 1.0f;
    input.length = 0.3f;
    Pump::Params lows = input;
    lows.band = Pump::kLows;
    lows.crossoverHz = 150.0f;
    Pump::Params grid;
    grid.depth = 0.9f;
    grid.rateBeats = 0.5;
    grid.length = 0.5f;
    Shape::Params sp;
    sp.lane[Shape::kVolume] = {true, shapeIdx("Square"), 1.0, 0.7f, 0.0f};
    sp.lane[Shape::kFilter] = {true, shapeIdx("Sine"), 2.0, 1.0f, 0.0f};
    sp.lane[Shape::kPan] = {true, shapeIdx("Triangle"), 4.0, 0.5f, 0.0f};
    sp.cutoffHz = 800.0f;
    sp.resonance = 0.6f;

    for (const Transport& t : {playingAt(0.0), transport(120.0, 0.0, false)}) {
        const Played sum = play(in, base, t, echoLate);   // dry + wet, the part off
        CHECK(peak(minus(part(sum.out, X + 200, X + 1500), part(in, X + 200, X + 1500))) > 0.05f);   // (the wet is there at once)
        for (const Pump::Params* pp : {&input, &lows, &grid}) {
            const Buf* key = pp->trigger == Pump::kInput ? &in : nullptr;
            P p = base;
            p.pumpOn = true;
            p.pump = *pp;
            p.pumpTarget = Groove::kAll;
            const Played got = play(in, p, t, echoLate);
            const Buf want = pumpAlone(*pp, sum.out, key, t);
            CHECK(maxDiff(part(got.out, X, N), part(want, X, N)) < 2e-5);        // from the first sample after the switch
            CHECK(maxDiff(part(got.out, X, N), part(sum.out, X, N)) > 0.05);     // (and it ducks)
            CHECK(maxDiff(part(got.out, X, X + 600), part(sum.out, X, X + 600)) > 0.01);   // (including right there)
        }
        P sh = base;
        sh.shapeOn = true;
        sh.shape = sp;
        sh.shapeTarget = Groove::kAll;
        const Played got = play(in, sh, t, echoLate);
        CHECK(maxDiff(part(got.out, X, N), part(shapeAlone(sp, sum.out, t), X, N)) < 2e-5);
        CHECK(maxDiff(part(got.out, X, N), part(sum.out, X, N)) > 0.05);

        // A part's target moved onto the bus the part was not on, with the echo sounding: Dry to All (the wet's
        // instance begins), Echo to All (the dry's begins). Once the 10 ms fade-in is over they are one Pump (Shape) on
        // the sum. (A Shape's filter starts with nothing in it, on a signal that is already going: it rings for a while
        // before it is the filter on the sum, so this one has the gains of the Volume and Pan lanes only.)
        Shape::Params gains = sp;
        gains.lane[Shape::kFilter].on = false;
        P both = base;
        both.echoOn = true;
        const Played all = play(in, both, t, [](P&, size_t) {});   // dry + wet from the start
        for (int from : {Groove::kDry, Groove::kEcho}) {
            auto move = [&](P& x, size_t at) { x.pumpTarget = at >= X ? Groove::kAll : from; };
            P p = both;
            p.pumpOn = true;
            p.pump = input;
            const Played got = play(in, p, t, move);
            CHECK(maxDiff(part(got.out, X + 500, N), part(pumpAlone(input, all.out, &in, t), X + 500, N)) < 2e-5);
            P q = both;
            q.shapeOn = true;
            q.shape = gains;
            auto moveShape = [&](P& x, size_t at) { x.shapeTarget = at >= X ? Groove::kAll : from; };
            const Played gs = play(in, q, t, moveShape);
            CHECK(maxDiff(part(gs.out, X + 500, N), part(shapeAlone(gains, all.out, t), X + 500, N)) < 2e-5);
            CHECK(maxDiff(part(gs.out, X + 500, N), part(all.out, X + 500, N)) > 0.05);
        }
    }
}

// Ruling R42: while Time's ring is allocated the Groove feeds it the audio at Time's place even when Time is off
// (record only: the output is untouched), so a Move that reads the past finds it. Never allocated: no work.
void grooveTimeListens() {
    const Buf noise = whiteNoise(6 * kBeat, 0.4f, 11);
    const Transport t = playingAt(0.0);
    // Off and allocated: bit for bit, listening, no part runs. Not allocated: nothing listens.
    for (int place = 0; place < 2; ++place) {
        P p;
        p.time.groove = Time::kPush;
        p.time.amount = 1.0f;
        p.time.pattern = timePat("Reverse Beat");
        p.time.patternStart = 1.0;
        p.timePlace = place;
        Groove g;
        Buf L = noise, R = noise;
        Transport tr = t;
        bool never = true, listens = true, idle = true;
        for (size_t pos = 0; pos < L.size(); pos += kChunk) {
            const int n = static_cast<int>(std::min(static_cast<size_t>(kChunk), L.size() - pos));
            if (pos == 8192) CHECK(g.allocateTime());
            g.set(p, tr);
            g.process(&L[pos], &R[pos], n);
            if (pos < 8192) never = never && !g.listening();
            else listens = listens && g.listening();
            idle = idle && g.running() == 0 && g.tailSamples() == 0;
            tr.beats += n / static_cast<double>(kRate) * tr.bpm / 60.0;
        }
        CHECK(never && listens && idle);
        CHECK(same(L, noise) && same(R, noise));
    }
    // Time on is not listening, it plays; off again, it listens once it has faded out.
    {
        P p;
        p.time.groove = Time::kPush;
        p.time.amount = 1.0f;
        Groove g;
        CHECK(g.allocateTime());
        Buf L = noise, R = noise;
        Transport tr = t;
        bool ok = true;
        for (size_t pos = 0; pos < 20480; pos += kChunk) {
            p.timeOn = pos >= 4096 && pos < 12288;
            g.set(p, tr);
            g.process(&L[pos], &R[pos], kChunk);
            if (pos < 4096 || pos >= 12288 + 2 * kChunk + 441) ok = ok && g.listening();
            else if (pos >= 4096 && pos < 12288 + 441) ok = ok && !g.listening();
            tr.beats += kChunk / static_cast<double>(kRate) * tr.bpm / 60.0;
        }
        CHECK(ok);
    }

    // Reverse Beat two seconds on: Time is switched on a tenth of a beat before the Move starts (beat 3) and plays the
    // beat before it, backwards, as a Time that had been on all along would. With no past (the memory arriving at the
    // switch) there would be nothing to play.
    {
        const int rev = timePat("Reverse Beat");
        const size_t S = 3 * kBeat, on = 63936;   // the Move's start; the switch, a chunk start 2214 samples before it
        P p;
        p.time.pattern = rev;
        p.time.patternStart = 3.0;
        auto switchOn = [&](P& x, size_t at) { x.timeOn = at >= on; };
        P all = p;
        all.timeOn = true;
        const Played listened = playLate(noise, p, t, switchOn, 0);
        const Played ref = playLate(noise, all, t, [](P&, size_t) {}, 0);
        const Played cold = playLate(noise, p, t, switchOn, on);
        CHECK(same(part(listened.out, 0, on), part(noise, 0, on)));
        CHECK(maxDiff(part(listened.out, S, S + kBeat + 1000), part(ref.out, S, S + kBeat + 1000)) < 1e-6);
        CHECK(peak(listened.out, S + 5000, S + kBeat - 1000) > 0.1f);   // the beat before, not silence
        CHECK(peak(cold.out, S + 5000, S + kBeat - 1000) < 0.01f);      // (with no past it is)
    }

    // The ring is cleared when Time's place moves while it rests (it holds the other place's signal): switched on
    // right after, Time waits for its groove's history; with the place unmoved it engages at once.
    {
        const Buf s = sine(110.0, 24576, 0.5f);
        P p;
        p.time.groove = Time::kPush;
        p.time.amount = 1.0f;
        const size_t X = 8192;
        auto moved = [&](P& x, size_t at) {
            x.timeOn = at >= X;
            x.timePlace = at >= X - 64 ? Groove::kPre : Groove::kPost;
        };
        auto stayed = [&](P& x, size_t at) { x.timeOn = at >= X; };
        const Played a = playLate(s, p, t, moved, 0), b = playLate(s, p, t, stayed, 0);
        CHECK(same(part(a.out, X, X + 1000), part(s, X, X + 1000)));    // an empty ring: the input passes
        CHECK(!same(part(b.out, X, X + 1000), part(s, X, X + 1000)));   // the past is there: it pushes at once
        CHECK(clickFree(a.out, X - 4096, X, X + 4096) && clickFree(b.out, X - 4096, X, X + 4096));
    }
}

// Rumble: a Pump on the Echo with the Input trigger ducks the repeats at the dry's kicks, and not at the
// repeats' own onsets.
void grooveRumbleKey() {
    const int lead = 4410, beats = 8;
    Buf in(static_cast<size_t>(lead + beats * kBeat), 0.0f);
    for (int k = 0; k < beats; ++k) addKick(in, lead + k * kBeat, 0.8f);
    const Transport t = transport(120.0, 0.0, false);
    P p;
    echoA(p, 0.5, 0.5f);   // a repeat half a beat after each kick, and one on the next kick
    const Buf plain = render(p, in, t);
    const Buf wet = minus(plain, in);
    P rumble = p;
    rumble.pumpOn = true;
    rumble.pump.trigger = Pump::kInput;
    rumble.pump.curve = gf::PC_HARD;
    rumble.pump.depth = 1.0f;
    rumble.pump.length = 0.3f;
    rumble.pumpTarget = Groove::kEcho;
    const Buf got = render(rumble, in, t);
    const Buf ducked = minus(got, in);

    // How much of the wet is left at the kick, and at the repeat half a beat after: the window from 6 to 30 ms in.
    auto share = [&](const Buf& w, const Buf& ref, int at) {
        double a = 0.0, b = 0.0;
        for (int i = at + 6 * 44; i < at + 30 * 44 && i < static_cast<int>(w.size()); ++i) {
            a += static_cast<double>(w[static_cast<size_t>(i)]) * w[static_cast<size_t>(i)];
            b += static_cast<double>(ref[static_cast<size_t>(i)]) * ref[static_cast<size_t>(i)];
        }
        return b > 1e-9 ? std::sqrt(a / b) : -1.0;
    };
    // The same Pump on the wet alone, with no key: it hears the repeats.
    const Buf selfFed = pumpAlone(rumble.pump, wet, nullptr, t);
    int tested = 0;
    for (int k = 2; k < beats - 1; ++k) {
        const int kick = lead + k * kBeat, repeat = kick + kBeat / 2;
        const double onKick = share(ducked, wet, kick), onRepeat = share(ducked, wet, repeat);
        CHECK(onKick >= 0.0 && onKick < 0.4);        // the repeat on the kick is ducked (to a third, 6 to 30 ms into the Hard curve)
        CHECK(onRepeat > 0.98 && onRepeat < 1.02);   // the repeat half a beat later is not: it is not a kick of the key
        CHECK(share(selfFed, wet, repeat) < 0.3);    // (a Pump listening to the wet would duck it)
        ++tested;
    }
    CHECK(tested == beats - 3);
    CHECK(maxDiff(part(got, 0, lead + kBeat / 2 - 100), part(in, 0, lead + kBeat / 2 - 100)) < 1e-9);   // the dry, with nothing from the echo yet

    // The dry's level does not touch the key: Rumble goes on with the dry off.
    P off = rumble;
    off.dry = 0.0f;
    CHECK(maxDiff(render(off, in, t), ducked) < 2e-6);
}

// The dry level (R29): 0 leaves only the wet, the send and the key are before it, the mix's input is not
// scaled, and it glides.
void grooveDry() {
    const Buf in = sine(220.0, 3 * kBeat, 0.4f);
    P echo;
    echoA(echo, 0.5, 0.5f);
    const Buf full = render(echo, in);
    const Buf wet = minus(full, in);
    P gone = echo;
    gone.dry = 0.0f;
    CHECK(maxDiff(render(gone, in), wet) < 1e-6);
    CHECK(peak(render(gone, in), 0, 11000) == 0.0f);   // the echo has not come yet
    CHECK(peak(render(gone, in), 12000, 40000) > 0.2f);
    P half = echo;
    half.dry = 0.5f;
    double worst = 0.0;
    const Buf h = render(half, in);
    for (size_t i = 0; i < in.size(); ++i) worst = std::max(worst, std::fabs(static_cast<double>(h[i]) - (0.5 * in[i] + wet[i])));
    CHECK(worst < 1e-6);
    // Without the Echo it scales the dry (nothing is left when the dry is down).
    P quiet;
    quiet.dry = 0.0f;
    CHECK(peak(render(quiet, in), 0, in.size()) == 0.0f);
    quiet.dry = 0.25f;
    worst = 0.0;
    const Buf qd = render(quiet, in);
    for (size_t i = 0; i < in.size(); ++i) worst = std::max(worst, std::fabs(static_cast<double>(qd[i]) - 0.25 * in[i]));
    CHECK(worst < 1e-6);
    // Out of range: held to 0..1; a number that is not one is 1.
    P hi = echo, lo = echo, nan = echo;
    hi.dry = 7.0f;
    lo.dry = -3.0f;
    nan.dry = std::numeric_limits<float>::quiet_NaN();
    CHECK(same(render(hi, in), full) && same(render(nan, in), full) && maxDiff(render(lo, in), wet) < 1e-6);
    // The mix is against the input as it came in: at 0 it is that input, with the dry down and the echo on.
    P m0 = gone;
    m0.mix = 0.0f;
    CHECK(same(render(m0, in), in));
    // A glide, no faster than 1 ms for a full scale.
    const Buf steady = dc(8192, 1.0f);
    P g;
    auto edit = [](P& x, size_t at) { x.dry = at >= 2048 ? 0.0f : 1.0f; };
    const Played r = play(steady, g, playingAt(0.0), edit);
    CHECK(steepest(r.out, 0, r.out.size()) <= kEdge + 1e-5);
    CHECK(r.out.back() == 0.0f && r.out[2048 + 8] > 0.5f && r.out[2048 + 8] < 1.0f);   // (a glide, not a jump)
}

// Review focus 3: the Echo and Time switched on for the first time while playing. The memory comes from the
// UI thread, before the switch or after it: the output is the same either way, bit for bit, from the same chunk;
// the part fades in over 10 ms, no click; before its memory has arrived it is silent and costs nothing.
void firstSwitchOn() {
    const size_t X = 8192;   // the switch
    const Transport t = playingAt(0.0);
    // The Echo: a sine, an echo 2 ms behind it. The wet is the sine again at the fade's gain.
    {
        const Buf s = sine(1000.0, 24576, 0.5f);
        P p;
        echoFree(p, 2.0f);
        auto switchOn = [&](P& x, size_t at) { x.echoOn = at >= X; };
        auto always = [](P&, size_t) {};
        const Played before = playLate(s, p, t, switchOn, 0);    // the memory first, the switch at X
        const Played late = playLate(s, p, t, always, X);        // switched on from the start, the memory at X
        const Played later = playLate(s, p, t, always, X + 96);  // ... three chunks after the switch
        CHECK(same(before.out, late.out));
        CHECK(same(part(before.out, 0, X), part(s, 0, X)) && same(part(late.out, 0, X + 64), part(s, 0, X + 64)));
        for (size_t c = 0; c < X / kChunk; ++c) CHECK(before.running[c] == 0 && late.running[c] == 0);   // costs nothing until then
        CHECK(before.running[X / kChunk] == 1 && late.running[X / kChunk] == 1);
        CHECK(same(part(later.out, 0, X + 96), part(s, 0, X + 96)));
        CHECK(later.running[X / kChunk] == 0 && later.running[X / kChunk + 3] == 1);
        CHECK(wetFade(s, before.out, 1000.0, 0.5f, 88.2, X, true));
        CHECK(peak(minus(part(before.out, X + 800, 24576), part(s, X + 800, 24576))) > 0.45f);   // the echo is there
        CHECK(allFinite(before.out) && allFinite(later.out));
        // ... and after the late memory it fades in just the same
        CHECK(wetFade(s, later.out, 1000.0, 0.5f, 88.2, X + 96, true));
    }
    // Time: Push, 20 ms late. A low sine: the fade is a crossfade between the input and the same tone later. Once
    // allocated, the ring listens whether Time is on or not (R42), so a Time that is switched on finds the past and
    // engages its groove at once; one whose memory arrives with the switch has an empty ring, and waits (warms up).
    {
        const Buf s = sine(110.0, 24576, 0.5f);
        P p;
        p.time.groove = Time::kPush;
        p.time.amount = 1.0f;
        auto switchOn = [&](P& x, size_t at) { x.timeOn = at >= X; };
        auto always = [](P&, size_t) {};
        const Played before = playLate(s, p, t, switchOn, 0);   // the memory first, the ring listening, the switch at X
        P q = p;
        q.timeOn = true;
        const Played late = playLate(s, q, t, always, X);       // on from the start, the memory at X: an empty ring
        CHECK(same(part(before.out, 0, X), part(s, 0, X)) && same(part(late.out, 0, X), part(s, 0, X)));
        for (size_t c = 0; c < X / kChunk; ++c) CHECK(before.running[c] == 0 && late.running[c] == 0);
        CHECK(before.running[X / kChunk] == 1 && late.running[X / kChunk] == 1);
        CHECK(clickFree(before.out, X - 4096, X, X + 4096) && clickFree(late.out, X - 4096, X, X + 4096));
        CHECK(!same(part(before.out, X, X + 1100), part(s, X, X + 1100)));   // it pushes at once ...
        CHECK(same(part(late.out, X, X + 1100), part(s, X, X + 1100)));      // ... and this one passes the input until the ring holds the 20 ms
        CHECK(maxDiff(part(before.out, X + 2000, 24576), part(s, X + 2000, 24576)) > 0.3);
        CHECK(maxDiff(part(late.out, X + 2000, 24576), part(s, X + 2000, 24576)) > 0.3);
        // Neither drops out where the lag would read what was never recorded.
        bool noHole = true, noHoleLate = true;
        for (size_t w = X; w < X + 3000; w += 100) {
            noHole = noHole && peak(before.out, w, w + 401) > 0.3f;
            noHoleLate = noHoleLate && peak(late.out, w, w + 401) > 0.3f;
        }
        CHECK(noHole && noHoleLate);
        CHECK(maxDiff(part(before.out, X + 3000, 24576), part(late.out, X + 3000, 24576)) < 1e-5);   // and then they are the same
        CHECK(allFinite(before.out) && allFinite(late.out));
    }
    // Everything at once, with the Pump and the Shape on All, and the Echo's order and Time's place in every combination.
    {
        const Buf s = kickPad(3, 4410);
        for (int order = 0; order < 2; ++order)
            for (int place = 0; place < 2; ++place) {
                P p;
                echoA(p, 0.5, 0.5f);
                p.order = order;
                p.timePlace = place;
                p.time.groove = Time::kPush;
                p.time.amount = 1.0f;
                p.pump.depth = 0.8f;
                p.pump.trigger = Pump::kInput;
                p.shape.lane[Shape::kVolume] = {true, shapeIdx("Square"), 0.5, 0.5f, 0.0f};
                auto switchOn = [&](P& x, size_t at) { x.echoOn = x.timeOn = x.pumpOn = x.shapeOn = at >= X; };
                auto always = [&](P& x, size_t) { x.echoOn = x.timeOn = x.pumpOn = x.shapeOn = true; };
                const Played a = playLate(s, p, t, switchOn, 0);
                const Played b = playLate(s, p, t, always, X);
                // (the parts that need no memory are on from the start in b, so only the memory's arrival is compared: from X on)
                CHECK(allFinite(a.out) && allFinite(b.out));
                CHECK(a.running[X / kChunk + 1] == 4);
                CHECK(b.running[X / kChunk + 1] == 4);
                CHECK(clickFree(a.out, X - 2048, X, X + 4096));
                CHECK(clickFree(b.out, X - 2048, X, X + 4096));   // (the memory arriving while the parts are already on)
            }
    }
}

// The Echo's switch: the wet fades over 10 ms either way, the part costs nothing once faded out, and it comes
// back clean (the repeats of before the switch are not heard).
void grooveEchoSwitch() {
    const Buf s = sine(1000.0, 49152, 0.5f);
    P p;
    echoFree(p, 2.0f);
    auto window = [](P& x, size_t at) { x.echoOn = at >= 4096 && at < 12288; };
    const Played r = play(s, p, playingAt(0.0), window);
    CHECK(wetFade(s, r.out, 1000.0, 0.5f, 88.2, 4096, true));
    CHECK(wetFade(s, r.out, 1000.0, 0.5f, 88.2, 12288, false));
    CHECK(same(part(r.out, 12288 + 500, 49152), part(s, 12288 + 500, 49152)));
    bool idle = true, busy = true;
    for (size_t c = 0; c < r.running.size(); ++c) {
        const size_t at = c * kChunk;
        if (at < 4096 || at >= 12288 + 441 + 2 * kChunk) idle = idle && r.running[c] == 0;
        else if (at < 12288 + 441) busy = busy && r.running[c] == 1;
    }
    CHECK(idle && busy);

    // Back from cleared state: long repeats, off for a while, on again == switched on there for the first time.
    {
        const Buf n = whiteNoise(60000, 0.4f, 9);
        P q;
        echoA(q, 0.25, 0.9f);
        auto again = [](P& x, size_t at) { x.echoOn = at < 12288 || at >= 30720; };
        auto never = [](P& x, size_t at) { x.echoOn = at >= 30720; };
        const Played a = play(n, q, playingAt(0.0), again), b = play(n, q, playingAt(0.0), never);
        CHECK(same(part(a.out, 30720, 60000), part(b.out, 30720, 60000)));
        CHECK(maxDiff(part(a.out, 0, 12288), part(n, 0, 12288)) > 0.1);
    }

    // The tail: while it rings, and none once it is faded out.
    {
        Groove g;
        P q;
        echoA(q, 0.5, 0.5f);
        CHECK(g.allocate(q));
        Buf L = part(s, 0, 4096), R = L;
        run(g, q, L, R, playingAt(0.0));
        CHECK(g.tailSamples() >= 11025);
        q.echoOn = false;
        L = part(s, 0, 2048);
        R = L;
        run(g, q, L, R, playingAt(0.0));
        CHECK(g.tailSamples() == 0 && g.running() == 0);
        // frozen: for ever
        q.echoOn = true;
        q.echo.freeze = true;
        L = part(s, 0, 4096);
        R = L;
        run(g, q, L, R, playingAt(0.0));
        CHECK(g.tailSamples() >= 1 << 20);
    }
}

// Time's switch and place: fades, nothing run once it is off, clean when back, and a move of the place goes
// out where it was and in where it is going.
void grooveTimeSwitch() {
    const Buf s = sine(110.0, 49152, 0.5f);
    P p;
    p.time.groove = Time::kPush;
    p.time.amount = 1.0f;
    auto window = [](P& x, size_t at) { x.timeOn = at >= 4096 && at < 12288; };
    const Played r = play(s, p, playingAt(0.0), window);
    CHECK(same(part(r.out, 0, 4096), part(s, 0, 4096)));
    CHECK(same(part(r.out, 12288 + 500, 49152), part(s, 12288 + 500, 49152)));
    CHECK(clickFree(r.out, 2048, 4096, 6144) && clickFree(r.out, 6144, 12288, 14336));
    CHECK(maxDiff(part(r.out, 6000, 12000), part(s, 6000, 12000)) > 0.3);
    bool idle = true, busy = true;
    for (size_t c = 0; c < r.running.size(); ++c) {
        const size_t at = c * kChunk;
        if (at < 4096 || at >= 12288 + 441 + 2 * kChunk) idle = idle && r.running[c] == 0;
        else if (at < 12288 + 441) busy = busy && r.running[c] == 1;
    }
    CHECK(idle && busy);
    // Back: the ring went on listening, so Time comes back with its past, as one that was allocated at the start and
    // first switched on there; one whose memory comes with the switch has an empty ring and waits.
    {
        auto again = [](P& x, size_t at) { x.timeOn = at < 12288 || at >= 30720; };
        auto never = [](P& x, size_t at) { x.timeOn = at >= 30720; };
        const Played a = play(s, p, playingAt(0.0), again);
        const Played b = playLate(s, p, playingAt(0.0), never, 0);
        const Played c = play(s, p, playingAt(0.0), never);
        CHECK(maxDiff(part(a.out, 30720, 49152), part(b.out, 30720, 49152)) < 1e-6);
        CHECK(same(part(c.out, 30720, 30720 + 1100), part(s, 30720, 30720 + 1100)));
        CHECK(maxDiff(part(c.out, 30720 + 3000, 49152), part(a.out, 30720 + 3000, 49152)) < 1e-5);
    }
    // The place: Pre to Post and back, always running, no click.
    {
        P q = p;
        q.timeOn = true;
        auto move = [](P& x, size_t at) { x.timePlace = (at >= 12288 && at < 24576) ? Groove::kPost : Groove::kPre; };
        const Played m = play(s, q, playingAt(0.0), move);
        CHECK(*std::min_element(m.running.begin(), m.running.end()) == 1);
        CHECK(clickFree(m.out, 8192, 12288, 16384) && clickFree(m.out, 20000, 24576, 28672));
        CHECK(allFinite(m.out));
    }
    // The tail: the lag of the groove, while it is on.
    {
        Groove g;
        P q = p;
        q.timeOn = true;
        CHECK(g.allocate(q));
        Buf L = part(s, 0, 4096), R = L;
        run(g, q, L, R, playingAt(0.0));
        CHECK(g.tailSamples() >= 882);
        q.timeOn = false;
        L = part(s, 0, 2048);
        R = L;
        run(g, q, L, R, playingAt(0.0));
        CHECK(g.tailSamples() == 0);
    }
}

// R61: a reset (the engine's, after a long suspend) empties Time's ring, so Time warms up from zero: the input passes bit
// for bit until the ring holds the groove's lag, and only then does the groove come in, by Time's own crossfade. (It once
// took the ring for full: a Push asked for its whole lag at once and got silence, then a step.) The run of zeros and the
// steepest step are measured on a 200 Hz and a 110 Hz sine, after a second of play and a reset in the middle of it.
void grooveResetWarmsTime() {
    struct Case { int groove; float amount; int lag; const char* name; };
    const Case cases[] = {{Time::kPush, 1.0f, 882, "Push 1.0"}, {Time::kPush, 0.5f, 441, "Push 0.5"}, {Time::kDrunk, 1.0f, 662, "Drunk 1.0"}};
    for (const double hz : {200.0, 110.0})
        for (const Case& c : cases) {
            P p;
            p.timeOn = true;
            p.time.groove = c.groove;
            p.time.amount = c.amount;
            Transport t = playingAt(0.0);
            Groove g;
            CHECK(g.allocate(p));
            double ph = 0.0;
            Buf in, out;
            bool recording = false;
            auto chunk = [&](float* L, float* R) {
                for (int i = 0; i < kChunk; ++i) {
                    L[i] = R[i] = 0.5f * static_cast<float>(std::sin(ph));
                    ph += 2.0 * kPi * hz / kRate;
                }
                if (recording) in.insert(in.end(), L, L + kChunk);
                g.set(p, t);
                g.process(L, R, kChunk);
                t.beats += kChunk / static_cast<double>(kRate) * t.bpm / 60.0;
            };
            float L[kChunk], R[kChunk];
            for (int k = 0; k < 44100 / kChunk; ++k) chunk(L, R);   // a second of play, Time already pushing
            g.reset();
            recording = true;
            for (int k = 0; k < 4480 / kChunk; ++k) {
                chunk(L, R);
                out.insert(out.end(), L, L + kChunk);
            }
            // Warming up it is the input itself, bit for bit; the groove then comes in without a hole or a step.
            const size_t warm = static_cast<size_t>(c.lag) + gf::TimeCore::kFade + kChunk;
            size_t run = 0, longest = 0;
            for (const float v : out) {
                run = v == 0.0f ? run + 1 : 0;
                longest = std::max(longest, run);
            }
            const double settled = steepest(in, 0, in.size());
            const double step = steepest(out, 0, out.size());
            if (longest > 2 || step > std::max(0.01, 1.25 * settled)) std::printf("  %-9s %3.0f Hz: %zu zeros in a row, steepest step %.4f (the input's own %.4f)\n", c.name, hz, longest, step, settled);
            CHECK(same(part(out, 0, warm), part(in, 0, warm)));
            CHECK(longest <= 2);   // (a sine crosses zero once in a while)
            CHECK(step <= std::max(0.01, 1.25 * settled));
            if (hz == 110.0) CHECK(maxDiff(part(out, warm + 600, out.size()), part(in, warm + 600, in.size())) > 0.1);   // and it does push
            CHECK(allFinite(out));
        }
}

// The chain waits for Time's ring to hold what the groove reads back: Time's own figure for the lag (Time::grooveLagSamples()), a
// crossfade and a chunk. For Swing, Drunk and Push at a few tempos (and the ones Time clamps) it is Time's tail, which counts the
// same lag and the same crossfade, and a chunk; and it is the length it always was: the lag's ceiling (the groove's old model,
// written out here) and the 220 samples of crossfade and the 32 of a chunk.
void grooveWarmsAsTimeLags() {
    const double tempi[] = {5.0, 20.0, 47.78, 90.0, 120.0, 128.0, 174.0, 999.0, 5000.0, std::nan("")};
    struct Case { int groove; float amount; Swing swing; };
    Swing s58, s66, straight;
    s58.amount = 0.58f;
    s66.amount = 0.66f;
    s66.unitBeats = 0.5;
    straight.amount = 0.5f;
    const Case cases[] = {{Time::kSwing, 0.5f, s58}, {Time::kSwing, 0.5f, s66}, {Time::kSwing, 0.5f, straight}, {Time::kDrunk, 1.0f, straight},
                          {Time::kDrunk, 0.3f, straight}, {Time::kPush, 1.0f, straight}, {Time::kPush, 0.5f, straight}, {Time::kPush, 0.0f, straight},
                          {Time::kOff, 1.0f, s58}};
    for (const double bpm : tempi)
        for (const Case& c : cases) {
            Time::Params tp;
            tp.groove = c.groove;
            tp.amount = c.amount;
            tp.swing = c.swing;
            Transport t = transport(bpm, 0.0, true);
            Time tm;
            CHECK(tm.allocate());
            tm.set(tp, t);
            const int warm = Groove::warmSamples(tp, t);
            const double lag = Time::grooveLagSamples(tp, t);
            CHECK(warm == (lag > 0.0 ? static_cast<int>(std::ceil(lag)) + gf::TimeCore::kFade + kChunk : 0));
            CHECK(warm == (tm.tailSamples() > 0 ? tm.tailSamples() + kChunk : 0));   // Time's own figure and a chunk
            // the length it always was
            const double clamped = bpm >= 20.0 ? std::min(bpm, 999.0) : (bpm < 20.0 ? 20.0 : 120.0);
            double was = 0.0;
            if (c.groove == Time::kSwing && gf::swingSplit(c.swing) > 0.0) was = (gf::swingSplit(c.swing) - c.swing.unitBeats) * 60.0 / clamped * kRate;
            if (c.groove == Time::kDrunk) was = c.amount * Time::kDrunkMs * 0.001 * kRate;
            if (c.groove == Time::kPush) was = c.amount * Time::kPushMs * 0.001 * kRate;
            const int wasWarm = was > 0.0 ? static_cast<int>(std::ceil(was)) + 220 + 32 : 0;
            if (warm != wasWarm) std::printf("  groove %d amount %.2f at %g BPM: warm %d, it was %d\n", c.groove, static_cast<double>(c.amount), bpm, warm, wasWarm);
            CHECK(warm == wasWarm);
        }
    // (a few values, by hand: a Push at its full amount is 20 ms = 882 samples at any tempo; Drunk's 15 ms is 661.5; a Swing of
    // 58% on 1/16 is 0.04 beats late = 882 samples at 120 BPM)
    Time::Params push;
    push.groove = Time::kPush;
    push.amount = 1.0f;
    CHECK(Groove::warmSamples(push, transport(60.0, 0.0, true)) == 882 + 220 + 32 && Groove::warmSamples(push, transport(174.0, 0.0, false)) == 882 + 220 + 32);
    push.groove = Time::kDrunk;
    CHECK(Groove::warmSamples(push, transport(120.0, 0.0, true)) == 662 + 220 + 32);
    Time::Params sw;
    sw.groove = Time::kSwing;
    sw.swing = s58;
    CHECK(Groove::warmSamples(sw, transport(120.0, 0.0, true)) == 882 + 220 + 32);
    CHECK(Groove::warmSamples(sw, transport(60.0, 0.0, true)) == 1764 + 220 + 32);   // half the tempo, twice the samples
}

// A part moved from one bus to another while the echo sounds: out of the one, into the other, 10 ms each,
// and from then on exactly what a part that had been there all along gives.
void grooveBusMoves() {
    const Buf in = sine(220.0, 4 * kBeat, 0.4f);
    P p;
    echoA(p, 0.5, 0.6f);
    silencePump(p);
    p.pumpOn = true;
    const Transport t = playingAt(0.2);
    Buf steady[3];
    for (int target : {Groove::kAll, Groove::kDry, Groove::kEcho}) {
        P q = p;
        q.pumpTarget = target;
        steady[target] = render(q, in, t);
    }
    // Dry, then Echo, All, Dry again.
    const int path[4] = {Groove::kDry, Groove::kEcho, Groove::kAll, Groove::kDry};
    auto edit = [&](P& x, size_t at) { x.pumpTarget = path[std::min<size_t>(at / 8192, 3)]; };
    const Played r = play(in, p, t, edit);
    for (int k = 0; k < 4; ++k) {
        const size_t from = static_cast<size_t>(k) * 8192, to = k == 3 ? 40000 : from + 8192;
        CHECK(maxDiff(part(r.out, from + 500, to), part(steady[path[k]], from + 500, to)) < 1e-6);
    }
    CHECK(same(part(r.out, 0, 8192), part(steady[Groove::kDry], 0, 8192)));
    CHECK(clickFree(r.out, 4096, 8192, 12288) && clickFree(r.out, 12288, 16384, 20000) && clickFree(r.out, 20000, 24576, 28672));
    // Costs: the Echo and the Pump, and a second instance only while the part is on the wet too.
    CHECK(r.running[100 / kChunk] == 2 && r.running[(8192 + 1000) / kChunk] == 2);
    CHECK(allFinite(r.out));
}

// Four parts: running() counts Shape, Pump, Echo and Time.
void grooveFourParts() {
    const Buf in = kickPad(2, 4410);
    P p;
    echoA(p, 0.5, 0.5f);
    p.pumpOn = p.shapeOn = p.timeOn = true;
    p.pump.depth = 0.5f;
    p.shape.lane[Shape::kVolume] = {true, shapeIdx("Square"), 1.0, 0.5f, 0.0f};
    for (int target : {Groove::kAll, Groove::kDry, Groove::kEcho})
        for (int order : {Groove::kEchoFirst, Groove::kEchoLast}) {
            p.pumpTarget = p.shapeTarget = target;
            p.order = order;
            const Played r = play(in, p, playingAt(0.0), [](P&, size_t) {});
            CHECK(r.running[20] == 4 && *std::min_element(r.running.begin(), r.running.end()) == 4);
        }
}

// The chain's contract with everything in it: finite for any parameters, a NaN in the input stays where it is
// (the echo never keeps it), reset forgets, the parts' tails add up.
void grooveChainContract() {
    const Buf inL = whiteNoise(16384, 1.0f, 7), inR = whiteNoise(16384, 1.0f, 8);
    const float inf = std::numeric_limits<float>::infinity(), nan = std::numeric_limits<float>::quiet_NaN();
    for (int variant = 0; variant < 6; ++variant) {
        P p;
        p.pumpOn = p.shapeOn = p.echoOn = p.timeOn = true;
        p.pump.depth = 1.0f;
        p.pump.rateBeats = 0.25;
        p.pump.trigger = Pump::kInput;
        p.pump.band = Pump::kHighs;
        p.shape.cutoffHz = 20.0f;
        p.shape.resonance = 1.0f;
        for (int l = 0; l < Shape::kLanes; ++l) {
            p.shape.lane[l].on = true;
            p.shape.lane[l].shape = 3 * l + variant;
            p.shape.lane[l].rateBeats = 0.25;
            p.shape.lane[l].depth = l == Shape::kVolume ? 1.0f : -1.0f;
        }
        p.echo.routing = Echo::kRing;
        for (int l = 0; l < Echo::kLines; ++l) {
            Echo::Line& e = p.echo.line[l];
            e.on = true;
            e.p.feedback = 1.0f;
            e.p.drive = 1.0f;
            e.p.divBeats = 0.0625 * (l + 1);
            e.modCutoff = 1.0f;
        }
        p.echo.send = 1.0f;
        p.time.groove = Time::kSwing;
        p.time.pattern = variant;
        p.time.patternStart = 0.5;
        p.time.loop = variant % 2 == 0;
        p.swing.amount = 0.75f;
        p.inputDb = 24.0f;
        p.outputDb = 12.0f;
        p.mix = 0.8f;
        p.pumpTarget = variant % 3;
        p.shapeTarget = (variant + 1) % 3;
        p.order = variant % 2;
        p.timePlace = (variant / 2) % 2;
        if (variant == 4) p.order = 99, p.timePlace = -7, p.dry = nan, p.pumpTarget = 42;
        if (variant == 5) p.dry = 0.3f, p.echo.freeze = true, p.inputDb = nan;
        for (int chunk : {kChunk, 13, 1}) {
            Buf L = inL, R = inR;
            Groove g;
            CHECK(g.allocate(p));
            run(g, p, L, R, playingAt(1.3), chunk);
            CHECK(allFinite(L) && allFinite(R));
            CHECK(g.tailSamples() >= 0);
        }
    }

    // A NaN or an infinity in the input stays where it is: the echo and the key never keep it.
    {
        P p;
        echoA(p, 0.25, 0.8f);
        p.timeOn = true;
        p.pump.trigger = Pump::kInput;
        p.pump.depth = 0.5f;
        p.pumpOn = true;
        p.pumpTarget = Groove::kEcho;
        Buf L = inL, R = inR;
        L[1000] = nan;
        R[3000] = inf;
        Groove g;
        CHECK(g.allocate(p));
        run(g, p, L, R, playingAt(0.0));
        bool finite = true;
        for (size_t i = 0; i < L.size(); ++i)
            if (i != 1000 && i != 3000) finite = finite && std::isfinite(L[i]) && std::isfinite(R[i]);
        CHECK(finite);
    }

    // Reset forgets everything: the same run again from the start.
    {
        P p;
        echoA(p, 0.5, 0.7f);
        p.pumpOn = p.shapeOn = p.timeOn = true;
        p.pump.depth = 0.8f;
        p.pump.trigger = Pump::kInput;
        p.time.groove = Time::kDrunk;
        p.time.amount = 1.0f;
        Shape::LaneParams& q = p.shape.lane[Shape::kFilter];
        q.on = true;
        q.shape = shapeIdx("Triangle");
        q.rateBeats = 1.0;
        q.depth = 1.0f;
        p.shape.cutoffHz = 2000.0f;
        p.shape.resonance = 0.9f;
        Groove g;
        CHECK(g.allocate(p));
        Buf aL = inL, aR = inR, bL = inL, bR = inR;
        run(g, p, aL, aR, playingAt(0.0));
        P other;
        other.inputDb = -5.0f;
        Buf xL = inL, xR = inR;
        run(g, other, xL, xR, playingAt(2.0));
        g.reset();
        run(g, p, bL, bR, playingAt(0.0));
        CHECK(same(aL, bL) && same(aR, bR));
    }

    // The tail of a chain: the Echo's, with the parts' rings after it.
    {
        Groove g;
        P p;
        echoA(p, 1.0, 0.5f);
        p.pumpOn = true;
        p.pump.depth = 1.0f;
        p.pump.band = Pump::kLows;
        CHECK(g.allocate(p));
        Buf L = part(inL, 0, 2048), R = part(inR, 0, 2048);
        run(g, p, L, R, playingAt(0.0));
        CHECK(g.tailSamples() >= kBeat + Pump::kTail);
        p.pumpTarget = Groove::kDry;
        L = part(inL, 0, 2048);
        R = part(inR, 0, 2048);
        run(g, p, L, R, playingAt(0.0));
        CHECK(g.tailSamples() >= kBeat);
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
    grooveEchoBus();
    grooveAllBothBuses();
    groovePumpWake();
    grooveSameKeyGains();
    grooveEchoOrder();
    grooveOrderFlip();
    grooveTimePlace();
    grooveStepSwingRule();
    grooveKeyAfterTime();
    grooveStepSwingEcho();
    grooveWetStartsWhereDryIs();
    grooveTimeListens();
    grooveRumbleKey();
    grooveDry();
    firstSwitchOn();
    grooveEchoSwitch();
    grooveTimeSwitch();
    grooveResetWarmsTime();
    grooveWarmsAsTimeLags();
    grooveBusMoves();
    grooveFourParts();
    grooveChainContract();
}
