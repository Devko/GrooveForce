# GrooveForce: design

A sibling of EffectForce for grooves: an insert effect for the Force that makes a techno groove move
on MPC's beat. It pumps, shapes, echoes and bends time. It is a shortcut to a groove that works, the
way ShaperBox or Kickstart are, not a showpiece.

- [Decided with the user](#decided-with-the-user)
- [What the Force does](#what-the-force-does)
- [Use cases](#use-cases)
- [The signal path and combining](#the-signal-path-and-combining)
- [The parts](#the-parts)
- [Moves](#moves)
- [Pages](#pages)
- [Presets](#presets)
- [Budget](#budget)
- [Success and failure](#success-and-failure)
- [Not in it](#not-in-it)
- [Plan](#plan)
- [Plan 2 rulings (2026-10-26)](#plan-2-rulings-2026-10-26)
- [Open](#open)

This is the design as approved on 2026-10-25, kept as the record. Plan 2 built the Echo, Time, the
Moves and the factory presets (2026-10-26) and made calls the design left open; where the built
plugin differs from the text below, [Plan 2 rulings](#plan-2-rulings-2026-10-26) says how. The
[user guide](USER_GUIDE.md) describes the plugin as built.

---

## Decided with the user

2026-10-25:

- **Purpose:** getting a good techno groove is hard. This makes it easier. It is a tool for
  getting there faster, not an effect to admire for its own sake.
- **References:** D16 Sigmund 2 (four delay lines with in-loop filters, drive and modulators that
  follow transients), ShaperBox 3 (tempo-locked shapes on volume, filter and pan), Kickstart 2
  (a sidechain pump without a sidechain), Akai Flex Beat (Gross Beat-style time and volume patterns
  on 16 pads; Akai doesn't offer it for the Force).
- **All four parts, used together:** Pump, Shape, Echo, Time. Combining them is the point.
- **Echo must be deep**, Sigmund-style, to cover very different grooves.
- **Time is all of Flex Beat, not just chops:** stutter, gates, reverse, half-time, tape stop,
  pitch.
- **Techno first:** 4/4 at about 120-150 BPM. The presets, defaults and rates assume it. Other
  styles work but aren't what it's tuned for.
- **On any track:** single sounds (kick, bass, hats, stabs), a drum bus or the master, "depending on
  what I want to achieve".
- **A plugin of its own**, not an EffectForce module. With every module on at its heaviest,
  EffectForce's p99 is 14.9% of its 15% budget. Its five tabs are full. Its parameter list freezes at
  v0.1. A groove tool will change shape while it is tried on real grooves. It reuses EffectForce's
  `dsp/` code and groundwork, as MasterForce does. Named GrooveForce, repo
  [Devko/GrooveForce](https://github.com/Devko/GrooveForce).
- **The rest was handed over** ("think about different use-cases, document and recommend them for
  me", 2026-10-25). The use cases, the parts' details, Moves, the presets and the plan below are
  Claude's calls.

## What the Force does

From EffectForce's [probe](https://github.com/Devko/EffectForce/blob/main/docs/PROBE.md) and its
design:

| | |
|---|---|
| Audio | 44.1 kHz; always 128-frame blocks, in place; 2 inputs, 2 outputs |
| Sidechain | **None:** an insert gets 2 inputs. Nothing can hear a kick on another track |
| Tempo and position | Valid while playing and stopped: tempo, ppq, bars, cycle (loop) position. Everything locks to the song position, as EffectForce's Pulse and looper do |
| Latency | Not compensated (`acceptIOChanges`: no). GrooveForce is zero-latency |
| Pages | MPC skins can't draw changing graphics (no drawn curves). Five tab groups show without a pager. Each page has its own Q-Link set |
| MIDI into an insert | **Untested.** The host says it can send MIDI. EffectForce tells MPC it takes none. See [Open](#open) |
| Getters | Polled about 600 times a second with the screen open; every getter must be cheap. 2026-10-26 (plan 1's ruling R22): not lock-free, as first written. Values are atomics; the texts of the stepper, the tiles and the readouts are a cache behind a mutex that only the UI thread takes. The audio thread never takes it |
| Stop | Suspends and resumes within 100 ms. Under 250 ms keeps every buffer (EffectForce's rule) |
| CPU | 2902 µs per block per instance; a plugin passes at p99 ≤ 15% |

## Use cases

The jobs GrooveForce is for, by the track it sits on. **Must** is v0.1, **should** comes once the
musts work, **later** is after 0.1. Each must has factory presets (see [Presets](#presets)).

### Kick

| Use case | What it does | Parts | |
|---|---|---|---|
| **Rumble** | The kick's tail becomes a dark, rolling bed between kicks, the hypnotic-techno rumble. Two lines (1/16 and 1/8., or 3/16) in parallel, a low-pass around 150-250 Hz and drive inside the loop, feedback 0.6-0.8, kept mono. Pump ducks only the echo on every kick, so the rumble swells between kicks and never covers the next one. The dry kick is untouched | Echo, Pump (target Echo) | must |
| Tail trim | A boomy kick shortened to the groove: a volume shape per beat | Shape | later |

### Bass

| Use case | What it does | Parts | |
|---|---|---|---|
| **Bass pump** | Kickstart on the bass track: a duck on every beat, its curve and depth picked by name, locked to the grid. No sidechain needed | Pump | must |
| **Rolling bass** | One offbeat bass hit becomes a 16th roll: a 1/16 line, feedback around 0.5, each repeat darker through the loop's filter. Pump keeps the beat free for the kick | Echo, Pump | must |
| Offbeat gate and acid movement | A volume shape on the offbeats and a filter shape per 16th | Shape | should |

### Hats and percussion

| Use case | What it does | Parts | |
|---|---|---|---|
| **Hat swing** | Real swing on audio: every second 16th of a straight hat loop or sample comes late by the swing amount, MPC-style (50% straight, 66% a triplet feel). For loops and resampled grooves, where MPC's own swing (on notes) can't reach | Time (groove) | must |
| **Polyrhythm** | A rim, clap or perc hit echoes at 3/16 and 5/16 (and 7/16) against the 4/4, the lines panned apart with a high-pass in the loop. A pattern that doesn't repeat every beat | Echo | must |
| Accents and shuffle | A volume shape per 16th that accents the offbeats and softens the rest, and a pan shape | Shape | should |
| Ping-pong hats | 1/8. ping-pong, high-pass in the loop, short feedback | Echo | should |

### Stabs and chords

| Use case | What it does | Parts | |
|---|---|---|---|
| **Dub chord echo** | Dub techno: 1/8. and 1/4 lines, feedback around 0.7, band-limited in the loop (about 300 Hz to 2.5 kHz) with drive and wow. A slow shape moves the lines' filter, and Pump keeps the wash off the kick | Echo, Shape, Pump | must |
| **Echo throw** | The echo only hears the stab when it is thrown: a Move opens the echo's send for the last beat of a phrase and raises the feedback, then closes. The tail rings out | Echo, Move | must |
| Chord gate and pump | A trance-gate shape (EffectForce Pulse's 16 patterns) with a pump | Shape, Pump | should |

### Drum bus and loops

| Use case | What it does | Parts | |
|---|---|---|---|
| **Fills** | A stutter or roll (1/8 to 1/32), half-time, reverse or a gate on the last beat or bar, on the grid. Played from the screen, or sequenced by automating the Move | Time, Move | must |
| Loop groove | Swing and accents on a whole drum loop | Time, Shape | should |
| Low pump | The pump only below about 120 Hz, so the bass breathes with the kick but the hats don't pump | Pump (band Lows) | should |

### Master and performance

| Use case | What it does | Parts | |
|---|---|---|---|
| **Breakdown moves** | A tape stop into the break, a half-time last bar, a reversed beat, an echo freeze: one tap, on the grid | Move | must |
| Build | A roll that speeds up over 4 or 8 bars, a filter rising and the echo's feedback climbing, landing on the downbeat | Move | should |
| Glue pump | A gentle pump of the whole mix, lows only | Pump | later |

### Recommendation

Build the nine musts first, in the order of the [Plan](#plan). Together they use every part and each
way of combining them. Rumble, Rolling bass and Dub chord echo prove the Echo with Pump. Hat swing
proves Time's groove. Polyrhythm proves the Echo's depth. Throw, Fills and Breakdown moves prove
Moves. If those nine sound right on the device, the shoulds are presets and a few options, not new
machinery.

## The signal path and combining

```
IN ─→ [Time, Pre] ─┬─→ dry ────────────────┬─→ Shape ─→ Pump ─→ [Time, Post] ─→ Mix ─→ Guard ─→ OUT
                   └─→ send ─→ Echo ─→ wet ─┘
```

Combining works on five levels:

1. **Targets.** Shape and Pump each act on **All**, **Dry** or **Echo** (the wet only). Rumble is
   Pump on Echo; a bass pump is Pump on All.
2. **Order.** **Echo first** (above, the default): the echo hears the input, and Shape and Pump move
   what comes out. **Echo last**: Shape and Pump come first and the echo hears the shaped signal, so
   a gated chord echoes as a rhythm. Echo last ignores the targets: Shape and Pump act on the dry.
3. **Time's place.** **Post** (the default): a tape stop stops everything, echoes included. **Pre**:
   only the input is bent, and the echo rings on through a stop. This is EffectForce's looper rule (a
   fixed stage, Pre or Post).
4. **One grid.** A global **Swing** (50-75%) on 1/16 or 1/8 applies to Time's groove, Shape's steps
   and Pump below 1/4. Swung hats, accents and pumps land on the same late 16ths. **Swing is applied
   once along the path** (my call, from the design review). Shape and Pump swing their steps only if
   the audio reaching them is already swung. If Time's Groove swings and Time is Post, Shape and
   Pump stay straight: Time moves their accents along with the hits. Otherwise they swing: after a
   Pre swing, or on audio that MPC's own note swing has already swung. Swinging the steps and then
   the audio would put each accent one swing late, on the hit before it.
5. **Moves** (below): one tap drives several parts at once over a length on the grid.

Across tracks, combining comes for free. Every copy locks to MPC's song position, so a pump on the
bass and a shape on the pads land on the same beats.

Every part can be switched on or off. Off costs no CPU and switching fades (10 ms), as in EffectForce's
rack. A part's buffers are allocated the first time it is switched on (UI thread), as EffectForce's
looper does, so a copy that only pumps holds no delay lines.

## The parts

### Pump

Kickstart, locked to the grid:

- **Rate:** 1/16, 1/8, 1/4 (default), 1/2, 1 bar.
- **Curve:** one of about 12 named curves (Smooth, Punch, Hard, Long, Late, Bounce...). A curve is
  a duck at the step's start that recovers across the step. **Depth** sets how deep (0 to full
  silence) and **Length** how much of the step the recovery takes.
- **Push:** moves the duck up to 10 ms early or 30 ms late. The grid is known ahead from the song
  position, so ducking before the beat needs no look-ahead.
- **Trigger:** **Grid** (default), or **Input**, where a transient detector on the input triggers
  each duck. Input is for a drum bus, or for Rumble on a kick track where the kicks aren't on every
  beat. **MIDI** only if the probe finds that MPC sends MIDI to an insert.
- **Band:** **Full**, **Lows** or **Highs**, split at a crossover (60-300 Hz, default 120) by a
  Linkwitz-Riley pair, so the bands sum flat.
- **Target** (All, Dry, Echo) and **Mix**.

Every duck is click-free: its fastest edge takes at least 1 ms, the way Pulse's gate has a shortest
edge.

### Shape

ShaperBox without the drawing. There are three lanes, each switched on on its own:

| Lane | What it moves | Its own settings |
|---|---|---|
| Volume | Gain, 0 to unity (never a boost) | |
| Filter | A resonant state-variable filter's cutoff, by up to ±4 octaves | Type (LP, BP, HP), Cutoff, Resonance |
| Pan | Constant-power balance (Pulse's) | |

Each lane has a **Shape**, a **Rate** (1/16 to 4 bars), a **Depth** (bipolar for Filter and Pan) and
a **Phase**. The part has one **Target** (All, Dry, Echo) and a **Mix**.

**Shapes** are named and picked, not drawn: about 32 in a library compiled in. Each is a short list
of breakpoints with a curve between each pair, ShaperBox's model, held as data in `surface.py` and
checked there. The library covers:

- **Waves:** sine, triangle, saws, ramps, square with soft edges.
- **Pumps:** the Pump's curves, for a filter that ducks with the volume.
- **Steps:** 16-step patterns. Pulse's 16 gate patterns, plus accent patterns that open steps at
  levels rather than on/off (offbeat accent, rolling 16ths, gallop, shuffle).
- **Random:** stepped and smooth, seeded per bar so it repeats with the loop.

The steps follow the global Swing.

### Echo

Sigmund's depth on EffectForce's delay. There are four lines, **A** to **D**, and each is a stereo
line built from EffectForce's `Delay`:

| Per line | |
|---|---|
| On, Level, Pan | |
| Time | Synced: EffectForce's 16 divisions plus 3/16, 5/16, 7/16 and 3/8 (for polyrhythms). Or free, 1-2000 ms |
| Feedback | 0-1; 1 holds the repeats (the limiter in the loop keeps it bounded, as in EffectForce) |
| Mode | Stereo, Ping-Pong, Mono (EffectForce's) |
| Filter | A resonant SVF (LP, BP, HP; cutoff, resonance), **In loop** (each repeat filtered again: rumble, dub) or **On output** (every repeat the same: a wah over the echoes). A fixed one-pole low cut stays in the loop at 30 Hz, so nothing builds up below the audible range |
| Drive | In the loop (EffectForce's cubic soft clipper) |
| Wow | EffectForce's wow and flutter |
| Modulator | **LFO** (a shape from Shape's library, at a rate), **Env** (an attack-decay envelope fired by input transients, Sigmund's transient detector) or **Follow** (an envelope follower). Three amounts: to Cutoff, to Time (small, glided: a pitch wobble) and to Level |

The whole Echo has:

- **Routing**, five topologies. Parallel (all four from the send). Serial (A → B → C → D, each
  line's output into the next). Pairs (A → B and C → D, in parallel). Ring (A → B → C → D → A, cross
  feedback, dense). Spread (A into B, C and D: one hit, then a cloud). Every line's output goes to
  the mix at its Level whatever the routing. A routing change crossfades over one chunk.
- **Send:** how much of the input reaches the lines (automatable; Throw moves open it).
- **Duck:** the wet ducks under the input (EffectForce's).
- **Freeze:** feedback to 1 and the send closed; the lines loop what they hold.
- **Glide** (Tape or Fade, EffectForce's), **Width** and **Mix**.

Each line holds 1 bar at 80 BPM (3 s). The lines are allocated when the Echo is first switched on.

Echo presets cover the grooves: Rumble, Dub, Roll, Polyrhythm, Ping-Pong, Tape, Slap, Spread Cloud,
Freeze.

### Time

Flex Beat's time patterns, at zero latency. An always-on recorder ring holds the last 2 bars at 80
BPM. The output plays from a **playhead** that is either now or behind it. Every jump crossfades
(EffectForce's looper: a 5 ms raised-cosine crossfade, 4-point Hermite reads, a level that fades as
the speed falls).

Zero latency means the playhead never runs ahead of now. A pattern that seems to need the future,
such as double speed, plays a captured slice twice. When a pattern ends, the playhead rejoins now
on a grid line, crossfaded.

There are two ways to use Time:

- **Groove** (continuous, on the TIME page). **Swing** delays every second 16th (or 8th) by the
  global Swing amount. Variants: Swing, Drunk (a seeded random late of a few ms per 16th, repeating
  per bar), Push (all hits a few ms late, for laid-back hats), Off (default). Swing's delay is
  (swing − 50%) × an 8th: at 120 BPM, 58% is 20 ms and 66% is 40 ms.
- **Moves** (one-shots on the MOVES page, below). Stutter, roll, half-time, reverse, tape stop,
  spin-back and gates are Time patterns inside Moves.

A **Time pattern** is a length (1/4, 1/2, 1 bar, 2 bars) plus two curves, a playhead offset (or
speed) curve and a volume curve. It is held as data like the shapes. That is Flex Beat's and Gross
Beat's model, without the editor.

## Moves

A Move is a named, one-tap action on the grid that drives several parts at once over its length,
then returns. It is EffectForce's FX library and scene moves, put on Flex Beat's pads. A Move is:

- an optional **Time pattern**;
- up to four **parameter ramps** (for example, the Echo's send to full and its feedback to 0.8 over a
  beat, or the Shape filter's cutoff down two octaves over a bar), relative to where the knobs are,
  and back at the end;
- a **mode**: **One Shot** (plays its length once from the next grid line), **Loop** (repeats until
  stopped) or **Hold** (while held, if the probe finds a skin button gives press and release; see
  [Open](#open)). Plan 2 built One Shot and Loop only: Hold waits for the probe (R24);
- a **launch quantize**: next 1/16, beat or bar.

The MOVES page shows **16 Move tiles** (Flex Beat's pads). The factory set, my call: Throw Beat,
Throw Bar, Dub Freeze, Half-Time Bar, Tape Stop, Slow Down (half speed reached over a bar), Reverse
Beat, Spin-Back, Stutter 1/8, Stutter 1/16, Roll Build 1 Bar, Roll Build 4 Bars, Filter Drop, Gate
1/16, Break (the dry drops out, the echo rings), Rewind.

Time's place changes how every Move sounds, and a Move has no place of its own (my call). On Post,
a tape stop stops the echoes too. On Pre the echoes ring on through it. Presets pick the place their
Moves are meant for.

Firing (my calls, from the design review):

- **A tile fires its Move** at the launch quantize. Mode and quantize belong to the page and apply to
  every Move.
- **The selected Move is also a parameter**, with a **None** value. A change of it fires the new Move
  at the quantize, so automating it in a sequence programs breaks per bar: fills get sequenced
  without MIDI. Whether MPC plays automation back into an insert is still a probe item (see
  [Open](#open)).
- **A Move switches on the parts it needs**, as EffectForce's FX tiles switch its looper on: Throw
  the Echo, a Time pattern Time. Reverse Beat, Spin-Back and Rewind play audio Time has already
  recorded, so the recorder ring runs whenever Time is on (Groove Off included), and every factory
  preset with Moves has Time on.

Moves are factory-only in v0.1. A user can pick, re-order and save them in a preset, but not edit
their curves (there is no way to draw them).

## Pages

Five tab groups (all MPC shows without a pager), each page with its own Q-Link set:

| Group | Pages |
|---|---|
| GROOVE | **GROOVE**: the four parts as tiles (lit = on), Order, Time's place, Swing and its unit; the eight knobs that finish most presets (Pump Depth, Shape Depth, Echo Send, Echo Feedback, Echo Tone (an offset to all four lines' cutoffs), Swing, Mix, Output). **PRESETS**: the browser by job |
| PUMP+SHAPE | **PUMP**, **SHAPE** (a card per lane) |
| ECHO | **ECHO** (routing, send, duck, freeze, glide, width, mix, the four lines' times and feedback at a glance), **LINE A**, **B**, **C**, **D** (a line and its modulator) |
| TIME | **TIME** (Groove and its options), **MOVES** (the 16 tiles, mode, quantize, the selected Move, GO) |
| SETUP | Input and Output levels, the Pump's crossover, the transient detectors' sensitivity, the status line |

## Presets

Factory presets are sorted by job, so you pick what you want to happen and the preset sets up the
parts it needs. There are eight categories: **Kick**, **Bass**, **Hats & Perc**, **Stabs & Chords**,
**Drum Bus**, **Master & Perform**, **Echo** (the echo types on their own) and **Utility** (Init:
everything off, unity).

v0.1 has at least three presets per must use case, about 40 in all. Each is level-matched as in
EffectForce (`make preset-levels`), on reference material that matches its category: a kick loop, a
bass line, a hat loop, chord stabs, a drum loop, a full mix. A preset should change the groove more
than the level.

Defaults are safe on any source: Init and every part's defaults change nothing until turned up, and
Time and Moves never act until chosen or triggered. On the master nothing bends by accident.

## Budget

p99 ≤ 15% of the 2902 µs block per instance with **everything on at its heaviest**: four lines with
in-loop filters, drive and modulators in Ring, three Shape lanes, a banded Pump on Input, and Time
playing a Move. That is EffectForce's rule (the worst case a preset can reach). The usual case is
several copies on several tracks, so a typical preset should cost **≤ 4%**.

| Part | Target on the Force |
|---|---|
| Echo, everything on | ≤ 6% |
| Time, a Move playing | ≤ 1.5% |
| Shape, three lanes | ≤ 1% |
| Pump, banded, Input trigger | ≤ 0.8% |
| The plugin's own work | ≤ 0.6% |

Memory per instance: ≤ 16 MB with every part on, nothing for a part never switched on.

Measured as EffectForce does: `make bench-device` on the Force, plus instruction counts under qemu-arm
before the device. A case with four instances at once checks that copies add up as expected.

## Success and failure

It works if:

- every must use case has a factory preset that gets there in one pick, finished with three knobs or
  fewer on the GROOVE page;
- it never clicks: every jump, mode change, routing change, Move start and end and part on/off is
  crossfaded, and checked by tests as EffectForce's are;
- it stays on MPC's grid through loops, locates and tempo changes;
- a typical preset costs ≤ 4% of a block.

It fails, even if it technically works, if:

- it makes mud: echoes or rumble that smear the low end of a kick or bass (the loop's low cut, mono
  rumble presets, Pump on Echo by default in low-end presets);
- anything drifts off the grid;
- a rumble takes setting four lines by hand (that is what presets by job are for);
- presets win by being louder (they're level-matched);
- something bends time on the master without being asked.

## Not in it

| | Why |
|---|---|
| Drawn curves | MPC skins can't draw changing graphics; shapes, Time patterns and Moves are picked by name |
| A sidechain from another track | An insert gets 2 inputs; Pump locks to the grid or follows its own input |
| Look-ahead, latency | MPC doesn't compensate it |
| Gross Beat / Flex Beat pattern import (.fnv) | Later, perhaps: a file format to learn, and no file selector on the device |
| A modulation matrix and macros | v0.2, perhaps. Moves, targets and per-line modulators do the combining in v0.1 |
| Editing Moves and Time patterns on the device | No way to draw them; factory sets in v0.1 |
| Pitch shifting with formants, time-stretching | Not a groove job |

## Plan

1. **Groundwork**, as MasterForce did: EffectForce's build (x86 under ASan/UBSan, ARM under qemu,
   profile-guided), plugin glue, surface machinery, vendored skin generator and installer, test
   harness and CI release; a new plugin ID. Probe the three open device questions (MIDI, press and
   release, automation playback) before the pages depend on them.
2. **Pump, Shape and the grid:** the parts with no delay memory, and the swing grid they share.
   Bass pump and hat accents work at the end of this step.
3. **Echo:** four lines, routings, modulators and the echo presets. Rumble, Rolling bass, Dub chord
   echo and Polyrhythm work at the end of this step.
4. **Time and Moves:** the ring and playhead, Groove (swing), the factory Moves and the MOVES page.
   Hat swing, Throw, Fills and Breakdown moves work at the end of this step.
5. **Presets by job, levels, the device:** the factory set, `make preset-levels`, the bench on the
   Force (one instance and four), playing each must use case on the device, then 0.0.1 and the
   catalog entry.

Where it stands, 2026-10-26: steps 1 and 2 are done (plan 1) and were played on the Force: the Pump
and the Shape work ("it works, but the hat are very very basic", the user, which pointed plan 2 at
the Echo). Plan 2 builds steps 3 and 4 and the factory presets of step 5. Its last task is the one that
needs the user: the bench on the Force, and playing the nine musts on the device.

### Your part

Two decisions shape how GrooveForce feels. The design meant the user to write them when the plan
reached them (as with EffectForce's Delay Glide and MasterForce's glue rule). Both were handed to
Claude instead, and the user judges them by ear on the Force:

- **The pump's curve** (step 2): the function from where in the step we are to the gain, for a
  curve, depth and length. What makes a duck feel like Kickstart. The user, 2026-10-25: "Design the
  pump for me for now, I will later check it on the force plugin" (R11 in plan 1's ledger). The ten
  curves in `dsp/pump_curves.cpp` are Claude's first version.
- **How swing moves audio** (step 4). When a 16th comes late, the playhead jumps back by the swing
  delay, which repeats the last few ms of the 16th before it. Alternatively it can glide back,
  bending the pitch a little like tape. Jump-and-crossfade is clean on drums. A glide is smoother on
  tonal loops but bends their pitch (at 120 BPM, 20 ms over a 16th is about 16% slow). Handed over
  with the rest of plan 2 ("do all at once, implement all that is open", 2026-10-26), and built as
  jump and crossfade (R25).

### Left to the builder

The DSP inside each part within its budget, the exact curves and the shape library's contents, the
routing set refined by ear, parameter names and the page layouts, the reference material for
level-matching, and anything here marked "about".

## Plan 2 rulings (2026-10-26)

The user asked to "do all at once, implement all that is open" (2026-10-26). These are Claude's calls,
made while building the Echo, Time, the Moves and the presets. The numbers are the plan ledger's; plan
1's carry their own ("plan 1's R22"). Each says what the design text above no longer says.

**Decided for the plan**

- **R24, Moves without the probe.** One Shot and Loop. Hold and MIDI-triggered Moves and Pump wait
  for the probe's answers ([Open](#open)). The Move parameter is automatable either way.
- **R25, swing on audio.** Jump and crossfade: the playhead jumps back by the swing delay at the late
  16th's straight line and forward again at the next pair's. Each jump is a 5 ms raised-cosine
  crossfade that *ends on* the grid line, so a hit on the line is never heard early. No pitch bend.
- **R26, the key.** The Pump's Input trigger always listens to the chain's input (after the input gain,
  after Time when Time is Pre, before any part), whatever the Pump's target. Rumble, the Pump on the
  Echo with the Input trigger, hears the dry kick, not the echo it ducks. The Echo's duck and its Env
  and Follow modulators listen to the same key.
- **R27, polyrhythm times.** "3/16" and "3/8" are the dotted 1/8 and 1/4 EffectForce's list already
  has. The Echo's list adds 5/16, 7/16 and 5/8, sorted by length: 19 divisions (the Echo table
  above says 16 plus four).
- **R28, tile order.** The 16 Move tiles hold the 16 factory Moves in a fixed order. The selected
  Move, the mode and the quantize are saved in a preset. Re-ordering the tiles waits.
- **R29, dry level.** The Break Move needs the dry to drop while the echo rings. A `dry` parameter
  (0 to 100%, default 100%, on SETUP) scales the dry bus where it joins the echo; the send, the key and
  Mix are not scaled. Moves can ramp it.

**Moves and Time**

- **R35, the selected Move.** The `Move` parameter holds the selected Move, as the design says: it
  stays on it when the Move ends. A change of it fires the new Move at the quantize, GO fires the
  selected one again, None stops one that plays, and a preset or project load fires nothing.
- **R37, ramps on the 0..1 travel.** A ramp's amount is a change of the parameter's 0..1 value. The
  Throw's "feedback +0.3" meant 0.3 of its -1..1 range, which is 0.15 of travel; a log parameter's
  travel is in octaves of its range (Filter Drop's two octaves).
- **R38, a part a Move switched on stays on until its tail has rung out.** The Echo a Throw opened
  keeps ringing (the chain's tail, at most 12 s) and then goes off as a switch does; a throw that cut
  its own echoes would defeat the throw. Time, Shape and Pump have no tail to keep.
- **R42, Time remembers.** Time's recorder runs whenever it has been allocated, not only while Time is
  on, with Time's output untouched, bit for bit. Reverse Beat, Rewind, Spin-Back and Tape Stop play
  audio from before the Move. The plugin allocates the recorder the first time Time is switched on or a
  Move that needs it is selected or pressed. This supersedes "the recorder ring runs whenever Time is
  on" in [Moves](#moves): on, or ever used. A copy that never used Time holds none.
- **R43, jumps in a crossfade wait.** A jump that arrives while a crossfade runs (a pattern change, a
  locate, a pattern's own jump) is made when the fade is over, at most 5 ms late, never a step.
- **R49, Time warms up.** A groove that asks for a delay (Swing, Drunk, Push) switched on from rest
  passes the input until the recorder holds that delay, a few tens of milliseconds. Without it the
  output would drop out for the length of the delay and step back.
- The Moves' ramps gained a **window** (from and to, as fractions of the Move's length), which Dub
  Freeze and Break use. Time's library holds 15 patterns; the 16 Moves play 11 of them. Stutter 1/32,
  Reverse Stutter, Gate 1/8 and Break Silence are in the library and on no tile.

**The Echo**

- **R36, the loop never gains.** Everything fed back into a line, its own feedback and what a routing
  passes from line to line, is scaled by one over the filter's peak gain at its present setting (the
  peak is Q for a low-pass or high-pass, up to 12 at resonance 100%; 1 for a band-pass and for
  resonance 0). So the loop gain never exceeds the feedback, silence stays silent, feedback 100% holds
  without growing and a Ring can't build up. A resonant filter in the loop keeps its tone in the
  repeats, and what is heard keeps the filter's boost. Costs: passband repeats die faster at high
  resonance.
- **R39, R46, Ring.** Ring is the one routing with a cycle. A line's feedback is its send round the
  ring and it has none of its own, so the loop gain is the product of the running lines' feedbacks (at
  most 1): it never runs away, and the feedback knobs mean how much goes on round. The ring closes over
  the lines that are on (R46); a lone running line feeds itself. This is what the design's "cross
  feedback, dense" became: in Ring the feedback knob changes its meaning.
- **R45, ping-pong in a Ring.** A ping-pong line in a Ring runs as stereo inside and passes its feed on
  with left and right swapped, so the ping-pong happens round the ring (per trip, not per own repeat),
  without loss.
- **R44, Freeze holds in every routing.** Frozen, the feeds between lines stop in Serial, Pairs and
  Spread, as the send does; in a Ring the feedbacks go to 1.
- A line that is off gives a routing nothing: with A off, Serial, Pairs and Spread pass nothing down
  from it, while a Ring closes over the lines that are on and hands the send to the first of them.
- **R40, a routing change** crosses over 45 samples (1 ms), not one 32-sample chunk: the 1 ms rule for
  gain edges wins.
- **R41, one detector.** The Echo runs one onset detector and one level follower on the key for all
  four lines' Env and Follow modulators, since they all hear the same key.
- **R32, bounds.** The Echo's wet is finite and bounded for any input and any parameters; the test
  bound is 8 (four lines, the pan law's root 2, the width), not the plan's 4.

**The chain**

- **R48, All on two buses.** A Pump or Shape aimed at All acts on the dry and the wet identically, as
  if on their sum: two instances with the same parameters, grid and key. When the wet instance starts
  (the Echo coming on, a target moving) it starts as a copy of the dry instance's state (grid phase,
  detector, slews), so the two gains match from the first sample. A Shape filter that starts on a
  wet that is already sounding rings in for a while: that is a limit.
- **R50, the order switch.** Echo First against Echo Last switches the Echo's send source
  (before or after the parts) with a 10 ms crossfade, so no switch steps what the Echo hears. For
  10 ms it hears a blend.
- **Plan 1's R21 is done:** the parts seed their grid from MPC's held position when stopped, and the
  engine gives the parts and the Moves one clock.

**From plan 1, which the design text above still has as first written**

- **R22, the getters.** Lock-free as written was wrong: see the Getters row of
  [What the Force does](#what-the-force-does).
- **R11, the pump curves** were handed to Claude, not written by the user: see
  [Your part](#your-part).
- **R19, R18c, the Input trigger.** The detector listens through a 150 Hz low-pass, so hats on a drum
  bus don't fire it and a kick with no low end won't. It is a level-ratio detector: a hit has to stand
  out about 9 to 12 dB over what is sustained under it, rolls of 1/16 kicks need a keener sensitivity,
  and rumble-like material can fire it now and then. A kick over a bass or a rumble wants the Grid
  trigger. The Pump's Input push is 0 to +30 ms: there is no look-ahead, so the duck can't start
  before the hit.
- **R3b, banded peaks.** The Lows and Highs split is an allpass in the path, so an edgy waveform can
  peak up to about 9 dB higher.

**What the built plugin names differently**

| The design says | Built |
|---|---|
| The Echo's "Mix" | **Echo Level** (the echo added to the dry, 0 to 100%) and **Echo Send** (the input into the lines) |
| About 12 named pump curves | Ten: Smooth, Soft, Punch, Hard, Snap, Long, Late, Linear, S-Curve, Hold |
| About 32 shapes; Pulse's 16 gate patterns | 38 shapes; 14 gate patterns (EffectForce's, without 1/16 and Pump) and four accents |
| A Time pattern is 1/4, 1/2, 1 or 2 bars | A length in beats, half a beat (Spin-Back) to 16 (Roll Build 4 Bars) |
| The Filter lane moves the cutoff by ±4 octaves | The cutoff dips from **Cutoff** by up to four octaves at the shape's bottom; a negative depth rises from it |
| The parts as lit tiles on GROOVE | Off / On segments, with ORDER (Echo First / Echo Last) and TIME PLACE (Post / Pre) beside them |
| SETUP: the transient detectors' sensitivity | Input, Output, Dry Level, the Pump's Sensitivity and Crossover, **Echo Sens** (the Env modulators' detector) |
| Preset categories Hats & Perc, Stabs & Chords, Master & Perform | The folders Hats + Perc, Chords and Perform (shown in capitals) |
| The status line on SETUP | Every page's header shows it: `CPU 3%   PEAK 5%   2 OF 4 ON` |

## Open

| Question | How it's answered | What changes |
|---|---|---|
| Does MPC send MIDI to an insert on the Force (pads, a MIDI track)? | A probe build that says it takes MIDI and logs what arrives | Yes: Pump gets a MIDI trigger and the Move tiles can be played from pads. No: tiles, Q-Links and automation only |
| Does a skin button give press and release, so a Move can play while held? | The same probe | Yes: Hold mode. No: One Shot and Loop only |
| Does MPC play recorded automation back into an insert? (EffectForce's probe still has it pending) | The same probe: record the Move parameter in a sequence, play it back | Yes: Fills are sequenced by automating the Move. No: Moves are played live only, and Fills move to performance |
| Do several instances spread over the Force's cores or add up on one? | The bench with four instances | How strict the typical ≤ 4% has to be |
| Does a Move tile fire one Move per tap, and does a project load fire none? (plan 2) | On the Force: tap each tile, load a project with a Move selected. The tile rule flips what MPC read back, ignores a value the tile already has, and ignores the same tile for a second as MPC's release echo | Yes: the rule stays. No: it changes. It also depends on whether MPC writes the tiles and the Move back at a project load |
| Do the ten pump curves feel right, and does the Input trigger hold up on a real kick and drum bus? (plans 1 and 2) | By ear on the Force | Retune the curves (one function); the detector if it misses or doubles hits |
| Do the Echo's rumble, rolling bass, dub chord and polyrhythm setups, Hat Swing and the Moves sound right? (plan 2) | Plan 2's last task, with the user | Fix what they hear, each fix with a test |
