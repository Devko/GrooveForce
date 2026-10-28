# GrooveWerk plan 2: Echo, Time and Moves, presets

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Everything the approved design still has open. That is the Echo (Sigmund-style, four lines), Time (swing and grooves on audio, Flex Beat-style patterns) and Moves (16 one-tap actions), plus the factory presets by job, level-matched, and the docs that make it 0.0.1-ready. The fixes and follow-ups plan 1 left open go in too.

**Architecture:** New DSP lives in `dsp/` with the module contract.
- **Echo** is four `EchoLine`s (EffectWerk's Delay grown a filter, a modulator and a read/write split, so the lines can feed each other) wired by a routing.
- **Time** is a recorder ring and a playhead that never runs ahead of now. Its patterns are data in `surface.py`, as the shapes are.
- **The chain** (`Groove`) gains a live wet bus, the Echo's order, Time's place and the key that Pump listens to.
- **Moves** live in the plugin (`plugin/moves.*`). They are parameter ramps plus a Time pattern, fired on the grid by tiles or by the automatable Move parameter, and played in the Engine on the parameters' 0..1 values (EffectWerk's scene moves).

**Tech Stack:** As plan 1: C++17, the siblings' VST2 glue, Python 3 (`surface.py`; Pillow for the skin), GNU make in WSL, qemu-arm, GitHub Actions.

**Spec:** [`docs/DESIGN.md`](../DESIGN.md), its Plan steps 3, 4 and 5. Plan 1 ([2026-10-25-groundwork-pump-shape.md](2026-10-25-groundwork-pump-shape.md)) built steps 1 and 2. Its Global Constraints hold unchanged, plus the ones below.

## Decisions made for this plan

The user asked to "do all at once, implement all that is open", 2026-10-26. These are Claude's calls, recorded in the ledger as rulings R24-R30.

