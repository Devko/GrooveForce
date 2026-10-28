// The Echo's line (dsp/echo_line.h) against its laws: repeats where the time says (free, synced at
// several tempos, with the modulator's factor) and as big as the 30 Hz low cut leaves them, the
// feedback ratio and how a feedback of 1 holds, the filter In loop (each repeat filtered again) and On
// output (every repeat through it once), Ping-Pong, Mono and Stereo, Tape bending and Fade not,
// the drive and the limiter keeping what is written under 0 dBFS, the loop never gaining however resonant
// the filter (R36: silence stays silent, a feedback of 1 holds a ring and doesn't grow it, what is heard is
// the stored signal through the filter's peak), a line wired to itself (times loopScale()) giving the same
// as feedback inside, reset leaving nothing from before, NaN never getting into the loop. Then the
// contract: the lines come only with allocate(), reads do not depend on the chunk size, controls move
// without a click, the extremes, the tail.
#include "signal.h"
#include "../dsp/echo_line.h"

#include <cstdio>
#include <cstring>

namespace {

using namespace eft;
using gf::EchoLine;
using P = EchoLine::Params;

constexpr int kSr = 44100;

// A line with its memory (the Echo allocates a line the first time it is switched on).
struct Line : EchoLine {
    Line() { CHECK(allocate()); }
};

// A plain line to measure: free time, no feedback, the filter off, no wow or drive.
P plain(float ms) {
    P p;
    p.sync = false;
    p.timeMs = ms;
    p.feedback = 0.0f;
    return p;
}
P synced(double beats) {
    P p = plain(375.0f);
    p.sync = true;
    p.divBeats = beats;
    return p;
}
ef::Transport tempo(double bpm) {
    ef::Transport t;
    t.bpm = bpm;
    return t;
}

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
bool same(const Buf& a, const Buf& b) { return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0; }
double maxDiff(const Buf& a, const Buf& b) {
    double m = 0.0;
    for (size_t i = 0; i < a.size(); ++i) m = std::max(m, static_cast<double>(std::fabs(a[i] - b[i])));
    return m;
}
size_t loudest(const Buf& x, size_t from = 0, size_t to = 0) {
    if (to == 0 || to > x.size()) to = x.size();
    size_t best = from;
    for (size_t i = from; i < to; ++i)
        if (std::fabs(x[i]) > std::fabs(x[best])) best = i;
    return best;
}
Buf silence(int n) { return Buf(static_cast<size_t>(n), 0.0f); }
Buf concat(Buf a, const Buf& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}
// A tone burst under a Hann window (its start and end are no click to measure).
Buf burst(double hz, int len, float amp) {
    Buf x = sine(hz, len, amp);
    for (int i = 0; i < len; ++i) x[static_cast<size_t>(i)] *= static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * i / len));
    return x;
}
// The upward zero crossings of x in [from, to): the cycles of a tone there.
int cycles(const Buf& x, size_t from, size_t to) {
    int n = 0;
    for (size_t i = from + 1; i < to; ++i)
        if (x[i - 1] < 0.0f && x[i] >= 0.0f) ++n;
    return n;
}

// Runs a line over n samples as the Echo does, in chunks: set, read the chunk, write the chunk.
void runLine(EchoLine& e, const P& p, const float* inL, const float* inR, float* outL, float* outR, size_t n, ef::Transport t = {},
             int chunk = ef::kChunk) {
    for (size_t pos = 0; pos < n; pos += static_cast<size_t>(chunk)) {
        const int m = static_cast<int>(std::min(static_cast<size_t>(chunk), n - pos));
        e.set(p, t);
        e.read(outL + pos, outR + pos, m);
        e.write(inL + pos, inR + pos, m);
        if (t.playing) t.beats += m / static_cast<double>(ef::kRate) * t.bpm / 60.0;
    }
}
void runLine(EchoLine& e, const P& p, const Buf& inL, const Buf& inR, Buf& outL, Buf& outR, ef::Transport t = {}, int chunk = ef::kChunk) {
    outL.assign(inL.size(), 0.0f);
    outR.assign(inR.size(), 0.0f);
    runLine(e, p, inL.data(), inR.data(), outL.data(), outR.data(), inL.size(), t, chunk);
}
// p for the first `at` samples, then q.
void runSplit(EchoLine& e, const P& p, const P& q, const Buf& inL, const Buf& inR, Buf& outL, Buf& outR, size_t at, ef::Transport t = {}) {
    outL.assign(inL.size(), 0.0f);
    outR.assign(inR.size(), 0.0f);
    runLine(e, p, inL.data(), inR.data(), outL.data(), outR.data(), at, t);
    runLine(e, q, inL.data() + at, inR.data() + at, outL.data() + at, outR.data() + at, inL.size() - at, t);
}

// What gets written passes a one-pole low cut at 30 Hz, a trapezoidal one: g = tan(pi 30 / rate), its
// low-pass follows by g / (1 + g) of the way a sample, and the output is what the low-pass leaves.
double lowCutG() { return std::tan(kPi * 30.0 / ef::kRate); }
double h0() { return 1.0 / (1.0 + lowCutG()); }   // the low cut's first sample: an impulse comes back this big

// An impulse at `at` read back `delay` samples later, in double: through the low cut and the 4-point
// Hermite kernel. What one repeat of the plain line must look like.
Buf echoOf(int n, int at, double delay, double amp = 1.0) {
    const double step = lowCutG() / (1.0 + lowCutG());
    std::vector<double> w(static_cast<size_t>(n), 0.0);
    double s = 0.0;
    for (int k = 0; k < n; ++k) {
        const double x = k == at ? amp : 0.0;
        const double v = (x - s) * step, lp = v + s;
        s = lp + v;
        w[static_cast<size_t>(k)] = x - lp;
    }
    const auto tap = [&](int k) { return k >= 0 && k < n ? w[static_cast<size_t>(k)] : 0.0; };
    const int il = static_cast<int>(delay);
    const double f = delay - il;
    Buf y(static_cast<size_t>(n));
    for (int k = 0; k < n; ++k) {
        const double xm1 = tap(k - il + 1), x0 = tap(k - il), x1 = tap(k - il - 1), x2 = tap(k - il - 2);
        const double c1 = 0.5 * (x1 - xm1), c2 = xm1 - 2.5 * x0 + 2.0 * x1 - 0.5 * x2, c3 = 0.5 * (x2 - xm1) + 1.5 * (x0 - x1);
        y[static_cast<size_t>(k)] = static_cast<float>(((c3 * f + c2) * f + c1) * f + x0);
    }
    return y;
}

// The plain line's answer to an impulse on both sides against echoOf() at `delay`; a whole number of
// samples comes back on the sample, h0 big.
bool echoes(EchoLine& e, const P& p, ef::Transport t, double delay) {
    const int at = 64, n = at + static_cast<int>(delay) + 64;
    e.reset();
    const Buf in = impulseAt(n, at);
    Buf L, R;
    runLine(e, p, in, in, L, R, t);
    const Buf want = echoOf(n, at, delay);
    double worst = 0.0;
    for (size_t i = 0; i < L.size(); ++i)
        worst = std::max({worst, std::fabs(static_cast<double>(L[i]) - want[i]), std::fabs(static_cast<double>(R[i]) - want[i])});
    bool ok = worst < 1e-5;
    if (delay == std::floor(delay)) ok = ok && loudest(L) == static_cast<size_t>(at + delay) && near(L[loudest(L)], h0(), 1e-6);
    if (!ok) std::printf("  echo at %.6f samples: off by %g\n", delay, worst);
    return ok;
}

