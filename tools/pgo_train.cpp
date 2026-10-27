// The profile-guided build's trainer (make arm-plugin with PGO): an instrumented copy of the plugin is
// linked in and plays the reference mix (tools/reference_mix.h) through VSTPluginMain under qemu-arm,
// the way MPC drives an insert at 128 BPM. The passes, in order:
//
//   Init          at rest, then hot: the plugin's own work and the output guard
//   choices       every option of every choice, with all four parts on (the cases of tools/cases.h at their
//                 heaviest, Time on a groove)
//   knobs         every numeric parameter moved across its range and back, one setting a block, as automation or a
//                 Q-Link turned: the profile otherwise never sees a knob move
//   everything    the cases' everything heaviest (tools/bench.cpp's, from tools/cases.h): a few bars, hot and at rest
//   transport     the same under a stopped transport (MPC suspends the insert for a moment on Stop and resumes it), a
//                 locate forward and back, a loop wrapping, every tempo from 20 to 999 BPM and a ramp through them,
//                 a long suspend (the clear), and a swing on 1/8
//   Moves         every Move, once as a One Shot and once going round
//   presets       every factory preset
//
// The presets are walked with the preset buttons (to the first, then NEXT until the stepper stops moving), so a preset
// added to presets/Factory (surface.py compiles the folder into the plugin) joins the training by itself; the
// trainer says how many it played and stops the build if that is fewer than the folder has. Every setting is read
// back (cases.h). The profile only steers the compiler (which paths are hot); what the trainer leaves out is still
// optimised as usual (-fprofile-partial-training).
#include "../plugin/vst2.h"
#include "cases.h"
#include "factory_presets.h"
#include "moves.h"
#include "param_ids.h"
#include "reference_mix.h"
#include "surface.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" AEffect* VSTPluginMain(audioMasterCallback);

