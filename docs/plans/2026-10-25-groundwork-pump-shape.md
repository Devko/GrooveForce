# GrooveWerk plan 1: groundwork, Pump and Shape

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A GrooveWerk that builds, tests and installs on the Force with two of its four parts,
Pump and Shape, on one shared swing grid. It comes with its pages, a first set of presets and the
release machinery. A probe build answers the design's three open device questions.

**Architecture:** GrooveWerk's groundwork (build, VST2 glue, surface generator, skin, installer,
CI) is copied from MasterWerk, the newest sibling. The shared DSP pieces are copied from
EffectWerk 0.0.3. The sound lives in `dsp/` with EffectWerk's module contract. The grid
(`grid.h`) maps MPC's song position to straight time and back, so Pump and Shape swing the same
way. `Groove` (`dsp/groove.*`) is the chain: a dry bus and a wet bus (silent until plan 2's Echo),
each part acting on All, Dry or Echo. `plugin/` plays the chain from parameters that
`surface/surface.py` defines.

**Tech Stack:** C++17 (GCC; `arm-linux-gnueabihf-g++` 11+ for the device), the siblings' own
`plugin/vst2.h`, Python 3 (`surface.py`; Pillow for the skin), GNU make, qemu-arm, GitHub Actions
(`arm32v7/gcc:11-bullseye`, glibc 2.31).

**Spec:** [`docs/DESIGN.md`](../DESIGN.md). This plan covers its Plan steps 1 and 2. Steps 3
(Echo), 4 (Time and Moves) and 5 (presets by job, levels, the device, 0.0.1) get plans of their own
once this one lands, because step 4 depends on this plan's probe results.

## Global Constraints

- Audio: 44.1 kHz; always 128-frame blocks, in place; 2 inputs, 2 outputs; **zero latency**.
- The module contract (`dsp/common.h`): `Module()` allocates (UI thread); `reset()`; `set(const Params&, const Transport&)` once per chunk; `process(float* L, float* R, int n)` in place, `1 <= n <= kChunk` (32); `tailSamples()`. No allocation, locks or exceptions in `set` / `process`; finite output for any finite input and any parameters; a NaN or infinity in the input never gets into a feedback path.
- Budget: p99 ≤ 15% of the 2902 µs block per instance with everything on at its heaviest. Targets on the Force: Pump, banded, Input trigger ≤ 0.8%; Shape, three lanes ≤ 1%; the plugin's own work ≤ 0.6%. A typical preset ≤ 4%.
- **Never clicks:** every jump, mode change, part on/off and transport jump is smoothed; no gain edge is faster than **1 ms**.
- **On MPC's grid** through loops, locates and tempo changes (the song position, as EffectWerk's Pulse). Stopped, the grid runs on at the tempo.
- **Defaults change nothing:** Init and every part's defaults pass the input through bit for bit until something is turned up. A part that is off costs no CPU; switching a part fades over 10 ms.
- **Swing:** 50-75% on 1/16 or 1/8. A swung unit comes (swing − 50%) × 2 units late: at 120 BPM on 1/16, 58% is 20 ms and 66% is 40 ms. **Swing is applied once along the path** (`Groove::stepSwing`, Task 10).
- No drawn curves: shapes and pump curves are picked by name.
- Pages: at most five tab groups; each page has its own Q-Link set (the Force's 8 knobs show the first 8 keys).
- Parameters: index 0 stays a read-only readout; `surface/params.lock` stops a key that moves; append new parameters at the end.
- Getters cheap and lock-free (MPC polls ~600/s); at most one `audioMasterUpdateDisplay` per 50 ms (`Surface::kTextGap` = 2205 frames).
- The `.so` exports only `VSTPluginMain`; release builds come from CI (glibc 2.31, GCC 11, profile-guided).
- Category names ≤ 12 characters, preset names ≤ 18 (surface.py checks both).
- Namespaces: `ef` for the code copied from EffectWerk, `gf` for GrooveWerk's own, `eft` for tests.
- Every commit message ends with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

1. **MPC jumps the song position mid-step** (a loop that isn't a whole number of steps, a locate): Pump and Shape land on the right step within a chunk, with no click. Tests: Task 8 `pumpFollowsJumps`, Task 9 `shapeFollowsJumps`.
2. **The tempo changes while playing:** the grid stays on the song position, with no phase step. Tests: Task 8 `pumpTempoChange`, Task 9 `shapeTempoChange`.
3. **Stop:** MPC suspends and resumes the plugin within 100 ms. Stopped, the pump keeps ducking at the tempo; resuming doesn't click. Tests: Task 8 `pumpStopped`, Task 11 `suspendResume`.
4. **The shortest steps at the fastest settings:** 1/16 at 150 BPM, swing 75%, push −10 ms, length 0.05. The windows crowd each other, and the 1 ms edge and the step order must hold. Tests: Task 8 `pumpShortestSteps`, Task 3 `gridExtremes`.
5. **Hostile values:** NaN or huge input, every parameter at both ends, denormal decays (the filter lane at full resonance and 20 Hz). Tests: Task 8 `pumpExtremes`, Task 9 `shapeExtremes`, Task 11 `stress`.

---

## File structure (at the end of this plan)

| Path | Responsibility | Task |
|---|---|---|
| `.gitattributes`, `.gitignore`, `LICENSE`, `README.md`, `CHANGELOG.md` | From MasterWerk, renamed | 1, 13 |
| `Makefile` | MasterWerk's, renamed (`gf_test`, `groovewerk.so`, `gfbench`), plus `test-module` | 1, 13 |
| `third_party/mpc-vst-plugins/` | Vendored skin generator, installer, catalog check (MasterWerk's copy) | 1 |
| `dsp/common.h`, `fastmath.h`, `simd.h`, `svf.h` | Copied from EffectWerk 0.0.3, unchanged | 1 |
| `dsp/grid.h` | Swing: song position ↔ straight time | 3 |
| `dsp/pump_curves.h`, `pump_curves.cpp` | The named pump curves, `pumpLevel()` (**your part**) | 4 |
| `dsp/shapes.h`, `shapes.cpp` | The shape library's evaluator; the data is generated | 5 |
| `dsp/transient.h` | Onset detector (Pump's Input trigger; plan 2's Echo modulators) | 6 |
| `dsp/crossover.h` | Linkwitz-Riley 4th-order low/high split | 7 |
| `dsp/pump.h`, `pump.cpp` | Pump | 8 |
| `dsp/shape.h`, `shape.cpp` | Shape: Volume, Filter, Pan lanes | 9 |
| `dsp/groove.h`, `groove.cpp` | The chain: buses, targets, on/off fades, input/output/mix, `stepSwing` | 10 |
| `plugin/*` | MasterWerk's glue without LEARN; `engine.*`, `groove_map.*` new | 11 |
| `surface/surface.py`, `skin_polish.py`, `fonts/`, `params.lock` | Parameters, pages, shapes data, preset checks | 1, 5, 11 |
| `presets/Factory/NN_Category/NN_Name.gfp` | Factory presets | 1, 12 |
| `test/check.h`, `signal.h`, `main.cpp`, `module_main.cpp`, `*_test.cpp`, `host.h` | Tests | every task |
| `tools/bench.cpp`, `pgo_train.cpp`, `reference_mix.h` | Bench, PGO trainer, reference material | 13 |
| `.github/workflows/build.yml` | CI: tests, device build, package, catalog check | 13 |
| `probe/` | GrooveWerk Probe: MIDI, press/release, automation playback | 2 |
| `docs/PROBE.md`, `docs/PERFORMANCE.md` | The probe's questions and results; bench numbers | 2, 14 |

---

### Task 1: Groundwork

The repo builds and runs an empty suite on x86 and under qemu-arm. `surface.py` generates
`build/param_ids.h` from a minimal parameter list.

**Files:**
- Create: `.gitattributes`, `.gitignore`, `LICENSE` (copied from `D:\DEV\MasterWerk`; LICENSE with the same holder line)
- Create: `Makefile` (MasterWerk's; see Step 3)
- Create: `third_party/mpc-vst-plugins/` (copied whole from MasterWerk, its `__pycache__` excluded)
- Create: `dsp/common.h`, `dsp/fastmath.h`, `dsp/simd.h`, `dsp/svf.h` (copied from `D:\DEV\EffectWerk\dsp` at 4b3e35a, unchanged)
- Create: `surface/surface.py`, `surface/skin_polish.py`, `surface/fonts/` (MasterWerk's; see Step 4)
- Create: `presets/Factory/08_Utility/01_Init.gfp` (one line: `groovewerk 1`)
- Create: `test/check.h`, `test/signal.h` (MasterWerk's), `test/main.cpp`, `test/module_main.cpp` (EffectWerk's), `test/dsp_test.cpp` (EffectWerk's)
- Create: `README.md` (a stub: what it is, the link to docs/DESIGN.md, "not released")

**Interfaces:**
- Produces: `test/main.cpp` defines `eft::g_fail`, `eft::g_pass` and `main()`, which calls each suite's `void <name>Tests();` in order and prints `N passed, M failed`. Every later task adds its suite's call.
- Produces: `make test`, `make test-arm`, `make test-module M=<name>` (builds `test/module_main.cpp` with `-DMODULE_TESTS=<name>Tests` and `test/<name>_test.cpp`).
- Produces: `surface.py` writes `surface/build/param_ids.h` (`P_COUNT`, `PARAM_SPECS`, `PARAM_INFO`, `Curve`, `Fmt`, `Kind`) as MasterWerk's does, in `namespace gf`.

- [ ] **Step 1: Copy the files above.** Run `git add --renormalize .` after `.gitattributes` is in, because `docs/DESIGN.md` was committed with CRLF.

- [ ] **Step 2: Write `test/main.cpp`** to call `dspTests()` only (EffectWerk's `test/dsp_test.cpp`: levels, fast math, ramp, delay line, transport, the helpers).

- [ ] **Step 3: Adapt the Makefile.** Rename `mf_test` → `gf_test`, `masterwerk.so` → `groovewerk.so`, `mfbench` → `gfbench`, `MF_` → `GF_`, `Devko - VST - MasterWerk` → `Devko - VST - GrooveWerk`, `--repo Devko/GrooveWerk`, the package's `--about` (from DESIGN.md's first paragraph) and `PKG := GrooveWerk-$(PLUGIN_VERSION)`, preset extension `.mfp` → `.gfp`. Remove the `proto` target. Set `TESTS := $(filter-out test/module_main.cpp,$(wildcard test/*.cpp))`. Add EffectWerk's `test-module` target.

- [ ] **Step 4: Cut surface.py down to the machinery.** `VST = {"name": "GrooveWerk", "vendor": "Devko", "uid": "GrFc", "version": 1000, "so": "groovewerk.so", "params": "params.json", "layout": "layout.conf", "effect": True}`. Parameters: `readout("status", "Status")`, `num("input", "Input", "lin", -24, 24, 0, "db")`, `num("output", "Output", "lin", -24, 24, 0, "db")`, `num("mix", "Mix", "lin", 0, 1, 1, "pct")`, and the preset stepper, browser tiles and buttons as MasterWerk has them. Pages: GROOVE (one card with the three knobs) and PRESETS (MasterWerk's browser). Keep the layout checks, `factory_presets()` (header `groovewerk `, `PRESET_EXT = ".gfp"`, no `LEARNED_KEYS`), `check_lock`, `header()` (namespace `gf`). Remove TARGETS, STYLES, MONO and LEARN.

- [ ] **Step 5: Check that `GrFc` is free.** Run `grep -rn '"GrFc"' third_party/mpc-vst-plugins` and search the catalog (`https://github.com/sd88me/mpc-vst-plugins/tree/main/catalog/plugins`) for `GrFc`. Expected: no hit. On a hit, pick another four-letter ID and say so in the commit.

- [ ] **Step 6: Run the suites.**
  Run (in WSL): `make surface && make test && make test-arm`
  Expected: `surface: <n> parameters, layout ok, 1 factory presets`; both suites print `N passed, 0 failed`. N is EffectWerk's dsp count, the same on both.

- [ ] **Step 7: Commit** — `git add -A && git commit -m "Groundwork: MasterWerk's build and surface, EffectWerk's shared DSP"`.

---

### Task 2: The probe

A GrooveWerk Probe build that answers the design's Open table: MIDI into an insert, press and
release, automation playback. The user runs it on the device. Nothing later in this plan waits for
its results; plan 3 (Time and Moves) does.

What the siblings already know (`plugin/surface.h` in MasterWerk): a button always reads back 0
and **no release ever follows** a press. A tile tap sends a **release echo about 0.7 s later**. The
probe checks whether that echo's timing follows how long the finger stays down (a hold) or is
fixed.

**Files:**
- Create: `probe/` (copied from `D:\DEV\EffectWerk\probe`: `Makefile`, `dsp/`, `plugin/`, `surface/surface.py`, `test/`; not `build/`, `dist/`, `surface/build/`)
- Modify: `probe/plugin/plugin.cpp`, `probe/plugin/probe.{h,cpp}`, `probe/surface/surface.py`, `probe/test/probe_test.cpp`
- Modify: `probe/test/host.h` (helpers the tests below use: `canDo(name)`, `midi(bytes, deltaFrames)` through `effProcessEvents`, `readout(key)`, `set(key, v)`, `advanceMs(ms)` on the probe's test clock, `logHas(text)`, `play(bpm, beats)`)
- Create: `docs/PROBE.md`

**Interfaces:**
- Consumes: Task 1's `third_party/` (the probe's Makefile points at `../third_party/mpc-vst-plugins`).
- Produces: `make -C probe test`, `make -C probe plugin-install`, `make -C probe probe-log`, `make -C probe probe-clear`; log at `/tmp/groovewerk_probe.log` on the device.

- [ ] **Step 1: Rename the probe.** It becomes "GrooveWerk Probe", uid `GfPb`, `groovewerk_probe.so`, folder `Devko - VST - GrooveWerk Probe`, log `/tmp/groovewerk_probe.log`.

- [ ] **Step 2: Write the failing tests** in `probe/test/probe_test.cpp`:

```cpp
void midiIsLogged() {   // canDo says yes; a note-on through effProcessEvents is counted and shown
    Host h;
    CHECK(h.canDo("receiveVstEvents") == 1 && h.canDo("receiveVstMidiEvent") == 1);
    h.midi({0x90, 0x3C, 0x64}, /*deltaFrames*/ 17);
    h.run(1);
    CHECK(h.readout("events").find("MIDI 1") != std::string::npos);
    CHECK(h.readout("events").find("90 3C 64") != std::string::npos);
}
void holdIsTimed() {    // every write to Hold and Tile is logged with the ms since the one before
    Host h;
    h.set("hold", 1.0f); h.advanceMs(1500); h.set("hold", 0.0f);
    CHECK(h.logHas("hold 1.000 +0 ms") && h.logHas("hold 0.000 +1500 ms"));
}
void automationIsTagged() {   // a parameter change says whether MPC was playing, and where
    Host h;
    h.play(120.0, /*beats*/ 6.5);
    h.set("mix", 0.25f);
    CHECK(h.logHas("mix 0.250 playing 6.50"));
}
```

- [ ] **Step 3: Run them.** Run `make -C probe test`. Expected: the three new checks fail.

- [ ] **Step 4: Implement.**
  - `canDoAnswer`: `receiveVstEvents` and `receiveVstMidiEvent` answer 1.
  - `dispatcher`: `effProcessEvents` logs each `VstMidiEvent` (deltaFrames, the three bytes in hex) and bumps a count shown on the Events readout as `MIDI <n>, last <b0> <b1> <b2>`. The audio thread doesn't log. It writes to a lock-free ring, which the heartbeat drains, as the probe already does for its parameter log.
  - New parameters, appended: a momentary `button("hold", "Hold")` and a `tile("tile", "Tile")`. Each write is logged as `<key> <value> +<ms since that key's last write> ms`.
  - Every parameter-change log line gains `playing <beats>` or `stopped`.

- [ ] **Step 5: Run the tests.** Run `make -C probe test`. Expected: `0 failed`.

- [ ] **Step 6: Write `docs/PROBE.md`.** Include the three questions (the design's Open table, verbatim), the install line, and these device steps:
  1. Add *GrooveWerk Probe* as an insert on a drum track. Play its pads. Then play notes into the same track from a MIDI track or the Force's keys.
  2. Tap Hold. Then press and hold it for about 2 s. Do the same with Tile.
  3. Record a Q-Link move of Mix into a sequence. Play it back without touching anything.
  4. `make -C probe probe-log`.
  
  Add an empty results table (question, answer, log lines) for the user's run.

- [ ] **Step 7: Commit** — `git add probe docs/PROBE.md && git commit -m "Probe: MIDI into an insert, press and release, automation playback"`.

- [ ] **Step 8: Hand the device run to the user.** Ask them to run the PROBE.md steps when they can. When they send the log, fill in the results table and commit `docs/PROBE.md`.

---

### Task 3: The grid

**Files:**
- Create: `dsp/grid.h`
- Test: `test/grid_test.cpp` (`gridTests()`, called from `test/main.cpp`)

**Interfaces:**
- Produces:

```cpp
namespace gf {
struct Swing {
    float amount = 0.5f;      // 0.5 straight .. 0.75 (clamped)
    double unitBeats = 0.25;  // the swung unit: 0.25 (1/16) or 0.5 (1/8)
};
// The song position (quarter-note beats) as straight time: where it would be on an unswung grid.
// Piecewise linear in each pair of units; pair boundaries are fixed points.
double toStraight(double beats, const Swing& s);
// The inverse: where a straight position lands in the swung audio.
double toSwung(double straightBeats, const Swing& s);
}
```

- [ ] **Step 1: Write the failing tests.**

```cpp
void gridStraight() {   // 0.5 is the identity, bit for bit, also before beat 0
    for (double b : {-3.7, 0.0, 0.124, 1.0, 17.375, 1e6 + 0.3}) {
        CHECK(gf::toStraight(b, {0.5f, 0.25}) == b);
        CHECK(gf::toSwung(b, {0.5f, 0.25}) == b);
    }
}
void gridSwing() {   // at 66% on 1/16, the second 16th of each 8th starts at 0.33 beats (40 ms at 120 BPM)
    const gf::Swing s{0.66f, 0.25};
    CHECK(std::fabs(gf::toSwung(0.25, s) - 0.33) < 1e-6);
    CHECK(std::fabs(gf::toStraight(0.33, s) - 0.25) < 1e-6);
    CHECK(gf::toStraight(0.5, s) == 0.5 && gf::toStraight(4.0, s) == 4.0);   // pair boundaries fixed
    CHECK(std::fabs((gf::toSwung(0.25, {0.58f, 0.25}) - 0.25) * 500.0 - 20.0) < 0.01);   // 58%: 20 ms at 120
    CHECK(std::fabs(gf::toSwung(0.5, {0.66f, 0.5}) - 0.66) < 1e-6);   // on 1/8: the second 8th of a beat
}
void gridRoundTrip() {   // monotonic, and each the other's inverse, across every amount and both units
    // 10000 random positions in -8..64 beats, amounts 0.5..0.75, units 0.25 and 0.5:
    // |toSwung(toStraight(b)) - b| < 1e-9, and b1 < b2 implies toStraight(b1) <= toStraight(b2)
}
void gridExtremes() {   // 75% clamps, 0.4 and NaN amounts act as 0.5, huge positions stay finite
    CHECK(gf::toSwung(0.25, {0.9f, 0.25}) == gf::toSwung(0.25, {0.75f, 0.25}));
    CHECK(gf::toStraight(1.3, {0.4f, 0.25}) == 1.3);
    CHECK(gf::toStraight(1.3, {NAN, 0.25}) == 1.3);
    CHECK(std::isfinite(gf::toStraight(1e12 + 0.1, {0.75f, 0.25})));
}
```

- [ ] **Step 2: Run them.** Run `make test-module M=grid`. Expected: compile errors (no `dsp/grid.h`).

- [ ] **Step 3: Implement `dsp/grid.h`** (header-only, `inline`). With `u = unitBeats`, a pair
  `2u`, `c = floor(beats / 2u)`, `x = beats − c·2u` and `split = 2u·amount`:
  `straight = c·2u + (x < split ? x·u/split : u + (x − split)·u/(2u − split))`. `toSwung` inverts
  it. An amount that isn't finite or is ≤ 0.5 returns the position unchanged (the bit-for-bit
  rule).

- [ ] **Step 4: Run them.** Run `make test-module M=grid` and then `make test`. Expected: `0 failed`.

- [ ] **Step 5: Commit** — `git commit -m "The grid: swing as a map between the song position and straight time"`.

---

### Task 4: Pump curves (your part)

**Files:**
- Create: `dsp/pump_curves.h`, `dsp/pump_curves.cpp`
- Test: `test/pump_curves_test.cpp` (`pumpCurvesTests()`)

**Interfaces:**
- Produces:

```cpp
namespace gf {
enum PumpCurve : int { PC_SMOOTH, PC_SOFT, PC_PUNCH, PC_HARD, PC_SNAP, PC_LONG, PC_LATE,
                       PC_LINEAR, PC_SCURVE, PC_HOLD, PC_COUNT };
inline constexpr const char* kPumpCurveNames[PC_COUNT] = {
    "Smooth", "Soft", "Punch", "Hard", "Snap", "Long", "Late", "Linear", "S-Curve", "Hold"};
// The level through a duck's recovery: x = 0 at the duck's bottom, 1 recovered. Returns 0..1
// (0 fully ducked, 1 the input's level). The gain is 1 - depth (1 - pumpLevel(curve, x)).
float pumpLevel(int curve, float x);
}
```

- [ ] **Step 1: Write the tests** that any good curve family passes:

```cpp
void curvesEnds() {        // every curve: 0 at the bottom, 1 recovered, x clamped outside 0..1
    for (int c = 0; c < gf::PC_COUNT; ++c) {
        CHECK(gf::pumpLevel(c, 0.0f) == 0.0f && gf::pumpLevel(c, 1.0f) == 1.0f);
        CHECK(gf::pumpLevel(c, -0.5f) == 0.0f && gf::pumpLevel(c, 7.0f) == 1.0f);
    }
}
void curvesRise() {        // never falls, stays in 0..1, finite (NaN x: 1)
    // for each curve, 4096 steps of x: level[i+1] >= level[i], 0 <= level <= 1
    // CHECK(gf::pumpLevel(c, NAN) == 1.0f)
}
void curvesDiffer() {      // each curve is its own: any two differ by > 0.05 somewhere in 0..1
    // for each pair (a, b): max over 256 x of |pumpLevel(a, x) - pumpLevel(b, x)| > 0.05
}
void curvesCharacter() {   // the names mean something
    CHECK(gf::pumpLevel(gf::PC_LINEAR, 0.5f) == 0.5f);
    CHECK(gf::pumpLevel(gf::PC_HOLD, 0.5f) < 0.05f);                                // held down, then back
    CHECK(gf::pumpLevel(gf::PC_LATE, 0.5f) < gf::pumpLevel(gf::PC_SMOOTH, 0.5f));   // comes back late
    CHECK(gf::pumpLevel(gf::PC_SNAP, 0.25f) > gf::pumpLevel(gf::PC_SMOOTH, 0.25f)); // snaps back early
}
```

- [ ] **Step 2: Write a placeholder `pumpLevel`** that clamps x and returns it for every curve. Run `make test-module M=pump_curves`. Expected: `curvesDiffer` and `curvesCharacter` fail, the rest pass.

- [ ] **Step 3: Your part. STOP here and ask the user to write the body of `pumpLevel()`** in
  `dsp/pump_curves.cpp`, about 10-15 lines, one shape per curve. This is the decision the design
  names: what makes a duck feel like Kickstart. Give them this guidance:
  - Smooth is the default sidechain feel, for example an ease-out such as `1 − (1 − x)²`.
  - Punch and Snap come back fast (a high power or an exponential).
  - Long and Late come back slowly, or start late.
  - S-Curve is a smoothstep.
  - Hold stays down for most of the window, then rises at the end.
  - The tests in Step 1 are the only hard rules.

  Wait for their code. Don't write it yourself.

- [ ] **Step 4: Run the tests** on their code. Run `make test-module M=pump_curves`, then `make test` and `make test-arm`. Expected: `0 failed`. If a check fails, show the user the failing line and let them adjust.

- [ ] **Step 5: Commit** with the user as author of the curve: `git commit -m "Pump curves: the ten named recoveries (the user's)"`.

---

### Task 5: The shape library

`surface.py` holds the shapes as data (as the design says) and writes `surface/build/shape_data.h`.
`dsp/shapes.cpp` evaluates them.

**Files:**
- Create: `dsp/shapes.h`, `dsp/shapes.cpp`
- Modify: `surface/surface.py` (`SHAPES`, its checks, `shape_data_header()`; added to `main()`'s outputs and the Makefile's `GEN`)
- Test: `test/shapes_test.cpp` (`shapesTests()`)

**Interfaces:**
- Consumes: `gf::pumpLevel`, `gf::PC_*` (Task 4).
- Produces:

```cpp
namespace gf {
enum class Seg : uint8_t { Linear, Smooth, Hold, Curve };   // from a point to the next
enum class ShapeKind : uint8_t { Points, Steps, Pump, RandomSteps, RandomSmooth };
struct ShapePoint { float x, y; Seg seg; float tension; };   // x ascending in [0, 1), y 0..1
struct ShapeDef {
    const char* name;
    ShapeKind kind;
    int first, count;    // Points: kShapePoints[first..first+count); Steps: 16 levels there (x = i/16)
    int pumpCurve;       // Pump: the PumpCurve
};
int numShapes();
const char* shapeName(int shape);
// The shape's value 0..1 at `cycles`: the fraction is the phase. The whole number seeds the random
// shapes, so they repeat wherever the song position repeats.
float shapeAt(int shape, double cycles);
}
```

  The generated `shape_data.h` defines `gf::kShapePoints[]`, `gf::kShapes[]` and `gf::kNumShapes`.

- [ ] **Step 1: Write `SHAPES` in surface.py.** There are 38 entries, in this order (the order is the
  parameter's option list):
  - Waves: Sine, Triangle, Saw Up, Saw Down, Ramp Up, Ramp Down, Square, Pulse 25.
  - Pumps, one per `kPumpCurveNames`: Pump Smooth, Pump Soft, Pump Punch, Pump Hard, Pump Snap,
    Pump Long, Pump Late, Pump Linear, Pump S-Curve, Pump Hold.
  - Gates, from EffectWerk's `Pulse::kPatternSteps` (`x` = 1, `.` = 0), without its "1/16" (all
    open) and "Pump": 1/8, 1/4, Offbeat, Off 16ths, Dotted, Tresillo, Gallop, Rev Gallop, Trance 1,
    Trance 2, Trance 3, Stutter, Half Bar, Build.
  - Accents, 16 levels each: Offbeat Accent (offbeat 8ths 1, other 16ths 0.45), Rolling 16ths
    (1, 0.55, 0.75, 0.55 repeating), Gallop Accent (Gallop's open steps with its first hit 1, the
    others 0.6), Shuffle (1, 0.3, 0.7, 0.5 repeating).
  - Random: Random Steps (16 steps per cycle), Random Smooth (4 smooth segments per cycle).
  
  The point lists for the waves are the builder's. Sine is `(0, 0) Smooth → (0.5, 1) Smooth`,
  which wraps to 0 exactly as a raised cosine does. A Pump shape is 0 at phase 0 and recovers by
  `pumpLevel` across the cycle.

- [ ] **Step 2: surface.py checks** each entry: names unique and ≤ 13 characters; Points have x
  ascending in [0, 1) starting at 0, y in 0..1 and tension in −1..1; Steps have exactly 16 levels in
  0..1; Pump names match `PUMP_CURVES` (a new list in surface.py, the same as `kPumpCurveNames`);
  38 entries. A wrong entry fails `make surface`.

- [ ] **Step 3: Write the failing tests.**

```cpp
void shapesNamed() {      // the library is the generated one, in surface.py's order
    CHECK(gf::numShapes() == 38);
    CHECK(std::string(gf::shapeName(0)) == "Sine" && std::string(gf::shapeName(37)) == "Random Smooth");
}
void shapesInRange() {    // every shape: 0..1 and finite at 4096 phases over 8 cycles, also cycles < 0
}
void shapesValues() {
    const int sine = 0, offbeat = idx("Offbeat"), pumpHard = idx("Pump Hard");
    CHECK(std::fabs(gf::shapeAt(sine, 0.25) - 0.5f) < 1e-4f && std::fabs(gf::shapeAt(sine, 0.5) - 1.0f) < 1e-6f);
    CHECK(gf::shapeAt(offbeat, 2.0 / 16 + 0.01) == 1.0f && gf::shapeAt(offbeat, 0.01) == 0.0f);  // "..x...x..."
    CHECK(gf::shapeAt(pumpHard, 0.0) == 0.0f);
    CHECK(gf::shapeAt(pumpHard, 0.5) == gf::pumpLevel(gf::PC_HARD, 0.5f));
}
void shapesWrap() {       // Points and Pump: continuous across the wrap unless the last segment is Hold
}
void shapesRandom() {     // the same `cycles` gives the same value; cycles 3 and 4 differ; 3.0 and 3.0 + 1e-9 agree
}
void shapesCurveSeg() {   // Seg::Curve with tension t from a to b: a + (b - a) (exp(6 t u) - 1) / (exp(6 t) - 1), t = 0: linear
}
```

- [ ] **Step 4: Run them.** Run `make test-module M=shapes`. Expected: fails to compile.

- [ ] **Step 5: Implement `dsp/shapes.cpp`.** Smooth is a raised cosine `a + (b − a)(1 − cos πu)/2`;
  Hold keeps `a`; Curve follows the formula in `shapesCurveSeg`. RandomSteps and RandomSmooth take
  their values from a hash of `(shape, floor(cycles), step)` (a 32-bit integer hash; no state), so
  they are pure functions of the position.

- [ ] **Step 6: Run** `make surface && make test && make test-arm`. Expected: `0 failed`.

- [ ] **Step 7: Commit** — `git commit -m "The shape library: 38 shapes as data in surface.py"`.

---

### Task 6: Transient detector

**Files:**
- Create: `dsp/transient.h`
- Test: `test/transient_test.cpp` (`transientTests()`)

**Interfaces:**
- Produces:

```cpp
namespace gf {
class TransientDetector {
public:
    void reset();
    void setSensitivity(float s);   // 0..1: higher catches quieter hits (default 0.5)
    // Follows n samples (n <= kChunk) of the louder side; the offset of the first onset in them,
    // or -1. Two onsets are at least kHoldoffMs apart.
    int process(const float* L, const float* R, int n);
    static constexpr float kHoldoffMs = 40.0f;
};
}
```

- [ ] **Step 1: Write the failing tests.**

```cpp
void transientKicks() {   // a kick (55 Hz, 60 ms decay) every beat at 128 BPM over soft noise -30 dB under it:
    // every kick found within 2 ms of its start, nothing else, over 16 beats (sensitivity 0.5)
}
void transientSteady() {  // a steady sine, steady white noise, a slow 2 s swell: no onsets after the first 50 ms
}
void transientHoldoff() { // a flam of two hits 20 ms apart: one onset; 60 ms apart: two
}
void transientSensitivity() {   // hits -24 dB under the loudest: found at 0.9, not at 0.1
}
void transientNan() {     // a NaN or inf in the input: no onset from it, still finite and finding the next kick
}
```

- [ ] **Step 2: Run them.** Run `make test-module M=transient`. Expected: fails to compile.

- [ ] **Step 3: Implement** a fast and a slow envelope of the louder side, using EffectWerk's
  NEON-lane follower style (`dsp/mod.h` `EnvFollower`): fast attack 0.1 ms and release 8 ms, slow
  attack 15 ms and release 120 ms. An onset is when fast > slow × ratio and fast > −50 dBFS, after
  the holdoff. The ratio goes from 4 (sensitivity 0) to 1.4 (sensitivity 1), log-spaced. The input
  is sanitized first.

- [ ] **Step 4: Run** `make test-module M=transient && make test`. Expected: `0 failed`.

- [ ] **Step 5: Commit** — `git commit -m "Transient detector: onsets for Pump's Input trigger"`.

---

### Task 7: Crossover

**Files:**
- Create: `dsp/crossover.h`
- Test: `test/crossover_test.cpp` (`crossoverTests()`)

**Interfaces:**
- Consumes: `ef::Svf` pieces in `dsp/svf.h` (two Butterworth sections per side make a Linkwitz-Riley 4th order, as EffectWerk's `comp.h` OTT does).
- Produces:

```cpp
namespace gf {
class Crossover {
public:
    void reset();
    void set(float hz);   // 60..300 (clamped); glides like the SVF's g across a chunk
    // Splits n samples (n <= kChunk) of L / R into lows and highs; low + high is an allpass of the input.
    void process(const float* L, const float* R, float* lowL, float* lowR, float* highL, float* highR, int n);
};
}
```

- [ ] **Step 1: Write the failing tests.**

```cpp
void crossoverFlat() {     // |low + high| within ±0.1 dB of the input from 20 Hz to 20 kHz (gainAt-style, at 120 Hz)
}
void crossoverSlopes() {   // at 120 Hz: low and high each -6 dB (±0.3) at 120 Hz; low ≤ -48 dB at 480 Hz; high ≤ -48 dB at 30 Hz
}
void crossoverMoves() {    // a sweep of set() 60 -> 300 Hz over 0.5 s on a sine: finite, no click (maxStep test)
}
```

- [ ] **Step 2: Run them.** Expected: fails to compile.
- [ ] **Step 3: Implement** with the SVF in L/R lanes (`f2`), as `dsp/svf.h` is used.
- [ ] **Step 4: Run** `make test-module M=crossover && make test && make test-arm`. Expected: `0 failed`.
- [ ] **Step 5: Commit** — `git commit -m "Crossover: a Linkwitz-Riley split for the banded pump"`.

---

### Task 8: Pump

**Files:**
- Create: `dsp/pump.h`, `dsp/pump.cpp`
- Test: `test/pump_test.cpp` (`pumpTests()`)

**Interfaces:**
- Consumes: `gf::Swing`, `toStraight`, `toSwung` (Task 3); `pumpLevel`, `PC_*` (Task 4); `TransientDetector` (Task 6); `Crossover` (Task 7).
- Produces:

```cpp
namespace gf {
inline constexpr ef::Division kPumpRates[] = {{"1/16", 0.25}, {"1/8", 0.5}, {"1/4", 1.0}, {"1/2", 2.0}, {"1 bar", 4.0}};
class Pump {
public:
    enum Trigger : int { kGrid, kInput };
    enum Band : int { kFull, kLows, kHighs };
    struct Params {
        double rateBeats = 1.0;     // one of kPumpRates
        int curve = PC_SMOOTH;
        float depth = 0.0f;         // 0..1: 1 is silence at the bottom
        float length = 0.6f;        // 0.05..1: the recovery's share of a step
        float pushMs = 0.0f;        // -10..30: the bottom this far from the step's start (Input: 0..30)
        int trigger = kGrid;
        float sensitivity = 0.5f;
        int band = kFull;
        float crossoverHz = 120.0f; // 60..300
        float mix = 1.0f;           // gain' = 1 + mix (gain - 1)
        Swing swing;                // set by the chain (Groove::stepSwing)
    };
    Pump();
    void reset();
    void set(const Params& p, const ef::Transport& t);
    void process(float* L, float* R, int n);
    int tailSamples() const;        // 0 in Full; the crossover's ring (~1000) when banded
};
}
```

- [ ] **Step 1: Write the failing tests.** Material: DC 0.5 on both sides (the gain is visible
  sample by sample), MPC playing at 120 BPM from beat 0, `run()` from `test/signal.h`.

```cpp
void pumpRests() {          // default Params: output == input bit for bit, any band and trigger
}
void pumpGrid() {           // 1/4, Hard, depth 1, length 0.5: the minimum within ±1 ms of each beat is < 0.005;
                            // 0.5 (recovered) from 250 ms after each beat to 2 ms before the next (the drop starts 1 ms early)
}
void pumpPush() {           // push +20: the bottoms 20 ms after the beats (±1 ms); push -10: 10 ms before
}
void pumpCurveShape() {     // between bottom and recovery the gain follows 1 - depth (1 - pumpLevel(curve, x)) within 0.01,
                            // for Smooth and Long
}
void pumpSwing() {          // 1/16 at swing 66% (unit 1/16): bottoms of odd 16ths 40 ms late; even ones on time
}
void pumpEdges() {          // depth 1, Hold, length 1, 1/16 at 150 BPM: no sample-to-sample gain step > 1/44.1 (1 ms full scale)
}
void pumpInput() {          // Input trigger on beat()-like hits at random, off-grid times: a bottom within 1.5 ms after each hit
}
void pumpBand() {           // 50 Hz + 5 kHz sines, depth 1, at a bottom: Lows takes 50 Hz ≥ 30 dB down and 5 kHz within 1 dB;
                            // Highs the other way round; Full both down
}
void pumpMix() {            // mix 0.5, depth 1: the bottom's gain is 0.5
}
void pumpStopped() {        // MPC stopped: ducks keep coming every 500 ms at 120 BPM; starting to play doesn't click
}
void pumpFollowsJumps() {   // a locate from beat 3.3 to 9.8 and a 1.5-beat loop: the next bottom on the grid, no click (noClicks rule)
}
void pumpTempoChange() {    // 120 -> 128 BPM while playing: bottoms stay on the song position's beats, no click
}
void pumpShortestSteps() {  // 1/16, 150 BPM, swing 75%, push -10, length 0.05: a bottom per 16th in order, the 1 ms edge holds
}
void pumpExtremes() {       // every Params field at both ends and past them, NaN and 1e30 input: finite, never above the input's level
}
void pumpBlockSizes() {     // chunks of 1, 7, 32: the same output as 32 (within 1e-6)
}
```

  The no-click rule is EffectWerk's (`test/pulse_test.cpp` `noClicks`). After each change, the
  steepest step in the next 0.1 s stays under `max(0.01, 1.25 × the settled steepest before and
  after)`. Copy its `steepest()` helper into `test/signal.h`.

- [ ] **Step 2: Run them.** Run `make test-module M=pump`. Expected: fails to compile.

- [ ] **Step 3: Implement.**
  - Each chunk: the straight position from the transport (playing and valid: `toStraight(t.beats)`;
    otherwise the last position advanced at the tempo).
  - The target gain per sample: before a step's bottom, unity; from the bottom on, the recovery by
    `pumpLevel` over `length × rateBeats`.
  - Grid drops start 1 ms before the bottom, since the next bottom's time is known. Then a slope
    limiter of 1 ms full scale (Pulse's gate ramps) keeps every edge at 1 ms or slower.
  - Input: the detector's onset is the bottom plus push (≥ 0).
  - Banded: the crossover splits, the gain applies to one band, and they sum. Depth 0 or off skips
    the crossover entirely.
  - The gain applies to both sides; mix blends gains.

- [ ] **Step 4: Run** `make test-module M=pump && make test && make test-arm`. Expected: `0 failed`.

- [ ] **Step 5: Commit** — `git commit -m "Pump: Kickstart on MPC's grid, with push, an input trigger and bands"`.

---

### Task 9: Shape

**Files:**
- Create: `dsp/shape.h`, `dsp/shape.cpp`
- Test: `test/shape_test.cpp` (`shapeTests()`)

**Interfaces:**
- Consumes: `shapeAt`, `numShapes` (Task 5); `Swing`, `toStraight` (Task 3); the SVF (`dsp/svf.h`).
- Produces:

```cpp
namespace gf {
inline constexpr ef::Division kShapeRates[] = {
    {"1/16", 0.25}, {"1/8T", 1.0 / 3}, {"1/8", 0.5}, {"1/4T", 2.0 / 3}, {"1/8.", 0.75}, {"1/4", 1.0},
    {"1/4.", 1.5}, {"1/2", 2.0}, {"1/2.", 3.0}, {"1 bar", 4.0}, {"2 bars", 8.0}, {"4 bars", 16.0}};
class Shape {
public:
    enum Lane : int { kVolume, kFilter, kPan, kLanes };
    enum FilterType : int { kLP, kBP, kHP };
    struct LaneParams {
        bool on = false;
        int shape = 0;
        double rateBeats = 1.0;   // one of kShapeRates
        float depth = 0.0f;       // Volume 0..1; Filter and Pan -1..1
        float phase = 0.0f;       // 0..1 cycles
    };
    struct Params {
        LaneParams lane[kLanes];
        float amount = 1.0f;      // 0..1: scales every lane's depth (the GROOVE page's Shape Depth)
        int filterType = kLP;
        float cutoffHz = 20000.0f;   // 20..20000: the filter at the shape's top (value 1)
        float resonance = 0.0f;      // 0..1: Q 0.7071 .. 12
        float mix = 1.0f;
        Swing swing;              // set by the chain
    };
    Shape();
    void reset();
    void set(const Params& p, const ef::Transport& t);
    void process(float* L, float* R, int n);
    int tailSamples() const;      // 0 unless the filter lane is on (then its ring)
};
}
```

  The lanes' laws, with v = the shape's value and d = depth × amount:
  - Volume: `gain = 1 − d (1 − v)` (never a boost).
  - Filter: `cutoff = cutoffHz · 2^(4 d (v − 1))`. A positive d dips from the top, a negative one
    rises from it.
  - Pan: `p = d (2v − 1)`, Pulse's constant-power law `sqrt2 cos θ`, `sqrt2 sin θ`,
    `θ = π/4 (1 + p)`.

- [ ] **Step 1: Write the failing tests.**

```cpp
void shapeRests() {        // default Params: bit for bit; a lane on with depth 0: Volume and Pan bit for bit
}
void shapeVolumeSteps() {  // Volume, "Offbeat", 1 bar, depth 1, DC input, 120 BPM: open (0.5) on 16ths 2, 6, 10, 14, else 0
}
void shapeVolumeSine() {   // Volume, Sine, 1/4, depth 0.5: gain 0.5..1 following the raised cosine within 0.01
}
void shapeFilter() {       // LP 8 kHz, Saw Down, 1 bar, depth +1: a 2 kHz sine -3 dB (±0.5) at half a bar
                           // (v 0.5: 8k · 2^-2 = 2 kHz); resonance 0 is Butterworth
}
void shapePan() {          // mono input: L^2 + R^2 within 1e-4 of 2 x^2 throughout; full left at v 0 with depth 1
}
void shapeSwing() {        // Volume, Rolling 16ths at 1 bar, depth 1, swing 66%: the change from 16th 0 (1) to 16th 1 (0.55) lands 40 ms late
}
void shapePhase() {        // phase 0.25 shifts a 1/4 Sine by 125 ms at 120 BPM
}
void shapeEdges() {        // Volume Hold steps (a gate) never step the gain faster than 1 ms full scale
}
void shapeStopped() {      // stopped: the cycle runs on at the tempo; starting to play doesn't click
}
void shapeFollowsJumps() { // locate and short loops: the right step within a chunk, no click (noClicks rule)
}
void shapeTempoChange() {  // a tempo change while playing: stays on the song position, no click
}
void shapeExtremes() {     // every field at both ends (resonance 1 at 20 Hz on loud noise), NaN input: finite, bounded by sqrt2 x input
}
void shapeRandomRepeats() {   // Random Steps under a 1-bar MPC loop: bar 2's output equals bar 1's
}
```

- [ ] **Step 2: Run them.** Run `make test-module M=shape`. Expected: fails to compile.

- [ ] **Step 3: Implement.**
  - Each lane evaluates `shapeAt` at the chunk's start and end, on straight time: cycles =
    `toStraight(beats) / rateBeats + phase`. It goes in a straight line between them.
  - Volume and Pan gains go through a 1 ms slope limiter.
  - The filter is the SVF with its g gliding across the chunk (`dsp/svf.h`'s `Glide`).
  - Lanes that are off cost nothing. Mix blends gains for Volume and Pan, and dry/wet for the
    filter.

- [ ] **Step 4: Run** `make test-module M=shape && make test && make test-arm`. Expected: `0 failed`.

- [ ] **Step 5: Commit** — `git commit -m "Shape: volume, filter and pan from the library, on the grid"`.

---

### Task 10: The chain

**Files:**
- Create: `dsp/groove.h`, `dsp/groove.cpp`
- Test: `test/groove_test.cpp` (`grooveTests()`)

**Interfaces:**
- Consumes: `Pump` (Task 8), `Shape` (Task 9), `Swing` (Task 3).
- Produces:

```cpp
namespace gf {
class Groove {
public:
    enum Target : int { kAll, kDry, kEcho };
    struct Params {
        bool pumpOn = false, shapeOn = false;
        Pump::Params pump;   int pumpTarget = kAll;
        Shape::Params shape; int shapeTarget = kAll;
        Swing swing;         // the global Swing
        float inputDb = 0.0f, outputDb = 0.0f, mix = 1.0f;
    };
    Groove();
    void reset();
    void set(const Params& p, const ef::Transport& t);
    void process(float* L, float* R, int n);
    int tailSamples() const;
    // Swing applied once along the path (docs/DESIGN.md "The signal path and combining", 4): the
    // swing Shape's and Pump's steps follow. Until Time exists (plan 3) it is the global Swing.
    static Swing stepSwing(const Params& p);
};
}
```

  The path in this plan is input gain → dry bus and wet bus (the wet is silent until plan 2's
  Echo) → Shape → Pump (each on its target's bus: All the sum, Dry the dry, Echo the wet) → output
  gain → mix against the chain's input. Order and Time's place come with plans 2 and 3.

- [ ] **Step 1: Write the failing tests.**

```cpp
void grooveRests() {        // default Params: output == input bit for bit (snap 0 dB and mix 1 exactly)
}
void grooveTargets() {      // Pump depth 1 on All and on Dry: the same ducked output; on Echo: unchanged (no echo yet)
}
void grooveFades() {        // pump on/off and shape on/off mid-sine: a 10 ms fade (±1 ms), no click; off costs no work
                            // (a counter in the test build, or the output bit for bit within a chunk of the fade's end)
}
void grooveOrder() {        // Shape then Pump: Volume Offbeat with depth 1 under a 1/4 pump equals the product of each alone (within 1e-5)
}
void grooveStepSwing() {    // stepSwing returns the global Swing; Pump's and Shape's swing are set from it
}
void grooveGains() {        // input +6, output -6: unity; mix 0: the input bit for bit; mix 0.5: halfway
}
```

- [ ] **Step 2: Run them.** Expected: fails to compile.
- [ ] **Step 3: Implement.** The fades are EffectWerk's rack fades (`dsp/rack.cpp`: 10 ms, then the part skipped).
- [ ] **Step 4: Run** `make test-module M=groove && make test && make test-arm`. Expected: `0 failed`.
- [ ] **Step 5: Commit** — `git commit -m "The chain: buses, targets and fades for Pump and Shape"`.

---

### Task 11: The plugin and its pages

**Files:**
- Create: `plugin/vst2.h`, `exports.map`, `paths.{h,cpp}`, `library.{h,cpp}`, `presets.{h,cpp}`, `state.{h,cpp}`, `surface.{h,cpp}`, `trace.{h,cpp}`, `plugin.cpp` (MasterWerk's, `mf` → `gf`, LEARN removed)
- Create: `plugin/engine.{h,cpp}`, `plugin/groove_map.{h,cpp}`
- Modify: `surface/surface.py` (the parameters and pages below)
- Create: `test/host.h` (MasterWerk's), `test/plugin_test.cpp` (`pluginTests()`)

**Interfaces:**
- Consumes: `Groove` (Task 10); `kPumpRates`, `kShapeRates`, `kPumpCurveNames`, `numShapes` (Tasks 4, 5, 8, 9).
- Produces:

```cpp
namespace gf {
float paramValue(int id, float norm);
float paramNorm(int id, float value);
std::string paramDisplay(int id, float norm);
Groove::Params grooveParams(const float* norm);   // norm[P_COUNT]
class Engine {
public:
    Engine();
    void setParams(const float* norm);
    void render(float* L, float* R, int n, const ef::Transport& t);   // any n, chunk by chunk
    void reset();
    int tailSamples() const;
};
}
```

  Environment variables: `GF_PRESET_ROOTS`, `GF_DATA_DIR`, `GF_FIXED_SEED`. Preset roots are
  `Presets` in the plugin folder and `/media/AkaiForce/GrooveWerk Presets`. State text starts
  `groovewerk 1`. Trace file: `/tmp/groovewerk.trace`. `canDo` stays as MasterWerk's (no MIDI
  until the probe says so). `groove_map.cpp` `static_assert`s every option list against the DSP's,
  as `master_map.cpp` does.

- [ ] **Step 1: The parameters**, appended after Task 1's, in this order (key, name, range, default,
  format). `fmt` gains `ms`, `hz` and `spct` (a signed percent, "+40%"), with matching `Fmt::Ms`,
  `Fmt::Hz` and `Fmt::SignedPercent` in `paramDisplay`.

| Key | Name | Range | Default | Fmt |
|---|---|---|---|---|
| `swing` | Swing | lin 0.5..0.75 | 0.5 | pct |
| `swing_unit` | Swing Unit | 1/16, 1/8 | 1/16 | enum |
| `pump_on` | Pump | Off, On | Off | enum |
| `pump_rate` | Pump Rate | `kPumpRates` names | 1/4 | enum |
| `pump_curve` | Pump Curve | `kPumpCurveNames` | Smooth | enum |
| `pump_depth` | Pump Depth | lin 0..1 | 0 | pct |
| `pump_length` | Pump Length | lin 0.05..1 | 0.6 | pct |
| `pump_push` | Pump Push | lin -10..30 | 0 | ms |
| `pump_trigger` | Pump Trigger | Grid, Input | Grid | enum |
| `pump_sens` | Sensitivity | lin 0..1 | 0.5 | pct |
| `pump_band` | Pump Band | Full, Lows, Highs | Full | enum |
| `pump_xover` | Crossover | log 60..300 | 120 | hz |
| `pump_target` | Pump Target | All, Dry, Echo | All | enum |
| `pump_mix` | Pump Mix | lin 0..1 | 1 | pct |
| `shape_on` | Shape | Off, On | Off | enum |
| `shape_depth` | Shape Depth | lin 0..1 | 1 | pct |
| `shape_target` | Shape Target | All, Dry, Echo | All | enum |
| `shape_mix` | Shape Mix | lin 0..1 | 1 | pct |
| `vol_on`, `flt_on`, `pan_on` | Volume / Filter / Pan | Off, On | Off | enum |
| `vol_shape`, `flt_shape`, `pan_shape` | … Shape | `SHAPES` names | Sine | enum |
| `vol_rate`, `flt_rate`, `pan_rate` | … Rate | `kShapeRates` names | 1/4 | enum |
| `vol_depth` | Volume Depth | lin 0..1 | 0 | pct |
| `flt_depth`, `pan_depth` | Filter / Pan Depth | lin -1..1 | 0 | spct |
| `vol_phase`, `flt_phase`, `pan_phase` | … Phase | lin 0..1 | 0 | pct |
| `flt_type` | Filter Type | LP, BP, HP | LP | enum |
| `flt_cutoff` | Cutoff | log 20..20000 | 20000 | hz |
| `flt_reso` | Resonance | lin 0..1 | 0 | pct |

- [ ] **Step 2: The pages**, three groups for now. ECHO and TIME go before SETUP in plans 2 and 3.
  - **GROOVE**:
    - **GROOVE** page: Pump and Shape as toggles, Swing with its unit, knobs.
      Q-Links: `pump_depth`, `shape_depth`, `swing`, `mix`, `output`, `pump_length`, `vol_depth`,
      `flt_depth`. Plan 2 swaps in the design's echo knobs.
    - **PRESETS** page: MasterWerk's browser.
  - **PUMP+SHAPE**:
    - **PUMP** page. Q-Links: `pump_depth`, `pump_length`, `pump_curve`, `pump_rate`, `pump_push`,
      `pump_band`, `pump_xover`, `pump_mix`, then `pump_trigger`, `pump_sens`, `pump_target`,
      `pump_on`.
    - **SHAPE** page: a card per lane. Q-Links: `vol_depth`, `vol_shape`, `vol_rate`, `flt_depth`,
      `flt_shape`, `flt_cutoff`, `pan_depth`, `pan_shape`, then `vol_phase`, `flt_rate`,
      `flt_reso`, `flt_type`, `pan_rate`, `pan_phase`, `shape_mix`, `shape_target`.
  - **SETUP**:
    - **SETUP** page: `input`, `output`, `pump_sens`, `pump_xover`, the status line.

  The positions are the builder's within `surface.py`'s layout checks.

- [ ] **Step 3: Write the failing tests** in `test/plugin_test.cpp`, after MasterWerk's:

```cpp
void basics() {          // VSTPluginMain, 2 in / 2 out, uid 'GrFc', P_COUNT names, index 0 a readout, canDo has no MIDI
}
void passThrough() {     // Init through the VST entry points, in place and separate buffers: bit for bit
}
void pumpThroughMpc() {  // pump on, depth 1, Hard, 1/4, MPC playing 120 BPM: the host's output dips on each beat
}
void detents() {         // a Q-Link detent on pump_curve and vol_shape moves exactly one option, both ways
}
void suspendResume() {   // Stop: effMainsChanged 0 / 1 within 100 ms while pumping: no click, state kept; > 250 ms: cleared
}
void presetsRoundTrip() {   // save a user preset, load Init, load it back: every sound parameter as saved
}
void projectState() {    // effGetChunk / effSetChunk round trip; an unknown key and a missing one are survived
}
void displayRate() {     // a fader sweep of 4096 frames asks for at most 2 UpdateDisplays (kTextGap)
}
void stress() {          // 60 s of random parameter writes, transport jumps and NaN bursts: finite, ≤ +18 dBFS
}
```

- [ ] **Step 4: Run them.** Run `make surface && make test`. Expected: the plugin's tests fail (no plugin yet).

- [ ] **Step 5: Implement** the glue, `Engine` (MasterWerk's chunk loop, transport and output
  guard), `groove_map`, and the status line (CPU, as EffectWerk's).

- [ ] **Step 6: Build the skin** and check it. Run `make skin preview`. Expected: `surface/build/page_*.png`. Look at every page for overlap and clipped labels.

- [ ] **Step 7: Run** `make test && make test-arm`. Expected: `0 failed`.

- [ ] **Step 8: Commit** — `git commit -m "GrooveWerk: the plugin, its pages and state"`.

---

### Task 12: First factory presets

Pump and Shape's use cases. Each preset sets only what it changes. Level-matching comes in plan 4
(`make preset-levels`), so Output stays 0 dB here. The design's categories "Stabs & Chords" and
"Master & Perform" are longer than the 12-character tile limit: they are **Chords** and **Perform**.

**Files:**
- Create: `presets/Factory/02_Bass/01_Bass_Pump.gfp`, `02_Hard_Pump.gfp`, `03_Double_Pump.gfp`, `04_Offbeat_Gate.gfp`
- Create: `presets/Factory/03_Hats_+_Perc/01_Hat_Accents.gfp`, `02_Hat_Shuffle.gfp`
- Create: `presets/Factory/04_Chords/01_Chord_Gate_Pump.gfp`, `02_Filter_Pump.gfp`
- Create: `presets/Factory/05_Drum_Bus/01_Low_Pump.gfp`
- Modify: `test/plugin_test.cpp` (`factoryPresets`)

| Preset | Lines (after `groovewerk 1`) |
|---|---|
| Bass Pump | `pump_on=On`, `pump_rate=1/4`, `pump_curve=Smooth`, `pump_depth=0.8`, `pump_length=0.6` |
| Hard Pump | `pump_on=On`, `pump_curve=Hard`, `pump_depth=1`, `pump_length=0.45` |
| Double Pump | `pump_on=On`, `pump_rate=1/8`, `pump_curve=Punch`, `pump_depth=0.7`, `pump_length=0.7` |
| Offbeat Gate | `shape_on=On`, `vol_on=On`, `vol_shape=Offbeat`, `vol_rate=1 bar`, `vol_depth=1` |
| Hat Accents | `shape_on=On`, `vol_on=On`, `vol_shape=Offbeat Accent`, `vol_rate=1 bar`, `vol_depth=0.6` |
| Hat Shuffle | `shape_on=On`, `vol_on=On`, `vol_shape=Shuffle`, `vol_rate=1 bar`, `vol_depth=0.5`, `pan_on=On`, `pan_shape=Triangle`, `pan_rate=1 bar`, `pan_depth=0.4` (no Swing. Without Time, Swing only says the audio is already swung, as MPC's note swing would. On straight hats it would put every accent late. Swung hats come with Time in plan 3) |
| Chord Gate Pump | `shape_on=On`, `vol_on=On`, `vol_shape=Trance 1`, `vol_rate=1 bar`, `vol_depth=1`, `pump_on=On`, `pump_depth=0.5` |
| Filter Pump | `shape_on=On`, `flt_on=On`, `flt_shape=Pump Smooth`, `flt_rate=1/4`, `flt_depth=0.6`, `flt_cutoff=6000`, `flt_reso=0.3` |
| Low Pump | `pump_on=On`, `pump_band=Lows`, `pump_xover=120`, `pump_trigger=Input`, `pump_curve=Smooth`, `pump_depth=0.7`, `pump_length=0.5` |

- [ ] **Step 1: Write the failing test**:

```cpp
void factoryPresets() {  // every factory preset loads through the browser; on the reference mix at 120 BPM each one
                         // changes the output (rms of the difference > -40 dB re the input) and stays finite and ≤ +18 dBFS
}
```

- [ ] **Step 2: Run it.** Run `make surface && make test`. Expected: `factoryPresets` fails (only Init exists).
- [ ] **Step 3: Write the nine preset files.**
- [ ] **Step 4: Run** `make surface && make test`. Expected: `surface: … 10 factory presets`, `0 failed`.
- [ ] **Step 5: Commit** — `git commit -m "Presets: the first pump and shape presets"`.

---

### Task 13: Device build, bench and CI

**Files:**
- Create: `tools/reference_mix.h` (MasterWerk's), `tools/bench.cpp`, `tools/pgo_train.cpp` (MasterWerk's, adapted)
- Create: `.github/workflows/build.yml` (MasterWerk's: `--expect-id groovewerk --expect-repo Devko/GrooveWerk`, artifact `GrooveWerk-mpc-armv7`)
- Create: `CHANGELOG.md` (`## Unreleased`), `docs/PERFORMANCE.md`
- Modify: `README.md` (what works now, building, the install line, "preview: Pump and Shape")

**Interfaces:**
- Consumes: the plugin (Task 11), the presets (Task 12).
- Produces: `make arm-plugin` (profile-guided when qemu-arm is on PATH: `build/arm/so_flags` starts with `1`), `make bench`, `make bench-device`, `make test-arm-pgo`, `make plugin-package`, `make plugin-install`.

- [ ] **Step 1: The bench cases** in `tools/bench.cpp` (`gfbench <so> [-s s] [-c cpu]`). Each case is noise in place through the plugin's entry points, MPC playing 128 BPM.

| Case | What's on |
|---|---|
| Everything off | Init |
| Pump heaviest | 1/16, Lows, Input trigger, depth 1, push −10 |
| Shape heaviest | three lanes on 1/16, BP, resonance 1, Random Smooth on the filter |
| Both heaviest | the two above together, swing 75% |
| Four instances | four plugins, each Both heaviest, one after another per block |

- [ ] **Step 2: The PGO trainer** plays the reference mix at rest, with every option of every enum, the Both heaviest case and every factory preset.

- [ ] **Step 3: Run the device build.**
  Run (in WSL, as a login shell): `make arm-plugin && make test-arm-pgo && make bench`
  Expected: `profile-guided build`, `exported symbols: 1`, the suite `0 failed`, the bench prints every case.

- [ ] **Step 4: Package.** Run `make plugin-package`. Expected: `dist/GrooveWerk-0.0.1-mpc-armv7.zip`. Then run `python3 third_party/mpc-vst-plugins/tools/catalog_check.py dist/GrooveWerk-0.0.1-mpc-armv7.zip --expect-id groovewerk`. Expected: OK (a local glibc warning is fine; CI's build is the release one).

- [ ] **Step 5: Commit**, then push on the user's go-ahead (CI runs on push). Commit: `git commit -m "Device build, bench, PGO and CI"`.
  Expected on push: both CI jobs green, with the zip as an artifact.

---

### Task 14: On the Force

The user's ears are the test here. Follow the deploy notes from the siblings: run `git fetch`
first, use a WSL login shell (`bash -l`), and use `ssh -o BatchMode=yes`. The Force's IP moves with
DHCP, so ask the user for it.

**Files:**
- Create: `local.mk` (ignored: `FORCE = root@<ip>`, `SSH_KEY = $(HOME)/.ssh/mockba_force`, `PY = $(HOME)/.venvs/rackwerk/bin/python`)
- Modify: `docs/PERFORMANCE.md` (the device numbers), `docs/DESIGN.md` (nothing unless the device disagrees with it)

- [ ] **Step 1: Bench on the device.** Run `make bench-device`. Expected: Both heaviest p99 ≤ 2.4% (Pump 0.8 + Shape 1 + overhead 0.6); Four instances about four times that. Record the table in `docs/PERFORMANCE.md`.

- [ ] **Step 2: Install.** Run `make plugin-install` (it stops and restarts MPC: the user saves their project first).

- [ ] **Step 3: Ask the user to play these and say how each sounds:**
  - Bass Pump, Hard Pump and Double Pump on a bass line under a kick.
  - Low Pump on a drum bus.
  - Hat Accents and Hat Shuffle on a straight hat loop.
  - Chord Gate Pump and Filter Pump on stabs.
  - Save and reopen the project.
  - Two instances on two tracks.
  - Stop while pumping.
  - Loop a 1.5-bar region.

  Fix what they hear, with a test for each fix, and write down what they say.

- [ ] **Step 4: Commit** — `git commit -m "Docs: GrooveWerk on the Force, Pump and Shape"`.
