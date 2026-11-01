#!/bin/bash
# Build Predator: Concrete Jungle for PC from YOUR copy of the game.
#
#   ./build.sh <game folder or .iso> [--linux] [--jobs N]
#
# <game folder>: the extracted Xbox disc (the folder with default.xbe in it), or an Xbox disc
# image (.iso / .xiso), which is extracted to ./game-files first.
# Runs on Ubuntu 24.04, including WSL2 on Windows (open "Ubuntu", cd to this folder,
# give the path as /mnt/c/...). Output: dist/PCJ-PC/ with pcj.exe for Windows (and ./pcj for
# Linux with --linux). Nothing from the game is downloaded or included: the recompiler reads
# your default.xbe, and the game reads your disc files at run time.
#
# Takes 10-20 minutes the first time. Re-runs reuse work/.
set -euo pipefail
cd "$(dirname "$0")"
ROOT=$PWD

GAME=""; LINUX=0; JOBS=$(nproc 2>/dev/null || echo 2)
while [ $# -gt 0 ]; do
  case "$1" in
    --linux) LINUX=1 ;;
    --jobs) JOBS=$2; shift ;;
    -h|--help) sed -n 2,15p "$0"; exit 0 ;;
    *) GAME=$1 ;;
  esac
  shift
done
[ -n "$GAME" ] || { echo "usage: ./build.sh <game folder or .iso> [--linux]"; exit 2; }

