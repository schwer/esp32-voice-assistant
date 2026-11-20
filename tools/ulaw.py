#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""G.711 mu-law encoding for the board's built-in voice clips.

Usage (convert 16 kHz 16-bit PCM clips): ulaw.py <file.pcm>... -> writes <file>.ulaw, removes the .pcm
"""
import sys
from pathlib import Path

import numpy as np


def ulaw_encode(pcm):
    """Exactly like dsp_ulaw_encode() in components/dsp/dsp.c (the board decodes it)."""
    v = np.asarray(pcm).astype(np.int32)
    sign = np.where(v < 0, 0x80, 0)
    v = np.minimum(np.abs(v), 32635) + 0x84
    exp = np.clip(np.frexp(v.astype(np.float64))[1] - 1 - 7, 0, 7)
    mant = (v >> (exp + 3)) & 0x0F
    return ((~(sign | (exp << 4) | mant)) & 0xFF).astype(np.uint8)


if __name__ == "__main__":
    for name in sys.argv[1:]:
        src = Path(name)
        pcm = np.frombuffer(src.read_bytes(), dtype="<i2")
        src.with_suffix(".ulaw").write_bytes(ulaw_encode(pcm).tobytes())
        src.unlink()