void lineTime() {        // an impulse comes back at 1/8. at 120 BPM (16537.5 samples, ±1) with level 1; free 375 ms the same
    Line e;
    const int at = 1000, n = at + 16538 + 200;
    for (int isFree = 0; isFree < 2; ++isFree) {
        const P p = isFree ? plain(375.0f) : synced(0.75);
        e.reset();
        const Buf in = impulseAt(n, at);
        Buf L, R;
        runLine(e, p, in, in, L, R, tempo(120.0));
        // Where the repeat is: the centre of its energy; how big: the sum of its samples.
        const int c = at + 16537;
        double energy = 0.0, centre = 0.0, level = 0.0;
        for (int i = c - 4; i <= c + 5; ++i) {
            const double y = L[static_cast<size_t>(i)];
            energy += y * y;
            centre += i * y * y;
            level += y;
        }
        CHECK(near(centre / energy, at + 16537.5, 1.0));
        CHECK(near(db(level), 0.0, 0.3));
        CHECK(same(L, R));
        CHECK(peak(L, 0, static_cast<size_t>(c - 8)) < 1e-6f);   // nothing before it
        CHECK(echoes(e, p, tempo(120.0), 16537.5));              // and to the sample
    }

    // Free: timeMs later, between samples as the Hermite kernel puts it. 1 ms is the shortest.
    for (float ms : {1.0f, 2.5f, 10.0f, 250.0f, 333.3f, 1000.0f, 2000.0f}) CHECK(echoes(e, plain(ms), {}, static_cast<double>(ms) * kSr / 1000.0));
    CHECK(echoes(e, plain(0.01f), {}, 44.1));
    CHECK(echoes(e, plain(5000.0f), {}, 2.0 * kSr));
    // Synced: beats x 60 / BPM seconds, at several tempos; whole numbers of samples are exact (a 1/16T
    // at 100 BPM is 4410, not 4410.000000000001). The longest is 1 bar at 80 BPM, 3 s; slower is clamped there.
    const double cases[][2] = {{0.5, 120}, {1.5, 90}, {1.0 / 6, 100}, {1.0 / 12, 300}, {0.0625, 175}, {1.0 / 3, 140},
                               {1.25, 120}, {1.75, 90}, {2.5, 120},   {3.0, 100},      {4.0, 80}};
    for (const auto& c : cases) CHECK(echoes(e, synced(c[0]), tempo(c[1]), c[0] * 60.0 / c[1] * kSr));
    CHECK(echoes(e, synced(4.0), tempo(60.0), 3.0 * kSr));
    CHECK(echoes(e, synced(4.0), tempo(20.0), 3.0 * kSr));
    // The modulator's factor scales whichever time it is.
    {
        P p = plain(200.0f);
        p.timeMod = 0.5f;
        CHECK(echoes(e, p, {}, 4410.0));
        p.timeMod = 2.0f;
        CHECK(echoes(e, p, {}, 17640.0));
        P q = synced(0.75);
        q.timeMod = 1.5f;
        CHECK(echoes(e, q, tempo(120.0), 16537.5 * 1.5));
    }
}

void lineFeedback() {    // feedback 0.5: repeat n at 0.5^(n-1) (±0.01 dB with filter off); feedback 1 holds 30 s without growing past 0 dBFS
    Line e;
    const int T = 11025, len = 4410;   // 250 ms; 100 ms tone bursts
    const double hz = 2000.0;          // where the 30 Hz low cut takes 0.001 dB a pass
    const Buf b = burst(hz, len, 0.5f);
    {
        P p = plain(250.0f);
        p.feedback = 0.5f;
        const Buf in = concat(b, silence(6 * T));
        Buf L, R;
        runLine(e, p, in, in, L, R);
        const double first = magnitude(L, hz, T, T + len);
        CHECK(near(db(first / magnitude(b, hz)), 0.0, 0.01));
        for (int n = 2; n <= 6; ++n) {
            const double got = db(magnitude(L, hz, static_cast<size_t>(n * T), static_cast<size_t>(n * T + len)) / first);
            const double want = db(std::pow(0.5, n - 1));
            if (!near(got, want, 0.01)) std::printf("  repeat %d: %.4f dB, wanted %.4f\n", n, got, want);
            CHECK(near(got, want, 0.01));
        }
    }
    // Feedback 1 holds: 30 s on, the burst is as loud as it was (the low cut's 0.001 dB a pass), never louder.
    {
        P p = plain(250.0f);
        p.feedback = 1.0f;
        const Buf in = concat(b, silence(30 * kSr - len));
        Buf L, R;
        e.reset();
        runLine(e, p, in, in, L, R);
        const double first = magnitude(L, hz, T, T + len);
        const double last = magnitude(L, hz, 117 * T, 117 * T + len);
        CHECK(near(db(last / first), 0.0, 0.2) && last <= first * 1.0001);
        CHECK(allFinite(L) && peak(L) <= 0.5f + 1e-5f);
    }
    // And with the repeats stacking on the input: a loud tone, feedback 1, 30 s. The limiter holds it
    // under 0 dBFS; it stays up, it does not run away.
    {
        P p = plain(100.0f);
        p.feedback = 1.0f;
        const Buf in = sine(200.0, 30 * kSr, 0.9f);
        Buf L, R;
        e.reset();
        runLine(e, p, in, in, L, R);
        CHECK(allFinite(L) && peak(L) <= 1.00001f && e.peakStored() <= 1.00001f);
        CHECK(rms(L, 29 * kSr, 30 * kSr) > 0.3);
    }
}

// The level of a repeat of a burst of `hz`, in dB against the burst: the energy around where it lands
// (the filter delays it a little and rings on a little), as a level.
double repeatDb(const Buf& y, int n, int T, int len, double hz) {
    const size_t from = static_cast<size_t>(n * T) - 220, to = static_cast<size_t>(n * T + len) + 441;
    const double got = rms(y, from, to) * std::sqrt(static_cast<double>(to - from));
    const Buf b = burst(hz, len, 0.5f);
    return db(got / (rms(b) * std::sqrt(static_cast<double>(len))));
}
// A Butterworth low-pass of the SVF (bilinear transform, prewarped): the gain at `hz` for a cutoff of `fc`.
double butter(double hz, double fc) {
    const double r = std::tan(kPi * hz / ef::kRate) / std::tan(kPi * fc / ef::kRate);
    return 1.0 / std::sqrt(1.0 + r * r * r * r);
}

// The SVF's Q at a resonance (0.7071 .. 12, evenly in octaves of Q) and the peak gain of its low-pass and high-pass at it
// (Q / sqrt(1 - 1/(4 Q^2)); the band-pass peaks at 1).
double qOf(double res) { return 0.70710678 * std::pow(12.0 / 0.70710678, res); }
double peakOf(double res) {
    const double q = qOf(res);
    return q > 0.70710678 ? q / std::sqrt(1.0 - 0.25 / (q * q)) : 1.0;
}

void lineInLoop() {      // LP 1 kHz in the loop: a 4 kHz tone loses ≥ 12 dB more per repeat than a 250 Hz one; On output: every repeat the same
    Line e;
    const int T = 11025, len = 4410;
    P p = plain(250.0f);
    p.feedback = 0.5f;
    p.filterType = EchoLine::kLP;
    p.cutoffHz = 1000.0f;
    double drop[2][2];   // [In loop, On output][250 Hz, 4 kHz]: the level lost from the first repeat to the second
    for (int place = 0; place < 2; ++place) {
        p.place = place ? EchoLine::kOnOutput : EchoLine::kInLoop;
        int i = 0;
        for (double hz : {250.0, 4000.0}) {
            const Buf in = concat(burst(hz, len, 0.5f), silence(4 * T));
            Buf L, R;
            e.reset();
            runLine(e, p, in, in, L, R);
            const double r1 = repeatDb(L, 1, T, len, hz), r2 = repeatDb(L, 2, T, len, hz), r3 = repeatDb(L, 3, T, len, hz);
            drop[place][i++] = r1 - r2;
            const double once = db(butter(hz, 1000.0));   // the filter's gain at this tone
            if (place == 0) {
                // each repeat through the filter once more, and fed back at 0.5
                CHECK(near(r1, once, 0.3) && near(r2 - r1, once - 6.02, 0.3) && near(r3 - r2, once - 6.02, 0.3));
            } else {
                // every repeat through it once: the feedback alone makes the drop
                CHECK(near(r1, once, 0.3) && near(r2 - r1, -6.02, 0.3) && near(r3 - r2, -6.02, 0.3));
            }
        }
    }
    CHECK(drop[0][1] - drop[0][0] >= 12.0);                  // In loop: 4 kHz falls away
    CHECK(near(drop[1][0], drop[1][1], 0.3));                // On output: every repeat the same for both
    CHECK(near(drop[1][0], 6.02, 0.3));

    // The other types In loop: a high-pass 1 kHz keeps 4 kHz and loses 250 Hz; a band-pass at 1 kHz keeps
    // its centre (0 dB, so feedback alone sets the drop) and loses both.
    for (int type : {EchoLine::kHP, EchoLine::kBP}) {
        P q = p;
        q.filterType = type;
        q.place = EchoLine::kInLoop;
        double d[3];
        int i = 0;
        for (double hz : {250.0, 1000.0, 4000.0}) {
            const Buf in = concat(burst(hz, len, 0.5f), silence(4 * T));
            Buf L, R;
            e.reset();
            runLine(e, q, in, in, L, R);
            d[i++] = repeatDb(L, 1, T, len, hz) - repeatDb(L, 2, T, len, hz);
        }
        if (type == EchoLine::kHP) CHECK(d[0] - d[2] >= 12.0 && near(d[2], 6.02, 0.3));
        else CHECK(d[0] - d[1] >= 6.0 && d[2] - d[1] >= 6.0 && near(d[1], 6.02, 0.3));
    }

    // The modulator's octaves move the cutoff: 2 kHz down an octave is 1 kHz.
    {
        P a = p, b = p;
        a.place = b.place = EchoLine::kOnOutput;
        a.feedback = b.feedback = 0.0f;
        a.cutoffHz = 2000.0f;
        a.cutoffModOct = -1.0f;
        const Buf in = concat(burst(4000.0, len, 0.5f), silence(2 * T));
        Buf A, B, R;
        runLine(e, a, in, in, A, R);
        e.reset();
        runLine(e, b, in, in, B, R);
        CHECK(maxDiff(A, B) < 1e-3);
    }
}

