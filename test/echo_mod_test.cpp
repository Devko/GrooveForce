// dsp/echo_mod.h: a line's modulator in the Echo. The LFO is the library's shape on MPC's grid (swing,
// loops, locates, a stopped transport), the Env fires on the input's onsets and falls 60 dB over its
// rate, the Follow reads 1 at a -6 dBFS sine and keeps its attack and release; Off is 0, and nothing a
// host or a surface can send gets a value out of 0..1.
#include "signal.h"
#include "../dsp/echo_mod.h"
#include "../dsp/shapes.h"
#include "../dsp/transient.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace eft;
using gf::EchoMod;
using gf::Swing;

constexpr int kChunk = ef::kChunk;
constexpr double kRate = ef::kRate;

int shapeIndex(const char* name) {
    for (int i = 0; i < gf::numShapes(); ++i)
        if (std::string(gf::shapeName(i)) == name) return i;
    std::printf("  no shape named %s\n", name);
    CHECK(false);
    return 0;
}

ef::Transport transport(double bpm, double beats, bool playing = true) {
    ef::Transport t;
    t.bpm = bpm;
    t.beats = beats;
    t.playing = playing;
    t.valid = true;
    return t;
}
// The song position a chunk on is at `bpm`.
double beatsPerChunk(double bpm) { return kChunk * bpm / 60.0 / kRate; }

EchoMod::Params lfoParams(int shape, double rateBeats, Swing swing = {}) {
    EchoMod::Params p;
    p.mode = EchoMod::kLfo;
    p.shape = shape;
    p.rateBeats = rateBeats;
    p.swing = swing;
    return p;
}
EchoMod::Params envParams(double decayBeats, float sensitivity = 0.5f) {
    EchoMod::Params p;
    p.mode = EchoMod::kEnv;
    p.rateBeats = decayBeats;
    p.sensitivity = sensitivity;
    return p;
}
EchoMod::Params followParams() {
    EchoMod::Params p;
    p.mode = EchoMod::kFollow;
    return p;
}

// A decaying sine added to x from `at`: a kick at 55 Hz with a 60 ms decay.
void addKick(Buf& x, int at, float amp) {
    const double tau = 0.060 * kRate;
    for (int i = 0; at + i < static_cast<int>(x.size()) && i < 10 * tau; ++i)
        x[static_cast<size_t>(at + i)] += amp * static_cast<float>(std::sin(2.0 * kPi * 55.0 * i / kRate) * std::exp(-i / tau));
}

// A value per chunk of m run over the key (L, R) in chunks of `chunk` samples, the transport playing along
// at `bpm` from beat 0 or stopped.
std::vector<float> values(EchoMod& m, const EchoMod::Params& p, const Buf& L, const Buf& R, double bpm = 120.0, bool playing = true,
                          int chunk = kChunk) {
    std::vector<float> v;
    ef::Transport t = transport(bpm, 0.0, playing);
    for (size_t pos = 0; pos < L.size(); pos += static_cast<size_t>(chunk)) {
        const int n = static_cast<int>(std::min(static_cast<size_t>(chunk), L.size() - pos));
        v.push_back(m.next(p, t, &L[pos], &R[pos], n));
        if (playing) t.beats += n * bpm / 60.0 / kRate;
    }
    return v;
}

// --- LFO --------------------------------------------------------------------------------------

// A Saw Down at 1/4 is the library's shape on the song position, chunk by chunk, and that is a ramp from 1
// to 0 across each beat.
void modLfoOnGrid() {
    const int saw = shapeIndex("Saw Down");
    const EchoMod::Params p = lfoParams(saw, 1.0);
    EchoMod m;
    ef::Transport t = transport(120.0, 0.3);
    double worst = 0.0, worstRamp = 0.0;
    int ramp = 0;
    for (int c = 0; c < 3 * 44100 / kChunk; ++c) {
        const float v = m.next(p, t, nullptr, nullptr, kChunk);
        worst = std::max(worst, std::fabs(v - static_cast<double>(gf::shapeAt(saw, t.beats))));
        const double frac = t.beats - std::floor(t.beats);
        if (frac > 0.01 && frac < 0.99) {   // away from the edge at the wrap
            worstRamp = std::max(worstRamp, std::fabs(v - (1.0 - frac)));
            ++ramp;
        }
        t.beats += beatsPerChunk(120.0);
    }
    CHECK(worst <= 1e-4);
    CHECK(worstRamp <= 2e-4 && ramp > 4000);
    if (worst > 1e-4 || worstRamp > 2e-4) std::printf("  Saw Down at 1/4: off the shape by %g, off the ramp by %g\n", worst, worstRamp);
}

