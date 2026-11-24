// SPDX-License-Identifier: MIT
// Audio maths: resampler, mu-law, speaker processing, trimming, take check.
#include <stdlib.h>
#include <string.h>
#include "dsp.h"
#include "test.h"

#define PI 3.14159265358979

static int16_t *tone(int rate, double hz, double amp, size_t n)
{
    int16_t *b = malloc(n * sizeof(int16_t));
    for (size_t i = 0; i < n; i++) b[i] = (int16_t)(amp * 32767 * sin(2 * PI * hz * i / rate));
    return b;
}

static double rms_db(const int16_t *x, size_t n)
{
    double s = 0;
    for (size_t i = 0; i < n; i++) s += (double)x[i] * x[i];
    return 20 * log10(sqrt(s / n) / 32768.0 + 1e-12);
}

static void resampler_produces_two_thirds(void)
{
    int16_t *in = tone(24000, 440, 0.5, 24000);
    int16_t out[DSP_RESAMPLE_MAX_OUT(24000)];
    dsp_resampler_t r;
    dsp_resampler_reset(&r);
    size_t n = dsp_resample_24_16(&r, in, 24000, out);
    CHECK(n >= 15999 && n <= 16001);
    CHECK(n <= DSP_RESAMPLE_MAX_OUT(24000));
    free(in);
}

static void resampler_is_accurate_and_continuous_across_blocks(void)
{
    // Feed one second of a 440 Hz tone in awkward block sizes; compare to the ideal.
    int16_t *in = tone(24000, 440, 0.3, 24000);
    static int16_t out[20000];
    int16_t tmp[DSP_RESAMPLE_MAX_OUT(9999)];
    dsp_resampler_t r;
    dsp_resampler_reset(&r);
    size_t total = 0, off = 0;
    const size_t sizes[] = { 1000, 333, 4801, 7, 1, 2500, 9999 };
    for (int k = 0; off < 24000; k = (k + 1) % 7) {
        size_t n = sizes[k];
        if (off + n > 24000) n = 24000 - off;
        size_t o = dsp_resample_24_16(&r, in + off, n, tmp);
        memcpy(out + total, tmp, o * sizeof(int16_t));
        total += o;
        off += n;
    }
    CHECK(total >= 15999 && total <= 16001);
    double err = 0, peak = 0;
    for (size_t i = 2; i < total - 2; i++) {
        double ideal = 0.3 * 32767 * sin(2 * PI * 440 * i / 16000.0);
        double d = fabs(out[i] - ideal);
        err += d * d;
        if (d > peak) peak = d;
    }
    double rel_rms = sqrt(err / (total - 4)) / (0.3 * 32767 / sqrt(2));
    CHECK(rel_rms < 0.01);          // < 1% error
    CHECK(peak < 0.03 * 32767);     // no glitches at block boundaries
    free(in);
}

static void resampler_reset_clears_state(void)
{
    dsp_resampler_t r;
    dsp_resampler_reset(&r);
    r.pos = 0.7f;
    r.hist[0] = 123;
    dsp_resampler_reset(&r);
    CHECK(r.pos == 0 && r.hist[0] == 0 && r.hist[1] == 0);
}

static void ulaw_round_trip_is_close_for_every_value(void)
{
    int worst = 0;
    for (int v = -32768; v <= 32767; v++) {
        int d = dsp_ulaw_decode(dsp_ulaw_encode((int16_t)v));
        if (abs(v) > 1000 && abs(d - v) * 16 > abs(v)) worst++;
    }
    CHECK(worst == 0);              // within 6% above the quietest levels
    for (int u = 0; u < 256; u++) {
        // decode -> encode -> decode is stable (codes are fixed points)
        int16_t d = dsp_ulaw_decode((uint8_t)u);
        CHECK(dsp_ulaw_decode(dsp_ulaw_encode(d)) == d);
    }
}

static void ulaw_snr_matches_g711(void)
{
    int16_t *x = tone(16000, 440, 0.6, 16000);
    double se = 0, ss = 0;
    for (int i = 0; i < 16000; i++) {
        int d = dsp_ulaw_decode(dsp_ulaw_encode(x[i]));
        se += (double)(d - x[i]) * (d - x[i]);
        ss += (double)x[i] * x[i];
    }
    CHECK(10 * log10(ss / se) > 35);   // G.711 mu-law is ~38 dB
    free(x);
}

static void speakerize_sets_loudness_and_limits_peaks(void)
{
    for (double amp = 0.05; amp <= 0.9; amp *= 3) {
        int16_t *x = tone(16000, 800, amp, 16000);
        dsp_speakerize(x, 16000);
        CHECK_NEAR(rms_db(x, 16000), -13.0, 1.5);
        int peak = 0;
        for (int i = 0; i < 16000; i++) if (abs(x[i]) > peak) peak = abs(x[i]);
        CHECK(peak < 32767 * 0.96);     // soft limiter keeps it below clipping
        free(x);
    }
}

/** Amplitude of the `hz` component of x[from..n) (single-bin DFT). */
static double component(const int16_t *x, size_t from, size_t n, double hz)
{
    double c = 0, s = 0;
    for (size_t i = from; i < n; i++) {
        c += x[i] * cos(2 * PI * hz * i / 16000.0);
        s += x[i] * sin(2 * PI * hz * i / 16000.0);
    }
    return 2 * sqrt(c * c + s * s) / (n - from);
}

