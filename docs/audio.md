# Audio: the game mixer played natively (2026-10-27)

## How the game makes sound
EngineX mixes everything itself. AudioMixer::Mix (0x2C71E0), called by xbMixerThread
(0x2B2520) every ~4 ms, decodes and mixes every voice (effects, speech, music streams, 3D
panning) into five 16-bit mono rings of 4096 samples at 44.1 kHz, one per speaker
(0x4D4328 + ch x 0x2000): 0 front left, 1 front right, 2 back left, 3 back right, 4 centre;
the LFE bin gets every channel at -7.84 dB. XBMixer_Five_One::Init (0x2B2690) gives each ring
to DirectSound as a looping buffer, plays all five in sync, and the mixer thread keeps up to
2048 samples ahead of buffer 0's play cursor. Only XMV movies use DirectSound streams of
their own (decoder.obj).

## Before
On the PC every one of those DirectSound calls ran the Xbox DirectSound library against the
emulated MCPX APU: register access trapped as access faults (~1,700 a second on Windows plus
~250 single steps), the APU's voice processor resampled the five voices to 48 kHz.

## Now (src/native_dsound.c, CJ_DSOUND, default on)
- CreateSpeakerChannel (0x2B23E0) is native: clears the ring, writes the mix-bin table the
  original leaves behind, stores a stand-in handle (0xD50 + ch). The game passes that handle
  only to Play, GetCurrentPosition, Stop and Release (checked: no other code reads
  0x4DE330..0x4DE343), and those four DirectSound entry points answer stand-in handles
  natively; any other handle runs the original library.
- Output: the APU's output thread calls g_apu_output_hook (xboxrecomp patch 0024) for every
  48 kHz period; the hook reads the rings at the play cursor, resamples 44.1 -> 48 kHz
  (linear), downmixes 5.0 to stereo (centre and surrounds at -3 dB) and adds it in. One audio
  device, as before; movies still come out of the APU.
- GetCurrentPosition reports the cursor the output consumed, so the mixer thread paces itself
  on the real device clock.
- Off in the deterministic mode (recordings keep the original path).

Settings: `CJ_DSOUND=0` original path; `CJ_DSOUND_WAV=<file>` writes what the game mixer plays;
`CJ_AUDIO_51=1` opens a separate six-channel device instead (FL FR C LFE BL BR) when the
system allows a second device.

## Checked
Linux, dummy audio driver, boot -> first level, 216 s: steady levels (rms 0.03-0.09, peaks
to 0.57), no clicks, the only silent seconds the boot (tools/wav_stats.py). Not yet heard on
real speakers: listen (compare with CJ_DSOUND=0).
Windows build under Wine (no audio device there), boot -> menu: 123 access faults per frame
with the native path vs 145 with the original. So most of the trapped APU traffic is not the
mixer's five voices: the DirectSound library still runs its per-frame work (DirectSoundDoWork,
interrupt service) against the APU. Removing that is the next step (stub the library's idle
work when no movie stream is open) and needs a measurement on real hardware first.
