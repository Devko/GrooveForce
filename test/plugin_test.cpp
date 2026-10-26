// The whole plugin driven through its VST2 entry points the way MPC drives an insert effect (128-frame
// blocks in place, 0..1 params, the Force's taps and Q-Link detents, MPC's transport). The suites of the
// modules are test/*_test.cpp; pluginTests() is called by test/main.cpp after them. Built with ASan/UBSan
// by `make test`, for the Force's CPU under qemu by `make test-arm`.
//
// What is tested here is the glue and the chain behind it: the parameters, their texts and their map onto
// the chain (grooveParams), the presets (the factory set part by part, its Moves and its levels: factoryPresets,
// factoryLevels) and the project state, the redraws and the status line, the Pump on
// MPC's grid (pumpThroughMpc), suspend and resume with the Pump running, and that nothing hostile gets out
// of the whole thing at once (stressEverything). The Echo, Time and the Moves through the plugin (mappingEcho, the tiles,
// GO and the Move parameter, the loop and the locate, the memory that arrives while it plays, two instances, a preset
// during a Move, the Echo's tail after a Throw, the project) are the second half. The modules' own behaviour is in their
// suites.
#include "host.h"
#include "../plugin/engine.h"
#include "../plugin/moves.h"
#include "../plugin/presets.h"
#include "../plugin/surface.h"
#include "factory_presets.h"
#include "reference_mix.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>

long long eft::g_msPerEvent = 1000;

