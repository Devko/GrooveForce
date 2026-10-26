// plugin/moves.h against the data surface.py generates (build/move_data.h): the sixteen Moves (a name that is the
// MOVE parameter's option, a pattern or ramps, the plan's anchors: the Throws, Dub Freeze, Filter Drop, Break and
// the Time patterns), then the engine: a Move fired mid-beat starts on the next 1/16, beat or bar line (a press a hair
// after a line counts as on it), a Throw raises the send for its beat and the knob's own value returns, a Loop goes
// round until stop(), a Move switches on the parts it needs for its length, Dub Freeze holds an option and restores it;
// one Move replaces another on its line; stop() glides the ramps back; a locate, a loop point, a tempo change and a
// stopped transport leave a Move where it was in its length; wild input; and the pattern Time is told to play,
// driven through TimeCore, starts on the line and goes round with the Move.
#include "signal.h"
#include "moves.h"
#include "../dsp/time.h"
#include "move_data.h"   // generated: surface/build
#include "param_ids.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace {

using namespace eft;
using gf::Moves;
using gf::MoveDef;
using gf::MoveRamp;
using ef::kChunk;
using ef::kRate;

constexpr double kBeat = 22050.0;                       // a beat at 120 BPM, in samples
constexpr double kChunkBeats = kChunk * 2.0 / kRate;    // a chunk at 120 BPM
constexpr double kFadeBeats = gf::TimeCore::kFade * 2.0 / kRate;   // Time's crossfade at 120 BPM: where a pattern is handed over
constexpr double kLeadBeats = 20.0 * 0.001 * 2.0;       // the parts' lead, 20 ms at 120 BPM
constexpr int kGlide = 15;                              // chunks after which a 10 ms glide-back is over (14 + 1)
constexpr int P_SEND = gf::P_ECHO_SEND, P_FB = gf::P_ECHO_FB;

int move(const char* name) {
    for (int i = 0; i < gf::numMoves(); ++i)
        if (std::strcmp(gf::moveDef(i).name, name) == 0) return i;
    std::printf("  no Move named %s\n", name);
    CHECK(false);
    return 0;
}

// The Moves, the knobs as MPC's snapshot has them (the parameters' defaults, to be changed), and the chunks played
// one at a time: each chunk starts from the knobs and lets the Moves lay theirs on top, as the engine will.
struct Rig {
    Moves m;
    float base[gf::P_COUNT], out[gf::P_COUNT];
    int pat = -1;
    double start = 0.0;
    bool loop = false;
    double beats = 0.0, bpm = 120.0;
    bool playing = true;
    double stoppedAt = 12345.0;   // what a stopped transport says it is at: not used

    Rig() {
        for (int i = 0; i < gf::P_COUNT; ++i) base[i] = out[i] = gf::PARAM_INFO[i].def;
    }
    ef::Transport transport() const {
        ef::Transport t;
        t.bpm = bpm;
        t.beats = playing ? beats : stoppedAt;
        t.playing = playing;
        t.valid = true;
        return t;
    }
    void step(int frames = kChunk) {
        std::memcpy(out, base, sizeof out);
        m.apply(out, transport(), &pat, &start, &loop, frames);
        beats += frames * bpm / 60.0 / kRate;
    }
    // Chunks until the next one would start at or after `to` beats; the last chunk's start comes back.
    double run(double to) {
        double last = beats;
        while (beats < to) {
            last = beats;
            step();
        }
        return last;
    }
    bool atRest() const { return std::memcmp(out, base, sizeof out) == 0; }
};

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// --- the data ---------------------------------------------------------------------------------------

void moveData() {
    CHECK(gf::numMoves() == 16 && gf::kNumMoves == 16);
    CHECK(gf::PARAM_INFO[gf::P_MOVE].nopts == 17 && std::strcmp(gf::PARAM_INFO[gf::P_MOVE].opts[0], "None") == 0);
    int withPattern = 0;
    bool names = true, shape = true, ramps = true, parts = true, windows = true, lengths = true;
    for (int i = 0; i < gf::numMoves(); ++i) {
        const MoveDef& d = gf::moveDef(i);
        // the tile's text is the Move parameter's option, in order (R28)
        names = names && d.name && std::strlen(d.name) > 0 && std::strcmp(d.name, gf::PARAM_INFO[gf::P_MOVE].opts[i + 1]) == 0;
        shape = shape && (d.timePattern >= 0 || d.rampCount > 0);   // a pattern or ramps
        lengths = lengths && d.lengthBeats > 0.0f && d.lengthBeats <= 16.0f;
        if (d.timePattern >= 0) {
            ++withPattern;
            const bool real = d.timePattern < gf::numTimePatterns() && std::strlen(gf::timePatternName(d.timePattern)) > 0;
            lengths = lengths && real && d.lengthBeats == gf::timePatternDef(d.timePattern).lengthBeats;   // as long as its pattern
            parts = parts && (d.partsOn & gf::kPartTime) != 0;
        } else {
            parts = parts && (d.partsOn & gf::kPartTime) == 0;
        }
        parts = parts && d.partsOn != 0 && (d.partsOn & ~15) == 0;
        ramps = ramps && d.rampCount >= 0 && d.rampCount <= gf::kMaxMoveRamps && d.rampFirst >= 0 && d.rampFirst + d.rampCount <= gf::kNumMoveRamps;
        for (int k = 0; k < d.rampCount && ramps; ++k) {
            const MoveRamp& r = gf::moveRamp(d.rampFirst + k);
            ramps = ramps && r.param > 0 && r.param < gf::P_COUNT && gf::PARAM_INFO[r.param].kind == gf::Kind::Synth;
            if (r.kind == gf::kMoveAdd) {
                ramps = ramps && gf::PARAM_SPECS[r.param].curve != gf::Curve::Enum && r.amount >= -1.0f && r.amount <= 1.0f;
            } else {   // a Set holds one of the parameter's options, as its 0..1 value
                const double idx = static_cast<double>(r.amount) * (gf::PARAM_INFO[r.param].nopts - 1);
                ramps = ramps && r.kind == gf::kMoveSet && gf::PARAM_SPECS[r.param].curve == gf::Curve::Enum &&
                        near(idx, std::round(idx), 1e-5) && r.amount >= 0.0f && r.amount <= 1.0f;
            }
            ramps = ramps && r.shape <= gf::kMoveFall;
            windows = windows && r.from >= 0.0f && r.from < r.to && r.to <= 1.0f;
        }
    }
    CHECK(names);
    CHECK(shape);
    CHECK(lengths);
    CHECK(ramps);
    CHECK(windows);
    CHECK(parts);
    CHECK(withPattern == 11);   // the rest play their Time pattern: 11 of the 16, and the Throws, Dub Freeze, Filter Drop and Break do not
    CHECK(gf::kNumMoveRamps <= 16 * gf::kMaxMoveRamps);
    // an index out of range is the nearest Move
    CHECK(&gf::moveDef(-3) == &gf::moveDef(0) && &gf::moveDef(99) == &gf::moveDef(15));
    CHECK(Moves::count() == 16 && std::strcmp(Moves::name(0), "Throw Beat") == 0 && Moves::partsOf(0) == gf::kPartEcho);
}

