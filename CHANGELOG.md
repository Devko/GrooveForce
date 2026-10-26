# Changelog

Releases are built by CI from a `vX.Y.Z` tag ([`.github/workflows/build.yml`](.github/workflows/build.yml));
the section for the tag's version becomes the release's notes. While the version is 0.x the parameter
list may still change between releases.

## Unreleased

The groundwork and the first two parts, **Pump** and **Shape**: a preview, not played on a Force yet.

- **Pump:** a duck on MPC's grid with no sidechain: five rates (1/16 to 1 bar), ten named recoveries,
  depth, length, a push from 10 ms early to 30 ms late; locked to the grid or fired by the input's own
  transients; on the whole signal, the lows or the highs, split by a Linkwitz-Riley pair at 60-300 Hz.
- **Shape:** three lanes (Volume, Filter, Pan), each running one of 38 named shapes (waves, the Pump's
  curves, 16-step patterns, random) at a rate from 1/16 to 4 bars, with its own depth and phase; the
  filter lane moves a low-pass, band-pass or high-pass with cutoff and resonance.
- **Swing:** 50-75% on 1/16 or 1/8, applied once along the path. In this preview it moves the Shape's
  and the Pump's steps to match audio that MPC's own note swing has already swung (set it to MPC's
  swing amount); it doesn't swing the audio (that comes with the Time part), and on a straight loop it
  puts accents off the hits.
- **The plugin:** zero latency, on MPC's song position through loops, locates and tempo changes,
  no gain edge faster than 1 ms, and Init passes the input through bit for bit. Pages GROOVE and
  PRESETS, PUMP and SHAPE, SETUP; factory presets, favorites, recents and user presets.
- **Build:** `make arm-plugin` is profile-guided where qemu-arm runs (the trainer plays the reference
  mix through every option of every choice, the heaviest case and every factory preset);
  `make bench` and `make bench-device` time five cases (everything off, the Pump and the Shape at their
  heaviest, both with swing at 75%, four instances); CI builds in `arm32v7/gcc:11-bullseye` (glibc
  2.31), runs the suite on x86 under ASan/UBSan and on ARM, and checks the zip with the plugin
  catalog's `catalog_check.py`.
