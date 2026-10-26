# The probe

Before GrooveForce is built on what MPC does with an insert, the **GrooveForce Probe** finds out
three things the [design](DESIGN.md#open) leaves open. It is EffectForce's probe (a small tempo
delay that logs every call MPC makes) with MIDI switched on and two controls added: a button and a
tile. It builds on its own in `probe/`. Plan 2 built Time and the Moves without waiting for the
results (2026-10-26): Moves are One Shot and Loop, played from the tiles, GO, the Q-Links and the
automatable Move parameter, and the plugin tells MPC it takes no MIDI. What still waits for these
answers is Hold, MIDI-triggered Moves and Pump, and whether recorded automation plays back into an
insert. The results table at the end is still to be filled in on the Force.

- [What it answers](#what-it-answers)
- [What the siblings already know](#what-the-siblings-already-know)
- [Running it on the Force](#running-it-on-the-force)
- [Reading the log](#reading-the-log)
- [Results](#results)

---

## What it answers

From the design's Open table:

| Question | How it's answered | What changes |
|---|---|---|
| Does MPC send MIDI to an insert on the Force (pads, a MIDI track)? | A probe build that says it takes MIDI and logs what arrives | Yes: Pump gets a MIDI trigger and the Move tiles can be played from pads. No: tiles, Q-Links and automation only |
| Does a skin button give press and release, so a Move can play while held? | The same probe | Yes: Hold mode. No: One Shot and Loop only |
| Does MPC play recorded automation back into an insert? (EffectForce's probe still has it pending) | The same probe: record the Move parameter in a sequence, play it back | Yes: Fills are sequenced by automating the Move. No: Moves are played live only, and Fills move to performance |

## What the siblings already know

From MasterForce's `plugin/surface.h`: a button always reads back 0 and **no release ever follows**
a press. A tile tap sends a **release echo about 0.7 s later**. The probe checks whether that echo's
timing follows how long the finger stays down (a hold) or is fixed.

## Running it on the Force

Install (stops and restarts MPC: save the project first; `FORCE=root@<device-ip>` on the line or in
`local.mk`):

```bash
make -C probe plugin-install
```

Then, in MPC:

1. Add *GrooveForce Probe* as an insert on a drum track. Play its pads. Then play notes into the
   same track from a MIDI track or the Force's keys.
2. Tap Hold (second page, PRESS). Then press and hold it for about 2 s. Do the same with Tile.
3. Record a Q-Link move of Mix into a sequence. Play it back without touching anything.
4. `make -C probe probe-log`.

`make -C probe probe-clear` empties the log, to start a step over.

Uninstall (also restarts MPC):

```bash
make -C probe plugin-uninstall
```

## Reading the log

`/tmp/grooveforce_probe.log` on the device. A line starts with the wall-clock time and `#n`, the
instance.

| Line | Means |
|---|---|
| `MPC asked canDo("receiveVstMidiEvent"): we said 1` | The probe told MPC it takes MIDI |
| `MIDI 90 3C 64, delta 17 frames, stopped` | A MIDI event arrived: the bytes, where in the block, and whether MPC was playing (`playing 6.50`: at beat 6.50) or not |
| `param hold 1.000 +0 ms stopped (set on another thread)` | A write to Hold (`tile_1` for the tile): its value, the ms since the write before, the transport |
| `param mix 0.250 playing 6.50 (25%, set on an audio thread, 8 writes)` | Mix changed: the value, the transport, which thread, how many writes in the 100 ms |

The Events readout on the first page shows the MIDI count and the last event: `MIDI 12, last 90 3C 64`.

For the questions:

- **MIDI:** `MIDI` lines, from the pads and from the MIDI track. None, and the count stays 0: no.
- **Press and release:** after a Hold press, does a `hold 0.000` line follow, and when? Only `1.000`
  lines: no release. For Tile, the second line is the echo: its `+ms` is about 700 for a tap; after
  a 2 s hold it is about 2000 if the echo follows the finger, about 700 if it is fixed.
- **Automation:** `mix` lines while the sequence plays and nothing touches the Q-Link, with
  `playing` and the beat, and `set on an audio thread` or not.

## Results

Not run yet.

| Question | Answer | Log lines |
|---|---|---|
| MIDI into an insert (pads, a MIDI track) | | |
| Hold: press and release | | |
| Tile: the release echo, tap and hold | | |
| Automation played back into an insert | | |