// The plan's anchors, one by one.
void moveAnchors() {
    const auto ramp = [](const char* mvName, int param, int nth = 0) -> const MoveRamp* {
        const MoveDef& d = gf::moveDef(move(mvName));
        int seen = 0;
        for (int k = 0; k < d.rampCount; ++k)
            if (gf::moveRamp(d.rampFirst + k).param == param && seen++ == nth) return &gf::moveRamp(d.rampFirst + k);
        return nullptr;
    };
    const auto pure = [](const char* mvName) { return gf::moveDef(move(mvName)).rampCount == 0; };
    // Throws: the Echo's send to 1 for the length (an Add of the whole travel, held), the feedback +0.3, the Echo switched on
    for (const char* n : {"Throw Beat", "Throw Bar"}) {
        const MoveDef& d = gf::moveDef(move(n));
        const MoveRamp* s = ramp(n, P_SEND);
        const MoveRamp* f = ramp(n, P_FB);
        CHECK(s && s->kind == gf::kMoveAdd && s->amount == 1.0f && s->shape == gf::kMoveHold && s->from == 0.0f && s->to == 1.0f);
        CHECK(f && f->kind == gf::kMoveAdd && near(f->amount, 0.3, 1e-6) && f->shape == gf::kMoveHold);
        CHECK(d.partsOn == gf::kPartEcho && d.timePattern < 0 && d.rampCount == 2);
    }
    CHECK(gf::moveDef(move("Throw Beat")).lengthBeats == 1.0f && gf::moveDef(move("Throw Bar")).lengthBeats == 4.0f);
    // Dub Freeze: the Echo's freeze On for a bar after a send pulse
    {
        const MoveDef& d = gf::moveDef(move("Dub Freeze"));
        const MoveRamp* fz = ramp("Dub Freeze", gf::P_ECHO_FREEZE);
        const MoveRamp* pulse = ramp("Dub Freeze", P_SEND);
        CHECK(fz && fz->kind == gf::kMoveSet && fz->amount == 1.0f);
        CHECK(fz && near((fz->to - fz->from) * d.lengthBeats, 4.0, 1e-5) && fz->to == 1.0f);   // 1 bar, to the end of the Move
        CHECK(pulse && pulse->kind == gf::kMoveAdd && pulse->amount == 1.0f && pulse->from == 0.0f && fz && pulse->to <= fz->from);   // before it
        CHECK(d.partsOn == gf::kPartEcho && d.timePattern < 0);
    }
    // Filter Drop: the Shape's filter lane on, its cutoff falling 2 octaves over a bar
    {
        const MoveDef& d = gf::moveDef(move("Filter Drop"));
        const MoveRamp* cut = ramp("Filter Drop", gf::P_FLT_CUTOFF);
        const MoveRamp* on = ramp("Filter Drop", gf::P_FLT_ON);
        const double octave = std::log(2.0) / std::log(static_cast<double>(gf::PARAM_SPECS[gf::P_FLT_CUTOFF].hi) / gf::PARAM_SPECS[gf::P_FLT_CUTOFF].lo);
        CHECK(cut && cut->kind == gf::kMoveAdd && cut->shape == gf::kMoveRise && near(cut->amount, -2.0 * octave, 1e-5));
        CHECK(on && on->kind == gf::kMoveSet && on->amount == 1.0f);
        CHECK(d.lengthBeats == 4.0f && d.partsOn == gf::kPartShape && d.timePattern < 0);
    }
    // Break: dry 0 for a bar with a send pulse at its start
    {
        const MoveDef& d = gf::moveDef(move("Break"));
        const MoveRamp* dry = ramp("Break", gf::P_DRY);
        const MoveRamp* pulse = ramp("Break", P_SEND);
        CHECK(dry && dry->kind == gf::kMoveAdd && dry->amount == -1.0f && dry->shape == gf::kMoveHold && dry->from == 0.0f && dry->to == 1.0f);
        CHECK(pulse && pulse->kind == gf::kMoveAdd && pulse->amount == 1.0f && pulse->from == 0.0f && pulse->to < 1.0f);
        CHECK(d.lengthBeats == 4.0f && d.partsOn == gf::kPartEcho && d.timePattern < 0);
    }
    // the rest play their Time pattern, by their own name, and nothing else
    for (const char* n : {"Half-Time Bar", "Tape Stop", "Slow Down", "Reverse Beat", "Spin-Back", "Stutter 1/8", "Stutter 1/16",
                          "Roll Build 1 Bar", "Roll Build 4 Bars", "Gate 1/16", "Rewind"}) {
        const MoveDef& d = gf::moveDef(move(n));
        CHECK(d.timePattern >= 0 && std::strcmp(gf::timePatternName(d.timePattern), n) == 0);
        CHECK(pure(n) && d.partsOn == gf::kPartTime);
    }
}

