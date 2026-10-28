# Changelog

Releases are built by CI from a `vX.Y.Z` tag ([`.github/workflows/build.yml`](.github/workflows/build.yml));
the section for the tag's version becomes the release's notes. While the version is 0.x the parameter
list may still change between releases.

## Unreleased: GrooveForce is now GrooveWerk <!-- werk-rename: keep -->

**A new name, the same groove.** "Force" is the name of Akai's device, so it no longer belongs in this
plugin's name. Nothing about the sound changes.

- MPC's plugin list shows **GrooveWerk**, and the plugin lives in `/sdcard/Synths/Devko - VST - GrooveWerk`.
- Your presets, favorites and recents move over by themselves: the installer carries them out of the old `Devko - VST - GrooveForce` folder and removes it, and the first start renames `/media/AkaiForce/GrooveForce Presets` to `GrooveWerk Presets`. <!-- werk-rename: keep -->
- Projects saved with GrooveForce open with GrooveWerk and their settings: the plugin keeps GrooveForce's plugin ID, and it reads presets and projects saved under the old name (the same way checked with SubWerk on a Force). <!-- werk-rename: keep -->

## 0.0.2

- **Echo: every routing closes over the lines that are on**, as the Ring already did. A line whose
  source is off takes what that source would have taken: in **Pairs** B takes the send when A is off
  (and D when C is off), a **Serial** chain skips a line that is off (with B off, C hears A), and in
  **Spread** B, C and D take the send when A is off. In 0.0.1 a line switched on could stay silent,
  so that in Pairs only some lines seemed to work. Switching still crossfades in about 1 ms.

## 0.0.1

The first version, a preview: four parts, **Pump**, **Shape**, **Echo** and **Time**, 16 one-tap
**Moves** and factory presets by job. The Pump, the Shape and the swing were played on a Force
(MPC OS 3.x) on 2026-10-26 and work. The Echo, Time and the Moves pass the test suite on x86 and under
ARM emulation and run on the Force within their CPU budget (everything at its heaviest: 10% of a block at
p99), but have not been judged by ear yet.

- **Pump:** a duck on MPC's grid with no sidechain: five rates (1/16 to 1 bar), ten named recoveries,
  depth, length, a push from 10 ms early to 30 ms late; locked to the grid or fired by the input's own
  transients; on the whole signal, the lows or the highs, split by a Linkwitz-Riley pair at 60-300 Hz.
  It acts on the dry, the echo (the rumble: the kick ducks only its own echo) or both.
- **Shape:** three lanes (Volume, Filter, Pan), each running one of 38 named shapes (waves, the Pump's
  curves, 16-step patterns, random) at a rate from 1/16 to 4 bars, with its own depth and phase; the
  filter lane moves a low-pass, band-pass or high-pass with cutoff and resonance. It acts on the dry,
  the echo or both.
- **Echo:** four stereo delay lines, A to D, each a division (19 of them, 1/64 to a bar, with
  triplets, dotted values and 5/16, 7/16 and 5/8 for polyrhythms) or 1 to 2000 ms, with feedback, level,
  pan, a mode (stereo, ping-pong, mono), a filter (low-pass, band-pass or high-pass, in the loop or on
  the output), drive, wow, and a modulator (an LFO on one of the 38 shapes, an envelope fired by the
  input's hits, or the input's level) that moves the line's cutoff, time and level. Five routings
  (Parallel, Serial, Pairs, Ring, Spread), a send, a duck, a freeze, a width and an Echo First or Echo
  Last order. In a Ring a line's feedback is how much goes on round the ring, so the ring never runs
  away; it closes over the lines that are on. A line that is off passes nothing down Serial, Pairs or
  Spread. Freeze holds what is in the lines in every routing. A resonant filter in the loop keeps its
  tone in the repeats, but the loop never gains.
- **Time:** time tricks on the audio at zero latency, placed before or after the other parts. The
  groove is Swing (every second 16th or 8th comes late by the swing amount, with a 5 ms crossfade that
  ends on the grid line, no pitch bend), Drunk (each 16th a fixed few milliseconds late, the same every
  bar) or Push (everything a little late). It records whenever it has been used, so Reverse Beat,
  Rewind, Spin-Back and Tape Stop always have audio.
- **Moves:** 16 actions on the grid, fired by a tile, GO or the automatable Move parameter at the next
  1/16, beat or bar, One Shot or Loop: Throw Beat, Throw Bar, Dub Freeze, Half-Time Bar, Tape Stop,
  Slow Down, Reverse Beat, Spin-Back, Stutter 1/8, Stutter 1/16, Roll Build 1 Bar, Roll Build 4 Bars,
  Filter Drop, Gate 1/16, Break, Rewind. A Move switches on the parts it needs; the Echo it opened
  keeps ringing until its repeats are gone. The Move parameter holds the selected Move; None stops one.
  No Hold and no MIDI yet: they wait for the probe.
- **Swing:** 50-75% on 1/16 or 1/8, applied once along the path. With Time's Groove on Swing it moves
  the audio, so a straight hat loop swings. Placed Post, Time moves the Shape's and Pump's accents
  along with the hits and their steps stay straight; placed Pre, they swing their steps to match.
  Otherwise it moves the Shape's, the Pump's and the Echo's LFO steps to match audio that MPC's own
  note swing has already swung (set it to MPC's swing amount); the audio is then not moved, and on a
  straight loop it puts accents off the hits.
- **Two buses:** a Pump or Shape on All acts on the dry and the echo alike, and starts on both in step
  when the Echo comes on. Changing the Echo's order crossfades over 10 ms.
- **The plugin:** zero latency, on MPC's song position through loops, locates and tempo changes, no gain
  edge faster than 1 ms, and Init passes the input through bit for bit. Five tabs: GROOVE (GROOVE and
  PRESETS), PUMP+SHAPE, ECHO (ECHO and LINE A to D), TIME (TIME and MOVES) and SETUP. 43 factory presets by
  job, favorites, recents and user presets.
- **Limits:** the Pump's Input trigger finds hits that stand out about 9-12 dB over what is sustained
  under them (a kick over a bass needs Grid); the banded Pump's split can raise the peaks of an edgy
  waveform by up to about 9 dB; there is no sidechain from another track (an insert gets two inputs on
  the Force); a Shape filter moved onto an echo that is already sounding rings in. All in the
  [user guide](docs/USER_GUIDE.md#the-limits).
- **Build:** `make arm-plugin` is profile-guided where qemu-arm runs (the trainer plays the reference
  mix through every option of every choice, the heaviest case and every factory preset); `make bench`
  and `make bench-device` time the bench's ten cases (Init, each part at its heaviest, four instances,
  everything at its heaviest; their settings are in tools/cases.h) on this machine and on the Force,
  `make bench-count` counts their ARM instructions per block under qemu, and the numbers are in
  [docs/PERFORMANCE.md](docs/PERFORMANCE.md); `make preset-levels` level-matches the factory presets
  on reference material; CI builds in `arm32v7/gcc:11-bullseye` (glibc 2.31), runs the suite on x86
  under ASan/UBSan and on ARM, and checks the zip with the plugin catalog's `catalog_check.py`.
