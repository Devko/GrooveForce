#!/usr/bin/env python3
"""GrooveForce touchscreen surface: the ONE place the plugin's parameters and pages are defined.

Writes, next to this file:
  params.json        ordered VST parameter list (index = position; append-only once released:
                     MPC projects store values by index)
  params.lock        the keys in that order, one per line (check_lock): a key that moves fails here
  layout.conf        the skin (shadow_page.conf syntax, see
                     third_party/mpc-vst-plugins/tools/shadow_skin.py); coords are 1280x800
                     Force-Shadow pixels, the plugin area is y = 86..714
  vst.json           plugin identity for the vendored gen_vst.py
  build/param_ids.h  everything the C++ is compiled against: parameter ids, kinds, value
                     curves, names, options, defaults (the C++ never reads gen_vst's params.h, so
                     `make test` and the .so build need only Python, not the skin toolchain)
  build/factory_presets.h  the factory presets (presets/Factory/*/*.gfp), checked and embedded
  build/shape_data.h  the shape library (SHAPES), checked: the data dsp/shapes.cpp evaluates
  build/time_data.h  the Time patterns (TIME_PATTERNS), checked: the data dsp/time.cpp plays
  build/move_data.h  the 16 Moves (MOVES), checked: the data plugin/moves.cpp plays
  build/skin_style.json  the palette, knob looks, primary buttons and page groups for
                     skin_polish.py, which `make skin` runs after the generator

Continuous parameters are declared to MPC as 0..1: the real range and curve live in param_ids.h,
and the plugin formats every value text itself, so the knob, its label and the DSP can never
disagree.

Before writing anything the layout is checked the way shadow_skin.py would, plus geometry and the
parameter names MPC shows, so a broken page fails here instead of on the device. The layout
machinery and its checks are MasterForce's, which has them from EffectForce (PolyForce's), with two
more: at most five page groups, and every option text of a popup or a segment group fits where it is drawn.

Run: python3 surface/surface.py   (make surface does this)
"""
import json
import math
import os
import re
import shlex
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

STEPPER_RANGE = 1023        # the preset stepper's VST range: 0..1023 items (stepItem moves 1 per event)
BROWSER_CATS = 16           # category tiles on the browser page (2 x 8)
BROWSER_ITEMS = 24          # preset tiles (3 x 8)

VST = {"name": "GrooveForce", "vendor": "Devko", "uid": "GrFc", "version": 1000,
       "so": "grooveforce.so", "params": "params.json", "layout": "layout.conf", "effect": True}


# --- parameters ------------------------------------------------------------------------------
# kind:
#   synth    a sound parameter: saved in the state, automatable; curve lin|log|int|pow|enum
#   ui       a value the surface keeps for itself: not saved
#   readout  text the plugin writes (the status line, "PAGE 2 / 4"): read only
#   stepper  plugin-owned index into a list (presets), text = the item; moves one item per
#            Q-Link/wheel event; comes with <key>_prev / <key>_next buttons
#   button   momentary: acts on the press, springs back to 0
#   tile     a tile (list widget): lit = 1, text = the item; a tap acts
#   toggle   plugin-owned on/off (lit state follows the plugin), a tap acts
#   popup    the hidden "<key>__open" flag of a popup list (shadow_skin's popup_params)
# fmt: how the plugin prints the value (plugin/groove_map.cpp paramDisplay)
P = []


def _add(key, name, kind, curve, lo, hi, default, fmt, **extra):
    d = dict(key=key, name=name, kind=kind, curve=curve, lo=lo, hi=hi, default=default, fmt=fmt)
    d.update(extra)
    P.append(d)


def readout(key, name):
    _add(key, name, "readout", "readout", 0, 0, 0, "none")


def enum(key, name, options, default, kind="synth"):
    _add(key, name, kind, "enum", 0, len(options) - 1, options.index(default), "enum", options=options)


def num(key, name, curve, lo, hi, default, fmt):
    _add(key, name, "synth", curve, lo, hi, default, fmt)


def stepper(key, name):
    _add(key, name, "stepper", "int", 0, STEPPER_RANGE, 0, "text")
    button(key + "_prev", name + " Prev")
    button(key + "_next", name + " Next")


def button(key, name):
    _add(key, name, "button", "int", 0, 1, 0, "none")


def tile(key, name):
    _add(key, name, "tile", "enum", 0, 1, 0, "text", options=["-", "On"])


def toggle(key, name):
    _add(key, name, "toggle", "enum", 0, 1, 0, "enum", options=["Off", "On"])


def popup_flag(of):
    src = next(p for p in P if p["key"] == of)
    _add(of + "__open", "%s List" % src["name"], "popup", "enum", 0, 1, 0, "none", options=["Closed", "Open"],
         popup_of=of)


readout("status", "Status")            # index 0 must stay a read-only readout: MPC sets it at load. The status line.
# The whole effect (docs/DESIGN.md "Pages"): the level in and out, and the blend with the unprocessed signal.
# The parts' parameters (Swing, Pump, Shape) come after the preset browser's, written below the shape library,
# whose names the lanes' shape lists are.
num("input", "Input", "lin", -24, 24, 0, "db")
num("output", "Output", "lin", -24, 24, 0, "db")
num("mix", "Mix", "lin", 0, 1, 1, "pct")

# --- presets ---
stepper("preset", "Preset")
button("pre_save", "Save Preset")
button("pre_init", "Init Preset")

# --- the preset browser ---
for i in range(1, BROWSER_CATS + 1):
    tile("cat_%d" % i, "Category %d" % i)
button("cat_prev", "Categories Prev")
button("cat_next", "Categories Next")
for i in range(1, BROWSER_ITEMS + 1):
    tile("item_%d" % i, "Item %d" % i)
button("item_prev", "Items Prev")
button("item_next", "Items Next")
readout("item_page", "Items Page")
readout("br_now", "Loaded")
toggle("fav", "Favorite")
button("rnd", "Random Pick")


def norm(p):
    """The default as MPC's 0..1 value."""
    lo, hi, d = p["lo"], p["hi"], p["default"]
    if p["curve"] == "log":
        return math.log(d / lo) / math.log(hi / lo)
    if p["curve"] == "pow":
        return (d / hi) ** (1.0 / 3.0) if hi > 0 else 0.0
    return (d - lo) / (hi - lo) if hi > lo else 0.0


def params_json():
    out = []
    for p in P:
        k = p["kind"]
        e = {"key": p["key"], "name": p["name"]}
        if k == "readout":
            e.update(min=0, max=0, display="string", type="readout")
        elif k == "stepper":
            e.update(min=0, max=1, display="string", type="stepper")
        elif k == "button":
            e.update(min=0, max=1, momentary=True, type="trigger")
        elif k == "popup":
            e.update(options=p["options"], default=p["options"][0], popup_of=p["popup_of"], type="enum")
        elif k == "tile":
            e.update(options=p["options"], default=p["options"][0], display="string")
        elif "options" in p:
            e.update(options=p["options"], default=p["options"][p["default"]])
        else:
            e.update(min=0, max=1, default=round(norm(p), 6), display="string")
        out.append(e)
    return {"name": VST["name"], "params": out}


# --- touchscreen pages -------------------------------------------------------------------------
# EffectForce's look (PolyForce's td3 rounded cards on one flat colour; bg and box the same, so the opaque image of
# a control never shows a box behind it), in MasterForce's coral. Plugin area 1280x628 at y = 86..714. Every page: a
# header row (the status line from x=24; the preset stepper right-aligned to x=1256), then cards at y=158 and y=440
# (h=270) or one full-height card (h=552). Nothing may sit in a card's title band (y .. y+44: td3 draws the title
# rule at y+38). skin_polish.py (run by `make skin` after the generator) redraws the knob strips, the trigger
# buttons and the stepper arrows, and makes the pages of a group sub-pages of one tab.
PALETTE = {
    "bg": "15171c", "box": "15171c", "line": "2d3038", "ink": "e9e9f0", "ink_dim": "9a9aa8",
    "ink_faint": "282a31", "accent": "f07a5f", "accent_hi": "ffb7a6", "lcd": "0c0d10",
    "seg_inactive": "212329", "seg_active": "f07a5f", "seg_active_tx": "2b0f0a", "tile_on": "3b2420",
    "btn_bg": "2b2d34", "title": "aaa8b8", "knob_face": "26282e", "knob_ring": "3c3f47", "knob_dot": "f07a5f",
}
FONT_LABEL = "fonts/TitilliumWeb-SemiBold.ttf"   # font_label=: shadow_skin sizes the buttons with it
LIVE_FONT = "fonts/TitilliumWeb-SemiBold.ttf"    # the face MPC draws live text in (names, values)
TITLE_FONT = "fonts/TitilliumWeb-Bold.ttf"       # = SHADOW_TITLE_FONT in the Makefile: card titles, enum
                                                 # labels and segment, popup-option and button text
TITLE_SIZE = 17
THEME = ("style=td3\nfont_label=%s\ntitle_size=%d\n" % (FONT_LABEL, TITLE_SIZE)
         + "".join("theme_%s=%s\n" % kv for kv in PALETTE.items()))
TEXT_INK = PALETTE["ink_dim"]   # free bitmap text (column headers, slot numbers, hints)

S8 = [100, 252, 404, 556, 708, 860, 1012, 1164]   # 8 knob slots across a card = one Q-Link bank
L4, R4 = S8[:4], [724, 876, 1028, 1180]           # 4 slots in the left / right half card
R1, R2 = 158, 440                                 # card rows (h=270), or R1 with h=552

# Knobs: shadow_skin bakes ONE filmstrip per radius, so the radius picks the look. A bipolar knob (its arc
# grows from 12 o'clock) is one pixel smaller than a unipolar knob of the same size. This table is the only
# place that says so: check_layout() holds every knob to it and skin_polish.py draws the strips from it
# (exported as build/skin_style.json).
KNOB_SIZES = {"big": 30}
KNOB_STYLES = {r - b: {"bipolar": bool(b), "track": 4 if r - b >= 28 else 3, "pointer": 3.0 if r - b >= 28 else 2.5}
               for r in KNOB_SIZES.values() for b in (0, 1)}
BIPOLAR_EXTRA = ()                                # every bipolar knob here has a symmetric range
PRIMARY_BUTTONS = ("pre_save", "move_go")         # drawn in the accent colour by skin_polish.py
FRAMES = 128                                      # shadow_skin: every filmstrip has 128 frames


def bipolar(key):
    """A knob whose value runs both ways from the middle (the levels): symmetric range."""
    p = PARAMS[key]
    return p["lo"] == -p["hi"] or key in BIPOLAR_EXTRA


def knob_radius(key, size="big"):
    return KNOB_SIZES[size] - (1 if bipolar(key) else 0)


