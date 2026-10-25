// dsp/transient.h: the onset detector behind Pump's Input trigger.
#include "signal.h"
#include "../dsp/transient.h"

namespace {

using namespace eft;
using gf::TransientDetector;

constexpr int kHoldoffSamples = 1764;   // 40 ms at 44.1 kHz
constexpr int kStartup = 2205;          // 50 ms: a signal that starts out of silence is an onset itself

// x += a hit: a decaying sine starting at `at` (a kick at 55 Hz, 60 ms; a short tom or snare-like hit above).
void addHit(Buf& x, int at, double hz, double decayMs, float amp) {
    const double tau = decayMs * 0.001 * ef::kRate;
    for (int i = 0; at + i < static_cast<int>(x.size()) && i < 10 * tau; ++i)
        x[static_cast<size_t>(at + i)] += amp * static_cast<float>(std::sin(2.0 * kPi * hz * i / ef::kRate) * std::exp(-i / tau));
}
void addKick(Buf& x, int at, float amp) { addHit(x, at, 55.0, 60.0, amp); }

// Where each onset is (sample numbers), the signal fed in chunks of `chunk` samples.
std::vector<int> onsets(TransientDetector& d, const Buf& L, const Buf& R, int chunk = ef::kChunk) {
    std::vector<int> found;
    for (size_t pos = 0; pos < L.size(); pos += static_cast<size_t>(chunk)) {
        const int n = static_cast<int>(std::min(static_cast<size_t>(chunk), L.size() - pos));
        const int at = d.process(&L[pos], &R[pos], n);
        CHECK(at >= -1 && at < n);
        if (at >= 0) found.push_back(static_cast<int>(pos) + at);
    }
    return found;
}
// The same without the start: what a signal arriving out of silence makes of the first 50 ms.
std::vector<int> onsetsAfterStart(TransientDetector& d, const Buf& L, const Buf& R, int chunk = ef::kChunk) {
    std::vector<int> found = onsets(d, L, R, chunk);
    found.erase(std::remove_if(found.begin(), found.end(), [](int at) { return at < kStartup; }), found.end());
    return found;
}

// The start sample of beat k at 128 BPM, `lead` samples in.
int beatStart(int lead, int k) { return lead + static_cast<int>(std::lround(k * 60.0 / 128.0 * ef::kRate)); }

// A kick every beat at 128 BPM over white noise, both sides: `noiseDb` under the kick's peak (as RMS).
void kickLoop(Buf& L, Buf& R, int lead, int beats, float noiseDb) {
    const float kick = 0.8f;
    const int n = beatStart(lead, beats);
    const float amp = kick * ef::dbToGain(noiseDb) * 1.7320508f;   // a uniform noise's RMS is its amplitude / sqrt(3)
    L = whiteNoise(n, amp, 1);
    R = whiteNoise(n, amp, 2);
    for (int k = 0; k < beats; ++k) {
        addKick(L, beatStart(lead, k), kick);
        addKick(R, beatStart(lead, k), kick);
    }
}

void transientKicks() {   // a kick (55 Hz, 60 ms decay) every beat at 128 BPM over soft noise -30 dB under it:
    // every kick found within 2 ms of its start, nothing else, over 16 beats (sensitivity 0.5)
    const int lead = 8820, beats = 16;
    Buf L, R;
    kickLoop(L, R, lead, beats, -30.0f);
    TransientDetector d;   // the default sensitivity
    const std::vector<int> found = onsetsAfterStart(d, L, R);
    CHECK(static_cast<int>(found.size()) == beats);
    for (int k = 0; k < beats && k < static_cast<int>(found.size()); ++k) {
        const int late = found[static_cast<size_t>(k)] - beatStart(lead, k);
        CHECK(late >= 0 && late <= 88);   // 2 ms
    }
    for (size_t k = 1; k < found.size(); ++k) CHECK(found[k] - found[k - 1] >= kHoldoffSamples);

    // The same wherever the chunks end, and after a reset; setSensitivity(0.5) is the default.
    d.reset();
    d.setSensitivity(0.5f);
    CHECK(onsetsAfterStart(d, L, R, 7) == found);
    d.reset();
    CHECK(onsetsAfterStart(d, L, R, 1) == found);

    // It follows the louder side: a kick on one side alone is the same kick.
    Buf Z(L.size(), 0.0f);
    d.reset();
    const std::vector<int> left = onsetsAfterStart(d, L, Z);
    d.reset();
    const std::vector<int> right = onsetsAfterStart(d, Z, L);
    CHECK(static_cast<int>(left.size()) == beats && left == right);
    for (int k = 0; k < beats && k < static_cast<int>(left.size()); ++k) {
        const int late = left[static_cast<size_t>(k)] - beatStart(lead, k);
        CHECK(late >= 0 && late <= 88);
    }
}

void transientSteady() {  // a steady sine, steady white noise, a slow 2 s swell: no onsets after the first 50 ms
    const int n = 88200;
    TransientDetector d;
    const Buf s = sine(440.0, n, 0.5f);
    const Buf noiseL = whiteNoise(n, 0.5f, 3), noiseR = whiteNoise(n, 0.5f, 4);
    for (float keen : {0.5f, 1.0f}) {   // even at the keenest, steady is steady
        d.reset();
        d.setSensitivity(keen);
        CHECK(onsetsAfterStart(d, s, s).empty());
        d.reset();
        CHECK(onsetsAfterStart(d, noiseL, noiseR).empty());
    }
    d.reset();
    d.setSensitivity(0.5f);

    d.reset();
    Buf swell = sine(220.0, n, 0.5f);   // up from nothing to 0.5 over the 2 s
    for (int i = 0; i < n; ++i) swell[static_cast<size_t>(i)] *= static_cast<float>(i) / static_cast<float>(n);
    CHECK(onsetsAfterStart(d, swell, swell).empty());

    d.reset();
    Buf fade = s;   // and back down, to the point of silence
    for (int i = 0; i < n; ++i) fade[static_cast<size_t>(i)] *= 1.0f - static_cast<float>(i) / static_cast<float>(n);
    CHECK(onsetsAfterStart(d, fade, fade).empty());

    d.reset();
    const Buf silence(n, 0.0f);   // silence is no onset either
    CHECK(onsets(d, silence, silence).empty());
}

void transientHoldoff() { // a flam of two hits 20 ms apart: one onset; 60 ms apart: two
    TransientDetector d;
    const int n = 22050, at = 4410;
    for (int gapMs : {20, 60}) {
        Buf x(static_cast<size_t>(n), 0.0f);
        addHit(x, at, 180.0, 12.0, 0.6f);
        addHit(x, at + gapMs * 44, 180.0, 12.0, 0.6f);
        d.reset();
        const std::vector<int> found = onsets(d, x, x);
        if (gapMs == 20) {
            CHECK(found.size() == 1);
        } else {
            CHECK(found.size() == 2);
            if (found.size() == 2) CHECK(found[1] - found[0] >= kHoldoffSamples);
        }
        if (!found.empty()) CHECK(found[0] >= at && found[0] <= at + 88);
    }

    // To the sample: a click that comes before the holdoff is over still has its envelope up when it
    // ends, and counts then: the second onset is never closer than 1764 samples (40 ms) to the first.
    for (int gap : {kHoldoffSamples - 1, kHoldoffSamples, kHoldoffSamples + 1000}) {
        Buf x(static_cast<size_t>(n), 0.0f);
        x[static_cast<size_t>(at)] = 0.5f;
        x[static_cast<size_t>(at + gap)] = 0.5f;
        d.reset();
        const std::vector<int> found = onsets(d, x, x);
        CHECK(found.size() == 2);
        if (found.size() == 2) CHECK(found[0] == at && found[1] == at + std::max(gap, kHoldoffSamples));
    }
    CHECK(TransientDetector::kHoldoffMs == 40.0f);
}

void transientSensitivity() {   // hits -24 dB under the loudest: found at 0.9, not at 0.1
    // A groove at 128 BPM: a loud kick on beats 1 and 3, a ghost kick 24 dB under it on 2 and 4, a
    // beat after the loud one, whose tail is still in the slow envelope: the ghost rises to about
    // twice that, past the 1.55 of sensitivity 0.9 and short of the 3.6 of 0.1.
    const int lead = 4410, beats = 8;
    const int n = beatStart(lead, beats);
    Buf x(static_cast<size_t>(n), 0.0f);
    for (int k = 0; k < beats; ++k) addKick(x, beatStart(lead, k), k % 2 ? 0.8f * ef::dbToGain(-24.0f) : 0.8f);
    TransientDetector d;
    d.setSensitivity(0.9f);
    const std::vector<int> keen = onsets(d, x, x);
    d.reset();
    d.setSensitivity(0.1f);
    const std::vector<int> dull = onsets(d, x, x);
    CHECK(static_cast<int>(keen.size()) == beats);       // every kick and every ghost
    for (int k = 0; k < beats && k < static_cast<int>(keen.size()); ++k) {
        const int late = keen[static_cast<size_t>(k)] - beatStart(lead, k);
        CHECK(late >= 0 && late <= 441);   // the ghost's within 10 ms: the onset is its own
    }
    CHECK(static_cast<int>(dull.size()) == beats / 2);   // the loud kicks alone
    for (int k = 0; k < beats / 2 && k < static_cast<int>(dull.size()); ++k) {
        const int late = dull[static_cast<size_t>(k)] - beatStart(lead, 2 * k);
        CHECK(late >= 0 && late <= 88);
    }

    // Nothing under -50 dBFS counts, however keen; a hit just over it does, however dull.
    Buf quiet(22050, 0.0f), audible(22050, 0.0f);
    addKick(quiet, 4410, 0.0025f);     // -52 dBFS
    addKick(audible, 4410, 0.0063f);   // -44 dBFS
    d.reset();
    d.setSensitivity(1.0f);
    CHECK(onsets(d, quiet, quiet).empty());
    d.reset();
    d.setSensitivity(0.0f);
    CHECK(onsets(d, audible, audible).size() == 1);

    // Out of range is clamped to the two ends, 0 and 1; not a number counts as 0.
    d.reset();
    d.setSensitivity(-5.0f);
    const std::vector<int> least = onsets(d, x, x);
    d.reset();
    d.setSensitivity(0.0f);
    CHECK(onsets(d, x, x) == least);
    d.reset();
    d.setSensitivity(7.0f);
    const std::vector<int> most = onsets(d, x, x);
    d.reset();
    d.setSensitivity(1.0f);
    CHECK(onsets(d, x, x) == most);
    d.reset();
    d.setSensitivity(std::nanf(""));
    CHECK(onsets(d, x, x) == least);
    CHECK(most.size() >= keen.size() && keen.size() >= dull.size() && dull.size() >= least.size());
}

void transientNan() {     // a NaN or inf in the input: no onset from it, still finite and finding the next kick
    // Silence with bad samples only: nothing to find, from the first sample.
    const int n = 22050;
    Buf L(static_cast<size_t>(n), 0.0f), R(static_cast<size_t>(n), 0.0f);
    const float bad[] = {std::nanf(""), INFINITY, -INFINITY};
    for (int i = 0; i < 6; ++i) {
        L[static_cast<size_t>(i * 1000)] = bad[i % 3];
        R[static_cast<size_t>(i * 1000 + 3)] = bad[(i + 1) % 3];
    }
    for (int i = 0; i < 40; ++i) L[static_cast<size_t>(9000 + i)] = bad[i % 3];   // a burst
    TransientDetector d;
    CHECK(onsets(d, L, R).empty());

    // And then a kick: the detector is alive and still finds it.
    addKick(L, 12000, 0.8f);
    addKick(R, 12000, 0.8f);
    d.reset();
    const std::vector<int> found = onsets(d, L, R);
    CHECK(found.size() == 1);
    if (!found.empty()) CHECK(found[0] >= 12000 && found[0] <= 12000 + 88);

    // Kicks over noise, with bad samples in the gaps, before a kick, and inside one: the same kicks.
    const int lead = 8820, beats = 8;
    Buf kl, kr;
    kickLoop(kl, kr, lead, beats, -30.0f);
    d.reset();
    const std::vector<int> clean = onsetsAfterStart(d, kl, kr);
    CHECK(static_cast<int>(clean.size()) == beats);
    for (int k = 0; k < beats; ++k) {
        const size_t p = static_cast<size_t>(beatStart(lead, k));
        kl[p - 5000] = bad[k % 3];                     // in the gap
        kr[p - 3000] = bad[(k + 1) % 3];
        for (size_t i = 1; i <= 30; ++i) kl[p - i] = bad[i % 3];   // a burst up to the kick
        if (k % 2) kr[p + 400] = std::nanf("");        // inside the kick
    }
    d.reset();
    const std::vector<int> dirty = onsetsAfterStart(d, kl, kr);
    CHECK(static_cast<int>(dirty.size()) == beats);
    for (int k = 0; k < beats && k < static_cast<int>(dirty.size()); ++k) {
        const int late = dirty[static_cast<size_t>(k)] - beatStart(lead, k);
        CHECK(late >= 0 && late <= 88);
    }
}

} // namespace

void transientTests() {
    transientKicks();
    transientSteady();
    transientHoldoff();
    transientSensitivity();
    transientNan();
}
