"""The launcher runs this (with the installer's private Python) for Texture Packs > Available
to download.

  packs.py --list TEXTURES_DIR     one line per pack: PACK <id> <installed 0/1> <size> <name>|<about>
  packs.py --get ID TEXTURES_DIR   download into TEXTURES_DIR: PROGRESS <id> <bytes> <total> lines,
                                   then DONE <id> or FAIL <id> <reason>

The list is packs.json from the port's GitHub repo (so new packs show up without an update),
else the copy in this install folder. Downloads go to <file>.part, are checked against the
listed SHA-1 and only then renamed into place; links are tried in order.
"""
import hashlib
import json
import os
import ssl
import sys
import urllib.request

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MANIFEST = "https://raw.githubusercontent.com/Adolini1715/Concrete-Jungle-PC/main/packs.json"


def fetch(url, timeout=60):
    req = urllib.request.Request(url, headers={"User-Agent": "pcj-launcher"})
    return urllib.request.urlopen(req, timeout=timeout, context=ssl.create_default_context())


def manifest():
    try:
        return json.load(fetch(MANIFEST, 20))["packs"]
    except Exception:   # noqa: BLE001  offline or not published yet: the local copy
        try:
            with open(os.path.join(REPO, "packs.json"), encoding="utf-8") as f:
                return json.load(f)["packs"]
        except (OSError, ValueError, KeyError):
            return []


def installed(tex, p):
    return any(os.path.isfile(os.path.join(tex, d, p["file"])) for d in ("", "disabled"))


def clean(s):
    return str(s).replace("\n", " ").replace("|", "/")


def main():
    a = sys.argv[1:]
    if len(a) == 2 and a[0] == "--list":
        tex = a[1]
        for p in manifest():
            print(f"PACK {p['id']} {int(installed(tex, p))} {p.get('size', 0)} {clean(p['name'])}|{clean(p.get('about', ''))}",
                  flush=True)
        return 0
    if len(a) == 3 and a[0] == "--get":
        pid, tex = a[1], a[2]
        packs = {p["id"]: p for p in manifest()}
        if pid not in packs:
            print(f"FAIL {pid} not in the pack list", flush=True)
            return 1
        p = packs[pid]
        os.makedirs(tex, exist_ok=True)
        dst = os.path.join(tex, p["file"])
        part = dst + ".part"
        why = "no download link"
        for url in p.get("urls", []):
            try:
                h = hashlib.sha1()
                got, total = 0, int(p.get("size", 0))
                with fetch(url, 120) as r, open(part, "wb") as f:
                    total = int(r.headers.get("Content-Length") or total)
                    last = -1
                    while True:
                        b = r.read(1 << 18)
                        if not b:
                            break
                        f.write(b)
                        h.update(b)
                        got += len(b)
                        pct = got * 100 // total if total else 0
                        if pct != last:
                            print(f"PROGRESS {pid} {got} {total}", flush=True)
                            last = pct
                if p.get("sha1") and h.hexdigest() != p["sha1"]:
                    why = "the download did not match its checksum"
                    os.remove(part)
                    continue
                os.replace(part, dst)
                print(f"DONE {pid}", flush=True)
                return 0
            except Exception as e:   # noqa: BLE001  next link
                why = str(e) or e.__class__.__name__
                try:
                    os.remove(part)
                except OSError:
                    pass
        print(f"FAIL {pid} {clean(why)}", flush=True)
        return 1
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