// Every shape, at several rates and swings, is the library's value at toStraight(song position) / rate:
// the grid, swung as the other parts swing it. In 0..1 and finite.
void modLfoShapesSwingRates() {
    const Swing swings[] = {Swing{}, Swing{0.58f, 0.25}, Swing{0.66f, 0.5}, Swing{0.75f, 0.25}};
    const double rates[] = {0.25, 1.0, 4.0, 16.0};
    double worst = 0.0;
    bool inRange = true;
    for (int s = 0; s < gf::numShapes(); ++s)
        for (const Swing& sw : swings)
            for (double rate : rates) {
                const EchoMod::Params p = lfoParams(s, rate, sw);
                EchoMod m;
                ef::Transport t = transport(133.0, 1.7);
                for (int c = 0; c < 1500; ++c) {
                    const float v = m.next(p, t, nullptr, nullptr, kChunk);
                    worst = std::max(worst, std::fabs(v - static_cast<double>(gf::shapeAt(s, gf::toStraight(t.beats, sw) / rate))));
                    inRange = inRange && v >= 0.0f && v <= 1.0f;
                    t.beats += beatsPerChunk(133.0);
                }
            }
    CHECK(worst <= 1e-4);
    CHECK(inRange);
    if (worst > 1e-4) std::printf("  a shape is off the grid by %g\n", worst);
}

// The swung grid: a Saw Down on 1/16 with 66% swing wraps where the swung 16th starts (beat 0.33), not at
// the straight 0.25; before the wrap it has fallen nearly to 0, after it is back near 1.
void modLfoSwings() {
    const int saw = shapeIndex("Saw Down");
    EchoMod m;
    const Swing swung{0.66f, 0.25};
    float v = m.next(lfoParams(saw, 0.25, swung), transport(120.0, 0.30), nullptr, nullptr, kChunk);
    CHECK(v > 0.05f && v < 0.15f);   // 0.30 is at the end of the swung cycle (0.91 in); straight it would be 20% in: 0.8
    v = m.next(lfoParams(saw, 0.25, swung), transport(120.0, 0.34), nullptr, nullptr, kChunk);
    CHECK(v > 0.90f && v < 0.98f);
    v = m.next(lfoParams(saw, 0.25), transport(120.0, 0.30), nullptr, nullptr, kChunk);   // no swing: the straight grid
    CHECK(std::fabs(v - 0.8f) < 2e-4f);
}

// Through a loop (the same bars again), a locate forwards and back and a tempo change: the value is the
// shape's at the song position the host says, from the next chunk on. The random shapes play the same
// bar on every pass.
void modLfoFollowsJumps() {
    const int rnd = shapeIndex("Random Steps"), smooth = shapeIndex("Random Smooth"), trance = shapeIndex("Trance 1");
    for (int shape : {rnd, smooth, trance}) {
        const EchoMod::Params p = lfoParams(shape, 1.0);
        EchoMod m;
        double worst = 0.0;
        std::vector<float> pass[3];
        // three passes through a two-bar loop at 120 BPM
        for (int loop = 0; loop < 3; ++loop)
            for (double b = 0.0; b < 8.0; b += beatsPerChunk(120.0)) {
                const float v = m.next(p, transport(120.0, b), nullptr, nullptr, kChunk);
                worst = std::max(worst, std::fabs(v - static_cast<double>(gf::shapeAt(shape, b))));
                pass[loop].push_back(v);
            }
        CHECK(pass[0] == pass[1] && pass[1] == pass[2]);
        // locates and a tempo change
        const double jumps[] = {0.3, 7.1, 2.6, 40.0, 0.0};
        const double bpms[] = {120.0, 120.0, 120.0, 87.0, 174.0};
        for (int j = 0; j < 5; ++j) {
            double b = jumps[j];
            for (int c = 0; c < 200; ++c) {
                const float v = m.next(p, transport(bpms[j], b), nullptr, nullptr, kChunk);
                worst = std::max(worst, std::fabs(v - static_cast<double>(gf::shapeAt(shape, b))));
                b += beatsPerChunk(bpms[j]);
            }
        }
        CHECK(worst <= 1e-4);
        if (worst > 1e-4) std::printf("  %s: off the song position by %g\n", gf::shapeName(shape), worst);
    }
}

