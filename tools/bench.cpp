// CPU bench for the built plugin: dlopen()s the .so like MPC does, feeds noise at 44.1 kHz in
// 128-frame blocks, in place, and times every processReplacing call with the thread's own CPU clock.
// Same verdict rule as sd88me's tools/bench.sh (percent of the 2902 us block): PASS p99 <= 15% and
// max <= 50%, WARN p99 <= 35% and max <= 80%, else FAIL.
//
//   gfbench <plugin.so> [-s seconds] [-c cpu]
//
// Cases (MPC playing at 128 BPM, parameters set through the plugin's own entry points, so the bench
// needs nothing of the plugin but the .so): everything off (Init: the plugin's own work); the Pump
// at its heaviest (1/16, Lows, Input trigger, depth 1, push -10 ms); the Shape at its heaviest (three
// lanes on 1/16, a band-pass at full resonance, Random Smooth on the filter); both together with
// swing at 75%; and four instances of that, one after another per block, to see whether copies add up
// as expected: that line is the sum of the four calls of each block, its verdict the per-instance
// share (a quarter). What a case leaves unsaid stays at its default; depths are set to 1 and the
// filter's cutoff to 1 kHz, so no lane is silent or wide open. Hermetic: the plugin reads no user
// folders, saves nothing, and draws the same random numbers every run.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../plugin/vst2.h"
#include "param_ids.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <sched.h>
#include <vector>

