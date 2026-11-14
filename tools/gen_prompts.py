#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate spoken status prompts as 16 kHz mono signed 16-bit raw PCM.

Usage: gen_prompts.py <voice.onnx> <prompts.tsv> <out_dir>

Uses Piper TTS. The default voice (en_US-ljspeech) is trained on the public-domain
LJ Speech dataset.
"""
import io
import sys
import wave
from pathlib import Path

import numpy as np
import soxr
from piper import PiperVoice

TARGET_RATE = 16000
PEAK = 0.7          # -3 dBFS
TAIL_S = 0.08


def synth(voice: PiperVoice, text: str) -> np.ndarray:
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        voice.synthesize_wav(text, w)
    buf.seek(0)
    with wave.open(buf, "rb") as r:
        rate = r.getframerate()
        pcm = np.frombuffer(r.readframes(r.getnframes()), dtype=np.int16)
        if r.getnchannels() > 1:
            pcm = pcm.reshape(-1, r.getnchannels()).mean(axis=1)
    audio = pcm.astype(np.float32) / 32768.0
    if rate != TARGET_RATE:
        audio = soxr.resample(audio, rate, TARGET_RATE)
    return audio


def trim(audio: np.ndarray, thresh: float = 0.01) -> np.ndarray:
    idx = np.where(np.abs(audio) > thresh)[0]
    if len(idx) == 0:
        return audio
    start = max(0, idx[0] - int(0.02 * TARGET_RATE))
    end = min(len(audio), idx[-1] + int(0.05 * TARGET_RATE))
    return audio[start:end]


def main() -> None:
    model, tsv, out_dir = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3])
    out_dir.mkdir(parents=True, exist_ok=True)
    voice = PiperVoice.load(model)
    total = 0
    for line in tsv.read_text().splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        pid, text = line.split("\t", 1)
        audio = trim(synth(voice, text.strip()))
        peak = float(np.max(np.abs(audio))) or 1.0
        audio = audio * (PEAK / peak)
        fade = int(0.01 * TARGET_RATE)
        audio[:fade] *= np.linspace(0, 1, fade)
        audio[-fade:] *= np.linspace(1, 0, fade)
        audio = np.concatenate([audio, np.zeros(int(TAIL_S * TARGET_RATE), dtype=np.float32)])
        pcm = (np.clip(audio, -1, 1) * 32767).astype("<i2")
        (out_dir / f"{pid}.pcm").write_bytes(pcm.tobytes())
        total += pcm.nbytes
        print(f"{pid:16s} {len(pcm) / TARGET_RATE:5.2f}s  {text}")
    print(f"total {total / 1024:.0f} KB")


if __name__ == "__main__":
    main()
