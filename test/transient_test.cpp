// dsp/transient.h: the onset detector behind Pump's Input trigger.
#include "signal.h"
#include "../dsp/transient.h"

namespace {

using namespace eft;
using gf::TransientDetector;

constexpr int kHoldoffSamples = 1764;   // 40 ms at 44.1 kHz
constexpr int kStartup = 2205;          // 50 ms: a signal that starts out of silence is an onset itself
constexpr int kFirstWave = 220;         // 5 ms: a 55 Hz kick's first half wave (crest at 4.5 ms); a kick behind another is found within it

// x += a hit: a decaying sine starting at `at` (a kick at 55 Hz, 60 ms; a short tom or snare-like hit above).
void addHit(Buf& x, int at, double hz, double decayMs, float amp) {
    const double tau = decayMs * 0.001 * ef::kRate;
    for (int i = 0; at + i < static_cast<int>(x.size()) && i < 10 * tau; ++i)
        x[static_cast<size_t>(at + i)] += amp * static_cast<float>(std::sin(2.0 * kPi * hz * i / ef::kRate) * std::exp(-i / tau));
}
void addKick(Buf& x, int at, float amp) { addHit(x, at, 55.0, 60.0, amp); }

// Where each onset is (sample numbers), the signal fed in chunks of `chunk` samples. Every offset it
// returns is in range: one check per call, not per chunk.
std::vector<int> onsets(TransientDetector& d, const Buf& L, const Buf& R, int chunk = ef::kChunk) {
    std::vector<int> found;
    bool inRange = true;
    for (size_t pos = 0; pos < L.size(); pos += static_cast<size_t>(chunk)) {
        const int n = static_cast<int>(std::min(static_cast<size_t>(chunk), L.size() - pos));
        const int at = d.process(&L[pos], &R[pos], n);
        inRange = inRange && at >= -1 && at < n;
        if (at >= 0) found.push_back(static_cast<int>(pos) + at);
    }
    CHECK(inRange);
    return found;
}
// The same without the start: what a signal arriving out of silence makes of the first 50 ms.
std::vector<int> onsetsAfterStart(TransientDetector& d, const Buf& L, const Buf& R, int chunk = ef::kChunk) {
    std::vector<int> found = onsets(d, L, R, chunk);
    found.erase(std::remove_if(found.begin(), found.end(), [](int at) { return at < kStartup; }), found.end());
    return found;
}

// White noise `noiseDb` under a kick of 0.8 (as RMS: a uniform noise's RMS is its amplitude / sqrt(3));
// under -150 none at all.
Buf noiseBed(int n, float noiseDb, uint32_t seed) {
    return whiteNoise(n, noiseDb < -150.0f ? 0.0f : 0.8f * ef::dbToGain(noiseDb) * 1.7320508f, seed);
}

// The start sample of beat k at 128 BPM, `lead` samples in.
int beatStart(int lead, int k) { return lead + static_cast<int>(std::lround(k * 60.0 / 128.0 * ef::kRate)); }

// A kick every beat at 128 BPM over white noise, both sides (noiseBed).
void kickLoop(Buf& L, Buf& R, int lead, int beats, float noiseDb, double hz = 55.0, double decayMs = 60.0) {
    const int n = beatStart(lead, beats);
    L = noiseBed(n, noiseDb, 1);
    R = noiseBed(n, noiseDb, 2);
    for (int k = 0; k < beats; ++k) {
        addHit(L, beatStart(lead, k), hz, decayMs, 0.8f);
        addHit(R, beatStart(lead, k), hz, decayMs, 0.8f);
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
    // A groove at 100 BPM: a loud kick on each beat (the loudest), and a ghost kick 24 dB under it 280 ms
    // after, where the loud one's tail is still in the slow envelope: the ghost rises to about 2.2 times
    // that, past the 1.55 of sensitivity 0.9 and short of the 3.6 of 0.1.
    const int lead = 4410, beats = 8, beat = 26460, ghostAt = 12348;   // 600 ms, 280 ms
    const int n = lead + beats * beat;
    Buf x(static_cast<size_t>(n), 0.0f);
    for (int k = 0; k < beats; ++k) {
        addKick(x, lead + k * beat, 0.8f);
        addKick(x, lead + k * beat + ghostAt, 0.8f * ef::dbToGain(-24.0f));
    }
    TransientDetector d;
    d.setSensitivity(0.9f);
    const std::vector<int> keen = onsets(d, x, x);
    d.reset();
    d.setSensitivity(0.1f);
    const std::vector<int> dull = onsets(d, x, x);
    CHECK(static_cast<int>(keen.size()) == 2 * beats);   // every kick and every ghost
    for (int k = 0; k < 2 * beats && k < static_cast<int>(keen.size()); ++k) {
        const int late = keen[static_cast<size_t>(k)] - (lead + k / 2 * beat + (k % 2 ? ghostAt : 0));
        CHECK(late >= 0 && late <= (k % 2 ? 441 : 88));   // the ghost's within 10 ms: the onset is its own
    }
    CHECK(static_cast<int>(dull.size()) == beats);       // the loud kicks alone
    for (int k = 0; k < beats && k < static_cast<int>(dull.size()); ++k) {
        const int late = dull[static_cast<size_t>(k)] - (lead + k * beat);
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

void transientRoll() {    // a roll of 8 equal kicks, one every 1/16, at 120, 128, 140 and 150 BPM: every kick found (0.5)
    // The slow envelope has to have let go of one kick when the next comes, 100 ms later at 150 BPM
    // (the second kick against the first one's tail is the hardest of the roll). A kick behind another
    // is found 1.6 to 3.3 ms in: its start is hidden by the one before.
    const int kicks = 8, lead = 8820;
    for (double bpm : {120.0, 128.0, 140.0, 150.0}) {
        const int step = static_cast<int>(std::lround(60.0 / bpm / 4.0 * ef::kRate)), n = lead + kicks * step + 13230;
        for (float noiseDb : {-200.0f, -30.0f}) {   // clean, and over noise 30 dB under the kick
            Buf L = noiseBed(n, noiseDb, 1), R = noiseBed(n, noiseDb, 2);
            for (int k = 0; k < kicks; ++k) {
                addKick(L, lead + k * step, 0.8f);
                addKick(R, lead + k * step, 0.8f);
            }
            TransientDetector d;
            const std::vector<int> found = onsetsAfterStart(d, L, R);
            CHECK(static_cast<int>(found.size()) == kicks);
            for (int k = 0; k < kicks && k < static_cast<int>(found.size()); ++k) {
                const int late = found[static_cast<size_t>(k)] - (lead + k * step);
                CHECK(late >= 0 && late <= (k ? kFirstWave : 88));   // 2 ms for the first; the others within their first wave
            }
        }
    }
}

void transientLongKick() {   // a long kick (45 Hz, 250 ms decay) every beat at 128 BPM, at 1.0: one onset per kick
    // At the keenest sensitivity the body of a long kick stands over the slow envelope again where
    // the holdoff ends: it must not count twice.
    for (float noiseDb : {-200.0f, -30.0f}) {
        Buf L, R;
        kickLoop(L, R, 8820, 16, noiseDb, 45.0, 250.0);
        TransientDetector d;
        d.setSensitivity(1.0f);
        const std::vector<int> found = onsetsAfterStart(d, L, R);
        CHECK(found.size() == 16);
        for (int k = 0; k < 16 && k < static_cast<int>(found.size()); ++k) {
            const int late = found[static_cast<size_t>(k)] - beatStart(8820, k);
            CHECK(late >= 0 && late <= 88);
        }
    }
}

void transientBass() {    // a steady 40 Hz sine: no onsets at 0.5 or 1.0; a kick over a sustained 40 Hz bed 12 dB under it: found every time
    const int n = 88200;
    const Buf bass = sine(40.0, n, 0.5f);
    TransientDetector d;
    for (float keen : {0.5f, 1.0f}) {
        d.reset();
        d.setSensitivity(keen);
        CHECK(onsetsAfterStart(d, bass, bass).empty());
    }

    // The limit: a hit has to stand out over what is sustained under it. The bed (a sine, sustained)
    // comes in at the start and is itself an onset there; the kicks follow it.
    const int lead = 8820, beats = 16, len = beatStart(lead, beats);
    Buf x = sine(40.0, len, 0.8f * ef::dbToGain(-12.0f));
    for (int k = 0; k < beats; ++k) addKick(x, beatStart(lead, k), 0.8f);
    TransientDetector standard;   // sensitivity 0.5
    const std::vector<int> found = onsetsAfterStart(standard, x, x);
    CHECK(static_cast<int>(found.size()) == beats);
    for (int k = 0; k < beats && k < static_cast<int>(found.size()); ++k) {
        const int late = found[static_cast<size_t>(k)] - beatStart(lead, k);
        CHECK(late >= 0 && late <= kFirstWave);   // 0.8 to 2.4 ms in: the bed hides the start
    }
}

void transientRearm() {   // a swell at 1.0 stays over the slow envelope: onsets at least 100 ms apart, not every 40
    const int n = 88200;
    Buf swell = sine(220.0, n, 0.5f);   // up from nothing to 0.5 over the 2 s
    for (int i = 0; i < n; ++i) swell[static_cast<size_t>(i)] *= static_cast<float>(i) / static_cast<float>(n);
    TransientDetector d;
    d.setSensitivity(1.0f);
    const std::vector<int> found = onsets(d, swell, swell);
    CHECK(!found.empty());
    for (size_t k = 1; k < found.size(); ++k) CHECK(found[k] - found[k - 1] >= 4410);
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
    transientRoll();
    transientLongKick();
    transientBass();
    transientRearm();
    transientNan();
}