namespace {

using namespace gf;

constexpr int kBlock = 128;
constexpr double kRate = 44100.0;
constexpr double kBudgetUs = kBlock * 1e6 / kRate;   // 2902 us
constexpr double kBpm = 128.0;
constexpr int kMaxInstances = 4;

VstTimeInfo g_time{};

intptr_t master(AEffect*, int32_t op, int32_t, intptr_t, void*, float) {
    if (op == vst::audioMasterVersion) return 2400;
    if (op == vst::audioMasterGetTime) return reinterpret_cast<intptr_t>(&g_time);
    return 0;
}

double cpuUs() {
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<double>(ts.tv_sec) * 1e6 + static_cast<double>(ts.tv_nsec) * 1e-3;
}

// A real value (dB, ms, Hz, 0..1, an option's index) as MPC's 0..1: the curves of groove_map.cpp's paramNorm.
float norm(int id, float v) {
    const ParamSpec& s = PARAM_SPECS[id];
    float n = 0.0f;
    switch (s.curve) {
        case Curve::Lin:
        case Curve::Int: n = s.hi > s.lo ? (v - s.lo) / (s.hi - s.lo) : 0.0f; break;
        case Curve::Log: n = v > 0.0f ? std::log(v / s.lo) / std::log(s.hi / s.lo) : 0.0f; break;
        case Curve::Enum: n = s.hi > 0.0f ? v / s.hi : 0.0f; break;
        default: break;
    }
    return std::clamp(n, 0.0f, 1.0f);
}

void num(AEffect* e, int id, float v) { e->setParameter(e, id, norm(id, v)); }

// An option by its name, as surface.py shows it: a renamed option stops the bench instead of being measured as another.
void opt(AEffect* e, int id, const char* name) {
    const ParamInfo& p = PARAM_INFO[id];
    for (int o = 0; o < p.nopts; ++o)
        if (!std::strcmp(p.opts[o], name)) {
            num(e, id, static_cast<float>(o));
            return;
        }
    std::fprintf(stderr, "gfbench: %s has no option \"%s\"\n", p.key, name);
    std::exit(1);
}

enum Case { kOff, kPump, kShape, kBoth, kFour };

void pumpHeaviest(AEffect* e) {
    opt(e, P_PUMP_ON, "On");
    opt(e, P_PUMP_RATE, "1/16");
    opt(e, P_PUMP_BAND, "Lows");
    opt(e, P_PUMP_TRIGGER, "Input");
    num(e, P_PUMP_DEPTH, 1.0f);
    num(e, P_PUMP_PUSH, -10.0f);
}

void shapeHeaviest(AEffect* e) {
    opt(e, P_SHAPE_ON, "On");
    opt(e, P_VOL_ON, "On");
    opt(e, P_FLT_ON, "On");
    opt(e, P_PAN_ON, "On");
    opt(e, P_VOL_RATE, "1/16");
    opt(e, P_FLT_RATE, "1/16");
    opt(e, P_PAN_RATE, "1/16");
    opt(e, P_FLT_TYPE, "BP");
    opt(e, P_FLT_SHAPE, "Random Smooth");
    num(e, P_FLT_RESO, 1.0f);
    num(e, P_FLT_CUTOFF, 1000.0f);
    num(e, P_VOL_DEPTH, 1.0f);
    num(e, P_FLT_DEPTH, 1.0f);
    num(e, P_PAN_DEPTH, 1.0f);
}

void configure(AEffect* e, Case c) {
    if (c == kPump || c == kBoth || c == kFour) pumpHeaviest(e);
    if (c == kShape || c == kBoth || c == kFour) shapeHeaviest(e);
    if (c == kBoth || c == kFour) {
        num(e, P_SWING, 0.75f);
        opt(e, P_SWING_UNIT, "1/16");
    }
}

AEffect* create(void* lib) {
    auto entry = reinterpret_cast<AEffect* (*)(audioMasterCallback)>(dlsym(lib, "VSTPluginMain"));
    AEffect* e = entry ? entry(master) : nullptr;
    if (!e) {
        std::fprintf(stderr, "no plugin\n");
        std::exit(1);
    }
    e->dispatcher(e, vst::effOpen, 0, 0, nullptr, 0.0f);
    return e;
}

struct Result {
    double avg, p99, max;
};

// One instance's input: noise around -12 dBFS, a little different per side, with a kick-like burst
// (+9 dB for 20 ms) on every beat so an Input trigger has something to fire on.
struct Source {
    std::vector<float> L, R;
    uint32_t seed;
    explicit Source(uint32_t s) : L(kBlock), R(kBlock), seed(s) {}
    void fill(long long frame) {
        const double beatFrames = kRate * 60.0 / kBpm;
        for (int i = 0; i < kBlock; ++i) {
            seed = seed * 1664525u + 1013904223u;
            float x = static_cast<float>(static_cast<int32_t>(seed)) * (0.25f / 2147483648.0f);
            if (std::fmod(static_cast<double>(frame + i), beatFrames) < 0.02 * kRate) x *= 2.8f;
            L[static_cast<size_t>(i)] = x;
            R[static_cast<size_t>(i)] = 0.7f * x + 0.05f;
        }
    }
};

Result runCase(void* lib, int seconds, const char* name, Case c, int instances = 1) {
    AEffect* e[kMaxInstances] = {};
    std::vector<Source> src;
    for (int k = 0; k < instances; ++k) {
        e[k] = create(lib);
        configure(e[k], c);
        src.emplace_back(12345u + 977u * static_cast<uint32_t>(k));
    }
    g_time.ppqPos = 0.0;
    long long frame = 0;
    const auto block = [&](bool timed, double* us) {
        for (int k = 0; k < instances; ++k) src[static_cast<size_t>(k)].fill(frame);
        const double t0 = timed ? cpuUs() : 0.0;
        for (int k = 0; k < instances; ++k) {
            Source& s = src[static_cast<size_t>(k)];
            float* io[2] = {s.L.data(), s.R.data()};
            e[k]->processReplacing(e[k], io, io, kBlock);
        }
        if (timed) *us = cpuUs() - t0;
        frame += kBlock;
        g_time.ppqPos += kBlock / kRate * g_time.tempo / 60.0;
    };
    for (int b = 0; b < 256; ++b) block(false, nullptr);   // the settings land, the parts fade in
    const int blocks = seconds * static_cast<int>(kRate) / kBlock;
    std::vector<double> t(static_cast<size_t>(blocks));
    for (int b = 0; b < blocks; ++b) block(true, &t[static_cast<size_t>(b)]);
    for (int k = 0; k < instances; ++k) e[k]->dispatcher(e[k], vst::effClose, 0, 0, nullptr, 0.0f);

    std::vector<double> s = t;
    std::sort(s.begin(), s.end());
    double sum = 0.0;
    for (double v : t) sum += v;
    const Result r{100.0 * sum / blocks / kBudgetUs, 100.0 * s[static_cast<size_t>(blocks * 0.99)] / kBudgetUs,
                   100.0 * s.back() / kBudgetUs};
    // The verdict is per instance: a quarter of four instances' sum.
    const double share = r.p99 / instances, worst = r.max / instances;
    const char* verdict = share <= 15.0 && worst <= 50.0 ? "PASS" : share <= 35.0 && worst <= 80.0 ? "WARN" : "FAIL";
    std::printf("  %-38s avg %5.2f%%  p99 %5.2f%%  max %5.2f%%  %s\n", name, r.avg, r.p99, r.max, verdict);
    if (instances > 1)
        std::printf("  %-38s avg %5.2f%%  p99 %5.2f%%  max %5.2f%%\n", "  per instance", r.avg / instances, share,
                    worst);
    return {r.avg / instances, share, worst};
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <plugin.so> [-s seconds] [-c cpu]\n", argv[0]);
        return 2;
    }
    int seconds = 3, cpu = 1;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "-s")) seconds = std::max(1, std::atoi(argv[i + 1]));
        else if (!std::strcmp(argv[i], "-c")) cpu = std::atoi(argv[i + 1]);
    }
    if (cpu >= 0) {   // -c -1: don't pin (x86 runs, CI)
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu, &set);
        if (sched_setaffinity(0, sizeof set, &set) != 0) std::printf("(could not pin to cpu %d)\n", cpu);
    }
    // Hermetic: no user folders, nothing saved, the same random numbers every run.
    setenv("GF_PRESET_ROOTS", "/nonexistent-gfbench", 1);
    setenv("GF_DATA_DIR", "", 1);
    setenv("GF_FIXED_SEED", "1", 1);
    g_time.sampleRate = kRate;
    g_time.tempo = kBpm;
    g_time.timeSigNumerator = g_time.timeSigDenominator = 4;
    g_time.flags = vst::kVstTempoValid | vst::kVstPpqPosValid | vst::kVstTransportPlaying;

    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        std::fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }
    std::printf("%s, %d s per case\n", argv[1], seconds);
    bool fail = false;
    const auto check = [&fail](const Result& r) { fail = fail || !(r.p99 <= 35.0 && r.max <= 80.0); };
    check(runCase(lib, seconds, "everything off: Init", kOff));
    check(runCase(lib, seconds, "Pump heaviest", kPump));
    check(runCase(lib, seconds, "Shape heaviest", kShape));
    check(runCase(lib, seconds, "Both heaviest, swing 75%", kBoth));
    check(runCase(lib, seconds, "Four instances, Both heaviest (sum)", kFour, 4));
    dlclose(lib);
    return fail ? 1 : 0;
}