namespace eft {

intptr_t hostMaster(AEffect* e, int32_t op, int32_t index, intptr_t, void*, float opt) {
    HostLog* log = e ? static_cast<HostLog*>(e->user) : nullptr;
    if (op == vst::audioMasterVersion) return 2400;
    if (!log) return 0;
    if (op == vst::audioMasterUpdateDisplay) ++log->updates;
    if (op == vst::audioMasterAutomate) {
        log->automated[index] = opt;
        ++log->automateCount[index];
    }
    if (op == vst::audioMasterGetTime) return log->noTime ? 0 : reinterpret_cast<intptr_t>(&log->time);
    return 0;
}

std::string fixtureDir() {
    static const std::string dir = [] {
        char tmpl[] = "/tmp/gftest.XXXXXX";
        const char* d = mkdtemp(tmpl);
        return std::string(d ? d : "/tmp/gftest");
    }();
    return dir;
}

namespace {

using namespace gf;
namespace fs = std::filesystem;

constexpr float kCeiling = 7.9433f;   // +18 dBFS: the most that ever leaves the plugin

bool starts(const std::string& s, const char* with) { return s.compare(0, std::strlen(with), with) == 0; }
bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

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

// Every sound parameter at a value of its own: an option picked by name, a number somewhere in its range.
void spread(Host& h, Rng& rng) {
    for (int i = 0; i < P_COUNT; ++i) {
        if (PARAM_INFO[i].kind != Kind::Synth) continue;
        if (PARAM_INFO[i].nopts > 0) h.set(i, static_cast<float>(rng.below(PARAM_INFO[i].nopts)));
        else h.setN(i, rng.next());
    }
}

// The sound parameters of two instances alike (a state is written with six digits).
bool sameSound(Host& a, Host& b) {
    bool same = true;
    for (int i = 0; i < P_COUNT; ++i)
        if (PARAM_INFO[i].kind == Kind::Synth && std::fabs(a.get(i) - b.get(i)) > 2e-5f) {
            std::printf("  differs: %s %.6f / %.6f\n", PARAM_INFO[i].key, static_cast<double>(a.get(i)), static_cast<double>(b.get(i)));
            same = false;
        }
    return same;
}

bool allDefaults(Host& h) {
    for (int i = 0; i < P_COUNT; ++i)
        if (PARAM_INFO[i].kind == Kind::Synth && std::fabs(h.get(i) - PARAM_INFO[i].def) > 2e-5f) return false;
    return true;
}

// The user presets of earlier tests gone, so "User 001" is the first save of this one.
void freshUserPresets() {
    std::error_code ec;
    fs::remove_all(fixtureDir() + "/presets/User", ec);
}

constexpr int kBeat = 22050;   // a quarter note at 120 BPM, in samples
constexpr float kDuck = 0.005f;   // a duck is the output under this, a hundredth of the 0.5 the tests feed
// The steepest step of that 0.5 when its gain moves a full scale in a millisecond (and 10% more).
constexpr float kSlope = 0.5f / 44.1f * 1.1f;

// A steady input through the plugin, the output appended to `all`: the host's timeline from where it began.
void runDc(Host& h, Buf& all, int blocks, float l = 0.5f, float r = 0.25f) {
    h.run(Buf(static_cast<size_t>(blocks) * kBlock, l), Buf(static_cast<size_t>(blocks) * kBlock, r));
    all.insert(all.end(), h.L.begin(), h.L.end());
}

// Where the output goes under `below`: the first sample of each duck.
std::vector<int> ducks(const Buf& x, float below) {
    std::vector<int> at;
    for (size_t i = 0; i < x.size(); ++i)
        if (x[i] < below && (i == 0 || x[i - 1] >= below)) at.push_back(static_cast<int>(i));
    return at;
}

// The Pump as the tests use it: Hard, 1/4, the depth given, MPC playing at 120 BPM.
void pumpOn(Host& h, float depth) {
    h.on(P_PUMP_ON);
    h.option(P_PUMP_CURVE, "Hard");
    h.option(P_PUMP_RATE, "1/4");
    h.set(P_PUMP_DEPTH, depth);
    h.play(true);
}

// "1 OF 4 ON": the parts the status line says are running (-1: it doesn't say).
int partsOn(Host& h) {
    const std::string s = h.display(P_STATUS);
    const size_t at = s.find(" OF 4 ON");
    return at != std::string::npos && at > 0 && s[at - 1] >= '0' && s[at - 1] <= '9' ? s[at - 1] - '0' : -1;
}

std::string fileText(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// "preset=plugin:User/User 001.gfp" in a project's state: the file it names.
std::string savedFile(const std::string& chunk) {
    const size_t at = chunk.find("preset=plugin:");
    if (at == std::string::npos) return {};
    return fixtureDir() + "/presets/" + chunk.substr(at + 14, chunk.find('\n', at) - at - 14);
}

void basics() {
    std::printf("== the plugin as MPC sees it\n");
    Host h;
    AEffect* e = h.e;
    CHECK(e->magic == vst::kMagic && e->numInputs == 2 && e->numOutputs == 2);
    CHECK((e->flags & vst::effFlagsCanReplacing) && (e->flags & vst::effFlagsProgramChunks) && !(e->flags & vst::effFlagsIsSynth));
    CHECK(e->initialDelay == 0);   // zero latency: MPC can't compensate (docs/PROBE.md)
    CHECK(e->numParams == P_COUNT && e->uniqueID == vst::fourcc("GrFc"));
    CHECK(h.op(vst::effGetPlugCategory) == vst::kPlugCategEffect);
    char b[64] = {};
    h.op(vst::effGetEffectName, 0, 0, b);
    CHECK(std::string(b) == "GrooveForce");
    h.op(vst::effGetVendorString, 0, 0, b);
    CHECK(std::string(b) == "Devko");
    // No MIDI until the probe says MPC sends it to an insert.
    CHECK(h.op(vst::effCanDo, 0, 0, const_cast<char*>("plugAsChannelInsert")) == 1);
    CHECK(h.op(vst::effCanDo, 0, 0, const_cast<char*>("2in2out")) == 1);
    CHECK(h.op(vst::effCanDo, 0, 0, const_cast<char*>("receiveVstTimeInfo")) == 1);
    for (const char* no : {"receiveVstEvents", "receiveVstMidiEvent", "sendVstEvents", "sendVstMidiEvent", "bypass"})
        CHECK(h.op(vst::effCanDo, 0, 0, const_cast<char*>(no)) == -1);
    VstSpeakerArrangementHead st{1, 2}, mono{0, 1};
    CHECK(h.op(vst::effSetSpeakerArrangement, 0, reinterpret_cast<intptr_t>(&st), &st) == 1);
    CHECK(h.op(vst::effSetSpeakerArrangement, 0, reinterpret_cast<intptr_t>(&mono), &st) == 0);
    // Index 0 is the status line, a readout: MPC sets it when it loads a plugin, and it stays put.
    CHECK(P_STATUS == 0 && PARAM_INFO[0].kind == Kind::Readout);
    h.setN(P_STATUS, 0.7f);
    CHECK(h.get(P_STATUS) == 0.0f);
    CHECK(starts(h.display(P_STATUS), "CPU "));
    // Sound parameters are automatable; the surface's own are not.
    CHECK(h.op(vst::effCanBeAutomated, P_PUMP_ON) == 1 && h.op(vst::effCanBeAutomated, P_FLT_CUTOFF) == 1);
    CHECK(h.op(vst::effCanBeAutomated, P_STATUS) == 0 && h.op(vst::effCanBeAutomated, P_PRESET) == 0 &&
          h.op(vst::effCanBeAutomated, P_PRE_SAVE) == 0);
    // A name for every parameter, short enough for MPC's labels.
    for (int i = 0; i < P_COUNT; ++i) CHECK(!h.name(i).empty() && h.name(i).size() <= 24);
    CHECK(h.name(P_SWING) == "Swing" && h.name(P_PUMP_XOVER) == "Crossover");
    CHECK(h.op(vst::effGetTailSize) >= 1);   // 1: no tail
    CHECK(h.op(vst::effGetParamDisplay, P_COUNT + 5, 0, b) == 0 && b[0] == 0);   // out of range: empty, not a crash
}

// The values as the knobs show them, the defaults first, then the formats Task 11 adds: ms, Hz and the signed percent.
void displays() {
    std::printf("== value texts\n");
    Host h;
    CHECK(h.display(P_INPUT) == "+0.0 dB" && h.display(P_OUTPUT) == "+0.0 dB" && h.display(P_MIX) == "100%");
    CHECK(h.display(P_SWING) == "50%" && h.display(P_SWING_UNIT) == "1/16");
    CHECK(h.display(P_PUMP_ON) == "Off" && h.display(P_PUMP_RATE) == "1/4" && h.display(P_PUMP_CURVE) == "Smooth");
    CHECK(h.display(P_PUMP_DEPTH) == "0%" && h.display(P_PUMP_LENGTH) == "60%" && h.display(P_PUMP_PUSH) == "0.0 ms");
    CHECK(h.display(P_PUMP_TRIGGER) == "Grid" && h.display(P_PUMP_SENS) == "50%" && h.display(P_PUMP_BAND) == "Full");
    CHECK(h.display(P_PUMP_XOVER) == "120 Hz" && h.display(P_PUMP_TARGET) == "All" && h.display(P_PUMP_MIX) == "100%");
    CHECK(h.display(P_SHAPE_ON) == "Off" && h.display(P_SHAPE_DEPTH) == "100%" && h.display(P_SHAPE_TARGET) == "All");
    CHECK(h.display(P_VOL_SHAPE) == "Sine" && h.display(P_FLT_SHAPE) == "Sine" && h.display(P_PAN_SHAPE) == "Sine");
    CHECK(h.display(P_VOL_RATE) == "1/4" && h.display(P_FLT_RATE) == "1/4" && h.display(P_PAN_RATE) == "1/4");
    CHECK(h.display(P_VOL_DEPTH) == "0%" && h.display(P_FLT_DEPTH) == "0%" && h.display(P_PAN_DEPTH) == "0%");
    CHECK(h.display(P_FLT_TYPE) == "LP" && h.display(P_FLT_CUTOFF) == "20.0 kHz" && h.display(P_FLT_RESO) == "0%");

    // The signed percent: a sign for anything but zero.
    h.set(P_FLT_DEPTH, 0.4f);
    CHECK(h.display(P_FLT_DEPTH) == "+40%");
    h.set(P_FLT_DEPTH, -1.0f);
    CHECK(h.display(P_FLT_DEPTH) == "-100%");
    h.set(P_PAN_DEPTH, -0.25f);
    CHECK(h.display(P_PAN_DEPTH) == "-25%");
    h.set(P_PAN_DEPTH, 0.001f);
    CHECK(h.display(P_PAN_DEPTH) == "0%");   // never "+0%" or "-0%"
    // Milliseconds, early or late.
    h.set(P_PUMP_PUSH, 12.5f);
    CHECK(h.display(P_PUMP_PUSH) == "+12.5 ms");
    h.set(P_PUMP_PUSH, -4.0f);
    CHECK(h.display(P_PUMP_PUSH) == "-4.0 ms");
    h.set(P_PUMP_PUSH, 30.0f);
    CHECK(h.display(P_PUMP_PUSH) == "+30.0 ms");
    // Hertz, kilohertz where the rounded text would reach one.
    h.set(P_PUMP_XOVER, 300.0f);
    CHECK(h.display(P_PUMP_XOVER) == "300 Hz");
    h.set(P_FLT_CUTOFF, 20.0f);
    CHECK(h.display(P_FLT_CUTOFF) == "20 Hz");
    h.set(P_FLT_CUTOFF, 999.0f);
    CHECK(h.display(P_FLT_CUTOFF) == "999 Hz");
    h.set(P_FLT_CUTOFF, 1500.0f);
    CHECK(h.display(P_FLT_CUTOFF) == "1.50 kHz");
    h.set(P_FLT_CUTOFF, 12000.0f);
    CHECK(h.display(P_FLT_CUTOFF) == "12.0 kHz");
    h.set(P_SWING, 0.58f);
    CHECK(h.display(P_SWING) == "58%");
    h.set(P_INPUT, -7.25f);
    CHECK(h.display(P_INPUT) == "-7.2 dB" || h.display(P_INPUT) == "-7.3 dB");
    // The longest name of the shape lists shows whole, and the library's last one is the list's last option.
    h.option(P_VOL_SHAPE, "Offbeat Accent");
    CHECK(h.display(P_VOL_SHAPE) == "Offbeat Accent");
    h.option(P_PAN_SHAPE, "Random Smooth");
    CHECK(h.display(P_PAN_SHAPE) == "Random Smooth" && h.value(P_PAN_SHAPE) == PARAM_INFO[P_PAN_SHAPE].nopts - 1);

    // 0..1 to the real value and back, for every continuous parameter, at the ends and between.
    bool back = true;
    for (int i = 0; i < P_COUNT; ++i) {
        if (PARAM_INFO[i].kind != Kind::Synth || PARAM_INFO[i].nopts > 0) continue;
        for (float n : {0.0f, 0.1f, 0.5f, 0.9f, 1.0f}) back = back && std::fabs(paramNorm(i, paramValue(i, n)) - n) < 1e-5f;
        back = back && paramValue(i, 0.0f) == PARAM_SPECS[i].lo && std::fabs(paramValue(i, 1.0f) - PARAM_SPECS[i].hi) < 1e-3f;
    }
    CHECK(back);
    CHECK(paramValue(P_COUNT, 0.5f) == 0.0f && paramNorm(-1, 0.5f) == 0.0f && paramDisplay(P_COUNT, 0.5f).empty());
    CHECK(paramValue(P_SWING, std::nanf("")) == 0.5f && paramNorm(P_SWING, std::nanf("")) == 0.0f);   // NaN: the bottom
    CHECK(paramNorm(P_FLT_CUTOFF, 1e38f) == 1.0f);   // a hand-edited huge value is the top
}

// The parameters as the chain gets them (grooveParams): every one of them mapped, the lists to the DSP's own indices and
// beats, and a value within a hair of its rest exactly at rest (Init is the input, bit for bit, and a knob left a hair
// off its detent must not wake a part).
void mapping() {
    std::printf("== grooveParams: every parameter, every rest\n");
    float n[P_COUNT];
    auto defaults = [&n] {
        for (int i = 0; i < P_COUNT; ++i) n[i] = PARAM_INFO[i].def;
    };
    auto at = [&n](int id, float value) { n[id] = paramNorm(id, value); };   // by its real value (an option: its index)
    auto near = [](double a, double b) { return std::fabs(a - b) <= 1e-4 * std::max(1.0, std::fabs(b)); };

    // The defaults are the chain's own, and the rests are exact.
    defaults();
    const Groove::Params d = grooveParams(n);
    const Groove::Params z;
    CHECK(!d.pumpOn && !d.shapeOn && d.pumpTarget == Groove::kAll && d.shapeTarget == Groove::kAll);
    CHECK(d.inputDb == 0.0f && d.outputDb == 0.0f && d.mix == 1.0f);
    CHECK(d.swing.amount == 0.5f && d.swing.unitBeats == 0.25);
    CHECK(d.pump.depth == 0.0f && d.pump.mix == 1.0f && d.pump.pushMs == 0.0f && d.shape.amount == 1.0f && d.shape.mix == 1.0f);
    CHECK(d.pump.rateBeats == z.pump.rateBeats && d.pump.curve == z.pump.curve && d.pump.trigger == z.pump.trigger &&
          d.pump.band == z.pump.band && near(d.pump.length, z.pump.length) && near(d.pump.sensitivity, z.pump.sensitivity) &&
          near(d.pump.crossoverHz, z.pump.crossoverHz));
    bool lanes = d.shape.filterType == z.shape.filterType && d.shape.cutoffHz == 20000.0f && d.shape.resonance == 0.0f;
    for (int l = 0; l < Shape::kLanes; ++l) {
        const Shape::LaneParams &a = d.shape.lane[l], &b = z.shape.lane[l];
        lanes = lanes && a.on == b.on && a.shape == b.shape && a.rateBeats == b.rateBeats && a.depth == 0.0f && a.phase == 0.0f;
    }
    CHECK(lanes);

    // Every parameter, to a value of its own.
    defaults();
    at(P_SWING, 0.62f);
    at(P_SWING_UNIT, 1);
    at(P_PUMP_ON, 1);
    at(P_PUMP_RATE, 4);   // 1 bar
    at(P_PUMP_CURVE, PC_HOLD);
    at(P_PUMP_DEPTH, 0.7f);
    at(P_PUMP_LENGTH, 0.3f);
    at(P_PUMP_PUSH, -4.0f);
    at(P_PUMP_TRIGGER, Pump::kInput);
    at(P_PUMP_SENS, 0.8f);
    at(P_PUMP_BAND, Pump::kHighs);
    at(P_PUMP_XOVER, 200.0f);
    at(P_PUMP_TARGET, Groove::kEcho);
    at(P_PUMP_MIX, 0.4f);
    at(P_SHAPE_ON, 1);
    at(P_SHAPE_DEPTH, 0.3f);
    at(P_SHAPE_TARGET, Groove::kDry);
    at(P_SHAPE_MIX, 0.6f);
    for (int id : {P_VOL_ON, P_FLT_ON, P_PAN_ON}) at(id, 1);
    at(P_VOL_SHAPE, 5);
    at(P_FLT_SHAPE, 9);
    at(P_PAN_SHAPE, static_cast<float>(PARAM_INFO[P_PAN_SHAPE].nopts - 1));
    at(P_VOL_RATE, static_cast<float>(kNumShapeRates - 1));   // the longest
    at(P_FLT_RATE, 4);
    at(P_PAN_RATE, 0);
    at(P_VOL_DEPTH, 0.5f);
    at(P_FLT_DEPTH, -0.5f);
    at(P_PAN_DEPTH, 0.25f);
    at(P_VOL_PHASE, 0.25f);
    at(P_FLT_PHASE, 0.5f);
    at(P_PAN_PHASE, 0.75f);
    at(P_FLT_TYPE, Shape::kHP);
    at(P_FLT_CUTOFF, 1000.0f);
    at(P_FLT_RESO, 0.7f);
    at(P_INPUT, -6.0f);
    at(P_OUTPUT, 3.0f);
    at(P_MIX, 0.5f);
    const Groove::Params p = grooveParams(n);
    CHECK(p.pumpOn && p.shapeOn && p.pumpTarget == Groove::kEcho && p.shapeTarget == Groove::kDry);
    CHECK(near(p.swing.amount, 0.62) && p.swing.unitBeats == 0.5);
    CHECK(p.pump.rateBeats == kPumpRates[4].beats && p.pump.rateBeats == 4.0 && p.pump.curve == PC_HOLD);
    CHECK(near(p.pump.depth, 0.7) && near(p.pump.length, 0.3) && near(p.pump.pushMs, -4.0) && near(p.pump.sensitivity, 0.8));
    CHECK(p.pump.trigger == Pump::kInput && p.pump.band == Pump::kHighs && near(p.pump.crossoverHz, 200.0) && near(p.pump.mix, 0.4));
    CHECK(near(p.shape.amount, 0.3) && near(p.shape.mix, 0.6));
    CHECK(p.shape.lane[Shape::kVolume].on && p.shape.lane[Shape::kFilter].on && p.shape.lane[Shape::kPan].on);
    CHECK(p.shape.lane[Shape::kVolume].shape == 5 && p.shape.lane[Shape::kFilter].shape == 9 &&
          p.shape.lane[Shape::kPan].shape == PARAM_INFO[P_PAN_SHAPE].nopts - 1);
    CHECK(p.shape.lane[Shape::kVolume].rateBeats == kShapeRates[kNumShapeRates - 1].beats &&
          p.shape.lane[Shape::kFilter].rateBeats == kShapeRates[4].beats && p.shape.lane[Shape::kPan].rateBeats == kShapeRates[0].beats);
    CHECK(near(p.shape.lane[Shape::kVolume].depth, 0.5) && near(p.shape.lane[Shape::kFilter].depth, -0.5) &&
          near(p.shape.lane[Shape::kPan].depth, 0.25));
    CHECK(near(p.shape.lane[Shape::kVolume].phase, 0.25) && near(p.shape.lane[Shape::kFilter].phase, 0.5) &&
          near(p.shape.lane[Shape::kPan].phase, 0.75));
    CHECK(p.shape.filterType == Shape::kHP && near(p.shape.cutoffHz, 1000.0) && near(p.shape.resonance, 0.7));
    CHECK(near(p.inputDb, -6.0) && near(p.outputDb, 3.0) && near(p.mix, 0.5));
    // The ends of every list land on the list's own ends: the rates' first and last, the targets, the filter types.
    for (int o = 0; o < kNumShapeRates; ++o) {
        defaults();
        at(P_PAN_RATE, static_cast<float>(o));
        CHECK(grooveParams(n).shape.lane[Shape::kPan].rateBeats == kShapeRates[o].beats);
    }
    for (int o = 0; o < 5; ++o) {
        defaults();
        at(P_PUMP_RATE, static_cast<float>(o));
        CHECK(grooveParams(n).pump.rateBeats == kPumpRates[o].beats);
    }
    for (int o = 0; o < PC_COUNT; ++o) {
        defaults();
        at(P_PUMP_CURVE, static_cast<float>(o));
        CHECK(grooveParams(n).pump.curve == o);
    }

    // A hair off its rest is its rest, exactly; a real step is not.
    defaults();
    n[P_PUMP_DEPTH] = 4e-4f;
    n[P_VOL_DEPTH] = 4e-4f;
    n[P_FLT_DEPTH] = 0.5f + 2e-4f;
    n[P_PAN_DEPTH] = 0.5f - 2e-4f;
    n[P_PUMP_PUSH] = 0.25f + 1e-4f;
    n[P_PUMP_MIX] = 1.0f - 4e-4f;
    n[P_SHAPE_DEPTH] = 1.0f - 4e-4f;
    n[P_SHAPE_MIX] = 1.0f - 4e-4f;
    n[P_FLT_CUTOFF] = 1.0f - 1e-6f;
    n[P_FLT_RESO] = 4e-4f;
    n[P_INPUT] = 0.5f + 1e-5f;
    n[P_OUTPUT] = 0.5f - 1e-5f;
    n[P_MIX] = 1.0f - 4e-4f;
    n[P_SWING] = 1e-3f;
    const Groove::Params s = grooveParams(n);
    CHECK(s.pump.depth == 0.0f && s.shape.lane[Shape::kVolume].depth == 0.0f && s.shape.lane[Shape::kFilter].depth == 0.0f &&
          s.shape.lane[Shape::kPan].depth == 0.0f);
    CHECK(s.pump.pushMs == 0.0f && s.pump.mix == 1.0f && s.shape.amount == 1.0f && s.shape.mix == 1.0f);
    CHECK(s.shape.cutoffHz == 20000.0f && s.shape.resonance == 0.0f);
    CHECK(s.inputDb == 0.0f && s.outputDb == 0.0f && s.mix == 1.0f && s.swing.amount == 0.5f);
    defaults();
    n[P_PUMP_DEPTH] = 0.01f;
    n[P_INPUT] = 0.5f + 0.001f;
    n[P_SWING] = 0.02f;
    n[P_PUMP_PUSH] = 0.25f + 0.005f;
    const Groove::Params t = grooveParams(n);
    CHECK(near(t.pump.depth, 0.01) && t.inputDb > 0.04f && t.swing.amount > 0.504f && t.pump.pushMs > 0.1f);

    // Whatever MPC or a hand-edited file says, the chain gets numbers it can use.
    bool usable = true;
    for (float w : {std::nanf(""), -1.0f, 2.0f, 1e30f, -1e30f, 0.0f, 1.0f}) {
        for (int i = 0; i < P_COUNT; ++i) n[i] = w;
        const Groove::Params q = grooveParams(n);
        usable = usable && std::isfinite(q.inputDb) && std::isfinite(q.outputDb) && std::isfinite(q.mix) && std::isfinite(q.swing.amount);
        usable = usable && std::isfinite(q.pump.depth) && std::isfinite(q.pump.length) && std::isfinite(q.pump.pushMs) &&
                 std::isfinite(q.pump.crossoverHz) && q.pump.curve >= 0 && q.pump.curve < PC_COUNT && q.pump.rateBeats > 0.0;
        usable = usable && q.pumpTarget >= Groove::kAll && q.pumpTarget <= Groove::kEcho && q.shapeTarget >= Groove::kAll &&
                 q.shapeTarget <= Groove::kEcho && std::isfinite(q.shape.cutoffHz) && q.shape.filterType >= Shape::kLP &&
                 q.shape.filterType <= Shape::kHP;
        for (const Shape::LaneParams& lane : q.shape.lane)
            usable = usable && std::isfinite(lane.depth) && std::isfinite(lane.phase) && lane.rateBeats > 0.0 && lane.shape >= 0 &&
                     lane.shape < PARAM_INFO[P_VOL_SHAPE].nopts;
    }
    CHECK(usable);
}

void passThrough() {
    std::printf("== Init: the input, bit for bit\n");
    const Buf l = whiteNoise(kBlocksPerSec * kBlock, 0.5f, 3), r = whiteNoise(kBlocksPerSec * kBlock, 0.5f, 4);
    {
        Host h;
        h.run(l, r);
        CHECK(h.L == l && h.R == r);
    }
    {
        Host h;
        h.inPlace = false;   // a host with separate buffers
        h.run(l, r, 100);
        CHECK(h.L == l && h.R == r);
    }
    for (int block : {1, 37, 600}) {   // MPC always sends 128; any other length is the same
        Host h;
        h.run(Buf(l.begin(), l.begin() + 3000), Buf(r.begin(), r.begin() + 3000), block);
        CHECK(h.L == Buf(l.begin(), l.begin() + 3000) && h.R == Buf(r.begin(), r.begin() + 3000));
    }
    {   // the legacy accumulating process() adds to what is there
        Host h;
        Buf a = l, b = r, oL(l.size(), 1.0f), oR(l.size(), 1.0f);
        float* in[2] = {a.data(), b.data()};
        float* out[2] = {oL.data(), oR.data()};
        h.e->process(h.e, in, out, static_cast<int32_t>(l.size()));
        float worst = 0.0f;
        for (size_t i = 0; i < l.size(); ++i) worst = std::max(worst, std::fabs(oL[i] - 1.0f - l[i]));
        CHECK(worst < 1e-6f);
    }
    {   // a NaN or an infinity from the host never leaves the plugin; the rest passes untouched
        Host h;
        Buf x = l;
        x[1000] = std::nanf("");
        x[2000] = INFINITY;
        x[2001] = -INFINITY;
        h.run(x, r);
        CHECK(h.finite && h.L[1000] == 0.0f && h.L[2000] == 0.0f && h.L[2001] == 0.0f);
        CHECK(h.L[999] == l[999] && h.L[1001] == l[1001] && h.L[3000] == l[3000] && h.R == r);
    }
    {   // no input buffers: silence, not a crash
        Host h;
        float oL[kBlock], oR[kBlock];
        float* out[2] = {oL, oR};
        h.e->processReplacing(h.e, nullptr, out, kBlock);
        CHECK(oL[0] == 0.0f && oR[kBlock - 1] == 0.0f);
    }
    {   // Init loaded again after the knobs were turned: back to the input, bit for bit
        Host h;
        Rng rng(5);
        spread(h, rng);
        h.blocks(8);
        h.press(P_PRE_INIT);
        h.blocks(8);   // past the glides
        h.run(l, r);
        CHECK(h.L == l && h.R == r);
    }
}

// The three levels (the chain's own: Groove glides each at a full scale a millisecond, no faster).
void levels() {
    std::printf("== Input, Output and Mix\n");
    const Buf in(8 * kBlock, 0.1f);
    auto gain = [&in](Host& h) {   // the gain on a steady input once the glide is over
        h.run(in);
        return h.L.back() / 0.1f;
    };
    {
        Host h;
        h.blocks(1);
        h.set(P_INPUT, 6.0206f);
        CHECK(std::fabs(gain(h) - 2.0f) < 0.01f);
        h.set(P_OUTPUT, -6.0206f);
        CHECK(std::fabs(gain(h) - 1.0f) < 0.01f);
        h.set(P_INPUT, 0.0f);
        CHECK(std::fabs(gain(h) - 0.5f) < 0.01f);
    }
    {   // Mix: the processed signal against the dry one
        Host h;
        h.blocks(1);
        h.set(P_INPUT, 12.0f);
        h.set(P_MIX, 0.5f);
        CHECK(std::fabs(gain(h) - (0.5f + 0.5f * 3.981f)) < 0.02f);
        h.set(P_MIX, 0.0f);
        h.run(in);
        h.run(in);
        CHECK(h.L == in);   // all dry: the input, bit for bit, whatever the input level says
    }
    {   // a jump of 24 dB glides at a full scale a millisecond (the chain's rule): 15 ms, and no step bigger than that
        Host h;
        h.blocks(1);
        h.set(P_INPUT, 24.0f);
        h.run(Buf(6 * kBlock, 0.01f));
        CHECK(maxStep(h.L) < 0.01f / 44.1f * 1.05f && h.L.back() > 0.15f);
    }
    {   // the first snapshot is taken as it is: a project with its levels set doesn't glide from 0 dB
        Host h;
        h.set(P_INPUT, 12.0f);
        h.run(in);
        CHECK(std::fabs(h.L.front() / 0.1f - 3.981f) < 0.02f);
    }
}

// The Pump on MPC's grid: depth 1, Hard, 1/4, MPC playing at 120 BPM. The output dips to nothing on every beat,
// wherever in a 128-frame block the beat falls (so each chunk is given the song position at its own first sample),
// and is the input again between. A locate, a stop and a tempo change leave it on the grid.
void pumpThroughMpc() {
    std::printf("== the Pump through MPC: a dip on every beat\n");
    {   // off: the input, bit for bit, whatever depth says
        Host h;
        h.set(P_PUMP_DEPTH, 1.0f);
        h.play(true);
        const Buf in(8 * kBlock, 0.5f);
        h.run(in);
        CHECK(h.L == in);
    }
    Host h;
    pumpOn(h, 1.0f);
    Buf all;
    runDc(h, all, 700);   // four beats and a bit: the beats fall 34, 68 and 102 frames into their blocks
    const Buf right = h.R;
    const std::vector<int> at = ducks(all, kDuck);
    CHECK(at.size() == 5);
    bool onBeat = at.size() == 5, deep = true, whole = true;
    for (size_t k = 0; k < at.size() && k < 5; ++k) {
        const int beat = static_cast<int>(k) * kBeat;
        onBeat = onBeat && std::abs(at[k] - beat) <= 3;
        deep = deep && *std::min_element(all.begin() + std::max(beat - 3, 0), all.begin() + beat + 4) < 0.001f;
        if (k + 1 < at.size())   // recovered (0.6 of a beat) and not yet dropping for the next one
            for (int i = at[k] + 14000; i < at[k + 1] - 100; ++i) whole = whole && all[static_cast<size_t>(i)] > 0.499f;
    }
    CHECK(onBeat);   // the bottom of each duck is on the beat, to three samples
    CHECK(deep);
    CHECK(whole);
    float skew = 0.0f;   // both channels ducked alike: 0.5 and 0.25 stay two to one
    for (size_t i = 0; i < all.size(); ++i) skew = std::max(skew, std::fabs(2.0f * right[i] - all[i]));
    CHECK(skew < 1e-6f);
    Buf both = all;   // the join to the next run is part of it

    // A locate: the next beat is the one MPC says, a long way from where the audio was.
    h.log.time.ppqPos = 10.5;
    Buf after;
    runDc(h, after, 400);   // 2.3 beats
    std::vector<int> d = ducks(after, kDuck);
    CHECK(d.size() == 2 && std::abs(d[0] - kBeat / 2) <= 3 && std::abs(d[1] - kBeat / 2 - kBeat) <= 3);
    both.insert(both.end(), after.begin(), after.end());
    // Stopped, the grid runs on at the tempo from where it was.
    h.play(false);
    Buf idle;
    runDc(h, idle, 300);
    const double where = 10.5 + 400.0 * kBlock / kBeat;
    const int first = static_cast<int>(std::lround((std::ceil(where) - where) * kBeat));
    d = ducks(idle, kDuck);
    CHECK(d.size() == 2 && std::abs(d[0] - first) <= 3 && std::abs(d[1] - first - kBeat) <= 3);
    both.insert(both.end(), idle.begin(), idle.end());
    // A new tempo and a new place: 150 BPM is a beat every 17640 samples. (Landing on a beat the level comes down from 1 at a
    // millisecond's pace and meets the recovery on its way up, a hair above nothing: no duck at 0.)
    h.play(true);
    h.log.time.tempo = 150.0;
    h.log.time.ppqPos = 20.0;
    Buf fast;
    runDc(h, fast, 300);
    d = ducks(fast, kDuck);
    CHECK(d.size() == 2 && std::abs(d[0] - 17640) <= 3 && std::abs(d[1] - 2 * 17640) <= 3);
    both.insert(both.end(), fast.begin(), fast.end());
    CHECK(maxStep(both) < kSlope);   // no click anywhere, locates and all: nothing faster than a full scale a millisecond
    CHECK(h.finite);
}

// The status line: what the plugin costs and how many of its four parts (Shape, Pump, Echo, Time) run, told to MPC (an
// UpdateDisplay) when it changes. The first report is half a second of audio away, and so is each next one.
void statusLine() {
    std::printf("== the status line: CPU, PEAK and the parts that run\n");
    Host h;
    h.blocks(180);
    const std::string s = h.display(P_STATUS);
    CHECK(starts(s, "CPU ") && has(s, "%   PEAK ") && has(s, "%   0 OF 4 ON"));
    int told = h.log.updates;
    auto settle = [&] {   // what the line says once the next report is out, and whether MPC was asked to read it again
        h.blocks(180);
        const bool asked = h.log.updates > told;
        told = h.log.updates;
        return asked;
    };
    h.on(P_PUMP_ON);
    CHECK(settle() && partsOn(h) == 1);
    h.on(P_SHAPE_ON);
    CHECK(settle() && partsOn(h) == 2);
    // A part aimed at the Echo has nothing to work on while the Echo is off: it rests, and is not counted.
    h.option(P_SHAPE_TARGET, "Echo");
    CHECK(settle() && partsOn(h) == 1);
    h.option(P_SHAPE_TARGET, "All");
    CHECK(settle() && partsOn(h) == 2);
    h.option(P_PUMP_ON, "Off");   // not counted once its fade is over
    CHECK(settle() && partsOn(h) == 1);
    h.option(P_SHAPE_ON, "Off");
    CHECK(settle() && partsOn(h) == 0);
    // The Echo and Time are parts too: counted from the first block after the switch (the plugin made their memory
    // when they were switched on), and not once they have faded out.
    h.on(P_ECHO_ON);
    CHECK(settle() && partsOn(h) == 1);
    h.on(P_TIME_ON);
    CHECK(settle() && partsOn(h) == 2);
    h.on(P_PUMP_ON);
    h.on(P_SHAPE_ON);
    CHECK(settle() && partsOn(h) == 4);
    h.option(P_ECHO_ON, "Off");
    CHECK(settle() && partsOn(h) == 3);
    h.option(P_TIME_ON, "Off");
    CHECK(settle() && partsOn(h) == 2);
    h.option(P_PUMP_ON, "Off");
    h.option(P_SHAPE_ON, "Off");
    CHECK(settle() && partsOn(h) == 0);
    CHECK(starts(h.display(P_STATUS), "CPU "));
}

void detents() {
    std::printf("== a Q-Link detent moves a list one option\n");
    Host h;
    const int lists[] = {P_PUMP_CURVE, P_VOL_SHAPE, P_FLT_SHAPE, P_PAN_SHAPE, P_PUMP_RATE, P_FLT_RATE, P_PUMP_BAND,
                         P_FLT_TYPE, P_SWING_UNIT, P_PUMP_TRIGGER, P_SHAPE_TARGET, P_ECHO_ROUTE, P_A_MODE, P_D_FTYPE,
                         P_A_DIV, P_B_MSHAPE, P_C_MRATE, P_TIME_GROOVE, P_MOVE, P_MOVE_QUANT};
    for (int id : lists) {
        const int n = PARAM_INFO[id].nopts;
        h.set(id, static_cast<float>(n / 2));   // mid-list: room both ways
        bool exact = true;
        for (int k = 0; k < 4; ++k) {   // each a gesture of its own: the clock moves a second per event
            const int at = static_cast<int>(h.value(id));
            h.detent(id, +1);
            exact = exact && static_cast<int>(h.value(id)) == std::min(at + 1, n - 1);
        }
        for (int k = 0; k < 4; ++k) {
            const int at = static_cast<int>(h.value(id));
            h.detent(id, -1);
            exact = exact && static_cast<int>(h.value(id)) == std::max(at - 1, 0);
        }
        if (!exact) std::printf("  not one at a time: %s\n", PARAM_INFO[id].key);
        CHECK(exact);
    }
    // The ends hold.
    h.set(P_PUMP_CURVE, 0.0f);
    h.detent(P_PUMP_CURVE, -1);
    CHECK(h.value(P_PUMP_CURVE) == 0.0f);
    h.set(P_VOL_SHAPE, static_cast<float>(PARAM_INFO[P_VOL_SHAPE].nopts - 1));
    h.detent(P_VOL_SHAPE, +1);
    CHECK(h.value(P_VOL_SHAPE) == static_cast<float>(PARAM_INFO[P_VOL_SHAPE].nopts - 1));
    // The names the Q-Link walks are the lists' own.
    h.set(P_PUMP_CURVE, 0.0f);
    h.detent(P_PUMP_CURVE, +1);
    CHECK(h.display(P_PUMP_CURVE) == "Soft");
    h.set(P_VOL_SHAPE, 0.0f);
    h.detent(P_VOL_SHAPE, +1);
    CHECK(h.display(P_VOL_SHAPE) == "Triangle");
    h.detent(P_VOL_SHAPE, -1);
    CHECK(h.display(P_VOL_SHAPE) == "Sine");
    // Several detents of one turn (a few ms apart) are one option each as well, the 38 shapes included.
    h.set(P_VOL_SHAPE, 10.0f);
    {
        Turn turn;
        for (int k = 0; k < 5; ++k) h.detent(P_VOL_SHAPE, +1);
    }
    CHECK(h.value(P_VOL_SHAPE) == 15.0f);
    h.set(P_VOL_SHAPE, 15.0f);   // turned back after a pause: a turn of its own
    {
        Turn turn;
        for (int k = 0; k < 7; ++k) h.detent(P_VOL_SHAPE, -1);
    }
    CHECK(h.value(P_VOL_SHAPE) == 8.0f);
    // A knob follows MPC's value, and the snapped list is pushed back so the Force's knob lands on the option.
    h.blocks(2);
    CHECK(h.log.automated[P_VOL_SHAPE] == h.get(P_VOL_SHAPE));
    const float depth = h.get(P_PUMP_DEPTH);
    h.detent(P_PUMP_DEPTH, +1);
    CHECK(h.get(P_PUMP_DEPTH) > depth && h.get(P_PUMP_DEPTH) < depth + 0.02f);
    // A tap on an Off | On switch lands on the option it names (and MPC's release echo sends the same again).
    h.set(P_PUMP_ON, 1.0f);
    CHECK(h.value(P_PUMP_ON) == 1.0f && h.display(P_PUMP_ON) == "On");
    h.set(P_PUMP_ON, 0.0f);
    CHECK(h.value(P_PUMP_ON) == 0.0f);
}

void presetsRoundTrip() {
    std::printf("== a user preset: saved, Init, loaded back\n");
    freshUserPresets();
    Host h;
    CHECK(h.display(P_PRESET) == "PRESET  Utility / Init");   // a new instance is Init
    Rng rng(99);
    spread(h, rng);
    Host reference;   // the same sound, to compare with once it is back
    reference.load(h.chunk());
    h.press(P_PRE_SAVE);
    // The file is the state text: a header and a line for every sound parameter, no project's own.
    const std::string saved = fileText(fixtureDir() + "/presets/User/User 001.gfp");
    CHECK(starts(saved, "grooveforce 1\n") && has(saved, "\nswing=") && has(saved, "\nvol_shape=") && has(saved, "\nflt_cutoff="));
    CHECK(!has(saved, "preset=") && !has(saved, "status=") && !has(saved, "pre_save="));
    int lines = 0;
    for (char c : saved) lines += c == '\n';
    int sound = 0;
    for (int i = 0; i < P_COUNT; ++i) sound += PARAM_INFO[i].kind == Kind::Synth;
    CHECK(lines == sound + 1);
    CHECK(has(h.display(P_PRESET), "User 001"));
    // Init: every sound parameter at its default.
    h.press(P_PRE_INIT);
    CHECK(allDefaults(h) && has(h.display(P_PRESET), "Init"));
    // And back, by a Q-Link detent on the preset stepper: the user's category follows the factory's.
    h.detent(P_PRESET, +1);
    CHECK(has(h.display(P_PRESET), "User 001"));
    CHECK(sameSound(h, reference));
    // The same by the arrows.
    h.press(P_PRE_INIT);
    h.press(P_PRESET_NEXT);
    CHECK(sameSound(h, reference));
    h.press(P_PRESET_PREV);
    CHECK(allDefaults(h));
    // The next save is the next number; the first stays as it was.
    h.press(P_PRESET_NEXT);
    h.setN(P_SWING, 1.0f);
    h.press(P_PRE_SAVE);
    CHECK(fs::exists(fixtureDir() + "/presets/User/User 002.gfp") && fileText(fixtureDir() + "/presets/User/User 001.gfp") == saved);
    // A file edited by hand: an unknown key, a wrong value and a missing parameter don't spoil the rest.
    std::ofstream(fixtureDir() + "/presets/User/User 003.gfp")
        << "grooveforce 1\nswing=0.7\nno_such_key=5\npump_curve=Wobble\npump_rate=1 bar\nvol_depth=oops\n";
    Host g;
    g.press(P_PRESET_NEXT);   // Init -> User 001 -> User 002 -> User 003
    g.press(P_PRESET_NEXT);
    g.press(P_PRESET_NEXT);
    CHECK(has(g.display(P_PRESET), "User 003"));
    CHECK(g.display(P_SWING) == "70%" && g.display(P_PUMP_RATE) == "1 bar");
    CHECK(g.display(P_PUMP_CURVE) == "Smooth" && g.display(P_VOL_DEPTH) == "0%");   // what it didn't name is the default
    freshUserPresets();
}

// A new instance is Init, and its preset stepper stands there: wherever Init sorts (08_Utility, last of the factory set),
// the arrows go on from it, and the browser still opens on the first category. The first preset and the one before Init
// are the listing's, whatever the factory set holds.
void freshInstance() {
    std::printf("== a new instance: on Init\n");
    freshUserPresets();
    presetLibrary().rescan();   // the folders as they are now (no user presets)
    const auto lst = presetLibrary().listing();
    const int initAt = lst->find("builtin:Init");
    CHECK(initAt >= 1 && lst->items[static_cast<size_t>(std::max(initAt, 1) - 1)].builtin >= 0);
    const Entry& first = lst->items.front();
    const Entry& before = lst->items[static_cast<size_t>(std::max(initAt, 1) - 1)];
    Host init;
    init.press(P_PRE_INIT);
    Host h;
    CHECK(h.display(P_PRESET) == "PRESET  Utility / Init" && allDefaults(h));
    CHECK(h.get(P_PRESET) > 0.0f && h.get(P_PRESET) == init.get(P_PRESET));   // the stepper's value stands on it too
    CHECK(has(h.chunk(), "\npreset=builtin:Init\n"));
    CHECK(h.display(P_ITEM_1) == first.name);   // the first category's first preset
    h.press(P_PRESET_NEXT);   // no user presets: the end of the list, nothing to load
    CHECK(h.display(P_PRESET) == "PRESET  Utility / Init");
    h.press(P_PRESET_PREV);   // one back is the factory preset before Init
    CHECK(h.display(P_PRESET) == "PRESET  " + before.category + " / " + before.name && !allDefaults(h));
    h.press(P_PRESET_NEXT);
    CHECK(h.display(P_PRESET) == "PRESET  Utility / Init" && allDefaults(h));
    Host saver;   // with a user preset in the list, one arrow forward from a new instance is that preset
    saver.setN(P_SWING, 1.0f);
    saver.press(P_PRE_SAVE);
    Host g;
    g.press(P_PRESET_NEXT);
    CHECK(has(g.display(P_PRESET), "User 001") && g.display(P_SWING) == "75%");
    freshUserPresets();
}

// A beat for the factory presets to work on: two bars at 120 BPM (4 s) of a kick on every beat, a bass note a beat,
// hats on every sixteenth with the offbeats louder, and a detuned-saw chord a bar. Stereo; peaks at -1 dBFS.
void beat(Buf& L, Buf& R) {
    const size_t n = 2 * 4 * 22050;
    L.assign(n, 0.0f);
    R.assign(n, 0.0f);
    const double chords[2][3] = {{220.0, 261.6, 329.6}, {174.6, 220.0, 261.6}};
    const double bass[2] = {55.0, 43.65};
    uint32_t seed = 1;
    double hatLp = 0.0;
    float top = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / 44100.0;
        const int bar = static_cast<int>(t / 2.0);
        const double inBar = t - bar * 2.0, inBeat = std::fmod(t, 0.5), inStep = std::fmod(t, 0.125);
        const double env = std::min(1.0, inBar / 0.01) * (inBar < 1.8 ? 0.7 + 0.3 * std::exp(-inBar * 4.0) : (2.0 - inBar) / 0.2);
        double cl = 0.0, cr = 0.0;
        for (int v = 0; v < 3; ++v) {
            cl += 2.0 * std::fmod(t * chords[bar][v] * 1.003, 1.0) - 1.0;
            cr += 2.0 * std::fmod(t * chords[bar][v] * 0.997 + 0.3, 1.0) - 1.0;
        }
        const double kick = 0.5 * std::sin(2.0 * kPi * (50.0 * inBeat + 70.0 / 25.0 * (1.0 - std::exp(-inBeat * 25.0)))) *
                            std::exp(-inBeat * 18.0);
        const double sub = (std::fmod(t * bass[bar], 1.0) < 0.5 ? 0.12 : -0.12) * std::exp(-inBeat * 6.0);
        seed = seed * 1664525u + 1013904223u;
        const double hn = static_cast<double>(static_cast<int32_t>(seed)) / 2147483648.0;
        hatLp += 0.5 * (hn - hatLp);
        const bool off = static_cast<int>(t / 0.125) % 4 == 2;
        const double hat = (off ? 0.09 : 0.05) * (hn - hatLp) * std::exp(-inStep * 120.0);
        L[i] = static_cast<float>(0.06 * env * cl + sub + kick + hat);
        R[i] = static_cast<float>(0.06 * env * cr + sub + kick + hat * 0.9);
        top = std::max(top, std::max(std::fabs(L[i]), std::fabs(R[i])));
    }
    for (size_t i = 0; i < n; ++i) {
        L[i] *= 0.89f / top;
        R[i] *= 0.89f / top;
    }
}

// How far the output is from the input: the rms of the difference against the rms of the input, both channels, in dB.
double changeDb(const Buf& inL, const Buf& inR, const Buf& outL, const Buf& outR) {
    double diff = 0.0, sig = 0.0;
    for (size_t i = 0; i < inL.size(); ++i) {
        const double a = static_cast<double>(outL[i]) - inL[i], b = static_cast<double>(outR[i]) - inR[i];
        diff += a * a + b * b;
        sig += static_cast<double>(inL[i]) * inL[i] + static_cast<double>(inR[i]) * inR[i];
    }
    return db(std::sqrt(diff / std::max(sig, 1e-30)));
}

// Every line of a preset's text is in the parameters (an option by its name, a number to its precision), and
// everything it doesn't name is at its default.
bool landed(Host& h, const std::string& text) {
    std::istringstream in(text);
    std::string line;
    std::getline(in, line);   // the header
    std::vector<bool> named(P_COUNT, false);
    bool ok = true;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), val = line.substr(eq + 1);
        int id = -1;
        for (int i = 0; i < P_COUNT; ++i)
            if (PARAM_INFO[i].kind == Kind::Synth && key == PARAM_INFO[i].key) id = i;
        bool here = false;
        if (id >= 0) {
            named[static_cast<size_t>(id)] = true;
            if (PARAM_INFO[id].nopts > 0) {
                for (int o = 0; o < PARAM_INFO[id].nopts; ++o)
                    here = here || (val == PARAM_INFO[id].opts[o] && std::lround(h.value(id)) == o);
            } else {
                const double v = std::strtod(val.c_str(), nullptr);
                here = std::fabs(static_cast<double>(h.value(id)) - v) <= 1e-4 * std::max(1.0, std::fabs(v));
            }
        }
        if (!here) std::printf("  not in place: %s\n", line.c_str());
        ok = ok && here;
    }
    for (int i = 0; i < P_COUNT; ++i)
        if (PARAM_INFO[i].kind == Kind::Synth && !named[static_cast<size_t>(i)] && std::fabs(h.get(i) - PARAM_INFO[i].def) > 2e-5f) {
            std::printf("  not at its default: %s\n", PARAM_INFO[i].key);
            ok = false;
        }
    return ok;
}

