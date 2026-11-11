"""Update.bat runs this with the installer's private Python: fetch the latest release of the
port from GitHub and copy it over this folder, keeping everything that is yours (the game folder
with its saves, settings and texture packs, your disc, the build folder with its tools and cache).
Update.bat then runs the installer, which only redoes the steps whose inputs changed.

Exit codes: 0 updated (build next), 3 already up to date, 1 failed."""
import json
import os
import shutil
import ssl
import sys
import tempfile
import urllib.request
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
API = "https://api.github.com/repos/Adolini1715/Concrete-Jungle-PC/releases/latest"
# never overwritten: the player's game folder (saves, pcj.ini, texture packs), their disc, the
# build folder (compiler, git, cache), the private Python, the offline decomp copy, logs
KEEP = {"PUT YOUR GAME HERE", "Predator Concrete Jungle PC", "_build", "pcj-decomp", "install-log.txt",
        "update-log.txt", "Update.bat"}
KEEP_INSTALLER = {"python"}


def say(m):
    print(m, flush=True)
    try:
        with open(os.path.join(REPO, "update-log.txt"), "a", encoding="utf-8") as f:
            f.write(m + "\n")
    except OSError:
        pass


def version_tuple(v):
    v = v.strip().lstrip("vV")
    out = []
    for p in v.split("."):
        n = "".join(c for c in p if c.isdigit())
        out.append(int(n) if n else 0)
    return tuple(out + [0] * (3 - len(out)))


def fetch(url, timeout=60):
    req = urllib.request.Request(url, headers={"User-Agent": "pcj-updater", "Accept": "application/vnd.github+json"})
    return urllib.request.urlopen(req, timeout=timeout, context=ssl.create_default_context())


CHECK = "--check" in sys.argv


def main():
    vf = os.path.join(REPO, "VERSION")
    have = open(vf).read().strip() if os.path.isfile(vf) else "0.1.0"   # v0.1 shipped without a VERSION file
    if not CHECK:
        say(f"Installed version: {have}")
    try:
        rel = json.load(fetch(API))
    except Exception as e:   # noqa: BLE001
        if CHECK:
            print(f"OFFLINE HAVE {have}", flush=True)
            return 1
        say(f"Could not reach GitHub ({e}). Check the internet connection and try again.")
        return 1
    tag = rel.get("tag_name", "")
    if CHECK:   # the launcher asks: print one line, change nothing
        print(f"LATEST {tag} HAVE {have} NEWER {int(version_tuple(tag) > version_tuple(have))}", flush=True)
        return 0
    say(f"Latest release: {tag}")
    if version_tuple(tag) <= version_tuple(have):
        say("You already have the latest version.")
        return 3
    url = rel.get("zipball_url")
    if not url:
        say("The release has no source download.")
        return 1
    tmp = tempfile.mkdtemp(prefix="pcj-update-")
    try:
        z = os.path.join(tmp, "src.zip")
        say(f"Downloading {tag} ...")
        with fetch(url, 300) as r, open(z, "wb") as f:
            shutil.copyfileobj(r, f)
        with zipfile.ZipFile(z) as zf:
            zf.extractall(os.path.join(tmp, "x"))
        tops = os.listdir(os.path.join(tmp, "x"))
        if len(tops) != 1:
            say("Unexpected download layout.")
            return 1
        new = os.path.join(tmp, "x", tops[0])
        if not os.path.isfile(os.path.join(new, "installer", "install.py")):
            say("The download does not look like the port's source.")
            return 1
        n = 0
        for d, dirs, files in os.walk(new):
            rel_d = os.path.relpath(d, new)
            parts = [] if rel_d == "." else rel_d.split(os.sep)
            if parts and parts[0] in KEEP:
                dirs[:] = []
                continue
            if len(parts) >= 2 and parts[0] == "installer" and parts[1] in KEEP_INSTALLER:
                dirs[:] = []
                continue
            os.makedirs(os.path.join(REPO, rel_d), exist_ok=True)
            for f in files:
                if not parts and f in KEEP:
                    continue
                shutil.copy2(os.path.join(d, f), os.path.join(REPO, rel_d, f))
                n += 1
        # Update.bat runs from a temporary copy, so the new one can be written now
        ub = os.path.join(new, "Update.bat")
        if os.path.isfile(ub):
            shutil.copy2(ub, os.path.join(REPO, "Update.bat"))
        say(f"Updated {n} files to {tag}. Rebuilding (only what changed) ...")
        return 0
    except Exception as e:   # noqa: BLE001
        say(f"Update failed: {e}")
        return 1
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
