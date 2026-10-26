// The profile-guided build's trainer (make arm-plugin with PGO): an instrumented copy of the plugin is
// linked in and plays the reference mix (tools/reference_mix.h) through VSTPluginMain under qemu-arm,
// the way MPC drives an insert at 128 BPM: at rest (Init), then every option of every choice with the
// Pump and the Shape on, then the two at their heaviest together (as tools/bench.cpp's Both heaviest),
// then every factory preset. The presets are walked with the preset buttons, so a preset added to
// presets/Factory joins the training by itself. The profile only steers the compiler (which paths are
// hot); what the trainer leaves out is still optimised as usual (-fprofile-partial-training).
#include "../plugin/vst2.h"
#include "factory_presets.h"
#include "param_ids.h"
#include "reference_mix.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" AEffect* VSTPluginMain(audioMasterCallback);

namespace {

using namespace gf;

VstTimeInfo g_time{};

intptr_t master(AEffect*, int32_t op, int32_t, intptr_t, void*, float) {
    if (op == vst::audioMasterVersion) return 2400;
    if (op == vst::audioMasterGetTime) return reinterpret_cast<intptr_t>(&g_time);
    return 0;
}

std::vector<float> g_L, g_R;   // the reference mix
size_t g_at = 0;

// Plays `seconds` of the reference mix through the plugin at `gain`, in place, 128 frames at a time.
void play(AEffect* e, double seconds, float gain = 1.0f) {
    float L[128], R[128];
    float* io[2] = {L, R};
    const int blocks = static_cast<int>(seconds * 44100.0 / 128.0);
    for (int b = 0; b < blocks; ++b) {
        for (int i = 0; i < 128; ++i) {
            L[i] = gain * g_L[g_at];
            R[i] = gain * g_R[g_at];
            g_at = (g_at + 1) % g_L.size();
        }
        e->processReplacing(e, io, io, 128);
        g_time.ppqPos += 128.0 / 44100.0 * g_time.tempo / 60.0;
        e->getParameter(e, P_STATUS);   // MPC's UI side polls while the audio runs
        e->getParameter(e, P_PRESET);
    }
}

AEffect* fresh() {
    AEffect* e = VSTPluginMain(master);
    e->dispatcher(e, vst::effOpen, 0, 0, nullptr, 0.0f);
    return e;
}

void done(AEffect* e) {
    char b[256];
    for (int i = 0; i < P_COUNT; ++i) {
        e->dispatcher(e, vst::effGetParamName, i, 0, b, 0.0f);
        e->dispatcher(e, vst::effGetParamDisplay, i, 0, b, 0.0f);
    }
    e->dispatcher(e, vst::effClose, 0, 0, nullptr, 0.0f);
}

void press(AEffect* e, int id) { e->setParameter(e, id, e->getParameter(e, id) > 0.5f ? 0.0f : 1.0f); }

// A real value as MPC's 0..1 (groove_map.cpp's paramNorm, without linking it: the trainer only needs the
// parameters' ranges).
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

void opt(AEffect* e, int id, const char* name) {
    const ParamInfo& p = PARAM_INFO[id];
    for (int o = 0; o < p.nopts; ++o)
        if (!std::strcmp(p.opts[o], name)) {
            num(e, id, static_cast<float>(o));
            return;
        }
    std::fprintf(stderr, "pgo_train: %s has no option \"%s\"\n", p.key, name);
    std::exit(1);
}

// The Pump and the Shape on at moderate settings (the start of the choices' sweep) ...
void partsOn(AEffect* e) {
    opt(e, P_PUMP_ON, "On");
    opt(e, P_PUMP_BAND, "Lows");
    opt(e, P_PUMP_TRIGGER, "Input");
    num(e, P_PUMP_DEPTH, 0.8f);
    opt(e, P_SHAPE_ON, "On");
    opt(e, P_VOL_ON, "On");
    opt(e, P_FLT_ON, "On");
    opt(e, P_PAN_ON, "On");
    num(e, P_VOL_DEPTH, 0.6f);
    num(e, P_FLT_DEPTH, 0.6f);
    num(e, P_PAN_DEPTH, 0.6f);
    num(e, P_FLT_CUTOFF, 1000.0f);
    num(e, P_FLT_RESO, 0.4f);
}

// ... and the two at their heaviest together (tools/bench.cpp's Both heaviest).
void bothHeaviest(AEffect* e) {
    opt(e, P_PUMP_ON, "On");
    opt(e, P_PUMP_RATE, "1/16");
    opt(e, P_PUMP_BAND, "Lows");
    opt(e, P_PUMP_TRIGGER, "Input");
    num(e, P_PUMP_DEPTH, 1.0f);
    num(e, P_PUMP_PUSH, -10.0f);
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
    num(e, P_SWING, 0.75f);
    opt(e, P_SWING_UNIT, "1/16");
}

} // namespace

int main() {
    gfl::referenceMix(g_L, g_R);
    g_time.sampleRate = 44100.0;
    g_time.tempo = 128.0;
    g_time.timeSigNumerator = g_time.timeSigDenominator = 4;
    g_time.flags = vst::kVstTempoValid | vst::kVstPpqPosValid | vst::kVstTransportPlaying;

    {   // at rest (Init), then hot: the plugin's own work and the output guard
        AEffect* e = fresh();
        play(e, 2.0);
        play(e, 2.0, 4.0f);
        done(e);
    }
    {   // every option of every choice, the parts on; each choice goes back to what it was after its sweep
        AEffect* e = fresh();
        partsOn(e);
        play(e, 1.0);
        for (int id = 0; id < P_COUNT; ++id) {
            if (PARAM_INFO[id].kind != Kind::Synth || PARAM_INFO[id].nopts < 2) continue;
            const float was = e->getParameter(e, id);
            for (int o = 0; o < PARAM_INFO[id].nopts; ++o) {
                e->setParameter(e, id, static_cast<float>(o) / static_cast<float>(PARAM_INFO[id].nopts - 1));
                play(e, 0.3);
            }
            e->setParameter(e, id, was);
            play(e, 0.3);
        }
        done(e);
    }
    {   // both at their heaviest, swing at 75%, a few bars, the input hot and at rest
        AEffect* e = fresh();
        bothHeaviest(e);
        play(e, 8.0);
        play(e, 2.0, 4.0f);
        done(e);
    }
    {   // every factory preset: Init, back to the first with PREV, then NEXT through the list
        AEffect* e = fresh();
        press(e, P_PRE_INIT);
        play(e, 1.0);
        for (int p = 0; p < kNumFactoryPresets; ++p) press(e, P_PRESET_PREV);
        play(e, 1.0);
        for (int p = 1; p < kNumFactoryPresets; ++p) {
            press(e, P_PRESET_NEXT);
            play(e, 1.0);
        }
        done(e);
    }
    std::printf("pgo_train: done\n");
    return 0;
}