// How far apart two outputs are: the rms of their difference against the rms of the input, both channels, from `from`
// on, in dB.
double apartDb(const Buf& aL, const Buf& aR, const Buf& bL, const Buf& bR, const Buf& inL, const Buf& inR, size_t from = 0) {
    double diff = 0.0, sig = 0.0;
    for (size_t i = from; i < inL.size(); ++i) {
        const double x = static_cast<double>(aL[i]) - bL[i], y = static_cast<double>(aR[i]) - bR[i];
        diff += x * x + y * y;
        sig += static_cast<double>(inL[i]) * inL[i] + static_cast<double>(inR[i]) * inR[i];
    }
    return db(std::sqrt(diff / std::max(sig, 1e-30)));
}

// Every factory preset, loaded by the browser's arrows on a fresh instance: it is what its file says, a project names it
// and loads it back, and on the beat at 120 BPM with the transport running it stays a number under +18 dBFS and does
// what it is for. Init is the one that doesn't: the input, bit for bit.
//
// What a preset does is checked part by part: every part it switches on changes the beat by more than -40 dB (the rms
// of the difference re the input's) against the same preset with that part's work taken down to a hair, its switch and
// everything else as they are. A hair is the Pump's depth at 1% (a banded Pump's split stays in, so Low Pump shows its
// duck and not its crossover's phase), Shape's depth at 1% (a lane's static filter stays), the Echo's level at 1% (its
// wet) and Time's groove Off (Time then only records). A Pump or Shape aimed at the Echo works on the wet alone, so it is
// judged against the wet's own rms, both runs with the dry level at 0: a duck on a quiet ghost layer counts for what it
// does to that layer, not to the whole beat. Some parts are there for the Moves alone and do nothing at rest:
// an Echo whose send is closed (a Throw opens it) with a Pump or Shape aimed at it, and Time with no groove (the Moves
// play from its ring). A preset that selects a Move plays it from GO at the second bar, and the Move changes that bar by
// more than -40 dB; one without a Move changes the beat at rest.
void factoryPresets() {
    std::printf("== the factory presets\n");
    freshUserPresets();
    // The plan's set by job (docs/plans/2026-10-26-echo-time-presets.md, Task 13), by category folder
    // (presets/Factory/NN_Category): every one is there, in its category. The loop below checks whatever the set holds.
    static const struct { const char* category; const char* name; } kPlanned[] = {
        {"Kick", "Rumble"},              {"Kick", "Rumble Deep"},          {"Kick", "Rumble Short"},
        {"Bass", "Bass Pump"},           {"Bass", "Hard Pump"},            {"Bass", "Double Pump"},
        {"Bass", "Offbeat Gate"},        {"Bass", "Rolling Bass"},         {"Bass", "Rolling Dark"},
        {"Bass", "Rolling 3/16"},        {"Hats + Perc", "Hat Accents"},   {"Hats + Perc", "Hat Shuffle"},
        {"Hats + Perc", "Polyrhythm"},   {"Hats + Perc", "Poly 5/16"},     {"Hats + Perc", "Ping-Pong Hats"},
        {"Hats + Perc", "Hat Swing 58"}, {"Hats + Perc", "Hat Swing 66"},  {"Hats + Perc", "Drunk Hats"},
        {"Hats + Perc", "Rim Echo"},     {"Chords", "Chord Gate Pump"},    {"Chords", "Filter Pump"},
        {"Chords", "Dub Chord"},         {"Chords", "Dub Chord Wide"},     {"Chords", "Dub Chord Drive"},
        {"Chords", "Stab Throw"},        {"Drum Bus", "Low Pump"},         {"Drum Bus", "Loop Groove"},
        {"Drum Bus", "Bus Swing"},       {"Drum Bus", "Fill Roller"},      {"Perform", "Breakdown"},
        {"Perform", "Build"},            {"Perform", "Throws"},            {"Perform", "Glitch"},
        {"Echo", "Rumble Echo"},         {"Echo", "Dub"},                  {"Echo", "Roll"},
        {"Echo", "Polyrhythm Echo"},     {"Echo", "Ping-Pong"},            {"Echo", "Tape"},
        {"Echo", "Slap"},                {"Echo", "Spread Cloud"},         {"Echo", "Freeze"},
        {"Utility", "Init"}};
    struct Hair {
        int on, param;
        float value;
        const char* what;
    };
    static const Hair kHairs[] = {{P_PUMP_ON, P_PUMP_DEPTH, 0.01f, "Pump"}, {P_SHAPE_ON, P_SHAPE_DEPTH, 0.01f, "Shape"},
                                  {P_ECHO_ON, P_ECHO_LEVEL, 0.01f, "Echo"}, {P_TIME_ON, P_TIME_GROOVE, 0.0f, "Time"}};
    std::vector<int> factory;   // the listing's built-ins, in the order the arrows walk them
    int items = 0;
    {
        presetLibrary().rescan();   // the folders as they are now (no user presets)
        const auto lst = presetLibrary().listing();
        items = static_cast<int>(lst->items.size());
        for (int i = 0; i < items; ++i)
            if (lst->items[static_cast<size_t>(i)].builtin >= 0) factory.push_back(i);
        for (const auto& want : kPlanned) {
            const int at = lst->find(std::string("builtin:") + want.name);
            CHECK(at >= 0 && lst->items[static_cast<size_t>(std::max(at, 0))].category == want.category);
        }
    }
    Buf l, r;
    beat(l, r);
    const size_t bar2 = l.size() / 2 / kBlock * kBlock;   // the last block boundary before the second bar: GO goes there
    for (size_t k = 0; k < factory.size(); ++k) {
        Host h;
        const auto lst = presetLibrary().listing();
        const Entry& e = lst->items[static_cast<size_t>(factory[k])];
        // Init, back to the first item, then forward to this one.
        h.press(P_PRE_INIT);
        for (int i = 0; i < items; ++i) h.press(P_PRESET_PREV);
        for (size_t i = 0; i < k; ++i) h.press(P_PRESET_NEXT);
        const std::string shown = "PRESET  " + e.category + " / " + e.name;
        CHECK(h.display(P_PRESET) == shown);
        std::string text;
        CHECK(presetText(e.key, text) && landed(h, text));
        {
            Host back;   // a project that has it names it, and loads it back by that name ("Poly 5/16" has a slash)
            CHECK(back.load(h.chunk()) == 1 && back.display(P_PRESET) == shown);
        }
        h.play(true);
        h.run(l, r);
        const double change = changeDb(l, r, h.L, h.R);
        const float high = std::max(peak(h.L), peak(h.R));
        std::printf("  %-11s %-16s changes the beat by %6.1f dB, peak %5.1f dBFS;", e.category.c_str(), e.name.c_str(), change, db(high));
        CHECK(h.finite && high <= kCeiling);
        if (e.name == "Init") {
            std::printf(" the input, bit for bit\n");
            CHECK(h.L == l && h.R == r);
            continue;
        }
        // An Echo whose send is closed is silent until a Move opens it, and so is a part aimed at it.
        const bool closed = h.value(P_ECHO_SEND) == 0.0f;
        auto onEcho = [&h](int target) { return std::lround(h.value(target)) == Groove::kEcho; };
        for (const Hair& q : kHairs) {
            if (h.value(q.on) < 0.5f) continue;
            if (closed && (q.param == P_ECHO_LEVEL || (q.param == P_PUMP_DEPTH && onEcho(P_PUMP_TARGET)) ||
                           (q.param == P_SHAPE_DEPTH && onEcho(P_SHAPE_TARGET))))
                continue;   // for the Throws
            if (q.param == P_TIME_GROOVE && h.value(P_TIME_GROOVE) == 0.0f) continue;   // for the Moves
            const bool wet = (q.param == P_PUMP_DEPTH && onEcho(P_PUMP_TARGET)) || (q.param == P_SHAPE_DEPTH && onEcho(P_SHAPE_TARGET));
            Host f, g;   // the preset (the wet alone, for a part on the Echo) and the same with the part at a hair
            CHECK(g.load(text) == 1 && (!wet || f.load(text) == 1));
            g.set(q.param, q.value);
            if (wet) {
                f.set(P_DRY, 0.0f);
                g.set(P_DRY, 0.0f);
                f.play(true);
                f.run(l, r);
            }
            g.play(true);
            g.run(l, r);
            const Host& a = wet ? f : h;
            const double part = apartDb(a.L, a.R, g.L, g.R, wet ? a.L : l, wet ? a.R : r);
            std::printf(" %s%s %.1f", q.what, wet ? " (wet)" : "", part);
            if (part <= -40.0) std::printf(" <-- does nothing");
            CHECK(f.finite && g.finite && part > -40.0);
        }
        const int move = static_cast<int>(std::lround(h.value(P_MOVE))) - 1;   // -1: None
        if (move >= 0) {
            Host m;
            CHECK(m.load(text) == 1);
            m.play(true);
            Buf moved[2];
            m.run(Buf(l.begin(), l.begin() + static_cast<long>(bar2)), Buf(r.begin(), r.begin() + static_cast<long>(bar2)));
            moved[0] = m.L;
            moved[1] = m.R;
            m.press(P_MOVE_GO);
            m.run(Buf(l.begin() + static_cast<long>(bar2), l.end()), Buf(r.begin() + static_cast<long>(bar2), r.end()));
            moved[0].insert(moved[0].end(), m.L.begin(), m.L.end());
            moved[1].insert(moved[1].end(), m.R.begin(), m.R.end());
            const double played = apartDb(moved[0], moved[1], h.L, h.R, l, r, bar2);
            std::printf(" Move %s %.1f", PARAM_INFO[P_MOVE].opts[move + 1], played);
            CHECK(m.finite && std::max(peak(moved[0]), peak(moved[1])) <= kCeiling && played > -40.0);
        } else {
            CHECK(change > -40.0);
        }
        std::printf("\n");
    }
    CHECK(factory.size() >= sizeof kPlanned / sizeof kPlanned[0]);
}