class Layout:
    """layout.conf lines, one generator [tab] per page. Pages are grouped: a group is one button of MPC's tab strip
    and its pages are that button's sub-pages (the dots under it; a tap on the button again shows the next), each
    with its own screen and its own Q-Link set. skin_polish.py renumbers the generator's tabs into these groups
    (skin_style.json "tab_groups")."""

    def __init__(self):
        self.lines, self.groups = [THEME], []

    def group(self, name):
        self.groups.append({"name": name, "pages": []})

    def page(self, name, qlinks):
        """A page of the current group. Its name is also its Q-Link set's title, which MPC shows in the tab strip
        while the page is up."""
        self.lines.append("[tab %s]" % name)
        self.lines.append('qlinks "%s" = %s' % (name, ",".join(qlinks)))
        self.groups[-1]["pages"].append(name)

    def add(self, line):
        self.lines.append(line)

    def header(self, status_w=1232):
        """The status line from x=24 (the preset stepper goes right-aligned to x=1256 beside a shorter one)."""
        self.readout(24 + status_w // 2, 121, status_w, "status")

    def card(self, x, y, w, h, title):
        self.add('frame x=%d y=%d w=%d h=%d title="%s"' % (x, y, w, h, title))

    def knob(self, cx, cy, key, size="big"):
        self.add('knob cx=%d cy=%d r=%d label="%s" key=%s' % (cx, cy, knob_radius(key, size), PARAMS[key]["name"], key))

    def hseg(self, cx, cy, key, sw, label=None):
        self.add('enum_h cx=%d cy=%d sw=%d key=%s%s' % (cx, cy, sw, key, ' label="%s"' % label if label else ""))

    def vseg(self, cx, cy, key, sw=124, label=None):
        self.add('enum_v cx=%d cy=%d sw=%d key=%s%s' % (cx, cy, sw, key, ' label="%s"' % label if label else ""))

    def popup(self, cx, cy, w, key):
        self.add('popup cx=%d cy=%d w=%d h=40 key=%s' % (cx, cy, w, key))

    def stepper(self, cx, cy, w, key):
        self.add('stepper cx=%d cy=%d w=%d h=40 key=%s' % (cx, cy, w, key))

    def readout(self, cx, cy, w, key, h=40):
        self.add('readout cx=%d cy=%d w=%d h=%d key=%s' % (cx, cy, w, h, key))

    def button(self, cx, cy, label, key):
        self.add('button cx=%d cy=%d label="%s" key=%s' % (cx, cy, label, key))

    def text(self, cx, top, label, size=None):   # bitmap font; top = the top of the glyphs (render_conf_preview.c)
        self.add('text cx=%d cy=%d label="%s" color=%s%s' % (cx, top, label, TEXT_INK, " size=%s" % size if size else ""))

    def toggle(self, cx, cy, key):
        self.add('toggle cx=%d cy=%d label="%s" key=%s' % (cx, cy, PARAMS[key]["name"], key))

    def tiles(self, x, y, w, cols, rows, th, gap, key):
        self.add('list x=%d y=%d w=%d cols=%d rows=%d th=%d gap=%d key=%s' % (x, y, w, cols, rows, th, gap, key))


ON_SW = 64   # an Off | On switch (one segment image size per key: a key drawn twice is drawn alike)
ORDER_SW = 104   # Echo First | Echo Last


def build_layout():
    """Every page, in groups (see Layout). Each page has its own Q-Link set: the Force's 8 knobs show the first 8
    keys, the next bank the other 8 (shadow_skin qlink_for_slot). Where a page's first 8 keys are one card's, that card
    is the top one, its controls in the Q-Links' order."""
    L = Layout()

    # GROOVE (docs/DESIGN.md "Pages"): the eight knobs that finish most presets, with the swing's unit right under the
    # Swing knob (1/16 | 1/8: the one choice that says what the knob swings), and the parts' switches: the four parts,
    # the Echo's order and Time's place.
    L.group("GROOVE")
    L.page("GROOVE", ["pump_depth", "shape_depth", "echo_send", "echo_fb", "echo_tone", "swing", "mix", "output"])
    L.header(status_w=700)
    L.stepper(998, 121, 516, "preset")
    L.card(24, R1, 1232, 270, "KNOBS")
    for cx, k in zip(S8, ("pump_depth", "shape_depth", "echo_send", "echo_fb", "echo_tone", "swing", "mix", "output")):
        L.knob(cx, R1 + 126, k)
    L.hseg(S8[5], R1 + 244, "swing_unit", ON_SW)   # no label: it sits where the knob's value text ends, and says 1/16 | 1/8
    L.card(24, R2, 1232, 270, "PARTS")
    for cx, k, label in zip(S8, ("pump_on", "shape_on", "echo_on", "time_on"), ("PUMP", "SHAPE", "ECHO", "TIME")):
        L.hseg(cx, R2 + 150, k, ON_SW, label=label)
    L.hseg(S8[5], R2 + 150, "echo_order", ORDER_SW, label="ORDER")
    L.hseg(S8[7], R2 + 150, "time_place", ON_SW, label="TIME PLACE")

    # The browser has no knobs of its own: the Q-Links keep the preset stepper and the levels.
    L.page("PRESETS", ["preset", "input", "output", "mix"])
    L.header()
    L.card(24, R1, 360, 552, "CATEGORIES")
    L.tiles(44, 206, 320, 2, 8, 48, 8, "cat")
    L.button(124, 676, "< PREV", "cat_prev")
    L.button(304, 676, "NEXT >", "cat_next")
    L.card(400, R1, 856, 474, "PRESETS")
    L.tiles(420, 206, 816, 3, 8, 38, 8, "item")
    L.button(476, 596, "< PREV", "item_prev")
    L.readout(828, 596, 240, "item_page", h=36)
    L.button(1180, 596, "NEXT >", "item_next")
    L.readout(590, 676, 380, "br_now")   # the row: 400..1256, 8 px apart
    L.toggle(848, 668, "fav")
    L.button(968, 676, "RND", "rnd")
    L.button(1083, 676, "SAVE", "pre_save")
    L.button(1197, 676, "INIT", "pre_init")

    # PUMP: the top card is the first Q-Link bank, in order; a choice of two or a few is a column of segments, a
    # long one (the ten curves) a popup list. The second card is the trigger, the target and the switch.
    L.group("PUMP+SHAPE")
    L.page("PUMP", ["pump_depth", "pump_length", "pump_curve", "pump_rate", "pump_push", "pump_band", "pump_xover",
                    "pump_mix", "pump_trigger", "pump_sens", "pump_target", "pump_on"])
    L.header()
    L.card(24, R1, 1232, 270, "PUMP")
    L.knob(S8[0], R1 + 126, "pump_depth")
    L.knob(S8[1], R1 + 126, "pump_length")
    L.text(S8[2], R1 + 82, "CURVE")
    L.popup(S8[2], R1 + 126, 150, "pump_curve")
    L.vseg(S8[3], R1 + 160, "pump_rate", sw=110, label="RATE")
    L.knob(S8[4], R1 + 126, "pump_push")
    L.vseg(S8[5], R1 + 160, "pump_band", sw=110, label="BAND")
    L.knob(S8[6], R1 + 126, "pump_xover")
    L.knob(S8[7], R1 + 126, "pump_mix")
    L.card(24, R2, 1232, 270, "TRIGGER AND TARGET")
    L.vseg(S8[0], R2 + 160, "pump_trigger", sw=110, label="TRIGGER")
    L.knob(S8[1], R2 + 126, "pump_sens")
    L.vseg(S8[2], R2 + 160, "pump_target", sw=110, label="TARGET")
    L.hseg(S8[3], R2 + 126, "pump_on", ON_SW, label="ON")

    # SHAPE: a card per lane, each with its switch, the shape and rate it plays and the knobs that move it (the
    # filter adds its type, cutoff and resonance), and a card for the part: its switch, target, depth and mix.
    # The Q-Links run across the lanes, so the cards are not banks.
    def lane(x, y, w, p, title):
        L.card(x, y, w, 270, title)
        L.hseg(x + 90, y + 92, p + "_on", ON_SW, label="ON")
        L.text(x + 295, y + 54, "SHAPE")
        L.popup(x + 295, y + 94, 250, p + "_shape")
        L.text(x + 495, y + 54, "RATE")
        L.popup(x + 495, y + 94, 130, p + "_rate")
        L.knob(x + 100, y + 170, p + "_depth")
        L.knob(x + 252, y + 170, p + "_phase")

    L.page("SHAPE", ["vol_depth", "vol_shape", "vol_rate", "flt_depth", "flt_shape", "flt_cutoff", "pan_depth",
                     "pan_shape", "vol_phase", "flt_rate", "flt_reso", "flt_type", "pan_rate", "pan_phase",
                     "shape_mix", "shape_target"])
    L.header()
    lane(24, R1, 608, "vol", "VOLUME")
    lane(648, R1, 608, "pan", "PAN")
    lane(24, R2, 816, "flt", "FILTER")
    L.hseg(24 + 700, R2 + 92, "flt_type", 56, label="TYPE")
    L.knob(24 + 404, R2 + 170, "flt_cutoff")
    L.knob(24 + 556, R2 + 170, "flt_reso")
    L.card(856, R2, 400, 270, "SHAPE")
    L.hseg(856 + 90, R2 + 92, "shape_on", ON_SW, label="ON")
    L.hseg(856 + 285, R2 + 92, "shape_target", 56, label="TARGET")
    L.knob(856 + 100, R2 + 170, "shape_depth")
    L.knob(856 + 252, R2 + 170, "shape_mix")

    # ECHO: the top card is the first Q-Link bank, in order (the six knobs, the routing as a column, Freeze). Below it
    # the four lines at a glance (a line's switch, its division and its feedback; the rest is on its own page) and the
    # part's switch, order and glide.
    L.group("ECHO")
    L.page("ECHO", ["echo_send", "echo_fb", "echo_tone", "echo_duck", "echo_width", "echo_level", "echo_route",
                    "echo_freeze"])
    L.header()
    L.card(24, R1, 1232, 270, "ECHO")
    for cx, k in zip(S8, ("echo_send", "echo_fb", "echo_tone", "echo_duck", "echo_width", "echo_level")):
        L.knob(cx, R1 + 126, k)
    L.vseg(S8[6], R1 + 160, "echo_route", sw=110, label="ROUTING")
    L.hseg(S8[7], R1 + 126, "echo_freeze", ON_SW, label="FREEZE")
    L.card(24, R2, 916, 270, "LINES")
    for cx, (p, _) in zip((138, 368, 596, 826), LINES):
        L.text(cx - 85, R2 + 54, p.upper(), size=3)   # the line's letter, left of its switch
        L.hseg(cx, R2 + 66, p + "_on", ON_SW)
        L.popup(cx, R2 + 108, 150, p + "_div")
        L.knob(cx, R2 + 167, p + "_fb")
    L.card(948, R2, 308, 270, "PART")
    L.hseg(1102, R2 + 92, "echo_on", ON_SW, label="ON")
    L.hseg(1102, R2 + 160, "echo_order", ORDER_SW, label="ORDER")
    L.hseg(1102, R2 + 228, "echo_glide", ON_SW, label="GLIDE")

    # LINE A to D, one page each. The top card is the line: its switch and timing, the time (a division, or free in
    # ms), the feedback, level and pan, the mode and the wow. The filter (type and place under the cutoff and the
    # resonance) and the modulator (what it is and plays, and its three amounts) are below. The Q-Links are the knobs
    # that shape a line most (the first bank), then the rest.
    def line_page(p):
        n = p.upper()
        L.page("LINE " + n, [p + "_" + k for k in ("div", "fb", "level", "pan", "cut", "reso", "drive", "mcut", "mtime",
                                                    "mlevel", "wow", "mod", "ftype", "mode", "ms", "on")])
        L.header()
        L.card(24, R1, 1232, 270, "LINE " + n)
        L.hseg(S8[0], R1 + 92, p + "_on", ON_SW, label="ON")
        L.hseg(S8[0], R1 + 190, p + "_sync", ON_SW, label="TIME")
        L.text(S8[1], R1 + 82, "DIVISION")
        L.popup(S8[1], R1 + 126, 150, p + "_div")
        for cx, k in zip(S8[2:6], ("ms", "fb", "level", "pan")):
            L.knob(cx, R1 + 126, p + "_" + k)
        L.vseg(S8[6], R1 + 160, p + "_mode", sw=110, label="MODE")
        L.knob(S8[7], R1 + 126, p + "_wow")
        L.card(24, R2, 456, 270, "FILTER")
        for cx, k in zip(S8, ("cut", "reso", "drive")):
            L.knob(cx, R2 + 82, p + "_" + k)
        L.hseg(S8[0], R2 + 224, p + "_ftype", 46, label="TYPE")
        L.hseg(S8[1], R2 + 224, p + "_place", 72, label="PLACE")
        L.card(488, R2, 768, 270, "MODULATOR")
        L.hseg(626, R2 + 92, p + "_mod", 60, label="SOURCE")
        L.text(626, R2 + 118, "SHAPE")
        L.popup(626, R2 + 154, 250, p + "_mshape")
        L.text(626, R2 + 182, "RATE")
        L.popup(626, R2 + 218, 130, p + "_mrate")
        for cx, k in zip((846, 1010, 1174), ("mcut", "mtime", "mlevel")):
            L.knob(cx, R2 + 126, p + "_" + k)

    for p, _ in LINES:
        line_page(p)

    # TIME: its switch, where it sits in the chain and the groove it plays; below it the groove's amount and the swing
    # (the global one, as on GROOVE) with its unit. The Q-Links follow the cards.
    L.group("TIME")
    L.page("TIME", ["time_on", "time_place", "time_groove", "time_amount", "swing", "swing_unit"])
    L.header()
    L.card(24, R1, 1232, 270, "TIME")
    L.hseg(S8[1], R1 + 126, "time_on", ON_SW, label="ON")
    L.hseg(S8[3], R1 + 126, "time_place", ON_SW, label="PLACE")
    L.vseg(S8[5], R1 + 160, "time_groove", sw=110, label="GROOVE")
    L.card(24, R2, 1232, 270, "AMOUNT AND SWING")
    L.knob(S8[1], R2 + 126, "time_amount")
    L.knob(S8[3], R2 + 126, "swing")
    L.hseg(S8[3], R2 + 244, "swing_unit", ON_SW)

    # MOVES: the 16 Moves as pads (the plugin names them and lights the ones that play or wait; a tap selects the Move
    # and fires it at the quantize), and under them the mode and the quantize every Move shares, the selected Move
    # (the `move` parameter's own text) and GO, which fires it again.
    L.page("MOVES", ["move", "move_mode", "move_quant"])
    L.header()
    L.card(24, R1, 1232, 336, "MOVES")
    L.tiles(44, R1 + 52, 1192, 4, 4, 62, 8, "move")
    L.card(24, 502, 1232, 212, "FIRING")
    L.hseg(190, 640, "move_mode", 100, label="MODE")
    L.hseg(470, 640, "move_quant", 70, label="QUANTIZE")
    L.text(800, 594, "MOVE")
    L.readout(800, 640, 320, "move")
    L.button(1130, 640, "GO", "move_go")

    # SETUP (docs/DESIGN.md "Pages"): the levels (the dry bus too), and the detectors and the Pump's crossover. The
    # status line is the header's.
    L.group("SETUP")
    L.page("SETUP", ["input", "output", "dry", "pump_sens", "pump_xover", "echo_sens"])
    L.header()
    L.card(24, R1, 608, 270, "LEVELS")
    for cx, k in zip(L4, ("input", "output", "dry")):
        L.knob(cx, R1 + 126, k)
    L.card(648, R1, 608, 270, "PUMP AND ECHO")
    for cx, k in zip(R4, ("pump_sens", "pump_xover", "echo_sens")):
        L.knob(cx, R1 + 126, k)
    return L


def pages():
    """layout.conf."""
    return "\n".join(build_layout().lines) + "\n"


def skin_style():
    """What skin_polish.py needs (build/skin_style.json): the palette, knob looks and primary buttons to redraw the
    knob strips, buttons and stepper arrows, and the page groups to renumber the generator's tabs into sub-pages."""
    return {"palette": PALETTE, "title_font": TITLE_FONT, "frames": FRAMES,
            "knobs": {str(r): s for r, s in sorted(KNOB_STYLES.items())}, "primary_buttons": list(PRIMARY_BUTTONS),
            "tab_groups": build_layout().groups}


# --- layout check (offline, no skin toolchain) ---------------------------------------------------
# Geometry mirrors third_party/mpc-vst-plugins/tools/shadow_skin.py (component boxes, button_rect,
# seg_rects, popup_layout) and render_conf_preview.c (the bitmap font of `text`).
X0, Y0, X1, Y1 = 0, 86, 1280, 714
TITLE_BAND = 44              # a card's title band: y .. y+44
MAX_GROUPS = 5               # tab groups: all MPC's tab strip shows without a pager
NAME_MAX = 13                # MPC shows a knob/slider's effGetParamName at ~19.5 px in a 130 px box
MAX_IMAGE_H = 16384          # taller skin images draw wrongly (sd88me/mpc-vst-plugins catalog_check.py warns)
POP_ROW, POP_GAP, POP_PAD, POP_GROUP_ROWS = 40, 2, 6, 8
LIVE_PX, POP_ARROW = 26, 44   # a popup's closed field: live text 26 px in the field minus its arrow (shadow_skin)
FIELD_PAD = 8                 # and the text keeps this much clear of the field's edge
TILE_PX, TILE_PAD = 24, 24    # a list tile's live text: 24 px, in the tile's width minus this (shadow_skin)
BITMAP_GLYPHS = " ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.-/_>%+:#"   # font8x8.h font_chars
BITMAP_ADVANCE = {" ": 4, "J": 9, "j": 9, ".": 9, "-": 9, ":": 9}   # font_glyph_width(): last lit column + 2; else 10
INT_KEYS = ("x", "y", "w", "h", "cx", "cy", "r", "sw", "rows", "cols", "th", "gap", "cw")


def _widget(line):
    toks = shlex.split(line)
    w = {"kind": toks[0]}
    for t in toks[1:]:
        k, _, v = t.partition("=")
        w[k] = int(v) if k in INT_KEYS else v
    return w


def bitmap_width(s, scale):
    """render_conf_preview.c text_width()."""
    return int(sum(BITMAP_ADVANCE.get(c, 10) * scale for c in s))


_FONTS = {}


def _ttf_advances(path):
    """(unitsPerEm, {char: advance}) from a TrueType font's cmap (format 4) and hmtx tables."""
    import struct
    d = open(path, "rb").read()
    tabs = {}
    for i in range(struct.unpack(">H", d[4:6])[0]):
        tag, _, off, ln = struct.unpack(">4sIII", d[12 + 16 * i:28 + 16 * i])
        tabs[tag.decode("latin-1")] = off
    upem = struct.unpack(">H", d[tabs["head"] + 18:tabs["head"] + 20])[0]
    nhm = struct.unpack(">H", d[tabs["hhea"] + 34:tabs["hhea"] + 36])[0]
    adv = [struct.unpack(">H", d[tabs["hmtx"] + 4 * i:tabs["hmtx"] + 4 * i + 2])[0] for i in range(nhm)]
    co = tabs["cmap"]
    for i in range(struct.unpack(">H", d[co + 2:co + 4])[0]):
        pid, eid, off = struct.unpack(">HHI", d[co + 4 + 8 * i:co + 12 + 8 * i])
        so = co + off
        if struct.unpack(">H", d[so:so + 2])[0] != 4 or (pid, eid) not in ((3, 1), (0, 3), (0, 4)):
            continue
        n2 = struct.unpack(">H", d[so + 6:so + 8])[0]
        ends = struct.unpack(">%dH" % (n2 // 2), d[so + 14:so + 14 + n2])
        starts = struct.unpack(">%dH" % (n2 // 2), d[so + 16 + n2:so + 16 + 2 * n2])
        deltas = struct.unpack(">%dh" % (n2 // 2), d[so + 16 + 2 * n2:so + 16 + 3 * n2])
        ro = so + 16 + 3 * n2
        ranges = struct.unpack(">%dH" % (n2 // 2), d[ro:ro + n2])
        out = {}
        for s in range(n2 // 2):
            for c in range(starts[s], min(ends[s], 0x7e) + 1):
                if ranges[s]:
                    gi = ro + 2 * s + ranges[s] + 2 * (c - starts[s])
                    g = struct.unpack(">H", d[gi:gi + 2])[0]
                    g = (g + deltas[s]) & 0xFFFF if g else 0
                else:
                    g = (c + deltas[s]) & 0xFFFF
                out[chr(c)] = adv[min(g, nhm - 1)]
        return upem, out
    raise SystemExit("%s: no Unicode cmap" % path)


def ttf_width(font, px, s):
    """Advance width of s in a bundled font at px pixels, measured with Pillow as shadow_skin does. Without
    Pillow (surface.py needs only python3): from the font's own advance table, +1 px (that is within 0.75 px
    of Pillow for Titillium Web, and errs wide)."""
    key = (font, px)
    if key not in _FONTS:
        path = os.path.join(HERE, font)
        try:
            from PIL import ImageFont
            _FONTS[key] = ImageFont.truetype(path, px).getlength
        except ImportError:
            upem, adv = _ttf_advances(path)
            _FONTS[key] = lambda t: sum(adv.get(c, upem) for c in t) * px / upem + 1.0
    return _FONTS[key](s)


def _top_level(text):
    """The style keys before the first [tab] (shadow_skin apply_theme)."""
    top = {}
    for raw in text.splitlines():
        line = raw.strip()
        if line.startswith("["):
            break
        k, eq, v = line.partition("=")
        if eq and not line.startswith("#"):
            top[k.strip()] = v.strip()
    return top


class Geometry:
    """Where shadow_skin puts each widget, for this layout's style keys."""

    def __init__(self, top):
        self.td3 = top.get("style") == "td3"
        self.font_label = top.get("font_label")
        self.ls = float(top.get("label_scale", 1.15))

    def knob(self, w):   # shadow_skin build(): the filmstrip, the Name and Value labels under it
        r = w["r"]
        s = 2 * r + 10
        cw = max(130, s)
        name_y = s // 2 + r + 2
        ch = name_y + round(20 * self.ls) + 2 + round(26 * self.ls) + 6
        return (w["cx"] - cw // 2, w["cy"] - s // 2, cw, ch)

    def slider(self, w):
        sq = max(w["w"], w["h"])
        cw = w.get("cw", max(130, sq))
        ch = (sq - w["h"]) // 2 + w["h"] + 2 + 20 + 2 + 26 + 6
        return (w["cx"] - cw // 2, w["cy"] - sq // 2, cw, ch)

    def text_width(self, s):   # shadow_skin text_width(): sizes a button
        if self.font_label:
            return int(ttf_width(self.font_label, round(9 * 1.15 * 1.6), s) * 1.2)
        return int(len(s) * 10 * 1.15 - 1.15)

    def button(self, w):   # shadow_skin button_rect()
        bw, bh = self.text_width(w["label"]) + 36, 39
        if self.td3:
            bw, bh = bw + 24 + 4, 48 + 4
        return (w["cx"] - bw // 2, w["cy"] - bh // 2, bw, bh)

    @staticmethod
    def segs(w, n):   # shadow_skin seg_rects()
        if w["kind"] == "enum_v":
            sw = w.get("sw") or 135
            y0 = w["cy"] - (n * 32) // 2
            return [(w["cx"] - sw // 2, y0 + i * 32, sw, 30) for i in range(n)]
        sw, rows = w.get("sw") or 117, w.get("rows", 1)
        per = -(-n // rows)
        out = []
        for i in range(n):
            r, c = divmod(i, per)
            cnt = min(per, n - r * per)
            out.append((w["cx"] - (cnt * sw + (cnt - 1) * 2) // 2 + c * (sw + 2), w["cy"] - 16 + r * 35, sw, 33))
        return out

    @staticmethod
    def enum_label(w, n):   # the TrueType group label shadow_skin draws centred at (gx, gy), 18 px
        gy = w["cy"] - 33 // 2 - 22 if w["kind"] == "enum_h" else w["cy"] - (n * 32) // 2 - 24
        tw = int(ttf_width(TITLE_FONT, 18, w["label"])) + 2
        return (w["cx"] - tw // 2, gy - 10, tw, 20)

    @staticmethod
    def text(w):   # render_conf_preview.c draw_text_c(): cx centres, cy is the TOP of the glyphs
        size = float(w.get("size", 1.5))
        tw = bitmap_width(w["label"], size)
        return (w["cx"] - tw // 2, w["cy"], tw + 1, int(9 * size + 0.5))

    @staticmethod
    def popup_panel(w, n):   # shadow_skin popup_layout(): the open list
        fx, fy, fw, fh = w["cx"] - w["w"] // 2, w["cy"] - w["h"] // 2, w["w"], w["h"]
        below, above = Y1 - (fy + fh + 4), fy - 4 - Y0
        groups = [(t, int(c)) for t, _, c in (g.rpartition(":") for g in w["groups"].split(","))] \
            if w.get("groups") else None
        if groups:
            rows = min(POP_GROUP_ROWS, max(c for _, c in groups))
            cols = sum(-(-c // rows) for _, c in groups)
            ph = (rows + 1) * (POP_ROW + POP_GAP) - POP_GAP + 2 * POP_PAD
        else:
            for cols in ([int(w["cols"])] if w.get("cols") else range(1, n + 1)):
                rows = -(-n // cols)
                ph = rows * (POP_ROW + POP_GAP) - POP_GAP + 2 * POP_PAD
                if ph <= max(below, above):
                    break
        pw = cols * fw + (cols - 1) * POP_GAP + 2 * POP_PAD
        py = fy + fh + 4 if ph <= below else fy - 4 - ph if ph <= above else Y0
        return (max(0, min(fx, X1 - pw)), py, pw, ph)

    def rects(self, w, params):
        """[(x, y, w, h)] of a control, as shadow_skin places it (stepper: arrows and text together)."""
        k = w["kind"]
        if k == "knob":
            return [self.knob(w)]
        if k in ("slider_v", "slider_h"):
            return [self.slider(w)]
        if k == "toggle":
            return [(w["cx"] - 60, w["cy"] - 18, 120, 58)]
        if k == "button":
            return [self.button(w)]
        if k in ("readout", "stepper", "popup", "menu"):
            return [(w["cx"] - w["w"] // 2, w["cy"] - w["h"] // 2, w["w"], w["h"])]
        if k == "list":
            tw = (w["w"] - (w["cols"] - 1) * w["gap"]) // w["cols"]
            return [(w["x"] + c * (tw + w["gap"]), w["y"] + r * (w["th"] + w["gap"]), tw, w["th"])
                    for r in range(w["rows"]) for c in range(w["cols"])]
        if k in ("enum_h", "enum_v"):
            return self.segs(w, len(params[w["key"]]["options"]))
        if k == "meter":   # shadow_skin: a square of the larger side, centred (transparent padding)
            sq = max(w["w"], w["h"])
            return [(w["cx"] - sq // 2, w["cy"] - sq // 2, sq, sq)]
        return []


def _overlap(a, b):
    return a[0] < b[0] + b[2] and b[0] < a[0] + a[2] and a[1] < b[1] + b[3] and b[1] < a[1] + a[3]


def _inside(r):
    return r[0] >= X0 and r[1] >= Y0 and r[0] + r[2] <= X1 and r[1] + r[3] <= Y1


def _same_screen(m1, m2):
    return m1 is None or m2 is None or m1 == m2


def check_names(layout_tabs, geo, errors):
    """Parameter names: MPC shows them under knobs and sliders and in its Q-Link overlay, without page context."""
    seen = {}
    for p in P:
        if len(p["name"]) > 24:
            errors.append("parameter %s: name %r is longer than 24 characters" % (p["key"], p["name"]))
        if p["name"].lower() in seen:
            errors.append("parameters %s and %s have the same name %r" % (seen[p["name"].lower()], p["key"], p["name"]))
        seen[p["name"].lower()] = p["key"]
    for tab in layout_tabs:
        for w in tab["widgets"]:
            if w["kind"] not in ("knob", "slider_v", "slider_h", "toggle") or w.get("key") not in PARAMS:
                continue
            name = PARAMS[w["key"]]["name"]
            if len(name) > NAME_MAX:
                errors.append("%s: %s %s: name %r is longer than %d characters" % (tab["name"], w["kind"], w["key"], name,
                                                                                    NAME_MAX))
            # the live Name label: knob 17 x label_scale px in max(130, 2r+10); slider 17 px in cw; toggle 15 px in 120
            px, box = ((math.ceil(17 * geo.ls), max(130, 2 * w["r"] + 10)) if w["kind"] == "knob" else
                       (15, 120) if w["kind"] == "toggle" else (17, w.get("cw") or max(130, w["w"], w["h"])))
            if ttf_width(LIVE_FONT, px, name) > box - 4:
                errors.append("%s: %s %s: name %r does not fit its %d px label" % (tab["name"], w["kind"], w["key"], name, box))


def option_fit(w, options):
    """What of a popup's or a segment group's option texts is too wide for where it is drawn, as shadow_skin draws
    it: the closed popup's field shows the pick in the live face (LIVE_PX px) in the field minus the arrow; an open
    list's cells and the segments hold it in the title face, centred, 42% of the cell's height tall."""
    out = []
    for o in options:
        if w["kind"] == "popup":
            room = w["w"] - POP_ARROW - FIELD_PAD
            if ttf_width(LIVE_FONT, LIVE_PX, o) > room:
                out.append("%r is wider than the field (%d px of text, %d px of room)" % (o, ttf_width(LIVE_FONT, LIVE_PX, o),
                                                                                           room))
            cell_w, cell_h = w["w"], POP_ROW
        else:
            cell_w, cell_h = w.get("sw") or (135 if w["kind"] == "enum_v" else 117), 30 if w["kind"] == "enum_v" else 33
        px = max(10, int(cell_h * 0.42))
        if ttf_width(TITLE_FONT, px, o) > cell_w - 8:
            out.append("%r is wider than its %d px cell (%d px of text at %d px)" % (o, cell_w, ttf_width(TITLE_FONT, px, o), px))
    return out


def check_layout(text, groups):
    """Raise SystemExit on anything shadow_skin.py would refuse, plus geometry mistakes: outside the plugin
    area, overlaps on one screen (a page mode with everything shown in every mode), controls or text in a
    card's title band, open popup lists that leave the plugin area, unknown bitmap glyphs; and the page groups
    (Layout): every page in one group, in layout order, with exactly one Q-Link set titled like the page."""
    params = PARAMS
    geo = Geometry(_top_level(text))
    errors = []
    tabs = []
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or (not tabs and "=" in line and not line.startswith("[")):
            continue
        m = re.match(r"\[tab (.+)\]$", line)
        if m:
            tabs.append({"name": m.group(1), "widgets": [], "qlinks": []})
            continue
        if line.startswith("qlinks"):
            m = re.match(r'qlinks\s+"([^"]+)"\s*=\s*(.+)$', line)
            keys = [k.strip() for k in m.group(2).split(",") if k.strip()]
            tabs[-1]["qlinks"].append((m.group(1), keys))
            continue
        tabs[-1]["widgets"].append(_widget(line))
    if len(groups) > MAX_GROUPS:
        errors.append("%d page groups: MPC's tab strip shows five without a pager; keep it to %d" % (len(groups), MAX_GROUPS))
    errors += ["page group %s has no pages" % g["name"] for g in groups if not g["pages"]]
    grouped, names = [n for g in groups for n in g["pages"]], [t["name"] for t in tabs]
    if grouped != names:
        errors.append("the page groups list %s, the layout has the pages %s" % (grouped, names))
    errors += ["page %r: the name is taken (MPC tells pages apart by it)" % n for n in sorted(set(names))
               if names.count(n) > 1]
    for t in tabs:
        if [title for title, _ in t["qlinks"]] != [t["name"]]:
            errors.append("%s: a page has exactly one Q-Link set, titled like the page" % t["name"])
    check_names(tabs, geo, errors)
    errors += ["PRIMARY_BUTTONS: %r is not a button parameter" % k for k in PRIMARY_BUTTONS
               if PARAMS.get(k, {}).get("kind") != "button"]
    errors += ["BIPOLAR_EXTRA: %r is not a parameter" % k for k in BIPOLAR_EXTRA if k not in PARAMS]
    seg_images = {}   # shadow_skin names enum images sh_seg_<key>_<n> for the whole skin: one size per key
    for tab in tabs:
        T = tab["name"]
        frames, placed = [], []   # (rect, title, mode); (rect, what, mode)
        for w in tab["widgets"]:
            kind, key, mode = w["kind"], w.get("key"), w.get("when")
            if mode:
                mk, _, mo = mode.partition(":")
                opts = [o.lower() for o in params.get(mk, {}).get("options", [])]
                if len(opts) < 2 or mo.lower() not in opts:
                    errors.append("%s: when=%s is not an option of an option parameter" % (T, mode))
            if kind == "frame":
                r = (w["x"], w["y"], w["w"], w["h"])
                if not _inside(r):
                    errors.append("%s: frame %r at %s leaves the plugin area" % (T, w.get("title"), r))
                frames.append((r, w.get("title", ""), mode))
                continue
            if kind == "text":
                lab = w.get("label", "")
                bad = sorted(set(c for c in lab if c not in BITMAP_GLYPHS))
                if not lab or bad:
                    errors.append("%s: text %r: %s" % (T, lab, "the bitmap font has no %r" % "".join(bad) if bad
                                                       else "an empty label fails shadow_art"))
                placed.append((Geometry.text(w), "text %r" % lab, mode))
                continue
            if kind == "art":
                errors.append("%s: art needs the browser renderer" % T)
                continue
            need = ["%s_%d" % (key, i + 1) for i in range(w["cols"] * w["rows"])] if kind == "list" else [key]
            if kind == "stepper":
                need += [key + "_prev", key + "_next"]
            if kind == "popup":
                need.append(key + "__open")
            missing = [k for k in need if k not in params]
            for k in missing:
                errors.append("%s: %s key %r is not a parameter" % (T, kind, k))
            if missing:
                continue
            p = params[need[0]]
            if kind in ("enum_h", "enum_v", "popup") and "options" not in p:
                errors.append("%s: %s %r is not an option parameter" % (T, kind, key))
                continue
            if kind == "list" and p["kind"] != "tile":
                errors.append("%s: list %r tiles must be tile parameters" % (T, key))
            if kind == "list":   # the tile's text is live: what the plugin writes there must fit
                room = (w["w"] - (w["cols"] - 1) * w["gap"]) // w["cols"] - TILE_PAD
                errors += ["%s: list %s: %r is wider than its tile (%d px of text, %d px of room)" % (
                    T, key, t, ttf_width(LIVE_FONT, TILE_PX, t), room)
                    for t in TILE_TEXTS.get(key, []) if ttf_width(LIVE_FONT, TILE_PX, t) > room]
            if kind == "stepper" and p["kind"] != "stepper":
                errors.append("%s: stepper %r is not a stepper parameter" % (T, key))
            if kind == "meter" and p["kind"] != "meter":
                errors.append("%s: meter %r is not a meter parameter" % (T, key))
            if kind == "button" and not w.get("label"):
                errors.append("%s: button %r needs a label" % (T, key))
                continue
            strip = {"knob": lambda: 2 * w["r"] + 10, "slider_v": lambda: max(w["w"], w["h"]),
                     "slider_h": lambda: max(w["w"], w["h"]), "meter": lambda: max(w["w"], w["h"])}.get(kind)
            if strip and strip() * FRAMES > MAX_IMAGE_H:   # FRAMES square frames stacked: one tall image
                errors.append("%s: %s %s: its filmstrip is %d px tall; MPC draws images over %d px wrongly" % (
                    T, kind, key, strip() * FRAMES, MAX_IMAGE_H))
            if kind == "knob":
                style = KNOB_STYLES.get(w["r"])
                if not style:
                    errors.append("%s: knob %s: r=%d has no look in KNOB_STYLES" % (T, key, w["r"]))
                elif style["bipolar"] != bipolar(key):
                    errors.append("%s: knob %s: r=%d is a %s look, the parameter is %s" % (
                        T, key, w["r"], "bipolar" if style["bipolar"] else "unipolar",
                        "bipolar" if bipolar(key) else "unipolar"))
            if kind in ("readout", "stepper", "popup") and w["h"] < 36:
                errors.append("%s: %s %s: h=%d clips its 26 px live text (min 36)" % (T, kind, key, w["h"]))
            if kind in ("enum_h", "enum_v"):
                n = len(p["options"])
                size = (kind, w.get("sw"), n)
                if seg_images.setdefault(key, size) != size:
                    errors.append("%s: enum %s is drawn as %s and %s: its segment images are shared" % (
                        T, key, seg_images[key], size))
                if w.get("label"):
                    placed.append((Geometry.enum_label(w, n), "%s label" % key, mode))
            if kind == "popup":
                panel = Geometry.popup_panel(w, len(p["options"]))
                if not _inside(panel):
                    errors.append("%s: popup %s: its open list %s leaves the plugin area" % (T, key, panel))
            if kind in ("popup", "enum_h", "enum_v"):
                errors += ["%s: %s %s: %s" % (T, kind, key, bad) for bad in option_fit(w, p["options"])]
            for r in geo.rects(w, params):
                placed.append((r, "%s %s" % (kind, key), mode))
        for i, (r, what, mode) in enumerate(placed):
            if not _inside(r):
                errors.append("%s: %s at %s leaves the plugin area" % (T, what, r))
            for o_r, o_what, o_mode in placed[:i]:
                if what.startswith("meter ") and o_what.startswith("meter "):
                    continue   # a row of meters: their padded squares overlap, transparent and untouchable
                if _same_screen(mode, o_mode) and _overlap(r, o_r) and o_what != what:
                    errors.append("%s: %s overlaps %s" % (T, what, o_what))
            for f_r, title, f_mode in frames:
                band = (f_r[0], f_r[1], f_r[2], TITLE_BAND)
                if _same_screen(mode, f_mode) and _overlap(r, band):
                    errors.append("%s: %s at %s is in the title band of card %r" % (T, what, r, title))
        titles = [t for t, _ in tab["qlinks"]]
        for title, keys in tab["qlinks"]:
            if len(keys) > 16:
                errors.append("%s: qlinks %r has %d keys (max 16)" % (T, title, len(keys)))
            if len(title) > 12 or titles.count(title) > 1:
                errors.append("%s: qlinks title %r: keep it unique and at most 12 characters (MPC's tab strip)" % (T, title))
            for k in keys:
                if k not in params:
                    errors.append("%s: qlinks %r key %r is not a parameter" % (T, title, k))
    if errors:
        raise SystemExit("layout check failed:\n  " + "\n  ".join(errors))
    return tabs


# --- C++ header --------------------------------------------------------------------------------
CURVE = {"readout": "Readout", "enum": "Enum", "lin": "Lin", "log": "Log", "int": "Int", "pow": "Pow"}
FMT = {"none": "None", "enum": "Enum", "pct": "Percent", "spct": "SignedPercent", "db": "Db", "ms": "Ms", "hz": "Hz",
       "text": "Text"}
KIND = {"synth": "Synth", "ui": "Ui", "readout": "Readout", "stepper": "Stepper", "button": "Button", "tile": "Tile",
        "toggle": "Toggle", "popup": "Popup"}


def c_str(s):
    return '"' + str(s).replace("\\", "\\\\").replace('"', '\\"') + '"'


def header():
    index = {p["key"]: i for i, p in enumerate(P)}
    ids = ",\n".join("    P_%s%s" % (p["key"].upper(), " = 0" if i == 0 else "") for i, p in enumerate(P))
    specs = ",\n".join("    {Curve::%s, Fmt::%s, %sf, %sf}  /* %s */" % (
        CURVE[p["curve"]], FMT[p["fmt"]], float(p["lo"]), float(p["hi"]), p["key"]) for p in P)
    opts = []
    for i, p in enumerate(P):
        if "options" in p:
            opts.append("static constexpr const char* OPTS_%d[] = {%s};" % (i, ", ".join(c_str(o) for o in p["options"])))
    info = ",\n".join("    {%s, %s, Kind::%s, %rf, %d, %s, %d}" % (
        c_str(p["key"]), c_str(p["name"]), KIND[p["kind"]], float(round(norm(p), 6)),
        len(p.get("options", [])), "OPTS_%d" % i if "options" in p else "nullptr",
        index[p["popup_of"]] if p["kind"] == "popup" else -1) for i, p in enumerate(P))
    uid = int.from_bytes(VST["uid"].encode(), "big")
    return """// generated by surface/surface.py: do not edit
#pragma once
#include <cstdint>

namespace gf {

enum ParamId : int {
%s,
    P_COUNT
};

enum class Curve : unsigned char { Readout, Enum, Lin, Log, Int, Pow };
enum class Fmt : unsigned char { None, Enum, Percent, SignedPercent, Db, Ms, Hz, Text };
// Who owns the value and what a set does: see surface.py "kind".
enum class Kind : unsigned char { Synth, Ui, Readout, Stepper, Button, Tile, Toggle, Popup };

struct ParamSpec { Curve curve; Fmt fmt; float lo, hi; };
struct ParamInfo {
    const char* key;
    const char* name;
    Kind kind;
    float def;                  // MPC's 0..1 default
    int nopts;
    const char* const* opts;
    int popupOf;                // Kind::Popup: the parameter whose list it opens, else -1
};

static constexpr ParamSpec PARAM_SPECS[P_COUNT] = {
%s
};

%s

static constexpr ParamInfo PARAM_INFO[P_COUNT] = {
%s
};

constexpr const char* kPlugName = %s;
constexpr const char* kPlugVendor = %s;
constexpr int32_t kPlugUid = 0x%08x;   // '%s'
constexpr int32_t kPlugVersion = %d;

constexpr int kStepperRange = %d;
constexpr int kBrowserCats = %d;
constexpr int kBrowserItems = %d;

} // namespace gf
""" % (ids, specs, "\n".join(opts), info, c_str(VST["name"]), c_str(VST["vendor"]), uid, VST["uid"],
       VST["version"], STEPPER_RANGE, BROWSER_CATS, BROWSER_ITEMS)


# --- factory presets: presets/Factory/<NN_Category>/<NN_Name>.gfp, embedded in the .so ---------
PRESET_DIR = os.path.join(HERE, "..", "presets", "Factory")
PRESET_EXT = ".gfp"
STATE_EXTRA_KEYS = ("preset",)
PRESET_NAME_MAX = 18      # an item tile on the browser page (816 px / 3 columns)
CATEGORY_NAME_MAX = 12    # a category tile (320 px / 2 columns), shown in capitals
OUTPUT_MAX_DB = 9.0       # a factory preset's Output: tools/levels.cpp sets it within this
# The categories tools/reference_mix.h has material for (its kCategoryParts: change both together); anything else is
# judged on the full mix.
LEVEL_CATEGORIES = ("Kick", "Bass", "Hats + Perc", "Chords", "Drum Bus", "Perform", "Echo", "Utility")


def _shown(entry):
    """"02_Warm_Tape.gfp" -> "Warm Tape", "03_Loud" -> "Loud". A name can't hold a "/" in a file name: "%2F" is one
    ("07_Rolling_3%2F16.gfp" -> "Rolling 3/16", the Echo's division names)."""
    return re.sub(r"^\d+\s+", "", re.sub(r"\.gfp$", "", entry).replace("_", " ")).replace("%2F", "/")


def _defines(values):
    """What a factory preset states about each part, Shape lane and Echo line it has on, even where that is today's
    default (ruling R56): the values that define it, so that a later change of a default never moves a factory preset
    (params.lock pins the keys, not their defaults). The keys it must name, with what they belong to."""
    need = []
    if values.get("pump_on") == "On":
        need += [(k, "the Pump") for k in ("pump_rate", "pump_curve", "pump_depth", "pump_length")]
        if values.get("pump_band", "Full") != "Full":
            need.append(("pump_xover", "a banded Pump"))
    if values.get("shape_on") == "On":
        for lane, name in LANES:
            if values.get(lane + "_on") == "On":
                need += [(lane + k, "the %s lane" % name) for k in ("_shape", "_rate", "_depth")]
        if values.get("flt_on") == "On":
            need += [(k, "the Filter lane") for k in ("flt_type", "flt_cutoff")]
    if values.get("echo_on") == "On":
        need.append(("echo_route", "the Echo"))
        for line, _ in LINES:
            if values.get(line + "_on") == "On":
                what = "line " + line.upper()
                timing = (line + "_ms",) if values.get(line + "_sync") == "Free" else (line + "_div",)
                need += [(k, what) for k in (line + "_level",) + timing + (line + "_fb",)]
                if line + "_cut" in values:
                    need.append((line + "_ftype", what + "'s filter"))
    if values.get("time_on") == "On":
        need.append(("time_place", "Time"))
        groove = values.get("time_groove", "Off")
        if groove == "Swing":
            need += [(k, "Time's Swing") for k in ("swing", "swing_unit")]
        elif groove != "Off":
            need.append(("time_amount", "Time's " + groove))
    if values.get("move", "None") != "None":
        need += [(k, "the Move") for k in ("move_mode", "move_quant")]
    return need


def check_preset_sound(where, values, params):
    """What a factory preset's lines say together, beyond each line on its own (EffectForce's rules, GrooveForce's
    parts). Nothing is set on a part, a Shape lane or an Echo line the preset leaves off (a lane's or a line's switch
    included), unless the Move it selects switches that on; a Move's mode and quantize go with a selected Move. What
    defines each part that is on is stated (_defines, R56). A selected Move needs Time on: every factory preset with Moves
    has it, so that the other tiles' Reverse Beat, Spin-Back and Rewind find audio Time has recorded (docs/DESIGN.md,
    Moves; R42). The Output stays within +-OUTPUT_MAX_DB (tools/levels.cpp sets it). `values` maps keys to the lines'
    values, options by name. Returns the errors."""
    errors = []
    on = {k for k, v in values.items() if k.endswith("_on") and v == "On"}
    move = values.get("move", "None")
    mv = next((m for m in MOVES if m["name"] == move), None)
    if mv:
        on |= {MOVE_PART_SWITCH[q] for q in mv["parts"]}
        on |= {r["key"] for r in mv["ramps"] if r["kind"] == "set" and r["key"].endswith("_on") and r["value"] == "On"}
        if values.get("time_on") != "On":
            errors.append("%s: selects the Move %s but leaves Time off (a preset with Moves has Time on, so that Reverse "
                          "Beat, Spin-Back and Rewind find recorded audio)" % (where, move))
    else:
        errors += ["%s: sets %s but selects no Move" % (where, k) for k in ("move_mode", "move_quant") if k in values]
    names = {"pump": "the Pump", "shape": "Shape", "echo": "the Echo", "time": "Time"}
    for key, val in values.items():
        if key == "output" and abs(float(val)) > OUTPUT_MAX_DB + 1e-9:
            errors.append("%s: output=%s: a factory preset's Output stays within +-%g dB (tools/levels.cpp)"
                          % (where, val, OUTPUT_MAX_DB))
        part = _part_of(key)
        if part and key != MOVE_PART_SWITCH[part] and MOVE_PART_SWITCH[part] not in on:
            errors.append("%s: sets %s but leaves %s off" % (where, key, names[part]))
        own = next((s for s in ("vol_", "flt_", "pan_", "a_", "b_", "c_", "d_") if key.startswith(s)), None)
        if own and key != own + "on" and own + "on" not in on:
            errors.append("%s: sets %s but leaves %s off" % (where, key, own + "on"))
    for key, what in _defines(values):
        if key not in values:
            errors.append("%s: %s is on but %s is not stated (a preset states what defines it, R56)" % (where, what, key))
    return errors


def check_preset_line(params, key, val):
    """None if key=val is a valid sound line, else what is wrong with it. Options go by name (or index)."""
    p = params.get(key)
    if not p or p["kind"] != "synth":
        return "%r is not a sound parameter" % key
    if "options" in p:
        if val in p["options"] or (val.isdigit() and int(val) < len(p["options"])):
            return None
        return "%r is not an option of %s (%s)" % (val, key, ", ".join(p["options"]))
    try:
        v = float(val)
    except ValueError:
        return "%r is not a number" % val
    if not (min(p["lo"], p["hi"]) - 1e-9 <= v <= max(p["lo"], p["hi"]) + 1e-9):
        return "%s=%s outside %s..%s" % (key, val, p["lo"], p["hi"])
    return None


def factory_presets():
    """[(category, name, text)]: one folder per category, both in file order ("NN_" orders them, "_" shows as a
    space). A factory preset sets sound parameters only, never the project's own (the preset index). Every line a
    sound parameter with a value in range (options by name), the lines together sound (check_preset_sound), every
    name unique (keys are "builtin:<name>") and short enough for its tile: a typo fails the build, not the device. A
    category the level-matching has no material for is a warning."""
    params = {p["key"]: p for p in P}
    out, errors, seen = [], [], {}
    for d in sorted(os.listdir(PRESET_DIR)):
        folder = os.path.join(PRESET_DIR, d)
        if d.endswith(PRESET_EXT):
            errors.append("%s: put it in a category folder (presets/Factory/NN_Category/)" % d)
            continue
        if not os.path.isdir(folder):
            continue
        category = _shown(d)
        if not category or len(category) > CATEGORY_NAME_MAX:
            errors.append("%s: a category name of 1..%d characters" % (d, CATEGORY_NAME_MAX))
        if category not in LEVEL_CATEGORIES:
            print("warning: presets/Factory/%s: tools/reference_mix.h has no material for %r: make preset-levels "
                  "judges it on the full mix" % (d, category), file=sys.stderr)
        for f in sorted(os.listdir(folder)):
            if not f.endswith(PRESET_EXT):
                continue
            where = "%s/%s" % (d, f)
            text = open(os.path.join(folder, f), encoding="utf-8").read().replace("\r\n", "\n")
            lines = text.split("\n")
            if not lines[0].startswith("grooveforce "):
                errors.append("%s: no 'grooveforce N' header" % where)
            seen_keys, values = {}, {}
            for n, line in enumerate(lines[1:], 2):
                if not line.strip():
                    continue
                key, _, val = line.partition("=")
                if key in seen_keys:
                    errors.append("%s:%d: %s is set twice (line %d too)" % (where, n, key, seen_keys[key]))
                seen_keys[key] = n
                if key in STATE_EXTRA_KEYS:
                    errors.append("%s:%d: %s belongs in a project, not a preset" % (where, n, key))
                    continue
                bad = check_preset_line(params, key, val)
                if bad:
                    errors.append("%s:%d: %s" % (where, n, bad))
                else:
                    opts = params[key].get("options")
                    values[key] = opts[int(val)] if opts and val.isdigit() else val
            errors += check_preset_sound(where, values, params)
            name = _shown(f)
            if name in seen:
                errors.append("%s: the name %r is taken by %s" % (where, name, seen[name]))
            seen[name] = where
            if len(name) > PRESET_NAME_MAX:
                errors.append("%s: %r is longer than %d characters" % (where, name, PRESET_NAME_MAX))
            out.append((category, name, text))
    if "Init" not in seen:
        errors.append("no Init preset (the INIT button loads builtin:Init)")
    if errors:
        raise SystemExit("factory presets:\n  " + "\n  ".join(errors))
    return out


def presets_header(presets):
    rows = ",\n".join("    {%s, %s, %s}" % (c_str(c), c_str(n), c_str(t).replace("\n", "\\n")) for c, n, t in presets)
    return """// generated by surface/surface.py from presets/Factory/*/*.gfp: do not edit
#pragma once

namespace gf {

struct FactoryPreset { const char* category; const char* name; const char* text; };
static const FactoryPreset kFactoryPresets[] = {
%s
};
constexpr int kNumFactoryPresets = %d;

} // namespace gf
""" % (rows, len(presets))


# --- the shape library: docs/DESIGN.md "Shape" ---------------------------------------------------
# Named shapes over one cycle, held as data here and checked here; build/shape_data.h is what
# dsp/shapes.cpp evaluates. Shape's lanes pick one by name, so the order below is the shape
# parameters' option list (a saved project stores the choice by index: append, never reorder). Five kinds:
#   points         breakpoints (x, y, seg, tension): x ascending in [0, 1) from 0, y 0..1. A point's seg is
#                  the way to the next point; the last runs on to the first one at x = 1, so a shape wraps.
#                  seg: linear; smooth (a raised cosine); hold (stays at y); curve (an exponential from y to
#                  the next y, bent by tension -1..1: positive starts slow, negative fast, 0 is a line)
#   steps          16 levels, one per sixteenth of the cycle (a gate is 1 and 0)
#   pump           the Pump curve of that name (PUMP_CURVES): 0 at phase 0, recovering across the cycle
#   random_steps   a level per step drawn from the bar the cycle is in: 16 steps a cycle
#   random_smooth  4 smooth segments a cycle between drawn levels (dsp/shapes.cpp draws them)
PUMP_CURVES = ["Smooth", "Soft", "Punch", "Hard", "Snap", "Long", "Late", "Linear", "S-Curve", "Hold"]   # kPumpCurveNames
SHAPE_COUNT = 38            # a deliberate number: a project stores the shape by its index
SHAPE_NAME_MAX = 14         # "Offbeat Accent", which the factory presets name: an entry of a popup list
SHAPE_STEPS = 16
SHAPE_SEGS = ("linear", "smooth", "hold", "curve")
SHAPE_KINDS = ("points", "steps", "pump", "random_steps", "random_smooth")
HARD_EDGE = 0.9999          # a saw or ramp ends on a last point held to the wrap: a hard edge, as the square's

# EffectForce's gate patterns (Pulse::kPatternSteps; 'x' open, '.' closed) except "1/16" (all open) and "Pump".
GATES = [
    ("1/8", "x.x.x.x.x.x.x.x."), ("1/4", "x...x...x...x..."), ("Offbeat", "..x...x...x...x."),
    ("Off 16ths", ".x.x.x.x.x.x.x.x"), ("Dotted", "x..x..x..x..x..x"), ("Tresillo", "x..x..x.x..x..x."),
    ("Gallop", "x.xxx.xxx.xxx.xx"), ("Rev Gallop", "xxx.xxx.xxx.xxx."), ("Trance 1", "x.xx.xx.x.xx.xx."),
    ("Trance 2", "xx.xx.x.xx.xx.x."), ("Trance 3", "x.xxx.x.x.xxx.x."), ("Stutter", "x.x.x.x.xxxxxxxx"),
    ("Half Bar", "xxxxxxxx........"), ("Build", "x...x...x.x.xxxx"),
]


def _points(name, *points):
    """A points shape; a point is (x, y), (x, y, seg) or (x, y, seg, tension)."""
    return dict(name=name, kind="points",
                points=[(float(p[0]), float(p[1]), p[2] if len(p) > 2 else "linear", float(p[3]) if len(p) > 3 else 0.0)
                        for p in points])


def _steps(name, levels):
    return dict(name=name, kind="steps", levels=[float(v) for v in levels])


def _gate(name, pattern):
    return _steps(name, [1.0 if c == "x" else 0.0 for c in pattern])


def _accent(name, pattern, every, hit, rest):
    """The pattern's open steps at `rest`, those on every `every`th step (a group's head) at `hit`, the rest 0."""
    return _steps(name, [0.0 if c != "x" else hit if i % every == 0 else rest for i, c in enumerate(pattern)])


SHAPES = [
    # waves
    _points("Sine", (0, 0, "smooth"), (0.5, 1, "smooth")),
    _points("Triangle", (0, 0), (0.5, 1)),
    _points("Saw Up", (0, 0), (HARD_EDGE, 1, "hold")),
    _points("Saw Down", (0, 1), (HARD_EDGE, 0, "hold")),
    _points("Ramp Up", (0, 0, "curve", 0.5), (HARD_EDGE, 1, "hold")),        # a saw, bent: a build that swells late
    _points("Ramp Down", (0, 1, "curve", -0.5), (HARD_EDGE, 0, "hold")),     # a saw, bent: falls fast, trails off
    _points("Square", (0, 1, "hold"), (0.5, 0, "hold")),
    _points("Pulse 25", (0, 1, "hold"), (0.25, 0, "hold")),
] + [dict(name="Pump " + c, kind="pump", curve=c) for c in PUMP_CURVES] + [_gate(n, p) for n, p in GATES] + [
    # accents: steps at levels rather than on/off
    _steps("Offbeat Accent", [1.0 if i % 4 == 2 else 0.45 for i in range(SHAPE_STEPS)]),
    _steps("Rolling 16ths", [(1.0, 0.55, 0.75, 0.55)[i % 4] for i in range(SHAPE_STEPS)]),
    _accent("Gallop Accent", dict(GATES)["Gallop"], 4, 1.0, 0.6),   # the eighth that opens each gallop at 1, its two 16ths 0.6
    _steps("Shuffle", [(1.0, 0.3, 0.7, 0.5)[i % 4] for i in range(SHAPE_STEPS)]),
    # random
    dict(name="Random Steps", kind="random_steps"),
    dict(name="Random Smooth", kind="random_smooth"),
]


# --- the parts' parameters: docs/DESIGN.md "The parts" ----------------------------------------------
# Appended after the preset browser's (a project stores a parameter by its index), but written here because the
# lanes' shape lists are the names of SHAPES. A choice of many options is a popup list, so its hidden "__open"
# flag follows it. Everything is off or at rest by default: Init passes the input through bit for bit.
ON_OFF = ["Off", "On"]
TARGETS = ["All", "Dry", "Echo"]       # what a part acts on: the whole signal, the dry, or the Echo's wet only
PUMP_RATES = ["1/16", "1/8", "1/4", "1/2", "1 bar"]                                                           # kPumpRates
SHAPE_RATES = ["1/16", "1/8T", "1/8", "1/4T", "1/8.", "1/4", "1/4.", "1/2", "1/2.", "1 bar", "2 bars", "4 bars"]   # kShapeRates
SHAPE_NAMES = [sh["name"] for sh in SHAPES]
LANES = (("vol", "Volume"), ("flt", "Filter"), ("pan", "Pan"))

# the grid: one swing for every part (50% is straight)
num("swing", "Swing", "lin", 0.5, 0.75, 0.5, "pct")
enum("swing_unit", "Swing Unit", ["1/16", "1/8"], "1/16")

# Pump: the duck on every step of the grid (or every hit of the input)
enum("pump_on", "Pump", ON_OFF, "Off")
enum("pump_rate", "Pump Rate", PUMP_RATES, "1/4")
enum("pump_curve", "Pump Curve", PUMP_CURVES, "Smooth")
popup_flag("pump_curve")
num("pump_depth", "Pump Depth", "lin", 0, 1, 0, "pct")
num("pump_length", "Pump Length", "lin", 0.05, 1, 0.6, "pct")
num("pump_push", "Pump Push", "lin", -10, 30, 0, "ms")
enum("pump_trigger", "Pump Trigger", ["Grid", "Input"], "Grid")
num("pump_sens", "Sensitivity", "lin", 0, 1, 0.5, "pct")
enum("pump_band", "Pump Band", ["Full", "Lows", "Highs"], "Full")
num("pump_xover", "Crossover", "log", 60, 300, 120, "hz")
enum("pump_target", "Pump Target", TARGETS, "All")
num("pump_mix", "Pump Mix", "lin", 0, 1, 1, "pct")

# Shape: three lanes, each on its own (volume, filter, pan), each moved along a named shape
enum("shape_on", "Shape", ON_OFF, "Off")
num("shape_depth", "Shape Depth", "lin", 0, 1, 1, "pct")
enum("shape_target", "Shape Target", TARGETS, "All")
num("shape_mix", "Shape Mix", "lin", 0, 1, 1, "pct")
for lane, name in LANES:
    enum(lane + "_on", name, ON_OFF, "Off")
for lane, name in LANES:
    enum(lane + "_shape", name + " Shape", SHAPE_NAMES, "Sine")
    popup_flag(lane + "_shape")
for lane, name in LANES:
    enum(lane + "_rate", name + " Rate", SHAPE_RATES, "1/4")
    popup_flag(lane + "_rate")
num("vol_depth", "Volume Depth", "lin", 0, 1, 0, "pct")              # the volume never boosts: 0..1
num("flt_depth", "Filter Depth", "lin", -1, 1, 0, "spct")            # the filter and the pan move both ways
num("pan_depth", "Pan Depth", "lin", -1, 1, 0, "spct")
for lane, name in LANES:
    num(lane + "_phase", name + " Phase", "lin", 0, 1, 0, "pct")
enum("flt_type", "Filter Type", ["LP", "BP", "HP"], "LP")
num("flt_cutoff", "Cutoff", "log", 20, 20000, 20000, "hz")
num("flt_reso", "Resonance", "lin", 0, 1, 0, "pct")


# --- Echo, Time and Moves: docs/DESIGN.md "Echo", "Time" and "Moves" ------------------------------------------
# Appended after Shape's (plan 2; params.lock keeps plan 1's 100 where they are). The option lists the DSP also has are
# named here, one list each, in the order of the DSP's own enum or table (named at each): plugin/groove_map.cpp
# static_asserts them against it, so a list that drifts stops the build instead of mapping one option to another. A
# project stores an option by its index: append, never reorder.
ECHO_DIVS = ["1/64", "1/32T", "1/32", "1/16T", "1/16", "1/8T", "1/16.", "1/8", "1/4T", "1/8.", "1/4", "5/16", "1/2T",
             "1/4.", "7/16", "1/2", "5/8", "1/2.", "1 bar"]               # kEchoDivs: by length, so a Q-Link walks them
ECHO_ORDERS = ["Echo First", "Echo Last"]                                   # Groove::Order: kEchoFirst, kEchoLast
ECHO_ROUTES = ["Parallel", "Serial", "Pairs", "Ring", "Spread"]             # Echo::Routing
ECHO_GLIDES = ["Tape", "Fade"]                                              # EchoLine::Glide
LINES = (("a", "1/8."), ("b", "1/4"), ("c", "5/16"), ("d", "1/2"))          # the four lines, and their divisions at rest
LINE_SYNCS = ["Sync", "Free"]                                               # a line's time: a division, or 1..2000 ms
LINE_MODES = ["Stereo", "Ping-Pong", "Mono"]                                # EchoLine::Mode
LINE_FILTERS = ["LP", "BP", "HP"]                                           # EchoLine::FilterType
LINE_PLACES = ["In Loop", "On Output"]                                      # EchoLine::Place
LINE_MODS = ["Off", "LFO", "Env", "Follow"]                                 # EchoMod::Mode
TIME_PLACES = ["Post", "Pre"]                                               # Groove::Place: Time after or before the parts
TIME_GROOVES = ["Off", "Swing", "Drunk", "Push"]                            # Time::Groove
MOVE_NAMES = ["Throw Beat", "Throw Bar", "Dub Freeze", "Half-Time Bar", "Tape Stop", "Slow Down", "Reverse Beat",
              "Spin-Back", "Stutter 1/8", "Stutter 1/16", "Roll Build 1 Bar", "Roll Build 4 Bars", "Filter Drop",
              "Gate 1/16", "Break", "Rewind"]                                # the 16 factory Moves, in tile order
MOVE_MODES = ["One Shot", "Loop"]                                           # Moves::Mode
MOVE_QUANTS = ["1/16", "Beat", "Bar"]                                       # Moves::Quant
MOVE_TILES = len(MOVE_NAMES)
TILE_TEXTS = {"move": MOVE_NAMES}       # the texts the plugin writes on a list's tiles, where they are known here

# the dry bus's level (Break and the Moves ramp it), and the whole Echo: switched on with its order (before or after
# Shape and Pump), a routing between the lines, and what the lines hear and give back
num("dry", "Dry Level", "lin", 0, 1, 1, "pct")
enum("echo_on", "Echo", ON_OFF, "Off")
enum("echo_order", "Echo Order", ECHO_ORDERS, "Echo First")
enum("echo_route", "Echo Routing", ECHO_ROUTES, "Parallel")
num("echo_send", "Echo Send", "lin", 0, 1, 1, "pct")
num("echo_fb", "Echo FB", "lin", -1, 1, 0, "spct")                   # an offset to all four lines' feedbacks
num("echo_tone", "Echo Tone", "lin", -1, 1, 0, "spct")               # an offset to their cutoffs, up to 4 octaves
num("echo_duck", "Echo Duck", "lin", 0, 1, 0, "pct")
enum("echo_freeze", "Echo Freeze", ON_OFF, "Off")
enum("echo_glide", "Echo Glide", ECHO_GLIDES, "Tape")
num("echo_width", "Echo Width", "lin", 0, 1.5, 1, "pct")
num("echo_level", "Echo Level", "lin", 0, 1, 1, "pct")

# the lines, A to D: each a stereo delay with its own filter, drive, wow and modulator. The long lists are popups.
for p, div in LINES:
    n = p.upper()
    enum(p + "_on", n + " On", ON_OFF, "Off")
    num(p + "_level", n + " Level", "lin", 0, 1, 0.8, "pct")
    num(p + "_pan", n + " Pan", "lin", -1, 1, 0, "spct")
    enum(p + "_sync", n + " Sync", LINE_SYNCS, "Sync")
    enum(p + "_div", n + " Division", ECHO_DIVS, div)
    popup_flag(p + "_div")
    num(p + "_ms", n + " Time", "log", 1, 2000, 375, "ms")
    num(p + "_fb", n + " Feedback", "lin", 0, 1, 0.4, "pct")
    enum(p + "_mode", n + " Mode", LINE_MODES, "Stereo")
    enum(p + "_ftype", n + " Filter Type", LINE_FILTERS, "LP")
    num(p + "_cut", n + " Cutoff", "log", 20, 20000, 20000, "hz")
    num(p + "_reso", n + " Resonance", "lin", 0, 1, 0, "pct")
    enum(p + "_place", n + " Flt Place", LINE_PLACES, "In Loop")
    num(p + "_drive", n + " Drive", "lin", 0, 1, 0, "pct")
    num(p + "_wow", n + " Wow", "lin", 0, 1, 0, "pct")
    enum(p + "_mod", n + " Modulator", LINE_MODS, "Off")
    enum(p + "_mshape", n + " Mod Shape", SHAPE_NAMES, "Sine")
    popup_flag(p + "_mshape")
    enum(p + "_mrate", n + " Mod Rate", SHAPE_RATES, "1/4")
    popup_flag(p + "_mrate")
    num(p + "_mcut", n + " Mod Cutoff", "lin", -1, 1, 0, "spct")     # how far the modulator moves the cutoff,
    num(p + "_mtime", n + " Mod Time", "lin", -1, 1, 0, "spct")      # the time (a small pitch wobble)
    num(p + "_mlevel", n + " Mod Level", "lin", 0, 1, 0, "pct")      # and the level

# Time: the always-recording ring behind the Groove and the Moves; and the Moves, one tap each on the grid. The
# Move parameter holds the SELECTED Move (R35): it stays where it is when the Move ends, and a preset or project
# saves it. It is automatable: a change of it fires the new Move at the quantize, and None stops a Move that plays.
# A tile tap selects its Move and fires it, GO fires the selected one again (a preset or project load fires nothing).
# The plugin names the 16 tiles and lights the ones whose Move plays or waits for its line.
enum("time_on", "Time", ON_OFF, "Off")
enum("time_place", "Time Place", TIME_PLACES, "Post")
enum("time_groove", "Time Groove", TIME_GROOVES, "Off")
num("time_amount", "Time Amount", "lin", 0, 1, 0.5, "pct")
enum("move", "Move", ["None"] + MOVE_NAMES, "None")
enum("move_mode", "Move Mode", MOVE_MODES, "One Shot")
enum("move_quant", "Move Quantize", MOVE_QUANTS, "Beat")
button("move_go", "Move Go")
for i in range(1, MOVE_TILES + 1):
    tile("move_%d" % i, "Move %d" % i)

# the Env modulators' detector (on the SETUP page)
num("echo_sens", "Echo Sens", "lin", 0, 1, 0.5, "pct")

PARAMS = {p["key"]: p for p in P}   # every parameter is declared: the layout and its checks look them up by key


def check_shapes(shapes):
    """A wrong entry fails the build, not the device: names unique and short; points ascending from 0 with y in
    0..1, a known seg and a tension in -1..1; steps 16 levels in 0..1; the pumps one per PUMP_CURVES entry, in its
    order, named after the curve; and SHAPE_COUNT entries."""
    errors, seen = [], set()
    if len(shapes) != SHAPE_COUNT:
        errors.append("%d shapes, the library is %d" % (len(shapes), SHAPE_COUNT))
    for sh in shapes:
        name = sh.get("name", "")
        where = "%r" % name
        if not name or len(name) > SHAPE_NAME_MAX:
            errors.append("%s: a name of 1..%d characters" % (where, SHAPE_NAME_MAX))
        if name.lower() in seen:
            errors.append("%s: the name is taken" % where)
        seen.add(name.lower())
        kind = sh.get("kind")
        if kind not in SHAPE_KINDS:
            errors.append("%s: kind %r is none of %s" % (where, kind, ", ".join(SHAPE_KINDS)))
        elif kind == "points":
            pts = sh["points"]
            if not pts or pts[0][0] != 0:
                errors.append("%s: the first point is at x = 0" % where)
            for i, (x, y, seg, tension) in enumerate(pts):
                if not 0 <= x < 1:
                    errors.append("%s: point %d: x = %s is outside [0, 1)" % (where, i, x))
                if i and x <= pts[i - 1][0]:
                    errors.append("%s: point %d: x = %s does not ascend" % (where, i, x))
                if not 0 <= y <= 1:
                    errors.append("%s: point %d: y = %s is outside 0..1" % (where, i, y))
                if seg not in SHAPE_SEGS:
                    errors.append("%s: point %d: seg %r is none of %s" % (where, i, seg, ", ".join(SHAPE_SEGS)))
                if not -1 <= tension <= 1:
                    errors.append("%s: point %d: tension = %s is outside -1..1" % (where, i, tension))
        elif kind == "steps":
            levels = sh["levels"]
            if len(levels) != SHAPE_STEPS:
                errors.append("%s: %d levels, a steps shape has %d" % (where, len(levels), SHAPE_STEPS))
            if not all(0 <= v <= 1 for v in levels):
                errors.append("%s: a level is outside 0..1" % where)
        elif kind == "pump":
            if sh.get("curve") not in PUMP_CURVES:
                errors.append("%s: the pump curve %r is none of %s" % (where, sh.get("curve"), ", ".join(PUMP_CURVES)))
            elif name != "Pump " + sh["curve"]:
                errors.append("%s: a Pump shape is named after its curve (Pump %s)" % (where, sh["curve"]))
    pumps = [sh.get("curve") for sh in shapes if sh.get("kind") == "pump"]
    if pumps != PUMP_CURVES:
        errors.append("the Pump shapes are one per curve, in the order of PUMP_CURVES (%s), not %s"
                      % (", ".join(PUMP_CURVES), ", ".join(str(c) for c in pumps)))
    if errors:
        raise SystemExit("shapes:\n  " + "\n  ".join(errors))


def shape_data_header(shapes):
    """build/shape_data.h: every points shape's breakpoints and every steps shape's 16 levels (as points held,
    x = i/16) in one array, and a row per shape saying where its points are."""
    lines, rows, total = [], [], 0
    for sh in shapes:
        first = count = curve = 0
        pts = []
        if sh["kind"] == "points":
            pts = sh["points"]
        elif sh["kind"] == "steps":
            pts = [(i / SHAPE_STEPS, v, "hold", 0.0) for i, v in enumerate(sh["levels"])]
        elif sh["kind"] == "pump":
            curve = PUMP_CURVES.index(sh["curve"])
        if pts:
            first, count = total, len(pts)
            total += count
            lines.append("    // %s" % sh["name"])
            lines += ["    {%sf, %sf, Seg::%s, %sf}," % (repr(x), repr(y), seg.capitalize(), repr(t))
                      for x, y, seg, t in pts]
        rows.append("    {%s, ShapeKind::%s, %d, %d, %d}" % (
            c_str(sh["name"]), "".join(w.capitalize() for w in sh["kind"].split("_")), first, count, curve))
    return """// generated by surface/surface.py from SHAPES: do not edit
#pragma once
#include "../../dsp/shapes.h"

namespace gf {

// Each points shape's breakpoints, each steps shape's 16 levels (held, x = i/16), in shape order.
inline constexpr ShapePoint kShapePoints[] = {
%s
};

// name, kind, the first point and how many (points, steps), the PumpCurve (pump).
inline constexpr ShapeDef kShapes[] = {
%s
};
inline constexpr int kNumShapes = %d;
static_assert(sizeof kShapes / sizeof kShapes[0] == kNumShapes, "a row per shape");

} // namespace gf
""" % ("\n".join(lines), ",\n".join(rows), len(shapes))


# --- the Time patterns: docs/DESIGN.md "Time" -----------------------------------------------------------
# Flex Beat's and Gross Beat's patterns for Time's playhead, held as data here and checked here; build/time_data.h
# is what dsp/time.cpp plays. A pattern has a length in beats and two curves over x, in beats from its start:
#   src  the source s(x): where the playhead reads, in beats from the same start. s(x) = x is now; less is behind
#        it (a lag of x - s(x) beats); a negative s is audio from before the pattern began. Time has no latency,
#        so s(x) <= x always: the playhead never reads what has not been played.
#   vol  the volume, 0..1.
# A curve is points (x, y, seg), x ascending from 0 to the pattern's length. A point's seg is the way to the next one:
#   line    a constant speed
#   smooth  a raised cosine, flat at both ends: the speed rises from 0 and falls back to 0
#   jump    vertical: the next point has the same x, and the curve jumps there. A source jump is crossfaded over 5 ms
#           that END at its x (a hit on the line is never heard early); a volume jump is an edge of 1 ms that ends there.
# The last point's seg is unused (line). A pattern is loopable (it goes round while it is asked for, and starts and ends on
# the live signal) or a one-shot: past its end the playhead is back at now with the volume at 1, so a one-shot ends at
# s = x (a last jump to now) or with the volume at 0 (the return hides in the silence).
# The Moves name the patterns; the engine counts them in this order, which the tests pin: append, don't reorder.
TIME_PATTERN_COUNT = 15
TIME_NAME_MAX = 18             # as a preset's
TIME_SEGS = ("line", "smooth", "jump")
TIME_MAX_LENGTH = 16.0         # beats
TIME_MAX_LAG = 8.0             # beats a pattern reaches back: the ring holds 2 bars at 80 BPM, 8 beats
TIME_MIN_GAP = 0.125           # beats between source jumps, and from the start and to the end: their 5 ms crossfades
                               # (0.083 beats at the engine's fastest tempo, 999 BPM) must not overlap
BAR = 4.0


def _live(length):
    return [(0.0, 0.0, "line"), (float(length), float(length), "line")]


def _flat(length, level=1.0):
    return [(0.0, float(level), "line"), (float(length), float(level), "line")]


def _rejoin(points):
    """The source ends in a jump to now: s = x at the end."""
    x = points[-1][0]
    return points[:-1] + [(x, points[-1][1], "jump"), (x, x, "line")]


def _repeats(phases, rejoin):
    """The source of a stutter or a roll. A phase (start, end, slice) plays the first slice of the phase live, then
    repeats it until `end`: each repeat jumps back to the phase's start. A phase begins on the live signal, so the one
    before it ends in a jump to now. A one-shot (rejoin) ends in one as well."""
    pts = []
    for n, (start, end, d) in enumerate(phases):
        count = round((end - start) / d)
        assert count >= 1 and start + count * d == end, "a phase is a whole number of slices"
        pts.append((start, start, "line"))
        for k in range(1, count):
            at = start + k * d
            pts += [(at, start + d, "jump"), (at, start, "line")]
        last = n == len(phases) - 1
        pts.append((end, start + d, "line" if last and not rejoin else "jump"))
    if rejoin:
        pts.append((phases[-1][1], phases[-1][1], "line"))
    return pts


def _reverse_repeats(length, d):
    """The source of a reverse stutter: the first slice (d beats) live, then it plays backwards over and over: each repeat
    jumps back to the slice's end and runs down to its start at speed -1."""
    count = round(length / d)
    assert count >= 2 and count * d == length, "a whole number of slices"
    pts = [(0.0, 0.0, "line"), (d, d, "line")]
    for k in range(2, count + 1):
        pts += [(k * d, 0.0, "jump" if k < count else "line")] + ([(k * d, d, "line")] if k < count else [])
    return pts


def _speed_ramp(length, v0, v1, steps):
    """The source of a playhead whose speed runs from v0 to v1 over `length` beats: s(x) = v0 x + (v1 - v0) x^2 / 2 length,
    as `steps` lines (the speed steps by (v1 - v0) / steps, and with it the pitch and, near the end, the level: a tape stop of 256
    steps falls 0.4% at a time)."""
    return [(length * k / steps, v0 * (length * k / steps) + (v1 - v0) * (length * k / steps) ** 2 / (2.0 * length), "line")
            for k in range(steps + 1)]


def _gate_vol(length, step, opened):
    """A gate: of each `step` beats the first `opened` (a fraction) at volume 1, the rest at 0, the edges jumps."""
    pts = []
    count = round(length / step)
    for k in range(count):
        a = k * step
        b = a + step * opened
        pts += [(a, 1.0, "line"), (b, 1.0, "jump"), (b, 0.0, "line"), (a + step, 0.0, "jump" if k + 1 < count else "line")]
    return pts


def _pattern(name, length, loopable, src, vol):
    return dict(name=name, length=float(length), loopable=loopable,
                src=[(float(x), float(y), seg) for x, y, seg in src], vol=[(float(x), float(y), seg) for x, y, seg in vol])


TIME_PATTERNS = [
    # stutters: the bar's first slice, over and over (the bar's own beginning is live)
    _pattern("Stutter 1/8", BAR, True, _repeats([(0.0, BAR, 0.5)], False), _flat(BAR)),
    _pattern("Stutter 1/16", BAR, True, _repeats([(0.0, BAR, 0.25)], False), _flat(BAR)),
    _pattern("Stutter 1/32", BAR, True, _repeats([(0.0, BAR, 0.125)], False), _flat(BAR)),
    _pattern("Reverse Stutter", BAR, True, _reverse_repeats(BAR, 0.5), _flat(BAR)),   # jumps into a segment that runs backwards
    # rolls: each phase repeats its own first slice, the slices shortening; back to now at the end
    _pattern("Roll Build 1 Bar", BAR, False,
             _repeats([(0.0, 2.0, 1.0), (2.0, 3.0, 0.5), (3.0, 3.5, 0.25), (3.5, 4.0, 0.125)], True), _flat(BAR)),
    _pattern("Roll Build 4 Bars", 4 * BAR, False,
             _repeats([(0.0, 4.0, 1.0), (4.0, 8.0, 0.5), (8.0, 12.0, 0.25), (12.0, 16.0, 0.125)], True), _flat(4 * BAR)),
    # speed
    _pattern("Half-Time Bar", BAR, False, _rejoin([(0.0, 0.0, "line"), (BAR, BAR / 2, "line")]), _flat(BAR)),
    _pattern("Tape Stop", BAR, False, _speed_ramp(BAR, 1.0, 0.0, 256),
             [(0.0, 1.0, "line"), (BAR - 0.5, 1.0, "line"), (BAR, 0.0, "line")]),   # silent at the end: the speed has fallen to 0
    _pattern("Slow Down", BAR, False, _rejoin(_speed_ramp(BAR, 1.0, 0.5, 64)), _flat(BAR)),
    # backwards
    _pattern("Reverse Beat", 1.0, False, _rejoin([(0.0, 0.0, "line"), (1.0, -1.0, "line")]), _flat(1.0)),
    # already backwards at the first sample, then faster (speed -1 to -3): the level follows the speed, so a speed that
    # starts at 0 would leave a hole of ~8 ms at the start. Ends in silence, 1 beat back.
    _pattern("Spin-Back", 0.5, False, _speed_ramp(0.5, -1.0, -3.0, 48),
             [(0.0, 1.0, "line"), (0.1, 1.0, "line"), (0.5, 0.0, "line")]),
    _pattern("Rewind", 1.0, False, _rejoin([(0.0, 0.0, "line"), (1.0, -2.0, "line")]), _flat(1.0)),
    # gates and the break: the playhead stays at now, the volume moves
    _pattern("Gate 1/16", BAR, True, _live(BAR), _gate_vol(BAR, 0.25, 0.6)),
    _pattern("Gate 1/8", BAR, True, _live(BAR), _gate_vol(BAR, 0.5, 0.6)),
    _pattern("Break Silence", BAR, False, _live(BAR), _flat(BAR, 0.0)),
]


def _curve_at(points, x):
    """A curve's value at x, as dsp/time.cpp reads it: right-continuous at a jump."""
    i = 0
    while i + 2 < len(points) and x >= points[i + 1][0]:
        i += 1
    (x0, y0, seg), (x1, y1, _) = points[i], points[i + 1]
    if x >= x1:
        return y1
    if x <= x0:
        return y0
    u = (x - x0) / (x1 - x0)
    return y0 + (y1 - y0) * (0.5 * (1.0 - math.cos(math.pi * u)) if seg == "smooth" else u)


def check_time_patterns(patterns):
    """A wrong entry fails the build, not the device. TIME_PATTERN_COUNT patterns; names unique and short. Each curve:
    points from x = 0 to the length, x ascending, equal x exactly across a jump (none at the start, no two in a row), a
    known seg, the last seg a line. The source never ahead of now (s(x) <= x, on both sides of every jump and along every
    smooth segment) and reaching back at most TIME_MAX_LAG beats; its jumps TIME_MIN_GAP apart. The volume within 0..1.
    A one-shot ends at s = x or with the volume at 0."""
    errors, seen = [], set()
    if len(patterns) != TIME_PATTERN_COUNT:
        errors.append("%d patterns, the library is %d" % (len(patterns), TIME_PATTERN_COUNT))
    for pt in patterns:
        name = pt.get("name", "")
        where = "%r" % name
        before = len(errors)
        if not name or len(name) > TIME_NAME_MAX:
            errors.append("%s: a name of 1..%d characters" % (where, TIME_NAME_MAX))
        if name.lower() in seen:
            errors.append("%s: the name is taken" % where)
        seen.add(name.lower())
        length = pt["length"]
        if not 0 < length <= TIME_MAX_LENGTH:
            errors.append("%s: the length %s is outside (0, %g] beats" % (where, length, TIME_MAX_LENGTH))
        for key, label in (("src", "source"), ("vol", "volume")):
            pts = pt[key]
            if len(pts) < 2 or pts[0][0] != 0 or pts[-1][0] != length:
                errors.append("%s: the %s curve runs from x = 0 to the length (%s)" % (where, label, length))
                continue
            if pts[-1][2] != "line":
                errors.append("%s: the %s curve's last point's seg is unused: line" % (where, label))
            for i, (x, y, seg) in enumerate(pts):
                if seg not in TIME_SEGS:
                    errors.append("%s: %s point %d: seg %r is none of %s" % (where, label, i, seg, ", ".join(TIME_SEGS)))
                if i and x < pts[i - 1][0]:
                    errors.append("%s: %s point %d: x = %s does not ascend" % (where, label, i, x))
                if i + 1 < len(pts):
                    vertical = pts[i + 1][0] == x
                    if vertical != (seg == "jump"):
                        errors.append("%s: %s point %d at x = %s: %s" % (where, label, i, x, (
                            "a jump needs the next point at the same x" if seg == "jump"
                            else "the next point is at the same x but the seg is not jump")))
                    if seg == "jump" and (x <= 0 or (i + 2 < len(pts) and pts[i + 2][0] == x)):
                        errors.append("%s: %s point %d: a jump at the start, or two in a row" % (where, label, i))
                if key == "vol" and not 0 <= y <= 1:
                    errors.append("%s: volume point %d: y = %s is outside 0..1" % (where, i, y))
        if len(errors) > before:
            continue   # the shape of the curves is wrong: the laws below would only repeat it
        src = pt["src"]
        places = [(x, y) for x, y, _ in src]
        for i in range(len(src) - 1):   # along a smooth segment s - x is not a line: look inside it
            if src[i][2] == "smooth":
                for k in range(65):
                    x = src[i][0] + (src[i + 1][0] - src[i][0]) * k / 64.0
                    places.append((x, _curve_at(src, x)))
        for x, y in places:
            if y > x + 1e-9:
                errors.append("%s: the source is ahead of now at x = %g (s = %g): Time has no latency" % (where, x, y))
                break
        reach = max(x - y for x, y in places)
        if reach > TIME_MAX_LAG:
            errors.append("%s: the source reaches back %g beats, the ring holds %g" % (where, reach, TIME_MAX_LAG))
        jumps = sorted(src[i][0] for i in range(len(src) - 1) if src[i][2] == "jump")
        edges = [0.0] + [x for x in jumps if x < length] + [length]
        if any(b - a < TIME_MIN_GAP for a, b in zip(edges, edges[1:])):
            errors.append("%s: jumps (and the ends) under %g beats apart: their 5 ms crossfades would overlap" % (where, TIME_MIN_GAP))
        if not pt["loopable"] and not (abs(src[-1][1] - length) < 1e-9 or pt["vol"][-1][1] == 0):
            errors.append("%s: a one-shot ends at s = x (rejoining now) or with the volume at 0" % where)
    if errors:
        raise SystemExit("time patterns:\n  " + "\n  ".join(errors))


def time_data_header(patterns):
    """build/time_data.h: every pattern's source and volume points in one array, and a row per pattern saying where they are."""
    lines, rows, total = [], [], 0
    for pt in patterns:
        first = {}
        for key, label in (("src", "source"), ("vol", "volume")):
            first[key] = total
            lines.append("    // %s, %s" % (pt["name"], label))
            lines += ["    {%sf, %sf, TSeg::%s}," % (repr(float(x)), repr(float(y)), seg.capitalize()) for x, y, seg in pt[key]]
            total += len(pt[key])
        rows.append("    {%s, %sf, %s, %d, %d, %d, %d}" % (
            c_str(pt["name"]), repr(float(pt["length"])), "true" if pt["loopable"] else "false",
            first["src"], len(pt["src"]), first["vol"], len(pt["vol"])))
    return """// generated by surface/surface.py from TIME_PATTERNS: do not edit
#pragma once
#include "../../dsp/time.h"

namespace gf {

// Every pattern's source curve, then its volume curve, in pattern order.
inline constexpr TPoint kTimePoints[] = {
%s
};

// name, length in beats, loopable, the source points (first, count), the volume points (first, count).
inline constexpr TimePatternDef kTimePatterns[] = {
%s
};
inline constexpr int kNumTimePatterns = %d;
static_assert(sizeof kTimePatterns / sizeof kTimePatterns[0] == kNumTimePatterns, "a row per pattern");

} // namespace gf
""" % ("\n".join(lines), ",\n".join(rows), len(patterns))


# --- the Moves: docs/DESIGN.md "Moves" ------------------------------------------------------------------
# Sixteen one-tap actions for the MOVES page's tiles, held as data here and checked here; build/move_data.h is what
# plugin/moves.cpp plays. A Move is a length in beats from its line on the grid, and
#   pattern  the Time pattern it plays (by name, or None); the Move is as long as the pattern
#   parts    the parts it switches on while it plays: "pump", "shape", "echo", "time" (the bits of MoveDef.partsOn)
#   ramps    up to four changes of parameters on top of the knobs, each a key, a kind, a value, a shape and a window:
#              add  a change of the knob's 0..1 value (a log parameter's travel is octaves of its range: _octaves), the
#                   result held within 0..1; "relative to where the knobs are"
#              set  an option held while the ramp is on (an enum parameter; the value is the option's name)
#              shape  how an add ramp moves over its window: rise 0 to 1, hold 1, fall 1 to 0
#              window  from and to, as fractions of the Move's length (the whole Move unless it says)
#            Outside its window and when the Move ends a ramp glides back to the knob's value over 10 ms.
# A Move names the parameters it moves by key and the parts it needs, and the build checks both. The tiles are in this
# order (MOVE_NAMES, R28): append, don't reorder.
MOVE_PARTS = ("pump", "shape", "echo", "time")      # kPartPump, kPartShape, kPartEcho, kPartTime
MOVE_PART_SWITCH = {"pump": "pump_on", "shape": "shape_on", "echo": "echo_on", "time": "time_on"}
MOVE_KINDS = ("add", "set")                         # kMoveAdd, kMoveSet
MOVE_SHAPES = ("rise", "hold", "fall")              # kMoveRise, kMoveHold, kMoveFall
MOVE_MAX_RAMPS = 4
MOVE_MAX_LENGTH = 16.0                              # beats
MOVE_SWEEP = 2.0                                    # octaves the Filter Drop's cutoff falls over its bar


def _add(key, amount, shape="hold", at=(0.0, 1.0)):
    return dict(key=key, kind="add", value=float(amount), shape=shape, frm=float(at[0]), to=float(at[1]))


def _set(key, option, at=(0.0, 1.0)):
    return dict(key=key, kind="set", value=option, shape="hold", frm=float(at[0]), to=float(at[1]))


def _real(key, change):
    """A linear parameter's travel for a change of `change` in its own units (R37): echo_fb +0.3 is +0.15 of its -1..1."""
    p = PARAMS[key]
    return change / (p["hi"] - p["lo"])


def _octaves(key, n):
    """A log parameter's travel for `n` octaves up (down if negative): its 0..1 value is log(v / lo) / log(hi / lo)."""
    return n * math.log(2.0) / math.log(PARAMS[key]["hi"] / PARAMS[key]["lo"])


def _move(name, pattern=None, length=None, parts=(), ramps=()):
    """A Move with a pattern is as long as it; one without says how long."""
    if pattern is not None:
        length = next((pt["length"] for pt in TIME_PATTERNS if pt["name"] == pattern), None)
    return dict(name=name, pattern=pattern, length=None if length is None else float(length), parts=tuple(parts), ramps=list(ramps))


def _throw(name, length):
    """The Echo's send open for the whole Move and its feedback up by 0.3 (in the offset's own -1..1 units): a beat or a bar goes in and rings."""
    return _move(name, length=length, parts=("echo",), ramps=[_add("echo_send", 1.0), _add("echo_fb", _real("echo_fb", 0.3))])


MOVES = [
    _throw("Throw Beat", 1.0),
    _throw("Throw Bar", BAR),
    # a beat goes into the Echo (the send open, the feedback up so the lines take it), then the lines are frozen for a bar
    _move("Dub Freeze", length=BAR + 1.0, parts=("echo",),
          ramps=[_add("echo_send", 1.0, at=(0.0, 0.2)), _add("echo_fb", _real("echo_fb", 0.3), at=(0.0, 0.2)), _set("echo_freeze", "On", at=(0.2, 1.0))]),
    _move("Half-Time Bar", pattern="Half-Time Bar", parts=("time",)),
    _move("Tape Stop", pattern="Tape Stop", parts=("time",)),
    _move("Slow Down", pattern="Slow Down", parts=("time",)),
    _move("Reverse Beat", pattern="Reverse Beat", parts=("time",)),
    _move("Spin-Back", pattern="Spin-Back", parts=("time",)),
    _move("Stutter 1/8", pattern="Stutter 1/8", parts=("time",)),
    _move("Stutter 1/16", pattern="Stutter 1/16", parts=("time",)),
    _move("Roll Build 1 Bar", pattern="Roll Build 1 Bar", parts=("time",)),
    _move("Roll Build 4 Bars", pattern="Roll Build 4 Bars", parts=("time",)),
    # the Shape's filter lane on, its cutoff falling two octaves over the bar (at the filter's own type and resonance)
    _move("Filter Drop", length=BAR, parts=("shape",),
          ramps=[_set("flt_on", "On"), _add("flt_cutoff", _octaves("flt_cutoff", -MOVE_SWEEP), "rise")]),
    _move("Gate 1/16", pattern="Gate 1/16", parts=("time",)),
    # the dry drops out for the bar; the first beat of it goes into the Echo, which rings on
    _move("Break", length=BAR, parts=("echo",), ramps=[_add("dry", -1.0), _add("echo_send", 1.0, at=(0.0, 0.25))]),
    _move("Rewind", pattern="Rewind", parts=("time",)),
]


def _part_of(key):
    """The part a parameter belongs to (None: the chain's own, the dry level and the mix among them)."""
    if key.startswith("pump_"):
        return "pump"
    if key.startswith(("shape_", "vol_", "flt_", "pan_")):
        return "shape"
    if key.startswith("echo_") or key[:2] in ("a_", "b_", "c_", "d_"):
        return "echo"
    if key.startswith("time_"):
        return "time"
    return None


def check_moves(moves, patterns):
    """A wrong entry fails the build, not the device. One Move per MOVE_NAMES entry, in its order. Each: a length in
    (0, MOVE_MAX_LENGTH] beats; a Time pattern that exists and is as long as the Move (and the "time" part with it, and
    only then); parts from MOVE_PARTS; a pattern or a ramp; at most MOVE_MAX_RAMPS ramps. Each ramp names a sound
    parameter (not a part's switch: the Move's parts do that) of a part the Move switches on or of none; an add ramp a
    number parameter and an amount in -1..1, a set ramp an enum parameter and one of its options; a known shape and a
    window 0 <= from < to <= 1."""
    errors = []
    if [mv["name"] for mv in moves] != MOVE_NAMES:
        errors.append("the Moves are %s, the tiles %s" % ([mv["name"] for mv in moves], MOVE_NAMES))
    lengths = {pt["name"]: pt["length"] for pt in patterns}
    switches = set(MOVE_PART_SWITCH.values())
    for mv in moves:
        where = "%r" % mv["name"]
        length, pattern = mv["length"], mv["pattern"]
        if pattern is not None and pattern not in lengths:
            errors.append("%s: no Time pattern named %r" % (where, pattern))
        elif pattern is not None and length is not None and abs(lengths[pattern] - length) > 1e-9:
            errors.append("%s: the Move is %g beats and its pattern %g" % (where, length, lengths[pattern]))
        if length is None or not 0 < length <= MOVE_MAX_LENGTH:
            errors.append("%s: the length %s is outside (0, %g] beats" % (where, length, MOVE_MAX_LENGTH))
        if pattern is None and not mv["ramps"]:
            errors.append("%s: neither a pattern nor a ramp" % where)
        if (pattern is None) == ("time" in mv["parts"]):
            errors.append("%s: the \"time\" part and a Time pattern go together" % where)
        for part in mv["parts"]:
            if part not in MOVE_PARTS:
                errors.append("%s: no part %r (%s)" % (where, part, ", ".join(MOVE_PARTS)))
        if len(set(mv["parts"])) != len(mv["parts"]):
            errors.append("%s: a part twice" % where)
        if len(mv["ramps"]) > MOVE_MAX_RAMPS:
            errors.append("%s: %d ramps, at most %d" % (where, len(mv["ramps"]), MOVE_MAX_RAMPS))
        for r in mv["ramps"]:
            key = r["key"]
            there = "%s: the ramp on %r" % (where, key)
            p = PARAMS.get(key)
            if p is None or p["kind"] != "synth":
                errors.append("%s is not a sound parameter" % there)
                continue
            if key in switches:
                errors.append("%s is a part's switch: name the part instead" % there)
            if _part_of(key) not in (None,) + tuple(mv["parts"]):
                errors.append("%s belongs to the %s, which the Move does not switch on" % (there, _part_of(key)))
            if r["kind"] not in MOVE_KINDS:
                errors.append("%s: kind %r is none of %s" % (there, r["kind"], ", ".join(MOVE_KINDS)))
            elif r["kind"] == "add":
                if p["curve"] == "enum" or not isinstance(r["value"], float) or not -1.0 <= r["value"] <= 1.0:
                    errors.append("%s: an add needs a number parameter and an amount in -1..1" % there)
            elif p["curve"] != "enum" or r["value"] not in p.get("options", []):
                errors.append("%s: a set needs an enum parameter and one of its options, not %r" % (there, r["value"]))
            if r["shape"] not in MOVE_SHAPES:
                errors.append("%s: shape %r is none of %s" % (there, r["shape"], ", ".join(MOVE_SHAPES)))
            if not 0.0 <= r["frm"] < r["to"] <= 1.0:
                errors.append("%s: the window %s..%s is not within 0..1" % (there, r["frm"], r["to"]))
    if len(moves) != MOVE_TILES:
        errors.append("%d Moves, the MOVES page has %d tiles" % (len(moves), MOVE_TILES))
    if errors:
        raise SystemExit("moves:\n  " + "\n  ".join(errors))


def move_data_header(moves, patterns):
    """build/move_data.h: every Move's ramps in one array, and a row per Move saying where they are. A set ramp's amount
    is its option's 0..1 value (index / (options - 1), as MPC stores an enum)."""
    rows, ramps, total = [], [], 0
    index = {pt["name"]: i for i, pt in enumerate(patterns)}
    for mv in moves:
        ramps.append("    // %s" % mv["name"])
        for r in mv["ramps"]:
            p = PARAMS[r["key"]]
            amount = r["value"] if r["kind"] == "add" else p["options"].index(r["value"]) / float(len(p["options"]) - 1)
            ramps.append("    {P_%s, kMove%s, %sf, kMove%s, %sf, %sf}," % (
                r["key"].upper(), r["kind"].capitalize(), repr(float(amount)), r["shape"].capitalize(), repr(r["frm"]), repr(r["to"])))
        parts = " | ".join("kPart%s" % q.capitalize() for q in MOVE_PARTS if q in mv["parts"]) or "0"
        rows.append("    {%s, %d, %sf, %d, %d, %s}" % (
            c_str(mv["name"]), -1 if mv["pattern"] is None else index[mv["pattern"]], repr(mv["length"]), total, len(mv["ramps"]), parts))
        total += len(mv["ramps"])
    return """// generated by surface/surface.py from MOVES: do not edit
#pragma once
#include "../../plugin/moves.h"
#include "param_ids.h"

namespace gf {

// Every Move's ramps, in Move order: parameter, kind, amount, shape, and the window as fractions of the Move's length.
inline constexpr MoveRamp kMoveRamps[] = {
%s
};

// name, Time pattern (an index into kTimePatterns, -1: none), length in beats, ramps (first, count), parts switched on.
inline constexpr MoveDef kMoves[] = {
%s
};
inline constexpr int kNumMoves = %d;
inline constexpr int kNumMoveRamps = %d;
static_assert(sizeof kMoves / sizeof kMoves[0] == kNumMoves, "a row per Move");
static_assert(sizeof kMoveRamps / sizeof kMoveRamps[0] == kNumMoveRamps, "a row per ramp");

} // namespace gf
""" % ("\n".join(ramps), ",\n".join(rows), len(moves), total)


def check_lock(keys):
    """params.lock: the parameter keys in index order, one per line, the file sd88me's gen_vst.py writes since
    2026-10-21 (their #488), so a later copy of it reads ours as its own. MPC projects, automation lanes and Q-Link
    maps store parameters by index: a locked key that moves (one inserted mid-list, a reorder) stops the build, a key
    removed from the end only warns (its index stays taken). GEN_VST_RELOCK=1 accepts a deliberate break.
    Returns the file's new text, or None when it is current."""
    path = os.path.join(HERE, "params.lock")
    if os.path.exists(path) and os.environ.get("GEN_VST_RELOCK") != "1":
        with open(path) as f:
            locked = f.read().split()
        for i, k in enumerate(locked):
            if i >= len(keys):
                print("warning: params.lock: %d locked key(s) from %r on are gone (removed from the end)"
                      % (len(locked) - i, k), file=sys.stderr)
                return None
            if keys[i] != k:
                raise SystemExit("params.lock: parameter %d was %r and is now %r. MPC projects, automation and Q-Link "
                                 "maps store parameters by index: append new ones at the end, never insert or reorder "
                                 "(GEN_VST_RELOCK=1 accepts the break)" % (i, k, keys[i]))
        if len(keys) == len(locked):
            return None
    return "".join(k + "\n" for k in keys)


def main():
    keys = [p["key"] for p in P]
    assert len(set(keys)) == len(keys), "duplicate parameter key"
    lock = check_lock(keys)
    assert P[0]["kind"] == "readout", "parameter 0 must stay a read-only readout"
    layout = pages()
    check_layout(layout, build_layout().groups)
    presets = factory_presets()   # everything checked before anything is written
    check_shapes(SHAPES)
    check_time_patterns(TIME_PATTERNS)
    check_moves(MOVES, TIME_PATTERNS)
    outputs = [
        ("params.json", json.dumps(params_json(), indent=1)),
        ("layout.conf", layout),
        ("vst.json", json.dumps(VST, indent=1)),
        (os.path.join("build", "skin_style.json"), json.dumps(skin_style(), indent=1)),
        (os.path.join("build", "factory_presets.h"), presets_header(presets)),
        (os.path.join("build", "shape_data.h"), shape_data_header(SHAPES)),
        (os.path.join("build", "time_data.h"), time_data_header(TIME_PATTERNS)),
        (os.path.join("build", "move_data.h"), move_data_header(MOVES, TIME_PATTERNS)),
        (os.path.join("build", "param_ids.h"), header()),   # last: make's target, newer than the rest
    ]
    if lock:
        outputs.insert(-1, ("params.lock", lock))
    os.makedirs(os.path.join(HERE, "build"), exist_ok=True)
    for name, text in outputs:
        path = os.path.join(HERE, name)
        with open(path + ".tmp", "w", newline="\n", encoding="utf-8") as f:
            f.write(text)
        os.replace(path + ".tmp", path)
    print("surface: %d parameters, layout ok, %d factory presets, %d shapes, %d time patterns, %d moves"
          % (len(P), len(presets), len(SHAPES), len(TIME_PATTERNS), len(MOVES)))


if __name__ == "__main__":
    sys.exit(main())