- **R24, Moves without the probe.** The device probe hasn't run. Moves get **One Shot** and **Loop**. **Hold** and MIDI-triggered Moves and Pump wait for the probe's answers (design, Open). The Move parameter is automatable either way.
- **R25, swing on audio** (the design's second "Your part", handed over). **Jump and crossfade**: the playhead jumps back by the swing delay at the late 16th's straight line, and forward again at the next pair's line. Each jump is a 5 ms raised-cosine crossfade that *ends on* the grid line, so a hit on the line is never heard early. No pitch bend.
- **R26, the key.** The Pump's Input trigger always listens to the chain's input (after the input gain, before any part), whatever the Pump's target. Rumble (Pump on Echo, Input) hears the dry kick, not the echo it ducks.
- **R27, polyrhythm times.** "3/16" and "3/8" are already EffectWerk's "1/8." and "1/4.". The Echo's list adds 5/16, 7/16 and 5/8, sorted by length.
- **R28, tile order.** The 16 Move tiles hold the 16 factory Moves in a fixed order. Picking the selected Move, its mode and its quantize is saved in a preset. Re-ordering tiles waits.
- **R29, dry level.** "Break" (the dry drops out, the echo rings) needs a Dry level. A `dry` parameter (0..1, default 1) on SETUP scales the dry bus, and Moves can ramp it.
- **R30, the same parallel waves as plan 1.** A task starts when its dependencies are implemented, in its own worktree. Its own review gates its merge.

## Global Constraints (in addition to plan 1's, which all hold)

- **Echo:** four lines A-D, each stereo, each holding 1 bar at 80 BPM (3 s) plus the wow's depth. The minimum time is 1 ms, more than a 32-sample chunk, so lines can read all their outputs before writing any input (Ring). Every gain in the loop is ≤ 1 and the in-loop limiter keeps writes under 0 dBFS, so feedback 1 holds and never runs away. Lines are allocated the first time the Echo is switched on (UI thread, handed over through an atomic pointer, as EffectWerk's looper does). Until then the Echo passes nothing and costs nothing.
- **Time:** zero latency. The playhead reads at or behind now, never ahead. Every jump is a 5 ms raised-cosine crossfade that ends on its grid line. The ring holds 2 bars at 80 BPM (6 s) and is allocated lazily like the Echo's lines. Time on with Groove Off and no pattern passes its input bit for bit (it only records).
- **Swing once** (design, "combining" 4, the plan 1 ledger's ruling): `Groove::stepSwing` returns straight (0.5) for Shape's and Pump's steps when Time is on, its Groove is Swing and its place is Post. Otherwise it returns the global Swing.
- **All on two buses:** with the wet sounding, a part aimed at All acts on the dry and the wet identically (as if on their sum). Echo-targeted parts act on the wet only.
- **Budget** (p99 on the Force, the device bench): everything on at its heaviest ≤ 15%. That means four lines in Ring with in-loop filters, drive and modulators, three Shape lanes and a banded Input Pump on All over both buses, and Time playing a Move. Targets: Echo everything on ≤ 6%, Time playing a Move ≤ 1.5%, a typical preset ≤ 4%.
- **Pages:** five tab groups, in order: GROOVE (GROOVE, PRESETS), PUMP+SHAPE (PUMP, SHAPE), ECHO (ECHO, LINE A, LINE B, LINE C, LINE D), TIME (TIME, MOVES), SETUP. Parameters append after plan 1's 100 (`params.lock`).
- Every commit message ends with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Test suites: a module `foo_bar` has `test/foo_bar_test.cpp` with `void fooBarTests()` (called from test/main.cpp) plus a one-line `foo_barTests()` alias for `make test-module M=foo_bar`, as `test/pump_curves_test.cpp` does. A module that needs other sources to link gets its `MOD_DEPS_<module>` line in the Makefile.

## Review Focus

1. **The Echo at feedback 1 with drive and the Ring routing** stays bounded for minutes and never NaNs. Test: Task 6 `echoRingBounded`.
2. **A Move fired across a loop wrap or a locate** finishes or re-anchors cleanly: no stuck playhead, no click, the playhead back at now. Test: Task 11 `moveAcrossLoop`.
3. **Switching the Echo or Time on for the first time while playing** (the allocation hand-over) neither clicks nor reads uninitialised memory. Tests: Task 8 `firstSwitchOn`, Task 11 `allocateWhilePlaying`.
4. **Two instances, one pumping its echo on the Input trigger,** both stay on MPC's grid and the key never mixes their states. Test: Task 11 `twoInstancesRumble`.
5. **Preset changes while a Move plays** end the Move (the knobs it ramps return) with no click and no stale ramp. Test: Task 11 `presetDuringMove`.

---

## File structure (new or changed)

| Path | Responsibility | Task |
|---|---|---|
| `dsp/slew.h` | One slew helper (`toward`) for every part; replaces pump.cpp's, groove.cpp's and shape.cpp's copies | 4 |
| `dsp/echo_line.{h,cpp}` | One stereo delay line: time, feedback, mode, filter, drive, wow, glide, limiter; read/write split | 1 |
| `dsp/echo_mod.h` | A line's modulator: LFO (a library shape), Env (attack-decay fired by onsets), Follow | 2 |
| `dsp/echo.{h,cpp}` | The Echo: four lines, routing, send, duck, freeze, width, wet level, tone and feedback offsets, allocation | 6 |
| `dsp/time.{h,cpp}` | Time: recorder ring, playhead, crossfaded jumps, Groove (Swing, Drunk, Push), pattern playback | 3, 7 |
| `dsp/pump.*`, `dsp/shape.*` | The key input; seeding the grid from MPC's stopped position | 4 |
| `dsp/groove.*` | The live wet bus, All on two buses, Echo order, Time place, dry level, stepSwing, the key | 8 |
| `plugin/moves.{h,cpp}` | Moves: definitions, firing on the grid, modes, ramps on 0..1 values, parts switched on | 9 |
| `plugin/engine.*`, `groove_map.*`, `surface.*`, `plugin.cpp` | Echo and Time parameters, Moves in the engine, tiles and Go, allocation on the UI thread | 10, 11 |
| `surface/surface.py` | The new parameters, ECHO and TIME pages, `TIME_PATTERNS` and `MOVES` as data (generated `time_data.h`, `move_data.h`) | 3, 10 |
| `tools/levels.cpp`, `tools/reference_mix.h` | Level-matching the factory presets per category (`make levels`, `make preset-levels`) | 5 |
| `tools/bench.cpp`, `tools/pgo_train.cpp` | Echo, Time and Move cases; knob sweeps and stop/locate in the trainer | 12 |
| `presets/Factory/*` | The factory set by job, level-matched | 13 |
| `docs/USER_GUIDE.md`, `README.md`, `CHANGELOG.md`, `docs/PERFORMANCE.md`, `docs/DESIGN.md` | The user guide, 0.0.1's notes, device numbers, the design record updated | 14 |

**Waves.**
- **A, in parallel:** Tasks 1, 2, 3, 4 and 5.
- **B:** Task 6 (after 1 and 2) and Task 7 (after 3).
- **C:** Task 8 (after 4, 6 and 7) and Task 10 (after 3; written against the interfaces below).
- **D:** Task 9 (after 7 and 10), then Task 11 (after 8, 9 and 10).
- **E:** Task 12 (after 11), Task 13 (after 5 and 11) and Task 14 (after 11) in parallel.
- **Last:** Task 15 on the Force with the user.

---

### Task 1: One echo line

**Files:**
- Create: `dsp/echo_line.h`, `dsp/echo_line.cpp`
- Test: `test/echo_line_test.cpp` (`echoLineTests()`)

**Interfaces:**
- Consumes: EffectWerk's `dsp/delay.{h,cpp}` (D:\DEV\EffectWerk, read-only) as the starting point: Tape/Fade glide, the cubic drive, the limiter, wow, Hermite reads, the cheap reset. Also `dsp/svf.h`.
- Produces:

```cpp
namespace gf {
inline constexpr ef::Division kEchoDivs[] = {   // sorted by length: a Q-Link turn walks them in order (R27)
    {"1/64", 0.0625}, {"1/32T", 1.0 / 12}, {"1/32", 0.125}, {"1/16T", 1.0 / 6}, {"1/16", 0.25}, {"1/8T", 1.0 / 3},
    {"1/16.", 0.375}, {"1/8", 0.5}, {"1/4T", 2.0 / 3}, {"1/8.", 0.75}, {"1/4", 1.0}, {"5/16", 1.25},
    {"1/2T", 4.0 / 3}, {"1/4.", 1.5}, {"7/16", 1.75}, {"1/2", 2.0}, {"5/8", 2.5}, {"1/2.", 3.0}, {"1 bar", 4.0}};
class EchoLine {
public:
    enum Mode : int { kStereo, kPingPong, kMono };
    enum FilterType : int { kLP, kBP, kHP };
    enum Place : int { kInLoop, kOnOutput };
    enum Glide : int { kTape, kFade };
    struct Params {
        bool sync = true; double divBeats = 0.75; float timeMs = 375.0f;   // 1..2000 free
        float feedback = 0.4f;          // 0..1: 1 holds
        int mode = kStereo;
        int filterType = kLP; float cutoffHz = 20000.0f; float resonance = 0.0f; int place = kInLoop;
        float drive = 0.0f, wow = 0.0f; int glide = kTape;
        float timeMod = 1.0f;           // a factor on the time this chunk (the modulator's), glided by the glide
        float cutoffModOct = 0.0f;      // octaves added to the cutoff this chunk
    };
    EchoLine();                          // no allocation
    bool allocate();                     // UI thread: 3 s + wow per side; false if out of memory
    bool allocated() const;
    void reset();
    void set(const Params& p, const ef::Transport& t);
    // Phase 1: the line's output for the next n samples (n <= kChunk), read from what was written >= 1 ms ago.
    void read(float* outL, float* outR, int n);
    // Phase 2: write n samples of input; the line adds its own feedback (from the read) inside.
    void write(const float* inL, const float* inR, int n);
    int tailSamples() const;
};
}
```

The loop, per side:
- **Read:** `read → (In loop: filter) → output`.
- **Write:** `input + feedback × (In loop: the filtered read; On output: the plain read) → drive → limiter → buffer`.
- **On output:** the filter shapes only what is heard.
- **Always in the loop:** a fixed one-pole low cut at 30 Hz.
- **Ping-Pong:** the mono sum of the input enters L only; L's read feeds R's write and R's feeds L's, as EffectWerk's.

- [ ] **Step 1: Failing tests.**

```cpp
void lineTime() {        // an impulse comes back at 1/8. at 120 BPM (16537.5 samples, ±1) with level 1; free 375 ms the same
}
void lineFeedback() {    // feedback 0.5: repeat n at 0.5^(n-1) (±0.01 dB with filter off); feedback 1 holds 30 s without growing past 0 dBFS
}
void lineInLoop() {      // LP 1 kHz in the loop: a 4 kHz tone loses ≥ 12 dB more per repeat than a 250 Hz one; On output: every repeat the same
}
void linePingPong() {    // an impulse in mono: repeats alternate L, R, L at the line's time
}
void lineModes() {       // Mono: L == R; Stereo: L and R independent
}
void lineGlide() {       // a time change: Tape bends (a 1 kHz tone's repeat shifts in pitch while gliding), Fade doesn't (no pitch shift, a 50 ms crossfade); no click either way
}
void lineDriveBounded() {// drive 1, feedback 1, a full-scale square for 20 s: every write ≤ 0 dBFS, output finite
}
void lineReadWrite() {   // a line whose input is its own read (Ring of one) at 1 ms: stable and identical to feedback through write()
}
void lineCheapReset() {  // reset() then a new impulse: nothing from before ever comes out
}
void lineNan() {         // NaN in the input: finite output, the loop never takes it
}
```

- [ ] **Step 2: Run them.** Run `make test-module M=echo_line` (add `MOD_DEPS_echo_line` to the Makefile). Expected: fails to compile.
- [ ] **Step 3: Implement.** Start from EffectWerk's Delay. Replace its two one-pole cuts with the SVF filter and the fixed 30 Hz low cut. Split process into `read` and `write`, and drop its ducking (that becomes the Echo's).
- [ ] **Step 4: Run** `make test-module M=echo_line && make test && make test-arm`. Expected: `0 failed`.
- [ ] **Step 5: Commit** — `Echo: one line, read and write apart`.