// --- firing ---------------------------------------------------------------------------------------------

// Fired mid-beat, a Move starts on the next line of its quantize: in the chunk the line falls in, its pattern starts there
// (and is handed over 5 ms before), nothing is heard of it before.
void moveQuantizes() {
    const int rev = move("Reverse Beat");
    struct Case { int quant; double at, line; };
    const Case cases[] = {
        {Moves::kSixteenth, 2.30, 2.50}, {Moves::kSixteenth, 2.60, 2.75}, {Moves::kSixteenth, 2.52, 2.75},
        {Moves::kBeat, 2.30, 3.0}, {Moves::kBeat, 2.95, 3.0}, {Moves::kBeat, 3.024, 4.0},
        {Moves::kBar, 2.30, 4.0}, {Moves::kBar, 4.10, 8.0}, {Moves::kBar, 0.5, 4.0},
    };
    for (const Case& c : cases) {
        Rig r;
        r.beats = c.at;
        r.m.fire(rev, Moves::kOneShot, c.quant);
        CHECK(r.m.armed() == rev && r.m.playing() == -1);
        double handed = -1.0, began = -1.0;
        bool quiet = true;
        for (int k = 0; k < 40000 && began < 0.0; ++k) {
            const double s0 = r.beats;
            r.step();
            if (r.pat >= 0 && handed < 0.0) handed = s0;
            if (r.m.playing() == rev) began = s0;
            else quiet = quiet && r.m.playing() == -1 && r.m.armed() == rev && (r.pat < 0) == (handed < 0.0);
        }
        CHECK(began >= 0.0);
        CHECK(quiet);
        CHECK(began <= c.line && c.line < began + kChunkBeats);      // the chunk the line is in
        CHECK(r.pat == gf::moveDef(rev).timePattern && near(r.start, c.line, 1e-9));
        CHECK(r.m.armed() == -1);
        // the pattern is handed over when its 5 ms of crossfade begin, not before
        CHECK(handed >= 0.0 && c.line - (handed + kChunkBeats) <= kFadeBeats + 1e-9 && c.line - handed > kFadeBeats);
    }
    {   // a press up to 10 ms after a line counts as on it: the Move starts in that chunk, its line the line
        Rig r;
        r.beats = 3.0 + 0.005 * 2.0;   // 5 ms after beat 3
        r.m.fire(rev, Moves::kOneShot, Moves::kBeat);
        r.step();
        CHECK(r.m.playing() == rev && near(r.start, 3.0, 1e-9));
    }
    {   // 12 ms after it does not
        Rig r;
        r.beats = 3.0 + 0.012 * 2.0;
        r.m.fire(rev, Moves::kOneShot, Moves::kBeat);
        r.step();
        CHECK(r.m.playing() == -1 && r.m.armed() == rev);
        r.run(4.1);
        CHECK(r.m.playing() == rev && near(r.start, 4.0, 1e-9));
    }
    {   // exactly on a line: that line
        Rig r;
        r.beats = 8.0;
        r.m.fire(rev, Moves::kOneShot, Moves::kBar);
        r.step();
        CHECK(r.m.playing() == rev && near(r.start, 8.0, 1e-12));
    }
    {   // nothing is heard of a Move before its line: the knobs bit for bit, every chunk
        Rig r;
        r.base[P_SEND] = 0.0f;
        r.beats = 0.3;
        r.m.fire(move("Throw Beat"), Moves::kOneShot, Moves::kBeat);
        bool rest = true;
        while (r.beats < 0.9) {
            r.step();
            rest = rest && r.atRest();
        }
        CHECK(rest);
    }
    {   // fired again before the line: the new Move takes the place, from its own line
        Rig r;
        r.beats = 2.30;
        r.m.fire(rev, Moves::kOneShot, Moves::kBar);
        r.run(2.6);
        r.m.fire(move("Rewind"), Moves::kOneShot, Moves::kBeat);
        CHECK(r.m.armed() == move("Rewind"));
        r.run(3.1);
        CHECK(r.m.playing() == move("Rewind") && near(r.start, 3.0, 1e-9));
        r.run(4.2);   // the Rewind is over (a beat) and the first one, replaced, never starts on beat 4
        CHECK(r.m.playing() == -1 && r.m.armed() == -1 && r.pat == -1);
    }
}

