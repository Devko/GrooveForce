// The Echo, Time and the Moves through the plugin (plan 2, Task 11): MPC's tempo and transport, the 16 Move tiles, GO and the Move
// parameter, a Move over a loop wrap, a locate, a tempo change and a stop, the memory that arrives while it plays, two instances
// side by side, a preset or a project loaded during a Move, the Echo a Throw keeps on, the project, and 60 s of everything at
// once. It is the second half of the plugin's suite (test/plugin_test.cpp is the first: the parameters, the presets, the Pump on
// MPC's grid); the fixtures and the helpers both use are in test/host.h. Called by test/main.cpp after pluginTests(); built with
// ASan/UBSan by `make test`, for the Force's CPU under qemu by `make test-arm`.
#include "host.h"
#include "../plugin/engine.h"
#include "../plugin/moves.h"
#include "../plugin/surface.h"

#include <cstdio>
#include <functional>
#include <map>
#include <string>

namespace eft {
namespace {

using namespace gf;

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

    // A flood of taps in one block (200 of them): the last one counts, each replacing the one waiting, and nothing is stuck.
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


// R53: the chain's parameters are worked out (grooveParams: about 8000 ARM instructions) at most once in 128 frames while a
// Move's ramps move, not once a 32-sample chunk. Through the engine alone, called in blocks of 128 and in chunks of 32:
// the first block after a snapshot converts once, an idle engine never, Filter Drop (a ramp that moves every chunk) once a
// block all the way through its four beats, and idle again, with nothing left over, when it has glided out.
void rampsConvertOncePerBlock() {
    std::printf("== a ramping Move: the chain's parameters are worked out once a block\n");
    float n[P_COUNT];
    for (int i = 0; i < P_COUNT; ++i) n[i] = PARAM_INFO[i].def;
    const int filterDrop = moveOf("Filter Drop");
    for (int calls : {kBlock, 32}) {
        Engine e;
        e.setParams(n);
        ef::Transport t;
        t.playing = t.valid = true;
        Buf l(kBlock, 0.1f), r(kBlock, 0.1f);
        int block = 0;
        auto run = [&](int blocks) {
            std::vector<int> converted;
            for (int b = 0; b < blocks; ++b, ++block) {
                const uint32_t was = e.conversions();
                for (int pos = 0; pos < kBlock; pos += calls) {
                    t.beats = 1.0 + (block * kBlock + pos) / static_cast<double>(kBeat);
                    e.render(&l[static_cast<size_t>(pos)], &r[static_cast<size_t>(pos)], calls, t);
                }
                converted.push_back(static_cast<int>(e.conversions() - was));
            }
            return converted;
        };
        const std::vector<int> idle = run(40);
        CHECK(idle[0] == 1 && std::count(idle.begin() + 1, idle.end(), 0) == 39);   // the snapshot, then nothing
        for (int k = 0; k < 2; ++k) {   // a new snapshot is worked out at once, a chunk after the last one too (the gap is for ramps)
            const uint32_t was = e.conversions();
            e.setParams(n);
            e.render(l.data(), r.data(), 32, t);
            CHECK(e.conversions() == was + 1);
        }
        MoveCall fire;
        fire.kind = MoveCall::kFire;
        fire.move = filterDrop;
        e.moveCall(fire);   // beat 1.23 now: the line is beat 2, 173 blocks on; the Move is four beats, 689 blocks
        const std::vector<int> moving = run(900);
        int most = 0, inside = 0, busy = 0;
        for (size_t b = 0; b < moving.size(); ++b) {
            most = std::max(most, moving[b]);
            if (b >= 200 && b < 800) {   // well inside the Move: the ramp moves in every chunk
                ++inside;
                busy += moving[b];
            }
        }
        CHECK(most == 1);                 // never twice in a block
        CHECK(busy == inside);            // and every block of the ramp's way converts once: it keeps up
        CHECK(std::count(moving.end() - 20, moving.end(), 0) == 20);   // glided out: nothing more to work out
    }
}

// A reset (the slot was switched off for a while) in the middle of a Move's ramp: the first chunk after it is the knobs' alone,
// not the ramp's last step held for the 1 to 3 chunks the conversion gate would keep it. Filter Drop three beats into its four,
// a block's frames more by 0, 32, 64 and 96 (the four phases of the gate), reset, one block of noise: the same as an engine
// that only ever had the knobs and was reset too.
void resetConvertsTheKnobs() {
    std::printf("== a reset in the middle of a ramp: the knobs at once\n");
    float n[P_COUNT];
    for (int i = 0; i < P_COUNT; ++i) n[i] = PARAM_INFO[i].def;
    MoveCall fire;
    fire.kind = MoveCall::kFire;
    fire.move = moveOf("Filter Drop");
    for (int extra = 0; extra < kBlock; extra += 32) {
        Engine e;
        e.setParams(n);
        ef::Transport t;
        t.playing = t.valid = true;
        e.moveCall(fire);
        double pos = 1.0;   // beats: the line is beat 2, the Move four beats
        auto render = [&](Engine& x, const Buf& in, int frames) {
            Buf l(in.begin(), in.begin() + frames), r = l;
            t.beats = pos;
            x.render(l.data(), r.data(), frames, t);
            return l;
        };
        for (int b = 0; b < 3 * kBeat / kBlock; ++b, pos += kBlock / static_cast<double>(kBeat)) render(e, whiteNoise(kBlock, 0.5f, 1 + static_cast<uint32_t>(b)), kBlock);
        if (extra) {
            render(e, whiteNoise(extra, 0.5f, 99), extra);
            pos += extra / static_cast<double>(kBeat);
        }
        CHECK(e.litMoves() != 0u);   // (the Move is playing)
        e.reset();
        Engine knobs;
        knobs.setParams(n);
        knobs.reset();
        const Buf in = whiteNoise(kBlock, 0.5f, 777);
        const Buf a = render(e, in, kBlock), b = render(knobs, in, kBlock);
        if (maxDiff(a, b, 0, kBlock) > 1e-6) std::printf("  %d frames into the gate: the first block after the reset differs by %g\n", extra, maxDiff(a, b, 0, kBlock));
        CHECK(maxDiff(a, b, 0, kBlock) < 1e-6);
    }
}

// R53's cost is a ramp that moves in steps of 128 frames (2.9 ms) instead of 32: Filter Drop on a 2 kHz sine, the filter's
// cutoff falling two octaves from 6 kHz over its four beats, follows the low-pass's own response (second order, Butterworth)
// to a percent or so all the way to the end of its ramp, the sine is the sine before and after, and nothing clicks.
void rampKeepsItsWay() {
    std::printf("== Filter Drop: the sweep follows the filter's response, click-free\n");
    const int total = 4 * kBeat * 2 + 4 * kBeat;
    Buf in(static_cast<size_t>(total));
    for (int i = 0; i < total; ++i) in[static_cast<size_t>(i)] = 0.4f * static_cast<float>(std::sin(2.0 * kPi * 2000.0 * i / 44100.0));
    Host h;
    h.play(true);
    h.set(P_FLT_CUTOFF, 6000.0f);
    const Played p = playActs(h, in, {{kPressBlock, [](Host& x) { x.tap(tileOf(moveOf("Filter Drop"))); }}});
    const int line = kLine, len = 4 * kBeat;
    bool follows = true;
    for (double u : {0.1, 0.25, 0.5, 0.75, 0.9, 0.97}) {   // the ramp's whole way, to the last thirtieth of it
        const int at = line + static_cast<int>(u * len);
        const double fc = 6000.0 * std::pow(2.0, -2.0 * u);
        const double want = 0.4 / std::sqrt(1.0 + std::pow(2000.0 / fc, 4.0));
        const double got = magnitude(p.l, 2000.0, static_cast<size_t>(at - 1500), static_cast<size_t>(at + 1500));
        if (std::fabs(got - want) > 0.006) std::printf("  at %.2f of the Move: %.4f, the response says %.4f\n", u, got, want);
        follows = follows && std::fabs(got - want) <= 0.006;
    }
    CHECK(follows);
    CHECK(std::fabs(magnitude(p.l, 2000.0, static_cast<size_t>(line - 5000), static_cast<size_t>(line - 500)) - 0.4) < 2e-3);   // before it
    CHECK(std::fabs(magnitude(p.l, 2000.0, static_cast<size_t>(line + len + 2000), static_cast<size_t>(line + len + 8000)) - 0.4) < 2e-3);   // after
    CHECK(maxStep(p.l, 20000) <= maxStep(in, 20000) * 1.01f);
}

// R54: the calls from the UI thread coalesce. The latest fire, GO or stop is the call; a load is a flag that is never lost,
// and comes first (it ends what played before it). Straight through the surface: forty taps are one call, a load among 20
// taps and 5 more is the load and the last tap, GO takes its Move with it, and GO with None is nothing.
void moveCallsCoalesce() {
    std::printf("== the Move calls from the UI: the latest counts, a load is never lost\n");
    Surface s;
    auto drain = [&s] {
        std::vector<std::pair<int, int>> got;
        MoveCall c;
        while (s.takeMoveCall(c)) got.push_back({c.kind, c.move});
        return got;
    };
    using Calls = std::vector<std::pair<int, int>>;
    auto tap = [&s](int k) { s.set(tileOf(k), 1.0f); };
    CHECK(drain().empty());
    for (int k = 0; k < 40; ++k) tap(k % 16);
    CHECK(drain() == Calls({{MoveCall::kFire, 7}}));                       // the 40th tap: tile 8
    CHECK(drain().empty());
    for (int k = 0; k < 20; ++k) tap(k % 16);
    s.set(P_PRE_INIT, 1.0f);
    for (int k = 0; k < 5; ++k) tap(k + 3);
    CHECK(drain() == Calls({{MoveCall::kLoad, 0}, {MoveCall::kFire, 7}}));   // the load, then the last tap (tile 8)
    for (int k = 0; k < 30; ++k) tap(k % 16);
    s.set(P_PRE_INIT, 1.0f);
    CHECK(drain() == Calls({{MoveCall::kLoad, 0}}));                         // nothing fired before a load is fired after it
    s.set(P_PRE_INIT, 1.0f);
    for (int k = 0; k < 40; ++k) tap(k % 16);
    CHECK(drain() == Calls({{MoveCall::kLoad, 0}, {MoveCall::kFire, 7}}));
    // the Move parameter: None after a fire is a stop; the selection and GO take their Move with them
    tap(4);
    s.set(P_MOVE, 0.0f);   // None
    CHECK(drain() == Calls({{MoveCall::kStop, 0}}));
    tap(5);
    s.set(P_MOVE_GO, 1.0f);
    CHECK(drain() == Calls({{MoveCall::kGo, 5}}));
    s.set(P_MOVE, 0.0f);
    drain();
    s.set(P_MOVE_GO, 1.0f);
    CHECK(drain().empty());   // GO with None selected: no call
    tap(2);
    s.dropMoveCalls();
    CHECK(drain().empty());
    CHECK(s.get(P_MOVE) > 0.0f);   // (a tap selected it all the same)
}

// R54 through the host: a Break plays (the dry is down); in one block 20 taps, a preset load and five taps follow, the last
// on Stutter 1/16. The load is not lost: the Break ends before the Stutter's line, the dry is back a few ms on, while a lost
// load would leave it down until the Stutter's line at beat 2.
void loadAmongCalls() {
    std::printf("== a load among a flood of calls in one block is never lost\n");
    const int total = 60000;
    const Buf in(static_cast<size_t>(total), 0.5f);
    Host h;
    h.play(true);
    const Played p = playActs(h, in, {{100, [](Host& x) { x.tap(tileOf(moveOf("Break"))); }},
                                      {330, [](Host& x) {   // sample 42240, beat 1.916: the next line is beat 2 (44100)
                                           for (int k = 0; k < 20; ++k) x.tap(tileOf(k % 14));
                                           x.press(P_PRE_INIT);
                                           for (int k = 0; k < 4; ++k) x.tap(tileOf(k));
                                           x.tap(tileOf(9));
                                       }}});
    CHECK(peak(p.l, kBeat + 2000, 42240) < 1e-3f);         // the Break was playing
    CHECK(std::fabs(p.l[43500] - 0.5f) < 1e-6f);             // and is over: the dry is back before the Stutter's line
    CHECK(h.value(P_MOVE) == 10.0f);                          // the last tap selected Stutter 1/16
    // GO takes the Move it was pressed for, whatever the engine's snapshot says (it may be a block old): through the engine
    // alone, with nothing selected in it.
    float n[P_COUNT];
    for (int i = 0; i < P_COUNT; ++i) n[i] = PARAM_INFO[i].def;
    Engine e;
    e.setParams(n);
    ef::Transport t;
    t.playing = t.valid = true;
    Buf l(kBlock, 0.1f), r(kBlock, 0.1f);
    e.render(l.data(), r.data(), kBlock, t);
    CHECK(e.litMoves() == 0u);
    MoveCall go;
    go.kind = MoveCall::kGo;
    go.move = 9;
    e.moveCall(go);
    CHECK(e.litMoves() == 1u << 9);
    MoveCall stop;
    stop.kind = MoveCall::kStop;
    e.moveCall(stop);
    CHECK(e.litMoves() == 0u);
}

// R38's cap: a Throw whose Echo would ring for minutes (feedback 0.99: sixty dB is 687 trips) is kept on for twelve seconds at
// most, and then goes off as a switch does: still ringing a moment before, silent a moment after, and no part runs.
void echoHoldIsCapped() {
    std::printf("== a Throw's Echo that never rings out: held for twelve seconds, then off\n");
    const int total = 16 * 44100;
    const int click = kLine + 15435, moveEnd = kLine + kBeat;
    Buf in(static_cast<size_t>(total), 0.0f);
    in[static_cast<size_t>(click)] = 0.5f;
    Host h;
    h.play(true);
    lineAOnly(h, "1/8", 0.99f, 0.0f);
    const size_t ends = static_cast<size_t>(moveEnd + Engine::kMaxHoldSamples);   // (the Move lets go within a chunk of its end)
    float ringing = -1.0f, off = -1.0f;
    const Played p = playActs(h, in, {{kPressBlock, [](Host& x) { x.tap(tileOf(0)); }},
                                      {static_cast<int>(ends - 2 * 44100) / kBlock, [&](Host& x) { ringing = static_cast<float>(partsOn(x)); }},
                                      {static_cast<int>(ends + 2 * 44100) / kBlock, [&](Host& x) { off = static_cast<float>(partsOn(x)); }}});
    CHECK(h.finite && ringing == 1.0f && off == 0.0f);
    CHECK(peak(p.l, ends - 30000, ends - 2000) > 0.1f);   // 0.4 x 0.99 to the 48th trip: still a clear repeat a moment before
    CHECK(peak(p.l, ends + 2000, p.l.size()) == 0.0f);    // and nothing after the Echo has faded off
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
// and stop and start again): every sample that leaves is a number, none above +18 dBFS, the four parts really run (their
// switches are held on, a preset the random presses load or not; everything else is turned at random), and Init afterwards
// is the input again.
void stressEverything() {
    std::printf("== 60 s of everything at once, at random\n");
    freshUserPresets();
    Host h;
    Rng rng(12345);
    const int total = 60 * kBlocksPerSec;
    float worst = 0.0f;
    int saves = 0, threeOn = 0, fourOn = 0, moving = 0, taps = 0;
    h.play(true);
    static const int kParts[] = {P_PUMP_ON, P_SHAPE_ON, P_ECHO_ON, P_TIME_ON};   // never written at random, and on again every block
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
            threeOn += parts >= 3;
            fourOn += parts == 4;
            bool lit = false;
            for (int k = 0; k < 16; ++k) lit = lit || h.get(tileOf(k)) > 0.5f;
            moving += lit;
        }
        if (blk % (4 * kBlocksPerSec) == 0) everything(h);   // (the writes below turn their insides off and on again at random in between)
        for (int id : kParts) h.on(id);                       // a preset a press loaded has them off: they come back at once
        for (int k = rng.below(4); k > 0; --k) {   // parameters: any value, in range or not
            const int id = rng.below(P_COUNT);
            if (std::find(std::begin(kParts), std::end(kParts), id) != std::end(kParts)) continue;
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
    std::printf("  the status line said three parts or more ran in %d of 60 seconds, all four in %d; a Move tile lit in %d (%d taps)\n",
                threeOn, fourOn, moving, taps);
    CHECK(threeOn >= 59 && fourOn >= 58);   // the four parts really were running (all four in every second, bar a Stop's)
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

void pluginMovesTests() {
    using namespace eft;
    setupPluginTests();

    std::printf("== the plugin: the Echo, Time and the Moves\n");
    GF_RUN(mappingEcho);
    GF_RUN(echoThroughMpc);
    GF_RUN(tilesFireMoves);
    GF_RUN(moveParameterFires);
    GF_RUN(moveAcrossLoop);
    GF_RUN(moveWhileStopped);
    GF_RUN(moveReadsThePast);
    GF_RUN(allocateWhilePlaying);
    GF_RUN(twoInstancesRumble);
    GF_RUN(presetDuringMove);
    GF_RUN(echoHoldsAfterThrow);
    GF_RUN(echoHoldIsCapped);
    GF_RUN(rampsConvertOncePerBlock);
    GF_RUN(resetConvertsTheKnobs);
    GF_RUN(rampKeepsItsWay);
    GF_RUN(moveCallsCoalesce);
    GF_RUN(loadAmongCalls);
    GF_RUN(stateWithEcho);
    GF_RUN(moveSuspend);
    GF_RUN(stressEverything);

    cleanupPluginTests();
}

void plugin_movesTests() { pluginMovesTests(); }   // make test-module M=plugin_moves
