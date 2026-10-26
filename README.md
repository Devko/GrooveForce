# GrooveForce

**A groove for the Akai Force: an insert effect that makes a techno groove move on MPC's beat.**

A VST2 insert effect for MPC OS's built-in plugin host, a sibling of
[EffectForce](https://github.com/Devko/EffectForce) and [MasterForce](https://github.com/Devko/MasterForce)
on the same groundwork. It pumps, shapes, echoes and bends time: a shortcut to a groove that works, not
a showpiece.

> [!NOTE]
> **Preview: Pump and Shape. Not released yet.** Two of the four parts are built, the Pump and the
> Shape, with the swing they share; the Echo and Time come after them. What exists passes the full suite
> on x86 and under ARM emulation. It has not been played on a Force yet, so there are no device numbers
> ([docs/PERFORMANCE.md](docs/PERFORMANCE.md)). The parameter list may still change before v0.1.
> The design is in [docs/DESIGN.md](docs/DESIGN.md).

## What works now

| Part | |
|---|---|
| Pump | A Kickstart-style duck on MPC's grid, with no sidechain: 1/16 to 1 bar, ten named recoveries (Smooth, Punch, Hard, Long, Late...), depth, length, and a push from 10 ms early to 30 ms late. The duck follows the grid, or fires on the input's own transients (Input). It can act on the lows or the highs only, split at 60-300 Hz |
| Shape | Three lanes, Volume, Filter and Pan, each running a named shape (38 of them: waves, the Pump's curves, 16-step patterns, random) at a rate from 1/16 to 4 bars, with its own depth and phase. The filter lane moves a low-pass, band-pass or high-pass, with cutoff and resonance |
| Swing | 50-75% on 1/16 or 1/8, applied once along the path, so a swung pump and a swung accent land on the same late 16ths. In this preview it moves the Shape's and the Pump's steps to match audio that MPC's own note swing has already swung (set it to MPC's swing amount); it doesn't swing the audio, which comes with the Time part, and on a straight loop it puts accents off the hits |
| Presets | Factory presets sorted by job, a browser with favorites and recents, user presets saved from the screen |

Whatever is on, it stays on MPC's song position through loops, locates and tempo changes (stopped, the
grid runs on at the tempo), adds no latency, never clicks (no gain edge is faster than 1 ms) and changes
nothing until something is turned up: Init passes the input through bit for bit.

## Documentation

| Document | What's in it |
|---|---|
| [Design](docs/DESIGN.md) | What it is and why: the use cases, the four parts, how they combine, the pages, the budget |
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
From a checkout it is one step, with `FORCE=root@<device-ip>` on the line or in `local.mk`:

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
make bench                 # the bench on this machine (x86 numbers say nothing about the Force)
make bench-device          # CPU on the Force (FORCE=root@<ip> in local.mk)
make skin preview          # the skin, and every page as surface/build/page_*.png
make plugin-package        # dist/GrooveForce-<version>-mpc-armv7.zip
make plugin-install        # onto the Force; restarts MPC
```

`surface/surface.py` is the single source of the parameter list, the pages and the factory presets'
checks. `make arm-plugin` plays the reference mix through an instrumented copy of the plugin
(`tools/pgo_train.cpp`: every option of every choice, the heaviest case, every factory preset) and
builds the shipped `.so` with that profile. Release packages come from CI
([`.github/workflows/build.yml`](.github/workflows/build.yml)): the device build in
`arm32v7/gcc:11-bullseye` (glibc 2.31) under QEMU, profile-guided, checked with the plugin catalog's
`catalog_check.py`; a `vX.Y.Z` tag publishes it with its CHANGELOG section.

## License

MIT, see [LICENSE](LICENSE). `dsp/` holds EffectForce's shared pieces (simd, fastmath, the SVF) and
GrooveForce's own (the grid, the crossover, the Pump's curves, the shape library); `plugin/` and
`surface/` are MasterForce's plugin layer, cut down and given GrooveForce's parameters;
`third_party/mpc-vst-plugins/` is sd88me's skin generator and installer (MIT), vendored with PolyForce's
patches.
