// SPDX-License-Identifier: MIT
//
// Audio maths shared by the firmware and the host unit tests (tests/host).
// No ESP-IDF or FreeRTOS dependencies.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DSP_RATE 16000

/** Streaming 24 kHz -> 16 kHz resampler (3:2) with a light low-pass. */
typedef struct {
    float pos;           // position into the next block, in input samples
    int16_t hist[2];     // last two input samples of the previous block
} dsp_resampler_t;

void dsp_resampler_reset(dsp_resampler_t *r);
/** Max outputs for `in_samples` inputs (size `out` with this). */
#define DSP_RESAMPLE_MAX_OUT(in_samples) ((in_samples) * 2 / 3 + 2)
/**
 * Resample one block; continuous across calls. Returns samples written to `out`
 * (at most DSP_RESAMPLE_MAX_OUT(n)).
 */
size_t dsp_resample_24_16(dsp_resampler_t *r, const int16_t *in, size_t n, int16_t *out);

/** G.711 µ-law. */
uint8_t dsp_ulaw_encode(int16_t pcm);
int16_t dsp_ulaw_decode(uint8_t u);

/**
 * IMA ADPCM, 4 bits per sample (two per byte, first sample in the low nibble), one
 * continuous stream starting from predictor 0, step index 0. Used for the built-in
 * voice clips; tools/adpcm.py encodes exactly the same way.
 */
typedef struct { int32_t predictor; int index; } dsp_adpcm_t;
void dsp_adpcm_reset(dsp_adpcm_t *s);
/** Encode n samples (n even) into n/2 bytes. Returns bytes written. */
size_t dsp_adpcm_encode(dsp_adpcm_t *s, const int16_t *pcm, size_t n, uint8_t *out);
/** Decode `bytes` bytes into 2*bytes samples. Returns samples written. */
size_t dsp_adpcm_decode(dsp_adpcm_t *s, const uint8_t *in, size_t bytes, int16_t *out);

/**
 * Make speech clear on the board's small speaker (same as tools/gen_prompts.py):
 * 150 Hz high-pass, +3 dB at 3 kHz, speech loudness to -13 dBFS, soft limiter.
 * In place, no allocation.
 */
void dsp_speakerize(int16_t *pcm, size_t n);

/**
 * Trim leading/trailing silence (keeping 20 ms / 50 ms), speakerize, fade the edges.
 * In place; returns the new length (0 if it was all silence).
 */
size_t dsp_tidy(int16_t *pcm, size_t n);

/** Does a take of `seconds` plausibly read `text` aloud (not answer or ramble)? */
bool dsp_take_plausible(const char *text, float seconds);