---

### Task 2: A line's modulator

**Files:**
- Create: `dsp/echo_mod.h`
- Test: `test/echo_mod_test.cpp` (`echoModTests()`)

**Interfaces:**
- Consumes: `shapeAt` (dsp/shapes.h), `TransientDetector` (dsp/transient.h), `toStraight` and `Swing` (dsp/grid.h). Also EffectWerk's `EnvFollower` (D:\DEV\EffectWerk\dsp\mod.h) for Follow.
- Produces:

```cpp
namespace gf {
class EchoMod {
public:
    enum Mode : int { kOff, kLfo, kEnv, kFollow };
    struct Params { int mode = kOff; int shape = 0; double rateBeats = 1.0; float sensitivity = 0.5f; Swing swing; };
    void reset();
    // One value 0..1 for the chunk of n samples starting now. key: the chain's input (R26), for Env and Follow.
    float next(const Params& p, const ef::Transport& t, const float* keyL, const float* keyR, int n);
};
}
```

- **LFO:** `shapeAt(shape, toStraight(beats)/rateBeats)`, on MPC's grid, running on at the tempo when stopped.
- **Env:** an onset (the detector at `sensitivity`) sets the value to 1, and it decays over `rateBeats` (exponential, -60 dB at the end).
- **Follow:** the key's level, attack 5 ms and release 150 ms, scaled so -6 dBFS reads 1.

- [ ] **Step 1: Failing tests.** `modLfoOnGrid` (a Saw Down at 1/4 equals the shape on the song position, ±1e-4), `modEnvFires` (a kick every beat: 1 within 2 ms after each, below 0.001 by the next at a 1/4 decay), `modFollow` (a -6 dBFS sine reads 1 ±0.05, silence 0 within 1 s), `modOff` (always 0), `modStopped` (stopped: the LFO runs on at the tempo).
- [ ] **Step 2: Run** `make test-module M=echo_mod`. Expected: fails to compile.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `make test-module M=echo_mod && make test && make test-arm`. Expected: `0 failed`.
- [ ] **Step 5: Commit** — `Echo: a line's modulator`.

---

### Task 3: Time's engine and its patterns