// Throw Beat raises the send (and the feedback) for its beat, glides back over 10 ms, and the knob's own value returns.
void moveRampsReturn() {
    const int tb = move("Throw Beat");
    Rig r;
    r.base[P_SEND] = 0.0f;
    r.base[P_FB] = 0.5f;   // 0 on its -1..1 travel
    r.beats = 0.3;
    r.m.fire(tb, Moves::kOneShot, Moves::kBeat);   // the line is beat 1, the end beat 2
    r.run(0.99);
    CHECK(r.out[P_SEND] == 0.0f && r.out[P_FB] == 0.5f);
    // in over 1 ms: from the chunk the line is in, a few chunks, never down, and then full
    float prev = 0.0f;
    bool rising = true;
    double up = -1.0, full = -1.0;
    while (r.beats < 1.03) {
        const double s0 = r.beats;
        r.step();
        rising = rising && r.out[P_SEND] >= prev;
        prev = r.out[P_SEND];
        if (prev > 0.0f && up < 0.0) up = s0;
        if (prev == 1.0f && full < 0.0) full = s0;
    }
    CHECK(rising);
    CHECK(up >= 0.0 && up <= 1.0 && 1.0 < up + kChunkBeats);     // the chunk the line is in
    CHECK(full > up && full <= up + 2.0 * kChunkBeats + 1e-9);    // and 1 ms later all of it
    r.run(1.5);
    CHECK(r.out[P_SEND] == 1.0f && near(r.out[P_FB], 0.8, 1e-6));   // +0.3 of the travel
    CHECK(r.m.playing() == tb);
    bool held = true;
    while (r.beats < 1.99) {
        r.step();
        held = held && r.out[P_SEND] == 1.0f && near(r.out[P_FB], 0.8, 1e-6);
    }
    CHECK(held);
    // the end: the first chunk at beat 2 starts the glide back; down 10 ms later, steadily
    r.run(2.0 + 1.5 * kChunkBeats);
    CHECK(r.m.playing() == -1);
    prev = r.out[P_SEND];
    bool falling = true, smooth = true;
    int chunks = 0;
    while (r.out[P_SEND] > 0.0f && chunks < 40) {
        r.step();
        ++chunks;
        falling = falling && r.out[P_SEND] <= prev;
        smooth = smooth && prev - r.out[P_SEND] <= kChunk / (0.010 * kRate) + 1e-5;   // a full scale takes 10 ms
        prev = r.out[P_SEND];
    }
    CHECK(falling && smooth);
    CHECK(chunks <= kGlide);   // 10 ms is 14 chunks
    CHECK(r.out[P_SEND] == 0.0f && r.out[P_FB] == 0.5f);
    r.step();
    CHECK(r.atRest());
    // the knob's own value is where it returns to, whatever it became meanwhile
    r.base[P_SEND] = 0.4f;
    r.step();
    CHECK(r.out[P_SEND] == 0.4f && r.atRest());
    // and an Add is held within 0..1: over a knob that is up already it does nothing, a negative one cannot pass 0
    Rig q;
    q.base[P_SEND] = 0.7f;
    q.base[P_FB] = 0.9f;
    q.base[gf::P_DRY] = 0.6f;
    q.m.fire(tb, Moves::kOneShot, Moves::kBar);
    q.run(0.2);
    CHECK(q.out[P_SEND] == 1.0f && q.out[P_FB] == 1.0f);
    q.m.fire(move("Break"), Moves::kOneShot, Moves::kBar);
    q.run(4.5);
    CHECK(q.out[gf::P_DRY] == 0.0f);
}

// Loop goes round until stop(): Break's dry stays out across the laps and its pulse comes again on each bar.
void moveLoop() {
    const int brk = move("Break");
    Rig r;
    r.base[P_SEND] = 0.0f;
    r.m.fire(brk, Moves::kLoop, Moves::kBar);   // beat 0 is a line
    int pulses = 0;
    bool high = false, dryOut = true, playing = true;
    int chunks = 0;
    while (r.beats < 15.9) {
        r.step();
        ++chunks;
        if (r.out[P_SEND] > 0.5f && !high) ++pulses;
        high = r.out[P_SEND] > 0.5f;
        if (r.beats > 0.01) dryOut = dryOut && r.out[gf::P_DRY] == 0.0f;
        playing = playing && r.m.playing() == brk;
    }
    CHECK(pulses == 4);   // beats 0, 4, 8 and 12
    CHECK(dryOut);        // and the dry never comes back between laps
    CHECK(playing);
    r.run(20.0);
    CHECK(r.m.playing() == brk && r.out[gf::P_DRY] == 0.0f);   // 5 laps and on
    // stop(): the ramps glide back over 10 ms and it is over
    r.m.stop();
    CHECK(r.m.playing() == -1);
    float prev = r.out[gf::P_DRY];
    bool rising = true;
    for (int k = 0; k < kGlide; ++k) {
        r.step();
        rising = rising && r.out[gf::P_DRY] >= prev;
        prev = r.out[gf::P_DRY];
    }
    CHECK(rising && r.out[gf::P_DRY] == 1.0f && r.atRest());
    r.run(40.0);   // no new lap
    CHECK(r.atRest() && r.m.playing() == -1);

    // a Loop with a Time pattern: the pattern is the lap's, started on the lap's line and handed over to the next one 5 ms before it
    const int st = move("Stutter 1/16");
    Rig s;
    s.m.fire(st, Moves::kLoop, Moves::kBar);
    s.step();
    CHECK(s.pat == gf::moveDef(st).timePattern && s.loop && s.start == 0.0);
    s.run(2.0);
    CHECK(s.start == 0.0 && s.loop);
    s.run(3.95);
    CHECK(s.start == 0.0);
    s.run(3.9999);
    CHECK(s.start == 4.0 && s.loop);   // the next lap, once its pre-roll begins
    s.run(6.0);
    CHECK(s.start == 4.0 && s.pat == gf::moveDef(st).timePattern);
    s.run(9.0);
    CHECK(s.start == 8.0 && s.m.playing() == st);
    s.m.stop();
    s.step();
    CHECK(s.pat == -1 && !s.loop);

    // a One Shot of the same ends at its length: no pattern, no Move
    Rig o;
    o.m.fire(st, Moves::kOneShot, Moves::kBar);
    o.run(3.9);
    CHECK(o.pat >= 0 && !o.loop && o.start == 0.0 && o.m.playing() == st);
    o.run(4.01);
    CHECK(o.pat == -1 && o.m.playing() == -1);
}

