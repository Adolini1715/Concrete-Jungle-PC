"""Predator: Concrete Jungle PC: build the game from your own disc, on Windows.

Install.bat runs this with a private copy of Python (installer/bootstrap.ps1).
Every download is pinned by SHA-256 (or by git commit) and checked before it is
used. Nothing from the game is downloaded: the recompiler reads your
default.xbe and the game reads your disc files.

Steps (each is skipped when a previous run finished it):
  1. find the game in "PUT YOUR GAME HERE" (an .iso, or the extracted folder)
  2. download the build tools (GCC with CMake and Ninja, MinGit, SDL2, libepoxy)
  3. fetch the recompiler toolkit and the decompilation
  4. recompile default.xbe into C (the long step)
  5. compile pcj.exe
  6. put it all in "Predator Concrete Jungle PC" with a desktop shortcut
"""
import hashlib
import os
import shutil
import subprocess
import sys
import tarfile
import time
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
GAME_IN = os.path.join(REPO, "PUT YOUR GAME HERE")
OUT = os.path.join(REPO, "Predator Concrete Jungle PC")

XBE_SHA1S = {"7fbc1249c5948d638a62101cc2d23d6637b76284",   # USA retail, as on the disc
             "30a1e430892c9e472122a3a25d90a233e42b99cd"}   # the same with 3 certificate bytes changed
XR_URL = "https://github.com/sp00nznet/xboxrecomp"
XR_REV = "6376eb829e035d23c1c65377d8523eceb068ee1d"         # v0.15.0
DECOMP_URL = os.environ.get("PCJ_DECOMP_URL", "https://github.com/Adolini1715/Concrete-Jungle-Decomp")
DECOMP_REV = os.environ.get("PCJ_DECOMP_REV", "8b5077a462900e7bc31f54a494aded252667bfeb")   # v0.1
EPOXY_URL = "https://github.com/anholt/libepoxy"
EPOXY_REV = "c84bc9459357a40e46e2fec0408d04fbdde2c973"      # 1.5.10
SDL_VER = "2.30.9"

DOWNLOADS = {
    # name: (url, sha256, size)
    "gcc": ("https://github.com/brechtsanders/winlibs_mingw/releases/download/13.2.0posix-17.0.6-11.0.1-msvcrt-r5/"
            "winlibs-x86_64-posix-seh-gcc-13.2.0-mingw-w64msvcrt-11.0.1-r5.zip",
            "675f79b6fba8d3c8165bd9b5fb8e96ab73370dd8611f13e520cb185357d3dafc", 222828150),
    "git": ("https://github.com/git-for-windows/git/releases/download/v2.47.0.windows.2/MinGit-2.47.0.2-64-bit.zip",
            "c4a5d3a2adda98b25fe59349733fca56b3843360b962dba5535282a9d8120b31", 47222137),
    "sdl2": (f"https://github.com/libsdl-org/SDL/releases/download/release-{SDL_VER}/SDL2-devel-{SDL_VER}-mingw.tar.gz",
             "b188d165dc4a0372042bff3d0c18e9bb90772581e92a7b8b47b521b47714f75c", None),
    "capstone": ("https://files.pythonhosted.org/packages/py3/c/capstone/capstone-5.0.9-py3-none-win_amd64.whl",
                 "732cedbbb56d42e723f14d7af6387f1454194a820b4b96b56d1e53f865ef85d0", None),
    "meson": ("https://files.pythonhosted.org/packages/py3/m/meson/meson-1.12.1-py3-none-any.whl",
              "930bc7542cbd9f57009e182fd014eba48cf1a0180a7b9006d2d32d8f168d8b02", None),
}

LOG = None


def say(msg=""):
    print(msg, flush=True)
    if LOG:
        LOG.write(msg + "\n")
        LOG.flush()


def fail(msg):
    say("")
    say("PROBLEM: " + msg)
    say("Nothing was changed outside this folder. Fix the problem and run Install.bat again;")
    say("it carries on where it stopped. The full log is install-log.txt.")
    sys.exit(1)


