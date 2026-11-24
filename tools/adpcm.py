#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""IMA ADPCM (4 bits/sample) for the board's built-in voice clips.

Exactly like dsp_adpcm_encode() in components/dsp/dsp.c: one stream from predictor 0
and step index 0, two samples per byte, first sample in the low nibble. Checked
against the C code by tests/host/test_dsp.c (fixture written by --fixture).

Usage: adpcm.py <clip.ulaw>...        convert mu-law clips to <clip>.adpcm (removes the .ulaw)
       adpcm.py --fixture <dir>       write the cross-check fixture (sweep.pcm, sweep.adpcm)
"""
import math
import sys
from pathlib import Path

STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
        73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408,
        449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
        2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
        9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767]
INDEX = [-1, -1, -1, -1, 2, 4, 6, 8]


def encode(samples) -> bytes:
    """16-bit samples (any int sequence) -> IMA ADPCM bytes. Odd lengths get a 0 pad."""
    samples = [int(s) for s in samples]
    if len(samples) % 2:
        samples.append(0)
    pred, idx = 0, 0
    codes = []
    for s in samples:
        diff = s - pred
        code = 0
        if diff < 0:
            code, diff = 8, -diff
        step = STEP[idx]
        if diff >= step:
            code |= 4
            diff -= step
        if diff >= step >> 1:
            code |= 2
            diff -= step >> 1
        if diff >= step >> 2:
            code |= 1
        delta = step >> 3
        if code & 4:
            delta += step
        if code & 2:
            delta += step >> 1
        if code & 1:
            delta += step >> 2
        pred = max(-32768, min(32767, pred - delta if code & 8 else pred + delta))
        idx = max(0, min(88, idx + INDEX[code & 7]))
        codes.append(code)
    return bytes(codes[i] | (codes[i + 1] << 4) for i in range(0, len(codes), 2))


def ulaw_decode(u: int) -> int:
    """Like dsp_ulaw_decode()."""
    u = ~u & 0xFF
    sign, exp, mant = u & 0x80, (u >> 4) & 7, u & 0x0F
    v = (((mant << 3) + 0x84) << exp) - 0x84
    return -v if sign else v


def sweep():
    """Test signal: a speech-like mix of tones, a loud burst and silence (16 kHz, 1 s)."""
    out = []
    for i in range(16000):
        t = i / 16000
        v = 9000 * math.sin(2 * math.pi * (150 + 1800 * t) * t) + 3000 * math.sin(2 * math.pi * 2400 * t)
        if 0.6 < t < 0.65:
            v = 30000 * (1 if (i // 8) % 2 else -1)
        if t > 0.9:
            v = 0
        out.append(int(max(-32768, min(32767, v))))
    return out


if __name__ == "__main__":
    if sys.argv[1:2] == ["--fixture"]:
        d = Path(sys.argv[2])
        d.mkdir(parents=True, exist_ok=True)
        pcm = sweep()
        (d / "sweep.pcm").write_bytes(b"".join(int(s).to_bytes(2, "little", signed=True) for s in pcm))
        (d / "sweep.adpcm").write_bytes(encode(pcm))
    else:
        for name in sys.argv[1:]:
            src = Path(name)
            src.with_suffix(".adpcm").write_bytes(encode(ulaw_decode(b) for b in src.read_bytes()))
            src.unlink()
