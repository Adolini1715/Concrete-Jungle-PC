"""The launcher runs this (with the installer's private Python) when a file or folder is dropped
on its window.

  mods.py GAME_FOLDER DROPPED_PATH [DROPPED_PATH ...]

Prints STEP <text>, PROGRESS <done> <total>, then DONE <summary> or FAIL <reason>.

What can be dropped:
  * a texture pack for this port (a zip with replace/<16 hex digits>_*.png): copied into textures
  * a PS2 texture mod made for PCSX2 (a folder, zip, 7z or rar; .dds or .png files named by the
    emulator's texture hash, like the "Definitive Edition" remaster on GameBanana): converted into
    packs for this port, switched on and off in the launcher like any other pack

PS2 textures are matched to the PC ones through ps2map.json (PCSX2 file name -> this port's texture
key, worked out once by comparing the two versions' textures; no game data in it). PS2 alpha runs
0..128, so it is doubled; each texture is resized to the Xbox shape; textures that are solid on the
Xbox stay solid, and where the PS2 cutouts were wrong the Xbox alpha is used, read from the
player's own disc (GAME_FOLDER/game). The remaster also gets a "remaster-xbox-colours" pack (off by
default) that pulls its colours back towards the Xbox ones.

Needs Pillow; the first run downloads the pinned wheel and installs it into this Python.
Personal use: the packs it makes are for the person who downloaded the mod. Don't share them.
"""
import hashlib
import io
import json
import os
import re
import shutil
import ssl
import struct
import subprocess
import sys
import tempfile
import urllib.request
import zipfile
from concurrent.futures import ProcessPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
PILLOW = ("https://files.pythonhosted.org/packages/8c/ce/e7dfc873bdd9828f3b6e5c2bbb74e47a98ec23cc5c74fc4e54462f0d9204/"
          "pillow-11.3.0-cp312-cp312-win_amd64.whl",
          "a6444696fce635783440b7f7a9fc24b3ad10a9ea3f0ab66c5905be1c19ccf17d", 6986324)
SPLIT = 1_600_000_000            # the game reads zips with a 32-bit offset: stay well under 2 GB
OFF_BY_DEFAULT = ("remaster-xbox-colours", "remaster-costume-")
README = """Made by the PC port's launcher from a PS2 texture mod: {name}.
PS2 alpha rescaled, shapes stretched back to the Xbox aspect, solid Xbox textures kept solid,
Xbox transparency used where the PS2 cutouts were wrong.
Personal use only. Do not share this file; point people to where you got the mod instead.
"""
NV = {2: 0x0C, 3: 0x0C, 4: 0x0E, 6: 0x06}


def say(*a):
    print(*a, flush=True)


# ---------------------------------------------------------------- Pillow
def need_pillow():
    try:
        import PIL  # noqa: F401
        return True
    except ImportError:
        pass
    if sys.platform != "win32":
        return False
    say("STEP Getting the image library (7 MB, once)")
    url, want, size = PILLOW
    d = tempfile.mkdtemp(prefix="pcj-pillow-")
    whl = os.path.join(d, url.rsplit("/", 1)[1])
    req = urllib.request.Request(url, headers={"User-Agent": "pcj-launcher"})
    h = hashlib.sha256()
    with urllib.request.urlopen(req, timeout=120, context=ssl.create_default_context()) as r, open(whl, "wb") as f:
        got = 0
        while True:
            b = r.read(1 << 18)
            if not b:
                break
            f.write(b)
            h.update(b)
            got += len(b)
            say("PROGRESS", got, size)
    if h.hexdigest() != want:
        return False
    subprocess.run([sys.executable, "-m", "pip", "install", "-q", "--no-index", "--no-deps",
                    "--no-warn-script-location", whl], capture_output=True)
    shutil.rmtree(d, ignore_errors=True)
    try:
        import PIL  # noqa: F401,F811
        return True
    except ImportError:
        return False


