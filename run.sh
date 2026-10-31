#!/bin/sh
# Boot the recompiled Predator: Concrete Jungle headless against the extracted disc.
# Frames presented by the NV2A pushbuffer executor are dumped to run/frames/fNNNNN.bmp.
DISC=${DISC:-${1:-$PWD/game-files}}
cd "$(dirname "$0")" && mkdir -p run/save run/frames
export RECOMP_AC97_READY=1        # emulated APU + AC97 traps (DirectSoundCreate)
export RECOMP_TITLE_KEVENTS=1     # D3D's statically built KEVENTs (vblank event)
export RECOMP_VBLANK=${RECOMP_VBLANK:-paced}            # 60 Hz vblank interrupts (EXApp frame gate)
export RECOMP_PB_EXEC=1           # execute the pushbuffer (software raster)
export RECOMP_FB_DUMP=${RECOMP_FB_DUMP-run/frames/f} RECOMP_FB_DUMP_FLIPS=${RECOMP_FB_DUMP_FLIPS-1}
export RECOMP_WATCHDOG_SECS=${RECOMP_WATCHDOG_SECS:-50}
exec ${PCJ_BIN:-./build/pcj} "$DISC" "${CJ_RUN_SAVE:-$PWD/run/save}"
