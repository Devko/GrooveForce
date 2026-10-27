# Performance

- [The budget](#the-budget)
- [The bench](#the-bench)
- [Device measurements](#device-measurements)
- [Instruction counts](#instruction-counts)
- [What keeps it cheap](#what-keeps-it-cheap)

---

## The budget

MPC renders 128-frame blocks: **2902 µs per block**, per plugin instance. A plugin passes at
**p99 ≤ 15%** of the block (warns up to 35%). GrooveForce's ceiling is **every part on at its
heaviest**, what the worst preset could reach: four Echo lines in a Ring with in-loop filters, drive
and modulators, three Shape lanes and a banded Pump on the Input trigger acting on both buses (the dry
and the Echo's wet), and Time playing a Move. Usually it sits on several tracks at once, one copy
each, so a typical preset should cost **≤ 4%** (the [design](DESIGN.md#budget) has the same table).

| Part | Target on the Force (p99, percent of a block) |
|---|---|
| Echo, everything on | ≤ 6 |
| Time, a Move playing | ≤ 1.5 |
| Shape, three lanes | ≤ 1 |
| Pump, banded, Input trigger | ≤ 0.8 |
| The plugin's own work | ≤ 0.6 |
| Pump and Shape together, heaviest | ≤ 2.4 (the Pump, the Shape and the plugin's own work added) |
| **Everything at its heaviest** | **≤ 15** |
| A typical preset | ≤ 4 |

## The bench

`make bench-device` copies the `.so` and `gfbench` (tools/bench.cpp) to the Force, runs them pinned to
core 1 (MPC's audio workers own cores 2 and 3) while MPC keeps running, and deletes them
(`BENCH_ARGS="-s 5"` for five seconds a case). Each case feeds noise in place through the plugin's own
entry points, MPC playing at 128 BPM, and times every block with the thread's CPU clock; a kick-like
burst on every beat (+9 dB for 20 ms) gives an Input trigger something to fire on. `make bench` runs the
same cases on the machine you build on, for one second each, and CI runs it too: x86 numbers say
nothing about the Force, it shows that the bench and the `.so` path work. `make bench-count` counts ARM
instructions instead ([below](#instruction-counts)).

The settings of every case are in tools/cases.h, which the profile-guided build's trainer
(tools/pgo_train.cpp) shares, so the profile is made from what the bench measures. A case sets what it
says through the plugin's own entry points and **checks itself**: every setting is read back
(`getParameter`) and compared with what was set, and after the warm-up and again at the end the status
line must say that as many parts ran as the case has on, and the Move's tile must be lit while its Move
plays. A key or option that was renamed, or a Move that never starts, stops the bench with a message
instead of being timed as another case. The 4 s warm-up fills the Echo's lines and the Time ring (eight
beats at 128 BPM, the farthest back a pattern reads) before anything is timed.

| # | Case | What's on |
|---|---|---|
| 0 | Everything off | Init: the plugin's own work (the parameters, the transport, the status meter) |
| 1 | Pump heaviest | Pump on, 1/16, band Lows, trigger Input, depth 100%, push -10 ms |
| 2 | Shape heaviest | Shape on, all three lanes on at 1/16 and depth 100%, the filter a band-pass at 1 kHz with resonance 100% running Random Smooth |
| 3 | Both heaviest | the two above together, swing 75% on 1/16 |
| 4 | Four instances | four plugins, each Both heaviest, one after another per block |
| 5 | Echo heaviest | Echo on, Ring, all four lines on and panned apart: In Loop band-pass at 1 kHz with resonance 100%, drive 100%, wow 100%, feedback 90%, an LFO (Random Smooth, at 1/16, 1/8, 1/4 and 1/2) moving each line's cutoff, time and level at 100%; duck 100%, width 130%, tone a hair under, glide Tape |
| 6 | Echo heaviest, Fade glide, Ping-Pong | case 5 with the Fade glide and Ping-Pong lines: the LFO moves every line's time, so a 50 ms crossfade between two read heads never ends |
| 7 | Time playing Roll Build 1 Bar | Time on, the Move Roll Build 1 Bar going round (a jump every few beats, the last ones every 1/8 beat), fired at the next 1/16 |
| 8 | Time in fades: Stutter 1/16 | Time on, the Move Stutter 1/16 going round: a jump every 1/16, so the crossfade (a second read, the weights) runs in 7% of the blocks |
| 9 | Everything heaviest | Pump and Shape heaviest on All (so on the dry and on the wet), swing 75%, case 6's Echo, Time on Roll Build 1 Bar |

The four instances are one thread calling four plugins in turn, as one of MPC's audio workers would;
the line shows the sum of the four calls of each block, and a second line the per-instance share (the
verdict is on that). It is the worst answer to the design's open question of whether several copies
spread over the Force's cores or add up on one: if MPC puts all four on one core, this is what that core
carries; if it spreads them, each copy costs its share.

## Device measurements

2026-10-26, the Force (plan 1: the Pump, the Shape and the plugin's own work), `make bench-device`, the
profile-guided `.so`, 5 s per case, core 1, MPC running, percent of the 2902 µs block:

| Case | avg | p99 | Design target (p99) |
|---|---|---|---|
| Everything off | – | 0.21 | ≤ 0.6 |
| Pump heaviest | 0.79 | 1.08 | 0.8 (a touch over; 0.87 net of the plugin's own work) |
| Shape heaviest | 0.81 | 1.15 | 1.0 (0.94 net) |
| **Both heaviest**, swing 75% | 1.49 | 1.93 | **2.4** |
| Four instances (sum) | 5.88 | 6.57 | about 4 × 2.4 (1.64 each) |

Every case passes at 15%. Four instances cost less than four times one (6.57 against 4 × 1.93): if MPC
puts several copies on one core they add up, with nothing shared between them and no penalty.

2026-10-27, the Force (plan 2: every case), the same way (`make bench-device`, the profile-guided `.so`,
5 s per case, core 1, MPC running):

| Case | avg | p99 | max | Budget (p99) |
|---|---|---|---|---|
| 0 Everything off: Init | 0.21 | 0.42 | 0.68 | ≤ 0.6 (the plugin's own work, now with plan 2's engine) |
| 1 Pump heaviest | 0.81 | 1.15 | 1.63 | 0.8 |
| 2 Shape heaviest | 0.86 | 1.23 | 1.51 | 1.0 |
| 3 **Both heaviest**, swing 75% | 1.49 | 1.99 | 2.43 | **2.4** |
| 4 Four instances (sum) | 5.92 | 6.78 | 7.53 | about 4 × 2.4 (1.70 each) |
| 5 Echo heaviest | 4.75 | 5.73 | 6.06 | ≤ 6 |
| 6 Echo heaviest, Fade glide, Ping-Pong | 5.46 | 6.55 | 6.90 | ≤ 6 (over: kept for 0.0.1, see below) |
| 7 Time playing Roll Build 1 Bar | 0.69 | 1.14 | 1.46 | ≤ 1.5 |
| 8 Time in fades: Stutter 1/16 | 0.69 | 1.18 | 1.47 | ≤ 1.5 |
| **9 Everything heaviest** | 8.73 | **10.06** | 10.94 | **≤ 15** |

Every case passes at 15%. Case 6 is 0.55 point over the Echo's own target: a Fade glide under time
modulation starts a new 50 ms crossfade whenever the time moves by half a sample, so the second head is
read most of the time. It stays as it is for 0.0.1, since everything at its heaviest leaves 5 points of the
15; the lever, if it is ever needed, is a minimum spacing between Fade glides (a change to the sound).

Before the device run, the x86 numbers (this machine, `make bench`, the median of five runs of 3 s a case:
they only show that the cases run) and the estimates from the instruction counts below, against the Force:

| Case | x86 avg / p99 | Instructions per block, thousands (avg / p99) | Estimate, Force p99 | Force p99 |
|---|---|---|---|---|
| 5 Echo heaviest | 0.71 / 0.94 | 195.6 / 196.0 | 4.5-5.4 | 5.73 |
| 6 Echo heaviest, Fade glide, Ping-Pong | 0.84 / 1.11 | 244.8 / 245.4 | 5.6-6.8 | 6.55 |
| 7 Time playing Roll Build 1 Bar | 0.14 / 0.17 | 41.9 / 60.0 | 1.4-1.7 (net of the plugin's own work 1.1-1.3) | 1.14 |
| 8 Time in fades: Stutter 1/16 | 0.13 / 0.18 | 41.8 / 59.2 | 1.3-1.6 (net 1.1-1.3) | 1.18 |
| **9 Everything heaviest** | 1.26 / 1.79 | 397.8 / 415.9 | **9.5-11.6** | **10.06** |

The estimates divide the p99 count by 44k and by 36k instructions to a point of the block (see below),
and include the plugin's own work (case 0: 11.8k, 0.3 point), as the plan 1 cases do. The Echo with the
Fade glide is the one close to its budget, and only at the pessimistic end; its hot spots are listed
below. The Time cases are at their edge the same way: their net is inside, and the heaviest Move of the
sixteen, Spin-Back going round, is a little over (avg 43.3k, p99 63.9k: 1.8 at most, 1.2-1.5 net).

## Instruction counts

Without the device, ARM instructions per 128-frame block are the measure, as for EffectForce:
`make bench-count` builds the plain `.so` (the device flags, no profile, unstripped, with `-g`), the ARM
bench and tools/gfcount.c, a qemu TCG plugin, and runs the bench with `-m` under a plugin-enabled
qemu-arm (`QEMU_PLUGINS`, and `QEMU_PLUGIN_INCLUDE` for the folder with its `qemu-plugin.h`, the 8.2
API; both can go in local.mk). The bench marks where each measured `processReplacing` call starts and
ends with a system call that no kernel has (the plugin sees it on its way in), and the plugin counts every
instruction in between. Every case prints its thousands of instructions per block (average, p99 and max
over 2 s, 689 blocks) and two hashes of its output, the warm-up's and the whole case's: a change meant to
keep the sound bit for bit keeps every hash, built with the same compiler. `CASES=5,6` runs only those
cases, numbered as the bench lists them; `PROF=<file>` also writes every translated block that ran in the
measured calls (`vaddr insns execs`), and the log, build/arm/count/log.txt, has the bench's memory map: an
address less `BASE` is the `.so`'s, for `arm-linux-gnueabihf-addr2line` (or `objdump -dl`, which gives the
instructions with their lines). The four instances' count is the sum of the four calls.

EffectForce's rule of thumb is about 36k-44k instructions to a point of the block on the Force. Plan 1's
device run bears it out for the p99 (Pump 44.8k for 1.08, Shape 40.3k for 1.15, both 73.8k for 1.93: 35k-42k
to a point; the device average runs 49k-57k a point). That sets today's counts, plan 2's build (case 0, the
plugin's own 11.8k, now has plan 2's engine in it), against plan 1's device times. The counts miss what the device adds (cache misses, VFP
divisions and flag transfers that stall), which the device's p99 over its average shows.

2026-10-26, thousands of instructions per block (GCC 13, the device flags, 2 s a case):

| Case | avg | p99 | max | Estimate, Force p99 (percent of a block) |
|---|---|---|---|---|
| 0 Everything off | 11.8 | 11.8 | 11.9 | 0.3 (measured: 0.21) |
| 1 Pump heaviest | 44.8 | 44.8 | 44.9 | 1.0-1.2 (measured: 1.08) |
| 2 Shape heaviest | 39.9 | 40.3 | 40.3 | 0.9-1.1 (measured: 1.15) |
| 3 Both heaviest, swing 75% | 73.4 | 73.8 | 73.9 | 1.7-2.1 (measured: 1.93) |
| 4 Four instances (sum) | 293.6 | 295.2 | 295.3 | 1.7-2.1 each |
| 5 Echo heaviest | 195.6 | 196.0 | 196.1 | 4.5-5.4 |
| 6 Echo heaviest, Fade glide, Ping-Pong | 244.8 | 245.4 | 245.4 | 5.6-6.8 |
| 7 Time playing Roll Build 1 Bar | 41.9 | 60.0 | 67.0 | 1.4-1.7 |
| 8 Time in fades: Stutter 1/16 | 41.8 | 59.2 | 66.9 | 1.3-1.6 |
| **9 Everything heaviest** | **397.8** | **415.9** | 423.7 | **9.5-11.6** |

- **Everything heaviest** has 3.4 points or more to spare of its 15, with the Echo at its heaviest.
- **The Echo** with the Tape glide is within its 6 (4.5-5.4). The Fade glide with Ping-Pong lines is a
  quarter dearer (a second read head per line, always crossfading) and reaches 5.6-6.8, across the 6; it
  was left as it is. Per block, in thousands, case 5: `EchoLine::readRun` 73.9 (the lines' reads: the
  Hermite taps, the filter and the wow), `Echo::process` 38.4 (the routing, the modulators, the duck, the
  mix), `EchoLine::writeRun` 36.7, the filters' coefficients (`setFilter`) 6.3; case 6: `readRun` with the second
  head 115.3, `Echo::process` 43.4, `writeRun` 36.7, `setFilter` 6.3. Everything else is under 5. The reads are
  the Echo: a four-point Hermite and a state-variable filter per line and side, per sample, and a second head
  while a time change fades.
- **Time** met its budget only after work (next paragraph): Roll Build 1 Bar went from 140.2k at p99 (3.2-3.9
  points) to 60.0k. In a Move the rest of the cost is the engine's: `Engine::render` compares the 229-float
  snapshot with the one the chain was made from (`memcmp`) every chunk, 5.0k a block, 12% of case 7, to find out
  whether a Move changed anything; `Moves::apply`, `Groove::set` and the plugin's `runBlock` are 1.5k, 1.2k and
  1.2k. The playhead itself is about 27k.

**Time's playhead, before and after.** The first count of cases 7 and 8 was 140.2k and 146.5k at p99 (3.2-4.1
points, over the 1.5): even the blocks without a jump cost 98k, 700 instructions a sample. The playhead
looked its curves up in every sample: a cursor into the source and the volume, the slope, the next Jump (each a
double division, tens of cycles on the device and one instruction in the count), the fade's cosine from libm,
the four ring taps with 64-bit indices (a 64-bit conversion is a library call on ARM), and two scalar Hermite
reads. Now:

- a curve keeps the numbers of the segment its cursor stands on (its ends, its slope, the Jump it heads for), so
  a call on a straight segment divides nothing (a Smooth segment, none in the library today, still divides and
  takes a cosine or a sine per call);
- a **run**: the first sample of a stretch (one segment of the source, one straight piece of the volume, up to
  the next line's fade or inside it) looks everything up and leaves the formulas; the samples after it only
  evaluate them, with the same arithmetic, so a sample has the same answer by either way (tested: bit for bit);
- the crossfade's weight is a table of 4096 steps with linear interpolation, within 1.2e-7 of the raised cosine;
- the ring is indexed in 32 bits, the sample numbers go through doubles, the Hermite of both channels is one NEON
  vector, and the reads inline.

| Case (thousands of instructions per block) | Before (avg / p99 / max) | Now |
|---|---|---|
| 7 Time playing Roll Build 1 Bar | 101.6 / 140.2 / 142.4 | 41.9 / 60.0 / 67.0 |
| 8 Time in fades: Stutter 1/16 | 107.0 / 146.5 / 147.4 | 41.8 / 59.2 / 66.9 |
| 9 Everything heaviest | 457.4 / 497.2 / 499.2 | 397.8 / 415.9 / 423.7 |

The output moved by float rounding, no more. Against the old engine, over 18M frames of all 15 patterns at 128
and 300 BPM, in chunks of 32, with a lag, a tempo change and a locate under the pattern, the largest difference
was 7.5e-8 on ARM (1.3e-7 on x86), the rms difference 7e-10 on a signal of 0.12 (-165 dB); the review's wider
set (5,850 scenarios: one-shot and going round, 83-999 BPM, grooves, tempo steps and ramps, locates, MPC loops,
re-fires, pattern switches, resets, chunks of 1 to 128) found at most 2.1e-7 (about two float steps at 0.7) on x86
and 1.8e-7 on ARM, nothing over 1e-6. test/time_test.cpp keeps the parts of that proof that can be kept: the
weights table against the cosine (under 1.2e-7), the curves' numbers against dividing at every call (under 1e-12,
for every pattern, going forward and jumping about), and every pattern, one-shot and going round, played with
runs and with runs off (`useRuns(false)`) through chunks of 1, 7, 31, 32 and 128, a tempo step and a ramp, a
locate, a re-fire, another pattern at the same start and a reset: identical, bit for bit. Without the run's
two resets (the tempo's, the pattern's) 14 and 26 of its 30 cases fail.

## What keeps it cheap

- **A part that is off costs nothing:** the engine skips it, and switching fades over 10 ms. The one thing it
  does cost is Time's ring: once allocated it records the audio at Time's place while Time is off (a copy of each
  sample), so that a Move that reads the past finds it. At the defaults (Init) the block is handed back
  untouched, bit for bit, with or without the ring.
- **Memory is made on the UI thread**, the first time a part is switched on (or a Move that needs it is
  selected): the Echo's lines (4.2 MB) and Time's ring (4 MB) are handed over through an atomic pointer, and
  the part is silent and free until they arrive. A part never switched on holds nothing.
- **Control rate:** parameters, the grid's song position, the modulators and coefficients are worked out
  once per 32-sample chunk; every part glides across the chunk.
- **Pure functions of the song position:** a shape, a pump curve and a Time pattern are evaluated from where the
  song is, with no state of their own to keep in step, so a loop or a locate costs nothing extra.
  Even the random shapes take their seed from the cycle they are in.
- **Runs, not lookups:** Time's playhead looks its curves up once per segment and evaluates formulas for the
  samples after (see above).
- **NEON where the work is parallel:** left and right run as two lanes of one vector in the state-variable
  filters (the Shape's filter lane, the Pump's crossover, the Echo's lines) and in the playhead's Hermite
  reads; the crossover is three filters, not four.
- **No libm and no 64-bit conversions per sample:** EffectForce's fast math (`dsp/fastmath.h`), per-chunk
  coefficients, a table for the crossfade's cosine, ring indices in 32 bits; the tan behind a cutoff is
  worked out once per chunk at most.
- **Settled is free:** a level that isn't moving and sits at unity is not touched.
- **The profile-guided build:** `make arm-plugin` trains on the reference mix (`tools/pgo_train.cpp`, with
  the cases of `tools/cases.h`) so the compiler knows which paths are hot: at rest and hot; every option of
  every choice with all four parts on; every numeric parameter moved across its range and back, a setting a
  block, as automation or a Q-Link does; everything at its heaviest; the transport stopped, located, looped
  and at tempos from 20 to 999 BPM (with MPC's suspend and resume, long and short); swing on 1/8; every Move,
  as a One Shot and going round; and every preset in the library (the factory presets are compiled in from
  presets/Factory, and the trainer walks the browser from the first to the last, so a preset added there joins
  by itself; it stops the build if it played fewer than the folder has). Every setting is read back. About a
  minute and a half under qemu. Release builds are profile-guided in CI too.
- **Cheap getters:** MPC polls names and texts hundreds of times a second. A parameter's value is an atomic;
  its text is made from the value as it is asked for. Only the texts of the tiles, steppers and readouts
  (`Surface::display`) take a lock (`mtx_`), and only MPC's UI thread and the surface's own refresh take
  it: the audio thread never does.
- **At most one redraw per 50 ms:** the status line's CPU figure changes now and then; MPC rebuilds its
  parameter screen on each redraw, so the plugin asks for at most one every 50 ms.
