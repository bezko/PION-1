/*
 * PION/1 -- host renderer.
 *
 * Renders the synth's bit stream to a WAV file on the host, using exactly
 * the same DSP code as the firmware.  That makes it possible to *listen*
 * to what the firmware will produce, and to inspect it with any audio tool,
 * without having the hardware on the bench.
 *
 * The output is a 16-bit PCM WAV at 44.1 kHz.  The synth runs at 500 kHz
 * internally, so the renderer decimates by 500000/44100 = 11.34 samples per
 * output word; the boxcar filter used for the decimation is the same
 * averaging the analog front end performs.
 *
 * Usage:
 *     render_host [--freq HZ] [--fm-freq HZ] [--fm-depth N] [--seconds N]
 *                 [--tier 0|1|2] [--out FILE]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "bitops.h"
#include "sine_table.h"
#include "mixer.h"
#include "resample.h"
#include "voices.h"

#define PION1_FS_HZ 500000u
#define PION1_OUT_HZ 44100u

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [--freq HZ] [--fm-freq HZ] [--fm-depth N] [--seconds N]\n"
        "          [--tier 0|1|2] [--out FILE]\n", argv0);
}

int main(int argc, char **argv)
{
    double freq = 440.0;
    double fm_freq = 0.0;
    uint32_t fm_depth = 0;
    double seconds = 2.0;
    uint32_t tier = PION1_TIER_BOXCAR;
    const char *out_path = "pion1.wav";

    uint32_t words_total, i;
    uint32_t *words;
    FILE *f;
    uint32_t nsamples;
    uint32_t s;
    int16_t *pcm;

    for (i = 1; i < (uint32_t)argc; i++) {
        if (!strcmp(argv[i], "--freq") && i + 1 < (uint32_t)argc) {
            freq = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--fm-freq") && i + 1 < (uint32_t)argc) {
            fm_freq = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--fm-depth") && i + 1 < (uint32_t)argc) {
            fm_depth = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--seconds") && i + 1 < (uint32_t)argc) {
            seconds = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--tier") && i + 1 < (uint32_t)argc) {
            tier = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--out") && i + 1 < (uint32_t)argc) {
            out_path = argv[++i];
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    if (freq < 0.0 || freq > 20000.0) {
        fprintf(stderr, "frequency out of range\n");
        return 1;
    }

    words_total = (uint32_t)(seconds * (double)PION1_FS_HZ / 32.0);
    if (words_total == 0u) {
        fprintf(stderr, "nothing to render\n");
        return 1;
    }

    words = (uint32_t *)calloc(words_total, sizeof(uint32_t));
    if (!words) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    /*
     * Render.
     *
     * The oscillator voice is driven exactly as the firmware drives it:
     * one word covers 32 consecutive samples of the 1-bit output, so the
     * phase advances 32 times per word.  The word then goes through the
     * same mask-select mixer the firmware uses, which is what gives the
     * bit stream its rotating slot assignment.
     */
    {
        pion1_voice_t v;
        pion1_mixer_t mixer;
        uint32_t phase_step = PION1_PHASE_STEP(freq, PION1_FS_HZ);
        uint32_t fm_step = PION1_PHASE_STEP(fm_freq, PION1_FS_HZ);
        uint32_t w;

        pion1_voice_init(&v, PION1_VOICE_OSC);
        v.increment = phase_step;
        v.fm_increment = fm_step;
        v.fm_depth = fm_depth;
        v.fm_index = 0x8000u;

        pion1_mixer_init(&mixer, 1u, 0u);
        pion1_mixer_set_level(&mixer, 0u, 65535u);

        for (w = 0; w < words_total; w++) {
            uint32_t vw[1];
            vw[0] = pion1_voice_osc_word(&v);
            words[w] = pion1_mix(&mixer, vw);
        }
    }

    /*
     * Decimate to the output rate.
     *
     * The synth runs at 500 kHz and the output is 44.1 kHz, so each output
     * sample covers 500000/44100 = 11.34 input samples.  The bit stream is
     * averaged over that window, which is precisely what the analog
     * low-pass in the output stage does.  The resampler does the same job
     * with the selected quality tier.
     */
    nsamples = (uint32_t)(seconds * (double)PION1_OUT_HZ);
    pcm = (int16_t *)calloc(nsamples, sizeof(int16_t));
    if (!pcm) {
        fprintf(stderr, "out of memory\n");
        free(words);
        return 1;
    }

    {
        pion1_resampler_t rs;
        uint32_t step = (uint32_t)(((uint64_t)PION1_FS_HZ << 16) / PION1_OUT_HZ);
        uint32_t win = (tier == PION1_TIER_CRUDE) ? 1u : 32u;
        /*
         * The resampler treats the source as a ring and masks the word
         * index, so the ring has to be a power of two in words.  Pad the
         * rendered stream up to the next power of two; the extra bits are
         * silence and are never reached within the requested duration.
         */
        uint32_t ring_words = 1u;
        while (ring_words < words_total) {
            ring_words <<= 1;
        }
        {
            uint32_t *ring = (uint32_t *)calloc(ring_words, sizeof(uint32_t));
            if (!ring) {
                fprintf(stderr, "out of memory\n");
                free(words);
                free(pcm);
                return 1;
            }
            memcpy(ring, words, words_total * sizeof(uint32_t));

            pion1_resampler_init(&rs, (pion1_tier_t)tier, win, step);

            for (s = 0; s < nsamples; s++) {
                double level = pion1_resample_analog(&rs, ring, ring_words * 32u);
                pcm[s] = (int16_t)(level * 20000.0);
            }
            free(ring);
        }
    }

    /* Write the WAV. */
    f = fopen(out_path, "wb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", out_path);
        free(words);
        free(pcm);
        return 1;
    }
    {
        uint32_t data_bytes = nsamples * 2u;
        uint8_t hdr[44];
        uint32_t riff_size = 36u + data_bytes;

        memcpy(hdr + 0, "RIFF", 4);
        memcpy(hdr + 4, &riff_size, 4);
        memcpy(hdr + 8, "WAVEfmt ", 8);
        {
            uint32_t v = 16u;
            memcpy(hdr + 16, &v, 4);
        }
        {
            uint16_t v = 1u;      /* PCM */
            memcpy(hdr + 20, &v, 2);
        }
        {
            uint16_t v = 1u;      /* mono */
            memcpy(hdr + 22, &v, 2);
        }
        {
            uint32_t v = PION1_OUT_HZ;
            memcpy(hdr + 24, &v, 4);
        }
        {
            uint32_t v = PION1_OUT_HZ * 2u;
            memcpy(hdr + 28, &v, 4);
        }
        {
            uint16_t v = 2u;      /* block align */
            memcpy(hdr + 32, &v, 2);
        }
        {
            uint16_t v = 16u;     /* bits per sample */
            memcpy(hdr + 34, &v, 2);
        }
        memcpy(hdr + 36, "data", 4);
        memcpy(hdr + 40, &data_bytes, 4);

        fwrite(hdr, 1, sizeof(hdr), f);
        fwrite(pcm, 2, nsamples, f);
    }
    fclose(f);

    printf("wrote %s: %u words, %u samples at %u Hz, %.1f Hz tone%s\n",
           out_path, words_total, nsamples, PION1_OUT_HZ, freq,
           fm_depth ? " with FM" : "");

    free(words);
    free(pcm);
    return 0;
}
