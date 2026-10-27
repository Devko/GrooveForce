#pragma once
// A fake MPC for the insert effect: drives the plugin through its VST2 entry points the way MPC
// does (128-frame blocks, in place, 0..1 params, the Force's taps and Q-Link detents), serves
// transport time, and records what the plugin pushes back (audioMasterAutomate,
// audioMasterUpdateDisplay). MasterForce's, for GrooveForce's parameters.
#include "signal.h"
#include "../plugin/vst2.h"
#include "../plugin/groove_map.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

extern "C" AEffect* VSTPluginMain(audioMasterCallback master);

namespace eft {

constexpr int kBlock = 128;
constexpr int kBlocksPerSec = 44100 / kBlock;

struct HostLog {
    int updates = 0;
    std::map<int, float> automated;   // index -> last value pushed
    std::map<int, int> automateCount;
    VstTimeInfo time{};
    bool noTime = false;              // getTime returns null
};

intptr_t hostMaster(AEffect* e, int32_t op, int32_t index, intptr_t, void*, float opt);   // plugin_test.cpp

struct Host {
    AEffect* e;
    HostLog log;
    Buf L, R;              // the last run's output
    bool inPlace = true;   // MPC's way; false: separate input and output buffers
    bool finite = true;    // every output sample so far

    Host() : e(VSTPluginMain(hostMaster)) {
        e->user = &log;
        log.time.sampleRate = 44100.0;
        log.time.tempo = 120.0;
        log.time.timeSigNumerator = log.time.timeSigDenominator = 4;
        log.time.flags = vst::kVstTempoValid | vst::kVstPpqPosValid;
        op(vst::effOpen);
        op(vst::effMainsChanged, 0, 1);
    }
    ~Host() { op(vst::effClose); }
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    intptr_t op(int32_t code, int32_t idx = 0, intptr_t val = 0, void* ptr = nullptr, float opt = 0.0f) {
        return e->dispatcher(e, code, idx, val, ptr, opt);
    }

    void set(int id, float value) { e->setParameter(e, id, gf::paramNorm(id, value)); }
    void setN(int id, float n) { e->setParameter(e, id, n); }
    float get(int id) { return e->getParameter(e, id); }
    float value(int id) { return gf::paramValue(id, get(id)); }
    void option(int id, const char* name) {
        for (int o = 0; o < gf::PARAM_INFO[id].nopts; ++o)
            if (!std::strcmp(gf::PARAM_INFO[id].opts[o], name)) set(id, static_cast<float>(o));
    }
    void on(int onParam) { option(onParam, "On"); }

    // A tap on a button, as a Force sends it: MPC toggles the value it last read back. A button
    // reads back 0 (it springs back), so a tap is a single 1, never followed by a release.
    void press(int id) { e->setParameter(e, id, get(id) > 0.5f ? 0.0f : 1.0f); }
    // A tap on a tile or toggle: MPC toggles what it read back.
    void tap(int id) { press(id); }
    // One Q-Link detent (dir +1 / -1), as a Force sends it: the value MPC last read back plus
    // 1/128 of the range, rounded to 1/1000 (MPC OS 3.9.1, measured by sd88me/mpc-vst-plugins,
    // docs/NOTES.md "Input probe"). Several in a Turn are one gesture.
    void detent(int id, int dir) {
        e->setParameter(e, id, std::round((get(id) + static_cast<float>(dir) / 128.0f) * 1000.0f) / 1000.0f);
    }

    void play(bool on) {
        if (on) log.time.flags |= vst::kVstTransportPlaying;
        else log.time.flags &= ~vst::kVstTransportPlaying;
    }

    // Feeds l / r through the plugin in blocks of `block` frames; the output lands in L / R. The
    // transport advances while playing.
    void run(const Buf& l, const Buf& r, int block = kBlock) {
        L.assign(l.size(), 0.0f);
        R.assign(l.size(), 0.0f);
        Buf bl(static_cast<size_t>(block)), br(bl.size());
        for (size_t pos = 0; pos < l.size(); pos += static_cast<size_t>(block)) {
            const int n = static_cast<int>(std::min(static_cast<size_t>(block), l.size() - pos));
            std::copy(l.begin() + static_cast<long>(pos), l.begin() + static_cast<long>(pos) + n, bl.begin());
            std::copy(r.begin() + static_cast<long>(pos), r.begin() + static_cast<long>(pos) + n, br.begin());
            if (inPlace) {
                float* io[2] = {bl.data(), br.data()};
                e->processReplacing(e, io, io, n);
                std::copy(bl.begin(), bl.begin() + n, L.begin() + static_cast<long>(pos));
                std::copy(br.begin(), br.begin() + n, R.begin() + static_cast<long>(pos));
            } else {
                float* in[2] = {bl.data(), br.data()};
                float* out[2] = {L.data() + pos, R.data() + pos};
                e->processReplacing(e, in, out, n);
            }
            if (log.time.flags & vst::kVstTransportPlaying) {
                log.time.samplePos += n;
                log.time.ppqPos += n / 44100.0 * log.time.tempo / 60.0;
            }
        }
        for (size_t i = 0; i < L.size(); ++i)
            if (!std::isfinite(L[i]) || !std::isfinite(R[i])) finite = false;
    }
    void run(const Buf& mono, int block = kBlock) { run(mono, mono, block); }
    void silence(int samples) { run(Buf(static_cast<size_t>(samples), 0.0f)); }
    void blocks(int n) { silence(n * kBlock); }