// Every factory preset stays level-matched (EffectForce's testFactory): the reference material of its category
// (tools/reference_mix.h: the kick for Kick, the bass line for Bass, the hats and the snare for Hats + Perc...) comes out
// as loud as it went in, within 0.5 dB, as `make preset-levels` left it; a preset whose Output is at the +-9 dB cap
// passes as near as the cap lets it and says how far short it is. It plays finite, its peaks under +6 dBFS. A change to
// the DSP or to a preset that moves a preset's level fails here: run make preset-levels again.
void factoryLevels() {
    std::printf("== the factory presets are level-matched\n");
    int bad = 0;
    for (int i = 0; i < kNumFactoryPresets; ++i) {
        const FactoryPreset& f = kFactoryPresets[i];
        Buf inL, inR;
        gfl::referenceMix(inL, inR, gfl::partsFor(f.category));
        const double ref = gfl::lufs(inL, inR);
        Host h;
        std::string text;
        CHECK(presetText(std::string("builtin:") + f.name, text) && h.load(text) == 1);
        h.play(true);
        h.run(inL, inR);
        const double gain = gfl::lufs(h.L, h.R) - ref;
        const bool capped = std::fabs(h.value(P_OUTPUT)) >= 8.95f;   // as matched as the +-9 dB cap allows
        const float high = std::max(peak(h.L), peak(h.R));
        const bool off = !h.finite || (std::fabs(gain) > 0.5 && !capped) || high > 2.0f;
        if (off || (capped && std::fabs(gain) > 0.5))
            std::printf("  %s / %s: %+.1f dB, peak %.1f dBFS%s%s\n", f.category, f.name, gain, db(high),
                        capped ? " (Output at the cap)" : "", h.finite ? "" : ", NOT FINITE");
        bad += off;
    }
    std::printf("  %d presets, %d off by more than 0.5 dB\n", kNumFactoryPresets, bad);
    CHECK(bad == 0);
}

void projectState() {
    std::printf("== the project: effGetChunk / effSetChunk\n");
    freshUserPresets();
    Host a;
    Rng rng(7);
    spread(a, rng);
    a.press(P_PRE_INIT);   // the project came from a preset: it remembers which
    spread(a, rng);
    const std::string chunk = a.chunk();
    CHECK(starts(chunk, "grooveforce 1\n") && has(chunk, "\npreset=builtin:Init\n"));
    {   // a second instance with the state is the first one
        Host b;
        CHECK(b.load(chunk) == 1);
        CHECK(sameSound(a, b));
        CHECK(has(b.display(P_PRESET), "Init"));
    }
    {   // the chunk MPC gets carries its own length, and a state with trailing zeros loads as well
        Host b;
        void* data = nullptr;
        const intptr_t size = a.op(vst::effGetChunk, 0, 0, &data);
        CHECK(size == static_cast<intptr_t>(chunk.size()) + 1 && data != nullptr);
        std::string padded(static_cast<const char*>(data), static_cast<size_t>(size));
        padded.append(5, '\0');
        CHECK(b.op(vst::effSetChunk, 0, static_cast<intptr_t>(padded.size()), padded.data()) == 1 && sameSound(a, b));
    }
    {   // an unknown key, a wrong value and a missing parameter are survived: the rest is taken, the others stay
        std::string odd = chunk;
        for (const char* key : {"pump_depth", "swing", "flt_type"}) {   // not in this state, or with a value that is none
            const size_t at = odd.find(std::string("\n") + key + "=");
            odd.erase(at + 1, odd.find('\n', at + 1) - at);
        }
        odd += "future_param=12\nflt_type=Notch\nswing=oops\n=\njust a line\n";
        Host b;
        b.setN(P_PUMP_DEPTH, 0.3f);
        b.setN(P_SWING, 0.6f);
        b.set(P_FLT_TYPE, 2.0f);
        CHECK(b.load(odd) == 1);
        CHECK(std::fabs(b.get(P_PUMP_DEPTH) - 0.3f) < 1e-6f);   // not listed: as it was
        CHECK(std::fabs(b.get(P_SWING) - 0.6f) < 1e-6f);        // a value that is no number: as it was
        CHECK(b.value(P_FLT_TYPE) == 2.0f);                     // an option that is none: as it was
        CHECK(std::fabs(b.get(P_FLT_CUTOFF) - a.get(P_FLT_CUTOFF)) < 2e-5f && b.get(P_VOL_SHAPE) == a.get(P_VOL_SHAPE));
    }
    {   // a state written by a later version (keys we don't have, "grooveforce 2"), a text editor's byte-order mark and CRLF
        std::string future = chunk;
        future[12] = '2';
        future += "echo_on=On\n";
        Host b;
        CHECK(b.load(future) == 1 && sameSound(a, b));
        Host c;
        std::string crlf = "\xEF\xBB\xBF";
        for (char ch : chunk) {
            if (ch == '\n') crlf += '\r';
            crlf += ch;
        }
        CHECK(c.load(crlf) == 1 && sameSound(a, c));
    }
    {   // not ours: refused, and nothing changes
        Host b;
        const std::string before = b.chunk();
        for (const char* junk : {"", "hello", "masterforce 1\nswing=0.7\n", "grooveforce\nswing=0.7\n", "grooveforce 0\nswing=0.7\n",
                                 "effectforce 1\n"})
            CHECK(b.load(junk) == 0 && b.chunk() == before);
        CHECK(b.op(vst::effSetChunk, 0, 0, nullptr) == 0 && b.op(vst::effSetChunk, 0, 1 << 21, const_cast<char*>(chunk.c_str())) == 0);
    }
    {   // a project from a user preset names it; a file gone since only leaves the browser at no item
        Host b;
        b.setN(P_SWING, 1.0f);
        b.press(P_PRE_SAVE);
        const std::string pc = b.chunk();
        const std::string file = savedFile(pc);
        CHECK(!file.empty() && fs::exists(file));
        Host c;
        CHECK(c.load(pc) == 1 && has(c.display(P_PRESET), "User 001") && c.display(P_SWING) == "75%");
        fs::remove(file);
        Host d;
        CHECK(d.load(pc) == 1 && d.display(P_SWING) == "75%");   // the sound is in the project, the file is not needed
    }
    freshUserPresets();
}

// MPC rebuilds its parameter screen on every UpdateDisplay, and a Q-Link turned on track edit -> automation
// loses its value to the rebuild (sd88me/mpc-vst-plugins NOTES 2026-10-21). So a sound parameter's write
// asks for none; a text that changed asks at most once per 50 ms, and a change inside the gap is told at its
// end, not dropped. The first CPU report is ~172 blocks away.
void displayRate() {
    std::printf("== redraws: none for a sound parameter, at most one per 50 ms\n");
    freshUserPresets();
    Host h;
    for (int k = 0; k < 3; ++k) {   // some presets for the stepper to walk
        h.setN(P_SWING, 0.2f * static_cast<float>(k + 1));
        h.press(P_PRE_SAVE);
    }
    h.press(P_PRE_INIT);
    h.blocks(Surface::kTextGap / kBlock + 1);   // the first texts told, the gap over
    const int settled = h.log.updates;
    const float depth = h.get(P_PUMP_DEPTH), curve = h.get(P_PUMP_CURVE);
    {
        Turn turn;
        for (int k = 0; k < 16; ++k) {   // two Q-Links turned, a detent per block: a knob and a list
            h.detent(P_PUMP_DEPTH, +1);
            h.detent(P_PUMP_CURVE, +1);
            h.blocks(1);
        }
    }
    CHECK(h.get(P_PUMP_DEPTH) > depth && h.get(P_PUMP_CURVE) > curve);
    CHECK(h.log.automated[P_PUMP_CURVE] == h.get(P_PUMP_CURVE));   // the snapped step pushed back, without a redraw
    CHECK(h.log.updates == settled);
    // The preset stepper walks presets, and each one is a new text: a sweep of 4096 frames asks at most twice...
    const int before = h.log.updates;
    {
        Turn turn;
        for (int k = 0; k < 4096 / kBlock; ++k) {
            h.press(k % 2 == 0 ? P_PRESET_NEXT : P_PRESET_PREV);
            h.blocks(1);
        }
    }
    CHECK(h.log.updates - before >= 1 && h.log.updates - before <= 2);
    // ...and where it stopped is told once the gap is over.
    const int swept = h.log.updates;
    h.blocks(Surface::kTextGap / kBlock + 1);
    CHECK(h.log.updates == swept + 1);
    h.blocks(Surface::kTextGap / kBlock + 1);
    CHECK(h.log.updates == swept + 1);   // and nothing more
    freshUserPresets();
}

// Stop: MPC suspends the plugin and resumes it ~100 ms later. The slot's ON button off: the same, for as long as it is
// off. Under 250 ms every state is kept, over it everything is cleared. The state a level has is its glide (a full scale
// a millisecond: 0 -> +24 dB takes 655 samples); suspendPumping below is the same with the Pump running.
void suspendResume() {
    std::printf("== suspend and resume (Stop, the slot's ON button)\n");
    const Buf in(kBlock, 0.1f);
    auto gliding = [&in](Host& h) {   // Input 0 -> +24 dB: a glide of 655 samples, 128 of them played
        h.blocks(1);
        h.set(P_INPUT, 24.0f);
        h.run(in);
    };
    {   // Stop (~100 ms): the glide goes on where it was, no step, and the project is as it was
        Host h;
        gliding(h);
        const float last = h.L.back();
        const std::string project = h.chunk();
        CHECK(last > 0.3f && last < 0.5f);
        {
            Turn quick(100);
            h.op(vst::effMainsChanged, 0, 0);
            h.op(vst::effMainsChanged, 0, 1);
        }
        h.run(in);
        CHECK(std::fabs(h.L.front() - last) < 0.003f);           // no click: one step of the glide (0.0023)
        CHECK(h.L.back() > last + 0.2f && h.L.back() < 1.0f);    // still on its way to 1.585
        CHECK(h.chunk() == project);
    }
    {   // MPC suspends a second time before it resumes (it does): the first time counts
        Host h;
        gliding(h);
        const float last = h.L.back();
        {
            Turn quick(100);
            h.op(vst::effMainsChanged, 0, 0);
            h.op(vst::effMainsChanged, 0, 0);
            h.op(vst::effMainsChanged, 0, 1);
        }
        h.run(in);
        CHECK(std::fabs(h.L.front() - last) < 0.003f);
    }
    {   // the slot switched off for a while: everything cleared, the glide done at once
        Host h;
        gliding(h);
        const std::string project = h.chunk();
        {
            Turn slow(300);
            h.op(vst::effMainsChanged, 0, 0);
            h.op(vst::effMainsChanged, 0, 1);
        }
        h.run(in);
        CHECK(std::fabs(h.L.front() - 1.5849f) < 0.005f && std::fabs(h.L.back() - 1.5849f) < 0.005f);
        CHECK(h.chunk() == project);   // the sound is the user's, not a tail
        h.run(in);
        CHECK(std::fabs(h.L.back() - 1.5849f) < 0.005f);
    }
    {   // resumed without having been suspended: nothing is cleared
        Host h;
        gliding(h);
        const float last = h.L.back();
        {
            Turn slow(2000);
            h.op(vst::effMainsChanged, 0, 1);
        }
        h.run(in);
        CHECK(std::fabs(h.L.front() - last) < 0.003f);
    }
    CHECK(Host().op(vst::effGetTailSize) >= 1);
}

// The same with the Pump running (the case the glue's 250 ms rule is for): the Pump has its depth coming in over
// 20 ms when MPC stops, and the beat is a fifth of a second away. A Stop of ~100 ms resets nothing, so the depth goes
// on coming in where it was (no step), the dip lands on the beat, and the project is as it was; a slot off for 300 ms
// clears it, and the depth is all there at once. A reset on a short Stop would be a step of 0.3 here.
void suspendPumping() {
    std::printf("== suspend and resume with the Pump running\n");
    auto begin = [](Host& h, Buf& all) {
        pumpOn(h, 0.0f);
        runDc(h, all, 1);      // depth 0: the input as it is
        h.set(P_PUMP_DEPTH, 1.0f);
        runDc(h, all, 2);      // 256 samples on its way up: about a third of the depth
    };
    for (int times = 1; times <= 2; ++times) {   // Stop: ~100 ms, MPC suspends twice before it resumes (it does)
        Host h;
        Buf all;
        begin(h, all);
        const float last = all.back();
        const std::string project = h.chunk();
        CHECK(last > 0.3f && last < 0.45f);
        {
            Turn quick(100);
            for (int k = 0; k < times; ++k) h.op(vst::effMainsChanged, 0, 0);
            h.op(vst::effMainsChanged, 0, 1);
        }
        runDc(h, all, 180);   // past the first beat, at 22050
        CHECK(std::fabs(all[384] - last) < kSlope);   // no click: the next step of the glide
        CHECK(maxStep(all) < kSlope);
        const std::vector<int> at = ducks(all, kDuck);
        CHECK(at.size() == 1 && std::abs(at[0] - kBeat) <= 3);   // and the beat is where MPC's song position puts it
        CHECK(h.chunk() == project);
        CHECK(h.finite);
    }
    {   // the slot off for 300 ms: cleared; the depth is all there, and the beat is still on the grid
        Host h;
        Buf all;
        begin(h, all);
        const float last = all.back();
        {
            Turn slow(300);
            h.op(vst::effMainsChanged, 0, 0);
            h.op(vst::effMainsChanged, 0, 1);
        }
        runDc(h, all, 180);
        CHECK(all[384] < 0.1f && last - all[384] > 0.2f);   // full depth at once: 0.5 x the level 0.085 into the recovery
        const std::vector<int> at = ducks(all, kDuck);
        CHECK(at.size() == 1 && std::abs(at[0] - kBeat) <= 3);
        CHECK(h.finite);
    }
    {   // MPC stops the transport and then suspends: the grid runs on at the tempo
        Host h;
        Buf all;
        begin(h, all);
        h.play(false);
        {
            Turn quick(100);
            h.op(vst::effMainsChanged, 0, 0);
            h.op(vst::effMainsChanged, 0, 1);
        }
        runDc(h, all, 180);   // Play was not pressed: still 22050 to the beat, at 120 BPM
        const std::vector<int> at = ducks(all, kDuck);
        CHECK(at.size() == 1 && std::abs(at[0] - kBeat) <= 3 && maxStep(all) < kSlope);
    }
}

// MPC's transport doing odd things, and the host's too.
void shake(Host& h, Rng& rng) {
    static const double odd[] = {std::nan(""), INFINITY, -INFINITY, 1e300, -1e300, 0.0, -3.0, 1e15, -1e15};
    const int oddCount = static_cast<int>(sizeof odd / sizeof odd[0]);
    switch (rng.below(10)) {
        case 0: h.log.time.ppqPos = rng.next() * 64.0 - 8.0; break;                       // a locate, also before the start
        case 1: h.log.time.ppqPos = std::floor(rng.next() * 16.0) * 4.0; break;           // back to a loop's start
        case 2: h.log.time.tempo = 30.0 + rng.next() * 270.0; break;                      // a tempo change
        case 3: h.play(rng.next() < 0.5f); break;                                         // Play, Stop
        case 4: h.log.noTime = rng.next() < 0.3f; break;                                  // a host with no time to give
        case 5: h.log.time.flags ^= vst::kVstPpqPosValid; break;
        case 6: h.log.time.flags ^= vst::kVstTempoValid; break;
        case 7: h.log.time.tempo = odd[rng.below(oddCount)]; break;
        case 8: h.log.time.ppqPos = odd[rng.below(oddCount)]; break;
        default: h.log.time.ppqPos += (rng.next() - 0.5) * 0.01; break;                    // a position a hair off
    }
}

// --- the Echo's, Time's and the Moves' parameters as the chain gets them ----------------------------------------------

using Fields = std::map<std::string, double>;

// Every field grooveParams writes, by name: a parameter that lands on another's field (or on two) shows in the difference.
Fields fieldsOf(const Groove::Params& p) {
    Fields f;
    f["pumpOn"] = p.pumpOn;
    f["shapeOn"] = p.shapeOn;
    f["pumpTarget"] = p.pumpTarget;
    f["shapeTarget"] = p.shapeTarget;
    f["pump.rateBeats"] = p.pump.rateBeats;
    f["pump.curve"] = p.pump.curve;
    f["pump.depth"] = p.pump.depth;
    f["pump.length"] = p.pump.length;
    f["pump.pushMs"] = p.pump.pushMs;
    f["pump.trigger"] = p.pump.trigger;
    f["pump.sensitivity"] = p.pump.sensitivity;
    f["pump.band"] = p.pump.band;
    f["pump.crossoverHz"] = p.pump.crossoverHz;
    f["pump.mix"] = p.pump.mix;
    f["shape.amount"] = p.shape.amount;
    f["shape.mix"] = p.shape.mix;
    f["shape.filterType"] = p.shape.filterType;
    f["shape.cutoffHz"] = p.shape.cutoffHz;
    f["shape.resonance"] = p.shape.resonance;
    for (int l = 0; l < Shape::kLanes; ++l) {
        const std::string k = "shape.lane" + std::to_string(l) + ".";
        const Shape::LaneParams& q = p.shape.lane[l];
        f[k + "on"] = q.on;
        f[k + "shape"] = q.shape;
        f[k + "rateBeats"] = q.rateBeats;
        f[k + "depth"] = q.depth;
        f[k + "phase"] = q.phase;
    }
    f["swing.amount"] = p.swing.amount;
    f["swing.unitBeats"] = p.swing.unitBeats;
    f["inputDb"] = p.inputDb;
    f["outputDb"] = p.outputDb;
    f["mix"] = p.mix;
    f["dry"] = p.dry;
    f["echoOn"] = p.echoOn;
    f["order"] = p.order;
    f["timeOn"] = p.timeOn;
    f["timePlace"] = p.timePlace;
    f["time.groove"] = p.time.groove;
    f["time.amount"] = p.time.amount;
    f["time.pattern"] = p.time.pattern;   // a Move's, not a parameter's: grooveParams leaves them be
    f["time.patternStart"] = p.time.patternStart;
    f["time.loop"] = p.time.loop;
    const Echo::Params& e = p.echo;
    f["echo.routing"] = e.routing;
    f["echo.send"] = e.send;
    f["echo.duck"] = e.duck;
    f["echo.freeze"] = e.freeze;
    f["echo.glide"] = e.glide;
    f["echo.width"] = e.width;
    f["echo.level"] = e.level;
    f["echo.feedbackOffset"] = e.feedbackOffset;
    f["echo.toneOct"] = e.toneOct;
    f["echo.sensitivity"] = e.sensitivity;
    f["echo.swing.amount"] = e.swing.amount;   // set by the chain
    f["echo.swing.unitBeats"] = e.swing.unitBeats;
    for (int i = 0; i < Echo::kLines; ++i) {
        const std::string k = std::string("echo.line") + static_cast<char>('A' + i) + ".";
        const Echo::Line& q = e.line[i];
        f[k + "on"] = q.on;
        f[k + "level"] = q.level;
        f[k + "pan"] = q.pan;
        f[k + "sync"] = q.p.sync;
        f[k + "divBeats"] = q.p.divBeats;
        f[k + "timeMs"] = q.p.timeMs;
        f[k + "feedback"] = q.p.feedback;
        f[k + "mode"] = q.p.mode;
        f[k + "filterType"] = q.p.filterType;
        f[k + "cutoffHz"] = q.p.cutoffHz;
        f[k + "resonance"] = q.p.resonance;
        f[k + "place"] = q.p.place;
        f[k + "drive"] = q.p.drive;
        f[k + "wow"] = q.p.wow;
        f[k + "glide"] = q.p.glide;   // the Echo's, not the line's
        f[k + "timeMod"] = q.p.timeMod;
        f[k + "cutoffModOct"] = q.p.cutoffModOct;
        f[k + "mod.mode"] = q.mod.mode;
        f[k + "mod.shape"] = q.mod.shape;
        f[k + "mod.rateBeats"] = q.mod.rateBeats;
        f[k + "mod.sensitivity"] = q.mod.sensitivity;
        f[k + "modCutoff"] = q.modCutoff;
        f[k + "modTime"] = q.modTime;
        f[k + "modLevel"] = q.modLevel;
    }
    return f;
}