XBE_SHA1S="7fbc1249c5948d638a62101cc2d23d6637b76284 30a1e430892c9e472122a3a25d90a233e42b99cd"   # USA retail (as on the disc; and with 3 certificate bytes changed)
XR_URL=https://github.com/sp00nznet/xboxrecomp
XR_REV=6376eb829e035d23c1c65377d8523eceb068ee1d                                          # v0.15.0
DECOMP_URL=${PCJ_DECOMP_URL:-https://github.com/Adolini1715/Concrete-Jungle-Decomp}
SDL_VER=2.30.9
SDL_SHA256=b188d165dc4a0372042bff3d0c18e9bb90772581e92a7b8b47b521b47714f75c
EPOXY_VER=1.5.10
EPOXY_COMMIT=c84bc9459357a40e46e2fec0408d04fbdde2c973
DECOMP_REF=${PCJ_DECOMP_REF:-v0.1}                         # the decomp release this port is built against

say() { printf '\n== %s\n' "$*"; }

# ---- 1. host packages ----------------------------------------------------------------------
need=()
for p in git cmake ninja-build python3 python3-pip python3-venv gcc g++ pkg-config \
         gcc-mingw-w64-x86-64-posix g++-mingw-w64-x86-64-posix mingw-w64-x86-64-dev zip curl \
         python3-clang-18 libclang-18-dev; do
  dpkg -s "$p" >/dev/null 2>&1 || need+=("$p")
done
if [ $LINUX = 1 ]; then
  for p in libsdl2-dev libepoxy-dev libssl-dev; do dpkg -s "$p" >/dev/null 2>&1 || need+=("$p"); done
fi
if [ ${#need[@]} -gt 0 ]; then
  say "Installing build tools: ${need[*]}"
  SUDO=""; [ "$(id -u)" = 0 ] || SUDO=sudo
  $SUDO apt-get update && $SUDO apt-get install -y "${need[@]}"
fi
if [ ! -x .venv/bin/python ]; then python3 -m venv --system-site-packages .venv; fi   # system site: python3-clang-18
.venv/bin/pip -q install capstone meson
export PATH=$ROOT/.venv/bin:$PATH

# ---- 2. the game ---------------------------------------------------------------------------
if [ -f "$GAME" ]; then
  say "Extracting the disc image to game-files/"
  python3 tools/xdvdfs.py "$GAME" x game-files
  GAME=$ROOT/game-files
fi
XBE=""
for n in default.xbe DEFAULT.XBE Default.xbe; do [ -f "$GAME/$n" ] && XBE="$GAME/$n"; done
[ -n "$XBE" ] || { echo "No default.xbe in $GAME. Give the folder you extracted the disc into."; exit 1; }
got=$(sha1sum "$XBE" | cut -d' ' -f1)
if ! printf "%s\n" $XBE_SHA1S | grep -qx "$got"; then
  echo "default.xbe is not the USA retail release this port is built for (sha1 $got)."
  echo "Other releases are not supported yet."
  exit 1
fi
mkdir -p game && cp "$XBE" game/default.xbe

# ---- 3. sources: the recompiler toolkit and the decompilation ------------------------------
if [ ! -d xboxrecomp/.git ]; then
  say "Fetching xboxrecomp $XR_REV"
  GIT_LFS_SKIP_SMUDGE=1 git clone -q "$XR_URL" xboxrecomp
  git -C xboxrecomp checkout -q "$XR_REV"
  [ "$(git -C xboxrecomp rev-parse HEAD)" = "$XR_REV" ] || { echo "xboxrecomp is not at $XR_REV"; exit 1; }
  git -C xboxrecomp apply "$ROOT"/patches/xboxrecomp-v0.15.0-pcj.patch
fi
if [ -z "${PCJ_DECOMP:-}" ]; then
  if [ ! -d pcj-decomp/.git ]; then
    say "Fetching pcj-decomp"
    git clone -q --depth 1 -b "$DECOMP_REF" "$DECOMP_URL" pcj-decomp
  fi
  export PCJ_DECOMP=$ROOT/pcj-decomp
fi

# ---- 4. Windows libraries (SDL2 release, libepoxy built from source) -----------------------
if [ ! -f win-deps/sdl2/lib/libSDL2.dll.a ]; then
  say "Fetching SDL2 $SDL_VER (MinGW)"
  mkdir -p win-deps/dl
  curl -sSL -o win-deps/dl/sdl2.tgz "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL2-devel-$SDL_VER-mingw.tar.gz"
  echo "$SDL_SHA256  win-deps/dl/sdl2.tgz" | sha256sum -c --quiet - || { echo "SDL2 download does not match its checksum"; rm -f win-deps/dl/sdl2.tgz; exit 1; }
  tar -xzf win-deps/dl/sdl2.tgz -C win-deps/dl
  mkdir -p win-deps/sdl2 && cp -r win-deps/dl/SDL2-$SDL_VER/x86_64-w64-mingw32/. win-deps/sdl2/
fi
if [ ! -f win-deps/epoxy/lib/libepoxy.dll.a ]; then
  say "Building libepoxy $EPOXY_VER for Windows"
  [ -d win-deps/dl/libepoxy ] || git clone -q --depth 1 -b $EPOXY_VER https://github.com/anholt/libepoxy win-deps/dl/libepoxy
  [ "$(git -C win-deps/dl/libepoxy rev-parse HEAD)" = "$EPOXY_COMMIT" ] || { echo "libepoxy $EPOXY_VER is not the expected commit"; exit 1; }
  cat > win-deps/dl/mingw.cross <<'EOF'
[binaries]
c = 'x86_64-w64-mingw32-gcc-posix'
ar = 'x86_64-w64-mingw32-ar'
strip = 'x86_64-w64-mingw32-strip'
windres = 'x86_64-w64-mingw32-windres'
[host_machine]
system = 'windows'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOF
  meson setup win-deps/dl/epoxy-build win-deps/dl/libepoxy --cross-file win-deps/dl/mingw.cross \
    -Dprefix="$ROOT/win-deps/epoxy" -Degl=no -Dx11=false -Dtests=false -Ddocs=false --buildtype=release >/dev/null
  ninja -C win-deps/dl/epoxy-build install >/dev/null
fi

# ---- 5. recompile ---------------------------------------------------------------------------
if [ ! -f work/.done ]; then
  say "Recompiling default.xbe (5-15 minutes the first time)"
  mkdir -p build work
  sh tools/regen_all.sh
  touch work/.done
fi

# ---- 6. build -------------------------------------------------------------------------------
say "Building pcj.exe"
[ -f build-win/CMakeCache.txt ] || cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=xboxrecomp/cmake/mingw-w64-x86_64.cmake \
  -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc-posix -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++-posix \
  -DCMAKE_BUILD_TYPE=Release -DPCJ_DECOMP_DIR="$PCJ_DECOMP" >/dev/null
cmake --build build-win -j "$JOBS" --target pcj
OUT=dist/PCJ-PC
mkdir -p "$OUT"
x86_64-w64-mingw32-strip -o "$OUT/pcj.exe" build-win/pcj.exe
cp win-deps/sdl2/bin/SDL2.dll win-deps/epoxy/bin/libepoxy-0.dll /usr/x86_64-w64-mingw32/lib/libwinpthread-1.dll "$OUT/"
[ -f "$OUT/pcj.ini" ] || cp dist/pkgsrc/pcj.ini "$OUT/"
sed "s/\r*$/\r/" dist/pkgsrc/README-PC.txt > "$OUT/README-PC.txt"
mkdir -p "$OUT/licenses"   # licence texts of the bundled libraries
cp win-deps/dl/SDL2-$SDL_VER/LICENSE.txt "$OUT/licenses/SDL2.txt" 2>/dev/null || true
cp win-deps/dl/libepoxy/COPYING "$OUT/licenses/libepoxy.txt" 2>/dev/null || true
cp /usr/share/doc/mingw-w64-x86-64-dev/copyright "$OUT/licenses/winpthreads.txt" 2>/dev/null || true
[ -f LICENSE ] && cp LICENSE "$OUT/licenses/pcj-pc.txt"
mkdir -p "$OUT/textures/sharp-ui"
cp packs/sharp-ui/textures/replace/*.png packs/sharp-ui/README.txt "$OUT/textures/sharp-ui/"   # our own redrawn UI art, on by default
if [ $LINUX = 1 ]; then
  say "Building the Linux binary"
  [ -f build/CMakeCache.txt ] || cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPCJ_DECOMP_DIR="$PCJ_DECOMP" >/dev/null
  cmake --build build -j "$JOBS" --target pcj
  cp build/pcj "$OUT/pcj"
fi
say "Done: $OUT"
echo "Copy that folder anywhere on Windows and run pcj.exe. The first time it asks for your game"
echo "folder (the extracted disc: $GAME). Texture packs go in its textures folder."
