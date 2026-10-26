// CPU bench for the built plugin: dlopen()s the .so like MPC does, feeds noise at 44.1 kHz in
// 128-frame blocks, in place, and times every processReplacing call with the thread's own CPU clock.
// Same verdict rule as sd88me's tools/bench.sh (percent of the 2902 us block): PASS p99 <= 15% and
// max <= 50%, WARN p99 <= 35% and max <= 80%, else FAIL.
//
//   gfbench <plugin.so> [-s seconds] [-c cpu] [-m]
//
// -m (make bench-count): no timing; marks where each measured processReplacing call starts and ends
// for tools/gfcount.c, a qemu plugin that counts the ARM instructions in between, and hashes every
// case's output (FNV-1a over the samples' bits): a change meant to keep the sound bit for bit keeps
// every hash. GF_CASES=5,6 runs only those cases (numbered from 0, in the order below).
//
// Cases (MPC playing at 128 BPM, parameters set through the plugin's own entry points, so the bench
// needs nothing of the plugin but the .so; tools/cases.h has the settings, shared with the profile-guided
// build's trainer): everything off (Init: the plugin's own work); the Pump at its heaviest (1/16, Lows, Input
// trigger, depth 1, push -10 ms); the Shape at its heaviest (three lanes on 1/16, a band-pass at full
// resonance, Random Smooth on the filter); both together with swing at 75%; four instances of that, one
// after another per block, to see whether copies add up as expected (that line is the sum of the four
// calls of each block, its verdict the per-instance share, a quarter). Then plan 2's: the Echo at its
// heaviest (four lines in a Ring, In loop band-pass at full resonance, drive, wow and an LFO on each), and
// that with the Fade glide and Ping-Pong lines, which is heavier; Time playing a Move's pattern (Roll Build
// 1 Bar, going round); Time in its crossfades (Stutter 1/16, which jumps every 1/16); and everything at its
// heaviest at once (the Echo with the Fade glide). What a case leaves unsaid stays at its
// default; depths are set to 1 and the filters' cutoffs to 1 kHz, so no lane is silent or wide open.
//
// Each case checks itself, so a bench cannot measure something other than what it says: every setting is
// read back (cases.h), and after the warm-up and again at the end the plugin's status line must say that
// as many parts ran as the case has on, and the Move's tile must be lit while its Move plays. Hermetic:
// the plugin reads no user folders, saves nothing, and draws the same random numbers every run. The input
// is noise around -12 dBFS with a kick-like burst (+9 dB for 20 ms) on every beat, for an Input trigger
// to fire on.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../plugin/vst2.h"
#include "cases.h"
#include "param_ids.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <sched.h>
#include <string>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>