# ---------------------------------------------------------------- unpacking
def seven_zip():
    for p in (shutil.which("7z"), shutil.which("7za"),
              os.path.join(os.environ.get("ProgramFiles", r"C:\Program Files"), "7-Zip", "7z.exe"),
              os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"), "7-Zip", "7z.exe")):
        if p and os.path.isfile(p):
            return p
    return None


def rar_volumes(path):
    """All parts of a multi-part rar, in order: name.part1.rar, name.part2.rar ... (also part3_xxxx.rar,
    as GameBanana sometimes renames a part)."""
    d, base = os.path.split(path)
    m = re.match(r"(.*?part)0*\d+[^.]*\.rar$", base, re.I)
    if not m:
        return [path]
    vols = {}
    for f in os.listdir(d):
        n = re.match(re.escape(m.group(1)) + r"0*(\d+)[^.]*\.rar$", f, re.I)
        if n:
            vols[int(n.group(1))] = os.path.join(d, f)
    return [vols[k] for k in sorted(vols)]


def unpack(path, work):
    """Archive -> folder. zip with Python; 7z / rar with 7-Zip if it's installed, else Windows' tar."""
    low = path.lower()
    out = os.path.join(work, "x")
    os.makedirs(out, exist_ok=True)
    if low.endswith(".zip"):
        with zipfile.ZipFile(path) as z:
            names = [n for n in z.namelist() if n.lower().endswith((".dds", ".png", ".txt"))]
            for i, n in enumerate(names):
                dst = os.path.normpath(os.path.join(out, n))
                if not dst.startswith(out):
                    continue
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                with z.open(n) as s, open(dst, "wb") as f:
                    shutil.copyfileobj(s, f)
                if i % 50 == 0:
                    say("PROGRESS", i, len(names))
        return out
    first = path
    if low.endswith(".rar"):
        vols = rar_volumes(path)
        if len(vols) > 1:   # give the parts the names the unpacker expects
            stem = re.sub(r"part0*\d+[^.]*\.rar$", "", os.path.basename(vols[0]), flags=re.I)
            for i, v in enumerate(vols, 1):
                dst = os.path.join(work, f"{stem}part{i}.rar")
                say("STEP Gathering the parts of the archive", i, "of", len(vols))
                try:
                    os.link(v, dst)
                except OSError:
                    shutil.copyfile(v, dst)
            first = os.path.join(work, f"{stem}part1.rar")
    say("STEP Unpacking", os.path.basename(path))
    sz = seven_zip()
    if sz:
        r = subprocess.run([sz, "x", "-y", "-bso0", "-bsp0", "-o" + out, first, "*.dds", "*.png", "-r"],
                           capture_output=True)
        if r.returncode in (0, 1):
            return out
    tar = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "tar.exe")
    if os.path.isfile(tar):
        r = subprocess.run([tar, "-xf", first, "-C", out], capture_output=True)
        if r.returncode == 0:
            return out
    return None


# ---------------------------------------------------------------- the player's disc (Xbox alpha, colours)
def game_folder(out_dir):
    """the extracted disc: next to the launcher, else in the install folder (install-root.txt)"""
    cands = [os.path.join(out_dir, "game")]
    try:
        with open(os.path.join(out_dir, "install-root.txt"), encoding="utf-8", errors="replace") as f:
            root = f.readline().strip()
        cands += [os.path.join(root, d, "game") for d in sorted(os.listdir(root))]
    except OSError:
        pass
    cands.append(os.path.join(os.path.dirname(HERE), "Predator Concrete Jungle PC", "game"))
    for c in cands:
        if os.path.isfile(os.path.join(c, "Predator", "Binary", "_bin_xb", "BuildData", "Filelist.txt")):
            return c
    return out_dir


def builddata(game):
    for root, dirs, files in os.walk(game):
        if "Filelist.txt" in files:
            return root
    return None


def morton(w, h):
    xs, ys = [0] * w, [0] * h
    bit, bx, by = 0, 1, 1
    while bx < w or by < h:
        if bx < w:
            for x in range(w):
                if x & bx:
                    xs[x] |= 1 << bit
            bit += 1
            bx <<= 1
        if by < h:
            for y in range(h):
                if y & by:
                    ys[y] |= 1 << bit
            bit += 1
            by <<= 1
    return xs, ys