void linePingPong() {    // an impulse in mono: repeats alternate L, R, L at the line's time
    Line e;
    const int t = 4410;
    const double h = h0();
    {
        P p = plain(100.0f);
        p.mode = EchoLine::kPingPong;
        p.feedback = 0.5f;
        const Buf in = impulseAt(7 * t + 100, 0);
        Buf L, R;
        runLine(e, p, in, in, L, R);
        for (int k = 1; k <= 6; ++k) {
            const Buf& on = k % 2 ? L : R;
            const Buf& off = k % 2 ? R : L;
            const size_t at = static_cast<size_t>(k * t);
            CHECK(near(on[at], std::pow(0.5, k - 1) * std::pow(h, k), 1e-5));
            CHECK(std::fabs(off[at]) < 1e-6);
            CHECK(loudest(on, at - t / 2, at + t / 2) == at);
        }
        CHECK(peak(L, 0, t - 8) < 1e-6f && peak(R, 0, 2 * t - 8) < 1e-6f);   // L starts, R follows
    }
    // A side alone counts half (the mono sum), and still starts on L.
    {
        e.reset();
        P p = plain(100.0f);
        p.mode = EchoLine::kPingPong;
        p.feedback = 0.5f;
        const Buf zero = silence(3 * t), one = impulseAt(3 * t, 0);
        Buf L, R;
        runLine(e, p, zero, one, L, R);
        CHECK(near(L[t], 0.5 * h, 1e-5) && std::fabs(R[t]) < 1e-6);
        CHECK(near(R[2 * t], 0.25 * h * h, 1e-5) && std::fabs(L[2 * t]) < 1e-6);
    }
    // With the filter In loop the repeats keep alternating.
    {
        e.reset();
        P p = plain(100.0f);
        p.mode = EchoLine::kPingPong;
        p.feedback = 0.7f;
        p.filterType = EchoLine::kLP;
        p.cutoffHz = 3000.0f;
        const Buf in = impulseAt(6 * t, 0);
        Buf L, R;
        runLine(e, p, in, in, L, R);
        for (int k = 1; k <= 4; ++k) {
            const Buf& on = k % 2 ? L : R;
            const Buf& off = k % 2 ? R : L;
            const size_t at = static_cast<size_t>(k * t);
            CHECK(peak(on, at - 20, at + 60) > 0.05f * static_cast<float>(std::pow(0.7, k)));
            CHECK(peak(off, at - 20, at + 60) < 1e-6f);
        }
    }
}

void lineModes() {       // Mono: L == R; Stereo: L and R independent
    Line e;
    P p = plain(120.0f);
    p.mode = EchoLine::kMono;
    p.feedback = 0.6f;
    p.wow = 0.7f;
    p.drive = 0.5f;
    p.filterType = EchoLine::kBP;
    p.cutoffHz = 1500.0f;
    p.resonance = 0.3f;
    const Buf inL = whiteNoise(30000, 0.5f, 5), inR = sine(330.0, 30000, 0.5f);
    Buf L, R;
    runLine(e, p, inL, inR, L, R);
    CHECK(same(L, R) && peak(L) > 0.1f);
    // It is the echo of the mono sum: the same as Stereo's L fed that sum on both sides.
    Buf sum(inL.size());
    for (size_t i = 0; i < sum.size(); ++i) sum[i] = 0.5f * inL[i] + 0.5f * inR[i];
    Line s;
    P q = p;
    q.mode = EchoLine::kStereo;
    Buf A, B;
    runLine(s, q, sum, sum, A, B);
    CHECK(same(A, L));
    // Mono through a Fade and a time change, too: the two heads' wow is the same on both sides.
    {
        Line m;
        P f = p, g = p;
        f.glide = g.glide = EchoLine::kFade;
        g.timeMs = 170.0f;
        Buf C, D;
        runSplit(m, f, g, inL, inR, C, D, 10000);
        CHECK(same(C, D));
    }
    // Stereo: each side its own line. L's input never comes out on R, whatever R is fed.
    P st = plain(50.0f);
    st.feedback = 0.7f;
    st.wow = 0.5f;
    st.filterType = EchoLine::kLP;
    st.cutoffHz = 4000.0f;
    const Buf noise = whiteNoise(20000, 0.5f, 3);
    Buf L1, R1, L2, R2;
    e.reset();
    runLine(e, st, noise, silence(20000), L1, R1);
    CHECK(peak(R1) < 1e-9f && peak(L1) > 0.1f);
    e.reset();
    runLine(e, st, noise, sine(500.0, 20000, 0.8f), L2, R2);
    CHECK(same(L1, L2) && peak(R2) > 0.1f);
}

void lineGlide() {       // a time change: Tape bends (a 1 kHz tone's repeat shifts in pitch while gliding), Fade doesn't (no pitch shift, a 50 ms crossfade); no click either way
    Line e;
    const size_t at = 32 * 1500;   // the change: a chunk boundary
    const int n = static_cast<int>(at) + 2 * kSr;
    const Buf tone = sine(1000.0, n, 0.5f);
    const int window = 1323;       // 30 ms after the change
    for (int glide = 0; glide < 2; ++glide) {
        for (float to : {215.0f, 185.0f}) {
            P p = plain(200.0f), q = plain(to);
            p.glide = q.glide = glide ? EchoLine::kFade : EchoLine::kTape;
            Buf L, R;
            e.reset();
            runSplit(e, p, q, tone, tone, L, R, at);
            // 15 ms apart is 15 cycles of 1 kHz: Fade's two heads are in phase, so it stays at 30 cycles in
            // 30 ms. Tape's pitch is 1 - (how fast the time moves): its 60 ms one-pole moves 15 ms by 5.9 ms
            // in the first 30 ms, so a tone of 30 cycles shows 24 going up and 36 going down.
            const int c = cycles(L, at, at + window);
            if (glide == EchoLine::kTape) CHECK(std::abs(c - 30) >= 4);     // 24 going up, 36 going down
            else CHECK(c >= 29 && c <= 31);
            if (glide == EchoLine::kTape && to > 200.0f) CHECK(c >= 22 && c <= 26);
            if (glide == EchoLine::kTape && to < 200.0f) CHECK(c >= 34 && c <= 38);
            // No click either way: nothing steeper than a little over the tone's own slope (Fade's two
            // heads add up to 3 dB when they meet in phase).
            const float steady = maxStep(L, at - 4000, at);
            CHECK(maxStep(L, at - 2000, at + 7000) <= 1.5f * steady);
            // It lands on the new time exactly.
            CHECK(e.timeSamples() == static_cast<double>(to) * kSr / 1000.0);
            CHECK(allFinite(L));
        }
    }

    // Fade's crossfade: 12.5 ms apart is half a cycle off, so the equal-power halfway point cancels.
    // The dip is at 25 ms, the level is back by 50 ms; Tape on the same change never dips.
    {
        P p = plain(200.0f), q = plain(212.5f);
        p.glide = q.glide = EchoLine::kFade;
        Buf L, R;
        e.reset();
        runSplit(e, p, q, tone, tone, L, R, at);
        const double steady = rms(L, at - 4000, at);
        const size_t mid = at + 1103;
        CHECK(rms(L, mid - 88, mid + 88) < 0.15 * steady);
        CHECK(rms(L, at, at + 176) > 0.85 * steady);
        CHECK(near(rms(L, at + 2800, at + 3800) / steady, 1.0, 0.05));
        p.glide = q.glide = EchoLine::kTape;
        e.reset();
        runSplit(e, p, q, tone, tone, L, R, at);
        for (size_t m = at; m < at + 3000; m += 176) CHECK(rms(L, m, m + 176) > 0.7 * steady);
    }

    // A tempo change glides a synced line the same way (Tape: 60 ms one-pole stepped every 32 samples).
    {
        P p = synced(0.5);
        Buf L = whiteNoise(kSr, 0.5f, 9), R = L, oL, oR;
        e.reset();
        runLine(e, p, L, R, oL, oR, tempo(120.0));
        CHECK(e.timeSamples() == 11025.0);
        const int part = 83 * 32;   // 60.2 ms
        const Buf z = silence(part);
        runLine(e, p, z, z, oL, oR, tempo(60.0));
        const double want = 22050.0 - 11025.0 * std::exp(-part / (0.060 * kSr));
        CHECK(near(e.timeSamples(), want, 1e-6 * want));
        const Buf z3 = silence(3 * kSr);
        runLine(e, p, z3, z3, oL, oR, tempo(60.0));
        CHECK(allFinite(oL) && e.timeSamples() == 22050.0);
    }
}