// A Move switches on the parts it needs, for its length (and from 20 ms before its line); a part that is on stays on.
void movePartsOn() {
    const int tb = move("Throw Beat");
    {
        Rig r;
        r.base[gf::P_ECHO_ON] = 0.0f;
        r.beats = 0.3;
        r.m.fire(tb, Moves::kOneShot, Moves::kBeat);   // the line is 1.0, the end 2.0
        bool off = true;
        while (r.beats < 1.0 - kLeadBeats - 2.0 * kChunkBeats) {
            r.step();
            off = off && r.out[gf::P_ECHO_ON] == 0.0f;
        }
        CHECK(off);
        r.run(1.0 - 0.5 * kLeadBeats);   // 10 ms before the line: in already (the part's fade is 10 ms)
        CHECK(r.out[gf::P_ECHO_ON] == 1.0f && r.m.playing() == -1);
        r.run(1.5);
        CHECK(r.out[gf::P_ECHO_ON] == 1.0f && r.m.playing() == tb);
        bool on = true;
        while (r.beats < 1.99) {
            r.step();
            on = on && r.out[gf::P_ECHO_ON] == 1.0f;
        }
        CHECK(on);
        r.run(2.0 + 1.5 * kChunkBeats);   // the Move is over: the knob's value
        CHECK(r.out[gf::P_ECHO_ON] == 0.0f);
        // nothing else was switched
        CHECK(r.out[gf::P_PUMP_ON] == 0.0f && r.out[gf::P_SHAPE_ON] == 0.0f && r.out[gf::P_TIME_ON] == 0.0f);
    }
    {   // on already: on before, during and after
        Rig r;
        r.base[gf::P_ECHO_ON] = 1.0f;
        r.beats = 0.3;
        r.m.fire(tb, Moves::kOneShot, Moves::kBeat);
        bool on = true;
        while (r.beats < 3.0) {
            r.step();
            on = on && r.out[gf::P_ECHO_ON] == 1.0f;
        }
        CHECK(on);
    }
    {   // a Time pattern switches Time on (not the Echo), a Filter Drop the Shape, nothing the Pump
        Rig r;
        r.m.fire(move("Reverse Beat"), Moves::kOneShot, Moves::kBar);
        r.run(0.5);
        CHECK(r.out[gf::P_TIME_ON] == 1.0f && r.out[gf::P_ECHO_ON] == 0.0f && r.out[gf::P_SHAPE_ON] == 0.0f);
        r.run(1.5);
        CHECK(r.out[gf::P_TIME_ON] == 0.0f);
        Rig f;
        f.base[gf::P_FLT_CUTOFF] = 1.0f;
        f.m.fire(move("Filter Drop"), Moves::kOneShot, Moves::kBar);
        f.run(2.0);
        CHECK(f.out[gf::P_SHAPE_ON] == 1.0f && f.out[gf::P_FLT_ON] == 1.0f && f.out[gf::P_PUMP_ON] == 0.0f && f.out[gf::P_TIME_ON] == 0.0f);
        // the cutoff falls 2 octaves over the bar: from 20 kHz to 5 kHz at the end
        f.run(3.998);
        const gf::ParamSpec& sp = gf::PARAM_SPECS[gf::P_FLT_CUTOFF];   // a log parameter: lo (hi / lo) ^ value
        const double hz = sp.lo * std::pow(static_cast<double>(sp.hi) / sp.lo, static_cast<double>(f.out[gf::P_FLT_CUTOFF]));
        CHECK(near(hz, 5000.0, 60.0));
        f.run(4.0 + 1.5 * kChunkBeats);
        CHECK(f.m.playing() == -1);
        f.run(4.2);
        CHECK(f.atRest() && f.out[gf::P_FLT_CUTOFF] == 1.0f && f.out[gf::P_FLT_ON] == 0.0f);
    }
    {   // the filter falls steadily
        Rig f;
        f.base[gf::P_FLT_CUTOFF] = 1.0f;
        f.m.fire(move("Filter Drop"), Moves::kOneShot, Moves::kBar);
        float prev = 2.0f;
        bool falling = true;
        while (f.beats < 3.9) {
            f.step();
            falling = falling && f.out[gf::P_FLT_CUTOFF] <= prev;
            prev = f.out[gf::P_FLT_CUTOFF];
        }
        CHECK(falling && prev < 0.81f && prev > 0.79f);
    }
}

// Dub Freeze holds the Echo's freeze for the bar after its pulse, then restores the option.
void moveSetOption() {
    const int df = move("Dub Freeze");
    Rig r;
    r.base[P_SEND] = 0.0f;
    r.base[P_FB] = 0.5f;
    r.base[gf::P_ECHO_FREEZE] = 0.0f;
    r.m.fire(df, Moves::kOneShot, Moves::kBar);   // line 0; the pulse is beat 0, the freeze beats 1 to 5
    r.run(0.5);
    CHECK(r.out[P_SEND] == 1.0f && near(r.out[P_FB], 0.8, 1e-6) && r.out[gf::P_ECHO_FREEZE] == 0.0f);   // the pulse, not frozen yet
    r.run(0.99);
    CHECK(r.out[gf::P_ECHO_FREEZE] == 0.0f && r.out[P_SEND] == 1.0f);
    r.run(1.01);
    CHECK(r.out[gf::P_ECHO_FREEZE] == 1.0f);   // the option is set at once
    r.run(1.5);
    CHECK(r.out[P_SEND] == 0.0f && r.out[P_FB] == 0.5f && r.out[gf::P_ECHO_FREEZE] == 1.0f);   // the pulse has glided back
    bool held = true;
    while (r.beats < 4.99) {
        r.step();
        held = held && r.out[gf::P_ECHO_FREEZE] == 1.0f && r.out[gf::P_ECHO_ON] == 1.0f && r.m.playing() == df;
    }
    CHECK(held);
    r.run(5.0 + 1.5 * kChunkBeats);   // over: the knob's option is back, in the chunk
    CHECK(r.out[gf::P_ECHO_FREEZE] == 0.0f && r.m.playing() == -1);
    r.run(5.3);
    CHECK(r.atRest());

    // frozen by the knob already: the option is held throughout
    Rig k;
    k.base[gf::P_ECHO_FREEZE] = 1.0f;
    k.m.fire(df, Moves::kOneShot, Moves::kBar);
    bool still = true;
    while (k.beats < 7.0) {
        k.step();
        still = still && k.out[gf::P_ECHO_FREEZE] == 1.0f;
    }
    CHECK(still);
}

// --- the flow ---------------------------------------------------------------------------------------------

