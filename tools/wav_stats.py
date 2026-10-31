#!/usr/bin/env python3
"""Level and continuity check of a 16-bit stereo WAV (CJ_DSOUND_WAV output).

    python3 -I tools/wav_stats.py out.wav

Prints length, RMS and peak per 1-second window, the share of silent windows, and counts of
'clicks' (sample-to-sample jumps above 40% of full scale in otherwise quiet material), which
show up when a ring buffer underruns or the cursor jumps."""
import struct, sys, math


def main():
    d = open(sys.argv[1], 'rb').read()
    assert d[:4] == b'RIFF' and d[8:12] == b'WAVE'
    i = d.find(b'data') + 8
    n = (len(d) - i) // 4
    s = struct.unpack('<%dh' % (n * 2), d[i:i + n * 4])
    rate = 44100
    print('%.1f s, %d frames' % (n / rate, n))
    silent = 0
    clicks = 0
    win = rate
    for w in range(0, n, win):
        seg = s[w * 2:(w + win) * 2]
        if not seg:
            break
        rms = math.sqrt(sum(v * v for v in seg) / len(seg)) / 32768
        peak = max(abs(v) for v in seg) / 32768
        if peak < 0.001:
            silent += 1
        for k in range(2, len(seg), 2):
            a, b = seg[k - 2], seg[k]
            if abs(b - a) > 13000 and abs(a) < 3000:
                clicks += 1
        print('  %4ds  rms %.3f  peak %.3f' % (w // rate, rms, peak))
    print('silent windows: %d of %d; click candidates: %d' % (silent, (n + win - 1) // win, clicks))


if __name__ == '__main__':
    main()