// Stopped, the cycle runs on at the tempo from where it was (0 after a reset: the position MPC reports
// while stopped doesn't matter); starting to play moves it to the song position.
void modStopped() {
    const int saw = shapeIndex("Saw Down");
    const EchoMod::Params p = lfoParams(saw, 1.0);

    // Stopped from the start: 0, and on at the tempo. 120 BPM is 2 beats a second.
    for (double bpm : {120.0, 90.0}) {
        EchoMod m;
        double worst = 0.0;
        for (int c = 0; c < 4 * 44100 / kChunk; ++c) {
            const float v = m.next(p, transport(bpm, 77.7, false), nullptr, nullptr, kChunk);
            worst = std::max(worst, std::fabs(v - static_cast<double>(gf::shapeAt(saw, c * beatsPerChunk(bpm)))));
        }
        CHECK(worst <= 1e-4);
        if (worst > 1e-4) std::printf("  stopped at %g BPM: off the tempo by %g\n", bpm, worst);
    }
    // A slower tempo is slower: after 1 s at 120 BPM the cycle is at 2, at 90 at 1.5.
    EchoMod a, b;
    float va = 0.0f, vb = 0.0f;
    for (int c = 0; c < 44100 / kChunk; ++c) {
        va = a.next(p, transport(120.0, 0.0, false), nullptr, nullptr, kChunk);
        vb = b.next(p, transport(90.0, 0.0, false), nullptr, nullptr, kChunk);
    }
    CHECK(std::fabs(va - static_cast<float>(gf::shapeAt(saw, 1377 * beatsPerChunk(120.0)))) < 1e-4f);
    CHECK(std::fabs(vb - static_cast<float>(gf::shapeAt(saw, 1377 * beatsPerChunk(90.0)))) < 1e-4f);
    CHECK(std::fabs(va - vb) > 0.1f);

    // Playing for 1.2 s, then stopped (MPC freezes the position): it goes on where it was. Then played from
    // elsewhere, or with no valid position: the song position, or on at the tempo.
    EchoMod m;
    double worst = 0.0;
    double expect = 0.0;
    const double per = beatsPerChunk(120.0);
    for (int c = 0; c < 6 * 44100 / kChunk; ++c) {
        ef::Transport t;
        if (c < 1000) {
            t = transport(120.0, 0.3 + c * per);
            expect = t.beats;
        } else if (c < 2000) {
            t = transport(120.0, 0.3 + 1000 * per, false);   // the position MPC freezes at
            expect = 0.3 + c * per;
        } else if (c < 2500) {
            t = transport(120.0, 5.3 + (c - 2000) * per);
            expect = t.beats;
        } else {
            t = transport(120.0, 99.0);
            t.valid = false;   // playing, but nobody says where: on at the tempo
            expect = 5.3 + 500 * per + (c - 2500) * per;
        }
        worst = std::max(worst, std::fabs(m.next(p, t, nullptr, nullptr, kChunk) - static_cast<double>(gf::shapeAt(saw, expect))));
    }
    CHECK(worst <= 1e-4);
    if (worst > 1e-4) std::printf("  stopped and started: off the cycle by %g\n", worst);

    // A position that is not a number is not a position either.
    EchoMod n;
    ef::Transport t = transport(120.0, NAN);
    const float v0 = n.next(p, t, nullptr, nullptr, kChunk), v1 = n.next(p, t, nullptr, nullptr, kChunk);
    CHECK(std::fabs(v0 - static_cast<float>(gf::shapeAt(saw, 0.0))) <= 1e-4f);
    CHECK(std::fabs(v1 - static_cast<float>(gf::shapeAt(saw, per))) <= 1e-4f);
}

// --- Off --------------------------------------------------------------------------------------

