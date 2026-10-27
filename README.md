# GrooveForce

**A groove for the Akai Force: an insert effect that makes a techno groove move on MPC's beat.**

A VST2 insert effect for MPC OS's built-in plugin host, a sibling of
[EffectForce](https://github.com/Devko/EffectForce) and [MasterForce](https://github.com/Devko/MasterForce)
on the same groundwork. It pumps, shapes, echoes and bends time: a shortcut to a groove that works, not
a showpiece. Four parts, **Pump**, **Shape**, **Echo** and **Time**, and 16 one-tap **Moves**.

> [!NOTE]
> **Preview (0.0.2).** All four parts, the Moves and the factory presets are built and pass the full
> suite on x86 and under ARM emulation. The Pump, the Shape and the swing were played on a Force
> (MPC OS 3.x) on 2026-10-26 and work. The Echo, Time and the Moves run on the Force within their CPU
> budget (installed and benched on 2026-10-27) but have not been judged by ear yet. The parameter list
> may still change before v0.1. What it costs on the Force is in
> [docs/PERFORMANCE.md](docs/PERFORMANCE.md); the design is in [docs/DESIGN.md](docs/DESIGN.md).

## What works now

| Part | |
|---|---|
| Pump | A Kickstart-style duck on MPC's grid, with no sidechain: 1/16 to 1 bar, ten named recoveries (Smooth, Punch, Hard, Long, Late...), depth, length, and a push from 10 ms early to 30 ms late. The duck follows the grid, or fires on the input's own transients (Input). It can act on the lows or the highs only, split at 60-300 Hz, and on the dry, the echo or both |
| Shape | Three lanes, Volume, Filter and Pan, each running a named shape (38 of them: waves, the Pump's curves, 16-step patterns, random) at a rate from 1/16 to 4 bars, with its own depth and phase. The filter lane moves a low-pass, band-pass or high-pass, with cutoff and resonance. It acts on the dry, the echo or both |
| Echo | Sigmund-style depth: four stereo delay lines, A to D. Each has a division (1/64 to a bar, with triplets, dotted values and 5/16, 7/16 and 5/8 for polyrhythms) or a time of 1 to 2000 ms, its own feedback, level, pan and mode (stereo, ping-pong, mono), a filter (low-pass, band-pass, high-pass; in the loop or on the output), drive, wow, and a modulator (an LFO on a Shape's shape, an envelope fired by the input's hits, or the input's level) that moves its filter, time and level. Five routings (Parallel, Serial, Pairs, Ring, Spread), send, duck, freeze, width |
| Time | Zero-latency time tricks on the audio: a swing that moves the audio itself (Swing), a loose hand (Drunk) and a laid-back one (Push), placed before or after the other parts, so a tape stop either stops the echoes or lets them ring. Behind it a recorder that never plays the future |
| Moves | 16 one-tap actions on MPC's grid, fired at the next 1/16, beat or bar: Throw Beat, Throw Bar, Dub Freeze, Half-Time Bar, Tape Stop, Slow Down, Reverse Beat, Spin-Back, Stutter 1/8, Stutter 1/16, Roll Build 1 Bar, Roll Build 4 Bars, Filter Drop, Gate 1/16, Break, Rewind. One Shot or Loop; a tile, GO or the automatable Move parameter fires one |
| Swing | 50-75% on 1/16 or 1/8, applied once along the path. With Time's Groove on Swing it moves the audio, so a straight hat loop swings; placed Post, Time moves the Shape's and Pump's accents along and their steps stay straight, placed Pre they swing their steps to match. Otherwise it moves the Shape's, the Pump's and the Echo's LFO steps to match audio that MPC's own note swing has already swung (set it to MPC's swing amount), and on a straight loop it puts accents off the hits |
| Presets | 43 factory presets sorted by job (Kick, Bass, Hats + Perc, Chords, Drum Bus, Perform, Echo, Utility), a browser with favorites and recents, user presets saved from the screen |

Whatever is on, it stays on MPC's song position through loops, locates and tempo changes (stopped, the
grid runs on at the tempo), adds no latency, never clicks (no gain edge is faster than 1 ms, every
jump a 5 ms crossfade) and changes nothing until something is turned up: Init passes the input through
bit for bit. A part that is off costs no CPU (but for Time's recording, once Time has been used).

Five tabs: GROOVE, PUMP+SHAPE, ECHO, TIME and SETUP, each page with its own Q-Link set. Not in it yet:
Hold (a Move while its tile is held) and MIDI-triggered Moves and Pump, which wait for the
[probe](docs/PROBE.md)'s answers about what MPC sends an insert; and a sidechain from another track (an
insert gets two inputs on the Force).

## Documentation

| Document | What's in it |
|---|---|
| [User guide](docs/USER_GUIDE.md) | The pages, every part, swing, the 16 Moves, presets, grooves by job (rumble, rolling bass, hats, dub chords, drum bus, perform), the limits |
| [Design](docs/DESIGN.md) | What it is and why: the use cases, the four parts, how they combine, the pages, the budget, and the rulings made while building |
| [Probe](docs/PROBE.md) | What MPC does with an insert that the design leaves open (MIDI, press and release, automation), and how to find out |
| [Performance](docs/PERFORMANCE.md) | The CPU budget, the bench, and what keeps it cheap |
| [Changelog](CHANGELOG.md) | What changed |

## Requirements

- An **Akai Force** (other first-generation MPC OS devices may work; untested), **MPC OS 3.x**.
- **Root SSH access** to the device (for example through MockbaMod): stock MPC OS has no way to
  install third-party plugins.

## Installation

There is no release yet. Build a package with `make plugin-package`, unzip it and follow the
`INSTALL.md` inside:

```sh
scp -r GrooveForce-<version> root@<device-ip>:/tmp/
ssh -t root@<device-ip> sh /tmp/GrooveForce-<version>/install.sh
```

The installer asks for confirmation, **stops MPC** (save your project first), copies the plugin,
backs up and edits `MPC.settings`, and starts MPC again. Then put **GrooveForce** on a track's insert.
From a checkout it is one step, with `FORCE=root@<device-ip>` on the line or in `local.mk`; it
restarts MPC too:

```sh
make plugin-install
```

A package built on a newer distribution than MPC OS 2.x's links a newer glibc (the build says so): it
loads on MPC OS 3.x only, and the plugin catalog refuses it. Releases come from CI, built against glibc
2.31.

## Building

Linux or WSL, as for the siblings (g++, `arm-linux-gnueabihf-g++` and qemu-user for the device build):

```sh
make test                  # the parts and the plugin under ASan/UBSan
make test-arm              # the same suite built for the Force, under qemu-arm
make test-module M=grid    # one module's tests on their own
make arm-plugin            # build/arm/grooveforce.so (profile-guided where qemu-arm runs)
make test-arm-pgo          # the suite against the objects that .so is linked from
make levels                # how loud each factory preset comes out against its reference material
make preset-levels         # sets their Output so each comes out about as loud as it went in
make bench                 # the cases of tools/cases.h timed on this machine (x86 numbers say nothing about the Force)
make bench-count           # the same cases in ARM instructions per block, counted under qemu (QEMU_PLUGINS in local.mk)
make bench-device          # the same cases timed on the Force (FORCE=root@<ip> in local.mk)
make skin preview          # the skin, and every page as surface/build/page_*.png
make plugin-package        # dist/GrooveForce-<version>-mpc-armv7.zip
make plugin-install        # onto the Force; restarts MPC
```

The bench has ten cases: Init, each part at its heaviest, four instances at once and everything at its
heaviest. Their settings are in `tools/cases.h` (shared with the profile-guided build's trainer); what they
cost on the Force, in time and in ARM instructions, is in [docs/PERFORMANCE.md](docs/PERFORMANCE.md).

`surface/surface.py` is the single source of the parameter list, the pages, the shape library, the Time
patterns, the 16 Moves and the factory presets' checks. `make arm-plugin` plays the reference mix
through an instrumented copy of the plugin (`tools/pgo_train.cpp`: every option of every choice, the
heaviest case, every factory preset) and builds the shipped `.so` with that profile. Release packages
come from CI ([`.github/workflows/build.yml`](.github/workflows/build.yml)): the device build in
`arm32v7/gcc:11-bullseye` (glibc 2.31) under QEMU, profile-guided, checked with the plugin catalog's
`catalog_check.py`; a `vX.Y.Z` tag publishes it with its CHANGELOG section.

## License

MIT, see [LICENSE](LICENSE). `dsp/` holds EffectForce's shared pieces (simd, fastmath, the SVF) and
GrooveForce's own (the grid, the crossover, the Pump's curves, the shape library, the Echo, whose lines
are EffectForce's Delay taken apart, and Time); `plugin/` and `surface/` are MasterForce's plugin layer,
cut down and given GrooveForce's parameters and the Moves; `third_party/mpc-vst-plugins/` is sd88me's
skin generator and installer (MIT), vendored with PolyForce's patches.
