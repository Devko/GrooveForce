// The whole plugin driven through its VST2 entry points the way MPC drives an insert effect (128-frame
// blocks in place, 0..1 params, the Force's taps and Q-Link detents, MPC's transport). The suites of the
// modules are test/*_test.cpp; pluginTests() is called by test/main.cpp after them. Built with ASan/UBSan
// by `make test`, for the Force's CPU under qemu by `make test-arm`.
//
// What is tested here is the glue and the chain behind it: the parameters, their texts and their map onto
// the chain (grooveParams), the presets and the project state, the redraws and the status line, the Pump on
// MPC's grid (pumpThroughMpc), suspend and resume with the Pump running, and that nothing hostile gets out
// of the whole thing at once (stress). The modules' own behaviour is in their suites.
#include "host.h"
#include "../plugin/presets.h"
#include "../plugin/surface.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

// "1 OF 2 ON": the parts the status line says are running (-1: it doesn't say).
int partsOn(Host& h) {
    const std::string s = h.display(P_STATUS);
    const size_t at = s.find(" OF 2 ON");
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

// The status line: what the plugin costs and how many of its two parts run, told to MPC (an UpdateDisplay) when it
// changes. The first report is half a second of audio away, and so is each next one.
void statusLine() {
    std::printf("== the status line: CPU, PEAK and the parts that run\n");
    Host h;
    h.blocks(180);
    const std::string s = h.display(P_STATUS);
    CHECK(starts(s, "CPU ") && has(s, "%   PEAK ") && has(s, "%   0 OF 2 ON"));
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
    // A part aimed at the Echo has nothing to work on until plan 2: it rests, and is not counted.
    h.option(P_SHAPE_TARGET, "Echo");
    CHECK(settle() && partsOn(h) == 1);
    h.option(P_SHAPE_TARGET, "All");
    CHECK(settle() && partsOn(h) == 2);
    h.option(P_PUMP_ON, "Off");   // not counted once its fade is over
    CHECK(settle() && partsOn(h) == 1);
    h.option(P_SHAPE_ON, "Off");
    CHECK(settle() && partsOn(h) == 0);
    CHECK(starts(h.display(P_STATUS), "CPU "));
}

void detents() {
    std::printf("== a Q-Link detent moves a list one option\n");
    Host h;
    const int lists[] = {P_PUMP_CURVE, P_VOL_SHAPE, P_FLT_SHAPE, P_PAN_SHAPE, P_PUMP_RATE, P_FLT_RATE, P_PUMP_BAND,
                         P_FLT_TYPE, P_SWING_UNIT, P_PUMP_TRIGGER, P_SHAPE_TARGET};
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
    CHECK(has(h.display(P_PRESET), "Init") || h.display(P_PRESET) == "PRESET  -");
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
    g.press(P_PRE_INIT);   // a new instance's stepper stands at the first item, which is no longer Init
    g.press(P_PRESET_NEXT);   // Init -> User 001 -> User 002 -> User 003
    g.press(P_PRESET_NEXT);
    g.press(P_PRESET_NEXT);
    CHECK(has(g.display(P_PRESET), "User 003"));
    CHECK(g.display(P_SWING) == "70%" && g.display(P_PUMP_RATE) == "1 bar");
    CHECK(g.display(P_PUMP_CURVE) == "Smooth" && g.display(P_VOL_DEPTH) == "0%");   // what it didn't name is the default
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

// Every factory preset, loaded by the browser's arrows on a fresh instance: it is what its file says, and on the beat at
// 120 BPM with the transport running it stays a number under +18 dBFS and changes the sound (the rms of the difference
// to the input above -40 dB re the input). Init is the one that doesn't: the input, bit for bit.
void factoryPresets() {
    std::printf("== the factory presets\n");
    freshUserPresets();
    // The first set, by category folder (presets/Factory/NN_Category): later ones are checked by the loop below too.
    static const struct { const char* category; const char* name; } kFirst[] = {
        {"Bass", "Bass Pump"},          {"Bass", "Hard Pump"},         {"Bass", "Double Pump"},      {"Bass", "Offbeat Gate"},
        {"Hats + Perc", "Hat Accents"}, {"Hats + Perc", "Hat Shuffle"}, {"Chords", "Chord Gate Pump"}, {"Chords", "Filter Pump"},
        {"Drum Bus", "Low Pump"},       {"Utility", "Init"}};
    std::vector<int> factory;   // the listing's built-ins, in the order the arrows walk them
    int items = 0;
    {
        presetLibrary().rescan();   // the folders as they are now (no user presets)
        const auto lst = presetLibrary().listing();
        items = static_cast<int>(lst->items.size());
        for (int i = 0; i < items; ++i)
            if (lst->items[static_cast<size_t>(i)].builtin >= 0) factory.push_back(i);
        for (const auto& want : kFirst) {
            const int at = lst->find(std::string("builtin:") + want.name);
            CHECK(at >= 0 && lst->items[static_cast<size_t>(std::max(at, 0))].category == want.category);
        }
    }
    Buf l, r;
    beat(l, r);
    for (size_t k = 0; k < factory.size(); ++k) {
        Host h;
        const auto lst = presetLibrary().listing();
        const Entry& e = lst->items[static_cast<size_t>(factory[k])];
        // Init, back to the first item, then forward to this one.
        h.press(P_PRE_INIT);
        for (int i = 0; i < items; ++i) h.press(P_PRESET_PREV);
        for (size_t i = 0; i < k; ++i) h.press(P_PRESET_NEXT);
        CHECK(h.display(P_PRESET) == "PRESET  " + e.category + " / " + e.name);
        std::string text;
        CHECK(presetText(e.key, text) && landed(h, text));
        h.play(true);
        h.run(l, r);
        const double change = changeDb(l, r, h.L, h.R);
        const float high = std::max(peak(h.L), peak(h.R));
        std::printf("  %-14s %-18s changes the beat by %6.1f dB, peak %5.1f dBFS\n", e.category.c_str(), e.name.c_str(), change, db(high));
        CHECK(h.finite && high <= kCeiling);
        if (e.name == "Init") CHECK(h.L == l && h.R == r);
        else CHECK(change > -40.0);
    }
    CHECK(factory.size() >= sizeof kFirst / sizeof kFirst[0]);
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

// 60 s of random parameter writes, taps and detents, transport jumps and bursts of NaN and infinity in loud
// input, with the chain (Pump, Shape, every lane, the targets, the levels) running a good part of the time: every
// sample that leaves is a number, none above +18 dBFS, and Init afterwards is the input again.
void stress() {
    std::printf("== 60 s of everything at once, at random\n");
    freshUserPresets();
    Host h;
    Rng rng(12345);
    const int total = 60 * kBlocksPerSec;
    float worst = 0.0f;
    int saves = 0, anyOn = 0, bothOn = 0;
    h.play(true);
    for (int blk = 0; blk < total; ++blk) {
        if (blk % kBlocksPerSec == kBlocksPerSec - 1) {   // once a second: how many parts the status line says run
            const int parts = partsOn(h);
            anyOn += parts >= 1;
            bothOn += parts == 2;
        }
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
                h.press(id);   // the browser: tiles, arrows, RND, favourites, Init, the stepper
            }
        }
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
    std::printf("  the status line said a part ran in %d of 60 seconds, both in %d\n", anyOn, bothOn);
    CHECK(anyOn >= 20 && bothOn >= 3);   // the chain really was running, not resting through it
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
    basics();
    displays();
    mapping();
    passThrough();
    levels();
    pumpThroughMpc();
    detents();
    presetsRoundTrip();
    factoryPresets();
    projectState();
    displayRate();
    statusLine();
    suspendResume();
    suspendPumping();
    stress();

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}