void modOff() {
    EchoMod m;
    const Buf loud = whiteNoise(kChunk * 64, 0.9f, 3);
    bool zero = true;
    for (bool playing : {true, false})
        for (int c = 0; c < 60; ++c) {
            ef::Transport t = transport(120.0, 1.0 + c * 0.1, playing);
            const EchoMod::Params p;   // Off is the default
            zero = zero && m.next(p, t, &loud[static_cast<size_t>(c) * kChunk], &loud[static_cast<size_t>(c) * kChunk], kChunk) == 0.0f;
        }
    CHECK(zero);
    // Off through a mode that was running: back to exactly 0, and the other modes come back from nothing.
    EchoMod::Params p = followParams();
    const Buf sig = sine(200.0, kChunk * 400, 0.9f);
    float v = 0.0f;
    for (int c = 0; c < 400; ++c) v = m.next(p, transport(120.0, 0.0), &sig[static_cast<size_t>(c) * kChunk], &sig[static_cast<size_t>(c) * kChunk], kChunk);
    CHECK(v > 0.9f);
    p.mode = EchoMod::kOff;
    CHECK(m.next(p, transport(120.0, 0.0), sig.data(), sig.data(), kChunk) == 0.0f);
    p.mode = EchoMod::kFollow;
    CHECK(m.next(p, transport(120.0, 0.0), &sig[0], &sig[0], kChunk) < 0.3f);   // the level starts again from 0 (it was 1)
}

// --- Env --------------------------------------------------------------------------------------

constexpr int kBeat = 22050;   // a beat at 120 BPM

// 16 kicks, one a beat at 120 BPM from `lead`, over noise 30 dB under them, in both channels.
void kickLoop(Buf& L, Buf& R, int lead, int beats, float kickAmp = 0.8f) {
    const int n = lead + beats * kBeat + kBeat / 2;
    L = whiteNoise(n, kickAmp * 0.0316f * 1.7320508f, 1);
    R = whiteNoise(n, kickAmp * 0.0316f * 1.7320508f, 2);
    for (int k = 0; k < beats; ++k) {
        addKick(L, lead + k * kBeat, kickAmp);
        addKick(R, lead + k * kBeat, kickAmp);
    }
}

// A kick on every beat: the value is 1 within 2 ms of each, and by the next it has fallen the 60 dB
// of a 1/4 decay.
void modEnvFires() {
    const int lead = 8820, beats = 16;
    Buf L, R;
    kickLoop(L, R, lead, beats);
    EchoMod m;
    const std::vector<float> v = values(m, envParams(1.0), L, R);
    int fires = 0;
    std::vector<size_t> fireAt;
    for (size_t c = 0; c < v.size(); ++c)
        if (v[c] == 1.0f) {
            ++fires;
            fireAt.push_back(c);
        }
    CHECK(fires == beats);
    // each kick's own value 1: in the chunk its onset is in, at most 2 ms (88 samples) after it starts
    bool within = fires == beats;
    for (int k = 0; k < beats && within; ++k) {
        const long start = lead + static_cast<long>(k) * kBeat;
        const long chunkStart = static_cast<long>(fireAt[static_cast<size_t>(k)]) * kChunk;
        within = chunkStart > start - kChunk && chunkStart <= start + 88;
    }
    CHECK(within);
    // The chunk before each next fire: -60 dB at the end of the beat, so a little over 0.001 at the start of
    // a chunk a few samples short of it (0.00101 to 0.00102 measured).
    double highest = 0.0, lowest = 1.0;
    for (size_t k = 1; k < fireAt.size(); ++k) {
        const double before = v[fireAt[k] - 1];
        highest = std::max(highest, before);
        lowest = std::min(lowest, before);
    }
    CHECK(highest < 0.00103 && lowest > 0.001);
    if (!(highest < 0.00103)) std::printf("  before the next kick: %g (should be 0.001)\n", highest);
    // and in between it falls steadily
    bool falls = true;
    for (size_t c = (fireAt.empty() ? v.size() : fireAt[0]) + 1; c < v.size(); ++c)
        if (v[c] != 1.0f && !(v[c] < v[c - 1] || v[c - 1] == 1.0f || v[c] == 0.0f)) falls = false;
    CHECK(falls);
    // the same wherever the chunks end
    EchoMod m1;
    const std::vector<float> v1 = values(m1, envParams(1.0), L, R, 120.0, true, 1);
    int fires1 = 0;
    for (float x : v1) fires1 += x == 1.0f;
    CHECK(fires1 == beats);
}