int idOf(const std::string& key) {
    for (int i = 0; i < P_COUNT; ++i)
        if (key == PARAM_INFO[i].key) return i;
    return -1;
}

using Want = std::vector<std::pair<std::string, double>>;

// From the defaults, parameter `key` at the real value `value` (an option: its index): exactly the fields named have
// changed, to the values named, and every other field is what the defaults make it.
bool lands(const std::string& key, double value, const Want& want) {
    const int id = idOf(key);
    if (id < 0) {
        std::printf("  no parameter %s\n", key.c_str());
        return false;
    }
    float n[P_COUNT];
    for (int i = 0; i < P_COUNT; ++i) n[i] = PARAM_INFO[i].def;
    const Fields base = fieldsOf(grooveParams(n));
    n[id] = paramNorm(id, static_cast<float>(value));
    const Fields got = fieldsOf(grooveParams(n));
    Fields exp = base;
    for (const auto& w : want) {
        if (!exp.count(w.first)) {
            std::printf("  %s: no field %s\n", key.c_str(), w.first.c_str());
            return false;
        }
        exp[w.first] = w.second;
    }
    bool ok = true;
    for (const auto& e : exp) {
        const double g = got.at(e.first);
        if (!(std::fabs(g - e.second) <= 1e-4 * std::max(1.0, std::fabs(e.second)))) {
            std::printf("  %s = %g: %s is %g, not %g\n", key.c_str(), value, e.first.c_str(), g, e.second);
            ok = false;
        }
    }
    return ok;
}

// Every new key lands on its field, one at a time (so a swapped pair fails), every list on its own entries, and a value a
// hair off its rest is its rest exactly.
void mappingEcho() {
    std::printf("== grooveParams: the Echo, Time and the lines, one key at a time\n");
    auto near = [](double a, double b) { return std::fabs(a - b) <= 1e-4 * std::max(1.0, std::fabs(b)); };

    // The parameters that are the chain's own.
    CHECK(lands("dry", 0.25, {{"dry", 0.25}}));
    CHECK(lands("echo_on", 1, {{"echoOn", 1}}));
    CHECK(lands("echo_order", 1, {{"order", Groove::kEchoLast}}));
    for (int o = 0; o < Echo::kRoutings; ++o) CHECK(lands("echo_route", o, {{"echo.routing", o}}));
    CHECK(lands("echo_send", 0.4, {{"echo.send", 0.4}}));
    CHECK(lands("echo_fb", 0.3, {{"echo.feedbackOffset", 0.3}}));
    CHECK(lands("echo_fb", -0.6, {{"echo.feedbackOffset", -0.6}}));
    CHECK(lands("echo_tone", 0.5, {{"echo.toneOct", 2.0}}));   // -1..1 is four octaves either way
    CHECK(lands("echo_tone", -1.0, {{"echo.toneOct", -4.0}}));
    CHECK(lands("echo_duck", 0.6, {{"echo.duck", 0.6}}));
    CHECK(lands("echo_freeze", 1, {{"echo.freeze", 1}}));
    CHECK(lands("echo_glide", 1, {{"echo.glide", EchoLine::kFade}}));
    CHECK(lands("echo_width", 1.25, {{"echo.width", 1.25}}));
    CHECK(lands("echo_width", 0.0, {{"echo.width", 0.0}}));
    CHECK(lands("echo_level", 0.7, {{"echo.level", 0.7}}));
    CHECK(lands("echo_sens", 0.8, {{"echo.sensitivity", 0.8}}));
    CHECK(lands("time_on", 1, {{"timeOn", 1}}));
    CHECK(lands("time_place", 1, {{"timePlace", Groove::kPre}}));
    for (int o = 0; o < 4; ++o) CHECK(lands("time_groove", o, {{"time.groove", o}}));
    CHECK(lands("time_amount", 0.8, {{"time.amount", 0.8}}));
    // The Move parameters are not the chain's: the Moves read them, the chain gets none of them.
    for (const char* key : {"move", "move_mode", "move_quant", "move_go", "move_1", "move_16"}) CHECK(lands(key, 1, {}));
    CHECK(lands("move", 9, {}));

    // The four lines, each its twenty keys.
    for (int i = 0; i < Echo::kLines; ++i) {
        const std::string p = std::string(1, static_cast<char>('a' + i)) + "_";
        const std::string f = std::string("echo.line") + static_cast<char>('A' + i) + ".";
        auto key = [&p](const char* k) { return p + k; };
        CHECK(lands(key("on"), 1, {{f + "on", 1}}));
        CHECK(lands(key("level"), 0.3, {{f + "level", 0.3}}));
        CHECK(lands(key("pan"), -0.6, {{f + "pan", -0.6}}));
        CHECK(lands(key("sync"), 1, {{f + "sync", 0}}));   // Sync | Free
        CHECK(lands(key("ms"), 120.0, {{f + "timeMs", 120.0}}));
        CHECK(lands(key("fb"), 0.9, {{f + "feedback", 0.9}}));
        CHECK(lands(key("ftype"), 1, {{f + "filterType", EchoLine::kBP}}));
        CHECK(lands(key("ftype"), 2, {{f + "filterType", EchoLine::kHP}}));
        CHECK(lands(key("cut"), 800.0, {{f + "cutoffHz", 800.0}}));
        CHECK(lands(key("reso"), 0.6, {{f + "resonance", 0.6}}));
        CHECK(lands(key("place"), 1, {{f + "place", EchoLine::kOnOutput}}));
        CHECK(lands(key("drive"), 0.45, {{f + "drive", 0.45}}));
        CHECK(lands(key("wow"), 0.35, {{f + "wow", 0.35}}));
        CHECK(lands(key("mcut"), -0.5, {{f + "modCutoff", -0.5}}));
        CHECK(lands(key("mtime"), 0.7, {{f + "modTime", 0.7}}));
        CHECK(lands(key("mlevel"), 0.4, {{f + "modLevel", 0.4}}));
        for (int o = 0; o < EchoLine::kMono + 1; ++o) CHECK(lands(key("mode"), o, {{f + "mode", o}}));
        for (int o = 0; o < 4; ++o) CHECK(lands(key("mod"), o, {{f + "mod.mode", o}}));   // Off | LFO | Env | Follow
        // The lists, every entry: a division is the beats of kEchoDivs, a rate those of kShapeRates, a shape its index.
        for (int o = 0; o < kNumEchoDivs; ++o) CHECK(lands(key("div"), o, {{f + "divBeats", kEchoDivs[o].beats}}));
        for (int o = 0; o < kNumShapeRates; ++o) CHECK(lands(key("mrate"), o, {{f + "mod.rateBeats", kShapeRates[o].beats}}));
        for (int o : {1, 7, PARAM_INFO[P_A_MSHAPE].nopts - 1}) CHECK(lands(key("mshape"), o, {{f + "mod.shape", o}}));
    }
    // The defaults of the lines are the surface's: A 1/8., B 1/4, C 5/16, D 1/2.
    {
        float n[P_COUNT];
        for (int i = 0; i < P_COUNT; ++i) n[i] = PARAM_INFO[i].def;
        const Groove::Params d = grooveParams(n);
        CHECK(d.echo.line[0].p.divBeats == 0.75 && d.echo.line[1].p.divBeats == 1.0 && d.echo.line[2].p.divBeats == 1.25 &&
              d.echo.line[3].p.divBeats == 2.0);
        bool allOff = !d.echoOn && !d.timeOn && d.order == Groove::kEchoFirst && d.timePlace == Groove::kPost && d.dry == 1.0f;
        for (const Echo::Line& l : d.echo.line) allOff = allOff && !l.on && near(l.level, 0.8) && l.p.sync && near(l.p.feedback, 0.4);
        CHECK(allOff);
        CHECK(d.echo.send == 1.0f && d.echo.width == 1.0f && d.echo.level == 1.0f && d.echo.duck == 0.0f && d.echo.toneOct == 0.0f &&
              d.echo.feedbackOffset == 0.0f && !d.echo.freeze && d.echo.routing == Echo::kParallel && d.echo.glide == EchoLine::kTape);
        CHECK(d.time.groove == Time::kOff && near(d.time.amount, 0.5) && d.time.pattern == -1 && !d.time.loop);
    }

    // A hair off its rest is its rest, exactly; a real step is not.
    float n[P_COUNT];
    for (int i = 0; i < P_COUNT; ++i) n[i] = PARAM_INFO[i].def;
    n[P_DRY] = 1.0f - 4e-4f;
    n[P_ECHO_SEND] = 1.0f - 4e-4f;
    n[P_ECHO_FB] = 0.5f + 2e-4f;
    n[P_ECHO_TONE] = 0.5f - 2e-4f;
    n[P_ECHO_DUCK] = 4e-4f;
    n[P_ECHO_WIDTH] = 1.0f / 1.5f + 4e-4f;
    n[P_ECHO_LEVEL] = 1.0f - 4e-4f;
    for (int i = 0; i < Echo::kLines; ++i) {
        const std::string p = std::string(1, static_cast<char>('a' + i)) + "_";
        n[idOf(p + "cut")] = 1.0f - 1e-6f;
        n[idOf(p + "reso")] = 4e-4f;
        n[idOf(p + "drive")] = 4e-4f;
        n[idOf(p + "wow")] = 4e-4f;
        n[idOf(p + "pan")] = 0.5f + 2e-4f;
        n[idOf(p + "mcut")] = 0.5f - 2e-4f;
        n[idOf(p + "mtime")] = 0.5f + 2e-4f;
        n[idOf(p + "mlevel")] = 4e-4f;
    }
    const Groove::Params s = grooveParams(n);
    CHECK(s.dry == 1.0f && s.echo.send == 1.0f && s.echo.feedbackOffset == 0.0f && s.echo.toneOct == 0.0f && s.echo.duck == 0.0f &&
          s.echo.width == 1.0f && s.echo.level == 1.0f);
    bool rests = true;
    for (const Echo::Line& l : s.echo.line)
        rests = rests && l.p.cutoffHz == 20000.0f && l.p.resonance == 0.0f && l.p.drive == 0.0f && l.p.wow == 0.0f && l.pan == 0.0f &&
                l.modCutoff == 0.0f && l.modTime == 0.0f && l.modLevel == 0.0f;
    CHECK(rests);
    for (int i = 0; i < P_COUNT; ++i) n[i] = PARAM_INFO[i].def;
    n[P_ECHO_DUCK] = 0.01f;
    n[P_A_DRIVE] = 0.01f;
    n[P_ECHO_FB] = 0.5f + 0.002f;
    const Groove::Params t = grooveParams(n);
    CHECK(near(t.echo.duck, 0.01) && near(t.echo.line[0].p.drive, 0.01) && t.echo.feedbackOffset > 0.003f);

    // Whatever MPC or a hand-edited file says, the lines get numbers they can use.
    bool usable = true;
    for (float w : {std::nanf(""), -1.0f, 2.0f, 1e30f, -1e30f, 0.0f, 1.0f}) {
        for (int i = 0; i < P_COUNT; ++i) n[i] = w;
        const Groove::Params q = grooveParams(n);
        usable = usable && std::isfinite(q.dry) && std::isfinite(q.echo.send) && std::isfinite(q.echo.duck) &&
                 std::isfinite(q.echo.width) && std::isfinite(q.echo.level) && std::isfinite(q.echo.feedbackOffset) &&
                 std::isfinite(q.echo.toneOct) && std::isfinite(q.echo.sensitivity) && std::isfinite(q.time.amount);
        usable = usable && q.echo.routing >= 0 && q.echo.routing < Echo::kRoutings && q.echo.glide >= 0 && q.echo.glide <= 1 &&
                 q.order >= Groove::kEchoFirst && q.order <= Groove::kEchoLast && q.timePlace >= Groove::kPost &&
                 q.timePlace <= Groove::kPre && q.time.groove >= Time::kOff && q.time.groove <= Time::kPush;
        for (const Echo::Line& l : q.echo.line)
            usable = usable && std::isfinite(l.level) && std::isfinite(l.pan) && std::isfinite(l.p.divBeats) && l.p.divBeats > 0.0 &&
                     std::isfinite(l.p.timeMs) && l.p.timeMs >= 1.0f && l.p.timeMs <= 2000.0f && std::isfinite(l.p.feedback) &&
                     l.p.mode >= EchoLine::kStereo && l.p.mode <= EchoLine::kMono && l.p.filterType >= EchoLine::kLP &&
                     l.p.filterType <= EchoLine::kHP && std::isfinite(l.p.cutoffHz) && std::isfinite(l.p.resonance) &&
                     l.p.place >= EchoLine::kInLoop && l.p.place <= EchoLine::kOnOutput && std::isfinite(l.p.drive) &&
                     std::isfinite(l.p.wow) && l.mod.mode >= EchoMod::kOff && l.mod.mode <= EchoMod::kFollow &&
                     l.mod.shape >= 0 && l.mod.shape < PARAM_INFO[P_A_MSHAPE].nopts && l.mod.rateBeats > 0.0 &&
                     std::isfinite(l.modCutoff) && std::isfinite(l.modTime) && std::isfinite(l.modLevel);
    }
    CHECK(usable);
}

// --- Moves, the Echo and Time through the host -------------------------------------------------------------------------

constexpr int kBar = 4 * kBeat;

// The tile of Move `m` (0..15, in the order of the Move parameter's options after None).
int tileOf(int m) { return P_MOVE_1 + m; }
// The Move parameter's option for a Move's name (0 is None); -1 when there is none.
int optionOf(const char* name) {
    for (int o = 0; o < PARAM_INFO[P_MOVE].nopts; ++o)
        if (!std::strcmp(PARAM_INFO[P_MOVE].opts[o], name)) return o;
    return -1;
}
int moveOf(const char* name) { return optionOf(name) - 1; }   // 0..15

// Every sample says where it is, so a repeated slice shows as a saw.
Buf ramp(int n, float slope) {
    Buf x(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) x[static_cast<size_t>(i)] = slope * static_cast<float>(i);
    return x;
}

// The first sample from `from` on where a and b differ (the size when they never do).
size_t firstDiff(const Buf& a, const Buf& b, size_t from = 0) {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = from; i < n; ++i)
        if (a[i] != b[i]) return i;
    return n;
}
bool same(const Buf& a, const Buf& b, size_t from, size_t to) {
    to = std::min(to, std::min(a.size(), b.size()));
    for (size_t i = from; i < to; ++i)
        if (a[i] != b[i]) return false;
    return true;
}
double maxDiff(const Buf& a, const Buf& b, size_t from, size_t to) {
    to = std::min(to, std::min(a.size(), b.size()));
    double m = 0.0;
    for (size_t i = from; i < to; ++i) m = std::max(m, std::fabs(static_cast<double>(a[i]) - b[i]));
    return m;
}

// What the host does just before block `block` of a run.
struct Act {
    int block;
    std::function<void(Host&)> fn;
};
struct Played {
    Buf l, r;
};
// l / r through the host in blocks of 128, from where it stands; the acts are done before their block. Both channels are
// kept. (The transport is the host's: playing, it moves on a block at a time.)
Played playActs(Host& h, const Buf& l, const Buf& r, const std::vector<Act>& acts = {}) {
    Played out;
    out.l.reserve(l.size());
    out.r.reserve(l.size());
    for (size_t pos = 0, block = 0; pos < l.size(); pos += kBlock, ++block) {
        for (const Act& a : acts)
            if (a.block == static_cast<int>(block)) a.fn(h);
        const size_t n = std::min<size_t>(kBlock, l.size() - pos);
        h.run(Buf(l.begin() + static_cast<long>(pos), l.begin() + static_cast<long>(pos + n)),
              Buf(r.begin() + static_cast<long>(pos), r.begin() + static_cast<long>(pos + n)));
        out.l.insert(out.l.end(), h.L.begin(), h.L.end());
        out.r.insert(out.r.end(), h.R.begin(), h.R.end());
    }
    return out;
}
Played playActs(Host& h, const Buf& in, const std::vector<Act>& acts = {}) { return playActs(h, in, in, acts); }

// x += a kick at `at`: 55 Hz, 60 ms decay.
void addKick(Buf& x, int at, float amp) {
    const double tau = 0.060 * 44100.0;
    for (int i = 0; at + i < static_cast<int>(x.size()) && i < 10 * tau; ++i)
        x[static_cast<size_t>(at + i)] += amp * static_cast<float>(std::sin(2.0 * kPi * 55.0 * i / 44100.0) * std::exp(-i / tau));
}

// A burst of a 220 Hz sine under a Hann window: nothing in it is a click.
void addBurst(Buf& x, int at, int len, float amp) {
    for (int i = 0; i < len && at + i < static_cast<int>(x.size()); ++i) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * i / len);
        x[static_cast<size_t>(at + i)] += amp * static_cast<float>(w * std::sin(2.0 * kPi * 220.0 * i / 44100.0));
    }
}

// Line A on its own, the Echo off: a repeat every `div`, `fb` of each coming back, none of the input sent unless the
// Move opens the send (send 0).
void lineAOnly(Host& h, const char* div, float fb, float send) {
    h.on(P_A_ON);
    h.option(P_A_DIV, div);
    h.set(P_A_FB, fb);
    h.set(P_ECHO_SEND, send);
}

// The Moves at 120 BPM, MPC playing from the start. Stutter 1/16 plays its first sixteenth live and then repeats it, so
// on a ramp (every sample says where it is) each repeat reads the same value at the same place in its slice: the Move is
// where the saw begins. The press at block 224 is at beat 1.3003, so the next beat is 2, sample 44100.
constexpr int kPressBlock = 224;
constexpr int kLine = 2 * kBeat;                  // sample of beat 2
constexpr int kSixteenth = kBeat / 4;             // 5512.5 samples: Stutter 1/16's slice
constexpr int kPreRoll = 220;                     // Time's crossfade, which ends on the line where the slice repeats

// The value of a ramp's repeat k (k >= 1) of the Stutter 1/16 that began at `line`, `into` samples into the slice: it is
// the ramp's value at the same place in the first slice.
size_t repeatAt(int line, int k, int into) { return static_cast<size_t>(line + std::lround(k * (kBeat / 4.0)) + into); }