static void speakerize_removes_bass_the_speaker_cannot_play(void)
{
    // Equal-level 50 Hz and 1 kHz tones in, 50 Hz well below 1 kHz out.
    int16_t *lo = tone(16000, 50, 0.3, 16000), *mid = tone(16000, 1000, 0.3, 16000);
    int16_t *mix = malloc(16000 * sizeof(int16_t));
    for (int i = 0; i < 16000; i++) mix[i] = (int16_t)(lo[i] / 2 + mid[i] / 2);
    dsp_speakerize(mix, 16000);
    double ratio_db = 20 * log10(component(mix, 4000, 16000, 50) / component(mix, 4000, 16000, 1000));
    CHECK(ratio_db < -10);
    free(lo); free(mid); free(mix);
}

static void tidy_trims_silence_and_fades(void)
{
    // 1 s silence, 0.5 s tone, 1 s silence
    size_t n = 16000 * 5 / 2;
    int16_t *x = calloc(n, sizeof(int16_t));
    int16_t *t = tone(16000, 500, 0.4, 8000);
    memcpy(x + 16000, t, 8000 * sizeof(int16_t));
    size_t m = dsp_tidy(x, n);
    CHECK(m > 8000 && m < 8000 + 16000 / 10);     // tone + 20 ms lead-in + 50 ms tail
    CHECK(abs(x[0]) < 50 && abs(x[m - 1]) < 50);  // faded edges: no clicks
    free(x); free(t);

    int16_t quiet[1000] = {0};
    CHECK(dsp_tidy(quiet, 1000) == 0);            // all silence
}

static void take_check_rejects_rambling_and_empty_takes(void)
{
    CHECK(dsp_take_plausible("Yes?", 0.7f));
    CHECK(!dsp_take_plausible("Yes?", 6.0f));     // answered instead of reading
    CHECK(!dsp_take_plausible("Yes?", 0.1f));     // nothing said
    const char *long_line = "I'm online, but I need a Gemini API key. Open my setup page to add one.";
    CHECK(dsp_take_plausible(long_line, 5.9f));
    CHECK(!dsp_take_plausible(long_line, 25.0f));
}

static size_t read_file(const char *path, void *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    return n;
}

static double snr_db(const int16_t *ref, const int16_t *got, size_t n)
{
    double sig = 0, err = 0;
    for (size_t i = 0; i < n; i++) {
        sig += (double)ref[i] * ref[i];
        err += (double)(ref[i] - got[i]) * (ref[i] - got[i]);
    }
    return 10 * log10(sig / (err + 1e-9));
}

static void adpcm_matches_the_python_encoder_byte_for_byte(void)
{
    // fixtures/adpcm was written by tools/adpcm.py --fixture: the clips the build
    // makes in Python must decode on the board exactly as intended.
    static int16_t pcm[16000], back[16000];
    static uint8_t py[8000], c[8000];
    size_t np = read_file("fixtures/adpcm/sweep.pcm", pcm, sizeof(pcm)) / 2;
    size_t nb = read_file("fixtures/adpcm/sweep.adpcm", py, sizeof(py));
    CHECK(np == 16000 && nb == 8000);
    dsp_adpcm_t s;
    dsp_adpcm_reset(&s);
    CHECK(dsp_adpcm_encode(&s, pcm, np, c) == nb);
    CHECK(memcmp(c, py, nb) == 0);
    dsp_adpcm_reset(&s);
    CHECK(dsp_adpcm_decode(&s, py, nb, back) == np);
    CHECK(snr_db(pcm, back, 9600) > 20);               // the tones (before the square-wave burst)
}

static void adpcm_decodes_in_chunks_like_one_block(void)
{
    static int16_t pcm[4000], whole[4000], parts[4000];
    static uint8_t enc[2000];
    for (int i = 0; i < 4000; i++) pcm[i] = (int16_t)(12000 * sin(i * 0.07) + 4000 * sin(i * 0.9));
    dsp_adpcm_t s;
    dsp_adpcm_reset(&s);
    size_t nb = dsp_adpcm_encode(&s, pcm, 4000, enc);
    dsp_adpcm_reset(&s);
    dsp_adpcm_decode(&s, enc, nb, whole);
    dsp_adpcm_reset(&s);
    size_t got = 0;
    for (size_t off = 0; off < nb; off += 333) {
        size_t k = nb - off < 333 ? nb - off : 333;
        got += dsp_adpcm_decode(&s, enc + off, k, parts + got);
    }
    CHECK(got == 4000 && memcmp(whole, parts, sizeof(whole)) == 0);
    CHECK(snr_db(pcm, whole, 4000) > 18);
}

static void adpcm_survives_extremes(void)
{
    static int16_t pcm[1000], back[1000];
    static uint8_t enc[500];
    for (int i = 0; i < 1000; i++) pcm[i] = (i / 4) % 2 ? 32767 : -32768;   // full-scale square
    dsp_adpcm_t s;
    dsp_adpcm_reset(&s);
    dsp_adpcm_encode(&s, pcm, 1000, enc);
    CHECK(s.index >= 0 && s.index <= 88);
    dsp_adpcm_reset(&s);
    dsp_adpcm_decode(&s, enc, 500, back);            // no overflow (UBSan would catch it)
    CHECK(s.index >= 0 && s.index <= 88);
}

int main(void)
{
    RUN(resampler_produces_two_thirds);
    RUN(resampler_is_accurate_and_continuous_across_blocks);
    RUN(resampler_reset_clears_state);
    RUN(ulaw_round_trip_is_close_for_every_value);
    RUN(ulaw_snr_matches_g711);
    RUN(adpcm_matches_the_python_encoder_byte_for_byte);
    RUN(adpcm_decodes_in_chunks_like_one_block);
    RUN(adpcm_survives_extremes);
    RUN(speakerize_sets_loudness_and_limits_peaks);
    RUN(speakerize_removes_bass_the_speaker_cannot_play);
    RUN(tidy_trims_silence_and_fades);
    RUN(take_check_rejects_rambling_and_empty_takes);
    TEST_MAIN_END("dsp");
}
