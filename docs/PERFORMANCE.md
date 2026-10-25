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
heaviest**: what the worst preset could reach. Usually it sits on several tracks at once, one copy
each, so a typical preset should cost **≤ 4%** (the [design](DESIGN.md#budget) has the full table;
the Echo's and Time's rows join this one when those parts exist).

| Part | Target on the Force (p99, percent of a block) |
|---|---|
| Pump, banded, Input trigger | ≤ 0.8 |
| Shape, three lanes | ≤ 1 |
| The plugin's own work | ≤ 0.6 |
| **Pump and Shape together, heaviest** | **≤ 2.4** (the three rows added) |
| A typical preset | ≤ 4 |

## The bench

`make bench-device` copies the `.so` and `gfbench` (tools/bench.cpp) to the Force, runs them pinned to
core 1 (MPC's audio workers own cores 2 and 3) while MPC keeps running, and deletes them. Each case
feeds noise in place through the plugin's own entry points, MPC playing at 128 BPM, and times every
block with the thread's CPU clock; a kick-like burst of that noise on every beat gives an Input trigger
something to fire on. `make bench` runs the same cases on the machine you build on, for one second each:
x86 numbers say nothing about the Force, it only shows that the bench and the `.so` path work.

| Case | What's on |
|---|---|
| Everything off | Init: the plugin's own work (the parameters, the transport, the status meter) |
| Pump heaviest | Pump on, 1/16, band Lows, trigger Input, depth 100%, push -10 ms |
| Shape heaviest | Shape on, all three lanes on at 1/16 and depth 100%, the filter a band-pass at 1 kHz with resonance 100% running Random Smooth |
| Both heaviest | the two above together, swing 75% on 1/16 |
| Four instances | four plugins, each Both heaviest, one after another per block |

The four instances are one thread calling four plugins in turn, as one of MPC's audio workers would;
the line shows the sum of the four calls of each block, and a second line the per-instance share (the
verdict is on that). It is the worst answer to the design's open question of whether several copies
spread over the Force's cores or add up on one: if MPC puts all four on one core, this is what that core
carries; if it spreads them, each copy costs its share.

## Device measurements

**Not measured yet.** The numbers come from the Force in the plan's last task (Task 14). Until then
the table is the bench's shape and the targets above.

| Case | avg | p99 | max | Design target (p99) |
|---|---|---|---|---|
| Everything off | to measure | to measure | to measure | ≤ 0.6 |
| Pump heaviest | to measure | to measure | to measure | ≤ 1.4 (0.8 and the plugin) |
| Shape heaviest | to measure | to measure | to measure | ≤ 1.6 (1 and the plugin) |
| **Both heaviest**, swing 75% | to measure | to measure | to measure | **≤ 2.4** |
| Four instances (sum) | to measure | to measure | to measure | about 4 × 2.4 |

The Pump and Shape cases measure their real cost once the chain is wired into the plugin; before that
the engine plays only the three levels, and the bench measures the plugin's own work whatever it is
told to switch on.

## Instruction counts

Without the device, ARM instructions per 128-frame block are the measure, as for EffectForce: the
bench's cases through the plain `.so` (the device flags, `PGO=0`) under a plugin-enabled qemu-arm, per
`processReplacing` call. EffectForce's rule of thumb is about 36k-44k instructions to a point of the
block on the Force. They miss what the device adds (cache misses, VFP divisions and flag transfers
that stall), which the device's p99 over its average shows. Not counted yet.

| Case | Thousands of instructions per block (avg / p99) |
|---|---|
| Everything off | to measure |
| Pump heaviest | to measure |
| Shape heaviest | to measure |
| Both heaviest | to measure |

## What keeps it cheap

- **A part that is off costs nothing:** the engine skips it, and switching fades over 10 ms. At the
  defaults (Init) the block is handed back untouched, bit for bit.
- **Control rate:** parameters, the grid's song position and coefficients are worked out once per
  32-sample chunk; every part glides across the chunk.
- **Pure functions of the song position:** a shape and a pump curve are evaluated from where the song is,
  with no state of their own to keep in step, so a loop or a locate costs nothing extra.
  Even the random shapes take their seed from the cycle they are in.
- **NEON where the work is parallel:** left and right run as two lanes of one vector in the state-variable
  filter (the Shape's filter lane and the Pump's crossover); the crossover is three filters, not four.
- **No libm per sample:** EffectForce's fast math (`dsp/fastmath.h`) and per-chunk coefficients; the tan
  behind a cutoff is worked out once per chunk at most.
- **Settled is free:** a level that isn't moving and sits at unity is not touched.
- **The profile-guided build:** `make arm-plugin` trains on the reference mix (`tools/pgo_train.cpp`:
  every option of every choice, the heaviest case, every factory preset) so the compiler knows which
  paths are hot. Release builds are profile-guided in CI too.
- **Cheap getters:** MPC polls names and texts hundreds of times a second; they read caches, lock-free.
- **At most one redraw per 50 ms:** the status line's CPU figure changes now and then; MPC rebuilds its
  parameter screen on each redraw, so the plugin asks for at most one every 50 ms.
