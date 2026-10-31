#!/usr/bin/env python3
"""Compare a PC-build gameplay session with an xemu reference session.

    python3 tools/compare_sessions.py <pc session dir> <xemu session dir>

Both loggers write events.jsonl and frames.csv with the same field names. The
report lines the sessions up on their first level_start and prints:
  - event counts by type, side by side
  - the ordered state events (menu, level, mission, cutscene, control) with
    their times since level start, so missing / extra / reordered steps show
  - cutscene and level durations
  - player state ranges (health, energy, speed, position extent) per session
Different play obviously differs; what matters are sequences that cannot
differ for the same inputs (cutscene order and length, scripted events) and
values outside anything the reference shows.
"""
import csv
import json
import os
import sys
from collections import Counter

STATE = {"menu", "level_start", "level_end", "mission", "cutscene_start", "cutscene_end", "control_gained",
         "control_lost", "death"}


def load(d):
    ev = [json.loads(l) for l in open(os.path.join(d, "events.jsonl")) if l.strip()]
    rows = list(csv.DictReader(open(os.path.join(d, "frames.csv"))))
    return ev, rows


def t0(ev):
    for e in ev:
        if e["type"] == "level_start":
            return e["t"]
    return 0.0


def durations(ev, a, b):
    out, start = [], None
    for e in ev:
        if e["type"] == a: start = e["t"]
        elif e["type"] == b and start is not None: out.append(e["t"] - start); start = None
    return out


def ranges(rows):
    def col(k):
        v = []
        for r in rows:
            try:
                if r.get("player") == "1": v.append(float(r[k]))
            except (KeyError, ValueError):
                pass
        return v
    out = {}
    for k in ("health", "energy", "speed", "x", "y", "z"):
        v = col(k)
        if v: out[k] = (min(v), max(v))
    return out


def main():
    if len(sys.argv) != 3:
        print(__doc__); return 2
    (pe, pr), (xe, xr) = load(sys.argv[1]), load(sys.argv[2])
    pt, xt = t0(pe), t0(xe)
    print("# Session comparison\n")
    print(f"PC:   {sys.argv[1]}  ({len(pe)} events, {len(pr)} rows)")
    print(f"xemu: {sys.argv[2]}  ({len(xe)} events, {len(xr)} rows)\n")
    pc, xc = Counter(e["type"] for e in pe), Counter(e["type"] for e in xe)
    print("## Event counts\n\n| type | PC | xemu |\n|---|---|---|")
    for k in sorted(set(pc) | set(xc)):
        print(f"| {k} | {pc[k]} | {xc[k]} |")
    print("\n## State sequence (seconds from first level start)\n\n| # | PC | xemu |\n|---|---|---|")
    ps = [e for e in pe if e["type"] in STATE]
    xs = [e for e in xe if e["type"] in STATE]
    def lab(e, z):
        extra = e.get("to", e.get("mission", ""))
        return f"{e['t'] - z:+8.1f} {e['type']} {extra}"
    for i in range(max(len(ps), len(xs))):
        a = lab(ps[i], pt) if i < len(ps) else ""
        b = lab(xs[i], xt) if i < len(xs) else ""
        flag = "" if (i < len(ps) and i < len(xs) and ps[i]["type"] == xs[i]["type"]) else "  <--"
        print(f"| {i} | {a} | {b}{flag} |")
    print("\n## Durations\n")
    for name, a, b in (("cutscenes", "cutscene_start", "cutscene_end"), ("levels", "level_start", "level_end")):
        print(f"- {name}: PC {[round(x, 1) for x in durations(pe, a, b)]}  xemu {[round(x, 1) for x in durations(xe, a, b)]}")
    print("\n## Player state ranges\n")
    pr_, xr_ = ranges(pr), ranges(xr)
    for k in ("health", "energy", "speed", "x", "y", "z"):
        print(f"- {k}: PC {tuple(round(v, 2) for v in pr_.get(k, ()))}  xemu {tuple(round(v, 2) for v in xr_.get(k, ()))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