def xbox_images(game, entries):
    """key -> raw RGBA bytes of the Xbox texture, for the entries that need them"""
    from PIL import Image
    bd = builddata(game)
    if not bd:
        return {}
    where = {}
    for line in open(os.path.join(bd, "Filelist.txt"), errors="replace"):
        m = re.search(r"\\([^\\]+?\.edb)\s*: Len\s+(\d+).*Loc\s+([0-9a-f]+):(\d+)", line)
        if m:
            where.setdefault(m.group(1).strip().lower(), (int(m.group(2)), int(m.group(3), 16), m.group(4)))
    by_edb = {}
    for e in entries:
        by_edb.setdefault(e["xbox"][0].lower(), []).append(e)
    got = {}
    for edb, es in by_edb.items():
        if edb not in where:
            continue
        ln, off, part = where[edb]
        with open(os.path.join(bd, "Filelist." + part), "rb") as f:
            f.seek(off)
            D = f.read(ln)
        n = struct.unpack_from("<H", D, 0xb8)[0]
        base = 0xbc + struct.unpack_from("<I", D, 0xbc)[0]
        for e in es:
            _, i, fmt, w, h = e["xbox"]
            if i >= n:
                continue
            o = struct.unpack_from("<I", D, base + i * 0x1c + 8)[0]
            if o + 0x30 > len(D) or struct.unpack_from("<HH", D, o) != (w, h) or D[o + 0x13] != fmt:
                continue
            p = (o + 0x2c + struct.unpack_from("<I", D, o + 0x2c)[0]) & 0xFFFFFFFF
            size = {2: w * h // 2, 3: w * h // 2, 4: w * h, 6: w * h * 4}[fmt]
            raw = D[p:p + size]
            if len(raw) != size or "%016x" % texkey(raw, NV[fmt], w, h) != e["key"]:
                continue
            if fmt == 6:
                xs, ys = morton(w, h)
                px = bytearray(w * h * 4)
                for y in range(h):
                    for x in range(w):
                        s = (ys[y] | xs[x]) * 4
                        t = (y * w + x) * 4
                        px[t:t + 4] = (raw[s + 2], raw[s + 1], raw[s], raw[s + 3])
                img = Image.frombytes("RGBA", (w, h), bytes(px))
            else:
                img = Image.frombytes("RGBA", (w, h), raw, "bcn", 1 if fmt in (2, 3) else 2)
            got[e["key"]] = img.tobytes()
    return got


def texkey(d, fmt, w, h):
    M = 0xFFFFFFFFFFFFFFFF
    k = 0xCBF29CE484222325
    for v in (fmt, w, h):
        k = ((k ^ v) * 0x100000001B3) & M
    n = len(d) - len(d) % 8
    for v in struct.unpack_from("<%dQ" % (n // 8), d):
        k = ((k ^ v) * 0x100000001B3) & M
        k ^= k >> 29
    for b in d[n:]:
        k = ((k ^ b) * 0x100000001B3) & M
    return k or 1


# ---------------------------------------------------------------- one texture
def convert(job):
    """(path, entry, xbox RGBA or None) -> (key, png, colour-matched png or None)"""
    from PIL import Image, ImageStat
    path, e, xb = job
    w, h, flags = e["w"], e["h"], e.get("flags", "")
    im = Image.open(path)
    im.load()
    im = im.convert("RGBA")
    r, g, b, a = im.split()
    if a.getextrema()[1] <= 176:   # PS2 alpha: 128 is solid
        a = a.point([min(255, v * 255 // 128) for v in range(256)])
    im = Image.merge("RGBA", (r, g, b, a)).resize((w, h), Image.LANCZOS)
    X = Image.frombytes("RGBA", (e["xbox"][3], e["xbox"][4]), xb).resize((w, h), Image.BICUBIC) if xb else None
    if "o" in flags:
        im.putalpha(255)
    elif "a" in flags and X is not None:
        im.putalpha(X.getchannel("A"))
    out = io.BytesIO()
    im.save(out, "PNG", compress_level=6)
    alt = None
    if "c" in flags and X is not None:
        mask = im.getchannel("A").point([255 if v > 128 else 0 for v in range(256)])
        if sum(mask.histogram()[255:]) < 16:
            mask = None
        sm = ImageStat.Stat(im, mask)
        ss = sm.stddev
        xs = ImageStat.Stat(X, mask)
        bands = list(im.split())
        for c in range(3):
            k = min(1.4, max(0.7, (xs.stddev[c] + 1) / (ss[c] + 1)))
            mu, tm = sm.mean[c], xs.mean[c]
            bands[c] = bands[c].point([max(0, min(255, int((v - mu) * k + tm))) for v in range(256)])
        o2 = io.BytesIO()
        Image.merge("RGBA", bands).save(o2, "PNG", compress_level=6)
        alt = o2.getvalue()
    return e["key"], out.getvalue(), alt


# ---------------------------------------------------------------- packs
class Packs:
    """writes each group's zip, starting a new part before the 2 GB limit"""

    def __init__(self, tex, name):
        self.tex, self.name, self.open, self.parts = tex, name, {}, {}

    def add(self, group, key, png):
        z = self.open.get(group)
        if z and z[1] + len(png) > SPLIT:
            z[0].close()
            z = None
        if not z:
            n = len(self.parts.setdefault(group, [])) + 1
            tmp = os.path.join(self.tex, f".{group}-{n}.zip.tmp")
            zf = zipfile.ZipFile(tmp, "w", zipfile.ZIP_STORED)
            zf.writestr("README.txt", README.format(name=self.name))
            self.parts[group].append(tmp)
            z = self.open[group] = [zf, 0]
        z[0].writestr(f"replace/{key}_ps2.png", png)
        z[1] += len(png)

    def finish(self):
        for z in self.open.values():
            z[0].close()
        made = []
        for group, tmps in self.parts.items():
            for n, tmp in enumerate(tmps, 1):
                name = f"{group}-{n}.zip" if len(tmps) > 1 else f"{group}.zip"
                # replace an earlier conversion where it sits (on or off), else new packs start on,
                # apart from the alternates
                dst = None
                for d in ("", "disabled"):
                    if os.path.isfile(os.path.join(self.tex, d, name)):
                        dst = os.path.join(self.tex, d, name)
                if not dst:
                    off = group.startswith(OFF_BY_DEFAULT)
                    os.makedirs(os.path.join(self.tex, "disabled"), exist_ok=True)
                    dst = os.path.join(self.tex, "disabled" if off else "", name)
                os.replace(tmp, dst)
                made.append(name)
        return made


def slug(s):
    s = re.sub(r"\.(zip|rar|7z)$", "", os.path.basename(s.rstrip("\\/")), flags=re.I)
    s = re.sub(r"part0*\d+.*$", "", s, flags=re.I)
    s = re.sub(r"[^A-Za-z0-9]+", "-", s).strip("-").lower()
    return ("mod-" + s)[:48] or "mod"


def is_port_pack(path):
    try:
        with zipfile.ZipFile(path) as z:
            return any(re.match(r"(.*/)?replace/[0-9a-f]{16}_", n, re.I) for n in z.namelist())
    except (OSError, zipfile.BadZipFile):
        return False


def main():
    if len(sys.argv) < 3:
        say("FAIL usage: mods.py GAME_FOLDER PATH...")
        return 2
    out_dir, drops = sys.argv[1], sys.argv[2:]
    tex = os.path.join(out_dir, "textures")
    os.makedirs(tex, exist_ok=True)
    # a pack for this port: just copy it in
    rest = []
    copied = []
    for p in drops:
        if p.lower().endswith(".zip") and is_port_pack(p):
            say("STEP Adding", os.path.basename(p))
            shutil.copyfile(p, os.path.join(tex, os.path.basename(p)))
            copied.append(os.path.basename(p))
        else:
            rest.append(p)
    if not rest:
        say("DONE Added " + ", ".join(copied))
        return 0
    if len(rest) > 1 and all(p.lower().endswith(".rar") for p in rest):
        rest = [rar_volumes(rest[0])[0]]   # all the parts of one archive were dropped
    if not need_pillow():
        say("FAIL Couldn't get the image library. Check the internet connection and try again.")
        return 1
    with open(os.path.join(HERE, "ps2map.json"), encoding="utf-8") as f:
        M = json.load(f)["textures"]
    by_name = {os.path.basename(e["file"]).lower(): e for e in M}
    made, summary = [], []
    work = tempfile.mkdtemp(prefix="mod-work-", dir=out_dir)   # same drive as the dropped files, usually
    try:
        for p in rest:
            name = os.path.basename(p.rstrip("\\/"))
            src = p if os.path.isdir(p) else unpack(p, work)
            if not src:
                say("FAIL Couldn't unpack " + name + ". Install 7-Zip, or unpack it yourself and drop the folder.")
                return 1
            say("STEP Looking for PS2 textures in", name)
            found, remaster = {}, 0
            for root, dirs, files in os.walk(src):
                for f in files:
                    e = by_name.get(f.lower())
                    if e and f.lower().endswith((".dds", ".png")):
                        found[e["key"]] = (os.path.join(root, f), e)
                        tail = os.path.relpath(os.path.join(root, f), src).replace("\\", "/").lower()
                        remaster += tail.endswith(e["file"].lower())
            if not found:
                summary.append(f"{name}: no textures this converter knows (it knows the PS2 ones the "
                               "remaster replaces)")
                continue
            is_remaster = remaster >= 600
            group_of = (lambda e: e["group"]) if is_remaster else (lambda e, s=slug(name): s)
            need = [e for _, e in found.values() if "xbox" in e]
            say("STEP Reading the Xbox textures from your disc")
            xb = xbox_images(game_folder(out_dir), need) if need else {}
            packs = Packs(tex, name)
            jobs = [(path, e, xb.get(e["key"])) for path, e in found.values()]
            say("STEP Converting", len(jobs), "textures")
            done = bad = 0
            with ProcessPoolExecutor(max(1, (os.cpu_count() or 2) - 1)) as ex:
                for fut in [ex.submit(convert, j) for j in jobs]:
                    try:
                        key, png, alt = fut.result()
                    except Exception:   # noqa: BLE001  a broken file in the mod: skip it
                        bad += 1
                        continue
                    e = found[key][1]
                    packs.add(group_of(e), key, png)
                    if alt and is_remaster:
                        packs.add("remaster-xbox-colours", key, alt)
                    done += 1
                    if done % 10 == 0:
                        say("PROGRESS", done, len(jobs))
            made += packs.finish()
            summary.append(f"{name}: {done} textures" + (f", {bad} unreadable" if bad else ""))
    finally:
        shutil.rmtree(work, ignore_errors=True)
    if not made and not copied:
        say("FAIL " + "; ".join(summary))
        return 1
    say("DONE " + "; ".join(summary + ([f"added {', '.join(copied)}"] if copied else [])) +
        f". New packs: {', '.join(made)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