// A Move fired while one plays waits for its line; there it replaces the first, whose ramps glide back as the new ones come in.
void moveReplaces() {
    const int tbar = move("Throw Bar"), brk = move("Break"), rev = move("Reverse Beat");
    Rig r;
    r.base[P_SEND] = 0.0f;
    r.base[gf::P_ECHO_ON] = 0.0f;
    r.m.fire(tbar, Moves::kOneShot, Moves::kBar);   // line 0, end 4
    r.run(2.5);
    CHECK(r.m.playing() == tbar && r.out[P_SEND] == 1.0f);
    r.m.fire(brk, Moves::kOneShot, Moves::kBar);    // the next bar is beat 4
    CHECK(r.m.armed() == brk && r.m.playing() == tbar);
    bool still = true;
    while (r.beats < 3.9) {
        r.step();
        still = still && r.m.playing() == tbar && r.m.armed() == brk && r.out[P_SEND] == 1.0f && r.out[gf::P_DRY] == r.base[gf::P_DRY];
    }
    CHECK(still);
    r.run(4.1);
    CHECK(r.m.playing() == brk && r.m.armed() == -1);
    // the Throw's end and the Break's start are one instant: the send does not dip (the Break's pulse is up as the Throw's glides)
    r.run(4.0 + 3.0 * kChunkBeats);
    CHECK(r.out[P_SEND] > 0.9f && r.out[gf::P_DRY] < 0.5f);
    r.run(5.5);
    CHECK(r.out[P_SEND] == 0.0f && r.out[gf::P_DRY] == 0.0f && r.out[gf::P_ECHO_ON] == 1.0f);   // the Break's: its pulse is over, the dry out

    // Echo then Time: the Echo stays on up to the line and goes with the Throw, Time comes in before the line
    Rig q;
    q.beats = 0.3;
    q.m.fire(move("Throw Beat"), Moves::kOneShot, Moves::kBeat);   // 1 to 2
    q.run(1.5);
    q.m.fire(rev, Moves::kOneShot, Moves::kBeat);                 // line 2 (the Throw's end), 1 beat
    q.run(2.0 - kLeadBeats - 2.0 * kChunkBeats);
    CHECK(q.out[gf::P_ECHO_ON] == 1.0f && q.out[gf::P_TIME_ON] == 0.0f && q.pat == -1);
    q.run(2.0 - 0.5 * kLeadBeats);
    CHECK(q.out[gf::P_ECHO_ON] == 1.0f && q.out[gf::P_TIME_ON] == 1.0f);
    q.run(2.2);
    CHECK(q.m.playing() == rev && q.out[gf::P_ECHO_ON] == 0.0f && q.out[gf::P_TIME_ON] == 1.0f && near(q.start, 2.0, 1e-9));

    // a Loop of one Move, another fired over it: the first goes round until the other's line, then the other plays once
    Rig l;
    l.m.fire(move("Stutter 1/8"), Moves::kLoop, Moves::kBar);
    l.run(5.0);
    l.m.fire(move("Gate 1/16"), Moves::kOneShot, Moves::kBar);   // line 8
    l.run(7.9);
    CHECK(l.m.playing() == move("Stutter 1/8") && l.loop && l.start == 4.0);
    l.run(8.1);
    CHECK(l.m.playing() == move("Gate 1/16") && !l.loop && l.start == 8.0);
    l.run(12.1);
    CHECK(l.m.playing() == -1 && l.pat == -1);
}

// stop(): a playing Move glides back, a waiting one never starts, and the next one fires as usual.
void moveStops() {
    const int tbar = move("Throw Bar");
    {
        Rig r;
        r.base[P_SEND] = 0.0f;
        r.m.fire(tbar, Moves::kOneShot, Moves::kBar);
        r.run(1.0);
        CHECK(r.out[P_SEND] == 1.0f);
        r.m.stop();
        CHECK(r.m.playing() == -1 && r.m.armed() == -1);
        float prev = 1.0f;
        bool falling = true;
        for (int k = 0; k < kGlide; ++k) {
            r.step();
            falling = falling && r.out[P_SEND] <= prev;
            prev = r.out[P_SEND];
        }
        CHECK(falling && r.out[P_SEND] == 0.0f && r.atRest());
        r.m.fire(move("Throw Beat"), Moves::kOneShot, Moves::kBeat);   // and again
        int guard = 0;
        while (r.m.playing() != move("Throw Beat") && guard++ < 2000) r.step();
        CHECK(r.m.playing() == move("Throw Beat") && r.out[P_SEND] > 0.0f);
    }
    {   // a Move that waits is cancelled
        Rig r;
        r.beats = 0.3;
        r.m.fire(tbar, Moves::kOneShot, Moves::kBar);
        r.run(1.0);
        CHECK(r.m.armed() == tbar);
        r.m.stop();
        CHECK(r.m.armed() == -1);
        bool rest = true;
        while (r.beats < 12.0) {
            r.step();
            rest = rest && r.atRest() && r.pat == -1;
        }
        CHECK(rest && r.m.playing() == -1);
    }
    {   // a glide in flight is not cut by another stop(); reset() clears at once
        Rig r;
        r.base[P_SEND] = 0.0f;
        r.m.fire(tbar, Moves::kOneShot, Moves::kBar);
        r.run(1.0);
        r.m.stop();
        r.step();
        r.m.stop();
        r.step();
        CHECK(r.out[P_SEND] > 0.0f && r.out[P_SEND] < 1.0f);
        r.m.reset();
        r.step();
        CHECK(r.atRest() && r.m.playing() == -1 && r.m.armed() == -1);
    }
}

