#!/usr/bin/env python3
"""Set up a PC-build replay of an xemu recording's controller input.

    python3 tools/xemu_input_to_replay.py <xemu session dir>

Prints the environment (or pcj.ini lines) that make the PC build replay the
session's input.csv: CJ_PAD_REPLAY=<input.csv> and CJ_PAD_REPLAY_ANCHOR=<t>,
where t is the first front-end state in the xemu recording (its first "menu"
event). The PC build starts its replay clock at its own first front-end state,
so both runs line up there; xemu's boot and the dashboard before it are skipped.
Also prints a short summary of what was pressed, per minute.
"""
import csv
import json
import os
import sys


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    d = sys.argv[1]
    ev = [json.loads(l) for l in open(os.path.join(d, "events.jsonl")) if l.strip()]
    menus = [e for e in ev if e["type"] == "menu"]
    if not menus:
        print("no front-end state in events.jsonl: the self-test probably failed, nothing to anchor to")
        return 1
    anchor = menus[0]["t"]
    rows = list(csv.DictReader(open(os.path.join(d, "input.csv"))))
    after = [r for r in rows if r["pad"] == "0" and float(r["t"]) >= anchor]
    path = os.path.abspath(os.path.join(d, "input.csv"))
    print(f"anchor: first front-end state at t={anchor:.3f} s ({menus[0].get('to')})")
    print(f"pad 0 rows after it: {len(after)} of {len(rows)}")
    print("\nLinux / Wine:")
    print(f"  CJ_PAD_REPLAY={path} CJ_PAD_REPLAY_ANCHOR={anchor:.3f}")
    print("\npcj.ini (Windows; use the Windows path of input.csv):")
    print("  CJ_PAD_REPLAY=<folder>\\input.csv")
    print(f"  CJ_PAD_REPLAY_ANCHOR={anchor:.3f}")
    names = {0x1000: "A", 0x2000: "B", 0x4000: "X", 0x8000: "Y", 0x0100: "LB/White", 0x0200: "RB/Black",
             0x0010: "Start", 0x0020: "Back", 0x0001: "Up", 0x0002: "Down", 0x0004: "Left", 0x0008: "Right"}
    per = {}
    last = 0
    for r in after:
        b = int(r["buttons"], 16)
        new = b & ~last
        last = b
        m = int((float(r["t"]) - anchor) // 60)
        for bit, n in names.items():
            if new & bit:
                per.setdefault(m, {}).setdefault(n, 0)
                per[m][n] += 1
    print("\npresses per minute after the anchor:")
    for m in sorted(per):
        print(f"  {m:3d}: " + ", ".join(f"{k} {v}" for k, v in sorted(per[m].items(), key=lambda kv: -kv[1])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