void lineDriveBounded() {// drive 1, feedback 1, a full-scale square for 20 s: every write ≤ 0 dBFS, output finite
    Line e;
    const int seconds = 20;
    Buf sq(static_cast<size_t>(seconds * kSr));
    for (size_t i = 0; i < sq.size(); ++i) sq[i] = std::fmod(static_cast<double>(i) * 110.0 / kSr, 1.0) < 0.5 ? 1.0f : -1.0f;

    struct Case {
        const char* name;
        float drive, feedback;
        int filterType;
        float cutoff, resonance;
        int place, mode;
        float wow;
    };
    const Case cases[] = {
        {"drive 1", 1.0f, 1.0f, EchoLine::kLP, 20000.0f, 0.0f, EchoLine::kInLoop, EchoLine::kStereo, 0.0f},
        {"no drive", 0.0f, 1.0f, EchoLine::kLP, 20000.0f, 0.0f, EchoLine::kInLoop, EchoLine::kStereo, 0.0f},
        {"resonant low-pass", 0.0f, 1.0f, EchoLine::kLP, 800.0f, 1.0f, EchoLine::kInLoop, EchoLine::kStereo, 0.0f},
        {"resonant band-pass, drive", 0.5f, 1.0f, EchoLine::kBP, 300.0f, 1.0f, EchoLine::kInLoop, EchoLine::kPingPong, 1.0f},
        {"resonant high-pass, mono", 0.2f, 1.0f, EchoLine::kHP, 2000.0f, 1.0f, EchoLine::kInLoop, EchoLine::kMono, 1.0f},
    };
    for (const Case& c : cases) {
        P p = plain(100.0f);
        p.drive = c.drive;
        p.feedback = c.feedback;
        p.filterType = c.filterType;
        p.cutoffHz = c.cutoff;
        p.resonance = c.resonance;
        p.place = c.place;
        p.mode = c.mode;
        p.wow = c.wow;
        e.reset();
        bool ok = true, bounded = true;
        Buf L(static_cast<size_t>(kSr)), R(static_cast<size_t>(kSr));
        float loudestOut = 0.0f;
        // What is heard is what is stored (3 s of it are in the lines) through the filter's peak gain and the
        // Hermite kernel's overshoot between samples: 1.3 covers that.
        const double gain = 1.3 * (c.filterType == EchoLine::kBP ? 1.0 : peakOf(c.resonance));
        for (int s = 0; s < seconds; ++s) {
            const float* in = sq.data() + static_cast<size_t>(s) * kSr;
            runLine(e, p, in, in, L.data(), R.data(), kSr);
            // The lines hold 3 s: looking every second sees every write.
            const double stored = e.peakStored();
            ok = ok && allFinite(L) && allFinite(R) && stored <= 1.00001;
            bounded = bounded && peak(L) <= gain * stored && peak(R) <= gain * stored;
            loudestOut = std::max({loudestOut, peak(L), peak(R)});
        }
        if (!ok || !bounded) std::printf("  %s: stored peak %g, out %g\n", c.name, static_cast<double>(e.peakStored()), static_cast<double>(loudestOut));
        CHECK(ok);
        CHECK(bounded);
        if (c.resonance == 0.0f && c.wow == 0.0f) CHECK(loudestOut <= 1.00001f);   // whole-sample reads of what was written
    }
}

// The plain line with a drive, in double: the low cut, then the cubic shaper on what is written,
// read back `delay` samples later.
Buf driven(const Buf& x, int delay, double drive) {
    const double step = lowCutG() / (1.0 + lowCutG());
    double s = 0.0;
    Buf written(x.size()), y(x.size(), 0.0f);
    for (size_t n = 0; n < x.size(); ++n) {
        const double u = x[n], v = (u - s) * step, lp = v + s, w = u - lp;
        s = lp + v;
        const double c = std::clamp(w, -0.5, 0.5);
        written[n] = static_cast<float>(w + drive * (c - 4.0 / 3.0 * c * c * c - w));
        if (n >= static_cast<size_t>(delay)) y[n] = written[n - static_cast<size_t>(delay)];
    }
    return y;
}

void lineDrive() {       // the drive is a cubic soft clipper on what is written, blended in by the amount
    Line e;
    const Buf in = sine(300.0, 20000, 0.9f);
    Buf plainL, R;
    e.reset();
    runLine(e, plain(100.0f), in, in, plainL, R);
    CHECK(maxDiff(plainL, driven(in, 4410, 0.0)) < 1e-5);   // drive 0 is the line as it was
    for (float d : {0.25f, 0.5f, 1.0f}) {
        P p = plain(100.0f);
        p.drive = d;
        Buf L;
        e.reset();
        runLine(e, p, in, in, L, R);
        const double worst = maxDiff(L, driven(in, 4410, static_cast<double>(d)));
        if (worst > 1e-5) std::printf("  drive %g: off by %g\n", static_cast<double>(d), worst);
        CHECK(worst < 1e-5);
        CHECK(maxDiff(L, plainL) > 0.1 * d);   // and it does change it
        if (d == 1.0f) CHECK(peak(L) <= 1.0f / 3.0f + 1e-5f);   // flat at +-1/3 from +-1/2 on
    }
}

void lineNearest() {     // the nearest a read comes is 34 samples: at 1 ms the wow gives way there
    Line e;
    P p = plain(1.0f);
    p.wow = 1.0f;
    const int period = 441, count = 220;   // an impulse every 10 ms for 2.2 s: a whole wow cycle (2 s)
    Buf in(static_cast<size_t>(period * count + 200), 0.0f);
    for (int k = 0; k < count; ++k) in[static_cast<size_t>(k * period + 100)] = 0.5f;
    Buf L, R;
    runLine(e, p, in, in, L, R);
    double nearest = 1e9, farthest = 0.0;
    for (int k = 0; k < count; ++k) {
        double energy = 0.0, centre = 0.0;
        for (int i = k * period + 100 + 20; i < k * period + 100 + 90; ++i) {
            const double y = L[static_cast<size_t>(i)];
            energy += y * y;
            centre += (i - (k * period + 100)) * y * y;
        }
        nearest = std::min(nearest, centre / energy);
        farthest = std::max(farthest, centre / energy);
    }
    CHECK(nearest >= 33.8);     // not 44.1 - 11.8 = 32.3
    CHECK(farthest >= 52.0);    // and the wow does reach the other way: 44.1 + 11.8
}