// A Stutter 1/16 on ramps (left slope 5e-6, right 2.5e-6) from the state the host is in: checks the Move began at `line`
// and ended four beats on. `at` is the first sample in the output the Move can be seen in.
void checkStutter(const Played& p, const Buf& in, const Buf& inR, int line, const char* what) {
    // Live until the first repeat's crossfade begins (a sixteenth on, less Time's 5 ms), a saw from there on.
    const size_t first = firstDiff(p.l, in, static_cast<size_t>(line) - 2000);
    if (first < static_cast<size_t>(line + kSixteenth - kPreRoll - 20) || first > static_cast<size_t>(line + kSixteenth - kPreRoll + 20))
        std::printf("  %s: the Move shows from sample %zu, the model says %d\n", what, first, line + kSixteenth - kPreRoll);
    CHECK(first >= static_cast<size_t>(line + kSixteenth - kPreRoll - 20) && first <= static_cast<size_t>(line + kSixteenth - kPreRoll + 20));
    bool repeats = true;
    for (int k = 1; k <= 12; ++k) {
        const size_t at = repeatAt(line, k, 1500);
        repeats = repeats && std::fabs(p.l[at] - in[static_cast<size_t>(line + 1500)]) < 5e-5f &&
                  std::fabs(p.r[at] - inR[static_cast<size_t>(line + 1500)]) < 3e-5f;
    }
    CHECK(repeats);
    // The Move is four beats and then the input is the input again, bit for bit, both sides.
    const size_t end = static_cast<size_t>(line + 4 * kBeat);
    CHECK(same(p.l, in, end + 3000, in.size()) && same(p.r, inR, end + 3000, in.size()));
    // The repeats are crossfaded: nothing steeper than the saw's 5 ms edges. The last one is the Move's end, from the fifteenth
    // slice back to now: 15 slices of the ramp (0.41) over the 220 samples of Time's crossfade, raised-cosine: 0.0030 at most.
    CHECK(maxStep(p.l, static_cast<size_t>(line) - 1000, end - 1000) < 5e-4f);
    CHECK(maxStep(p.l, end - 1000, end + 3000) < 0.0032f);
}

// A tap on a Move's tile selects it and fires it at the quantize: tile 10 is Stutter 1/16, from the next beat. Its tile is
// lit from the tap until the Move is over, the Move parameter holds it, the readout reads it; MPC's release echo (the tile's
// opposite, 5 ms on) is not a second tap.
void tilesFireMoves() {
    std::printf("== the Move tiles: named, lit while their Move plays or waits, a tap selects and fires\n");
    CHECK(Moves::count() == 16 && PARAM_INFO[P_MOVE].nopts == 17);
    {   // the names: the Move parameter's options after None, in order; nothing is playing
        Host h;
        bool named = true, dark = true;
        for (int k = 0; k < Moves::count(); ++k) {
            named = named && h.display(tileOf(k)) == PARAM_INFO[P_MOVE].opts[k + 1] && h.display(tileOf(k)) == Moves::name(k);
            dark = dark && h.get(tileOf(k)) == 0.0f;
        }
        CHECK(named && dark);
        CHECK(h.display(tileOf(9)) == "Stutter 1/16" && h.display(P_MOVE) == "None" && moveOf("Stutter 1/16") == 9);
    }
    const int total = 175000;
    const Buf in = ramp(total, 5e-6f), inR = ramp(total, 2.5e-6f);
    const int tile = tileOf(9);

    struct Seen {
        float lit[16] = {};
        float move = -1.0f;
        std::string readout;
        int parts = -1;
    };
    Seen waiting, playing, over;
    auto look = [](Seen& s) {
        return [&s](Host& h) {
            for (int k = 0; k < 16; ++k) s.lit[k] = h.get(tileOf(k));
            s.move = h.value(P_MOVE);
            s.readout = h.display(P_MOVE);
            s.parts = partsOn(h);
        };
    };
    Host h;
    h.play(true);
    const Played p = playActs(h, in, inR,
                              {{kPressBlock, [&](Host& x) { x.tap(tile); }},
                               {kPressBlock + 2, look(waiting)},   // the tap is seen, the line is 0.7 of a beat off
                               {800, look(playing)},               // sample 102400: a beat and a half into the Move
                               {1300, look(over)}});               // sample 166400: the Move ended at 132300
    CHECK(waiting.lit[9] == 1.0f && waiting.move == 10.0f && waiting.readout == "Stutter 1/16");   // lit from the tap
    bool onlyOne = true;
    for (int k = 0; k < 16; ++k) onlyOne = onlyOne && (k == 9 || (waiting.lit[k] == 0.0f && playing.lit[k] == 0.0f && over.lit[k] == 0.0f));
    CHECK(onlyOne);
    CHECK(playing.lit[9] == 1.0f && playing.move == 10.0f && playing.parts == 1);   // Time works for as long as it plays
    CHECK(over.lit[9] == 0.0f && over.move == 10.0f && over.readout == "Stutter 1/16" && over.parts == 0);   // selected still
    CHECK(h.log.automated[tile] == 0.0f && h.log.automateCount[tile] == 1);   // MPC was told once: dark again (it had set the tile lit itself)
    checkStutter(p, in, inR, kLine, "tile");

    // MPC's release echo, the opposite value a few ms after the tap, and then the same value again: not a second tap. A
    // second tap on the tile a second on is one, and it starts the Move over at the next beat.
    auto echoes = [&](Host& x) {
        Turn turn(5);
        x.setN(tile, 0.0f);
        x.setN(tile, 1.0f);
        x.setN(tile, 0.0f);
    };
    Host e;
    e.play(true);
    const Played echoed = playActs(e, in, inR, {{kPressBlock, [&](Host& x) { x.tap(tile); }}, {kPressBlock + 1, echoes}, {400, echoes}});   // (the second with the Move playing)
    CHECK(echoed.l == p.l && echoed.r == p.r);
    // A tile set to the value it has (MPC writing its tiles back as they are) is no tap: nothing is selected or fired.
    Host quiet;
    quiet.play(true);
    const Played unchanged = playActs(quiet, in, inR, {{kPressBlock, [&](Host& x) {
                                                            x.setN(tile, 0.0f);
                                                            x.setN(tileOf(3), 0.0f);
                                                        }}});
    CHECK(unchanged.l == in && unchanged.r == inR && quiet.value(P_MOVE) == 0.0f);
    Host again;
    again.play(true);
    const Played twice = playActs(again, in, inR, {{kPressBlock, [&](Host& x) { x.tap(tile); }}, {700, [&](Host& x) { x.tap(tile); }}});
    CHECK(twice.l != p.l);   // the second tap restarted it at the next beat, sample 88200 + 22050: 6.1 beats
    CHECK(same(twice.l, p.l, 0, static_cast<size_t>(5 * kBeat - 1000)));   // as before until the second tap's line (beat 5) is near

    // Another tile in the middle of the Move replaces it at its line: tile 9 (Stutter 1/8) from beat 4 on.
    Host r;
    r.play(true);
    const Played swapped = playActs(r, in, inR, {{kPressBlock, [&](Host& x) { x.tap(tile); }}, {600, [&](Host& x) { x.tap(tileOf(8)); }}});
    CHECK(r.value(P_MOVE) == 9.0f && r.display(P_MOVE) == "Stutter 1/8");
    CHECK(same(swapped.l, p.l, 0, static_cast<size_t>(4 * kBeat - 400)));   // the same until the second tap's line (beat 4) is near
    CHECK(swapped.l != p.l);
    CHECK(std::fabs(swapped.l[static_cast<size_t>(4 * kBeat + kBeat / 2 + 1500)] - in[static_cast<size_t>(4 * kBeat + 1500)]) < 5e-5f);   // Stutter 1/8's slice repeats

    // A flood of taps in one block: all of them heard, the last one wins, nothing stuck.
    Host f;
    f.play(true);
    const Played flood = playActs(f, in, inR, {{kPressBlock, [&](Host& x) {
                                                    for (int k = 0; k < 200; ++k) x.tap(tileOf(k % 16));
                                                    x.tap(tile);
                                                }}});
    CHECK(f.value(P_MOVE) == 10.0f);
    checkStutter(flood, in, inR, kLine, "flood");
}

// The Move parameter (R35) holds the selected Move: a change fires the new one at the quantize, as a tile does, whether it
// came from a Q-Link, a list or the host's automation; GO fires the selected Move again; None stops the one that plays; and
// a preset or a project that names a Move only selects it.
void moveParameterFires() {
    std::printf("== the Move parameter: a change fires at the quantize, GO fires again, None stops\n");
    const int total = 190000;
    const Buf in = ramp(total, 5e-6f), inR = ramp(total, 2.5e-6f);
    const int stutter = optionOf("Stutter 1/16");
    CHECK(stutter == 10);
    Played viaTile;
    {
        Host h;
        h.play(true);
        viaTile = playActs(h, in, inR, {{kPressBlock, [](Host& x) { x.tap(tileOf(9)); }}});
    }
    {   // automation writes the Move parameter: the same Move, from the same beat, bit for bit as the tile's
        Host h;
        h.play(true);
        const Played p = playActs(h, in, inR, {{kPressBlock, [&](Host& x) { x.set(P_MOVE, static_cast<float>(stutter)); }}});
        CHECK(p.l == viaTile.l && p.r == viaTile.r);
        CHECK(h.value(P_MOVE) == 10.0f && h.display(P_MOVE) == "Stutter 1/16");   // it stays selected when the Move is over
        checkStutter(p, in, inR, kLine, "parameter");
    }
    {   // the same write twice is one change
        Host h;
        h.play(true);
        const Played p = playActs(h, in, inR,
                                  {{kPressBlock, [&](Host& x) { x.set(P_MOVE, static_cast<float>(stutter)); }},
                                   {kPressBlock + 100, [&](Host& x) { x.set(P_MOVE, static_cast<float>(stutter)); }}});
        CHECK(p.l == viaTile.l);
    }
    {   // the quantize is the page's: a bar from beat 1.3 is beat 4, a sixteenth is the next quarter of a beat (1.5)
        for (int q = 0; q < 3; ++q) {
            static const char* names[] = {"1/16", "Beat", "Bar"};
            static const int lines[] = {kBeat + kBeat / 2, 2 * kBeat, 4 * kBeat};
            Host h;
            h.play(true);
            h.option(P_MOVE_QUANT, names[q]);
            const Played p = playActs(h, in, inR, {{kPressBlock, [&](Host& x) { x.set(P_MOVE, static_cast<float>(stutter)); }}});
            checkStutter(p, in, inR, lines[q], names[q]);
        }
    }
    {   // a Loop goes round: the second lap is the same saw, and None stops it, mid-lap, for good
        Host h;
        h.play(true);
        h.option(P_MOVE_MODE, "Loop");
        const int stopBlock = 1300;   // sample 166400, a lap and a half in
        const Played p = playActs(h, in, inR,
                                  {{kPressBlock, [&](Host& x) { x.set(P_MOVE, static_cast<float>(stutter)); }},
                                   {stopBlock, [](Host& x) { x.option(P_MOVE, "None"); }}});
        bool lapTwo = true;
        for (int k = 1; k <= 5; ++k) {   // the second lap began at beat 6 (sample 132300); the stop is a bar and a half on
            const size_t at = repeatAt(kLine + 4 * kBeat, k, 1500);
            lapTwo = lapTwo && std::fabs(p.l[at] - in[static_cast<size_t>(kLine + 4 * kBeat + 1500)]) < 5e-5f;
        }
        CHECK(lapTwo);
        CHECK(same(p.l, in, static_cast<size_t>(stopBlock) * kBlock + 4000, in.size()));   // stopped: the input again
        CHECK(h.value(P_MOVE) == 0.0f && h.display(P_MOVE) == "None");
    }
    {   // None while it only waits: it never starts
        Host h;
        h.play(true);
        h.option(P_MOVE_QUANT, "Bar");
        const Played p = playActs(h, in, inR,
                                  {{kPressBlock, [&](Host& x) { x.set(P_MOVE, static_cast<float>(stutter)); }},
                                   {kPressBlock + 400, [](Host& x) { x.option(P_MOVE, "None"); }}});   // sample 79872, before the line at 88200
        CHECK(p.l == in && p.r == inR);
        bool dark = true;
        for (int k = 0; k < 16; ++k) dark = dark && h.get(tileOf(k)) == 0.0f;
        CHECK(dark);
    }
    {   // GO: the selected Move again, from the next beat; it springs back to 0. With None selected it does nothing.
        Host h;
        h.play(true);
        const int go = 1100;   // sample 140800, beat 6.39: the next beat is 7
        float goRead = -1.0f;
        const Played p = playActs(h, in, inR,
                                  {{kPressBlock, [&](Host& x) { x.set(P_MOVE, static_cast<float>(stutter)); }},
                                   {go, [](Host& x) { x.press(P_MOVE_GO); }},
                                   {go + 3, [&](Host& x) { goRead = x.get(P_MOVE_GO); }}});
        CHECK(goRead == 0.0f);
        checkStutter(Played{Buf(p.l.begin(), p.l.begin() + 140800), Buf(p.r.begin(), p.r.begin() + 140800)}, Buf(in.begin(), in.begin() + 140800),
                     Buf(inR.begin(), inR.begin() + 140800), kLine, "first");
        const int line2 = 7 * kBeat;
        const size_t first = firstDiff(p.l, in, 140800);
        CHECK(first >= static_cast<size_t>(line2 + kSixteenth - kPreRoll - 20) && first <= static_cast<size_t>(line2 + kSixteenth - kPreRoll + 20));
        bool repeats = true;
        for (int k = 1; k <= 5; ++k) repeats = repeats && std::fabs(p.l[repeatAt(line2, k, 1500)] - in[static_cast<size_t>(line2 + 1500)]) < 5e-5f;
        CHECK(repeats);
        CHECK(h.value(P_MOVE) == 10.0f);

        Host n;
        n.play(true);
        const Played idle = playActs(n, in, inR, {{kPressBlock, [](Host& x) { x.press(P_MOVE_GO); }}});
        CHECK(idle.l == in && idle.r == inR && n.value(P_MOVE) == 0.0f);
    }
    {   // GO takes the mode and the quantize of the moment it is pressed
        Host h;
        h.play(true);
        h.option(P_MOVE, "Stutter 1/16");   // selected, fired: let it play out
        const Played p = playActs(h, in, inR,
                                  {{1100, [](Host& x) { x.option(P_MOVE_QUANT, "Bar"); }}, {1101, [](Host& x) { x.press(P_MOVE_GO); }}});   // beat 6.4: the bar is 8
        const size_t first = firstDiff(p.l, in, 142000);
        CHECK(first >= static_cast<size_t>(8 * kBeat + kSixteenth - kPreRoll - 20) && first <= static_cast<size_t>(8 * kBeat + kSixteenth - kPreRoll + 20));
    }
}

// Review Focus 2: a Move over a loop wrap or a locate. It counts beats played, not song positions: it keeps its place in
// its length, so the wrap must not show in the sound at all. Stutter 1/8 from beat 1 for four beats, on a sine, played
// three ways: straight through, with MPC's loop jumping back 1.7 beats in the middle of it, and with a locate 8.3 beats
// ahead. The three are the same sound.
void moveAcrossLoop() {
    std::printf("== a Move over a loop wrap and a locate: it keeps its place, rejoins now, no click\n");
    const int total = 150000;
    Buf in(static_cast<size_t>(total));
    // (219 Hz: a slice of 220 Hz would be a whole number of its cycles, and repeating it would change nothing.)
    for (int i = 0; i < total; ++i) in[static_cast<size_t>(i)] = 0.4f * static_cast<float>(std::sin(2.0 * kPi * 219.0 * i / 44100.0));
    const int tap = 100, line = kBeat;   // beat 0.58 at the tap: the line is beat 1 (sample 22050)
    const int slice = kBeat / 2;         // Stutter 1/8's slice
    const int end = line + 4 * kBeat;    // four beats played
    const int stutter = tileOf(moveOf("Stutter 1/8"));

    auto play = [&](std::function<void(Host&)> jump, int at) {
        Host h;
        h.play(true);
        std::vector<Act> acts = {{tap, [&](Host& x) { x.tap(stutter); }}};
        if (jump) acts.push_back({at, jump});
        return playActs(h, in, acts);
    };
    const Played straight = play(nullptr, 0);
    // The Move is there: a saw that is not the input, from a slice after its line, until its end.
    const size_t first = firstDiff(straight.l, in, 20000);
    CHECK(first >= static_cast<size_t>(line + slice - kPreRoll - 20) && first <= static_cast<size_t>(line + slice - kPreRoll + 20));
    CHECK(maxDiff(straight.l, in, static_cast<size_t>(line + slice), static_cast<size_t>(end)) > 0.2);
    CHECK(same(straight.l, in, static_cast<size_t>(end + 3000), in.size()));
    const float baseStep = maxStep(straight.l, 20000, static_cast<size_t>(end + 3000));

    // The loop goes back 1.7 beats (block 300 is at beat 1.74): the Move goes on where it was in its length.
    const Played wrapped = play([](Host& x) { x.log.time.ppqPos = 0.04; }, 300);
    CHECK(maxDiff(wrapped.l, straight.l, 0, in.size()) < 2e-5);
    CHECK(same(wrapped.l, in, static_cast<size_t>(end + 3000), in.size()));   // and ends after four beats played, not at beat 5
    CHECK(maxStep(wrapped.l, 20000, static_cast<size_t>(end + 3000)) <= baseStep * 1.25f);
    // A locate far ahead, in the second half of it.
    const Played located = play([](Host& x) { x.log.time.ppqPos = 40.0; }, 600);
    CHECK(maxDiff(located.l, straight.l, 0, in.size()) < 2e-5);
    CHECK(same(located.l, in, static_cast<size_t>(end + 3000), in.size()));
    // A loop that wraps over and over, a quarter of the Move apart: the same.
    {
        Host h;
        h.play(true);
        std::vector<Act> acts = {{tap, [&](Host& x) { x.tap(stutter); }}};
        for (int b = 300; b < 1200; b += 100) acts.push_back({b, [](Host& x) { x.log.time.ppqPos = std::fmod(x.log.time.ppqPos, 1.0); }});
        const Played loops = playActs(h, in, acts);
        CHECK(maxDiff(loops.l, straight.l, 0, in.size()) < 2e-5);
    }

    // A Move that only waits for its line when the position jumps is looked for again from where the position is: a
    // locate at block 150 (beat 0.87, before the line at beat 1) to beat 10.3 puts the line at beat 11.
    {
        Host h;
        h.play(true);
        const Played p = playActs(h, in, {{tap, [&](Host& x) { x.tap(stutter); }}, {150, [](Host& x) { x.log.time.ppqPos = 10.3; }}});
        const int jumpAt = 150 * kBlock;
        const int line2 = jumpAt + static_cast<int>(std::lround(0.7 * kBeat));   // beat 11 is 0.7 of a beat on
        const size_t at = firstDiff(p.l, in, 1000);
        CHECK(at >= static_cast<size_t>(line2 + slice - kPreRoll - 20) && at <= static_cast<size_t>(line2 + slice - kPreRoll + 20));
        CHECK(same(p.l, in, static_cast<size_t>(line2 + 4 * kBeat + 3000), in.size()));
        CHECK(maxStep(p.l, 1000, in.size()) <= baseStep * 1.25f);
    }
    // A tempo change in the middle of it (120 to 150 BPM at block 300): it goes on where it was in its length, so it ends four
    // beats of play after its line, the rest of them at the new tempo; and nothing clicks.
    {
        Host h;
        h.play(true);
        const Played p = playActs(h, in, {{tap, [&](Host& x) { x.tap(stutter); }}, {300, [](Host& x) { x.log.time.tempo = 150.0; }}});
        const double played = (300.0 * kBlock - line) / kBeat;   // the beats it had played at the change
        const int ends = 300 * kBlock + static_cast<int>(std::lround((4.0 - played) * 44100.0 * 60.0 / 150.0));
        CHECK(allFinite(p.l) && maxDiff(p.l, in, static_cast<size_t>(ends - 6000), static_cast<size_t>(ends - 3000)) > 0.1);   // still going shortly before
        CHECK(same(p.l, in, static_cast<size_t>(ends + 3000), in.size()));
        CHECK(maxStep(p.l, 20000, static_cast<size_t>(ends + 3000)) <= baseStep * 1.25f);
    }
    // The transport stopping and starting under a Move: it plays on at the tempo while stopped, and keeps its length.
    {
        Host h;
        h.play(true);
        const Played p = playActs(h, in,
                                  {{tap, [&](Host& x) { x.tap(stutter); }},
                                   {300, [](Host& x) { x.play(false); }},     // stopped at beat 1.74 ...
                                   {500, [](Host& x) { x.play(true); x.log.time.ppqPos = 3.0; }}});   // ... started again from beat 3
        CHECK(maxDiff(p.l, straight.l, 0, in.size()) < 2e-5);
    }
}

