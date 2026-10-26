// gf::Crossover (dsp/crossover.h): the Linkwitz-Riley split the banded Pump ducks one side of.
#include "signal.h"
#include "../dsp/crossover.h"

#include <cmath>
#include <limits>

namespace {

using namespace eft;

constexpr int kSettle = 8192;   // samples to let the filters settle before measuring
constexpr int kLen = 32768;     // and to measure over (20 Hz still gets seven cycles)

// What the crossover made of a mono signal (the same in L and R).
struct Split {
    Buf low, high;
    Buf sum() const {
        Buf s(low.size());
        for (size_t i = 0; i < s.size(); ++i) s[i] = low[i] + high[i];
        return s;
    }
};

// `in` through the crossover in chunks (kChunk, as the rack does), set(hzAt(position)) before each.
template <class F>
Split splitWith(gf::Crossover& x, const Buf& in, F hzAt, int chunk = ef::kChunk) {
    Split s;
    s.low.assign(in.size(), 0.0f);
    s.high = s.low;
    Buf lowR(in.size()), highR(in.size());
    for (size_t pos = 0; pos < in.size(); pos += static_cast<size_t>(chunk)) {
        const int n = static_cast<int>(std::min(static_cast<size_t>(chunk), in.size() - pos));
        x.set(hzAt(pos));
        x.process(&in[pos], &in[pos], &s.low[pos], &lowR[pos], &s.high[pos], &highR[pos], n);
    }
    return s;
}

Split splitAt(float hz, const Buf& in) {
    gf::Crossover x;
    x.reset();
    return splitWith(x, in, [hz](size_t) { return hz; });
}

// A band's gain in dB at hz, once settled.
double gainDb(const Buf& band, const Buf& in, double hz) {
    return db(magnitude(band, hz, kSettle) / magnitude(in, hz, kSettle));
}

// A sine at hz through the crossover at xo: the band gains in dB (low, high, sum).
struct Gains {
    double low, high, sum;
};
Gains gainsAt(float xo, double hz) {
    const Buf in = sine(hz, kSettle + kLen, 0.25f);
    const Split s = splitAt(xo, in);
    return {gainDb(s.low, in, hz), gainDb(s.high, in, hz), gainDb(s.sum(), in, hz)};
}

void crossoverFlat() {   // low + high is an allpass: flat from 20 Hz to 20 kHz, at the ends of the range too
    const double freqs[] = {20, 30, 45, 60, 80, 100, 120, 150, 200, 300, 480, 1000, 2000, 5000, 10000, 15000, 20000};
    for (const float xo : {60.0f, 120.0f, 300.0f})
        for (const double hz : freqs) CHECK(std::fabs(gainsAt(xo, hz).sum) < 0.1);
}

void crossoverSlopes() {   // each side -6 dB at the crossover, 4th order (24 dB / octave) away from it
    const Gains at = gainsAt(120.0f, 120.0);
    CHECK(std::fabs(at.low + 6.02) < 0.3 && std::fabs(at.high + 6.02) < 0.3);
    // Two octaves out an ideal Linkwitz-Riley 4th order is -48.2 dB (1 / sqrt(1 + 4^8)): below -48, and
    // not much below it (a steeper filter, or a lower cutoff, would not be this one).
    const double low480 = gainsAt(120.0f, 480.0).low, high30 = gainsAt(120.0f, 30.0).high;
    CHECK(low480 <= -48.0 && low480 > -48.7);
    CHECK(high30 <= -48.0 && high30 > -48.7);
    // The passbands pass: the low side below the crossover, the high side above it.
    CHECK(std::fabs(gainsAt(120.0f, 20.0).low) < 0.1);
    CHECK(std::fabs(gainsAt(120.0f, 5000.0).high) < 0.1);
    // The crossover follows set(): the same -6 dB point at both ends of the range.
    for (const float xo : {60.0f, 300.0f}) {
        const Gains g = gainsAt(xo, xo);
        CHECK(std::fabs(g.low + 6.02) < 0.3 && std::fabs(g.high + 6.02) < 0.3);
    }
}

void crossoverMoves() {   // set() sweeping 60 -> 300 Hz over 0.5 s, and back: finite, no click
    const int n = 22050;
    const auto up = [n](size_t pos) { return 60.0f + 240.0f * static_cast<float>(pos) / static_cast<float>(n); };
    const auto down = [n](size_t pos) { return 300.0f - 240.0f * static_cast<float>(pos) / static_cast<float>(n); };
    for (const double hz : {100.0, 200.0, 1000.0}) {   // below, at and above the sweep
        const Buf in = sine(hz, n, 0.5f);
        const float ref = maxStep(in);
        for (const bool rising : {true, false}) {
            gf::Crossover x;
            x.reset();
            const Split s = rising ? splitWith(x, in, up) : splitWith(x, in, down);
            const Buf sum = s.sum();
            CHECK(allFinite(s.low) && allFinite(s.high));
            // No band steps further than the sine itself does: a click would, and a sweep only
            // takes level away or moves the phase by a fraction of a step.
            CHECK(maxStep(s.low) <= 1.25f * ref && maxStep(s.high) <= 1.25f * ref && maxStep(sum) <= 1.25f * ref);
            // And none rises above the sine's level (past the filters' start: a sine begins at zero,
            // but not smoothly enough for them not to ring a little).
            CHECK(peak(s.low, 2000) <= 0.55f && peak(s.high, 2000) <= 0.55f && peak(sum, 2000) <= 0.55f);
        }
    }
}

// set() leaping across the whole range every 68 ms. No click is promised (the allpass's phase turns within a
// chunk, and a tone near the crossover swells for a moment), but it stays finite and bounded.
void crossoverJumps() {
    const int n = 22050;
    const auto leap = [](size_t pos) { return (pos / 3000) % 2 ? 300.0f : 60.0f; };
    for (const double hz : {100.0, 200.0, 1000.0}) {
        gf::Crossover x;
        x.reset();
        const Split s = splitWith(x, sine(hz, n, 0.5f), leap);
        CHECK(allFinite(s.low) && allFinite(s.high));
        CHECK(peak(s.low) <= 1.5f && peak(s.high) <= 1.5f && peak(s.sum()) <= 1.5f);
    }
}

void crossoverGlides() {   // a new cutoff is reached over a chunk, not in one sample; then it is the new filter
    const size_t n = 12288, at = 4096;
    const Buf in = sine(100.0, static_cast<int>(n), 0.5f);
    gf::Crossover x;
    x.reset();
    const Split moved = splitWith(x, in, [at](size_t pos) { return pos < at ? 60.0f : 300.0f; });
    const Split stayed = splitAt(60.0f, in), there = splitAt(300.0f, in);
    const auto apart = [&](size_t k) { return std::fabs(moved.high[at + k] - stayed.high[at + k]); };
    CHECK(apart(0) < 0.001f);   // the first sample is still the old filter's, near enough
    CHECK(apart(31) > 0.02f);   // and the chunk's end is well on its way
    float worst = 0.0f;         // a few thousand samples on, nothing of the old cutoff is left
    for (size_t i = 10000; i < n; ++i) {
        worst = std::max(worst, std::fabs(moved.low[i] - there.low[i]));
        worst = std::max(worst, std::fabs(moved.high[i] - there.high[i]));
    }
    CHECK(worst < 1e-4f);
}

void crossoverLanes() {   // L and R are separate: silence on one side stays silent
    const Buf in = sine(200.0, 4096, 0.5f), zero(4096, 0.0f);
    gf::Crossover x;
    x.reset();
    Buf lowL(4096), lowR(4096), highL(4096), highR(4096);
    for (size_t pos = 0; pos < in.size(); pos += static_cast<size_t>(ef::kChunk)) {
        x.set(120.0f);
        x.process(&in[pos], &zero[pos], &lowL[pos], &lowR[pos], &highL[pos], &highR[pos], ef::kChunk);
    }
    CHECK(peak(lowL) > 0.1f && peak(highL) > 0.1f);
    CHECK(peak(lowR) == 0.0f && peak(highR) == 0.0f);
}

void crossoverInPlace() {   // the outputs may be the inputs; a reset crossover starts over, bit for bit
    const Buf in = sine(150.0, 2048, 0.5f);
    const Split ref = splitAt(120.0f, in);
    Buf l, r, highL(in.size()), highR(in.size());
    gf::Crossover x;
    for (int round = 0; round < 2; ++round) {
        x.reset();
        l = r = in;
        for (size_t pos = 0; pos < in.size(); pos += static_cast<size_t>(ef::kChunk)) {
            x.set(120.0f);
            x.process(&l[pos], &r[pos], &l[pos], &r[pos], &highL[pos], &highR[pos], ef::kChunk);   // the lows in place
        }
        CHECK(l == ref.low && r == ref.low && highL == ref.high && highR == ref.high);
    }
}

void crossoverChunks() {   // a chunk of any length from 1 to kChunk: the same samples as with 32
    const Buf in = sine(150.0, 2048, 0.5f);
    const Split ref = splitAt(120.0f, in);
    for (const int chunk : {1, 7, 31}) {
        gf::Crossover x;
        x.reset();
        const Split s = splitWith(x, in, [](size_t) { return 120.0f; }, chunk);
        CHECK(s.low == ref.low && s.high == ref.high);
    }
}

void crossoverSafe() {   // any input and any set() leaves finite output; bad input never lodges in the filters
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    for (const float hz : {nan, inf, -inf, -5.0f, 0.0f, 1e9f, 59.0f, 301.0f}) {
        const Buf in = sine(100.0, 4096, 0.5f);
        const Split s = splitAt(hz, in);
        CHECK(allFinite(s.low) && allFinite(s.high));
        CHECK(std::fabs(db(magnitude(s.sum(), 100.0, 2048) / 0.5)) < 0.2);   // and still a flat split
    }
    // set() clamps to 60..300: the same split as at the edge.
    const Buf in = sine(100.0, 4096, 0.5f);
    CHECK(splitAt(1.0f, in).low == splitAt(60.0f, in).low && splitAt(1e6f, in).high == splitAt(300.0f, in).high);

    Buf bad = sine(100.0, 4096, 0.5f);
    for (size_t i = 1000; i < 1100; ++i) bad[i] = i % 3 == 0 ? nan : (i % 3 == 1 ? inf : -inf);
    bad[2000] = 3e38f;
    const Split s = splitAt(120.0f, bad);
    CHECK(allFinite(s.low) && allFinite(s.high));
    CHECK(peak(s.low, 3000) < 0.6f && peak(s.high, 3000) < 0.6f);   // and it has recovered by the end
}

} // namespace

void crossoverTests() {
    crossoverFlat();
    crossoverSlopes();
    crossoverMoves();
    crossoverJumps();
    crossoverGlides();
    crossoverLanes();
    crossoverInPlace();
    crossoverChunks();
    crossoverSafe();
}