**Files:**
- Create: `dsp/time.h`, `dsp/time.cpp` (the ring, the playhead and pattern playback; Task 7 adds the Groove modes)
- Modify: `surface/surface.py` (`TIME_PATTERNS`, its checks, `time_data_header()` → `surface/build/time_data.h`; the Makefile's GEN)
- Test: `test/time_test.cpp` (`timeTests()`)

**Interfaces:**
- Consumes: EffectWerk's looper (D:\DEV\EffectWerk\dsp\looper.{h,cpp}, read-only) for the ring, Hermite reads, the 5 ms raised-cosine crossfade with the old head playing past the edge, and the level fading with speed.
- Produces:

```cpp
namespace gf {
enum class TSeg : uint8_t { Line, Smooth, Jump };   // to the next point; Jump: the source jumps there (crossfaded, ending at x)
struct TPoint { float x, y; TSeg seg; };            // x in beats from the pattern's start; y: source (beats) or volume (0..1)
struct TimePatternDef { const char* name; float lengthBeats; bool loopable; int srcFirst, srcCount, volFirst, volCount; };
int numTimePatterns();
const char* timePatternName(int i);
class TimeCore {
public:
    TimeCore();
    bool allocate();   // UI thread: the 6 s ring
    bool allocated() const;
    void reset();
    // The playhead's source for this chunk: `lag` beats behind now (>= 0), or a pattern at x beats from its start.
    // Jumps crossfade over 5 ms ending on the line where they happen.
    void process(float* L, float* R, int n, const ef::Transport& t, int pattern, double patternStart, double lagBeats);
};
}
```

A pattern's source curve `s(x)` is where the playhead reads, in beats from the pattern's start, with `s(x) ≤ x` (never ahead of now). It may go negative, reading audio from before the pattern began (the ring's reach).

`TIME_PATTERNS` in surface.py holds about 14 entries, built with helper functions:
- Stutter 1/8 and Stutter 1/16 (1 bar, loopable).
- Roll Build 1 Bar (1/4, 1/8, 1/16 then 1/32 within a bar) and Roll Build 4 Bars (1/4 → 1/32 over 4 bars).
- Half-Time Bar (s = x/2).
- Tape Stop (1 bar: the speed falls 1 → 0, the volume fades over the last 1/8).
- Slow Down (1 bar: the speed falls to 0.5).
- Reverse Beat (1 beat: the beat before plays backwards).
- Spin-Back (1/2 beat: backwards, the speed rising, fading out).
- Rewind (1 beat: backwards at twice the speed).
- Gate 1/16 (1 bar: volume steps 1/16 open 60%).
- Break Silence (1 bar: the volume 0).

The checks: `s(x) ≤ x` everywhere, x ascending, the volume within 0..1, and a pattern that isn't loopable ending at `s = x` (rejoining now) or with the volume at 0.

- [ ] **Step 1: Failing tests.**

```cpp
void timeRest() {          // no pattern, lag 0: output == input bit for bit while the ring records
}
void timeLag() {           // lag 1/16 at 120 BPM: an impulse comes out 125 ms later (±1 sample), the switch crossfaded
}
void timeJumpEndsOnLine() {// a jump at x = 1 beat: its 5 ms crossfade ends exactly at the beat (the old head silent from there)
}
void timeStutter() {       // Stutter 1/16: each 1/16 of the bar repeats the bar's first 1/16 (correlation > 0.99 away from the crossfades)
}
void timeHalfTime() {      // Half-Time Bar: a 1 kHz tone plays at 500 Hz (±2 Hz)
}
void timeReverse() {       // Reverse Beat: a rising ramp of the beat before comes out falling
}
void timeTapeStop() {      // Tape Stop: the pitch falls continuously, silent at the end, then rejoins now with no click
}
void timeNeverAhead() {    // every pattern, every sample: the source ≤ now (an instrumented build or a reading check)
}
void timeNoClicks() {      // every pattern on a sine: the noClicks rule over its jumps
}
void timeAllocate() {      // before allocate(): input passes through untouched, no reads
}
```

- [ ] **Step 2: Run** `make surface && make test-module M=time`. Expected: fails to compile.
- [ ] **Step 3: Implement** the evaluator (`s` and the volume at x, the next jump within a chunk) and TimeCore.
- [ ] **Step 4: Run** `make test-module M=time && make test && make test-arm`. Expected: `0 failed`.
- [ ] **Step 5: Commit** — `Time: the ring, the playhead and its patterns`.

---

### Task 4: The key, seeded grids, one slew

**Files:**
- Create: `dsp/slew.h` (`inline float toward(float cur, float target, float step)`, the one copy)
- Modify: `dsp/pump.{h,cpp}`, `dsp/shape.{h,cpp}`, `dsp/groove.cpp` (use `slew.h`; the key)
- Test: `test/pump_test.cpp`, `test/shape_test.cpp` (new checks)

**Interfaces:**
- Produces: `void Pump::process(float* L, float* R, int n, const float* keyL, const float* keyR)`. The Input trigger listens to the key (R26) through the existing 150 Hz low-pass. Null keys mean the Pump's own input, so the old call keeps working.
- Produces: Pump and Shape seed their grid from `t.beats` on the first `set()` after `reset()` when `t.valid`, stopped or playing (plan 1's ruling R21).

- [ ] **Step 1: Failing tests.** `pumpKey`: Input trigger, the audio a steady pad, the key a kick track: it ducks on the key's kicks. `pumpSeededStopped`: reset while stopped at beat 9.5, a 1/4 rate, so the first bottom is at beat 10 (not 0.5 later). `shapeSeededStopped`: the same for a lane.
- [ ] **Step 2: Run them.** Expected: fail.
- [ ] **Step 3: Implement.** Replace the three slew copies with `slew.h`.
- [ ] **Step 4: Run** `make test && make test-arm`. Expected: `0 failed`.
- [ ] **Step 5: Commit** — `Pump and Shape: the key, a grid seeded while stopped, one slew`.

---

### Task 5: Level-matching tools

**Files:**
- Create: `tools/levels.cpp` (EffectWerk's tools/levels.cpp and tools/loudness.h, adapted)
- Modify: `tools/reference_mix.h` (the parts by category; drop MasterWerk's notes and the `efl` namespace for `gfl`), Makefile (`levels`, `preset-levels`, as EffectWerk's)
- Test: `make levels` on the current 10 presets

The reference material by category is the same four bars at 120 BPM throughout, at -18 LUFS:

| Category | Reference material |
|---|---|
| Kick | The kick alone |
| Bass | The bass line alone |
| Hats + Perc | The hats and snare |
| Chords | The chords |
| Drum Bus | Kick, snare and hats |
| Perform, Echo, Utility | The full mix |

`preset-levels` writes each preset's `output=` line so it comes out as loud as it went in (within ±9 dB), and regenerates.

- [ ] **Step 1:** Port the tools. `make levels` prints each preset's in/out loudness.
- [ ] **Step 2:** Run `make levels`. Expected: a table for the 10 presets, Init at 0.0 dB.
- [ ] **Step 3: Commit** — `Tools: level-matching by category`.

---

### Task 6: The Echo

**Files:**
- Create: `dsp/echo.h`, `dsp/echo.cpp`
- Test: `test/echo_test.cpp` (`echoTests()`)

**Interfaces:**
- Consumes: `EchoLine` (Task 1), `EchoMod` (Task 2).
- Produces:

```cpp
namespace gf {
class Echo {
public:
    static constexpr int kLines = 4;
    enum Routing : int { kParallel, kSerial, kPairs, kRing, kSpread };
    struct Line {
        bool on = false; float level = 1.0f; float pan = 0.0f;   // pan -1..1, constant power
        EchoLine::Params p;
        EchoMod::Params mod;
        float modCutoff = 0.0f;   // -1..1: ±4 octaves at the modulator's swing
        float modTime = 0.0f;     // -1..1: ±2% of the time
        float modLevel = 0.0f;    // 0..1: the level dips to 1 - modLevel at the modulator's 0
    };
    struct Params {
        Line line[kLines];
        int routing = kParallel;
        float send = 1.0f;          // 0..1: the input into the lines
        float duck = 0.0f;          // 0..1: the wet ducks under the key (EffectWerk's law)
        bool freeze = false;        // feedback to 1 and the send closed, glided 20 ms
        int glide = EchoLine::kTape;
        float width = 1.0f;         // 0..1.5: M/S width of the wet
        float level = 1.0f;         // 0..1: the wet into the chain
        float feedbackOffset = 0.0f;// -1..1 added to every line's feedback (clamped 0..1)
        float toneOct = 0.0f;       // -4..4 octaves on every line's cutoff
        float sensitivity = 0.5f;   // the Env modulators' detector
        Swing swing;                // the LFOs' grid (Groove::stepSwing)
    };
    Echo();
    bool allocate();   bool allocated() const;   // UI thread; until allocated, the wet is silent and nothing runs
    void reset();
    void set(const Params& p, const ef::Transport& t);
    // in: the send's source; key: the chain's input; wet out (n <= kChunk)
    void process(const float* inL, const float* inR, const float* keyL, const float* keyR, float* wetL, float* wetR, int n);
    int tailSamples() const;
};
}
```

The routings feed each line's input from the send `x` and the lines' reads `y`:

| Routing | Inputs |
|---|---|
| Parallel | Each line gets `x` |
| Serial | `A ← x`, `B ← yA`, `C ← yB`, `D ← yC` |
| Pairs | `A ← x`, `B ← yA`, `C ← x`, `D ← yC` |
| Ring | `A ← x + yD`, `B ← yA`, `C ← yB`, `D ← yC` |
| Spread | `A ← x`, then `B`, `C` and `D ← yA` |

In every routing, each line that is on is heard at its level and pan. Per chunk, every line reads first, then the routing writes. A routing change crossfades the inputs over one chunk. Lines that are off are neither read nor written.

- [ ] **Step 1: Failing tests.** `echoRests` (defaults: wet silent, no work), `echoParallel` (A at 1/8 and B at 5/16 give impulses at both times), `echoSerial` (B's first repeat at A + B), `echoRingBounded` (Ring, feedback 1 everywhere, drive 1, full-scale noise 60 s: finite, every sample ≤ 4 (four lines at 0 dBFS), never NaN — Review Focus 1), `echoPolyrhythm` (3/16 and 5/16 panned ±0.7: the left and right repeats at their times), `echoDuck`, `echoFreeze` (frozen: the lines hold, the send closed; 20 ms glides), `echoTone` (toneOct -2 darkens every line), `echoFeedbackOffset`, `echoModEnv` (Env on cutoff: repeats brighter after each kick), `echoAllocate` (before allocate(): silent, no reads), `echoNoClicks` (routing, mode, freeze, time and every switch).
- [ ] **Step 2: Run** `make test-module M=echo`. Expected: fails to compile.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `make test-module M=echo && make test && make test-arm`. Expected: `0 failed`.
- [ ] **Step 5: Commit** — `Echo: four lines, routings, modulators`.

---

### Task 7: Time's grooves

**Files:**
- Modify: `dsp/time.{h,cpp}` (the `Time` part around TimeCore)
- Test: `test/time_test.cpp`

**Interfaces:**
- Produces:

```cpp
namespace gf {
class Time {
public:
    enum Groove : int { kOff, kSwing, kDrunk, kPush };
    struct Params {
        int groove = kOff;
        float amount = 0.5f;        // Drunk: up to 15 ms late per 16th; Push: up to 20 ms late
        Swing swing;                // Swing's amount and unit (the global Swing)
        int pattern = -1;           // a Move's pattern, -1 none
        double patternStart = 0.0;  // beats where it started (a grid line)
        bool loop = false;          // repeat the pattern until told otherwise
    };
    Time();  bool allocate();  bool allocated() const;
    void reset();  void set(const Params& p, const ef::Transport& t);  void process(float* L, float* R, int n);
    int tailSamples() const;
};
}
```

- **Swing (R25):** each pair of units is `[c, c + 2u)` and the late unit starts at `c + 2u·amount`. Before the late unit's straight line `c + u`, the lag is 0. From it, the lag is `2u·amount − u`. At `c + 2u` it is 0 again. Each change is a jump whose crossfade ends on that line.
- **Drunk:** a lag per 16th from a hash of the 16th's index (0..amount·15 ms), so it repeats wherever MPC loops.
- **Push:** a constant lag of amount·20 ms.
- **A pattern** overrides the groove while it plays.

- [ ] **Step 1: Failing tests.**
  - `timeSwing`: at 66% on 1/16, 120 BPM, a click on every 16th. The odd 16ths come out 40 ms late (±1 sample), the even ones on time, and no click is heard early.
  - `timeDrunkRepeats`: Drunk under a 1-bar loop, bar 2 equals bar 1.
  - `timePush`: 20 ms late at amount 1.
  - `timeGrooveOff`: Off is bit for bit.
  - `timePatternOverGroove`: a pattern plays over Swing, then Swing resumes on the next pair.
- [ ] **Step 2: Run them.** Expected: fail.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `make test-module M=time && make test && make test-arm`. Expected: `0 failed`.
- [ ] **Step 5: Commit** — `Time: swing, drunk and push on the audio`.

---

### Task 8: The chain grows

**Files:**
- Modify: `dsp/groove.{h,cpp}`
- Test: `test/groove_test.cpp`

**Interfaces:**
- Consumes: `Echo` (6), `Time` (7), the Pump's key (4).
- Produces: `Groove::Params` gains the fields below; `Groove::allocate()` (UI thread) allocates the parts that are on; `running()` counts four parts.

```cpp
bool echoOn = false;  Echo::Params echo;  int order = kEchoFirst;   // enum Order { kEchoFirst, kEchoLast }
bool timeOn = false;  Time::Params time;  int timePlace = kPost;    // enum Place { kPost, kPre }
float dry = 1.0f;                                                   // 0..1 (R29)
```

The path:

```
input gain → [Time, Pre] → key (the chain's input) ─┐
  Echo first: dry ─┬───────────────── Shape → Pump (each on All / Dry / Echo) ─┬→ [Time, Post] → output gain → mix
                   └→ send → Echo → wet ─────────────────────────────────────────┘
  Echo last: Shape → Pump on the dry → send → Echo → wet; out = dry + wet (targets ignored)
```

- All on two buses: the part runs on the dry and on the wet. Shape has two instances with the same parameters and grid. The Pump has two instances fed the same key, so their gains are identical.
- `dry` scales the dry bus.
- `stepSwing`: 0.5 when `timeOn && time.groove == kSwing && timePlace == kPost`; otherwise the global Swing.
- Echo's and Time's switches fade like the others.
- The wet's tail counts in `tailSamples()`.

- [ ] **Step 1: Failing tests.**
  - `grooveEchoBus`: Pump depth 1 on Echo ducks only the repeats, and the dry is bit for bit.
  - `grooveAllBothBuses`: Pump on All equals ducking dry + wet.
  - `grooveOrder`: Echo last, the echo hears the gated signal.
  - `grooveTimePlace`: Post, a tape stop stops the echo too; Pre, the echo rings on.
  - `grooveStepSwingRule`: the four cases of the rule.
  - `grooveRumbleKey`: Pump on Echo, Input; the ducks follow the dry kicks.
  - `grooveDry`: dry 0 leaves only the wet.
  - `firstSwitchOn` (Review Focus 3): Echo and Time switched on mid-play after allocate(). Fades in, no click, nothing uninitialised read.
  - `grooveRests`: everything off is bit for bit, as before.
- [ ] **Step 2: Run them.** Expected: fail.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `make test && make test-arm`. Expected: `0 failed`.
- [ ] **Step 5: Commit** — `The chain: the Echo's bus, Time's place, the key`.

---

### Task 9: Moves

**Files:**
- Create: `plugin/moves.h`, `plugin/moves.cpp`
- Modify: `surface/surface.py` (`MOVES` as data → `surface/build/move_data.h`, checked: every ramp names a sound parameter, every pattern exists, 16 Moves)
- Test: `test/moves_test.cpp` (`movesTests()`)

**Interfaces:**
- Consumes: `numTimePatterns` / `timePatternName` (3), `param_ids.h` (10's parameters).
- Produces:

```cpp
namespace gf {
struct MoveRamp { int param; uint8_t kind; float amount; uint8_t shape; };   // kind: Add (to 0..1) | Set (an option);
                                                                            // shape: Rise (0→1 over the Move) | Hold | Fall
struct MoveDef { const char* name; int timePattern; float lengthBeats; int rampFirst, rampCount; int partsOn; };   // partsOn: bit per part
class Moves {
public:
    enum Mode : int { kOneShot, kLoop };
    enum Quant : int { kSixteenth, kBeat, kBar };
    // Audio thread, per chunk: a press counter (tiles, Go) and the Move parameter's value arm a Move for the next quant line.
    void fire(int move, int mode, int quant);      // from the engine, when a press or a Move-parameter change is seen
    void stop();                                    // a preset or project load, the Move parameter at None
    // Applies the playing Move to norm[P_COUNT] in place (ramps on top of the knobs) and says what Time should play.
    void apply(float* norm, const ef::Transport& t, int* timePattern, double* patternStart, bool* loop);
    int playing() const;                            // -1 none
};
}
```

The 16 factory Moves, in tile order (R28):
1. Throw Beat
2. Throw Bar
3. Dub Freeze
4. Half-Time Bar
5. Tape Stop
6. Slow Down
7. Reverse Beat
8. Spin-Back
9. Stutter 1/8
10. Stutter 1/16
11. Roll Build 1 Bar
12. Roll Build 4 Bars
13. Filter Drop
14. Gate 1/16
15. Break
16. Rewind

What each one does is the builder's, within the design and these anchors:
- **Throws:** echo_send to 1 for the length, then Fall, plus echo_fb +0.3. They switch the Echo on.
- **Dub Freeze:** echo_freeze On for 1 bar after a send pulse.
- **Filter Drop:** the Shape's filter lane on, its cutoff falling 2 octaves over a bar.
- **Break:** dry 0 for 1 bar, with a send pulse at its start.
- **The rest** play their Time pattern.

A Move ends on its length. In Loop it repeats until another Move, None, or a preset load. Its ramps glide back over 10 ms.

- [ ] **Step 1: Failing tests.**
  - `moveQuantizes`: fired mid-beat, it starts on the next beat (or 1/16, or bar).
  - `moveRampsReturn`: Throw Beat raises the send for a beat, then the knob's own value returns.
  - `moveLoop`: Loop repeats until stop().
  - `movePartsOn`: Throw switches the Echo on for its length.
  - `moveSetOption`: Dub Freeze holds the option, then restores it.
  - `moveData`: all 16 have a name, and a pattern or ramps.
- [ ] **Step 2: Run** `make surface && make test-module M=moves`. Expected: fails to compile.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `make test && make test-arm`. Expected: `0 failed`.
- [ ] **Step 5: Commit** — `Moves: sixteen one-tap actions on the grid`.

---

### Task 10: Parameters and pages

**Files:**
- Modify: `surface/surface.py` (the parameters below, appended; the ECHO and TIME groups; the GROOVE page's knobs), `plugin/groove_map.{h,cpp}` (map them; static_asserts against `kEchoDivs`, the routings, modes, Time's grooves)
- Test: `test/plugin_test.cpp` (the mapping test extended)

**The parameters** (appended in this order):

*Global and Echo:*

| Key | Values (default) |
|---|---|
| `dry` | lin 0..1 (1), pct |
| `echo_on` | Off/On |
| `echo_order` | Echo First, Echo Last |
| `echo_route` | Parallel, Serial, Pairs, Ring, Spread |
| `echo_send` | lin 0..1 (1) |
| `echo_fb` | lin -1..1 (0), spct — feedback offset |
| `echo_tone` | lin -1..1 (0), spct, ±4 octaves |
| `echo_duck` | lin 0..1 (0) |
| `echo_freeze` | Off/On |
| `echo_glide` | Tape, Fade |
| `echo_width` | lin 0..1.5 (1) |
| `echo_level` | lin 0..1 (1) |

*Per line,* `a_*`, `b_*`, `c_*`, `d_*`:

| Key | Values (default) |
|---|---|
| `on` | Off/On |
| `level` | lin 0..1 (0.8) |
| `pan` | lin -1..1 (0), spct |
| `sync` | Sync, Free |
| `div` | kEchoDivs names (A 1/8., B 1/4, C 5/16, D 1/2) |
| `ms` | log 1..2000 (375), ms |
| `fb` | lin 0..1 (0.4) |
| `mode` | Stereo, Ping-Pong, Mono |
| `ftype` | LP, BP, HP |
| `cut` | log 20..20000 (20000), hz |
| `reso` | lin 0..1 (0) |
| `place` | In Loop, On Output |
| `drive` | lin 0..1 (0) |
| `wow` | lin 0..1 (0) |
| `mod` | Off, LFO, Env, Follow |
| `mshape` | SHAPES (Sine) |
| `mrate` | kShapeRates names (1/4) |
| `mcut` | lin -1..1 (0) |
| `mtime` | lin -1..1 (0) |
| `mlevel` | lin 0..1 (0) |

*Time and Moves:*

| Key | Values (default) |
|---|---|
| `time_on` | Off/On |
| `time_place` | Post, Pre |
| `time_groove` | Off, Swing, Drunk, Push |
| `time_amount` | lin 0..1 (0.5) |
| `move` | None + the 16 Moves (None) — automatable |
| `move_mode` | One Shot, Loop |
| `move_quant` | 1/16, Beat, Bar (Beat) |
| `move_go` | button |
| `move_1` … `move_16` | tiles |

`echo_sens` (the Env modulators' sensitivity, lin 0..1, 0.5) goes on SETUP.

**The pages:**
- **GROOVE** Q-Links become the design's eight: pump_depth, shape_depth, echo_send, echo_fb, echo_tone, swing, mix, output. The page gets an Echo toggle and a Time toggle beside Pump and Shape.
- **ECHO** page: routing, send, fb, tone, duck, freeze, glide, width, level, order, and the four lines' on/div/fb at a glance. Q-Links: echo_send, echo_fb, echo_tone, echo_duck, echo_width, echo_level, echo_route, echo_freeze.
- **LINE A-D** pages: one per line. Q-Links: div, fb, level, pan, cut, reso, drive, mcut, then the rest.
- **TIME** page: time_on, place, groove, amount, swing, unit.
- **MOVES** page: the 16 tiles, mode, quantize, the Move readout, GO.

Groups: GROOVE, PUMP+SHAPE, ECHO, TIME, SETUP (five).

- [ ] **Step 1: Failing test.** `mappingEcho`: every new key lands on its field (one at a time, so swapped fields fail), and every option list matches the DSP.
- [ ] **Step 2: Implement** the parameters, the pages and grooveParams. `make skin preview`: look at every new page.
- [ ] **Step 3: Run** `make surface && make test && make test-arm`. Expected: `0 failed`.
- [ ] **Step 4: Commit** — `Surface: the Echo's and Time's parameters and pages`.

---

### Task 11: Echo, Time and Moves through the plugin

**Files:**
- Modify: `plugin/engine.{h,cpp}` (Moves applied per chunk on the snapshot; Time's pattern from Moves), `plugin/surface.{h,cpp}` (tiles and GO as press counters, the Move parameter's changes, the Move readout, a preset/project load stops a Move), `plugin/plugin.cpp` (allocation on the UI thread when echo_on or time_on is first set, or a Move needs one; status "N OF 4 ON")
- Test: `test/plugin_test.cpp`

- [ ] **Step 1: Failing tests.**
  - `echoThroughMpc`: Polyrhythm settings give repeats at 3/16 and 5/16.
  - `tilesFireMoves`: tile 10 plays Stutter 1/16 from the next beat.
  - `moveParameterFires`: an automated Move change fires at its quantize.
  - `moveAcrossLoop` (Review Focus 2): a Move over an MPC loop wrap or a locate re-anchors, rejoins now, no click.
  - `allocateWhilePlaying` (Review Focus 3).
  - `twoInstancesRumble` (Review Focus 4).
  - `presetDuringMove` (Review Focus 5).
  - `stateWithEcho`: a project with the Echo and a Move selected round-trips.
  - `stressEverything`: 60 s of random writes, tiles and transport jumps with every part on. Finite, ≤ +18 dBFS, and the parts actually run.
- [ ] **Step 2: Run them.** Expected: fail.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `make test && make test-arm && make arm-plugin && make test-arm-pgo`. Expected: `0 failed`, profile-guided, 1 export.
- [ ] **Step 5: Commit** — `GrooveWerk: Echo, Time and Moves through the plugin`.

---

### Task 12: Bench and PGO

**Files:**
- Modify: `tools/bench.cpp`, `tools/pgo_train.cpp`, `docs/PERFORMANCE.md` (the cases)

- [ ] **Step 1: Bench cases.**
  - Echo heaviest: four lines, Ring, In loop BP at resonance 1, drive 1, wow 1, LFO on each.
  - Time playing Roll Build 1 Bar.
  - Everything heaviest: Echo heaviest plus Shape and Pump heaviest on All, plus Time on a Move.
  - The plan-1 cases kept.
  - Each case reads its settings back with `getParameter` and fails loudly if one didn't land.
- [ ] **Step 2: The trainer.** It gets everything heaviest, a knob sweep per numeric parameter, a stop/locate/tempo-change pass, every Move, and every factory preset. One shared header (`tools/cases.h`) holds the cases for both tools.
- [ ] **Step 3: Run** `make bench` and `make arm-plugin` (profile-guided). Expected: every case PASS on x86.
- [ ] **Step 4: Commit** — `Bench and PGO: Echo, Time, Moves`.

---

### Task 13: The factory presets by job

**Files:**
- Create and modify: `presets/Factory/*` (categories 01_Kick, 02_Bass, 03_Hats_+_Perc, 04_Chords, 05_Drum_Bus, 06_Perform, 07_Echo, 08_Utility)
- Test: `test/plugin_test.cpp` (`factoryPresets` keeps holding; its hard-coded "Bass Pump" / "Low Pump" become computed from the listing)

At least three presets per must use case, about 45 in all. The names and intents are below; the values are the builder's, by ear on the reference material:

| Category | Presets |
|---|---|
| **Kick** | Rumble, Rumble Deep, Rumble Short (mono, LP in the loop, Pump on Echo, Input) |
| **Bass** | Bass Pump, Hard Pump, Double Pump, Offbeat Gate (kept), Rolling Bass, Rolling Dark, Rolling 3/16 |
| **Hats + Perc** | Hat Accents and Hat Shuffle (gain an echo layer), Polyrhythm, Poly 5/16, Ping-Pong Hats, Hat Swing 58, Hat Swing 66, Drunk Hats, Rim Echo |
| **Chords** | Chord Gate Pump, Filter Pump (kept), Dub Chord, Dub Chord Wide, Dub Chord Drive, Stab Throw |
| **Drum Bus** | Low Pump (kept), Loop Groove, Bus Swing, Fill Roller |
| **Perform** | Breakdown, Build, Throws, Glitch |
| **Echo** | Rumble Echo, Dub, Roll, Polyrhythm Echo, Ping-Pong, Tape, Slap, Spread Cloud, Freeze |
| **Utility** | Init |

Each preset sets only what it changes. `make preset-levels` sets every Output, and the user's hat feedback is the bar: hats that groove, not just accents.

- [ ] **Step 1:** Write the presets and run `make preset-levels`.
- [ ] **Step 2: Run** `make surface && make test && make test-arm`. Expected: `surface: … 45 factory presets` (about), `0 failed`.
- [ ] **Step 3: Commit** — `Presets: the factory set by job, level-matched`.

---

### Task 14: Docs for 0.0.1

**Files:**
- Create: `docs/USER_GUIDE.md` (EffectWerk's style: the pages, every part, swing, Moves, presets, the limits — the Input trigger's, the banded split's peaks)
- Modify:
  - `README.md` (what works, building, installing)
  - `CHANGELOG.md` (`## 0.0.1`)
  - `docs/PERFORMANCE.md` (the plan-1 device numbers from 2026-10-26, and the new cases to measure)
  - `docs/DESIGN.md` (plan 1's ruling R22: the getters' mutex is UI-thread only; the pump curves were handed to Claude; Moves without Hold until the probe)
  - `docs/PROBE.md` (unchanged results table, still the user's to run)

- [ ] **Step 1:** Write them, true to the code at head.
- [ ] **Step 2: Commit** — `Docs: the user guide and 0.0.1's notes`.

---

### Task 15: On the Force

With the user, as plan 1's Task 14:
1. `make bench-device`: every case passes, everything heaviest ≤ 15%. Record the numbers in PERFORMANCE.md.
2. `make plugin-install`: MPC restarts.
3. The user plays the hat presets first (their feedback), then Rumble, Rolling Bass, Dub Chord, the Moves on a loop, Hat Swing.
4. Fix what they hear, each fix with a test.
5. Pushing and tagging 0.0.1 is the user's call.
