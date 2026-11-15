// SPDX-License-Identifier: MIT
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "dsp.h"

// ---------------------------------------------------------------- resampler

void dsp_resampler_reset(dsp_resampler_t *r)
{
    memset(r, 0, sizeof(*r));
}

size_t dsp_resample_24_16(dsp_resampler_t *r, const int16_t *in, size_t n, int16_t *out)
{
    size_t o = 0;
    float pos = r->pos;
    // Raw sample i, reaching back into the previous block's history for i < 0.
    #define RAW(i) ((i) >= 0 ? in[(i)] : r->hist[2 + (i)])
    // 3-tap low-pass (1,2,1)/4 so the 8-12 kHz band doesn't alias into speech.
    #define LP(i)  ((RAW((i) - 1) + 2 * RAW(i) + ((i) + 1 < (long)n ? RAW((i) + 1) : RAW(i))) / 4)
    while (pos + 1 < (float)n) {
        long i = (long)floorf(pos);                  // pos can start slightly negative
        float frac = pos - (float)i;
        float a = (float)LP(i), b = (float)LP(i + 1);
        out[o++] = (int16_t)(a + (b - a) * frac);
        pos += 1.5f;
    }
    #undef LP
    #undef RAW
    r->pos = pos - (float)n;
    if (n >= 2) {
        r->hist[0] = in[n - 2];
        r->hist[1] = in[n - 1];
    } else if (n == 1) {
        r->hist[0] = r->hist[1];
        r->hist[1] = in[0];
    }
    return o;
}

// ---------------------------------------------------------------- µ-law

uint8_t dsp_ulaw_encode(int16_t pcm)
{
    const int BIAS = 0x84, CLIP = 32635;
    int sign = (pcm >> 8) & 0x80, v = pcm;
    if (sign) v = -v;
    if (v > CLIP) v = CLIP;
    v += BIAS;
    int exp = 7;
    for (int mask = 0x4000; (v & mask) == 0 && exp > 0; mask >>= 1) exp--;
    int mant = (v >> (exp + 3)) & 0x0F;
    return (uint8_t)~(sign | (exp << 4) | mant);
}

int16_t dsp_ulaw_decode(uint8_t u)
{
    u = ~u;
    int sign = u & 0x80, exp = (u >> 4) & 0x07, mant = u & 0x0F;
    int v = ((mant << 3) + 0x84) << exp;
    v -= 0x84;
    return (int16_t)(sign ? -v : v);
}

// ---------------------------------------------------------------- speaker processing

typedef struct { float b0, b1, b2, a1, a2, x1, x2, y1, y2; } biquad_t;

static float biquad(biquad_t *f, float x)
{
    float y = f->b0 * x + f->b1 * f->x1 + f->b2 * f->x2 - f->a1 * f->y1 - f->a2 * f->y2;
    f->x2 = f->x1; f->x1 = x;
    f->y2 = f->y1; f->y1 = y;
    return y;
}

static void filters(biquad_t *hp, biquad_t *pk)
{
    const float fs = DSP_RATE, pi = 3.14159265f;
    // 2nd-order Butterworth high-pass at 150 Hz (RBJ, Q = 0.7071)
    float w0 = 2 * pi * 150 / fs, al = sinf(w0) / (2 * 0.7071f), c = cosf(w0), a0 = 1 + al;
    *hp = (biquad_t){ .b0 = (1 + c) / 2 / a0, .b1 = -(1 + c) / a0, .b2 = (1 + c) / 2 / a0,
                      .a1 = -2 * c / a0, .a2 = (1 - al) / a0 };
    // +3 dB peaking EQ at 3 kHz, Q = 1
    float A = powf(10, 3.0f / 40), w1 = 2 * pi * 3000 / fs, al1 = sinf(w1) / 2, c1 = cosf(w1), d0 = 1 + al1 / A;
    *pk = (biquad_t){ .b0 = (1 + al1 * A) / d0, .b1 = -2 * c1 / d0, .b2 = (1 - al1 * A) / d0,
                      .a1 = -2 * c1 / d0, .a2 = (1 - al1 / A) / d0 };
}

void dsp_speakerize(int16_t *pcm, size_t n)
{
    // Pass 1: filtered loudness of the speech (ignoring pauses).
    biquad_t hp, pk;
    filters(&hp, &pk);
    double sum = 0;
    size_t cnt = 0;
    for (size_t i = 0; i < n; i++) {
        float f = biquad(&pk, biquad(&hp, pcm[i] / 32768.0f));
        if (fabsf(f) > 0.02f) { sum += (double)f * f; cnt++; }
    }
    float rms = cnt ? sqrtf((float)(sum / cnt)) : 1e-3f;
    float gain = powf(10, -13.0f / 20) / fmaxf(rms, 1e-6f);
    // Pass 2: filter again (same result), apply gain and the soft limiter.
    filters(&hp, &pk);
    const float knee = 0.6f;
    for (size_t i = 0; i < n; i++) {
        float v = biquad(&pk, biquad(&hp, pcm[i] / 32768.0f)) * gain, m = fabsf(v);
        if (m > knee) v = copysignf(knee + 0.35f * tanhf((m - knee) / 0.35f), v);
        pcm[i] = (int16_t)(v * 32767);
    }
}

size_t dsp_tidy(int16_t *pcm, size_t n)
{
    const int thresh = 600;
    size_t start = 0, end = n;
    while (start < n && abs(pcm[start]) < thresh) start++;
    while (end > start && abs(pcm[end - 1]) < thresh) end--;
    if (end <= start) return 0;
    start = start > DSP_RATE / 50 ? start - DSP_RATE / 50 : 0;     // keep 20 ms lead-in
    end = end + DSP_RATE / 20 < n ? end + DSP_RATE / 20 : n;       // and 50 ms tail
    n = end - start;
    memmove(pcm, pcm + start, n * sizeof(int16_t));

    dsp_speakerize(pcm, n);
    size_t fade = DSP_RATE / 100;
    for (size_t i = 0; i < fade && i < n; i++) {
        pcm[i] = (int16_t)(pcm[i] * (float)i / fade);
        pcm[n - 1 - i] = (int16_t)(pcm[n - 1 - i] * (float)i / fade);
    }
    return n;
}

bool dsp_take_plausible(const char *text, float seconds)
{
    float expect = 0.35f + strlen(text) * 0.065f;
    return seconds > 0.25f && seconds < fmaxf(expect * 2.5f, expect + 2.5f);
}