// Stopped, the grid runs on at the tempo, and the Moves are on that clock, and so is Time: MPC holds the position at 3.3
// beats, and the first block is there. The tap at block 224 is 1.3003 beats on, beat 4.6003, so the next beat is 5, which is
// 1.7 beats from the start (37485 samples) -- the same line Time plays the pattern from. With no position at all
// (a host that doesn't say) the clock starts at 0, and the line is beat 2.
void moveWhileStopped() {
    std::printf("== a Move while MPC is stopped: on the clock that runs on, with Time\n");
    const int total = 140000;
    const Buf in = ramp(total, 5e-6f), inR = ramp(total, 2.5e-6f);
    {
        Host h;
        h.log.time.ppqPos = 3.3;
        const Played p = playActs(h, in, inR, {{kPressBlock, [](Host& x) { x.tap(tileOf(9)); }}});
        checkStutter(p, in, inR, static_cast<int>(std::lround(1.7 * kBeat)), "held position");
    }
    {
        Host h;
        h.log.time.flags &= ~vst::kVstPpqPosValid;
        h.log.time.ppqPos = 3.3;
        const Played p = playActs(h, in, inR, {{kPressBlock, [](Host& x) { x.tap(tileOf(9)); }}});
        checkStutter(p, in, inR, kLine, "no position");
    }
    {   // Time woke while MPC was stopped, long after the clock began: the Move is on the clock all the same
        Host h;
        h.log.time.ppqPos = 3.3;
        h.on(P_TIME_ON);
        const Played p = playActs(h, in, inR, {{kPressBlock - 100, [](Host& x) { x.option(P_TIME_ON, "Off"); }},
                                               {kPressBlock - 50, [](Host& x) { x.option(P_TIME_ON, "On"); }},
                                               {kPressBlock, [](Host& x) { x.tap(tileOf(9)); }}});
        checkStutter(p, in, inR, static_cast<int>(std::lround(1.7 * kBeat)), "Time woke late");
    }
}

// R42: a Move that plays the past (Reverse Beat plays the beat before it, backwards) finds it, though Time was never switched
// on: picking the Move is enough, the plugin makes Time's ring when it is picked and the ring listens from then on. The Move
// is picked at beat 1.3 to start on the bar (beat 4), so the beat before it (3 to 4) has been heard; on a ramp, every sample
// says where it was, and the Move reads it backwards: out(line + k) = in(line - k).
void moveReadsThePast() {
    std::printf("== a Move that plays the past: the ring listens from the moment the Move is picked\n");
    const int total = 150000, line = 4 * kBeat;
    const Buf in = ramp(total, 5e-6f), inR = ramp(total, 2.5e-6f);
    const int reverse = optionOf("Reverse Beat");
    CHECK(reverse == 7);
    for (int viaTile = 0; viaTile < 2; ++viaTile) {
        Host h;
        h.play(true);
        h.option(P_MOVE_QUANT, "Bar");
        const Played p = playActs(h, in, inR, {{kPressBlock, [&](Host& x) {
                                                    if (viaTile) x.tap(tileOf(reverse - 1));
                                                    else x.set(P_MOVE, static_cast<float>(reverse));
                                                }}});
        bool backwards = true;
        for (int k : {600, 3000, 8000, 15000, 20000})
            backwards = backwards && std::fabs(p.l[static_cast<size_t>(line + k)] - in[static_cast<size_t>(line - k)]) < 2e-5f &&
                        std::fabs(p.r[static_cast<size_t>(line + k)] - inR[static_cast<size_t>(line - k)]) < 2e-5f;
        CHECK(backwards);
        CHECK(same(p.l, in, 0, static_cast<size_t>(line - 400)));                          // live until the Move's crossfade
        CHECK(same(p.l, in, static_cast<size_t>(line + kBeat + 3000), in.size()));          // and live again after its beat
        CHECK(maxStep(p.l, static_cast<size_t>(line) - 1000, static_cast<size_t>(line + kBeat + 3000)) < 0.003f);
    }
}

// Review Focus 3: the first time the Echo or Time is switched on while it plays. The plugin allocates their memory when
// the switch is made (UI thread) and the audio thread picks it up; nothing clicks and nothing comes out of memory that
// was never written. Through the host, and then through the engine with the memory arriving a little after the switch.
void allocateWhilePlaying() {
    std::printf("== the Echo and Time switched on while it plays: memory handed over, no click\n");
    const int total = 90000;
    Buf in(static_cast<size_t>(total), 0.0f);
    addBurst(in, 20000, 8820, 0.4f);
    addBurst(in, 60000, 8820, 0.4f);
    const float inStep = maxStep(in);
    {   // the Echo: a repeat at 3/16 (1/8.) after the switch, and not a sample of anything before there is something to repeat
        Host h;
        h.play(true);
        const Played p = playActs(h, in, {{100, [](Host& x) {
                                               x.on(P_ECHO_ON);
                                               x.on(P_A_ON);
                                               x.set(P_A_FB, 0.4f);
                                           }}});
        CHECK(h.finite);
        CHECK(maxDiff(p.l, in, 0, static_cast<size_t>(20000 + 16537 - 60)) < 1e-9);   // the dry, alone, until the first repeat
        double energy = 0.0, ref = 0.0;
        for (int i = 20000 + 16537 + 1000; i < 20000 + 16537 + 7800; ++i) {
            const double w = p.l[static_cast<size_t>(i)] - in[static_cast<size_t>(i)];
            energy += w * w;
            ref += static_cast<double>(in[static_cast<size_t>(i - 16537)]) * in[static_cast<size_t>(i - 16537)];
        }
        CHECK(energy > 0.4 * 0.4 * ref * 0.9 && energy < 0.8 * 0.8 * ref * 1.1);   // the line's level is 0.8
        CHECK(maxStep(p.l) <= inStep * 4.0f);
    }
    {   // Time: a Push of 20 ms; from rest it passes the input until it has heard its lag, then crossfades
        Host h;
        h.play(true);
        const Played p = playActs(h, in, {{100, [](Host& x) {
                                               x.on(P_TIME_ON);
                                               x.option(P_TIME_GROOVE, "Push");
                                               x.set(P_TIME_AMOUNT, 1.0f);
                                           }}});
        CHECK(h.finite && maxStep(p.l) <= inStep * 2.5f);
        // the second burst is 20 ms late (882 samples): the sine's phase, or its envelope, has moved
        CHECK(std::fabs(p.l[60000 + 4410 + 882] - in[60000 + 4410]) < 0.02f);
    }
    {   // a tile that needs the memory (the Throw: the Echo) is its own switch: the memory is there at the tap
        Host h;
        h.play(true);
        lineAOnly(h, "1/8", 0.3f, 0.0f);
        const Played p = playActs(h, in, {{kPressBlock - 100, [](Host& x) { x.tap(tileOf(0)); }}});
        CHECK(h.finite && maxStep(p.l) <= inStep * 4.0f);
    }

    // Through the engine alone, the memory arriving after the switch: the Echo has nothing until it is there, then fades in.
    {
        float n[P_COUNT];
        for (int i = 0; i < P_COUNT; ++i) n[i] = PARAM_INFO[i].def;
        n[P_ECHO_ON] = 1.0f;
        n[P_A_ON] = 1.0f;
        CHECK(kEchoDivs[7].beats == 0.5);
        n[P_A_DIV] = paramNorm(P_A_DIV, 7.0f);   // 1/8: 11025 samples
        n[P_TIME_ON] = 1.0f;
        n[P_TIME_GROOVE] = paramNorm(P_TIME_GROOVE, static_cast<float>(Time::kPush));
        n[P_TIME_AMOUNT] = 1.0f;
        Engine e;
        e.setParams(n);
        ef::Transport t;
        t.playing = t.valid = true;
        Buf l = in, r = in;
        const int late = 40000;   // the memory arrives here, between two blocks
        for (int pos = 0; pos < total; pos += kBlock) {
            if (pos == late - late % kBlock) {
                CHECK(e.allocateEcho());
                CHECK(e.allocateTime());
            }
            t.beats = pos / 22050.0 * 1.0;
            const int len = std::min(kBlock, total - pos);
            e.render(&l[static_cast<size_t>(pos)], &r[static_cast<size_t>(pos)], len, t);
        }
        CHECK(allFinite(l) && allFinite(r));
        // Nothing of the Echo or Time before the memory: the first burst (20000) goes through untouched, silent after it.
        CHECK(same(l, in, 0, static_cast<size_t>(late - late % kBlock)));
        CHECK(l == r);
        // After it the second burst (60000) has its repeat, 1/8 on (11025), and the click bound holds all the way.
        double energy = 0.0;
        for (int i = 60000 + 11025 + 500; i < 60000 + 11025 + 8000; ++i) energy += static_cast<double>(l[static_cast<size_t>(i)] - in[static_cast<size_t>(i)]) * (l[static_cast<size_t>(i)] - in[static_cast<size_t>(i)]);
        CHECK(energy > 1.0);
        CHECK(maxStep(l, static_cast<size_t>(late) - 500) <= inStep * 3.0f);
    }
}


// Polyrhythm: the Echo through MPC's tempo. Line A at 3/16 (1/8.) and line C at 5/16, nothing coming back, the lines
// panned apart so each is heard on its own side: the click comes back at 0.75 and at 1.25 beats, to the sample, at 120 BPM
// and at 150, and not at all with the Echo off.
void echoThroughMpc() {
    std::printf("== the Echo through MPC: repeats at 3/16 and 5/16\n");
    const int total = 4 * kBeat;
    const int click = 5000;
    auto repeats = [&](double bpm, bool echoOn) {
        Host h;
        h.log.time.tempo = bpm;
        h.play(true);
        h.set(P_A_FB, 0.0f);
        h.set(P_C_FB, 0.0f);
        h.option(P_A_DIV, "1/8.");
        h.option(P_C_DIV, "5/16");
        h.on(P_A_ON);
        h.on(P_C_ON);
        h.set(P_A_PAN, -1.0f);   // A on the left only, C on the right only
        h.set(P_C_PAN, 1.0f);
        if (echoOn) h.on(P_ECHO_ON);
        const Buf in = impulseAt(total, click, 0.5f);
        h.run(in, in);
        return std::make_pair(h.L, h.R);
    };
    // A sixteenth is 3/16 of a bar = 0.75 beats; 5/16 of a bar = 1.25 beats. Where the repeat's peak is: the largest sample.
    auto peakAt = [](const Buf& x, int from, int to) {
        int at = from;
        for (int i = from; i < to; ++i)
            if (std::fabs(x[static_cast<size_t>(i)]) > std::fabs(x[static_cast<size_t>(at)])) at = i;
        return at;
    };
    for (double bpm : {120.0, 150.0}) {
        const double samplesPerBeat = 44100.0 * 60.0 / bpm;
        const int a = click + static_cast<int>(std::lround(0.75 * samplesPerBeat)), c = click + static_cast<int>(std::lround(1.25 * samplesPerBeat));
        const auto got = repeats(bpm, true);
        const int pa = peakAt(got.first, click + 100, static_cast<int>(a + 2000)), pc = peakAt(got.second, click + 100, total);
        CHECK(std::abs(pa - a) <= 1 && std::abs(pc - c) <= 1);
        CHECK(std::fabs(got.first[static_cast<size_t>(pa)]) > 0.2f && std::fabs(got.second[static_cast<size_t>(pc)]) > 0.2f);   // 0.5 x the line's 0.8, about
        // each line is on its own side only, and nothing else repeats: the left has A's repeat and no C's, the right the other
        CHECK(std::fabs(got.first[static_cast<size_t>(c)]) < 0.01f && std::fabs(got.second[static_cast<size_t>(a)]) < 0.01f);
        CHECK(std::fabs(got.first[static_cast<size_t>(click)] - 0.5f) < 1e-6f);   // the dry
    }
    const auto off = repeats(120.0, false);   // switched off: the input, nothing after it
    CHECK(peak(off.first, click + 100, static_cast<size_t>(total)) == 0.0f && peak(off.second, click + 100, static_cast<size_t>(total)) == 0.0f);
}

// Review Focus 4: two instances on one clock each. One pumps its echo on the Input trigger (Rumble: a repeat that lands on
// a kick is ducked, one between kicks is not), the other pumps with the Grid at another tempo from another place. Run side
// by side a block at a time, each is what it is alone, to the bit: the key is each one's own. Both keep to MPC's beats.
void twoInstancesRumble() {
    std::printf("== two instances, one pumping its echo on the Input trigger: neither hears the other\n");
    const int lead = 4410, beats = 8, total = lead + beats * kBeat;
    Buf kicks(static_cast<size_t>(total), 0.0f);
    for (int k = 0; k < beats; ++k) addKick(kicks, lead + k * kBeat, 0.8f);
    const Buf pad = [&] {
        Buf x = sine(330.0, total, 0.25f);
        for (size_t i = 0; i < x.size(); ++i) x[i] += kicks[i];
        return x;
    }();
    const Buf dc(static_cast<size_t>(total), 0.5f);

    auto rumble = [](Host& h) {   // a repeat half a beat after each kick, and one on the next
        h.on(P_ECHO_ON);
        h.on(P_A_ON);
        h.option(P_A_DIV, "1/8");
        h.set(P_A_FB, 0.5f);
        h.play(true);
    };
    auto pumped = [&](Host& h) {
        rumble(h);
        h.on(P_PUMP_ON);
        h.option(P_PUMP_TARGET, "Echo");
        h.option(P_PUMP_TRIGGER, "Input");
        h.option(P_PUMP_CURVE, "Hard");
        h.set(P_PUMP_DEPTH, 1.0f);
        h.set(P_PUMP_LENGTH, 0.3f);
    };
    auto grid = [&](Host& h) {   // 150 BPM, from beat 0.37: the first beat is 0.63 of a beat on (11113 samples)
        pumpOn(h, 1.0f);
        h.log.time.tempo = 150.0;
        h.log.time.ppqPos = 0.37;
    };
    const Buf plain = [&] {
        Host h;
        rumble(h);
        h.run(pad, pad);
        return h.L;
    }();
    const Buf soloA = [&] {
        Host h;
        pumped(h);
        h.run(pad, pad);
        return h.L;
    }();
    const Buf soloB = [&] {
        Host h;
        grid(h);
        h.run(dc, dc);
        return h.L;
    }();
    // Side by side, a block each in turn.
    Host a, b;
    pumped(a);
    grid(b);
    Buf bothA, bothB;
    for (int pos = 0; pos < total; pos += kBlock) {
        const size_t from = static_cast<size_t>(pos), n = std::min<size_t>(kBlock, static_cast<size_t>(total - pos));
        a.run(Buf(pad.begin() + static_cast<long>(from), pad.begin() + static_cast<long>(from + n)));
        b.run(Buf(dc.begin() + static_cast<long>(from), dc.begin() + static_cast<long>(from + n)));
        bothA.insert(bothA.end(), a.L.begin(), a.L.end());
        bothB.insert(bothB.end(), b.L.begin(), b.L.end());
    }
    CHECK(bothA == soloA);
    CHECK(bothB == soloB);

    // The Rumble: the wet (the output less the input) is ducked at the kicks and not between them.
    auto wetShare = [&](const Buf& got, const Buf& ref, int at) {   // the ducked wet's rms over the plain wet's, 6..30 ms in
        double x = 0.0, y = 0.0;
        for (int i = at + 6 * 44; i < at + 30 * 44; ++i) {
            const double g = got[static_cast<size_t>(i)] - pad[static_cast<size_t>(i)], r = ref[static_cast<size_t>(i)] - pad[static_cast<size_t>(i)];
            x += g * g;
            y += r * r;
        }
        return y > 1e-9 ? std::sqrt(x / y) : -1.0;
    };
    int tested = 0;
    for (int k = 2; k < beats - 1; ++k) {
        const int kick = lead + k * kBeat, repeat = kick + kBeat / 2;
        const double onKick = wetShare(bothA, plain, kick), onRepeat = wetShare(bothA, plain, repeat);
        CHECK(onKick >= 0.0 && onKick < 0.4);
        CHECK(onRepeat > 0.98 && onRepeat < 1.02);
        ++tested;
    }
    CHECK(tested == beats - 3);
    // The other one is on its own beats: 17640 samples apart at 150 BPM, the first at 11113.
    const std::vector<int> at = ducks(bothB, kDuck);
    CHECK(at.size() == 10);   // (4.1 s of audio: 11113, 28753, ... 169873)
    for (size_t k = 0; k < at.size(); ++k) CHECK(std::abs(at[k] - static_cast<int>(std::lround(0.63 * 17640.0 + static_cast<double>(k) * 17640.0))) <= 3);
    // The other way round: B first in each pair of blocks. The instances don't share an order either.
    Host a2, b2;
    pumped(a2);
    grid(b2);
    Buf bothA2;
    for (int pos = 0; pos < total; pos += kBlock) {
        const size_t from = static_cast<size_t>(pos), n = std::min<size_t>(kBlock, static_cast<size_t>(total - pos));
        b2.run(Buf(dc.begin() + static_cast<long>(from), dc.begin() + static_cast<long>(from + n)));
        a2.run(Buf(pad.begin() + static_cast<long>(from), pad.begin() + static_cast<long>(from + n)));
        bothA2.insert(bothA2.end(), a2.L.begin(), a2.L.end());
    }
    CHECK(bothA2 == soloA);
}

