#!/usr/bin/env python3
"""The probe's parameter list and its two touchscreen pages: the single source for
params.json, layout.conf and vst.json (the skin generator's inputs) and build/param_ids.h (the
C++ side). Checks names and keys before writing anything.

    python3 surface/surface.py
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# A separate name and ID from the effect that follows: the probe uninstalls on its own and never
# shares saved state with it.
VST = {"name": "GrooveWerk Probe", "vendor": "Devko", "uid": "GfPb", "version": 1000,
       "so": "groovewerk_probe.so", "params": "params.json", "layout": "layout.conf", "effect": True}

# --- parameters ------------------------------------------------------------------------------
# kind: readout (text the plugin writes, read only), num (real value lo..hi, linear), enum (options),
# button (momentary: reads back 0, every write is logged), tile (a list tile, lit = 1: the plugin keeps
# what MPC wrote and logs every write). The button and the tile are the repo's surface/surface.py kinds.
# fmt: how the plugin prints a num (plugin/parameters.cpp): db, pct
P = []


def readout(key, name):
    P.append(dict(key=key, name=name, kind="readout", lo=0, hi=0, default=0, fmt="none"))


def num(key, name, lo, hi, default, fmt):
    P.append(dict(key=key, name=name, kind="num", lo=lo, hi=hi, default=default, fmt=fmt))


def enum(key, name, options, default):
    P.append(dict(key=key, name=name, kind="enum", lo=0, hi=len(options) - 1, default=options.index(default),
                  fmt="enum", options=options))


def button(key, name):
    P.append(dict(key=key, name=name, kind="button", lo=0, hi=1, default=0, fmt="none"))


def tile(key, name):
    # A layout `list` of one tile reads <key>_1 (shadow_skin list_keys), so the key ends in _1.
    P.append(dict(key=key, name=name, kind="tile", lo=0, hi=1, default=0, fmt="enum", options=["Tile", "Tile lit"]))


# dsp/delay.h kDivisions, in beats
DIVISIONS = [("1/16", 0.25), ("1/8T", 1 / 3), ("1/8", 0.5), ("1/8.", 0.75), ("1/4T", 2 / 3), ("1/4", 1.0),
             ("1/4.", 1.5), ("1/2", 2.0), ("1 bar", 4.0)]

readout("status", "Status")      # index 0 must stay a read-only readout: MPC sets it at load
readout("host", "Host")
readout("timing", "Timing")
readout("events", "Events")
num("gain", "Gain", -24.0, 24.0, 0.0, "db")
enum("div", "Delay Time", [d[0] for d in DIVISIONS], "1/8")
num("fb", "Feedback", 0.0, 0.95, 0.4, "pct")
num("mix", "Mix", 0.0, 1.0, 0.3, "pct")
button("hold", "Hold")
tile("tile_1", "Tile")


def norm(p):
    """The default as MPC's 0..1 value."""
    return (p["default"] - p["lo"]) / (p["hi"] - p["lo"]) if p["hi"] > p["lo"] else 0.0


def params_json():
    out = []
    for p in P:
        e = {"key": p["key"], "name": p["name"]}
        if p["kind"] == "readout":
            e.update(min=0, max=0, display="string", type="readout")
        elif p["kind"] == "button":
            e.update(min=0, max=1, momentary=True, type="trigger")
        elif p["kind"] == "tile":
            e.update(options=p["options"], default=p["options"][0], display="string")
        elif p["kind"] == "enum":
            e.update(options=p["options"], default=p["options"][p["default"]])
        else:
            e.update(min=0, max=1, default=round(norm(p), 6), display="string")
        out.append(e)
    return {"name": VST["name"], "params": out}


# --- the page --------------------------------------------------------------------------------
# PolyWerk's look (style=td3 rounded cards on one flat colour) in GrooveWerk's coral. Plugin area
# 1280x628 at y = 86..714: a header row (the status line), then cards at y=158 and y=440 (h=270).
# Nothing sits in a card's title band (y .. y+44). Page 1 is the delay and what MPC does; page 2 the
# button and the tile, the two controls whose presses and releases the log times.
# The fonts are probe/surface/fonts (copies of the repo's): the probe builds on its own.
THEME = """style=td3
font_label=fonts/TitilliumWeb-SemiBold.ttf
title_size=17
theme_bg=15171c
theme_box=15171c
theme_line=2d3038
theme_ink=e9e9f0
theme_ink_dim=9a9aa8
theme_ink_faint=282a31
theme_accent=f07a5f
theme_accent_hi=ffb7a6
theme_lcd=0c0d10
theme_seg_inactive=212329
theme_seg_active=f07a5f
theme_seg_active_tx=2b0f0a
theme_tile_on=3b2420
theme_btn_bg=2b2d34
theme_title=aaa8b8
theme_knob_face=26282e
theme_knob_ring=3c3f47
theme_knob_dot=f07a5f
"""