// A Move lasts its length in beats played: a locate or a loop point does not move it, a tempo change stretches it, a
// stopped transport runs the grid on at the tempo.
void moveGrid() {
    const int tb = move("Throw Bar");
    {   // a locate backwards in the middle of a Move: it goes on, and so does its pattern's start
        Rig r;
        r.base[P_SEND] = 0.0f;
        r.m.fire(tb, Moves::kOneShot, Moves::kBar);   // 0 to 4
        r.run(2.0);
        CHECK(r.out[P_SEND] == 1.0f);
        r.beats = 100.0;                               // a locate far ahead
        r.run(101.9);
        CHECK(r.out[P_SEND] == 1.0f && r.m.playing() == tb);   // 2 + 1.9 = 3.9 beats into it
        r.run(102.5);
        CHECK(r.m.playing() == -1);                    // and over at 4
        Rig s;
        s.m.fire(move("Reverse Beat"), Moves::kLoop, Moves::kBar);
        s.run(2.0);
        CHECK(s.start <= 2.1);
        s.beats = 50.0;
        s.step();
        CHECK(s.start > 49.0 && s.start <= 50.0 + 0.02);   // its pattern's start moved with the position (this lap's, or the next)
        s.run(53.0);
        CHECK(s.start > 52.0 && s.m.playing() == move("Reverse Beat"));
    }
    {   // a loop point under a waiting Move: the line is the next one in the new place
        Rig r;
        r.beats = 15.0;
        r.m.fire(move("Reverse Beat"), Moves::kOneShot, Moves::kBar);   // line 16
        r.run(15.9);
        r.beats = 0.0;                                  // the loop wraps (at 16, say)
        r.step();
        CHECK(r.m.playing() == move("Reverse Beat") && r.start == 0.0);   // beat 0 is a bar line
    }
    {   // the tempo: a beat of Throw lasts a beat, twice as long at 60 BPM
        Rig r;
        r.bpm = 60.0;
        r.base[P_SEND] = 0.0f;
        r.m.fire(move("Throw Beat"), Moves::kOneShot, Moves::kBar);
        int high = 0;
        for (int k = 0; k < 4000; ++k) {
            r.step();
            if (r.out[P_SEND] == 1.0f) ++high;
        }
        // 44100 samples a beat at 60 BPM, less the glide in (2 chunks) and the glide out (14)
        CHECK(std::abs(high * kChunk - 44100) <= 3 * kChunk);
        // a tempo change in the middle: the beats played count
        Rig q;
        q.base[P_SEND] = 0.0f;
        q.m.fire(move("Throw Bar"), Moves::kOneShot, Moves::kBar);
        q.run(2.0);
        q.bpm = 240.0;
        q.run(3.9);
        CHECK(q.out[P_SEND] == 1.0f && q.m.playing() == move("Throw Bar"));
        q.run(4.01);
        CHECK(q.m.playing() == -1);
    }
    {   // stopped: the position it says is not used, the grid runs on at the tempo
        Rig r;
        r.playing = false;
        r.base[P_SEND] = 0.0f;
        for (int k = 0; k < 40; ++k) r.step();   // the clock runs from 0: 40 chunks on (29 ms: past a press's grace)
        r.m.fire(move("Throw Beat"), Moves::kOneShot, Moves::kBeat);
        int chunks = 0;
        while (r.m.playing() == -1 && chunks < 100000) {
            r.step();
            ++chunks;
        }
        // the next beat is 1: 22050 samples from 0
        CHECK(std::abs((chunks + 40) * kChunk - 22050) <= 2 * kChunk);
        CHECK(r.out[P_SEND] > 0.0f);
        chunks = 0;
        while (r.m.playing() != -1 && chunks < 100000) {
            r.step();
            ++chunks;
        }
        CHECK(std::abs(chunks * kChunk - 22050) <= 2 * kChunk);   // and it lasts its beat
        // the grid it was told is the clock's, from where the stop was: the first stopped chunk started at 0
        Rig q;
        q.playing = false;
        q.step();
        q.step();
        CHECK(near(q.m.gridBeat(), kChunkBeats, 1e-12));
        q.playing = true;
        q.beats = 77.0;
        q.step();
        CHECK(near(q.m.gridBeat(), 77.0, 1e-12));
        q.m.reset();
        q.step();
        CHECK(near(q.m.gridBeat(), q.beats - kChunkBeats, 1e-12));
    }
    {   // playing, then stopped: the Move carries on from where it was
        Rig r;
        r.base[P_SEND] = 0.0f;
        r.m.fire(tb, Moves::kOneShot, Moves::kBar);
        r.run(1.0);
        r.playing = false;
        r.run(2.5);
        CHECK(r.out[P_SEND] == 1.0f && r.m.playing() == tb);
    }
}

