// dsp/transient.h: the onset detector behind Pump's Input trigger.
#include "signal.h"
#include "../dsp/transient.h"

namespace {

using namespace eft;
using gf::TransientDetector;

constexpr int kHoldoffSamples = 1764;   // 40 ms at 44.1 kHz
constexpr int kStartup = 2205;          // 50 ms: a signal that starts out of silence is an onset itself
constexpr int kFirstWave = 220;         // 5 ms: a 55 Hz kick's first half wave (crest at 4.5 ms); a kick behind another is found within it

// x += a hit: a decaying sine starting at `at` (a kick at 55 Hz, 60 ms; a short snare or rim-like hit above).
void addHit(Buf& x, int at, double hz, double decayMs, float amp) {
    const double tau = decayMs * 0.001 * ef::kRate;
    for (int i = 0; at + i < static_cast<int>(x.size()) && i < 10 * tau; ++i)
        x[static_cast<size_t>(at + i)] += amp * static_cast<float>(std::sin(2.0 * kPi * hz * i / ef::kRate) * std::exp(-i / tau));
}
void addKick(Buf& x, int at, float amp) { addHit(x, at, 55.0, 60.0, amp); }
// A kick whose pitch falls from `hz0` to `hz1` with a time constant of `sweepMs` (the first 15 ms of a
// kick drum's thump), and decays by `decayMs`.
void addSweptKick(Buf& x, int at, double hz0, double hz1, double sweepMs, double decayMs, float amp) {
    const double tau = decayMs * 0.001 * ef::kRate, sweep = sweepMs * 0.001 * ef::kRate;
    double phase = 0.0;
    for (int i = 0; at + i < static_cast<int>(x.size()) && i < 10 * tau; ++i) {
        x[static_cast<size_t>(at + i)] += amp * static_cast<float>(std::sin(phase) * std::exp(-i / tau));
        phase += 2.0 * kPi * (hz1 + (hz0 - hz1) * std::exp(-i / sweep)) / ef::kRate;
    }
}

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

// Gaussian noise of unit RMS (the sum of 12 uniform draws), mono: what a real mix's background is
// closer to than uniform white noise (whose peaks are bounded), and no independent L and R.
Buf gaussianNoise(int n, uint32_t seed) {
    Buf x(static_cast<size_t>(n));
    for (float& s : x) {
        float sum = 0.0f;
        for (int j = 0; j < 12; ++j) {
            seed = seed * 1664525u + 1013904223u;
            sum += static_cast<float>(seed >> 8) / 16777216.0f;
        }
        s = sum - 6.0f;
    }
    return x;
}
// x through a two-pole Butterworth low-pass at `hz` (bilinear transform, in double).
Buf lowpass2(const Buf& x, double hz) {
    const double k = std::tan(kPi * hz / ef::kRate), r2 = std::sqrt(2.0), norm = 1.0 / (1.0 + r2 * k + k * k);
    const double b0 = k * k * norm, b1 = 2.0 * b0, a1 = 2.0 * (k * k - 1.0) * norm, a2 = (1.0 - r2 * k + k * k) * norm;
    Buf y(x.size());
    double z1 = 0.0, z2 = 0.0;
    for (size_t i = 0; i < x.size(); ++i) {
        const double out = b0 * x[i] + z1;
        z1 = b1 * x[i] - a1 * out + z2;
        z2 = b0 * x[i] - a2 * out;
        y[i] = static_cast<float>(out);
    }
    return y;
}
void scaleToRms(Buf& x, double rmsWanted) {
    const double g = rmsWanted / rms(x);
    for (float& s : x) s = static_cast<float>(s * g);
}
enum class Bed { Gaussian, Low3k, Low500, Low120 };
// A mono background of the given colour (Gaussian noise, low-passed at 3 kHz, 500 or 120 Hz), `db` under a kick of 0.8 as RMS.
Buf bedOf(Bed kind, int n, float db, uint32_t seed = 7) {
    Buf x = gaussianNoise(n, seed);
    if (kind == Bed::Low3k) x = lowpass2(x, 3000.0);
    if (kind == Bed::Low500) x = lowpass2(x, 500.0);
    if (kind == Bed::Low120) x = lowpass2(x, 120.0);
    scaleToRms(x, 0.8 * ef::dbToGain(db));
    return x;
}

// The start sample of beat k at 128 BPM, `lead` samples in.
int beatStart(int lead, int k) { return lead + static_cast<int>(std::lround(k * 60.0 / 128.0 * ef::kRate)); }

// A kick every beat at 128 BPM over white noise, both sides (noiseBed). `sweepFrom` > 0: the pitch falls
// from there to `hz`, time constant 15 ms.
void kickLoop(Buf& L, Buf& R, int lead, int beats, float noiseDb, double hz = 55.0, double decayMs = 60.0, double sweepFrom = 0.0) {
    const int n = beatStart(lead, beats);
    L = noiseBed(n, noiseDb, 1);
    R = noiseBed(n, noiseDb, 2);
    for (int k = 0; k < beats; ++k) {
        addSweptKick(L, beatStart(lead, k), sweepFrom > 0.0 ? sweepFrom : hz, hz, 15.0, decayMs, 0.8f);
        addSweptKick(R, beatStart(lead, k), sweepFrom > 0.0 ? sweepFrom : hz, hz, 15.0, decayMs, 0.8f);
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
    // Short hits (180 Hz, 4 ms): the detector is armed again once the slow envelope has fallen by a
    // third, about 43 ms after the hit has gone (release 120 ms); a longer hit (a tom) is not over by
    // 60 ms.
    TransientDetector d;
    const int n = 22050, at = 4410;
    for (int gapMs : {20, 60}) {
        Buf x(static_cast<size_t>(n), 0.0f);
        addHit(x, at, 180.0, 4.0, 0.6f);
        addHit(x, at + gapMs * 44, 180.0, 4.0, 0.6f);
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

    // Two clicks: the second, 1764 samples (40 ms) after the first, is absorbed (the holdoff is over,
    // but the slow envelope has not yet fallen enough to arm the detector, and the click is gone by
    // then); 3000 samples (68 ms) after, it is an onset of its own.
    for (int gap : {kHoldoffSamples, 3000}) {
        Buf x(static_cast<size_t>(n), 0.0f);
        x[static_cast<size_t>(at)] = 0.5f;
        x[static_cast<size_t>(at + gap)] = 0.5f;
        d.reset();
        const std::vector<int> found = onsets(d, x, x);
        CHECK(static_cast<int>(found.size()) == (gap < 3000 ? 1 : 2));
        if (found.size() == 2) CHECK(found[0] == at && found[1] == at + gap);
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

void transientRoll() {    // a roll of 8 equal kicks, one every 1/16, at 120, 128, 140 and 150 BPM: every kick found (0.8)
    // A roll needs a keener sensitivity than the default 0.5: the slow envelope still holds the kick
    // before, 100 to 125 ms back, and the second kick against the first one's tail is the hardest of
    // the roll (at 150 BPM it starts in anti-phase to the tail). All 8 are found from 0.6 at 120 BPM,
    // 0.7 at 128 and 140, 0.8 at 150. A kick behind another is found up to 3 ms in: its start is
    // hidden by the one before.
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
            d.setSensitivity(0.8f);
            const std::vector<int> found = onsetsAfterStart(d, L, R);
            CHECK(static_cast<int>(found.size()) == kicks);
            for (int k = 0; k < kicks && k < static_cast<int>(found.size()); ++k) {
                const int late = found[static_cast<size_t>(k)] - (lead + k * step);
                CHECK(late >= 0 && late <= (k ? kFirstWave : 88));   // 2 ms for the first; the others within their first wave
            }
        }
    }
}

void transientLongKick() {   // a long kick (100 -> 45 Hz, 15 ms sweep, 250 ms decay) every beat at 128 BPM, at 1.0: one onset per kick
    // At the keenest sensitivity the body of a long kick stands over the slow envelope again where
    // the holdoff ends: it must not count twice.
    for (float noiseDb : {-200.0f, -30.0f}) {
        Buf L, R;
        kickLoop(L, R, 8820, 16, noiseDb, 45.0, 250.0, 100.0);
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
        CHECK(late >= 0 && late <= kFirstWave);   // 1.0 to 2.7 ms in: the bed hides the start
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

// How many of the beats' kicks the onsets found (each within its first wave) and how many are something else.
struct Score { int found = 0, extra = 0; };
Score scoreKicks(const std::vector<int>& onsets, int lead, int beats) {
    Score score;
    for (int at : onsets) {
        bool kick = false;
        for (int k = 0; k < beats; ++k) {
            const int late = at - beatStart(lead, k);
            if (late >= 0 && late <= kFirstWave) kick = true;
        }
        ++(kick ? score.found : score.extra);
    }
    return score;
}

void transientNoiseBeds() {   // kicks over a mono bed of Gaussian or white noise, -30 and -45 dB under the kick: every kick found, nothing else
    // A mix is not uniform white noise with independent sides: the same signal on both, Gaussian
    // peaks. Over seeds 1 to 6, at the default sensitivity, none of it counts.
    const int lead = 8820, beats = 16, len = beatStart(lead, beats);
    for (float db : {-30.0f, -45.0f}) {
        for (uint32_t seed = 1; seed <= 6; ++seed) {
            for (int white = 0; white < 2; ++white) {
                Buf x = white ? noiseBed(len, db, seed) : bedOf(Bed::Gaussian, len, db, seed);
                for (int k = 0; k < beats; ++k) addKick(x, beatStart(lead, k), 0.8f);
                TransientDetector d;   // 0.5
                const Score score = scoreKicks(onsetsAfterStart(d, x, x), lead, beats);
                CHECK(score.found == beats && score.extra == 0);
            }
        }
    }

    // Narrowband low noise (a rumble) is not held to that: measured, not asserted. Kicks found / extra
    // onsets / kicks missed, seeds 1 to 6, at 0.5.
    const struct { Bed kind; const char* name; } narrow[] = {{Bed::Low3k, "lp3k"}, {Bed::Low500, "lp500"}, {Bed::Low120, "lp120"}};
    for (const auto& bed : narrow) {
        for (float db : {-30.0f, -45.0f}) {
            std::printf("transient, measured (not asserted): kicks over %s noise %.0f dB, found/extra/missed of %d, seeds 1-6:", bed.name, db, beats);
            for (uint32_t seed = 1; seed <= 6; ++seed) {
                Buf x = bedOf(bed.kind, len, db, seed);
                for (int k = 0; k < beats; ++k) addKick(x, beatStart(lead, k), 0.8f);
                TransientDetector d;
                const Score score = scoreKicks(onsetsAfterStart(d, x, x), lead, beats);
                std::printf(" %d/%d/%d", score.found, score.extra, beats - score.found);
            }
            std::printf("\n");
        }
    }
}

void transientRumble() {  // steady mono Gaussian noise, 10 s: no onsets at 0.5
    const int n = 441000;
    for (uint32_t seed = 1; seed <= 6; ++seed) {
        Buf x = gaussianNoise(n, seed);
        scaleToRms(x, 0.2);
        TransientDetector d;
        CHECK(onsetsAfterStart(d, x, x).empty());
    }

    // A rumble (the same noise through a two-pole low-pass at 300 or 120 Hz) swells and fades enough
    // to fire it now and then: measured, not asserted. Onsets in 10 s after the first 50 ms, seeds 1 to 6.
    for (double hz : {300.0, 120.0}) {
        std::printf("transient, measured (not asserted): steady lp%.0f rumble, 10 s, onsets at 0.5, seeds 1-6:", hz);
        for (uint32_t seed = 1; seed <= 6; ++seed) {
            Buf x = lowpass2(gaussianNoise(n, seed), hz);
            scaleToRms(x, 0.2);
            TransientDetector d;
            std::printf(" %zu", onsetsAfterStart(d, x, x).size());
        }
        std::printf("\n");
    }
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
    transientNoiseBeds();
    transientRumble();
    transientNan();
}
