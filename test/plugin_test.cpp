// The whole plugin driven through its VST2 entry points the way MPC drives an insert effect (128-frame
// blocks in place, 0..1 params, the Force's taps and Q-Link detents, MPC's transport). The suites of the
// modules are test/*_test.cpp; pluginTests() is called by test/main.cpp after them. Built with ASan/UBSan
// by `make test`, for the Force's CPU under qemu by `make test-arm`.
//
// What is tested here is the glue and the chain behind it: the parameters, their texts and their map onto
// the chain (grooveParams), the presets (the factory set part by part, its Moves and its levels: factoryPresets,
// factoryLevels) and the project state, the redraws and the status line, the Pump on MPC's grid (pumpThroughMpc),
// and suspend and resume with the Pump running. The Echo, Time and the Moves through the plugin, and 60 s of
// everything at once, are test/plugin_moves_test.cpp; the fixtures and the helpers the two use are in
// test/host.h. The modules' own behaviour is in their suites.
#include "host.h"
#include "../plugin/presets.h"
#include "../plugin/surface.h"
#include "factory_presets.h"
#include "reference_mix.h"

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

// Every sound parameter at a value of its own: an option picked by name, a number somewhere in its range.
void spread(Host& h, Rng& rng) {
    for (int i = 0; i < P_COUNT; ++i) {
        if (PARAM_INFO[i].kind != Kind::Synth) continue;
        if (PARAM_INFO[i].nopts > 0) h.set(i, static_cast<float>(rng.below(PARAM_INFO[i].nopts)));
        else h.setN(i, rng.next());
    }
}

bool allDefaults(Host& h) {
    for (int i = 0; i < P_COUNT; ++i)
        if (PARAM_INFO[i].kind == Kind::Synth && std::fabs(h.get(i) - PARAM_INFO[i].def) > 2e-5f) return false;
    return true;
}

// A steady input through the plugin, the output appended to `all`: the host's timeline from where it began.
void runDc(Host& h, Buf& all, int blocks, float l = 0.5f, float r = 0.25f) {
    h.run(Buf(static_cast<size_t>(blocks) * kBlock, l), Buf(static_cast<size_t>(blocks) * kBlock, r));
    all.insert(all.end(), h.L.begin(), h.L.end());
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
    CHECK(std::string(b) == "GrooveWerk");
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
    CHECK(starts(saved, "groovewerk 1\n") && has(saved, "\nswing=") && has(saved, "\nvol_shape=") && has(saved, "\nflt_cutoff="));
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
        << "groovewerk 1\nswing=0.7\nno_such_key=5\npump_curve=Wobble\npump_rate=1 bar\nvol_depth=oops\n";
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

// Every factory preset stays level-matched (EffectWerk's testFactory): the reference material of its category
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
    CHECK(starts(chunk, "groovewerk 1\n") && has(chunk, "\npreset=builtin:Init\n"));
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
    {   // a state written by a later version (keys we don't have, "groovewerk 2"), a text editor's byte-order mark and CRLF
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
        for (const char* junk : {"", "hello", "masterwerk 1\nswing=0.7\n", "groovewerk\nswing=0.7\n", "groovewerk 0\nswing=0.7\n",
                                 "effectwerk 1\n"})
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

} // namespace

// The suite's own fixtures: where the presets and the data go, the surface's clock that only moves per host event.
void setupPluginTests() {
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
}

void cleanupPluginTests() {
    std::error_code ec;
    std::filesystem::remove_all(fixtureDir(), ec);
}

} // namespace eft

void pluginTests() {
    using namespace eft;
    setupPluginTests();

    std::printf("== the plugin\n");
    GF_RUN(basics);
    GF_RUN(displays);
    GF_RUN(mapping);
    GF_RUN(passThrough);
    GF_RUN(levels);
    GF_RUN(pumpThroughMpc);
    GF_RUN(detents);
    GF_RUN(presetsRoundTrip);
    GF_RUN(freshInstance);
    GF_RUN(factoryPresets);
    GF_RUN(factoryLevels);
    GF_RUN(projectState);
    GF_RUN(displayRate);
    GF_RUN(statusLine);
    GF_RUN(suspendResume);
    GF_RUN(suspendPumping);

    cleanupPluginTests();
}