def long_path(p):
    """Windows paths past 260 characters need the \\\\?\\ prefix."""
    p = os.path.abspath(p)
    return "\\\\?\\" + p if os.name == "nt" and not p.startswith("\\\\?\\") else p


def run(cmd, cwd=None, env=None, quiet=False, show=None):
    """Run cmd; everything goes to the log. On screen: all of its output, none
    (quiet), or the lines show(line) accepts. Progress lines ("... 12%", Ninja's
    "[12/280] ...") overwrite each other. On failure the last lines are shown."""
    import re
    if LOG:
        LOG.write("  > " + " ".join(cmd) + "\n")
    if not quiet and not show:
        print("  > " + " ".join(cmd), flush=True)
    p = subprocess.Popen(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True, errors="replace")
    tail = []
    shape = None
    for line in p.stdout:
        line = line.rstrip()
        tail = (tail + [line])[-40:]
        if re.match(r"^\[\d+/\d+\] ", line):
            now = "ninja"
        elif line.endswith("%"):
            now = "".join(c for c in line if not c.isdigit())
        else:
            now = None
        if LOG and (now is None or now != shape or now == "ninja"):
            LOG.write(line + "\n")
        if quiet or (show and now is None and not show(line)):
            continue
        if now is not None and now == shape:
            print("\r    " + line[:100].ljust(100), end="", flush=True)
        else:
            print(("\n" if shape else "") + "    " + line[:150], end="" if now else "\n", flush=True)
        shape = now
    if shape:
        print(flush=True)
    if p.wait():
        if quiet or show:
            print("    (last lines of its output:)")
            for line in tail:
                print("    " + line[:150])
        fail(f"{os.path.basename(cmd[0])} stopped with exit code {p.returncode}.")
    if LOG:
        LOG.flush()


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def sha1(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def download(name, dl_dir):
    url, want, size = DOWNLOADS[name]
    dst = os.path.join(dl_dir, os.path.basename(url))
    if os.path.isfile(dst) and sha256(dst) == want:
        return dst
    say(f"  downloading {os.path.basename(url)}" + (f" ({size // 1000000} MB)" if size else ""))
    tmp = dst + ".part"
    for attempt in range(3):
        try:
            with urllib.request.urlopen(url, timeout=60) as r, open(tmp, "wb") as f:
                total = int(r.headers.get("Content-Length") or 0)
                got, last = 0, 0
                while True:
                    b = r.read(1 << 20)
                    if not b:
                        break
                    f.write(b)
                    got += len(b)
                    if total and time.time() - last > 2:
                        last = time.time()
                        print(f"    {got * 100 // total}%", end="\r", flush=True)
            print("         ", end="\r", flush=True)
            break
        except OSError as e:
            say(f"    download failed ({e}), retrying")
            time.sleep(3)
    else:
        fail(f"could not download {url}. Check the internet connection.")
    got = sha256(tmp)
    if got != want:
        os.remove(tmp)
        fail(f"{os.path.basename(url)} does not match its checksum (got {got}). "
             "It was deleted; run Install.bat again to download it again.")
    os.replace(tmp, dst)
    return dst


def unzip(path, dst):
    with zipfile.ZipFile(path) as z:
        for m in z.infolist():
            target = os.path.join(dst, *m.filename.split("/"))
            if not os.path.abspath(target).startswith(os.path.abspath(dst) + os.sep):
                fail(f"{path} has an entry outside its folder: {m.filename}")
            if m.is_dir():
                os.makedirs(long_path(target), exist_ok=True)
                continue
            os.makedirs(long_path(os.path.dirname(target)), exist_ok=True)
            with z.open(m) as src, open(long_path(target), "wb") as out:
                shutil.copyfileobj(src, out)


def cp(src, dst):
    """shutil.copy2 without CopyFile2 (Python 3.12 on Windows uses it; Wine lacks it)."""
    if os.path.isdir(dst):
        dst = os.path.join(dst, os.path.basename(src))
    with open(src, "rb") as f, open(dst, "wb") as g:
        shutil.copyfileobj(f, g, 1 << 20)
    shutil.copystat(src, dst)
    return dst


def find_xbe(folder):
    for root, dirs, files in os.walk(folder):
        for f in files:
            if f.lower() == "default.xbe":
                return os.path.join(root, f)
        if root[len(folder):].count(os.sep) >= 3:
            dirs[:] = []
    return None


def git_env(work):
    """MinGit's own config turns on autocrlf and Git LFS: use none of it, and none of
    the user's own git settings either."""
    empty = os.path.join(work, "gitconfig-empty")
    open(empty, "a").close()
    return dict(os.environ, GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=empty,
                GIT_TERMINAL_PROMPT="0", GIT_LFS_SKIP_SMUDGE="1")


def fetch_commit(git, env, url, rev, dst):
    if os.path.isdir(os.path.join(dst, ".git")):
        r = subprocess.run([git, "-C", dst, "rev-parse", "HEAD"], env=env, capture_output=True, text=True)
        if r.stdout.strip() == rev:
            return False
        shutil.rmtree(dst, ignore_errors=True)
    os.makedirs(dst, exist_ok=True)
    say(f"  fetching {url.rsplit('/', 1)[-1]} {rev[:10]}")
    run([git, "-C", dst, "init", "-q"], env=env, quiet=True)
    run([git, "-C", dst, "fetch", "-q", "--depth", "1", url, rev], env=env, quiet=True)
    run([git, "-C", dst, "checkout", "-q", "FETCH_HEAD"], env=env, quiet=True)
    r = subprocess.run([git, "-C", dst, "rev-parse", "HEAD"], env=env, capture_output=True, text=True)
    if r.stdout.strip() != rev:
        fail(f"{url} did not give the expected commit {rev}.")
    return True


def copy_tree(src, dst, skip=()):
    for root, dirs, files in os.walk(src):
        rel = os.path.relpath(root, src)
        dirs[:] = [d for d in dirs if os.path.normpath(os.path.join(rel, d)) not in skip and d != ".git"]
        os.makedirs(long_path(os.path.join(dst, rel)), exist_ok=True)
        for f in files:
            s, d = os.path.join(root, f), os.path.join(dst, rel, f)
            if not os.path.exists(long_path(d)) or os.path.getsize(s) != os.path.getsize(long_path(d)):
                cp(long_path(s), long_path(d))


def shortcut(target, workdir):
    desktop = os.path.join(os.environ.get("USERPROFILE", ""), "Desktop")
    if not os.path.isdir(desktop):
        return None
    lnk = os.path.join(desktop, "Predator Concrete Jungle.lnk")
    ps = ("$s=(New-Object -ComObject WScript.Shell).CreateShortcut($env:PCJ_LNK);"
          "$s.TargetPath=$env:PCJ_TARGET;$s.WorkingDirectory=$env:PCJ_DIR;$s.Save()")
    env = dict(os.environ, PCJ_LNK=lnk, PCJ_TARGET=target, PCJ_DIR=workdir)
    try:
        r = subprocess.run(["powershell", "-NoProfile", "-Command", ps], env=env, capture_output=True)
    except OSError:
        return None
    return lnk if r.returncode == 0 else None


def build_root():
    """CMake, Ninja and meson trip over spaces in paths, and a user name with a space in
    it is common: build somewhere without one."""
    def plain(p):
        return " " not in p and p.isascii() and len(p) < 80
    if plain(REPO):
        return os.path.join(REPO, "_build")
    drive = os.environ.get("SystemDrive", "C:") + "\\"
    root = os.path.join(drive, "PCJ-build")
    say(f"This folder's path is long or has a space in it, so the build happens in {root}")
    say("(delete that folder after installing if you like).")
    return root


def main():
    global LOG
    # the build steps write their files as UTF-8 (Windows would use the ANSI code page)
    os.environ["PYTHONUTF8"] = "1"
    os.makedirs(OUT, exist_ok=True)
    LOG = open(os.path.join(REPO, "install-log.txt"), "a", encoding="utf-8")
    t0 = time.time()
    say("=" * 70)
    say("Predator: Concrete Jungle PC installer  " + time.strftime("%Y-%m-%d %H:%M"))
    say("=" * 70)
    if os.name != "nt":
        say("(not Windows: test mode, building with the tools on this machine's PATH)")
    root = build_root()
    src = os.path.join(root, "src")
    tools = os.path.join(root, "tools")
    dl = os.path.join(root, "downloads")
    for d in (src, tools, dl):
        os.makedirs(d, exist_ok=True)

    # ---- 1. the game ------------------------------------------------------------------
    say("\n[1/6] Looking for your game in \"PUT YOUR GAME HERE\"")
    game_out = os.path.join(OUT, "game")
    xbe = find_xbe(game_out) if os.path.isdir(game_out) else None
    if xbe and sha1(xbe) in XBE_SHA1S:
        say("  already in place: " + os.path.relpath(game_out, REPO))
    else:
        isos = [f for f in sorted(os.listdir(GAME_IN)) if f.lower().endswith((".iso", ".xiso"))] \
            if os.path.isdir(GAME_IN) else []
        disc = find_xbe(GAME_IN) if os.path.isdir(GAME_IN) else None
        free = shutil.disk_usage(REPO).free
        if isos:
            iso = os.path.join(GAME_IN, isos[0])
            need = os.path.getsize(iso) + (2 << 30)
            if free < need:
                fail(f"not enough free disk space: {need >> 30} GB needed here, {free >> 30} GB free.")
            say(f"  extracting {isos[0]} (a few minutes)")
            shutil.rmtree(game_out, ignore_errors=True)
            run([sys.executable, os.path.join(REPO, "tools", "xdvdfs.py"), iso, "x", game_out], quiet=True)
        elif disc:
            folder = os.path.dirname(disc)
            size = sum(os.path.getsize(os.path.join(r, f)) for r, _, fs in os.walk(folder) for f in fs)
            if free < size + (2 << 30):
                fail(f"not enough free disk space: {(size >> 30) + 2} GB needed here, {free >> 30} GB free.")
            say(f"  copying the game files ({size >> 20} MB)")
            shutil.rmtree(game_out, ignore_errors=True)
            copy_tree(folder, game_out)
        elif os.path.isdir(GAME_IN) and any(f.lower().endswith((".zip", ".7z", ".rar")) for f in os.listdir(GAME_IN)):
            fail("the game in \"PUT YOUR GAME HERE\" is still packed (.zip, .7z or .rar). Unpack it there first, "
                 "so the folder holds the .iso or the disc's files.")
        else:
            fail("there is no game in the \"PUT YOUR GAME HERE\" folder. Put your disc image (.iso) there, "
                 "or the folder you extracted the disc into (the one with default.xbe).")
        xbe = find_xbe(game_out)
        if not xbe:
            fail("the disc image has no default.xbe in it. Is it the Xbox disc of Predator: Concrete Jungle?")
        got = sha1(xbe)
        if got not in XBE_SHA1S:
            fail(f"default.xbe is not the USA retail release this port is built for (sha1 {got}). "
                 "Other releases are not supported yet.")
    say("  OK: USA retail default.xbe")

    # ---- 2. tools ---------------------------------------------------------------------
    say("\n[2/6] Build tools")
    win = os.name == "nt"
    py = sys.executable
    if win:
        gcc_dir = os.path.join(tools, "mingw64")
        if not os.path.isfile(os.path.join(gcc_dir, "bin", "gcc.exe")):
            z = download("gcc", dl)
            say("  unpacking GCC (a few minutes)")
            unzip(z, tools)
        git_dir = os.path.join(tools, "git")
        if not os.path.isfile(os.path.join(git_dir, "cmd", "git.exe")):
            unzip(download("git", dl), git_dir)
        git = os.path.join(git_dir, "cmd", "git.exe")
        bin_dir = os.path.join(gcc_dir, "bin")
        # subprocess finds programs on this process's PATH, not the child's
        os.environ["PATH"] = (bin_dir + os.pathsep + os.path.join(git_dir, "cmd") + os.pathsep +
                              os.path.dirname(py) + os.pathsep + os.environ.get("PATH", ""))
        os.environ["CC"], os.environ["CXX"] = "gcc", "g++"   # meson: not a Visual Studio cl.exe
        env = dict(os.environ)
    else:
        git = shutil.which("git")
        env = dict(os.environ)
    try:
        import capstone  # noqa: F401
        if win:
            import mesonbuild  # noqa: F401
    except ImportError:
        if not win:
            fail("pip install capstone first (test mode)")
        wheels = [download("capstone", dl), download("meson", dl)]
        run([py, "-m", "pip", "install", "-q", "--no-index", "--no-deps", "--no-warn-script-location", *wheels],
            quiet=True)
    gitenv = dict(git_env(root), PATH=env["PATH"])

    # ---- 3. sources -------------------------------------------------------------------
    say("\n[3/6] Sources")
    copy_tree(REPO, src, skip={"PUT YOUR GAME HERE", "Predator Concrete Jungle PC", "_build", "pcj-decomp",
                               "xboxrecomp", "installer", "game-files"})
    os.makedirs(os.path.join(src, "game"), exist_ok=True)
    cp(xbe, os.path.join(src, "game", "default.xbe"))
    xr = os.path.join(src, "xboxrecomp")
    if fetch_commit(git, gitenv, XR_URL, XR_REV, xr) or not os.path.isfile(os.path.join(xr, ".pcj-patched")):
        run([git, "-C", xr, "apply", os.path.join(src, "patches", "xboxrecomp-v0.15.0-pcj.patch")], env=gitenv,
            quiet=True)
        open(os.path.join(xr, ".pcj-patched"), "w").close()
    decomp = os.path.join(src, "pcj-decomp")
    local = os.path.join(REPO, "pcj-decomp")    # an offline copy next to Install.bat wins
    if os.path.isfile(os.path.join(local, "symbols", "functions.csv")):
        say("  using the decompilation in the pcj-decomp folder")
        copy_tree(local, decomp)
    else:
        fetch_commit(git, gitenv, DECOMP_URL, DECOMP_REV, decomp)

    # Windows libraries
    deps = os.path.join(root, "win-deps")
    if win:
        sdl = os.path.join(deps, "sdl2")
        if not os.path.isfile(os.path.join(sdl, "lib", "libSDL2.dll.a")):
            t = download("sdl2", dl)
            with tarfile.open(t) as tf:
                tf.extractall(dl, filter="data")
            shutil.rmtree(sdl, ignore_errors=True)
            shutil.copytree(os.path.join(dl, f"SDL2-{SDL_VER}", "x86_64-w64-mingw32"), sdl, copy_function=cp)
        epoxy = os.path.join(deps, "epoxy")
        if not os.path.isfile(os.path.join(epoxy, "lib", "libepoxy.dll.a")):
            say("  building libepoxy")
            esrc = os.path.join(dl, "libepoxy")
            fetch_commit(git, gitenv, EPOXY_URL, EPOXY_REV, esrc)
            ebuild = os.path.join(dl, "epoxy-build")
            shutil.rmtree(ebuild, ignore_errors=True)
            run([py, "-m", "mesonbuild.mesonmain", "setup", ebuild, esrc, "--prefix", epoxy,
                 "-Degl=no", "-Dx11=false", "-Dtests=false", "-Ddocs=false", "--buildtype=release"],
                env=env, quiet=True)
            run(["ninja", "-C", ebuild, "install"], env=env, quiet=True)

    # ---- 4. recompile -----------------------------------------------------------------
    say("\n[4/6] Recompiling default.xbe into C")
    done = os.path.join(src, "work", ".done")
    stamp = f"{XR_REV} {DECOMP_REV} {sha1(os.path.join(src, 'game', 'default.xbe'))}"
    if not (os.path.isfile(done) and open(done).read() == stamp):
        say("  about 5 to 15 minutes, depending on the PC")
        os.makedirs(os.path.join(src, "work"), exist_ok=True)
        run([py, os.path.join(src, "tools", "regen_all.py"), "--hybrid", os.path.join(src, "pregen", "hybrid"),
             "--decomp", decomp], cwd=src, env=env, show=lambda l: l.startswith(("[regen]", "REGEN")))
        with open(done, "w") as f:
            f.write(stamp)
    else:
        say("  already done")

    # ---- 5. compile -------------------------------------------------------------------
    say("\n[5/6] Compiling pcj.exe (5 to 20 minutes)")
    bdir = os.path.join(src, "build-win" if win else "build-installer")
    cfg = ["cmake", "-S", src, "-B", bdir, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
           f"-DPCJ_DECOMP_DIR={decomp}", f"-DCJ_HYBRID_PREGEN={os.path.join(src, 'pregen', 'hybrid')}",
           f"-DCJ_PYTHON={py}"]
    if win:
        cfg += ["-DCMAKE_C_COMPILER=gcc", "-DCMAKE_CXX_COMPILER=g++", f"-DCJ_WIN_DEPS={deps}"]
    if not os.path.isfile(os.path.join(bdir, "CMakeCache.txt")):
        run(cfg, env=env, quiet=True)
    jobs = str(max(1, (os.cpu_count() or 2)))
    run(["cmake", "--build", bdir, "-j", jobs, "--target", "pcj"], env=env,
        show=lambda l: "error" in l.lower())

    # ---- 6. assemble ------------------------------------------------------------------
    say("\n[6/6] Putting it together")
    exe = "pcj.exe" if win else "pcj"
    if win:
        run(["strip", "-o", os.path.join(OUT, exe), os.path.join(bdir, exe)], env=env, quiet=True)
        for f in (os.path.join(deps, "sdl2", "bin", "SDL2.dll"), os.path.join(deps, "epoxy", "bin", "libepoxy-0.dll"),
                  os.path.join(tools, "mingw64", "bin", "libwinpthread-1.dll")):
            cp(f, OUT)
    else:
        cp(os.path.join(bdir, exe), OUT)
    pkg = os.path.join(src, "dist", "pkgsrc")
    if not os.path.isfile(os.path.join(OUT, "pcj.ini")):
        cp(os.path.join(pkg, "pcj.ini"), OUT)
    with open(os.path.join(pkg, "README-PC.txt"), encoding="utf-8") as f, \
            open(os.path.join(OUT, "README-PC.txt"), "w", encoding="utf-8", newline="\r\n") as g:
        g.write(f.read())
    lic = os.path.join(OUT, "licenses")
    os.makedirs(lic, exist_ok=True)
    for s, d in ((os.path.join(dl, f"SDL2-{SDL_VER}", "LICENSE.txt"), "SDL2.txt"),
                 (os.path.join(dl, "libepoxy", "COPYING"), "libepoxy.txt"),
                 (os.path.join(pkg, "licenses", "winpthreads.txt"), "winpthreads.txt"),
                 (os.path.join(src, "LICENSE"), "pcj-pc.txt")):
        if os.path.isfile(s):
            cp(s, os.path.join(lic, d))
    tex = os.path.join(OUT, "textures", "sharp-ui")
    os.makedirs(tex, exist_ok=True)
    rep = os.path.join(src, "packs", "sharp-ui", "textures", "replace")
    for f in os.listdir(rep):
        if f.endswith(".png"):
            cp(os.path.join(rep, f), tex)
    cp(os.path.join(src, "packs", "sharp-ui", "README.txt"), tex)
    lnk = shortcut(os.path.join(OUT, exe), OUT) if win else None

    mins = (time.time() - t0) / 60
    say("\n" + "=" * 70)
    say(f"Done in {mins:.0f} minutes. The game is in the \"Predator Concrete Jungle PC\" folder.")
    if lnk:
        say("There is a shortcut on your desktop: \"Predator Concrete Jungle\".")
    say("You can delete the \"PUT YOUR GAME HERE\" contents now; the game was copied.")
    say("=" * 70)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        fail("stopped.")