void lineReadWrite() {   // a line whose input is its own read (Ring of one) at 1 ms: stable and identical to feedback through write()
    Line inside, wired;
    const int n = 32 * 6900;   // whole chunks, about 5 s
    const Buf seed = concat(whiteNoise(kSr / 2, 0.5f, 21), silence(n - kSr / 2));
    struct Case {
        float g;
        int type;
        float cutoff, resonance;
    };
    // R36: what is fed back is scaled by loopScale(), so the wire carries the read times that scale, and then a
    // Ring of one is the same as feedback inside whatever the filter: off, a Butterworth (scale 1), resonant.
    // Feedback 0.9 and 1 are stable all the way up to resonance 1.
    const Case cases[] = {{0.9f, EchoLine::kLP, 20000.0f, 0.0f}, {1.0f, EchoLine::kLP, 20000.0f, 0.0f}, {0.9f, EchoLine::kLP, 3000.0f, 0.0f},
                          {1.0f, EchoLine::kLP, 3000.0f, 0.0f},  {1.0f, EchoLine::kLP, 3000.0f, 0.3f},  {1.0f, EchoLine::kLP, 800.0f, 1.0f},
                          {0.9f, EchoLine::kHP, 2000.0f, 1.0f},  {1.0f, EchoLine::kBP, 1000.0f, 1.0f}};
    for (const Case& c : cases) {
        P p = plain(1.0f);
        p.filterType = c.type;
        p.cutoffHz = c.cutoff;
        p.resonance = c.resonance;
        p.drive = 0.2f;
        p.place = EchoLine::kInLoop;
        P fb = p;
        fb.feedback = c.g;
        inside.reset();
        wired.reset();
        Buf A, B;
        runLine(inside, fb, seed, seed, A, B);
        // The wire: each chunk's input is the seed plus g x (what the same chunk's read just gave x the scale).
        Buf C(static_cast<size_t>(n)), D(static_cast<size_t>(n));
        Buf wireL(static_cast<size_t>(ef::kChunk)), wireR(static_cast<size_t>(ef::kChunk));
        for (size_t pos = 0; pos < static_cast<size_t>(n); pos += ef::kChunk) {
            wired.set(p, {});
            wired.read(&C[pos], &D[pos], ef::kChunk);
            const float scale = wired.loopScale();
            for (size_t k = 0; k < static_cast<size_t>(ef::kChunk); ++k) {
                wireL[k] = seed[pos + k] + c.g * (C[pos + k] * scale);
                wireR[k] = seed[pos + k] + c.g * (D[pos + k] * scale);
            }
            wired.write(wireL.data(), wireR.data(), ef::kChunk);
        }
        // The same arithmetic in a different order (the device's compiler fuses multiplies into adds): a few float
        // roundings of the loudest thing heard (up to 12 times what is stored), which a loop at gain 1 keeps for 5 s.
        const double d = std::max(maxDiff(A, C), maxDiff(B, D)), tol = 1e-5 * std::max(1.0, static_cast<double>(peak(A)));
        if (d > tol) std::printf("  ring of one, feedback %g, type %d, cutoff %g, resonance %g: differs by %g (peak %g)\n", static_cast<double>(c.g), c.type, static_cast<double>(c.cutoff), static_cast<double>(c.resonance), d, static_cast<double>(peak(A)));
        CHECK(d <= tol);
        CHECK(allFinite(C) && wired.peakStored() <= 1.00001f && inside.peakStored() <= 1.00001f);
    }
}

void lineLoopScale() {   // R36: the scale is 1 / the filter's peak gain; In loop the feedback has it, On output the feedback doesn't
    Line e;
    const auto scaleOf = [&](int type, float cutoff, float res, int place) {
        P p = plain(100.0f);
        p.filterType = type;
        p.cutoffHz = cutoff;
        p.resonance = res;
        p.place = place;
        e.reset();
        const Buf z = silence(512);
        Buf L, R;
        runLine(e, p, z, z, L, R);
        return e.loopScale();
    };
    // Exactly 1 where the filter doesn't boost: bypassed, a Butterworth, the band-pass at any resonance.
    CHECK(scaleOf(EchoLine::kLP, 20000.0f, 1.0f, EchoLine::kInLoop) == 1.0f);   // bypassed
    CHECK(scaleOf(EchoLine::kLP, 1000.0f, 0.0f, EchoLine::kInLoop) == 1.0f);
    CHECK(scaleOf(EchoLine::kHP, 1000.0f, 0.0f, EchoLine::kInLoop) == 1.0f);
    CHECK(scaleOf(EchoLine::kBP, 1000.0f, 0.0f, EchoLine::kInLoop) == 1.0f);
    CHECK(scaleOf(EchoLine::kBP, 1000.0f, 1.0f, EchoLine::kInLoop) == 1.0f);
    // The low-pass and the high-pass at their peaks: Q / sqrt(1 - 1/(4 Q^2)).
    for (float res : {0.25f, 0.5f, 0.75f, 1.0f}) {
        const double want = 1.0 / peakOf(res);
        CHECK(near(scaleOf(EchoLine::kLP, 1000.0f, res, EchoLine::kInLoop), want, 1e-4 * want));
        CHECK(near(scaleOf(EchoLine::kHP, 3000.0f, res, EchoLine::kInLoop), want, 1e-4 * want));
    }
    // It is the filter's, for the routings: On output it is the same (the output is boosted either way).
    CHECK(near(scaleOf(EchoLine::kLP, 1000.0f, 1.0f, EchoLine::kOnOutput), 1.0 / peakOf(1.0), 1e-4));
    // Before the line has run (or reset) it is 1.
    e.reset();
    CHECK(e.loopScale() == 1.0f);

    // It glides: resonance 0 -> 1 at once, the scale chunk by chunk falls from 1 to 1/12 with no step. With the
    // filter on the resonance slews (a full scale in 10 ms) and the scale falls by a fifth a chunk at most. With the
    // filter coming on from bypassed the wet share fades in over 10 ms, and the scale with it, the steepest in the first
    // chunk (the wet share's first step): 1 to 0.55 in 32 samples, which is under a full scale per millisecond.
    for (int bypassed = 0; bypassed < 2; ++bypassed) {
        P p = plain(100.0f);
        p.cutoffHz = bypassed ? 20000.0f : 800.0f;
        e.reset();
        const Buf z = silence(32);
        Buf L, R;
        runLine(e, p, z, z, L, R);
        CHECK(e.loopScale() == 1.0f);
        p.cutoffHz = 800.0f;
        p.resonance = 1.0f;
        float last = 1.0f, worstRatio = 1.0f, worstDrop = 0.0f;
        bool falling = true;
        for (int chunk = 0; chunk < 40; ++chunk) {
            runLine(e, p, z, z, L, R);
            const float now = e.loopScale();
            worstRatio = std::min(worstRatio, now / last);
            worstDrop = std::max(worstDrop, last - now);
            falling = falling && now <= last;
            last = now;
        }
        CHECK(falling && worstDrop <= ef::kChunk / (0.001 * ef::kRate));   // no edge steeper than a full scale in 1 ms
        if (!bypassed) CHECK(worstRatio >= 0.8f);
        CHECK(near(last, 1.0 / peakOf(1.0), 1e-4));
    }

    // In loop the feedback has the scale, On output it hasn't. A 250 Hz tone in the low-pass's passband (1 kHz, Q 12:
    // gain 1.07 there) falls away 6.02 dB a repeat On output, and In loop that and the scale's 21.6 dB less the gain a pass;
    // the 1 kHz tone at the peak (+21.6 dB heard) is fed back at the feedback alone: 6.02 dB a repeat, either way.
    {
        const int T = 17640, len = 8820;   // 400 ms; 200 ms bursts: narrow against the filter's 83 Hz
        P p = plain(400.0f);
        p.feedback = 0.5f;
        p.filterType = EchoLine::kLP;
        p.cutoffHz = 1000.0f;
        p.resonance = 1.0f;
        const double passband = db(1.0 / std::sqrt(std::pow(1.0 - std::pow(std::tan(kPi * 250.0 / kSr) / std::tan(kPi * 1000.0 / kSr), 2.0), 2.0) +
                                                    std::pow(std::tan(kPi * 250.0 / kSr) / std::tan(kPi * 1000.0 / kSr) / qOf(1.0), 2.0)));
        for (int place = 0; place < 2; ++place) {
            p.place = place ? EchoLine::kOnOutput : EchoLine::kInLoop;
            double drops[2];
            double first[2];
            int i = 0;
            for (double hz : {250.0, 1000.0}) {
                const Buf in = concat(burst(hz, len, 0.5f), silence(4 * T));
                Buf L, R;
                e.reset();
                runLine(e, p, in, in, L, R);
                const double r1 = repeatDb(L, 1, T, len, hz), r2 = repeatDb(L, 2, T, len, hz);
                first[i] = r1;
                drops[i++] = r1 - r2;
            }
            CHECK(near(first[0], passband, 0.3));                  // heard: the filter's gain, in either place
            CHECK(first[1] > 18.0);                                // and its peak: +21.6 dB less the burst's width
            CHECK(near(drops[1], 6.02, 0.5));                      // the peak: the feedback alone
            if (place == 0) CHECK(near(drops[0], 6.02 + db(peakOf(1.0)) - passband, 0.4));   // In loop: the passband loses the scale too
            else CHECK(near(drops[0], 6.02, 0.3));                 // On output: the plain read, the feedback alone
        }
    }
}