// One hit: the value falls along 10^(-3 t / decay) from the sample the detector finds it at, whatever the
// rate and the tempo; half the decay is -30 dB, the end of it -60 dB.
void modEnvDecay() {
    const int lead = 4410;
    Buf L(static_cast<size_t>(lead + 3 * kBeat), 0.0f), R = L;
    addKick(L, lead, 0.8f);
    addKick(R, lead, 0.8f);
    // the sample the detector finds the hit at (alone, in chunks of 1)
    gf::TransientDetector det;
    int s0 = -1;
    for (size_t i = 0; i < L.size() && s0 < 0; ++i)
        if (det.process(&L[i], &R[i], 1) == 0) s0 = static_cast<int>(i);
    CHECK(s0 >= lead && s0 <= lead + 88);

    struct Case { double bpm, beats; };
    for (const Case c : {Case{120.0, 1.0}, Case{120.0, 0.25}, Case{60.0, 1.0}, Case{140.0, 2.0}}) {
        const double decay = c.beats * 60.0 / c.bpm * kRate;   // samples
        EchoMod m;
        const std::vector<float> v = values(m, envParams(c.beats), L, R, c.bpm);
        double worst = 0.0;
        int checked = 0;
        for (size_t i = 0; i < v.size(); ++i) {
            const double t = static_cast<double>(i) * kChunk - s0;
            if (t < kChunk || t > 2.0 * decay) continue;
            const double want = std::pow(10.0, -3.0 * t / decay);
            if (want < 1e-5) continue;
            worst = std::max(worst, std::fabs(v[i] / want - 1.0));
            ++checked;
        }
        CHECK(checked > 100);
        CHECK(worst < 0.01);
        if (worst >= 0.01) std::printf("  decay %g beats at %g BPM: off the exponential by %g\n", c.beats, c.bpm, worst);
        // 1 right at the hit, nothing before it
        bool before = true;
        for (size_t i = 0; (i + 1) * kChunk <= static_cast<size_t>(lead); ++i) before = before && v[i] == 0.0f;
        CHECK(before);
        CHECK(*std::max_element(v.begin(), v.end()) == 1.0f);
    }
}

// The sensitivity is the detector's: a step up in a steady tone is a hit to a keen detector and not to a dull
// one, and the Env finds the same hits as the detector it holds.
void modEnvSensitivity() {
    const int n = 3 * 44100, at = 2 * 44100;
    Buf L = sine(1000.0, n, 0.1f);
    for (int i = at; i < n; ++i) L[static_cast<size_t>(i)] *= 1.6f;
    const Buf R = L;
    auto fired = [&](float sens) {
        EchoMod m;
        const std::vector<float> v = values(m, envParams(0.25, sens), L, R);
        int count = 0;
        for (size_t c = static_cast<size_t>(at / kChunk) - 2; c < static_cast<size_t>(at / kChunk) + 8; ++c) count += v[c] == 1.0f;
        return count;
    };
    CHECK(fired(0.0f) == 0);
    CHECK(fired(1.0f) == 1);
    // out of range and not a number: the nearest, and the default
    EchoMod a, b, c, d;
    CHECK(values(a, envParams(0.25, -3.0f), L, R) == values(b, envParams(0.25, 0.0f), L, R));
    CHECK(values(c, envParams(0.25, NAN), L, R) == values(d, envParams(0.25, 0.5f), L, R));
}

// A mode picked while the key plays steady: the audio starting is not a hit. Nor is the start after a reset.
void modEnvWarmsUp() {
    const Buf L = sine(200.0, 2 * 44100, 0.6f), R = L;
    EchoMod m;
    EchoMod::Params off;
    values(m, off, L, R);   // off for 2 s with the audio playing
    const std::vector<float> v = values(m, envParams(0.5), L, R);
    CHECK(*std::max_element(v.begin(), v.end()) == 0.0f);
    m.reset();
    const std::vector<float> w = values(m, envParams(0.5), L, R);
    CHECK(*std::max_element(w.begin(), w.end()) == 0.0f);
    // and after a hit, a reset forgets it
    Buf K(static_cast<size_t>(44100), 0.0f), K2 = K;
    addKick(K, 3000, 0.8f);
    addKick(K2, 3000, 0.8f);
    EchoMod h;
    const std::vector<float> hit = values(h, envParams(1.0), K, K2);
    CHECK(*std::max_element(hit.begin(), hit.end()) == 1.0f);
    h.reset();
    const Buf silent(static_cast<size_t>(kChunk), 0.0f);
    CHECK(h.next(envParams(1.0), transport(120.0, 0.0), silent.data(), silent.data(), kChunk) == 0.0f);
}

// --- Follow -----------------------------------------------------------------------------------