namespace {

using namespace gf;
using namespace gfc;

VstTimeInfo g_time{};

intptr_t master(AEffect*, int32_t op, int32_t, intptr_t, void*, float) {
    if (op == vst::audioMasterVersion) return 2400;
    if (op == vst::audioMasterGetTime) return reinterpret_cast<intptr_t>(&g_time);
    return 0;
}

// The plugin tells gestures (a Q-Link turning) from taps by the time between a parameter's sets, and MPC's
// release echo from a second tap. The trainer is neither: every set it makes is a tap, however fast the
// emulated machine runs, so the clock the surface reads moves a second at every look (a short suspend
// needs one under 250 ms: kShortMs). With it, a set lands on exactly the option it names.
long long g_clock = 0, g_clockStep = 1000;
long long clockNow() { return g_clock += g_clockStep; }
constexpr long long kShortMs = 10;

std::vector<float> g_L, g_R;   // the reference mix
size_t g_at = 0;

bool playing() { return (g_time.flags & vst::kVstTransportPlaying) != 0; }

// One block of the reference mix at `gain` through the plugin, in place; the transport moves on while it plays.
void block(AEffect* e, float gain = 1.0f) {
    float L[128], R[128];
    float* io[2] = {L, R};
    for (int i = 0; i < 128; ++i) {
        L[i] = gain * g_L[g_at];
        R[i] = gain * g_R[g_at];
        g_at = (g_at + 1) % g_L.size();
    }
    e->processReplacing(e, io, io, 128);
    if (playing()) g_time.ppqPos += 128.0 / 44100.0 * g_time.tempo / 60.0;
    e->getParameter(e, P_STATUS);   // MPC's UI side polls while the audio runs
    e->getParameter(e, P_PRESET);
}

// `seconds` of it.
void play(AEffect* e, double seconds, float gain = 1.0f) {
    const int blocks = std::max(1, static_cast<int>(seconds * 44100.0 / 128.0));
    for (int b = 0; b < blocks; ++b) block(e, gain);
}

// `beats` of it, at the tempo of the moment.
void playBeats(AEffect* e, double beats) { play(e, beats * 60.0 / g_time.tempo); }

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

// MPC's Stop: the insert is suspended for about 100 ms and resumed. A short one keeps every state; a long one (the
// slot's ON button) clears them.
void suspend(AEffect* e, bool longOne) {
    g_clockStep = longOne ? 1000 : kShortMs;
    e->dispatcher(e, vst::effMainsChanged, 0, 0, nullptr, 0.0f);
    e->dispatcher(e, vst::effMainsChanged, 0, 1, nullptr, 0.0f);
    g_clockStep = 1000;
}

// All four parts on, at their heaviest, with Time on a groove (a Move would take its place, and the grooves are what
// the choices and the knobs are to reach). The Move is the transport pass's, and the Moves' own.
void everythingOnAGroove(AEffect* e) {
    bothHeaviest(e);
    opt(e, P_PUMP_TARGET, "All");
    opt(e, P_SHAPE_TARGET, "All");
    echoFadeHeaviest(e);
    opt(e, P_TIME_ON, "On");
    opt(e, P_TIME_GROOVE, "Drunk");
    num(e, P_TIME_AMOUNT, 0.8f);
}

// Every option of every choice, each for a moment; a choice goes back to what it was after its sweep.
void choices(AEffect* e) {
    for (int id = 0; id < P_COUNT; ++id) {
        if (PARAM_INFO[id].kind != Kind::Synth || PARAM_INFO[id].nopts < 2) continue;
        const float was = e->getParameter(e, id);
        for (int o = 0; o < PARAM_INFO[id].nopts; ++o) {
            put(e, id, static_cast<float>(o) / static_cast<float>(PARAM_INFO[id].nopts - 1));
            play(e, 0.06);
        }
        put(e, id, was);
        play(e, 0.06);
    }
}

// Every numeric parameter from the bottom of its range to the top and back to where it was, a setting a block.
void knobs(AEffect* e) {
    constexpr int kSteps = 24;
    for (int id = 0; id < P_COUNT; ++id) {
        const Curve c = PARAM_SPECS[id].curve;
        if (PARAM_INFO[id].kind != Kind::Synth || !(c == Curve::Lin || c == Curve::Log || c == Curve::Pow)) continue;
        const float was = e->getParameter(e, id);
        for (int s = 0; s <= kSteps; ++s) {
            put(e, id, static_cast<float>(s) / kSteps);
            block(e);
        }
        for (int s = 1; s <= 8; ++s) {
            put(e, id, was + (1.0f - static_cast<float>(s) / 8.0f) * (1.0f - was));
            block(e);
        }
        put(e, id, was);
        play(e, 0.05);
    }
}

void transport(AEffect* e) {
    everythingOnAGroove(e);
    fireMove(e, kRollMove);   // going round
    play(e, 2.0);
    g_time.flags &= ~vst::kVstTransportPlaying;   // stopped: the grid runs on at the tempo
    suspend(e, false);
    play(e, 1.5);
    g_time.flags |= vst::kVstTransportPlaying;
    suspend(e, false);
    play(e, 1.0);
    g_time.ppqPos += 13.37;   // a locate forward
    play(e, 0.5);
    g_time.ppqPos = 1.0;      // and back, to a place that is on no line
    play(e, 0.5);
    for (int lap = 0; lap < 5; ++lap) {   // a loop of two beats and a bit, wrapping
        g_time.ppqPos = 8.0;
        playBeats(e, 2.2);
    }
    for (const double bpm : {90.0, 174.0, 999.0, 20.0, 128.0}) {   // tempo changes
        g_time.tempo = bpm;
        playBeats(e, bpm > 500.0 ? 8.0 : bpm < 50.0 ? 0.4 : 2.0);
    }
    for (int b = 0; b < 250; ++b) {   // a ramp through the tempos
        g_time.tempo = 100.0 + 50.0 * b / 250.0;
        block(e);
    }
    g_time.tempo = 128.0;
    suspend(e, true);   // the slot switched off for a while and back: every tail cleared
    play(e, 1.0);
    // A Move that waits for its line (a bar off) when the grid is located: a little (the line goes with it) and a lot (it is looked for again).
    opt(e, P_MOVE, "None");
    opt(e, P_MOVE_QUANT, "Bar");
    opt(e, P_MOVE, "Tape Stop");
    play(e, 0.1);
    g_time.ppqPos += 0.005;
    play(e, 0.1);
    g_time.ppqPos += 7.77;
    playBeats(e, 5.0);
    opt(e, P_MOVE_QUANT, "1/16");
    opt(e, P_SWING_UNIT, "1/8");   // swing on 1/8, which the pass otherwise never sees
    num(e, P_SWING, 0.66f);
    play(e, 1.5);
    opt(e, P_TIME_GROOVE, "Swing");   // Time swinging the audio: the Pump's and Shape's steps go straight
    opt(e, P_MOVE, "None");
    play(e, 1.5);
    opt(e, P_TIME_PLACE, "Pre");
    play(e, 1.0);
}

// Every Move, from a sound with the Pump and the Shape on and the Echo's lines set but the Echo, like Time, off: the
// Move switches on what it needs. A One Shot is played to its end and a beat on (at most six beats of it), a Move going
// round for a lap and a quarter (at most five beats).
void moves(AEffect* e) {
    bothHeaviest(e);
    echoFadeHeaviest(e);
    opt(e, P_ECHO_ON, "Off");
    play(e, 0.5);
    for (int s = 0; s <= 20; ++s) {   // the dry level moving with no wet to join: the dry alone
        num(e, P_DRY, 1.0f - 0.5f * static_cast<float>(s <= 10 ? s : 20 - s) / 10.0f);
        block(e);
    }
    for (int pass = 0; pass < 2; ++pass) {
        opt(e, P_TIME_PLACE, pass == 0 ? "Post" : "Pre");   // (Pre, with Time off between the Moves: its ring listens before the key)
        for (int k = 0; k < Moves::count(); ++k) {
            const double len = moveDef(k).lengthBeats;
            fireMove(e, PARAM_INFO[P_MOVE].opts[k + 1], pass == 0 ? "One Shot" : "Loop");   // (None is the first option)
            playBeats(e, pass == 0 ? std::min(len, 6.0) + 0.5 : std::min(1.25 * len, 5.0));
            if (k % 4 == 3) {   // GO: the selected Move again
                press(e, P_MOVE_GO);
                playBeats(e, 1.0);
            }
            opt(e, P_MOVE, "None");
            play(e, 0.15);
        }
    }
}

// Every preset in the library: the stepper to the first (it stops there), then NEXT until it stops again.
constexpr int kMaxPresets = 2000;   // (a stepper that never stops is a bug, not a long list)
constexpr double kPresetSeconds = 0.75;
int presets(AEffect* e) {
    press(e, P_PRE_INIT);
    play(e, kPresetSeconds);
    float at = e->getParameter(e, P_PRESET);
    for (int n = 0; n < kMaxPresets; ++n) {
        press(e, P_PRESET_PREV);
        const float now = e->getParameter(e, P_PRESET);
        if (now == at) break;
        at = now;
    }
    if (at != 0.0f) {   // PREV did not reach the first preset: counting from here would pass for a walk of the whole library
        std::fprintf(stderr, "pgo_train: the preset walk stopped at %.4f, not on the first preset\n", static_cast<double>(at));
        std::exit(1);
    }
    play(e, kPresetSeconds);
    int played = 1;
    for (int n = 0; n < kMaxPresets; ++n) {
        press(e, P_PRESET_NEXT);
        const float now = e->getParameter(e, P_PRESET);
        if (now == at) break;
        at = now;
        ++played;
        play(e, kPresetSeconds);
    }
    return played;
}

} // namespace