void lineCheapReset() {  // reset() then a new impulse: nothing from before ever comes out
    Line e;
    for (int wow = 0; wow < 2; ++wow) {
        for (int glide = 0; glide < 2; ++glide) {
            P p = plain(2000.0f);   // the reads reach the oldest part of the line
            p.feedback = 0.5f;
            p.wow = static_cast<float>(wow);
            p.glide = glide;
            p.filterType = EchoLine::kLP;
            p.cutoffHz = 6000.0f;
            e.reset();
            // Fill the line all the way round with loud noise, and stop in the middle of a lap.
            const Buf loud = whiteNoise(4 * kSr + 1234, 0.9f, 7);
            Buf L, R;
            runLine(e, p, loud, loud, L, R);
            CHECK(peak(L) > 0.1f);
            // Now the new impulse, 100 samples in.
            e.reset();
            const int at = 100, n = 3 * kSr;
            const Buf in = impulseAt(n, at);
            runLine(e, p, in, in, L, R);
            const size_t echo = static_cast<size_t>(at + 2 * kSr);
            CHECK(peak(L, 0, echo - 400) < 1e-6f && peak(R, 0, echo - 400) < 1e-6f);   // silence until it comes back
            CHECK(peak(L, echo - 400, echo + 400) > 0.1f);
            // And again in the middle of a run, with the line only half written.
            e.reset();
            runLine(e, p, loud, loud, L, R, {}, 7);
            e.reset();
            const Buf in2 = impulseAt(kSr, 0);
            Buf L2, R2;
            runLine(e, p, in2, in2, L2, R2);
            CHECK(peak(L2) < 1e-6f);   // the new impulse hasn't come back yet; the old noise must not either
        }
    }
}

void lineNan() {         // NaN in the input: finite output, the loop never takes it
    Line a, b;
    P p = plain(120.0f);
    p.feedback = 0.8f;
    p.filterType = EchoLine::kBP;
    p.cutoffHz = 1500.0f;
    p.resonance = 0.4f;
    p.drive = 0.4f;
    p.wow = 0.5f;
    for (int mode = 0; mode < 3; ++mode) {
        p.mode = mode;
        const Buf inL = whiteNoise(60000, 0.5f, 11), inR = whiteNoise(60000, 0.5f, 12);
        Buf badL = inL, badR = inR, zeroL = inL, zeroR = inR;
        // NaN, +inf and -inf here and there in both channels, and a whole chunk of them; the reference
        // has silence in those places: a bad sample must be exactly that.
        for (size_t i = 100; i < 20000; i += 337) {
            badL[i] = NAN;
            zeroL[i] = 0.0f;
            badR[i + 3] = INFINITY;
            zeroR[i + 3] = 0.0f;
            badL[i + 5] = -INFINITY;
            zeroL[i + 5] = 0.0f;
        }
        for (size_t i = 25600; i < 25664; ++i) {
            badL[i] = NAN;
            badR[i] = -INFINITY;
            zeroL[i] = zeroR[i] = 0.0f;
        }
        Buf L1, R1, L2, R2;
        a.reset();
        b.reset();
        runLine(a, p, badL, badR, L1, R1);
        runLine(b, p, zeroL, zeroR, L2, R2);
        CHECK(allFinite(L1) && allFinite(R1) && a.peakStored() <= 1.00001f);
        CHECK(same(L1, L2) && same(R1, R2));
    }
    // A wild but finite sample is clamped before the limiter sees it: half a second later the tone is back
    // at its level (an unclamped 1e30 would hold the limiter down for seconds).
    {
        const Buf tone = sine(1000.0, kSr, 0.5f);
        Buf wild = tone;
        wild[100] = 1e30f;
        wild[101] = -3e38f;
        P q = plain(100.0f);
        Buf L1, L2, R;
        a.reset();
        runLine(a, q, tone, tone, L1, R);
        b.reset();
        runLine(b, q, wild, wild, L2, R);
        CHECK(allFinite(L2) && b.peakStored() <= 1.00001f);
        CHECK(near(rms(L2, 30000, 40000) / rms(L1, 30000, 40000), 1.0, 0.05));
        CHECK(rms(L2, 4510, 6000) < 0.5 * rms(L1, 30000, 40000));   // and it was held down, for a while
    }
}

void lineSilence() {     // R36: the loop never gains. Silence in is silence out, however resonant the filter and however high the feedback
    Line e;
    const int n = 30 * kSr;
    const Buf zero = silence(n);
    struct Case {
        int type;
        float cutoff;
    };
    // Low-pass and high-pass at resonance 1 (Q 12), feedback 0.5 and 1, at 1 ms and at 100 ms; the band-pass too.
    for (const Case& c : {Case{EchoLine::kLP, 800.0f}, Case{EchoLine::kHP, 3000.0f}, Case{EchoLine::kBP, 1000.0f}}) {
        for (float fb : {0.5f, 1.0f}) {
            for (float ms : {1.0f, 100.0f}) {
                P p = plain(ms);
                p.feedback = fb;
                p.filterType = c.type;
                p.cutoffHz = c.cutoff;
                p.resonance = 1.0f;
                e.reset();
                Buf L, R;
                runLine(e, p, zero, zero, L, R);
                const float loudest = std::max(peak(L), peak(R));
                if (!(loudest < 1e-12f)) std::printf("  type %d, feedback %g, %g ms: %g from silence\n", c.type, static_cast<double>(fb), static_cast<double>(ms), static_cast<double>(loudest));
                CHECK(loudest < 1e-12f);
            }
        }
    }
    // And at other cutoffs, the sharpest loop there is: 1 ms, feedback 1.
    for (int type : {EchoLine::kLP, EchoLine::kHP}) {
        for (float cutoff : {60.0f, 200.0f, 1000.0f, 5000.0f, 15000.0f}) {
            P p = plain(1.0f);
            p.feedback = 1.0f;
            p.filterType = type;
            p.cutoffHz = cutoff;
            p.resonance = 1.0f;
            e.reset();
            Buf L, R;
            runLine(e, p, silence(5 * kSr), silence(5 * kSr), L, R);
            CHECK(std::max(peak(L), peak(R)) < 1e-12f);
        }
    }
}

void lineHold() {        // R36: feedback 1 and resonance 1: after a noise burst the ring neither grows nor runs away for 60 s
    Line e;
    const int burstLen = kSr / 2, lap = 4410;
    const Buf burst = whiteNoise(burstLen, 0.02f, 31);
    for (int type : {EchoLine::kLP, EchoLine::kHP, EchoLine::kBP}) {
        P p = plain(100.0f);
        p.feedback = 1.0f;
        p.filterType = type;
        p.cutoffHz = 1000.0f;
        p.resonance = 1.0f;
        const Buf in = concat(burst, silence(60 * kSr - burstLen));
        Buf L, R;
        e.reset();
        runLine(e, p, in, in, L, R);
        const double early = rms(L, 5 * kSr, 15 * kSr), late = rms(L, 50 * kSr, 60 * kSr);
        // A loop that never gains holds at most the energy the burst put in: a lap of it at most, so its rms is
        // at most sqrt(burst / lap) times the burst's; heard through the filter's peak, 1.1 for slack.
        const double most = 1.1 * (type == EchoLine::kBP ? 1.0 : peakOf(1.0)) * std::sqrt(static_cast<double>(burstLen) / lap) * rms(burst);
        if (!(late <= 1.02 * early && late <= most)) std::printf("  type %d: rms %g early, %g late, at most %g\n", type, early, late, most);
        CHECK(allFinite(L) && e.peakStored() <= 1.00001f);
        CHECK(early > 0.0);
        CHECK(late <= 1.02 * early);                       // no growth
        CHECK(late <= most && early <= most);              // and nothing from nothing
        CHECK(peak(L) <= 1.3 * peakOf(1.0) * 1.00001);     // heard: what is stored through the filter's peak
    }
    // And with a cutoff the modulator keeps moving (3 Hz, +-2 octaves): a moving resonance must not pump energy in either.
    for (int type : {EchoLine::kLP, EchoLine::kHP}) {
        for (float ms : {1.0f, 100.0f}) {
            P p = plain(ms);
            p.feedback = 1.0f;
            p.filterType = type;
            p.cutoffHz = 1000.0f;
            p.resonance = 1.0f;
            const size_t n = static_cast<size_t>(20 * kSr);
            const Buf in = concat(burst, silence(static_cast<int>(n) - burstLen));
            Buf L(n), R(n);
            e.reset();
            for (size_t pos = 0; pos < n; pos += ef::kChunk) {
                p.cutoffModOct = static_cast<float>(2.0 * std::sin(2.0 * kPi * 3.0 * static_cast<double>(pos) / kSr));
                e.set(p, {});
                e.read(&L[pos], &R[pos], static_cast<int>(std::min<size_t>(ef::kChunk, n - pos)));
                e.write(&in[pos], &in[pos], static_cast<int>(std::min<size_t>(ef::kChunk, n - pos)));
            }
            const double most = 1.1 * peakOf(1.0) * std::sqrt(static_cast<double>(burstLen) / (ms * 44.1)) * rms(burst);
            const double late = rms(L, 15 * kSr, 20 * kSr);
            if (!(late <= most)) std::printf("  moving cutoff, type %d, %g ms: rms %g late, at most %g\n", type, static_cast<double>(ms), late, most);
            CHECK(allFinite(L) && e.peakStored() <= 1.00001f);
            CHECK(late <= most);
        }
    }
}