// The level of the key, a sine at -6 dBFS reads 1 (±0.05) from 30 Hz up, -12 dBFS reads half, silence
// reads 0 within a second.
void modFollow() {
    const EchoMod::Params p = followParams();
    for (double hz : {30.0, 100.0, 1000.0, 5000.0, 15000.0}) {
        const Buf s = sine(hz, 44100, 0.5f);
        EchoMod m;
        const std::vector<float> v = values(m, p, s, s);
        double lowest = 1.0, highest = 0.0;
        for (size_t c = v.size() * 3 / 4; c < v.size(); ++c) {
            lowest = std::min(lowest, static_cast<double>(v[c]));
            highest = std::max(highest, static_cast<double>(v[c]));
        }
        CHECK(lowest >= 0.95 && highest <= 1.0);
        if (lowest < 0.95) std::printf("  -6 dBFS sine at %g Hz reads %.4f..%.4f\n", hz, lowest, highest);
        // -12 dBFS reads half (below the clip the reading is linear)
        const Buf q = sine(hz, 44100, 0.25f);
        EchoMod m2;
        const std::vector<float> w = values(m2, p, q, q);
        const double half = w.back();
        CHECK(half > 0.47 && half < 0.53);
        if (!(half > 0.47 && half < 0.53)) std::printf("  -12 dBFS sine at %g Hz reads %.4f\n", hz, half);
    }
    // louder than -6 dBFS stays at 1
    const Buf loud = sine(1000.0, 44100, 0.95f);
    EchoMod m;
    const std::vector<float> v = values(m, p, loud, loud);
    CHECK(v.back() == 1.0f);

    // silence reads 0 from the start, and within a second of a loud signal
    const Buf zero(44100, 0.0f);
    EchoMod z;
    for (float x : values(z, p, zero, zero)) CHECK(x == 0.0f);
    Buf burst(2 * 44100, 0.0f);
    for (int i = 0; i < 44100 / 2; ++i) burst[static_cast<size_t>(i)] = 0.5f * static_cast<float>(std::sin(2.0 * kPi * 440.0 * i / kRate));
    EchoMod b;
    const std::vector<float> vb = values(b, p, burst, burst);
    CHECK(vb[static_cast<size_t>(0.45 * 44100 / kChunk)] > 0.95f);
    const float later = vb[static_cast<size_t>((0.5 + 1.0) * 44100 / kChunk) - 1];
    CHECK(later < 0.005f);
    if (!(later < 0.005f)) std::printf("  1 s after the burst: %g\n", static_cast<double>(later));
}

// Attack 5 ms and release 150 ms, one-pole: 63% of the way after the attack time, 37% left after the release's.
void modFollowTimes() {
    const EchoMod::Params p = followParams();
    const int n = 44100;
    // a constant key of 0.1 for 0.5 s, then silence: the level is 0.1 times the gain, so far from the clip
    Buf key(static_cast<size_t>(n), 0.0f);
    for (int i = 0; i < n / 2; ++i) key[static_cast<size_t>(i)] = 0.1f;
    // chunks of 1 so a value is the level at a sample
    EchoMod m;
    const std::vector<float> v = values(m, p, key, key, 120.0, true, 1);
    const float settled = v[static_cast<size_t>(n / 2 - 1)];
    CHECK(std::fabs(settled - 0.214f) < 0.004f);   // 0.1 x 2.14
    const float at5ms = v[static_cast<size_t>(0.005 * kRate) - 1];
    CHECK(std::fabs(at5ms / settled - (1.0 - std::exp(-1.0))) < 0.01);
    const float rel = v[static_cast<size_t>(n / 2 + 0.150 * kRate) - 1];
    CHECK(std::fabs(rel / settled - std::exp(-1.0)) < 0.01);
    // a chunk's value is the level at its end: the same whatever the chunk size
    EchoMod a, b;
    const Buf noise = whiteNoise(n, 0.2f, 5);
    const float v32 = values(a, p, noise, noise, 120.0, true, 32).back(), v7 = values(b, p, noise, noise, 120.0, true, 7).back();
    EchoMod c;
    const float v1 = values(c, p, noise, noise, 120.0, true, 1).back();
    CHECK(std::fabs(v32 - v1) < 1e-4f && std::fabs(v7 - v1) < 1e-4f);
}