// Anything a host or a surface can send: no value out of 0..1, nothing else touched, and the Moves go on.
void moveHostile() {
    {
        Rig r;
        r.m.fire(-1, 0, 0);
        r.m.fire(16, 0, 0);
        r.m.fire(1 << 30, 0, 0);
        r.m.fire(std::numeric_limits<int>::min(), 0, 0);
        CHECK(r.m.armed() == -1);
        r.step();
        CHECK(r.atRest() && r.m.playing() == -1 && r.pat == -1);
        r.m.fire(move("Throw Beat"), 99, -7);   // a mode and a quantize out of range: the nearest
        CHECK(r.m.armed() == move("Throw Beat"));
        r.run(3.0);
        CHECK(r.m.playing() == -1 || r.m.playing() == move("Throw Beat"));
    }
    const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
    const double bpms[] = {120.0, nan, inf, -inf, 0.0, -5.0, 1e9, 19.0, 999.0, 5000.0, 1e-9};
    const double positions[] = {0.0, nan, inf, -inf, 1e300, -1e300, 4.0, 1e15, -3.0};
    // the parameters a Move may touch: the rest stay bit for bit the knobs' own
    bool touched[gf::P_COUNT] = {};
    for (int i = 0; i < gf::numMoves(); ++i) {
        const MoveDef& d = gf::moveDef(i);
        for (int k = 0; k < d.rampCount; ++k) touched[gf::moveRamp(d.rampFirst + k).param] = true;
    }
    touched[gf::P_PUMP_ON] = touched[gf::P_SHAPE_ON] = touched[gf::P_ECHO_ON] = touched[gf::P_TIME_ON] = true;
    bool ok = true, rest = true, patterns = true;
    uint32_t seed = 7;
    for (int mode = 0; mode < 2; ++mode) {
        Rig r;
        for (float& v : r.base) v = 0.5f;
        for (int k = 0; k < 6000; ++k) {
            seed = seed * 1664525u + 1013904223u;
            if (k % 40 == 0) r.m.fire(static_cast<int>((seed >> 8) % 16), mode, static_cast<int>((seed >> 4) % 3));
            if (k % 997 == 500) r.m.stop();
            seed = seed * 1664525u + 1013904223u;
            ef::Transport t;
            t.bpm = bpms[(seed >> 8) % (sizeof bpms / sizeof bpms[0])];
            t.beats = positions[(seed >> 16) % (sizeof positions / sizeof positions[0])];
            t.playing = (seed >> 24) & 1;
            t.valid = ((seed >> 25) & 3) != 0;
            if (k % 3 == 0) {   // mostly sane, so that Moves actually start
                t.bpm = 120.0;
                t.beats = r.beats;
                t.playing = true;
                t.valid = true;
                r.beats += kChunkBeats;
            }
            std::memcpy(r.out, r.base, sizeof r.out);
            int pat = 5;
            double start = 5.0;
            bool loop = true;
            r.m.apply(r.out, t, &pat, &start, &loop, (k % 7 == 0) ? 1 + static_cast<int>(seed % 200) : ((k % 11 == 0) ? -3 : kChunk));
            for (int i = 0; i < gf::P_COUNT; ++i) {
                ok = ok && std::isfinite(r.out[i]) && r.out[i] >= 0.0f && r.out[i] <= 1.0f;
                if (!touched[i]) rest = rest && r.out[i] == r.base[i];
            }
            patterns = patterns && pat >= -1 && pat < gf::numTimePatterns() && std::isfinite(start) && (pat >= 0 || !loop);
            const int pl = r.m.playing();
            ok = ok && pl >= -1 && pl < 16;
        }
    }
    CHECK(ok);
    CHECK(rest);
    CHECK(patterns);
    {   // a NaN in the knobs themselves does not spread: the parameters a Move touches come out within 0..1
        Rig r;
        r.base[P_SEND] = std::numeric_limits<float>::quiet_NaN();
        r.m.fire(move("Throw Beat"), Moves::kOneShot, Moves::kBar);
        r.run(0.5);
        CHECK(std::isfinite(r.out[P_SEND]) && r.out[P_SEND] >= 0.0f && r.out[P_SEND] <= 1.0f);
    }
}

// --- Time -----------------------------------------------------------------------------------------------

// Told what to play, Time plays a Move's pattern from its line: bit for bit the input before it, a stutter's slice
// repeating from the line, the input again when it is over; a Loop goes round on its lap lines.
void moveDrivesTime() {
    const int st = move("Stutter 1/8");
    const size_t N = static_cast<size_t>(13.0 * kBeat);
    Buf in(N);
    for (size_t i = 0; i < N; ++i) in[i] = static_cast<float>(i) / static_cast<float>(N);   // a ramp: every sample says where it is
    // The input played through Time from beat 0 (sample i is at beat i / 22050), the Move fired when the song gets to `fireAt`.
    const auto play = [&](Rig& r, Buf& L, double fireAt, int which, int mode, int quant) {
        gf::TimeCore core;
        CHECK(core.allocate());
        Buf R = L;
        for (size_t i = 0; i < N; i += kChunk) {
            const int n = static_cast<int>(std::min<size_t>(kChunk, N - i));
            if (r.beats >= fireAt && fireAt >= 0.0) {
                r.m.fire(which, mode, quant);
                fireAt = -1.0;
            }
            const ef::Transport t = r.transport();
            r.step(n);
            core.process(&L[i], &R[i], n, t, r.pat, r.start, 0.0);
        }
    };
    const auto at = [](double beats) { return static_cast<size_t>(std::llround(beats * kBeat)); };
    {   // One Shot: the bar after beat 1, then live again
        Rig r;
        Buf L = in;
        play(r, L, 0.3, st, Moves::kOneShot, Moves::kBeat);   // fired at 0.3: the line is 1.0
        bool live = true, slices = true, back = true;
        for (size_t i = 0; i + 400 < at(1.0); ++i) live = live && L[i] == in[i];   // nothing before the line, to the bit
        const size_t slice = at(0.5);
        for (int k = 1; k < 8; ++k)
            for (size_t j = 400; j < slice - 400; j += 7) {
                const size_t x = at(1.0) + static_cast<size_t>(k) * slice + j;
                slices = slices && near(L[x], in[at(1.0) + j], 1e-5);   // the first slice again
            }
        for (size_t i = at(5.0) + 500; i < at(5.0) + 20000; i += 11) back = back && near(L[i], in[i], 1e-6);
        CHECK(live);
        CHECK(slices);
        CHECK(back);
    }
    {   // Loop: the bar again on the lap line, the slice from its new start
        Rig r;
        Buf L = in;
        play(r, L, 0.0, st, Moves::kLoop, Moves::kBar);   // line 0, laps at 4 and 8
        bool lap1 = true, lap2 = true;
        const size_t slice = at(0.5);
        for (size_t j = 400; j < slice - 400; j += 7) {
            lap1 = lap1 && near(L[at(4.0) + j], in[at(4.0) + j], 1e-5) && near(L[at(4.0) + slice + j], in[at(4.0) + j], 1e-5);
            lap2 = lap2 && near(L[at(8.0) + j], in[at(8.0) + j], 1e-5) && near(L[at(8.0) + 3 * slice + j], in[at(8.0) + j], 1e-5);
        }
        CHECK(lap1);
        CHECK(lap2);
    }
}

} // namespace

void movesTests() {
    moveData();
    moveAnchors();
    moveQuantizes();
    moveRampsReturn();
    moveLoop();
    movePartsOn();
    moveSetOption();
    moveReplaces();
    moveStops();
    moveGrid();
    moveHostile();
    moveDrivesTime();
}
