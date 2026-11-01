#!/usr/bin/env python3
"""Full recompiler pipeline from a clean checkout, on Linux or Windows.

The same steps as tools/regen_all.sh, run with the Python that runs this script,
so the Windows installer (installer/install.py) needs no shell. Writes
src/recomp/gen from game/default.xbe.

  python tools/regen_all.py [--hybrid DIR] [--jobs N]

--hybrid DIR uses an already generated hybridize output (pregen/hybrid) instead
of running tools/hybridize, which needs clang and libclang.
"""
import argparse
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PY = sys.executable


def step(name, cmd, cwd):
    t = time.time()
    print(f"[regen] {name}", flush=True)
    r = subprocess.run(cmd, cwd=cwd)
    if r.returncode:
        sys.exit(f"[regen] {name} failed (exit {r.returncode})")
    print(f"[regen] {name}: {time.time() - t:.0f} s", flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hybrid", help="pregenerated hybridize output")
    ap.add_argument("--decomp", default=os.environ.get("PCJ_DECOMP", os.path.join(ROOT, "pcj-decomp")))
    ap.add_argument("--jobs", type=int, default=0, help="disassembly worker processes (0: all cores)")
    a = ap.parse_args()
    decomp = os.path.abspath(a.decomp)
    if not os.path.isfile(os.path.join(decomp, "symbols", "functions.csv")):
        sys.exit(f"[regen] no decompilation at {decomp}")
    os.chdir(ROOT)
    os.makedirs("build", exist_ok=True)
    hy = a.hybrid
    if not hy:
        hy = os.path.join("build", "hybrid")
        targets = []
        for line in open(os.path.join("tools", "hybridize", "targets.txt")):
            if line.strip() and not line.startswith("#"):
                targets += line.split()
        env = dict(os.environ, PCJ_DECOMP=decomp)
        print("[regen] hybridize", flush=True)
        r = subprocess.run([PY, "tools/hybridize/hybridize.py", "gen", *targets, "-o", hy], env=env)
        if r.returncode:
            sys.exit("[regen] hybridize failed")
    # the recompiler leaves the functions these files define to them
    with open(os.path.join("build", "exclude_manual.c"), "wb") as out:
        for f in [os.path.join(hy, "hybrid_swap.c"), "src/det_file.c", "src/femouse.c",
                  "src/native_gfx.c", "src/native_dsound.c", "src/mods_archive.c"]:
            with open(f, "rb") as src:
                shutil.copyfileobj(src, out)

    xr = os.path.join(ROOT, "xboxrecomp")
    xbe = os.path.join("..", "game", "default.xbe")
    work = os.path.join("..", "work")
    step("xbe_parser", [PY, "-m", "tools.xbe_parser", xbe, "--json", "../game/default_analysis.json"], xr)
    if not os.path.isfile(os.path.join(ROOT, "work", "disasm", "functions.json")):
        cmd = [PY, "-m", "tools.disasm", xbe, "-v", "--seed-functions", "../game/map_seeds.json",
               "-o", "../work/disasm"]
        if a.jobs != 1 and os.path.isfile(os.path.join(xr, "tools", "disasm", "parallel.py")):
            cmd += ["--jobs", str(a.jobs)]
        step("disasm", cmd, xr)
    step("merge_map", [PY, "../tools/merge_map.py", "../work/disasm/functions.json",
                       os.path.join(decomp, "symbols", "functions.csv")], xr)
    if not os.path.isdir(os.path.join(ROOT, "work", "func_id")):
        step("func_id", [PY, "-m", "tools.func_id", xbe, "--functions", "../work/disasm/functions.json",
                         "--strings", "../work/disasm/strings.json", "--xrefs", "../work/disasm/xrefs.json",
                         "-o", "../work/func_id"], xr)
    if not os.path.isdir(os.path.join(ROOT, "work", "abi")):
        step("abi_analysis", [PY, "-m", "tools.abi_analysis", xbe, "--disasm-dir", "../work/disasm",
                              "--func-id-dir", "../work/func_id", "--output-dir", "../work/abi"], xr)
    shutil.rmtree(os.path.join(ROOT, "src", "recomp", "gen"), ignore_errors=True)
    step("recomp", [PY, "-m", "tools.recomp", xbe, "--all", "--split", "250",
                    "--game-name", "Predator: Concrete Jungle", "--gen-dir", "../src/recomp/gen",
                    "--disasm-dir", "../work/disasm", "--func-id-dir", "../work/func_id",
                    "--abi-dir", "../work/abi", "-o", "../work/recomp",
                    "--manual-functions", "../game/manual_functions.json",
                    "--exclude-manual", "../build/exclude_manual.c"], xr)
    print("REGEN OK", flush=True)


if __name__ == "__main__":
    main()