// The louder side, and nothing but numbers.
void modFollowKey() {
    const EchoMod::Params p = followParams();
    const Buf s = sine(1000.0, 44100, 0.5f), zero(44100, 0.0f);
    EchoMod m;
    CHECK(values(m, p, s, zero).back() > 0.95f);
    EchoMod m2;
    CHECK(values(m2, p, zero, s).back() > 0.95f);
    // a NaN or an infinity is silence, and does not stay in the level
    Buf bad = s;
    for (size_t i = 100; i < bad.size(); i += 1000) bad[i] = (i / 1000) % 2 ? NAN : INFINITY;
    EchoMod m3;
    bool finite = true;
    for (float x : values(m3, p, bad, bad)) finite = finite && std::isfinite(x) && x >= 0.0f && x <= 1.0f;
    CHECK(finite);
    // no key: silence
    EchoMod m4;
    float x = 1.0f;
    for (int i = 0; i < 10; ++i) x = m4.next(p, transport(120.0, 0.0), nullptr, nullptr, kChunk);
    CHECK(x == 0.0f);
    EchoMod m5;
    for (int i = 0; i < 10; ++i) x = m5.next(envParams(1.0), transport(120.0, 0.0), nullptr, nullptr, kChunk);
    CHECK(x == 0.0f);
}

// --- Any input --------------------------------------------------------------------------------

// Whatever a host or a surface sends: a value in 0..1, always a number.
void modAnyInput() {
    const int modes[] = {-7, 0, 1, 2, 3, 4, 99};
    const int shapes[] = {-100, 0, 3, 37, 38, 5000};
    const double rates[] = {NAN, 0.0, -1.0, 1e-9, 0.0625, 1.0, 64.0, 1e9, INFINITY, -INFINITY};
    const float senses[] = {NAN, -1.0f, 0.0f, 0.5f, 1.0f, 2.0f, INFINITY};
    const Swing swings[] = {Swing{}, Swing{NAN, 0.25}, Swing{0.99f, 0.0}, Swing{0.66f, NAN}, Swing{INFINITY, 0.5}, Swing{0.6f, 1e300}};
    const double bpms[] = {NAN, 0.0, -5.0, 19.0, 120.0, 1e9, INFINITY};
    const double positions[] = {NAN, INFINITY, -INFINITY, 1e300, -1e300, 0.0, -3.7, 1e15, 123.456};
    Buf key = whiteNoise(kChunk, 0.7f, 9);
    key[3] = NAN;
    key[5] = INFINITY;
    key[7] = -INFINITY;
    key[9] = 1e30f;
    bool ok = true;
    long count = 0;
    EchoMod m;
    for (int mode : modes)
        for (int shape : shapes)
            for (double rate : rates)
                for (float sens : senses) {
                    EchoMod::Params p;
                    p.mode = mode;
                    p.shape = shape;
                    p.rateBeats = rate;
                    p.sensitivity = sens;
                    p.swing = swings[static_cast<size_t>(count) % (sizeof swings / sizeof swings[0])];
                    const double bpm = bpms[static_cast<size_t>(count) % (sizeof bpms / sizeof bpms[0])];
                    const double pos = positions[static_cast<size_t>(count) % (sizeof positions / sizeof positions[0])];
                    ef::Transport t;
                    t.bpm = bpm;
                    t.beats = pos;
                    t.playing = count % 3 != 0;
                    t.valid = count % 5 != 0;
                    const bool noKey = count % 7 == 0;
                    for (int rep = 0; rep < 3; ++rep) {
                        const float v = m.next(p, t, noKey ? nullptr : key.data(), noKey ? nullptr : key.data(), 1 + (rep * 13 + static_cast<int>(count)) % 40);
                        ok = ok && std::isfinite(v) && v >= 0.0f && v <= 1.0f;
                    }
                    ++count;
                }
    CHECK(ok);
    CHECK(count == 2940);
}

} // namespace

void echoModTests() {
    modLfoOnGrid();
    modLfoShapesSwingRates();
    modLfoSwings();
    modLfoFollowsJumps();
    modStopped();
    modOff();
    modEnvFires();
    modEnvDecay();
    modEnvSensitivity();
    modEnvWarmsUp();
    modFollow();
    modFollowTimes();
    modFollowKey();
    modAnyInput();
}

// make test-module M=echo_mod builds with -DMODULE_TESTS=<M>Tests, i.e. echo_modTests; the suite
// (main.cpp) calls echoModTests.
void echo_modTests() { echoModTests(); }