LAYOUT = THEME + """
[tab PROBE]
readout cx=640 cy=121 w=1232 h=40 key=status
frame x=24 y=158 w=1232 h=270 title="TEMPO DELAY"
knob cx=170 cy=284 r=30 label="Gain" key=gain
enum_h cx=500 cy=270 sw=96 rows=3 label="DELAY TIME" key=div
knob cx=830 cy=284 r=30 label="Feedback" key=fb
knob cx=1090 cy=284 r=30 label="Mix" key=mix
frame x=24 y=440 w=1232 h=270 title="WHAT MPC DOES"
readout cx=640 cy=522 w=1184 h=40 key=host
readout cx=640 cy=584 w=1184 h=40 key=timing
readout cx=640 cy=646 w=1184 h=40 key=events
qlinks "PROBE" = gain,div,fb,mix

[tab PRESS]
readout cx=640 cy=121 w=1232 h=40 key=status
frame x=24 y=158 w=1232 h=270 title="BUTTON AND TILE"
button cx=330 cy=300 label="HOLD" key=hold
list x=700 y=260 w=360 cols=1 rows=1 th=80 gap=8 key=tile
qlinks "PRESS" = gain,div,fb,mix
"""

NAME_MAX = 13   # MPC shows a knob's effGetParamName at ~19.5 px in a 130 px box


def check():
    errors = []
    keys = [p["key"] for p in P]
    if len(set(keys)) != len(keys):
        errors.append("duplicate keys")
    names = [p["name"] for p in P]
    if len(set(names)) != len(names):
        errors.append("duplicate names (MPC tells parameters apart by name in the Q-Link overlay)")
    for p in P:
        if len(p["name"]) > NAME_MAX:
            errors.append("%s: name %r longer than %d" % (p["key"], p["name"], NAME_MAX))
    if P[0]["kind"] != "readout":
        errors.append("parameter 0 must be a readout")
    for line in LAYOUT.splitlines():
        toks = line.split()
        for tok in toks:
            if not tok.startswith("key="):
                continue
            # a `list` reads <key>_1 .. <key>_<cols * rows>
            need = [tok[4:]]
            if toks[0] == "list":
                f = dict(t.split("=", 1) for t in toks[1:] if "=" in t)
                need = ["%s_%d" % (tok[4:], i + 1) for i in range(int(f["cols"]) * int(f["rows"]))]
            for k in need:
                if k not in keys:
                    errors.append("layout: unknown key %s" % k)
    return errors


# --- C++ -------------------------------------------------------------------------------------
KIND = {"readout": "Readout", "num": "Num", "enum": "Enum", "button": "Button", "tile": "Tile"}
FMT = {"none": "None", "db": "Db", "pct": "Percent", "enum": "Enum"}


def c_str(s):
    return '"' + str(s).replace("\\", "\\\\").replace('"', '\\"') + '"'


def header():
    uid = VST["uid"]
    out = ["// generated by surface/surface.py: do not edit", "#pragma once", "#include <cstdint>", "",
           "namespace ef {", "", "enum ParamId : int {"]
    out += ["    P_%s," % p["key"].upper() for p in P]
    out += ["    P_COUNT", "};", "",
            "enum class Kind { Readout, Num, Enum, Button, Tile };",
            "enum class Fmt { None, Db, Percent, Enum };", "",
            "struct ParamInfo {",
            "    const char* key;",
            "    const char* name;",
            "    Kind kind;",
            "    Fmt fmt;",
            "    float lo, hi, def;   // real values; an enum's are option indices",
            "    int nOpts;",
            "    const char* const* opts;",
            "};", ""]
    for i, p in enumerate(P):
        if "options" in p:
            out.append("inline constexpr const char* OPTS_%d[] = {%s};" % (i, ", ".join(c_str(o) for o in p["options"])))
    out.append("")
    out.append("inline constexpr ParamInfo PARAM_INFO[P_COUNT] = {")
    for i, p in enumerate(P):
        opts = ("%d, OPTS_%d" % (len(p["options"]), i)) if "options" in p else "0, nullptr"
        out.append("    {%s, %s, Kind::%s, Fmt::%s, %rf, %rf, %rf, %s}," % (
            c_str(p["key"]), c_str(p["name"]), KIND[p["kind"]], FMT[p["fmt"]], float(p["lo"]), float(p["hi"]),
            float(p["default"]), opts))
    out.append("};")
    out.append("")
    out.append("inline constexpr double kDivisionBeats[] = {%s};" % ", ".join(repr(d[1]) for d in DIVISIONS))
    out.append("")
    out.append("inline constexpr char kPlugName[] = %s;" % c_str(VST["name"]))
    out.append("inline constexpr char kPlugVendor[] = %s;" % c_str(VST["vendor"]))
    out.append("inline constexpr int32_t kPlugUid = 0x%08x;   // '%s'" % (int.from_bytes(uid.encode(), "big"), uid))
    out.append("inline constexpr int32_t kPlugVersion = %d;" % VST["version"])
    out += ["", "} // namespace ef", ""]
    return "\n".join(out)


def write(path, text):
    """Only when it changed: make then rebuilds only what depends on it."""
    try:
        if open(path, encoding="utf-8").read() == text:
            return
    except OSError:
        pass
    open(path, "w", encoding="utf-8", newline="\n").write(text)


def main():
    errors = check()
    if errors:
        for e in errors:
            print("surface.py: " + e, file=sys.stderr)
        sys.exit(1)
    os.makedirs(os.path.join(HERE, "build"), exist_ok=True)
    write(os.path.join(HERE, "params.json"), json.dumps(params_json(), indent=1) + "\n")
    write(os.path.join(HERE, "layout.conf"), LAYOUT)
    write(os.path.join(HERE, "vst.json"), json.dumps(VST, indent=1) + "\n")
    write(os.path.join(HERE, "build", "param_ids.h"), header())
    print("surface.py: %d parameters" % len(P))


if __name__ == "__main__":
    main()