// Review Focus 5: a preset or a project loaded while a Move plays ends the Move. The knobs it ramped come back to the new
// sound's values over its 10 ms glide, nothing is left of its Echo, and no lap of a Loop starts again. Break takes the dry
// down for its four beats (a steady 0.5 goes to nothing); loading Init in the middle of it brings the 0.5 back.
void presetDuringMove() {
    std::printf("== a preset or project loaded during a Move: the Move ends, its ramps return\n");
    const int total = 300000;
    const Buf in(static_cast<size_t>(total), 0.5f);
    const int tap = 100, line = kBeat;   // Break from beat 1: the dry is down from sample 22050
    const int loadBlock = 330;           // sample 42240: a beat's worth into it
    const std::string initChunk = Host().chunk();
    struct Load {
        const char* what;
        std::function<void(Host&)> fn;
    };
    const Load loads[] = {{"Init button", [](Host& x) { x.press(P_PRE_INIT); }},
                          {"project", [&](Host& x) { x.load(initChunk); }},
                          {"browser arrow", [](Host& x) { x.press(P_PRESET_NEXT); x.press(P_PRESET_PREV); x.press(P_PRE_INIT); }}};
    for (const Load& load : loads) {
        Host h;
        h.play(true);
        h.option(P_MOVE_MODE, "Loop");
        const Played p = playActs(h, in, {{tap, [](Host& x) { x.tap(tileOf(moveOf("Break"))); }}, {loadBlock, load.fn}});
        // Down for the Move (a beat after the line, so the glide is over), then back.
        CHECK(peak(p.l, static_cast<size_t>(line + 2000), static_cast<size_t>(loadBlock) * kBlock) < 1e-3f);
        const size_t from = static_cast<size_t>(loadBlock) * kBlock;
        CHECK(maxStep(p.l, from - 600, from + 4000) < kSlope);    // the dry comes back in a glide: never faster than 1 ms a full scale
        CHECK(p.l[from + 3000] > 0.49f);
        // ... and stays back, to the bit, through what would have been the rest of the lap and two more laps.
        CHECK(same(p.l, in, from + 3000, in.size()) && same(p.r, in, from + 3000, in.size()));
        CHECK(h.value(P_MOVE) == 0.0f && h.display(P_MOVE) == "None");
        bool dark = true;
        for (int k = 0; k < 16; ++k) dark = dark && h.get(tileOf(k)) == 0.0f;
        CHECK(dark);
        if (!(same(p.l, in, from + 3000, in.size()))) std::printf("  (%s)\n", load.what);
    }
    // A Throw rings its echo on after the Move (the Echo is kept on for its tail); a load drops that too: the new sound is
    // what is heard, the old Echo is not. The impulse comes in during the Move, its repeats would come after. The sound
    // loaded has the line on and the Echo off (only the Echo kept on by the Move could ring it), as Init has neither.
    for (int keepsLine = 0; keepsLine < 2; ++keepsLine) {
        std::string loaded = Host().chunk();   // Init
        if (keepsLine) {
            Host line;
            lineAOnly(line, "1/8", 0.5f, 0.0f);
            loaded = line.chunk();
        }
        Host h;
        h.play(true);
        lineAOnly(h, "1/8", 0.5f, 0.0f);
        Buf imp(static_cast<size_t>(total), 0.0f);
        imp[static_cast<size_t>(kLine + 15435)] = 0.5f;
        const Played p = playActs(h, imp, {{kPressBlock, [](Host& x) { x.tap(tileOf(0)); }},
                                           {static_cast<int>((kLine + 15435 + 12000) / kBlock), [&](Host& x) { x.load(loaded); }}});
        const size_t cut = static_cast<size_t>(kLine + 15435 + 12000) + 3000;
        CHECK(peak(p.l, static_cast<size_t>(kLine + 15435 + 11025 - 3), static_cast<size_t>(kLine + 15435 + 11025 + 4)) > 0.3f);   // the first repeat was heard
        CHECK(peak(p.l, cut, imp.size()) < 1e-6f);                                                                              // and no tail after the load
    }
}

// R38: a Throw switches the Echo on for its beat and the repeats go on ringing after it, until the Echo's tail is out; then
// the Echo goes off, as a switch does. The click comes in 0.7 of a beat into the Throw and line A repeats every 1/8: the first
// repeat is after the Move ended (at 1.2 beats), the next at 1.7.
void echoHoldsAfterThrow() {
    std::printf("== a Throw: the Echo it switched on rings out its tail after the Move, then goes off\n");
    const int total = 12 * 44100;
    const int click = kLine + 15435;   // 0.7 of a beat after the line (the Throw is 1 beat)
    Buf in(static_cast<size_t>(total), 0.0f);
    in[static_cast<size_t>(click)] = 0.5f;
    Host h;
    h.play(true);
    lineAOnly(h, "1/8", 0.3f, 0.0f);   // the send is shut: only the Throw opens it
    float running = -1.0f, ended = -1.0f;
    const Played p = playActs(h, in, {{kPressBlock, [](Host& x) { x.tap(tileOf(0)); }},
                                      {static_cast<int>((kLine + kBeat + 4000) / kBlock), [&](Host& x) { running = static_cast<float>(partsOn(x)); }},
                                      {total / kBlock - 4, [&](Host& x) { ended = static_cast<float>(partsOn(x)); }}});
    CHECK(h.finite);
    // The click goes through the open send: its repeats are at 0.5 beat apart, at 0.4 (0.5 x the line's 0.8), 0.12, ...
    auto at = [&](int i) { return std::fabs(p.l[static_cast<size_t>(i)]); };
    auto near = [&](int i) { return std::max({at(i - 2), at(i - 1), at(i), at(i + 1), at(i + 2)}); };
    const int step = kBeat / 2;
    CHECK(near(click + step) > 0.3f);       // after the Move ended (it ended at kLine + kBeat = click + 6615), 4410 samples later
    CHECK(near(click + 2 * step) > 0.08f);
    CHECK(near(click + 3 * step) > 0.02f);
    CHECK(near(click + 2 * step) < near(click + step));   // falling
    // It rings out, then the Echo is off: silence (the input is silent) and the status says no part runs.
    CHECK(running == 1.0f && ended == 0.0f);
    float last = 0.0f;
    size_t lastAt = 0;
    for (size_t i = static_cast<size_t>(click) + 100; i < p.l.size(); ++i)
        if (p.l[i] != 0.0f && std::fabs(p.l[i]) > 1e-12f) last = std::fabs(p.l[i]), lastAt = i;
    std::printf("  the Echo's last sound: %.2e at %.2f s\n", static_cast<double>(last), static_cast<double>(lastAt) / 44100.0);
    CHECK(lastAt > static_cast<size_t>(click + 6 * step));   // it did ring on for a while
    CHECK(last < 2e-3f);                                      // and went off at a level nobody hears
    CHECK(peak(p.l, static_cast<size_t>(10 * 44100), p.l.size()) == 0.0f);
}


// The project: the Echo, the lines, Time, the order and a selected Move go in the chunk and come out as they went in, and
// the instance that loads it sounds as one that never heard of the Move: the Echo has its memory from the load, the Move is
// only selected (a load fires nothing), and GO fires it.
void stateWithEcho() {
    std::printf("== the project with the Echo and a Move selected\n");
    freshUserPresets();
    auto configure = [](Host& h, bool selectMove) {
        h.on(P_ECHO_ON);
        h.on(P_A_ON);
        h.on(P_C_ON);
        h.option(P_A_DIV, "1/8.");
        h.option(P_C_DIV, "5/16");
        h.set(P_A_FB, 0.5f);
        h.set(P_C_FB, 0.2f);
        h.set(P_C_PAN, 0.5f);
        h.option(P_C_FTYPE, "HP");
        h.set(P_C_CUT, 700.0f);
        h.option(P_ECHO_ROUTE, "Serial");
        h.option(P_ECHO_ORDER, "Echo Last");
        h.set(P_ECHO_TONE, 0.25f);
        h.on(P_TIME_ON);
        h.option(P_TIME_GROOVE, "Push");
        h.option(P_TIME_PLACE, "Pre");
        h.option(P_MOVE_MODE, "Loop");
        h.option(P_MOVE_QUANT, "Bar");
        h.set(P_DRY, 0.8f);
        if (selectMove) h.option(P_MOVE, "Stutter 1/16");
    };
    Host a;
    configure(a, true);
    const std::string chunk = a.chunk();
    for (const char* line : {"\necho_on=On\n", "\nmove=Stutter 1/16\n", "\nmove_mode=Loop\n", "\nmove_quant=Bar\n", "\na_div=1/8.\n",
                             "\nc_div=5/16\n", "\necho_route=Serial\n", "\necho_order=Echo Last\n", "\ntime_place=Pre\n", "\ntime_groove=Push\n",
                             "\nc_ftype=HP\n", "\na_on=On\n", "\nb_on=Off\n"})
        CHECK(has(chunk, line));
    CHECK(!has(chunk, "move_1=") && !has(chunk, "move_go=") && !has(chunk, "move_16="));   // the tiles and GO are the surface's
    Host b;
    CHECK(b.load(chunk) == 1);
    CHECK(sameSound(a, b));
    CHECK(b.chunk() == chunk);
    CHECK(b.value(P_MOVE) == 10.0f && b.display(P_MOVE) == "Stutter 1/16" && b.display(P_MOVE_MODE) == "Loop");

    // Sound: a loaded project that has a Move selected is the project without one, to the bit, for as long as nobody fires it.
    Host ref;
    configure(ref, false);
    const int total = 6 * kBeat;
    Buf in = whiteNoise(total, 0.0f);
    addBurst(in, 4000, 8820, 0.4f);
    addKick(in, 30000, 0.7f);
    addBurst(in, 70000, 8820, 0.4f);
    for (int i = 80000; i < total; ++i) in[static_cast<size_t>(i)] += 0.2f * static_cast<float>(std::sin(2.0 * kPi * 219.0 * i / 44100.0));   // something for the Move to repeat
    b.play(true);
    ref.play(true);
    b.run(in, in);
    ref.run(in, in);
    CHECK(b.L == ref.L && b.R == ref.R);
    CHECK(maxDiff(b.L, in, 0, in.size()) > 0.1);   // the Echo is working (it has its memory), and Time has a groove
    bool dark = true;
    for (int k = 0; k < 16; ++k) dark = dark && b.get(tileOf(k)) == 0.0f && b.log.automateCount[tileOf(k)] == 0;
    CHECK(dark);

    // GO fires the Move that was selected, at the bar. The Time pattern is Stutter 1/16, on the Pre place here.
    Host c;
    c.load(chunk);
    c.play(true);
    const Played go = playActs(c, in, {{kPressBlock, [](Host& x) { x.press(P_MOVE_GO); }}});
    CHECK(go.l != ref.L);
    CHECK(same(go.l, ref.L, 0, static_cast<size_t>(4 * kBeat - 1000)));   // the same until the bar line (beat 4, sample 88200) is near
    CHECK(maxDiff(go.l, ref.L, static_cast<size_t>(4 * kBeat + kSixteenth), in.size()) > 0.05);   // and the Stutter is heard after it
    CHECK(c.get(tileOf(9)) == 1.0f);                      // and it is a Loop: still going at the end of the six beats
    freshUserPresets();
}

// A short suspend (Stop) keeps a Move where it is; one over 250 ms (the slot's ON button) clears it with everything else:
// the Move is gone, its tile is dark, the input is the input.
void moveSuspend() {
    std::printf("== suspend and resume with a Move playing\n");
    const int total = 175000;
    const Buf in = ramp(total, 5e-6f), inR = ramp(total, 2.5e-6f);
    const int at = 500;   // sample 64000: the second slice of the Stutter
    Played base;
    {
        Host h;
        h.play(true);
        base = playActs(h, in, inR, {{kPressBlock, [](Host& x) { x.tap(tileOf(9)); }}});
    }
    {
        Host h;
        h.play(true);
        const Played p = playActs(h, in, inR,
                                  {{kPressBlock, [](Host& x) { x.tap(tileOf(9)); }},
                                   {at, [](Host& x) {
                                        Turn quick(100);
                                        x.op(vst::effMainsChanged, 0, 0);
                                        x.op(vst::effMainsChanged, 0, 1);
                                    }}});
        CHECK(p.l == base.l && p.r == base.r);   // a Stop is not a reset
    }
    {
        Host h;
        h.play(true);
        float lit = -1.0f;
        const Played p = playActs(h, in, inR,
                                  {{kPressBlock, [](Host& x) { x.tap(tileOf(9)); }},
                                   {at, [](Host& x) {
                                        Turn slow(300);
                                        x.op(vst::effMainsChanged, 0, 0);
                                        x.op(vst::effMainsChanged, 0, 1);
                                    }},
                                   {at + 4, [&](Host& x) { lit = x.get(tileOf(9)); }}});
        CHECK(same(p.l, in, static_cast<size_t>(at) * kBlock, in.size()) && same(p.r, inR, static_cast<size_t>(at) * kBlock, in.size()));
        CHECK(lit == 0.0f);
        CHECK(h.value(P_MOVE) == 10.0f);   // still the selected Move
    }
    {   // a tap made while the slot is off is not played when it comes back
        Host h;
        h.play(true);
        const Played p = playActs(h, in, inR, {{at, [](Host& x) {
                                                    x.op(vst::effMainsChanged, 0, 0);
                                                    x.tap(tileOf(9));
                                                    x.op(vst::effMainsChanged, 0, 1);   // (a second of the surface's clock on: over 250 ms)
                                                }}});
        CHECK(p.l == in && p.r == inR);
        CHECK(h.value(P_MOVE) == 10.0f && h.get(tileOf(9)) == 0.0f);
    }
}

// 60 s of random parameter writes, taps and detents, Move tiles and GO, transport jumps and bursts of NaN and infinity in
// loud input, with the chain (the four parts: Pump, Shape, the Echo and Time, their lines and lanes, the Moves that fire
// and stop and start again) running a good part of the time: every sample that leaves is a number, none above +18 dBFS,
// the parts really run, and Init afterwards is the input again.
void stressEverything() {
    std::printf("== 60 s of everything at once, at random\n");
    freshUserPresets();
    Host h;
    Rng rng(12345);
    const int total = 60 * kBlocksPerSec;
    float worst = 0.0f;
    int saves = 0, anyOn = 0, threeOn = 0, fourOn = 0, moving = 0, taps = 0;
    h.play(true);
    auto everything = [](Host& x) {   // all four parts and their insides on, as a producer would have them for a fill
        for (int id : {P_PUMP_ON, P_SHAPE_ON, P_ECHO_ON, P_TIME_ON, P_A_ON, P_B_ON, P_C_ON, P_D_ON, P_VOL_ON, P_FLT_ON, P_PAN_ON}) x.on(id);
        x.set(P_PUMP_DEPTH, 0.8f);
        x.set(P_VOL_DEPTH, 0.6f);
        x.set(P_ECHO_SEND, 0.7f);
    };
    everything(h);
    for (int blk = 0; blk < total; ++blk) {
        if (blk % kBlocksPerSec == kBlocksPerSec - 1) {   // once a second: how many parts the status line says run
            const int parts = partsOn(h);
            anyOn += parts >= 1;
            threeOn += parts >= 3;
            fourOn += parts == 4;
            bool lit = false;
            for (int k = 0; k < 16; ++k) lit = lit || h.get(tileOf(k)) > 0.5f;
            moving += lit;
        }
        if (blk % (4 * kBlocksPerSec) == 0) everything(h);   // (the writes below turn them off and on again at random in between)
        for (int k = rng.below(4); k > 0; --k) {   // parameters: any value, in range or not
            const int id = rng.below(P_COUNT);
            if (PARAM_INFO[id].kind == Kind::Synth) {
                static const float wild[] = {std::nanf(""), -1.0f, 2.0f, 1e30f, 0.0f, 1.0f};
                if (rng.next() < 0.1f) h.setN(id, wild[rng.below(6)]);
                else if (rng.next() < 0.3f) h.detent(id, rng.next() < 0.5f ? -1 : +1);
                else h.setN(id, rng.next());
            } else if (PARAM_INFO[id].kind != Kind::Readout && PARAM_INFO[id].kind != Kind::Popup) {
                if (rng.next() > 0.1f) continue;   // the browser is over half the parameters: a preset load now and then, not all the time
                if (id == P_PRE_SAVE && ++saves > 8) continue;   // a handful of files, not thousands
                h.press(id);   // the browser: tiles, arrows, RND, favourites, Init, the stepper; the Move tiles and GO
            }
        }
        if (rng.next() < 0.01f) {   // a Move tile, and now and then GO
            h.tap(tileOf(rng.below(16)));
            ++taps;
        }
        if (rng.next() < 0.004f) h.press(P_MOVE_GO);
        if (rng.next() < 0.05f) shake(h, rng);
        if (rng.next() < 0.002f) {   // a Stop and a resume, short or long
            Turn t(rng.next() < 0.5f ? 100 : 300);
            h.op(vst::effMainsChanged, 0, 0);
            h.op(vst::effMainsChanged, 0, 1);
        }
        Buf l = whiteNoise(kBlock, rng.next() * 2.0f, static_cast<uint32_t>(blk) * 2u + 1u);
        Buf r = whiteNoise(kBlock, rng.next() * 2.0f, static_cast<uint32_t>(blk) * 2u + 2u);
        if (rng.next() < 0.04f) {   // a burst of what a broken host or a broken upstream plugin sends
            static const float bad[] = {std::nanf(""), INFINITY, -INFINITY, 1e30f, -1e30f};
            const int from = rng.below(kBlock), len = 1 + rng.below(40);
            for (int i = from; i < std::min(kBlock, from + len); ++i) (rng.next() < 0.5f ? l : r)[static_cast<size_t>(i)] = bad[rng.below(5)];
        }
        h.run(l, r, rng.next() < 0.1f ? 1 + rng.below(300) : kBlock);
        worst = std::max(worst, std::max(peak(h.L), peak(h.R)));
    }
    CHECK(h.finite);
    std::printf("  highest peak over %d blocks: %.2f (%.1f dBFS)\n", total, static_cast<double>(worst), db(worst));
    CHECK(worst <= kCeiling);
    std::printf("  the status line said a part ran in %d of 60 seconds, three or more in %d, all four in %d; a Move tile lit in %d (%d taps)\n",
                anyOn, threeOn, fourOn, moving, taps);
    CHECK(anyOn >= 40 && threeOn >= 10 && fourOn >= 3);   // the chain really was running, not resting through it
    CHECK(moving >= 5);                                      // and Moves really played
    // Whatever was turned: Init is the input again, bit for bit, once the glides are over.
    h.play(false);
    h.log.noTime = false;
    h.log.time.tempo = 120.0;
    h.log.time.ppqPos = 0.0;
    h.log.time.flags = vst::kVstTempoValid | vst::kVstPpqPosValid;
    h.press(P_PRE_INIT);
    h.blocks(8);
    const Buf l = whiteNoise(kBlocksPerSec * kBlock, 0.5f, 21), r = whiteNoise(kBlocksPerSec * kBlock, 0.5f, 22);
    h.run(l, r);
    CHECK(h.L == l && h.R == r);
    freshUserPresets();
}

} // namespace
} // namespace eft

void pluginTests() {
    using namespace eft;
    const std::string root = fixtureDir();
    std::filesystem::create_directories(root + "/presets");
    std::filesystem::create_directories(root + "/data");
    setenv("GF_PRESET_ROOTS", (root + "/presets").c_str(), 1);
    setenv("GF_DATA_DIR", (root + "/data").c_str(), 1);
    setenv("GF_FIXED_SEED", "1", 1);   // every instance the same random numbers (the browser's RND)
    // Host events a second apart unless a test says otherwise (eft::Turn): stepping never
    // mistakes two of them for one turn, whatever the machine's speed (and qemu's).
    gf::Surface::clock = [] {
        static long long t = 0;
        return t += g_msPerEvent;
    };

    std::printf("== the plugin\n");
    // GF_TEST_ONLY=name,name runs just those (a development help; unset: all).
    const char* only = std::getenv("GF_TEST_ONLY");
    auto wants = [only](const char* name) { return !only || !*only || std::strstr(only, name) != nullptr; };
#define RUN(f)              \
    do {                    \
        if (wants(#f)) f(); \
    } while (0)
    RUN(basics);
    RUN(displays);
    RUN(mapping);
    RUN(mappingEcho);
    RUN(passThrough);
    RUN(levels);
    RUN(pumpThroughMpc);
    RUN(detents);
    RUN(presetsRoundTrip);
    RUN(freshInstance);
    RUN(factoryPresets);
    RUN(factoryLevels);
    RUN(projectState);
    RUN(displayRate);
    RUN(statusLine);
    RUN(suspendResume);
    RUN(suspendPumping);
    RUN(echoThroughMpc);
    RUN(tilesFireMoves);
    RUN(moveParameterFires);
    RUN(moveAcrossLoop);
    RUN(moveWhileStopped);
    RUN(moveReadsThePast);
    RUN(allocateWhilePlaying);
    RUN(twoInstancesRumble);
    RUN(presetDuringMove);
    RUN(echoHoldsAfterThrow);
    RUN(stateWithEcho);
    RUN(moveSuspend);
    RUN(stressEverything);
#undef RUN

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}