int main() {
    gfl::referenceMix(g_L, g_R);
    Surface::clock = clockNow;
    g_time.sampleRate = 44100.0;
    g_time.tempo = 128.0;
    g_time.timeSigNumerator = g_time.timeSigDenominator = 4;
    g_time.flags = vst::kVstTempoValid | vst::kVstPpqPosValid | vst::kVstTransportPlaying;

    {   // at rest (Init), then hot
        AEffect* e = fresh();
        play(e, 2.0);
        play(e, 2.0, 4.0f);
        done(e);
    }
    {
        AEffect* e = fresh();
        everythingOnAGroove(e);
        play(e, 1.0);
        choices(e);
        done(e);
    }
    {
        AEffect* e = fresh();
        everythingOnAGroove(e);
        play(e, 1.0);
        knobs(e);
        done(e);
    }
    {   // everything at its heaviest (the bench's case): a few bars, the input hot and at rest
        AEffect* e = fresh();
        everythingHeaviest(e);
        play(e, 8.0);
        play(e, 2.0, 4.0f);
        done(e);
    }
    {
        AEffect* e = fresh();
        transport(e);
        done(e);
    }
    {
        g_time.tempo = 240.0;   // (the Moves' lengths are in beats: a faster tempo trains them in less time)
        AEffect* e = fresh();
        moves(e);
        done(e);
        g_time.tempo = 128.0;
    }
    int played = 0;
    {
        AEffect* e = fresh();
        played = presets(e);
        done(e);
    }
    std::printf("pgo_train: %d presets played, %d factory presets\n", played, kNumFactoryPresets);
    if (played < kNumFactoryPresets) {   // the walk stopped early: a preset the build compiled in was not trained on
        std::fprintf(stderr, "pgo_train: the preset walk reached %d of the %d factory presets\n", played, kNumFactoryPresets);
        return 1;
    }
    std::printf("pgo_train: done\n");
    return 0;
}