void lineDivisions() {   // the sync divisions: sorted, EffectWerk's 16 and the three odd ones
    CHECK(gf::kNumEchoDivs == 19);
    for (int i = 1; i < gf::kNumEchoDivs; ++i) CHECK(gf::kEchoDivs[i].beats > gf::kEchoDivs[i - 1].beats);
    CHECK(gf::kEchoDivs[0].beats == 0.0625 && std::strcmp(gf::kEchoDivs[0].name, "1/64") == 0);
    CHECK(gf::kEchoDivs[gf::kNumEchoDivs - 1].beats == 4.0 && std::strcmp(gf::kEchoDivs[gf::kNumEchoDivs - 1].name, "1 bar") == 0);
    for (int i = 0; i < ef::kNumDelayDivs; ++i) {
        bool found = false;
        for (int j = 0; j < gf::kNumEchoDivs; ++j) found = found || (gf::kEchoDivs[j].beats == ef::kDelayDivs[i].beats && std::strcmp(gf::kEchoDivs[j].name, ef::kDelayDivs[i].name) == 0);
        CHECK(found);
    }
    for (const char* odd : {"5/16", "7/16", "5/8"}) {
        bool found = false;
        for (int j = 0; j < gf::kNumEchoDivs; ++j) found = found || std::strcmp(gf::kEchoDivs[j].name, odd) == 0;
        CHECK(found);
    }
    // Every division is a time the line can play: the shortest and the longest at the tempos the Echo allows.
    Line e;
    for (int i = 0; i < gf::kNumEchoDivs; ++i) CHECK(echoes(e, synced(gf::kEchoDivs[i].beats), tempo(120.0), std::min(gf::kEchoDivs[i].beats * 0.5 * kSr, 3.0 * kSr)));
}

void lineAllocation() {  // the lines come with allocate(); before it read() is silence and write() does nothing
    EchoLine e;
    CHECK(!e.allocated());
    CHECK(sizeof(EchoLine) < 2048);   // no memory in the object itself
    P p = plain(100.0f);
    p.feedback = 0.5f;
    Buf L(64, 0.7f), R(64, 0.7f);
    e.set(p, {});
    e.read(L.data(), R.data(), 32);
    e.write(L.data(), R.data(), 32);
    e.read(L.data() + 32, R.data() + 32, 32);
    CHECK(peak(L) == 0.0f && peak(R) == 0.0f);
    CHECK(e.peakStored() == 0.0f);
    // Allocated, it plays; a second allocate() is no harm.
    CHECK(e.allocate() && e.allocated() && e.allocate());
    const Buf in = impulseAt(10000, 100);
    Buf oL, oR;
    runLine(e, p, in, in, oL, oR);
    CHECK(near(oL[100 + 4410], h0(), 1e-5) && near(oL[100 + 8820], 0.5 * h0() * h0(), 1e-5));
    // A write without a read before it takes nothing.
    e.reset();
    e.set(p, {});
    const float one[32] = {1.0f};
    e.write(one, one, 32);
    float a[32], b[32];
    e.read(a, b, 32);
    CHECK(peak(Buf(a, a + 32)) == 0.0f);
}

void lineChunks() {      // what comes out does not depend on how the chunks are cut
    P a = plain(180.0f);
    a.feedback = 0.6f;
    a.mode = EchoLine::kPingPong;
    a.filterType = EchoLine::kBP;
    a.cutoffHz = 800.0f;
    a.resonance = 0.3f;
    a.drive = 0.3f;
    a.wow = 0.5f;
    a.glide = EchoLine::kFade;
    // The shortest time with the deepest wow: the reads come as near as they ever do. A chunk's reads
    // must still be of what was written before it, so cutting the chunks finer changes nothing.
    P b = plain(1.0f);
    b.feedback = 0.5f;
    b.wow = 1.0f;
    b.filterType = EchoLine::kLP;
    b.cutoffHz = 5000.0f;
    // A resonant loop: the scale glides with the filter, and that must not depend on the cutting either.
    P c = plain(60.0f);
    c.feedback = 0.9f;
    c.filterType = EchoLine::kLP;
    c.cutoffHz = 900.0f;
    c.resonance = 0.8f;
    const Buf in = whiteNoise(2 * kSr, 0.5f, 4);
    for (const P& p : {a, b, c}) {
        Line ref;
        Buf rl, rr;
        runLine(ref, p, in, in, rl, rr);
        for (int chunk : {20, 7, 1}) {
            Line e;
            Buf L, R;
            runLine(e, p, in, in, L, R, {}, chunk);
            const double d = std::max(maxDiff(L, rl), maxDiff(R, rr));
            if (d > 1e-6) std::printf("  chunks of %d: differ by %g\n", chunk, d);
            CHECK(d < 1e-6);
        }
    }
}

void lineMoves() {       // controls slew: nothing clicks when the filter, the feedback, the drive, the mode or the wow jump
    Line e;
    const size_t seg = 32 * 700;   // a chunk boundary every half second
    const Buf tone = sine(1000.0, 17 * static_cast<int>(seg), 0.05f);
    P base = plain(100.0f);
    base.feedback = 0.5f;
    std::vector<P> steps;
    steps.push_back(base);
    P q = base;
    q.filterType = EchoLine::kHP; q.cutoffHz = 3000.0f; q.resonance = 0.5f;
    steps.push_back(q);                                           // off to on, LP to HP
    q.place = EchoLine::kOnOutput;
    steps.push_back(q);                                           // In loop to On output
    q.filterType = EchoLine::kBP; q.cutoffHz = 1000.0f;
    steps.push_back(q);
    q.filterType = EchoLine::kLP; q.cutoffHz = 200.0f;
    steps.push_back(q);
    q.cutoffHz = 20000.0f;
    steps.push_back(q);                                           // back to off
    q.place = EchoLine::kInLoop; q.filterType = EchoLine::kHP; q.cutoffHz = 20.0f; q.resonance = 0.0f;
    steps.push_back(q);
    q = base;
    steps.push_back(q);
    q.feedback = 0.7f;
    steps.push_back(q);
    q.feedback = 0.0f;
    steps.push_back(q);
    q.feedback = 0.5f; q.drive = 1.0f;
    steps.push_back(q);
    q.drive = 0.0f;
    steps.push_back(q);
    q.mode = EchoLine::kPingPong;
    steps.push_back(q);
    q.mode = EchoLine::kMono;
    steps.push_back(q);
    q.mode = EchoLine::kStereo; q.wow = 1.0f;
    steps.push_back(q);
    q.wow = 0.0f;
    steps.push_back(q);
    q.timeMod = 1.3f;
    steps.push_back(q);                                           // Tape glides to the new time
    Buf L(tone.size()), R(tone.size());
    for (size_t s = 0; s < steps.size(); ++s) runLine(e, steps[s], tone.data() + s * seg, tone.data() + s * seg, L.data() + s * seg, R.data() + s * seg, seg);
    // After each move, 200 ms of output: against the tone's own slope (2 pi 1 kHz / rate = 0.14 of its
    // peak) nothing may be steeper than 1.5 times that, whatever the level has become. The filters ring
    // and swell a little (Q up to 2), the time bends; nothing jumps.
    double worst = 0.0;
    for (size_t s = 1; s < steps.size(); ++s)
        for (const Buf* x : {&L, &R}) {
            const float top = peak(*x, s * seg - 1, s * seg + 9000);
            if (top > 0.01f) worst = std::max(worst, maxStep(*x, s * seg - 1, s * seg + 9000) / static_cast<double>(top));
        }
    CHECK(allFinite(L) && allFinite(R) && worst <= 1.5 * 2.0 * kPi * 1000.0 / kSr);
}