namespace {

using namespace gf;

constexpr int kBlock = 128;
constexpr double kRate = 44100.0;
constexpr double kBudgetUs = kBlock * 1e6 / kRate;   // 2902 us
constexpr double kBpm = 128.0;
constexpr int kMaxInstances = 4;
// Before anything is timed the lines fill and the ring records: 4 s is eight beats at 128 BPM, the farthest back a
// pattern reads, and longer than the longest delay the Echo's lines are set to.
constexpr double kWarmSeconds = 4.0;

VstTimeInfo g_time{};
bool g_count = false;   // -m
int g_case = 0;

// -m: the events tools/gfcount.c counts by. 4000 is no system call: without the plugin it does nothing.
enum Mark { kBlockStart, kBlockEnd, kCaseEnd, kProfileOn, kProfileOff };
void mark(Mark op, long arg = 0) {
    if (g_count) syscall(4000, static_cast<long>(op), arg);
}

bool wanted(int c) {
    const char* e = std::getenv("GF_CASES");
    if (!e || !*e) return true;
    const std::string s = std::string(",") + e + ",";
    return s.find("," + std::to_string(c) + ",") != std::string::npos;
}

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

// A case: its settings, how many instances run it one after another, how many parts (of Shape, Pump, Echo and Time)
// it has on, and the Move whose tile must be lit while it plays (null: no Move).
struct Case {
    const char* name;
    void (*setup)(AEffect*);
    int instances;
    int parts;
    const char* move;
};

void noSetup(AEffect*) {}

const Case kCases[] = {
    {"everything off: Init", noSetup, 1, 0, nullptr},
    {"Pump heaviest", gfc::pumpHeaviest, 1, 1, nullptr},
    {"Shape heaviest", gfc::shapeHeaviest, 1, 1, nullptr},
    {"Both heaviest, swing 75%", gfc::bothHeaviest, 1, 2, nullptr},
    {"Four instances, Both heaviest (sum)", gfc::bothHeaviest, 4, 2, nullptr},
    {"Echo heaviest", gfc::echoHeaviest, 1, 1, nullptr},
    {"Echo heaviest, Fade glide, Ping-Pong", gfc::echoFadeHeaviest, 1, 1, nullptr},
    {"Time playing Roll Build 1 Bar", gfc::timeRoll, 1, 1, gfc::kRollMove},
    {"Time in fades: Stutter 1/16", gfc::timeFades, 1, 1, gfc::kFadesMove},
    {"Everything heaviest", gfc::everythingHeaviest, 1, 4, gfc::kRollMove},
};
constexpr int kNumCases = static_cast<int>(sizeof kCases / sizeof kCases[0]);

AEffect* create(void* lib) {
    auto entry = reinterpret_cast<AEffect* (*)(audioMasterCallback)>(dlsym(lib, "VSTPluginMain"));
    AEffect* e = entry ? entry(master) : nullptr;
    if (!e) {
        std::fprintf(stderr, "gfbench: no VSTPluginMain, or it made no plugin\n");
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

// How many parts the status line says ran in its last half second ("CPU 1%   PEAK 3%   2 OF 4 ON").
int partsRunning(AEffect* e) {
    char b[160] = {};
    e->dispatcher(e, vst::effGetParamDisplay, P_STATUS, 0, b, 0.0f);
    int cpu = 0, peak = 0, n = -1, of = 0;
    return std::sscanf(b, "CPU %d%% PEAK %d%% %d OF %d ON", &cpu, &peak, &n, &of) == 4 ? n : -1;
}

// The case does what it says: the parts it has on ran, and its Move plays.
void checkRunning(AEffect* e, const Case& c, const char* when) {
    const int ran = partsRunning(e);
    if (ran != c.parts) {
        std::fprintf(stderr, "gfbench: \"%s\": %s the status line says %d parts ran, the case has %d on\n", c.name, when, ran,
                     c.parts);
        std::exit(1);
    }
    if (c.move && !(e->getParameter(e, gfc::tileOf(c.move)) > 0.5f)) {
        std::fprintf(stderr, "gfbench: \"%s\": %s the Move %s is not playing (its tile is dark)\n", c.name, when, c.move);
        std::exit(1);
    }
}

Result runCase(void* lib, int seconds, const Case& c) {
    const int me = g_case++;
    if (!wanted(me)) return Result{0, 0, 0};
    if (g_count) {
        std::printf("case %d: %s\n", me, c.name);
        std::fflush(stdout);   // before gfcount's line (qemu's stderr)
    }
    AEffect* e[kMaxInstances] = {};
    std::vector<Source> src;
    for (int k = 0; k < c.instances; ++k) {
        e[k] = create(lib);
        c.setup(e[k]);
        src.emplace_back(12345u + 977u * static_cast<uint32_t>(k));
    }
    g_time.ppqPos = 0.0;   // every case from the song's start: one on its own runs as in the whole run
    long long frame = 0;
    uint64_t hash = 1469598103934665603ull;   // FNV-1a over every output sample's bits
    const auto digest = [&] {
        for (const Source& s : src)
            for (int i = 0; i < kBlock; ++i)
                for (const float v : {s.L[static_cast<size_t>(i)], s.R[static_cast<size_t>(i)]}) {
                    uint32_t u;
                    std::memcpy(&u, &v, 4);
                    hash = (hash ^ u) * 1099511628211ull;
                }
    };
    const auto block = [&](bool timed, double* us) {
        for (int k = 0; k < c.instances; ++k) src[static_cast<size_t>(k)].fill(frame);
        const double t0 = timed ? cpuUs() : 0.0;
        if (timed) mark(kBlockStart);
        for (int k = 0; k < c.instances; ++k) {
            Source& s = src[static_cast<size_t>(k)];
            float* io[2] = {s.L.data(), s.R.data()};
            e[k]->processReplacing(e[k], io, io, kBlock);
        }
        if (timed) {
            mark(kBlockEnd);
            *us = cpuUs() - t0;
        }
        digest();
        frame += kBlock;
        g_time.ppqPos += kBlock / kRate * g_time.tempo / 60.0;
    };
    const int warm = static_cast<int>(kWarmSeconds * kRate) / kBlock;
    for (int b = 0; b < warm; ++b) block(false, nullptr);   // the settings land, the parts fade in, the lines fill
    for (int k = 0; k < c.instances; ++k) checkRunning(e[k], c, "after the warm-up,");
    const uint64_t hashWarm = hash;
    const int blocks = seconds * static_cast<int>(kRate) / kBlock;
    std::vector<double> t(static_cast<size_t>(blocks));
    mark(kProfileOn);
    for (int b = 0; b < blocks; ++b) block(true, &t[static_cast<size_t>(b)]);
    mark(kProfileOff);
    for (int k = 0; k < c.instances; ++k) checkRunning(e[k], c, "at the end,");
    for (int k = 0; k < c.instances; ++k) e[k]->dispatcher(e[k], vst::effClose, 0, 0, nullptr, 0.0f);

    if (g_count) {   // the counts are gfcount's; the hashes, of the warm-up and of the whole case
        mark(kCaseEnd, me);
        std::printf("HASH case=%d warm=%016llx all=%016llx\n", me, static_cast<unsigned long long>(hashWarm),
                    static_cast<unsigned long long>(hash));
        std::fflush(stdout);
        return Result{0, 0, 0};
    }
    std::vector<double> s = t;
    std::sort(s.begin(), s.end());
    double sum = 0.0;
    for (double v : t) sum += v;
    const Result r{100.0 * sum / blocks / kBudgetUs, 100.0 * s[static_cast<size_t>(blocks * 0.99)] / kBudgetUs,
                   100.0 * s.back() / kBudgetUs};
    // The verdict is per instance: a quarter of four instances' sum.
    const double share = r.p99 / c.instances, worst = r.max / c.instances;
    const char* verdict = share <= 15.0 && worst <= 50.0 ? "PASS" : share <= 35.0 && worst <= 80.0 ? "WARN" : "FAIL";
    std::printf("  %-38s avg %5.2f%%  p99 %5.2f%%  max %5.2f%%  %s\n", c.name, r.avg, r.p99, r.max, verdict);
    if (c.instances > 1)
        std::printf("  %-38s avg %5.2f%%  p99 %5.2f%%  max %5.2f%%\n", "  per instance", r.avg / c.instances, share, worst);
    return {r.avg / c.instances, share, worst};
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <plugin.so> [-s seconds] [-c cpu] [-m]\n", argv[0]);
        return 2;
    }
    int seconds = 3, cpu = 1;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "-m")) g_count = true;
        else if (!std::strcmp(argv[i], "-s") && i + 1 < argc) seconds = std::max(1, std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "-c") && i + 1 < argc) cpu = std::atoi(argv[++i]);
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
    if (g_count) {   // where the .so and the libraries lie, for a profile's addresses (gfcount's prof=)
        Dl_info di;
        if (dladdr(dlsym(lib, "VSTPluginMain"), &di)) std::printf("BASE %p\n", di.dli_fbase);
        if (FILE* m = std::fopen("/proc/self/maps", "r")) {   // MAP <start>-<end> <perms> <offset> ... <path>
            char line[512];
            while (std::fgets(line, sizeof line, m)) std::printf("MAP %s", line);
            std::fclose(m);
        }
        std::fflush(stdout);
    }
    bool fail = false;
    for (int i = 0; i < kNumCases; ++i) {
        const Result r = runCase(lib, seconds, kCases[i]);
        fail = fail || !(r.p99 <= 35.0 && r.max <= 80.0);
    }
    dlclose(lib);
    return fail ? 1 : 0;
}
