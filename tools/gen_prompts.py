#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate spoken status prompts as 16 kHz mono signed 16-bit raw PCM.

Usage: gen_prompts.py <voice.onnx> <prompts.tsv> <out_dir>

Uses Piper TTS. The voice (en_US-ljspeech, "high" quality) is trained on the
public-domain LJ Speech dataset. These clips are only a fallback: the device re-records
every line in its Gemini conversation voice once it's set up.

Clips are processed for the board's small speaker: low bass removed (it can't play it,
and it muddies speech), a gentle presence lift for intelligibility, and a higher average
loudness with a soft peak limiter.
"""
import io
import sys
import wave
from pathlib import Path

import numpy as np
import soxr
from piper import PiperVoice
from piper.config import SynthesisConfig
from scipy.signal import butter, sosfiltfilt, lfilter

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ulaw import ulaw_encode  # noqa: E402
from adpcm import encode as adpcm_encode  # noqa: E402

TARGET_RATE = 16000
TAIL_S = 0.08
SPEED = 1.08            # length scale: slightly slower than default, easier to follow
TARGET_RMS_DB = -13.0   # average loudness of speech (about 4 dB louder than before)
LIMIT_KNEE = 0.6        # soft limiter starts here; output stays below ~0.95


def speakerize(audio: np.ndarray) -> np.ndarray:
    """Make speech clear on a small speaker. Mirrored in the firmware's recorder."""
    # Remove bass the speaker can't reproduce.
    sos = butter(2, 150, btype="highpass", fs=TARGET_RATE, output="sos")
    audio = sosfiltfilt(sos, audio)
    # +3 dB presence peak at 3 kHz (RBJ peaking EQ) for consonant clarity.
    f0, gain_db, q = 3000.0, 3.0, 1.0
    a = 10 ** (gain_db / 40)
    w0 = 2 * np.pi * f0 / TARGET_RATE
    alpha = np.sin(w0) / (2 * q)
    b = [1 + alpha * a, -2 * np.cos(w0), 1 - alpha * a]
    den = [1 + alpha / a, -2 * np.cos(w0), 1 - alpha / a]
    audio = lfilter(np.array(b) / den[0], np.array(den) / den[0], audio)
    # Loudness: set the RMS of the speech (not the pauses) to the target.
    speech = audio[np.abs(audio) > 0.02]
    rms = float(np.sqrt(np.mean(speech ** 2))) if len(speech) else 1e-3
    audio = audio * (10 ** (TARGET_RMS_DB / 20) / max(rms, 1e-6))
    # Soft limiter: smooth compression above the knee instead of clipping.
    mag = np.abs(audio)
    over = mag > LIMIT_KNEE
    audio[over] = np.sign(audio[over]) * (LIMIT_KNEE + 0.35 * np.tanh((mag[over] - LIMIT_KNEE) / 0.35))
    return audio.astype(np.float32)


def synth(voice: PiperVoice, text: str) -> np.ndarray:
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        voice.synthesize_wav(text, w, syn_config=SynthesisConfig(length_scale=SPEED))
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
    # Built-in clips: IMA ADPCM (4 bits/sample, smallest firmware). "ulaw": 8-bit mu-law
    # (the spoken "what's new" that boards download and play).
    fmt = sys.argv[4] if len(sys.argv) > 4 else "adpcm"
    out_dir.mkdir(parents=True, exist_ok=True)
    voice = PiperVoice.load(model)
    total = 0
    for line in tsv.read_text().splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        pid, text = line.split("\t", 1)
        audio = speakerize(trim(synth(voice, text.strip())))
        fade = int(0.01 * TARGET_RATE)
        audio[:fade] *= np.linspace(0, 1, fade)
        audio[-fade:] *= np.linspace(1, 0, fade)
        audio = np.concatenate([audio, np.zeros(int(TAIL_S * TARGET_RATE), dtype=np.float32)])
        pcm = (np.clip(audio, -1, 1) * 32767).astype("<i2")
        if fmt == "ulaw":
            data = ulaw_encode(pcm).tobytes()
        else:
            data = adpcm_encode(pcm.tolist())
            (out_dir / f"{pid}.ulaw").unlink(missing_ok=True)
        (out_dir / f"{pid}.{fmt}").write_bytes(data)
        (out_dir / f"{pid}.pcm").unlink(missing_ok=True)
        total += len(data)
        print(f"{pid:16s} {len(pcm) / TARGET_RATE:5.2f}s  {text}")
    print(f"total {total / 1024:.0f} KB")


if __name__ == "__main__":
    main()