void lineExtremes() {    // any parameters, any input: finite, the lines under 0 dBFS
    Line e;
    uint32_t seed = 12345;
    const auto rnd = [&seed](float lo, float hi) { return lo + (hi - lo) * (0.5f + 0.5f * ef::randBipolar(seed)); };
    const auto pick = [&](std::initializer_list<float> v) {
        const size_t k = static_cast<size_t>((0.5f + 0.5f * ef::randBipolar(seed)) * static_cast<float>(v.size())) % v.size();
        return *(v.begin() + k);
    };
    bool ok = true;
    float loudestOut = 0.0f;
    const int chunks = 5 * kSr / ef::kChunk;
    Buf in = whiteNoise(ef::kChunk, 1.0f, 3), L(ef::kChunk), R(ef::kChunk);
    for (int c = 0; c < chunks; ++c) {
        P p;
        p.sync = rnd(0.0f, 1.0f) > 0.5f;
        p.divBeats = static_cast<double>(pick({rnd(-1.0f, 8.0f), NAN, INFINITY, -INFINITY, 0.0f, 0.75f}));
        p.timeMs = pick({rnd(-10.0f, 3000.0f), NAN, INFINITY, -INFINITY, 1.0f, 2000.0f});
        p.feedback = pick({rnd(-0.5f, 1.5f), NAN, INFINITY, 1.0f, 0.0f});
        p.mode = static_cast<int>(rnd(-3.0f, 7.0f));
        p.filterType = static_cast<int>(rnd(-3.0f, 6.0f));
        p.cutoffHz = pick({rnd(-100.0f, 40000.0f), NAN, INFINITY, 0.0f, 20000.0f});
        p.resonance = pick({rnd(-1.0f, 2.0f), NAN, 1.0f});
        p.place = static_cast<int>(rnd(-2.0f, 4.0f));
        p.drive = pick({rnd(-1.0f, 2.0f), NAN, 1.0f});
        p.wow = pick({rnd(-1.0f, 2.0f), NAN, 1.0f});
        p.glide = static_cast<int>(rnd(-2.0f, 4.0f));
        p.timeMod = pick({rnd(-5.0f, 100.0f), NAN, INFINITY, 1.0f});
        p.cutoffModOct = pick({rnd(-30.0f, 30.0f), NAN, INFINITY, -INFINITY, 0.0f});
        ef::Transport t;
        t.bpm = static_cast<double>(pick({rnd(-10.0f, 400.0f), NAN, INFINITY, 0.0f, 120.0f}));
        for (int i = 0; i < ef::kChunk; ++i) {
            in[static_cast<size_t>(i)] = ef::randBipolar(seed);
            const float r = rnd(0.0f, 1.0f);
            if (r > 0.995f) in[static_cast<size_t>(i)] = pick({NAN, INFINITY, -INFINITY, 1e30f, -3e38f});
        }
        const int m = 1 + static_cast<int>(rnd(0.0f, static_cast<float>(ef::kChunk))) % ef::kChunk;
        e.set(p, t);
        e.read(L.data(), R.data(), m);
        e.write(in.data(), in.data(), m);
        for (int i = 0; i < m; ++i) {
            ok = ok && std::isfinite(L[static_cast<size_t>(i)]) && std::isfinite(R[static_cast<size_t>(i)]);
            loudestOut = std::max({loudestOut, std::fabs(L[static_cast<size_t>(i)]), std::fabs(R[static_cast<size_t>(i)])});
        }
        ok = ok && e.tailSamples() >= 0;
    }
    CHECK(ok);
    CHECK(loudestOut <= 1.3 * peakOf(1.0));   // the stored 1, through the sharpest filter's peak and the Hermite kernel
    CHECK(e.peakStored() <= 1.00001f);
}

void lineTail() {        // the tail: silence after it, 1 for ever at feedback 1
    Line e;
    for (int ring = 0; ring < 2; ++ring) {
        P p = plain(100.0f);
        p.feedback = ring ? 0.3f : 0.5f;
        if (ring) {   // a ringing resonance On output
            p.filterType = EchoLine::kLP;
            p.cutoffHz = 100.0f;
            p.resonance = 1.0f;
            p.place = EchoLine::kOnOutput;
        }
        e.reset();
        e.set(p, {});
        const size_t tail = static_cast<size_t>(e.tailSamples());
        CHECK(tail > 4410 && tail < 6 * static_cast<size_t>(kSr));
        const Buf in = impulseAt(static_cast<int>(tail) + 4 * kSr, 0);
        Buf L, R;
        runLine(e, p, in, in, L, R);
        CHECK(peak(L, tail, L.size()) < 2e-3f);
        CHECK(peak(L, 0, tail) > 0.01f);
    }
    // A resonant filter In loop (R36): the loop gain at the peak is the feedback, so the repeats still die away
    // and the tail covers them, ring and all: 60 dB down and more after it. Short lines, slow rings, feedbacks up to .99.
    struct Case {
        int type;
        float cutoff, ms, fb;
    };
    for (const Case& c : {Case{EchoLine::kLP, 300.0f, 50.0f, 0.5f}, Case{EchoLine::kLP, 300.0f, 50.0f, 0.9f}, Case{EchoLine::kHP, 2000.0f, 50.0f, 0.5f},
                          Case{EchoLine::kHP, 2000.0f, 50.0f, 0.9f}, Case{EchoLine::kLP, 60.0f, 10.0f, 0.5f},  Case{EchoLine::kHP, 8000.0f, 1.0f, 0.99f},
                          Case{EchoLine::kBP, 700.0f, 100.0f, 0.8f}}) {
        P p = plain(c.ms);
        p.feedback = c.fb;
        p.filterType = c.type;
        p.cutoffHz = c.cutoff;
        p.resonance = 1.0f;
        e.reset();
        e.set(p, {});
        const size_t tail = static_cast<size_t>(e.tailSamples());
        CHECK(tail > 2205 && tail < 40 * static_cast<size_t>(kSr));
        const Buf in = impulseAt(static_cast<int>(tail) + 4 * kSr, 0);
        Buf L, R;
        runLine(e, p, in, in, L, R);
        const float before = peak(L, 0, tail), after = peak(L, tail, L.size());
        if (!(after < 2e-3f * before)) std::printf("  type %d, %g Hz, %g ms, feedback %g: %g after the tail (%zu samples), %g before\n", c.type, static_cast<double>(c.cutoff), static_cast<double>(c.ms), static_cast<double>(c.fb), static_cast<double>(after), tail, static_cast<double>(before));
        CHECK(before > 0.0f && after < 2e-3f * before);
    }
    // Long after the tail the line holds nothing but its tiny offset (-360 dB): nothing decays into denormals.
    {
        P q = plain(100.0f);
        q.feedback = 0.9f;
        q.filterType = EchoLine::kBP;
        q.cutoffHz = 700.0f;
        e.reset();
        const Buf in = impulseAt(40 * kSr, 0);
        Buf L, R;
        runLine(e, q, in, in, L, R);
        CHECK(e.peakStored() > 1e-19f && e.peakStored() < 1e-12f);
        CHECK(peak(L, 39 * kSr, 40 * kSr) < 1e-12f);
    }
    P p = plain(100.0f);
    p.feedback = 1.0f;
    e.reset();
    e.set(p, {});
    CHECK(e.tailSamples() >= (1 << 30));
    p.feedback = 0.0f;
    e.set(p, {});
    CHECK(e.tailSamples() >= 4410 && e.tailSamples() < kSr);
    // Below 1 it is finite whatever the setting (R36: below 1 the repeats always die away).
    p.feedback = 0.9999f;
    p.filterType = EchoLine::kLP;
    p.cutoffHz = 20.0f;
    p.resonance = 1.0f;
    e.set(p, {});
    CHECK(e.tailSamples() > kSr && e.tailSamples() < (1 << 30));
}

} // namespace

void echoLineTests() {
    lineTime();
    lineFeedback();
    lineInLoop();
    linePingPong();
    lineModes();
    lineGlide();
    lineDriveBounded();
    lineDrive();
    lineNearest();
    lineReadWrite();
    lineCheapReset();
    lineNan();
    lineDivisions();
    lineAllocation();
    lineChunks();
    lineMoves();
    lineExtremes();
    lineTail();
    lineSilence();
    lineHold();
    lineLoopScale();
}

// make test-module M=echo_line builds with -DMODULE_TESTS=<M>Tests, i.e. echo_lineTests; the suite
// (main.cpp) calls echoLineTests.
void echo_lineTests() { echoLineTests(); }
