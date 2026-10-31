#!/usr/bin/env python3
"""Turn a prof.txt from src/prof.c into per-thread function profiles.

    python3 tools/prof_report.py <prof.txt> [unstripped exe] [--top N]

The exe defaults to build-winclang/pcj.exe for a Windows profile (module pcj.exe)
and build/pcj for a Linux one. Its symbols must match the build that wrote the
profile: dist/symbols/pcj-<date>.exe keeps the unstripped copy of every
shipped build (tools/package_win.sh). Samples in other modules are grouped by
module; system modules where a thread blocks (ntdll, kernelbase, win32u,
libwinpthread, libc futex) count as waiting.
"""
import bisect
import os
import re
import subprocess
import sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WAIT_MODS = {"ntdll.dll", "kernelbase.dll", "kernel32.dll", "win32u.dll", "libwinpthread-1.dll", "libc.so.6",
             "linux-vdso.so.1"}


def load_syms(exe, base_off):
    nm = "x86_64-w64-mingw32-nm" if exe.endswith(".exe") else "nm"
    out = subprocess.run([nm, "-n", "-C", exe], capture_output=True, text=True).stdout
    syms = []
    for line in out.splitlines():
        p = line.split(None, 2)
        if len(p) == 3 and p[1] in "tTwW":
            syms.append((int(p[0], 16) - base_off, p[2]))
    syms.sort()
    return [a for a, _ in syms], [n for _, n in syms]


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    top = 20
    if "--top" in sys.argv:
        top = int(sys.argv[sys.argv.index("--top") + 1])
        args = [a for a in args if a != str(top)]
    if not args:
        print(__doc__)
        return 2
    mods, threads, raw, chains = {}, {}, [], []
    hdr = ""
    for line in open(args[0], errors="replace"):
        if line.startswith("# pcj"): hdr = line.strip()
        elif line.startswith("mod "):
            _, i, name, base, end = line.split()
            mods[int(i)] = name
        elif line.startswith("## thread"):
            m = re.match(r"## thread (\d+) \(tid (\d+), start (\S+)\): (\d+) samples", line)
            if m: threads[int(m.group(1))] = (m.group(2), m.group(3), int(m.group(4)))
        elif line.startswith("chain "):
            p = line.split()
            chains.append((int(p[1]), int(p[2]), [int(x, 16) for x in p[3:7] if x != "0"]))
        elif line.startswith("raw "):
            _, t, m, off, c = line.split()
            raw.append((int(t), int(m), int(off, 16), int(c)))
    win = any(n == "pcj.exe" for n in mods.values())
    exe = args[1] if len(args) > 1 else os.path.join(ROOT, "build-winclang/pcj.exe" if win else "build/pcj")
    main_mod = "pcj.exe" if win else "pcj"
    addrs, names = load_syms(exe, 0x140000000 if win else 0)

    def fn(off):
        i = bisect.bisect_right(addrs, off) - 1
        return names[i] if i >= 0 else "?"

    print(hdr)
    print(f"symbols: {exe}\n")
    per = defaultdict(lambda: defaultdict(int))
    for t, m, off, c in raw:
        name = mods.get(m, "(unknown)")
        key = fn(off) if name == main_mod else f"[{name}]"
        per[t][key] += c
    for t in sorted(per, key=lambda t: -threads.get(t, ("", "", 0))[2]):
        tid, start, n = threads.get(t, ("?", "?", sum(per[t].values())))
        if start.startswith(main_mod + "+0x") and start != main_mod + "+0x0":
            start = fn(int(start.split("+")[1], 16))
        wait = sum(c for k, c in per[t].items() if k.strip("[]") in WAIT_MODS)
        busy = 100.0 * (n - wait) / n if n else 0
        print(f"## thread {t} (tid {tid}, starts in {start}): {n} samples, {busy:.0f}% outside wait functions")
        for k, c in sorted(per[t].items(), key=lambda kv: -kv[1])[:top]:
            print(f"  {100.0 * c / n:5.1f}%  {k}")
        tc = sorted((c for c in chains if c[0] == t), key=lambda c: -c[1])
        if tc:
            print("  call chains in the exe (innermost first):")
            for _, c, offs in tc[:top // 2 or 1]:
                print(f"  {100.0 * c / n:5.1f}%  " + " <- ".join(fn(o) for o in offs))
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