    std::string display(int id) {
        char b[256] = {};
        op(vst::effGetParamDisplay, id, 0, b);
        return b;
    }
    std::string name(int id) {
        char b[256] = {};
        op(vst::effGetParamName, id, 0, b);
        return b;
    }
    std::string chunk() {
        void* data = nullptr;
        const intptr_t size = op(vst::effGetChunk, 0, 0, &data);
        return size > 0 && data ? std::string(static_cast<const char*>(data)) : std::string();
    }
    intptr_t load(const std::string& s) {
        return op(vst::effSetChunk, 0, static_cast<intptr_t>(s.size()), const_cast<char*>(s.data()));
    }
};

std::string fixtureDir();   // per-run temp folder (removed at exit)

// The surface's clock moves this far per host event (plugin_test.cpp): a second, so separate
// events never read as one gesture, whatever the machine's speed.
extern long long g_msPerEvent;
// Within a Turn, events come a few ms apart, as a Q-Link turn's detents or a tile's release echo.
struct Turn {
    explicit Turn(long long ms = 5) : was(g_msPerEvent) { g_msPerEvent = ms; }
    ~Turn() { g_msPerEvent = was; }
    Turn(const Turn&) = delete;
    Turn& operator=(const Turn&) = delete;
    long long was;
};


// --- what the plugin suites share (test/plugin_test.cpp, test/plugin_moves_test.cpp) ------------------------------------

// Their fixtures: the folders for presets and data, and the surface's clock. Made again by each suite, removed by each.
void setupPluginTests();
void cleanupPluginTests();

// GF_TEST_ONLY=name,name runs just the tests of a suite named (exactly), a development help; unset or empty: all.
inline bool testWanted(const char* name) {
    const char* only = std::getenv("GF_TEST_ONLY");
    if (!only || !*only) return true;
    return (std::string(",") + only + ",").find(std::string(",") + name + ",") != std::string::npos;
}
#define GF_RUN(f)                         \
    do {                                  \
        if (eft::testWanted(#f)) f();     \
    } while (0)

inline constexpr float kCeiling = 7.9433f;   // +18 dBFS: the most that ever leaves the plugin

inline bool starts(const std::string& s, const char* with) { return s.compare(0, std::strlen(with), with) == 0; }
inline bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

// A small deterministic generator (0..1), so a failure reproduces.
struct Rng {
    uint32_t seed;
    explicit Rng(uint32_t s) : seed(s) {}
    float next() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / 16777216.0f;
    }
    int below(int n) { return std::min(static_cast<int>(next() * static_cast<float>(n)), n - 1); }
};

// The sound parameters of two instances alike (a state is written with six digits).
inline bool sameSound(Host& a, Host& b) {
    bool same = true;
    for (int i = 0; i < gf::P_COUNT; ++i)
        if (gf::PARAM_INFO[i].kind == gf::Kind::Synth && std::fabs(a.get(i) - b.get(i)) > 2e-5f) {
            std::printf("  differs: %s %.6f / %.6f\n", gf::PARAM_INFO[i].key, static_cast<double>(a.get(i)), static_cast<double>(b.get(i)));
            same = false;
        }
    return same;
}

// The user presets of earlier tests gone, so "User 001" is the first save of this one.
inline void freshUserPresets() {
    std::error_code ec;
    std::filesystem::remove_all(fixtureDir() + "/presets/User", ec);
}

inline constexpr int kBeat = 22050;   // a quarter note at 120 BPM, in samples
inline constexpr float kDuck = 0.005f;   // a duck is the output under this, a hundredth of the 0.5 the tests feed
// The steepest step of that 0.5 when its gain moves a full scale in a millisecond (and 10% more).
inline constexpr float kSlope = 0.5f / 44.1f * 1.1f;

// Where the output goes under `below`: the first sample of each duck.
inline std::vector<int> ducks(const Buf& x, float below) {
    std::vector<int> at;
    for (size_t i = 0; i < x.size(); ++i)
        if (x[i] < below && (i == 0 || x[i - 1] >= below)) at.push_back(static_cast<int>(i));
    return at;
}

// The Pump as the tests use it: Hard, 1/4, the depth given, MPC playing at 120 BPM.
inline void pumpOn(Host& h, float depth) {
    h.on(gf::P_PUMP_ON);
    h.option(gf::P_PUMP_CURVE, "Hard");
    h.option(gf::P_PUMP_RATE, "1/4");
    h.set(gf::P_PUMP_DEPTH, depth);
    h.play(true);
}

// "1 OF 4 ON": the parts the status line says are running (-1: it doesn't say).
inline int partsOn(Host& h) {
    const std::string s = h.display(gf::P_STATUS);
    const size_t at = s.find(" OF 4 ON");
    return at != std::string::npos && at > 0 && s[at - 1] >= '0' && s[at - 1] <= '9' ? s[at - 1] - '0' : -1;
}

} // namespace eft
