# User guide

- [Getting started](#getting-started)
- [The screen](#the-screen)
- [The path: how the parts combine](#the-path-how-the-parts-combine)
- [Pump](#pump)
- [Shape](#shape)
- [Echo](#echo)
- [Time](#time)
- [Swing](#swing)
- [Moves](#moves)
- [Presets](#presets)
- [Grooves by job](#grooves-by-job)
- [The limits](#the-limits)
- [How it behaves in MPC](#how-it-behaves-in-mpc)

---

## Getting started

1. Install it (`make plugin-install`, or the `install.sh` in the package `make plugin-package` builds;
   both stop and restart MPC, so save your project first).
2. On a track (a kick, a bass, a hat loop, a drum bus, the master), add **GrooveForce** from the insert
   effects.
3. Open its screen. The **GROOVE** page shows the four parts: **Pump**, **Shape**, **Echo** and
   **Time**. Everything starts off, and a new GrooveForce changes nothing until you turn something up.
4. Pick a factory preset (**PRESETS** page, or the preset stepper at the top of GROOVE), or switch a
   part on and set it up on its own page.

Three quick tries, to hear what each part is for:

- **Pump:** on the GROOVE page switch **PUMP** on and turn **Pump Depth** to 70%. A bass or a pad now
  ducks on every beat and comes back along the curve. No sidechain needed.
- **Echo:** switch **ECHO** on, then open **LINE A** and switch it **ON**. You hear a dotted-eighth
  echo (3/16) in time with the song. Turn **Echo FB** up for more repeats.
- **Time:** open **TIME**, switch it **ON**, set **Time Groove** to **Swing** and **Swing** to 58%.
  Every second 16th of a straight hat loop comes late, as it does when MPC swings notes.

GrooveForce assumes 4/4 at about 120 to 150 BPM: a bar is four beats. Other times work, but the
rates, the defaults and the presets are made for techno.

## The screen

Five tabs; a tab with several pages shows dots under it, and a tap on it again shows the next page.
Every page has its own Q-Link set (MPC shows its name in the tab strip): the Force's 8 knobs reach the
page's first bank, the next bank the rest. A choice with a few options is a row or a column of
segments; a long list (a curve, a shape, a division) is a popup: tap it and pick a row. A Q-Link
detent on a choice moves it by one option.

Most controls' names say what they belong to ("Pump Depth", "A Feedback"), because MPC's Q-Link
overlay shows the name without the page. A few are short: **Cutoff** and **Resonance** are the Shape's
filter lane, **Sensitivity** and **Crossover** the Pump's; **Input**, **Output**, **Mix**, **Swing**
and **Move** belong to the whole plugin.

| Tab | Pages |
|---|---|
| GROOVE | **GROOVE**: the eight knobs that finish most grooves, the four parts' switches, the Echo's order and Time's place, the preset stepper. **PRESETS**: the browser |
| PUMP+SHAPE | **PUMP**, **SHAPE** |
| ECHO | **ECHO** (the Echo as a whole, and the four lines at a glance), **LINE A**, **LINE B**, **LINE C**, **LINE D** |
| TIME | **TIME** (its switch, place, groove, amount and the swing), **MOVES** (the 16 Move tiles) |
| SETUP | **SETUP**: Input, Output, Dry Level, and the detectors and the crossover |

| Page | What's on it | Q-Links |
|---|---|---|
| GROOVE | **KNOBS** card: Pump Depth, Shape Depth, Echo Send, Echo FB, Echo Tone, Swing (with its unit, 1/16 or 1/8, under it), Mix, Output. **PARTS** card: PUMP, SHAPE, ECHO and TIME switches, ORDER (Echo First or Echo Last) and TIME PLACE (Post or Pre) | the eight knobs |
| PRESETS | CATEGORIES and PRESETS tiles, PREV / NEXT, the loaded preset, the Favorite toggle, RND, SAVE, INIT | the preset stepper, Input, Output, Mix |
| PUMP | **PUMP** card: Depth, Length, Curve, Rate, Push, Band, Crossover, Pump Mix. **TRIGGER AND TARGET** card: Trigger, Sensitivity, Target, ON | the PUMP card, then the second card |
| SHAPE | a card each for **VOLUME**, **PAN** and **FILTER** (ON, shape, rate, depth, phase; the filter adds type, Cutoff and Resonance) and a **SHAPE** card (ON, Target, Shape Depth, Shape Mix) | across the lanes, not card by card: Volume Depth, Volume Shape, Volume Rate, Filter Depth, Filter Shape, Cutoff, Pan Depth, Pan Shape; then Volume Phase, Filter Rate, Resonance, Filter Type, Pan Rate, Pan Phase, Shape Mix, Shape Target |
| ECHO | **ECHO** card: Echo Send, Echo FB, Echo Tone, Echo Duck, Echo Width, Echo Level, ROUTING, FREEZE. **LINES** card: each line's switch, division and feedback. **PART** card: ON, ORDER, GLIDE | the ECHO card |
| LINE A to D | **LINE** card: ON, TIME (Sync or Free), division, time in ms, feedback, level, pan, mode, wow. **FILTER** card: Cutoff, Resonance, Drive, type, place. **MODULATOR** card: source, shape, rate, and three amounts | Division, Feedback, Level, Pan, Cutoff, Resonance, Drive, Mod Cutoff; then Mod Time, Mod Level, Wow, Modulator, Filter Type, Mode, Time, On |
| TIME | **TIME** card: ON, PLACE, GROOVE. **AMOUNT AND SWING** card: Time Amount, Swing and its unit | Time, Time Place, Time Groove, Time Amount, Swing, Swing Unit |
| MOVES | the 16 tiles; **FIRING** card: MODE (One Shot or Loop), QUANTIZE (1/16, Beat or Bar), the selected Move, GO | Move, Move Mode, Move Quantize |
| SETUP | **LEVELS** card: Input, Output, Dry Level. **PUMP AND ECHO** card: Sensitivity (the Pump's), Crossover, Echo Sens | Input, Output, Dry Level, Sensitivity, Crossover, Echo Sens |

The status line at the top of every page shows what GrooveForce costs and how many parts run, for
example `CPU 3%   PEAK 5%   2 OF 4 ON`: the average and the worst block of the last half second, in
percent of MPC's audio block, and how many of the four parts were running.

Every knob and every choice can be automated. The tiles, GO, SAVE and the other browser buttons
can't.

## The path: how the parts combine

```
input gain -> [Time, Pre] -+-> dry --------- Shape -> Pump -- Dry Level --+
                           '-> send -> Echo -> Shape -> Pump --------------+-> [Time, Post] -> Output -> Mix
```

Four things decide how the parts work together.

**Targets.** Shape and Pump each act on **All**, **Dry** or **Echo** (the echo only). Pump on **Echo**
ducks the echo and leaves the dry alone: that is the kick rumble. Pump on **Dry** ducks the bass and
leaves its echoes ringing. On **All** the part acts on the dry and on the echo alike, as if it were
on their sum. A part aimed at the echo has nothing to work on while the Echo is off, and rests.

**Order.** **Echo First** (the default): the Echo hears the input, and Shape and Pump move what comes
out of it. **Echo Last**: Shape and Pump come first and the Echo hears what they made, so a gated
chord echoes as a rhythm. With Echo Last the targets don't apply: Shape and Pump act on the dry. A
change of the order crossfades over 10 ms.

**Time's place.** **Post** (the default): Time bends the sum of the dry and the echo, so a tape stop
stops the echoes too. **Pre**: Time bends the input only, and the echo rings on through a stop.
Changing the place starts Time's memory again.

**One grid.** Everything locks to MPC's song position, through loops, locates and tempo changes.
Stopped, the grid runs on from where MPC stopped, at the tempo, so you can hear a groove while you
set it up. Copies on other tracks lock to the same song position, so a pump on the bass and a shape
on the pads land on the same beats. **Swing** is one setting for all of it: [see below](#swing).

Every part has a switch, and switching fades over 10 ms. A part that is off costs no CPU, with one
exception: once Time has been used, its memory goes on recording while Time is off (a write per
sample), so the Moves that play the past always have it. The Pump, the Shape and the Echo come on
from cleared state, with no echo of what played minutes ago; Time comes on with the memory it has.
The Echo's lines and Time's memory (about 4 MB each) are made the first time they are needed, so a
copy that only pumps holds none.

The levels: **Input** (-24 to +24 dB) sets the level into everything. **Output** (-24 to +24 dB)
sets the level out. **Mix** blends the result with the input as it came in (100% for an insert;
lower to use the whole chain in parallel). **Dry Level** (on SETUP) scales the dry side where it joins
the echo: at 0% the dry is gone and the echo, which is still fed, rings on. That is how the **Break**
Move works.

## Pump

A Kickstart-style duck on MPC's grid: the signal is ducked at the start of every step and comes back
along a named curve. No sidechain, no look-ahead: the grid is known from the song position, so the
duck can start before the beat.

| Control | Range | Default | |
|---|---|---|---|
| **Pump** | Off, On | Off | The switch |
| **Pump Depth** | 0 to 100% | 0% | How deep the duck goes; 100% is silence at the bottom |
| **Pump Length** | 5 to 100% of a step | 60% | How long the recovery takes |
| **Pump Curve** | Smooth, Soft, Punch, Hard, Snap, Long, Late, Linear, S-Curve, Hold | Smooth | How the level comes back |
| **Pump Rate** | 1/16, 1/8, 1/4, 1/2, 1 bar | 1/4 | The step: a duck on every one |
| **Pump Push** | -10 to +30 ms | 0 | Moves the bottom of the duck early (negative) or late |
| **Pump Trigger** | Grid, Input | Grid | What fires a duck |
| **Sensitivity** | 0 to 100% | 50% | Input's detector: at 0% only hits that stand out count, at 100% quiet ones too |
| **Pump Band** | Full, Lows, Highs | Full | What is ducked |
| **Crossover** | 60 to 300 Hz | 120 Hz | The split for Lows and Highs |
| **Pump Target** | All, Dry, Echo | All | What it acts on ([above](#the-path-how-the-parts-combine)) |
| **Pump Mix** | 0 to 100% | 100% | How much of the duck is applied |

The recovery lasts Length times a step: Rate 1/4 and Length 60% is 0.6 of a beat. Depth 50% ducks
half way and recovers along the same curve.

**The curves.** Smooth, the default, eases back into the input without a snap. Soft is gentler and
lower early on, the breath of a compressor on a pad. Hard has a definite edge, Punch pushes through
faster, and Snap is nearly all the way back in the first quarter. Linear is a straight line up and
S-Curve is slow, quick, slow. Long stays down across most of the recovery and lets go at the end.
Late stays at the bottom for the first 30% of the recovery and Hold for the first 60%, and then both
rise. The ten are a first set, to be judged by ear on the Force and retuned.

**Trigger.** **Grid** ducks on every step of the grid. **Input** ducks on every hit it finds in the
input instead: for a drum bus, or for Rumble on a kick track where the kicks aren't on every beat.
The detector listens through a 150 Hz low-pass, so hats don't fire it. It has no look-ahead: the
duck starts when the hit is heard, 1.5 to 5 ms after a kick starts, and Pump Push can only be
late then (0 to +30 ms). Pump Rate still sets how long the recovery lasts (Rate times Length). It
has [limits](#the-limits): where a kick sits over a bass or a rumble, use Grid.

**Band.** **Lows** ducks the part below the Crossover and lets the rest through; **Highs** the
reverse. The two sides sum flat. A low pump (bass breathes with the kick, hats don't pump) is Band
Lows at 120 Hz.

Every duck is click-free: its fastest edge takes at least 1 ms, whatever the curve and the length.

## Shape

ShaperBox without the drawing. Three lanes move the sound along a named shape, locked to the grid:
**Volume**, **Filter** and **Pan**. Each lane has its own switch, so use one, two or all three.

| Control | Range | Default | |
|---|---|---|---|
| **Shape** | Off, On | Off | The switch for the whole part |
| **Shape Depth** | 0 to 100% | 100% | Scales every lane's depth: the one knob to finish with |
| **Shape Target** | All, Dry, Echo | All | What the part acts on |
| **Shape Mix** | 0 to 100% | 100% | Wet against dry for the whole part |
| **Volume**, **Filter**, **Pan** | Off, On | Off | A lane's switch |
| **Volume Shape**, **Filter Shape**, **Pan Shape** | 38 shapes | Sine | What the lane plays |
| **Volume Rate**, **Filter Rate**, **Pan Rate** | 1/16, 1/8T, 1/8, 1/4T, 1/8., 1/4, 1/4., 1/2, 1/2., 1 bar, 2 bars, 4 bars | 1/4 | The length of one cycle of the shape |
| **Volume Depth** | 0 to 100% | 0% | How far the lane moves the sound |
| **Filter Depth**, **Pan Depth** | -100 to +100% | 0% | The same; negative runs the other way |
| **Volume Phase**, **Filter Phase**, **Pan Phase** | 0 to 100% of a cycle | 0% | Slides the shape against the bar |
| **Filter Type** | LP, BP, HP | LP | The filter lane's filter |
| **Cutoff** | 20 Hz to 20 kHz | 20 kHz | Where the filter sits when the shape is at its top |
| **Resonance** | 0 to 100% | 0% | The filter's peak |

**Volume** never boosts: the shape at its top lets everything through, at its bottom Volume Depth
takes the level down (100% is silence). **Filter** moves the cutoff down from Cutoff by up to four
octaves at the shape's bottom (a negative depth moves it up from Cutoff instead: set Cutoff low
then). **Pan** swings the balance between left and right at constant power; a negative depth swings
it the other way.

**The shapes**, in the popup's order:

| Family | Shapes |
|---|---|
| Waves (8) | Sine, Triangle, Saw Up, Saw Down, Ramp Up, Ramp Down, Square, Pulse 25 |
| Pumps (10) | Pump Smooth, Pump Soft, Pump Punch, Pump Hard, Pump Snap, Pump Long, Pump Late, Pump Linear, Pump S-Curve, Pump Hold: the Pump's curves as a shape, down at the start of the cycle and recovering across it. A filter that ducks with the volume |
| Gates (14) | 1/8, 1/4, Offbeat, Off 16ths, Dotted, Tresillo, Gallop, Rev Gallop, Trance 1, Trance 2, Trance 3, Stutter, Half Bar, Build: 16-step on and off patterns |
| Accents (4) | Offbeat Accent, Rolling 16ths, Gallop Accent, Shuffle: 16 steps at levels instead of on and off |
| Random (2) | Random Steps, Random Smooth: drawn afresh each cycle, but the same every time round a loop |

A gate shows as 16 steps, `x` open and `.` closed:

| Gate | Steps | Gate | Steps |
|---|---|---|---|
| 1/8 | `x.x.x.x.x.x.x.x.` | Gallop | `x.xxx.xxx.xxx.xx` |
| 1/4 | `x...x...x...x...` | Rev Gallop | `xxx.xxx.xxx.xxx.` |
| Offbeat | `..x...x...x...x.` | Trance 1 | `x.xx.xx.x.xx.xx.` |
| Off 16ths | `.x.x.x.x.x.x.x.x` | Trance 2 | `xx.xx.x.xx.xx.x.` |
| Dotted | `x..x..x..x..x..x` | Trance 3 | `x.xxx.x.x.xxx.x.` |
| Tresillo | `x..x..x.x..x..x.` | Stutter | `x.x.x.x.xxxxxxxx` |
| Half Bar | `xxxxxxxx........` | Build | `x...x...x.x.xxxx` |

**A 16-step shape is 16 steps of one cycle.** Set the lane's Rate to **1 bar** and the steps are 16ths.
At 2 bars they are 8ths, and at the default 1/4 they are 64ths. The accents: Offbeat Accent has the
third 16th of every beat (the offbeat eighth) at full level and the rest at 45%. Rolling 16ths runs 100, 55, 75, 55%
through each beat. Shuffle runs 100, 30, 70, 50%. Gallop Accent opens the Gallop's steps, the head of
each group of four at 100% and the others at 60%.

The steps follow the global [Swing](#swing).

## Echo

Four delay lines, **A** to **D**, a routing between them, and a filter, a drive and a modulator on
each. It is deep on purpose: one setup can be a kick rumble, a dub chord wash or a polyrhythm. The
presets set up the common ones.

**If you hear nothing:** the Echo needs the **ECHO** part on *and* at least one line **ON**. A line
set to HP or BP starts at a Cutoff of 20 kHz, which lets almost nothing through: bring Cutoff down
first. Echo Send at 0% closes the input (right for a Throw Move, wrong for a normal echo).

### The lines

Each of the four lines has the same controls; their names start with the line's letter ("A
Division", "B Feedback").

| Control | Range | Default | |
|---|---|---|---|
| **A On** | Off, On | Off | The line's switch; a line that is off costs nothing |
| **A Sync** | Sync, Free | Sync | The time comes from a division, or from A Time in ms |
| **A Division** | 1/64, 1/32T, 1/32, 1/16T, 1/16, 1/8T, 1/16., 1/8, 1/4T, 1/8., 1/4, 5/16, 1/2T, 1/4., 7/16, 1/2, 5/8, 1/2., 1 bar | A: 1/8., B: 1/4, C: 5/16, D: 1/2 | The synced time, in order of length so a Q-Link walks them. `T` is a triplet, `.` a dotted value |
| **A Time** | 1 to 2000 ms | 375 ms | The free time |
| **A Feedback** | 0 to 100% | 40% | How much of the repeats goes round again; 100% holds them |
| **A Level** | 0 to 100% | 80% | How loud the line is heard |
| **A Pan** | -100 to +100% | 0% | Where it is heard, at constant power |
| **A Mode** | Stereo, Ping-Pong, Mono | Stereo | Ping-Pong bounces the repeats left, right, left; Mono gives both sides the same |
| **A Filter Type** | LP, BP, HP | LP | The line's filter |
| **A Cutoff** | 20 Hz to 20 kHz | 20 kHz | An LP at 20 kHz is no filter at all |
| **A Resonance** | 0 to 100% | 0% | The filter's peak |
| **A Flt Place** | In Loop, On Output | In Loop | See below |
| **A Drive** | 0 to 100% | 0% | A soft clip in the loop: each repeat a little dirtier |
| **A Wow** | 0 to 100% | 0% | Tape speed wobble: a slow wow and a fast flutter |
| **A Modulator** | Off, LFO, Env, Follow | Off | What moves the line (below) |
| **A Mod Shape** | the 38 shapes | Sine | The LFO's shape |
| **A Mod Rate** | 1/16 to 4 bars (as Shape's rates) | 1/4 | The LFO's cycle, or the Env's fall |
| **A Mod Cutoff** | -100 to +100% | 0% | How far the modulator moves the cutoff, up to four octaves |
| **A Mod Time** | -100 to +100% | 0% | How far it moves the time: a small pitch wobble, up to 2% |
| **A Mod Level** | 0 to 100% | 0% | How far it dips the level |

The list has no separate 3/16 or 3/8: 3/16 is the dotted eighth (**1/8.**) and 3/8 the dotted quarter
(**1/4.**). **5/16**, **7/16** and **5/8** are there for polyrhythms against the 4/4. At 120 BPM a
beat is 500 ms, so 1/8. is 375 ms. A line holds 3 seconds (one bar at 80 BPM): a longer time is
cut to 3 s.

**Filter place.** A filter **In Loop** is part of the loop: each repeat is filtered again, so the
echoes get darker, thinner or narrower with every pass (rumble, dub). **On Output** shapes only what
you hear: every repeat comes out through the same filter once, and the loop carries the unfiltered
repeat round again. A resonant filter In loop keeps its tone in the repeats, but the loop never gains: the repeats still die
away, and feedback 100% holds them without growing. What you hear can be louder than what went
in (up to 12 times at Resonance 100%), as with any resonant filter.

**Modulators.** One value from 0 to 1 per line moves the cutoff (Mod Cutoff), the time (Mod Time)
and the level (Mod Level), each by its own amount:

- **LFO:** a shape from the Shape library at A Mod Rate, on MPC's grid.
- **Env:** each hit in the input sets it to 1 and it falls away over A Mod Rate (one Mod Rate to
  -60 dB). It is 0 until the first hit. Mod Level at 100% makes a line that only speaks when the input
  hits. The detector is the Echo's, set by **Echo Sens** on SETUP.
- **Follow:** the input's level.

The modulators hear the input, never the echoes themselves.

**Time changes.** A new time (a tempo change, another division, a turn of A Time, a modulator) is
reached by **Echo Glide**: **Tape** glides, so the repeats in flight bend in pitch; **Fade** crossfades
over about 50 ms, no pitch bend.

### The whole Echo

| Control | Range | Default | |
|---|---|---|---|
| **Echo** | Off, On | Off | The part's switch |
| **Echo Order** | Echo First, Echo Last | Echo First | [See above](#the-path-how-the-parts-combine) |
| **Echo Routing** | Parallel, Serial, Pairs, Ring, Spread | Parallel | How the lines feed each other |
| **Echo Send** | 0 to 100% | 100% | How much of the input reaches the lines |
| **Echo FB** | -100 to +100% | 0% | Added to every line's feedback (the result held to 0 to 100%); in a Ring, to how much goes round |
| **Echo Tone** | -100 to +100% | 0% | Added to every line's cutoff: -50% is two octaves down, so a 20 kHz low-pass becomes 5 kHz |
| **Echo Duck** | 0 to 100% | 0% | The echo ducks under the input and blooms in the gaps |
| **Echo Freeze** | Off, On | Off | Holds what is in the lines (below) |
| **Echo Glide** | Tape, Fade | Tape | See above |
| **Echo Width** | 0 to 150% | 100% | The echo's stereo width; 0% is mono, 100% is untouched |
| **Echo Level** | 0 to 100% | 100% | How much echo is added to the dry |
| **Echo Sens** | 0 to 100% | 50% | The Env modulators' detector (SETUP) |

**Duck.** At 100% the echo is 6 dB down with the input at -24 dBFS and 19 dB down at -6 dBFS. Only
what you hear ducks, not the lines, so the repeats keep going underneath.

**Freeze.** The input closes and the lines hold what they have, in every routing. With a filter or
drive in the loop the repeats wear down slowly.

**Routing.** What a line hears:

| Routing | A | B | C | D |
|---|---|---|---|---|
| **Parallel** | the send | the send | the send | the send |
| **Serial** | the send | A's output | B's output | C's output |
| **Pairs** | the send | A's output | the send | C's output |
| **Ring** | the send and D's output (see below) | A's output | B's output | C's output |
| **Spread** | the send | A's output | A's output | A's output |

Every line that is on is heard at its Level and Pan, whatever the routing. A line that is off gives
the routing nothing. In Serial, Pairs and Spread the line it feeds hears nothing: every line after it
in Serial, B if A is off and D if C is off in Pairs, and B, C and D if A is off in Spread. So start a
chain with a line that is on. A **Ring** goes round the lines that are on instead: the send goes to
the first line that is on (B, if A is off), and each line hears the line that is on before it. A
routing change crossfades in about 1 ms.

In **Parallel**, **Serial**, **Pairs** and **Spread** every line keeps its own feedback: a line of
repeats. **Ring** is the one routing that goes round, and there the feedback means something else: a
line's Feedback is **how much goes on round the ring**, and a line has no repeats of its own. The ring
closes over the lines that are on (switch D off and it is A, B, C); a lone line feeds itself. The loop
gain is the product of the running lines' feedbacks, so it never runs away. Feedback 100% on every
line holds the ring, and a 0% anywhere opens it. A **Ping-Pong** line in a Ring alternates its side
on every trip round the ring.

## Time

Flex Beat's and Gross Beat's time tricks, with zero latency. Time keeps the last seconds of the sound
in a memory and plays from a read point that is *now* or behind it, never ahead. A trick that seems to
need the future, like double speed, plays something already recorded twice.

| Control | Range | Default | |
|---|---|---|---|
| **Time** | Off, On | Off | The part's switch |
| **Time Place** | Post, Pre | Post | [See above](#the-path-how-the-parts-combine) |
| **Time Groove** | Off, Swing, Drunk, Push | Off | A groove that runs all the time |
| **Time Amount** | 0 to 100% | 50% | How much, for Drunk and Push |

**Groove.** The groove delays hits so a straight loop grooves:

- **Swing:** every second 16th (or 8th) comes late by the [Swing](#swing) amount. The Swing knob is
  the amount; Time Amount is not used.
- **Drunk:** each 16th of the bar comes late by its own amount, up to Time Amount times 15 ms. The
  amounts are fixed, so they are the same on every bar and every loop: loose, but not random.
- **Push:** every hit comes late by Time Amount times 20 ms, for laid-back hats.
- **Off:** no groove. Time on with Groove Off and no Move passes the input bit for bit; it only records.

Every jump is a 5 ms crossfade that ends on the grid line, so a hit on the line is never heard early
and no pitch bends. A jump that arrives while a crossfade is running waits for it, so it can land up
to 5 ms late, and never clicks.

**Memory.** Once Time has been used, it keeps the last 11 seconds or so of its place, even while it
is switched off. So **Reverse Beat**, **Rewind**, **Spin-Back** and **Tape Stop** always
have something to play. A copy that has never used Time holds no memory: the first time you switch
Time on or pick a Move that needs it, it starts recording from then, so pick a Time Move a bar or
two before the first time you play it. A Move reaches back at most 8 beats. A groove switched on
right after the memory was made or cleared passes the input until the memory holds the groove's
longest delay plus a crossfade: about 26 ms for Swing 58% on 1/16 at 120 BPM, about 130 ms for Swing
75% on 1/8 (longer at slower tempos), up to about 26 ms for Drunk and Push.

**The patterns.** A pattern says where Time reads (now, or behind it) and how loud, over a length on
the grid. The Moves play them; the library holds 15:

| Pattern | Length | What the playhead does | Played by |
|---|---|---|---|
| Stutter 1/8 | 1 bar | the first eighth after the start, repeated | Stutter 1/8 |
| Stutter 1/16 | 1 bar | the first sixteenth, repeated | Stutter 1/16 |
| Stutter 1/32 | 1 bar | the first thirty-second, repeated | no tile yet |
| Reverse Stutter | 1 bar | the first eighth live, then played backwards over and over | no tile yet |
| Roll Build 1 Bar | 1 bar | slices of a quarter, an eighth, a sixteenth and a thirty-second, each repeated | Roll Build 1 Bar |
| Roll Build 4 Bars | 4 bars | the same, a bar for each size | Roll Build 4 Bars |
| Half-Time Bar | 1 bar | half speed | Half-Time Bar |
| Tape Stop | 1 bar | speed falls to 0, volume fades in the last half beat | Tape Stop |
| Slow Down | 1 bar | speed falls to half | Slow Down |
| Reverse Beat | 1 beat | backwards, from the start | Reverse Beat |
| Spin-Back | half a beat | backwards, speeding up from 1x to 3x, fading out | Spin-Back |
| Rewind | 1 beat | backwards at double speed | Rewind |
| Gate 1/16 | 1 bar | live; the volume opens for 60% of each sixteenth | Gate 1/16 |
| Gate 1/8 | 1 bar | the same on eighths | no tile yet |
| Break Silence | 1 bar | live, with the volume at 0 | no tile yet |

The stutters and the gates go round by themselves in Loop mode; the others start again lap by lap.
Every pattern ends back on the live sound.

## Swing

**Swing** (50 to 75%, **Swing Unit** 1/16 or 1/8) makes every second unit late: 50% is straight,
about 58% is a light shuffle, 66% is a triplet feel. At 120 BPM on 1/16, 58% is 20 ms late and 66%
is 40 ms. One setting runs everything, and it is applied **once** along the path:

- **Time Groove = Swing, Time Place = Post:** Time swings the audio itself. A straight hat loop gets
  a groove, which MPC's own swing (on notes) can't give a loop or a resampled groove. Pump and Shape
  stay straight, because Time moves their ducks and accents along with the hits.
- **Time Groove = Swing, Time Place = Pre:** Time swings the input first. Pump and Shape then swing
  their steps to match.
- **Anything else** (Time off, or Drunk, Push, Off): Pump, Shape and the Echo's LFOs swing their
  *steps* and the audio is not moved. That is for audio that MPC's own note swing has already
  swung: set Swing to the amount MPC uses, and a swung pump and a swung accent land on the same late
  16ths as the notes. On a straight loop this puts accents off the hits.

Swing moves every second unit, so whatever sits on the beat never moves: a Pump on 1/4 or longer
stays put. A Pump on 1/16 follows the swing (a Pump on 1/8 too when the unit is 1/8), and so do the
Shape lanes.

## Moves

A Move is a one-tap action on the grid that drives several parts at once over its length, then
returns. It is Flex Beat's pads on the Force: 16 tiles on the **MOVES** page, each a fill, a stop, a
throw or a break.

### The 16 tiles

In order, row by row:

| | Move | Length | What it does |
|---|---|---|---|
| 1 | **Throw Beat** | 1 beat | Opens the Echo's send fully and raises the feedback (+30 points) for the Move; the send then goes back to the Send knob's own value. The echo rings on after |
| 2 | **Throw Bar** | 1 bar | The same for a bar |
| 3 | **Dub Freeze** | 5 beats | A beat of send and feedback up, then the Echo is frozen for a bar |
| 4 | **Half-Time Bar** | 1 bar | The bar at half speed, an octave down, then back to live |
| 5 | **Tape Stop** | 1 bar | Winds down to a stop over the bar, fading out at the end, then back to live |
| 6 | **Slow Down** | 1 bar | Slows to half speed over the bar, then back to live |
| 7 | **Reverse Beat** | 1 beat | The beat that just played, backwards |
| 8 | **Spin-Back** | half a beat | A spin-back: the audio before the Move runs backwards, speeding up, and fades out |
| 9 | **Stutter 1/8** | 1 bar | Takes the first eighth after the launch and repeats it for the bar |
| 10 | **Stutter 1/16** | 1 bar | The same with a sixteenth |
| 11 | **Roll Build 1 Bar** | 1 bar | A roll that speeds up: repeats of a quarter for two beats, an eighth for a beat, a sixteenth for half a beat, a thirty-second for the last half beat |
| 12 | **Roll Build 4 Bars** | 4 bars | The same over four bars: a bar of quarters, a bar of eighths, a bar of sixteenths, a bar of thirty-seconds |
| 13 | **Filter Drop** | 1 bar | Switches the Shape's filter lane on and closes its Cutoff two octaves over the bar |
| 14 | **Gate 1/16** | 1 bar | Chops the bar into 16ths, each open for 60% of its step |
| 15 | **Break** | 1 bar | Turns the dry off for the bar. The first beat is sent into the Echo, which rings on |
| 16 | **Rewind** | 1 beat | The last two beats run backwards at double speed in a beat (an octave up) |

The tiles that play from Time (4 to 12, 14 and 16) switch Time on for the Move. **Throw Beat**,
**Throw Bar**, **Dub Freeze** and **Break** work the Echo: they switch the Echo part on, but the
lines are yours, so at least one line must be on (and, for a throw, Echo Send is best low before it,
since the Move opens it). **Filter Drop** moves the Shape's filter lane: the lane's shape, depth,
type and resonance are yours, and Cutoff is where the drop starts (at its default of 20 kHz the drop
runs from 20 kHz to 5 kHz; set Cutoff lower to hear more of it). [Time
Place](#the-path-how-the-parts-combine) decides what a Time Move bends: with **Post** the echoes too,
with **Pre** only the input. Reverse Beat, Rewind and Spin-Back play audio from before their line
(see [Time's memory](#time)).

A Move changes the knobs by an amount **relative to where they are**: send "to full" means "up by
100%, held at 100%". It glides in over 1 ms and back to the knob's own value over 10 ms when it ends.

### Firing

- **A tap on a tile** selects its Move and fires it. The tile lights while its Move plays or waits
  for its line.
- **Move** (a choice: None and the 16 names) holds the **selected Move**. It stays where it is
  when the Move ends. A change of it fires the new Move at the quantize, **None** stops one that
  plays (or cancels one that waits), and it can be automated: write the Move parameter into a sequence
  and the breaks play per bar. Whether MPC plays recorded automation into an insert is still to be
  tried on the Force.
- **GO** fires the selected Move again.
- **Move Quantize** is where the Move starts: the next **1/16**, **Beat** (the default) or **Bar** line
  of the grid. A tap up to a hair after a line counts as on it. Set it to **Bar** to land a fill on
  the downbeat.
- **Move Mode** is **One Shot** (plays its length once, the default) or **Loop** (goes round until
  you pick another Move, pick None, or load a preset). Dub Freeze is five beats long, so in Loop it
  walks off the bar.
- Mode and Quantize apply to every Move. Another Move fired while one plays waits for its line and
  replaces it there.

Loading a preset or a project ends a Move that plays and fires nothing, whatever Move it selects.
The selected Move, the mode and the quantize are saved in presets and projects. Turning a Q-Link
through the Move list fires every Move it passes; the last one before the line is the one that plays.

### What a Move leaves on

A Move switches on the parts it needs from a moment before its line to its end, so a Time Move works
on a copy where everything is off. A part the knobs already have on stays on. The Echo a Move
switched on stays on after the Move until its repeats have rung out (up to 12 seconds), so a throw
never cuts its own echoes. Time, Shape and Pump switch off when the Move ends.

## Presets

A factory preset is sorted by job, so you pick what you want to happen and the preset sets up the
parts it needs. A preset sets only what it changes; everything it doesn't name goes to its default
(Input, Output and Mix too). Each is level-matched on reference material for its category (the kick
for Kick, the bass line alone for Bass, a hat loop, chords, a drum loop, a full mix), so it comes out
about as loud as it went in, and you compare sounds, not levels. **Init** is everything off.

| Category | The job | Presets (the factory set's names) |
|---|---|---|
| **Kick** | The kick's tail becomes a dark rolling bed between kicks, the hypnotic-techno rumble: mono, a low-pass in the loop, the Pump ducking only the echo | Rumble, Rumble Deep, Rumble Short |
| **Bass** | A pump on the bass; or one offbeat hit turned into a 16th roll | Bass Pump, Hard Pump, Double Pump, Offbeat Gate, Rolling Bass, Rolling Dark, Rolling 3/16 |
| **Hats + Perc** | Hats that groove: accents and shuffle with an echo layer, polyrhythms on 3/16 and 5/16, ping-pong, swing and drunk timing | Hat Accents, Hat Shuffle, Polyrhythm, Poly 5/16, Ping-Pong Hats, Hat Swing 58, Hat Swing 66, Drunk Hats, Rim Echo |
| **Chords** | Gates and pumps for chords, dub echoes, a throw for stabs | Chord Gate Pump, Filter Pump, Dub Chord, Dub Chord Wide, Dub Chord Drive, Stab Throw |
| **Drum Bus** | A low pump, swing and accents on a loop, a roll for fills | Low Pump, Loop Groove, Bus Swing, Fill Roller |
| **Perform** | Moves for the master: breakdowns, builds, throws, glitch | Breakdown, Build, Throws, Glitch |
| **Echo** | The echo types on their own | Rumble Echo, Dub, Roll, Polyrhythm Echo, Ping-Pong, Tape, Slap, Spread Cloud, Freeze |
| **Utility** | | Init |

(The PRESETS page lists the set that is installed; the names above are the factory set's.)

- **PRESETS page:** categories on the left (**FAVORITES** and **RECENT** first), presets on the right;
  tap to load. The **Favorite** toggle marks the loaded preset, **RND** loads a random one of the category, **SAVE**
  writes `User NNN.gfp` (there is no text entry on the device), **INIT** loads Init.
- **The preset stepper** (GROOVE page, top right) walks all presets, one per Q-Link detent.
- Preset files live in the plugin's folder under `Presets/` and on the SSD under
  `GrooveForce Presets/`; folders become categories. Your saved presets go to `Presets/User/`.

## Grooves by job

Starting points, not rules. The presets above set up most of them; these say what they are made of,
so you can change them. The steps follow the screens, not a rule: switch a part on first and hear it
as you set it, or set it up with the part off and switch it on last to hear the finished setting come in.

### Kick rumble

On a kick track: the kick's tail becomes a dark bed that swells between kicks and never covers the
next one.

1. **Echo** on. **A On** and **B On**.
2. **A Division** 1/16, **B Division** 1/8.. **A Feedback** and **B Feedback** around 70%.
3. **A Filter Type** and **B Filter Type** LP, **A Cutoff** and **B Cutoff** around 200 Hz, **A Drive**
   and **B Drive** around 30%. **A Mode** and **B Mode** Mono.
4. **Pump** on: **Pump Target** Echo, **Pump Trigger** Input, **Pump Depth** 100%, **Pump Length** to
   taste.

The dry kick is untouched; the Pump ducks only the echo on every kick it hears. If the kick shares the
track with a bass, the Input trigger may miss kicks: use **Grid** (the kicks on every beat).

### Rolling bass

One offbeat bass hit becomes a 16th roll, each repeat a little darker, with the beat left free for
the kick.

1. **Echo** on, **A On**, **A Division** 1/16, **A Feedback** around 50%.
2. **A Filter Type** LP, **A Cutoff** around 1.5 kHz, so each repeat is darker.
3. **Pump** on at 1/4 (the default), Target **All**, Depth around 80%. The pump ducks the bass and the
   roll together on every beat.

### Bass pump

**Pump** on, **Pump Depth** 70%, **Pump Curve** Smooth for a long breath, Punch or Hard for a tight one.
Pump Rate 1/8 for a double pump. Pump Band **Lows** with the Crossover at 120 Hz if there is more in the
track than the bass.

### Hats

Hats that groove, not just accents. Build them in layers.

1. **Swing.** **Time** on, **Time Groove** Swing, **Swing** 58% for a light shuffle or 66% for a
   triplet feel. For a looser hand use **Drunk** with **Time Amount** around 30%.
2. **Accents.** **Shape** on, **Volume** on, **Volume Shape** Offbeat Accent, **Volume Rate** 1 bar,
   **Volume Depth** around 60%. (With Time swinging, Shape stays straight and Time moves the
   accents along with the hits.)
3. **An echo layer on 3/16 and 5/16.** **Echo** on. **A On**, **A Division** 1/8., **A Pan** -50%.
   **B On**, **B Division** 5/16, **B Pan** +50%. **A Filter Type** and **B Filter Type** HP with
   **A Cutoff** and **B Cutoff** around 500 Hz (a high-pass starts at 20 kHz, which is almost
   silent), and
   **A Feedback** and **B Feedback** around 45%. The 3/16 and 5/16 hits run against the 4/4, so a
   plain loop gains a pattern that doesn't repeat every beat.
4. **Move it.** Set **A Modulator** to LFO with **A Mod Rate** 2 bars and **A Mod Cutoff** around
   40%, and the filter breathes over the phrase.

### Dub chords

On a stab or chord track: 1/8. and 1/4 echoes, band-limited and a little worn.

1. **Echo** on. **A On**, **A Division** 1/8., **B On**, **B Division** 1/4, **A Feedback** and
   **B Feedback** around 70%. **A Pan** and **B Pan** -40% and +40% for width.
2. **A Filter Type** and **B Filter Type** BP, **A Cutoff** and **B Cutoff** around 900 Hz, so each
   repeat loses its lows and highs. **A Drive** around 20%, **A Wow** around 30%.
3. **A Modulator** LFO, **A Mod Rate** 4 bars, **A Mod Cutoff** around 40%: a slow filter move.
4. **Pump** on with **Pump Target** Echo to keep the wash off the kick.

**For a throw:** set **Echo Send** to 0% (the echo hears nothing), and tap **Throw Beat** or **Throw
Bar** on the last beat of a phrase. The send opens for the Move and the echo rings on.

### Drum bus

- **Low pump:** **Pump** on, **Pump Band** Lows, **Crossover** 120 Hz, **Pump Trigger** Input, **Pump
  Depth** around 70%: the bass breathes with the kick and the hats don't.
- **Loop groove:** Time **Swing** 54 to 58%, and a Volume lane **Shuffle** or **Offbeat Accent** at 1 bar.
- **Fill roller:** set **Move Quantize** to Bar and tap **Roll Build 1 Bar** during the second-to-last
  bar of the phrase. A Move waits for its line, so the roll plays in the last bar and speeds up into the
  downbeat.

### Perform (the master)

Put GrooveForce on the master (or on a drum bus and a music bus, one each) and keep Time on, so the
Moves that play the past have it. A Move waits for its line: tap it in the bar before the one you
want it in. **Move Quantize** Bar puts every Move on a downbeat; Beat (the default) puts it on a beat.

- **Into the break:** **Tape Stop** for the bar before it, then **Break** for the break itself (the
  dry goes and the echo rings: a line must be on).
- **Out of the break:** **Roll Build 4 Bars** speeds up into the downbeat at the end of its four
  bars: tap it in the bar before the four bars you want it in.
  **Reverse Beat** and **Rewind** run for a beat from their line and play the audio from before it
  backwards: with Quantize on Beat, tap them during the beat before the last beat of the phrase, so
  they start on the last beat and the downbeat comes back live.
- **Fills:** **Stutter 1/16**, **Gate 1/16** or **Half-Time Bar** for a bar.
- **Throw:** **Throw Bar** on a chord or a hit, with Echo Send low.

Time Place decides how a Move sounds. **Post** (the default): a Tape Stop stops the echoes too. **Pre**:
the echoes ring on through it.

## The limits

- **The Input trigger is a level-ratio detector.** It finds a hit when its level stands well over what
  is sustained under it. A hit has to stand out by roughly 9 to 12 dB. A kick over a 40 Hz bass 12 dB
  under the kick is found every time at the default sensitivity; at 9 dB every other one, at 6 dB one in
  four, at 3 dB none. Rumble-like material (low-passed noise) can fire it now and then. A roll of
  1/16 kicks needs a keener setting than the default (all 8 kicks found from 60% at 120 BPM, 70% at
  128 and 140, 80% at 150), which trades away some immunity to noise. The detector listens through a
  150 Hz low-pass, so a kick with no low end won't fire it. Use **Grid** for a kick that shares a
  track with bass or rumble, and Input for a drum bus.
- **The banded Pump can raise peaks.** The Lows and Highs split is an allpass in the path while the
  Pump ducks, so an edgy waveform (a full-scale square, say) can peak up to about 9 dB higher. Leave
  headroom for the next insert.
- **No sidechain.** An insert on the Force gets two inputs and nothing else: nothing can hear the kick
  on another track. The Pump and the Echo listen to the plugin's own input, or run on the grid. For
  a pad that ducks with the kick, use Grid: it locks to the same beats on any track.
- **A Shape filter moved onto an echo that is already sounding rings in.** Move Shape Target onto
  the Echo while the echo plays, and the filter starts on sound that is already there. It rings in
  for a moment, as any filter does on its first samples. The Pump and the Volume and Pan lanes don't.
- **Resonant filters in the loop.** The repeats keep the filter's resonant tone, but the loop never
  gains: feedback 100% holds the repeats and never makes them grow. What you hear can still be louder
  than what went in. The output is held to +18 dBFS.
- **The Ring's feedback means "round the ring".** See [Echo](#echo). With a line off, the line it
  feeds in Serial, Pairs and Spread hears nothing; a Ring goes round the lines that are on, and
  hands the send to the first of them.
- **Time never plays the future.** A trick that seems to need it plays what was recorded.
- **Moves are the factory 16.** You can pick and save the selected Move, the mode and the quantize in a
  preset, but not edit a Move or re-order the tiles. There is no Hold mode (a Move while the tile is
  held) and no MIDI trigger: they wait for the probe ([PROBE.md](PROBE.md)) to say what MPC sends an
  insert.
- **4/4 only.** A bar is four beats everywhere.
- **Not yet heard on the Force:** the Echo, Time and the Moves pass the test suite on x86 and under
  ARM emulation, but have not been played on a device. How the Move tiles tap there (a second tap on
  the same tile within a second is taken for MPC's release echo and ignored) and how the new
  presets sound are for the Force to say.

## How it behaves in MPC

- **Zero latency.** Nothing looks ahead, so nothing needs compensating. Time reads behind now, never
  ahead.
- **It is built not to click.** Every gain edge takes at least 1 ms and every jump is a 5 ms
  crossfade. Init passes the input bit for bit.
- **Tails ring on through Stop.** MPC briefly suspends an insert when the transport stops;
  GrooveForce keeps its echoes, its memory and a Move that plays through that. Switching the insert's
  slot **off** for longer than a quarter of a second clears them, so switching it back on never
  replays old echoes.
- **Projects** save every sound setting, the selected Move and the preset it came from. The Move that
  plays is not saved. Loading fires nothing.
- **Many copies.** Put one on each track you like. Each is independent and locks to the same song
  position; a part that is off costs no CPU (but for Time's recording, once Time has been used), and the
  Echo's and Time's memory is only taken where they are used.
- **MIDI.** GrooveForce tells MPC it takes none. Moves are played from the screen, the Q-Links and
  automation.
