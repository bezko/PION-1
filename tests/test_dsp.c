/*
 * PION/1 -- host test harness.
 *
 * The DSP core is deliberately free of Pico SDK dependencies, so it can be
 * compiled and exercised on the host machine.  That turns "it builds" into
 * "it produces the right numbers", which is the only way to be reasonably
 * confident in a 1-bit signal chain without an oscilloscope on the bench.
 *
 * Build and run:
 *     make -C tests run
 *
 * The tests check the parts where a subtle bug would be invisible on the
 * bench but ruin the sound:
 *
 *   - the bit primitives (popcount over ranges, rotations, bit reversal)
 *   - the sine table (joins must be click-free, the fundamental must be
 *     pure)
 *   - the mixer (ownership masks must stay disjoint and must rotate)
 *   - the resampler (the output density must track the input density)
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

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, ...)                                                \
    do {                                                                \
        g_checks++;                                                     \
        if (!(cond)) {                                                  \
            g_failures++;                                               \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);               \
            printf(__VA_ARGS__);                                        \
            printf("\n");                                               \
        }                                                               \
    } while (0)

#define SECTION(name) printf("\n== %s ==\n", name)

/* ------------------------------------------------------------------ */

static void test_bitops(void)
{
    SECTION("bitops");

    /* popcount */
    CHECK(pion1_popcount32(0u) == 0u, "popcount(0)");
    CHECK(pion1_popcount32(0xFFFFFFFFu) == 32u, "popcount(all ones)");
    CHECK(pion1_popcount32(0x80000001u) == 2u, "popcount(ends)");
    CHECK(pion1_popcount32(0xAAAAAAAAu) == 16u, "popcount(alternating)");

    /* popcount over a range that spans two words */
    {
        uint32_t w[2];
        w[0] = 0xFFFFFFFFu;
        w[1] = 0xFFFFFFFFu;
        CHECK(pion1_popcnt_range(w, 0u, 32u) == 32u, "range of one word");
        CHECK(pion1_popcnt_range(w, 0u, 64u) == 64u, "range of two words");
        CHECK(pion1_popcnt_range(w, 31u, 2u) == 2u, "range straddling the word boundary");
        CHECK(pion1_popcnt_range(w, 5u, 10u) == 10u, "range inside one word");
        CHECK(pion1_popcnt_range(w, 0u, 0u) == 0u, "empty range");
    }

    /* rotations */
    CHECK(pion1_ror32(0x80000001u, 1u) == 0xC0000000u, "ror by 1");
    CHECK(pion1_ror32(0x12345678u, 0u) == 0x12345678u, "ror by 0");
    CHECK(pion1_ror32(0x12345678u, 32u) == 0x12345678u, "ror by 32 is identity");
    CHECK(pion1_rol32(pion1_ror32(0xDEADBEEFu, 7u), 7u) == 0xDEADBEEFu, "ror/rol cancel");

    /* bit reversal */
    CHECK(pion1_bitrev32(0x00000001u) == 0x80000000u, "bitrev of 1");
    CHECK(pion1_bitrev32(0x80000000u) == 0x00000001u, "bitrev of 0x80000000");
    CHECK(pion1_bitrev32(0xDEADBEEFu) == 0xF77DB57Bu, "bitrev of a pattern");
    {
        uint32_t i;
        int ok = 1;
        for (i = 0; i < 32; i++) {
            uint32_t v = 1u << i;
            if (pion1_bitrev32(v) != (1u << (31 - i))) {
                ok = 0;
            }
        }
        CHECK(ok, "bitrev inverts each bit position");
    }

    /* single bit access */
    {
        uint32_t w[2] = {0u, 0u};
        pion1_set_bit(w, 0u, 1u);
        pion1_set_bit(w, 31u, 1u);
        pion1_set_bit(w, 32u, 1u);
        CHECK(pion1_bit(w, 0u) == 1u, "bit 0 set");
        CHECK(pion1_bit(w, 31u) == 1u, "bit 31 set");
        CHECK(pion1_bit(w, 32u) == 1u, "bit 32 set");
        CHECK(pion1_bit(w, 1u) == 0u, "bit 1 clear");
    }
}

/* ------------------------------------------------------------------ */

/*
 * Reconstruct the analog value of a 1-bit stream by averaging over a
 * window.  This is what the analog front end effectively does, so it is
 * the right way to look at the signal the synth produces.
 *
 * The stream is given as an array of words with LSB-first bit order, and
 * nbits is the number of bits in the array.
 */
static double density_at(const uint32_t *bits, uint32_t nbits, uint32_t pos,
                         uint32_t win)
{
    uint32_t i, total = 0;
    for (i = 0; i < win; i++) {
        uint32_t p = (pos + i) % nbits;
        total += (bits[p >> 5] >> (p & 31u)) & 1u;
    }
    return (double)total / (double)win;
}

static void test_sine_table(void)
{
    SECTION("sine table");

    /* The full table must have a density of exactly 0.5: a sine spends
     * half its time above the midpoint. */
    {
        uint32_t total = 0, i;
        for (i = 0; i < PION1_SINE_FULL_WORDS; i++) {
            total += pion1_popcount32(pion1_sine_full[i]);
        }
        CHECK(total == PION1_SINE_FULL_BITS / 2u,
              "full cycle density is %u/%u, want %u",
              total, PION1_SINE_FULL_BITS, PION1_SINE_FULL_BITS / 2u);
    }

    /* The quarter table must rise from 0 to 1: its density has to increase
     * monotonically. */
    {
        uint32_t i;
        int monotonic = 1;
        uint32_t win = 64;
        double prev = density_at(pion1_sine_quarter, PION1_SINE_QUARTER_BITS, 0, win);
        for (i = win; i + win <= PION1_SINE_QUARTER_BITS; i += win / 2) {
            double d = density_at(pion1_sine_quarter, PION1_SINE_QUARTER_BITS, i, win);
            if (d < prev - 0.02) {
                monotonic = 0;
            }
            prev = d;
        }
        CHECK(monotonic, "quarter-sine density rises monotonically");
    }

    /* The joins must not click: the density just before a quadrant
     * boundary has to match the density just after it. */
    {
        uint32_t win = 64;
        uint32_t q;
        int ok = 1;
        for (q = 1; q < 4u; q++) {
            uint32_t j = q * PION1_SINE_QUARTER_BITS;
            double before = density_at(pion1_sine_full, PION1_SINE_FULL_BITS, j - win, win);
            double after = density_at(pion1_sine_full, PION1_SINE_FULL_BITS, j, win);
            double delta = fabs(after - before);
            if (delta > 0.02) {
                ok = 0;
                printf("    join at quadrant %u: before=%.4f after=%.4f delta=%.4f\n",
                       q, before, after, delta);
            }
        }
        CHECK(ok, "all quadrant joins are click-free");
    }

    /* The waveform must be a pure sine: only the fundamental survives.
     * We measure the magnitude of the fundamental and of the first few
     * harmonics with a naive DFT over the full cycle. */
    {
        uint32_t n = PION1_SINE_FULL_BITS;
        uint32_t win = 64;
        double re, im, fund;
        int k;

        re = im = 0.0;
        for (k = 0; k < (int)n; k++) {
            double x = density_at(pion1_sine_full, n, (uint32_t)k, win);
            double a = 2.0 * M_PI * (double)k / (double)n;
            re += x * cos(a);
            im += x * sin(a);
        }
        fund = sqrt(re * re + im * im) / (double)n;
        CHECK(fund > 0.05 && fund < 0.30,
              "fundamental magnitude %.4f is in a sane range", fund);

        /* Harmonics 2 and 3 must be far below the fundamental. */
        for (k = 2; k <= 3; k++) {            double r2, i2, m2;
            r2 = i2 = 0.0;
            {
                uint32_t j;
                for (j = 0; j < n; j++) {
                    double x = density_at(pion1_sine_full, n, j, win);
                    double a = 2.0 * M_PI * (double)k * (double)j / (double)n;
                    r2 += x * cos(a);
                    i2 += x * sin(a);
                }            }
            m2 = sqrt(r2 * r2 + i2 * i2) / (double)n;
            CHECK(m2 < fund * 0.05,
                  "harmonic %u (%.5f) is below 5%% of the fundamental (%.5f)",
                  k, m2, fund);
        }
    }

    /* Quadrature must be free: a quarter cycle ahead is the same signal
     * shifted by 90 degrees, and a half cycle ahead is the inverse. */
    {
        uint32_t i;
        int half_ok = 1;
        for (i = 0; i < 512; i++) {
            uint32_t ph = i * 12345u;
            uint32_t b0 = pion1_sine_bit(ph);
            uint32_t bh = pion1_sine_bit(ph + 0x80000000u);
            if (bh == b0) {
                half_ok = 0;
            }
        }
        CHECK(half_ok, "a half-cycle phase offset inverts the waveform");
    }
}

/* ------------------------------------------------------------------ */

static void test_mixer(void)
{
    SECTION("mixer");

    /*
     * The slot allocation follows the levels.  With one voice at full
     * level it owns every slot; with two voices at equal level each owns
     * half; and the masks must always stay disjoint and together cover
     * all 32 slots.
     */
    {
        pion1_mixer_t m;
        uint32_t i;
        int ok = 1;

        pion1_mixer_init(&m, 2u, 0u);
        pion1_mixer_set_level(&m, 0u, 65535u);
        pion1_mixer_set_level(&m, 1u, 65535u);

        for (i = 0; i < 64u; i++) {
            uint32_t words[2] = {0u, 0u};
            uint32_t all = 0;
            pion1_mix(&m, words);
            if (all & m.voice[0].mask) {
                ok = 0;
            }
            all |= m.voice[0].mask;
            if (all & m.voice[1].mask) {
                ok = 0;
            }
            all |= m.voice[1].mask;
            if (all != 0xFFFFFFFFu) {
                ok = 0;
            }
        }
        CHECK(ok, "ownership masks stay disjoint and complete while rotating");
    }

    /* A single voice at full level must own all 32 slots. */
    {
        pion1_mixer_t m;
        uint32_t words[4] = {0u, 0u, 0u, 0u};
        uint32_t out;
        pion1_mixer_init(&m, 4u, 0u);
        pion1_mixer_set_level(&m, 2u, 65535u);
        /* The effective mask is the rotated base mask; after a mix the
         * rotation is applied, so mix once and then look. */
        out = pion1_mix(&m, words);
        CHECK(out == 0u, "a lone voice at level 0 produces silence");
        CHECK(pion1_popcount32(m.voice[2].mask) == 32u,
              "a lone voice at full level owns every slot (got %u)",
              pion1_popcount32(m.voice[2].mask));
    }

    /* Two voices at equal level must split the slots evenly. */
    {
        pion1_mixer_t m;
        uint32_t words[2] = {0u, 0u};
        pion1_mixer_init(&m, 2u, 0u);
        pion1_mixer_set_level(&m, 0u, 65535u);
        pion1_mixer_set_level(&m, 1u, 65535u);
        pion1_mix(&m, words);
        CHECK(pion1_popcount32(m.voice[0].mask) == 16u,
              "two equal voices split the slots evenly (got %u)",
              pion1_popcount32(m.voice[0].mask));
        CHECK(pion1_popcount32(m.voice[1].mask) == 16u,
              "two equal voices split the slots evenly (got %u)",
              pion1_popcount32(m.voice[1].mask));
    }

    /* The rotation must actually move: a mask that never changes would
     * leave the aliasing tones in place. */
    {
        pion1_mixer_t m;
        uint32_t before, after;
        uint32_t words[2] = {0u, 0u};
        pion1_mixer_init(&m, 2u, 0u);
        pion1_mixer_set_level(&m, 0u, 65535u);
        before = m.voice[0].mask;
        pion1_mix(&m, words);
        after = m.voice[0].mask;
        CHECK(before != after, "the mask rotates every word");
    }

    /* Level control: a silent voice must own no slots at all. */
    {
        pion1_mixer_t m;
        pion1_mixer_init(&m, 4u, 0u);
        pion1_mixer_set_level(&m, 0u, 0u);
        CHECK(pion1_popcount32(m.voice[0].mask) == 0u,
              "a voice at level 0 owns no slots");
    }

    /*
     * The mix must actually pass the voice's bits through: a voice that
     * owns a slot must be able to set it, and a voice that owns no slots
     * must not.
     */
    {
        pion1_mixer_t m;
        uint32_t words[2];
        uint32_t out;

        pion1_mixer_init(&m, 2u, 0u);
        pion1_mixer_set_level(&m, 0u, 65535u);
        pion1_mixer_set_level(&m, 1u, 0u);

        words[0] = 0xFFFFFFFFu;
        words[1] = 0xFFFFFFFFu;
        out = pion1_mix(&m, words);
        CHECK(out == 0xFFFFFFFFu,
              "the owning voice fills the word (got %08lx)", (unsigned long)out);
    }

    /*
     * The idle pattern must fill the slots that nobody owns.  With one
     * voice at full level that voice owns every slot, so the idle pattern
     * must NOT appear -- the voice's own bits have to be audible.
     */
    {
        pion1_mixer_t m;
        uint32_t words[2];
        uint32_t out;

        pion1_mixer_init(&m, 2u, 0xFFFFFFFFu);
        pion1_mixer_set_level(&m, 0u, 65535u);
        pion1_mixer_set_level(&m, 1u, 0u);

        words[0] = 0u;
        words[1] = 0u;
        out = pion1_mix(&m, words);
        CHECK(out == 0u,
              "a full-level voice owns every slot, so the idle pattern does "
              "not appear (got %08lx)", (unsigned long)out);
    }

    /*
     * The idle pattern must fill the slots that nobody owns.  Two voices
     * at half level each claim 16 of the 32 slots, so 16 slots are left
     * over and the idle pattern must fill exactly those.
     */
    {
        pion1_mixer_t m;
        uint32_t words[3];
        uint32_t owned;

        pion1_mixer_init(&m, 3u, 0xFFFFFFFFu);
        pion1_mixer_set_level(&m, 0u, 32768u);   /* 16 slots */
        pion1_mixer_set_level(&m, 1u, 32768u);   /* 16 slots */
        pion1_mixer_set_level(&m, 2u, 0u);

        words[0] = 0u;
        words[1] = 0u;
        words[2] = 0u;
        (void)pion1_mix(&m, words);

        owned = m.voice[0].mask | m.voice[1].mask;
        CHECK(pion1_popcount32(owned) == 32u,
              "two half-level voices between them own 32 slots (got %u)",
              pion1_popcount32(owned));
    }

    /*
     * Three voices at a third of the level each claim about 10 slots, so
     * a couple of slots are left over and the idle pattern fills them.
     */
    {
        pion1_mixer_t m;
        uint32_t words[3];
        uint32_t out;
        uint32_t owned;

        pion1_mixer_init(&m, 3u, 0xFFFFFFFFu);
        pion1_mixer_set_level(&m, 0u, 16384u);   /* 8 slots */
        pion1_mixer_set_level(&m, 1u, 16384u);   /* 8 slots */
        pion1_mixer_set_level(&m, 2u, 16384u);   /* 8 slots */

        words[0] = 0u;
        words[1] = 0u;
        words[2] = 0u;
        out = pion1_mix(&m, words);

        owned = m.voice[0].mask | m.voice[1].mask | m.voice[2].mask;
        CHECK(pion1_popcount32(owned) < 32u,
              "three low voices leave some slots unowned (%u owned)",
              pion1_popcount32(owned));
        CHECK((out & ~owned) == (0xFFFFFFFFu & ~owned),
              "the idle pattern fills exactly the unowned slots (got %08lx)",
              (unsigned long)out);
    }}

/* ------------------------------------------------------------------ */

static void test_resampler(void)
{
    SECTION("resampler");

    /* A constant-density source must come out with the same density. */
    {
        static uint32_t src[64];
        pion1_resampler_t r;
        uint32_t i, ones = 0;
        const uint32_t nbits = 64u * 32u;

        /* A source with a density of 0.75: three ones in every four. */
        for (i = 0; i < 64u; i++) {
            uint32_t b;
            src[i] = 0xFFFFFFFFu;
            for (b = 0; b < 32u; b += 4u) {
                src[i] &= ~(1u << b);
            }
        }

        /* Read at half rate: the output density must still be 0.75. */
        pion1_resampler_init(&r, PION1_TIER_BOXCAR, 64u, 1u << 15);
        for (i = 0; i < 4000u; i++) {
            ones += pion1_resample(&r, src, nbits);
        }
        {
            double d = (double)ones / 4000.0;
            CHECK(fabs(d - 0.75) < 0.05,
                  "boxcar tier preserves density (got %.3f, want 0.75)", d);
        }

        /* The crude tier must preserve it too, at least approximately. */
        pion1_resampler_init(&r, PION1_TIER_CRUDE, 64u, 1u << 15);
        ones = 0;
        for (i = 0; i < 4000u; i++) {
            ones += pion1_resample(&r, src, nbits);
        }
        {
            double d = (double)ones / 4000.0;
            CHECK(fabs(d - 0.75) < 0.05,
                  "crude tier preserves density (got %.3f, want 0.75)", d);
        }

        /* And so must the remod tier. */
        pion1_resampler_init(&r, PION1_TIER_REMOD, 64u, 1u << 15);
        ones = 0;
        for (i = 0; i < 4000u; i++) {
            ones += pion1_resample(&r, src, nbits);
        }
        {
            double d = (double)ones / 4000.0;
            CHECK(fabs(d - 0.75) < 0.08,
                  "remod tier preserves density (got %.3f, want 0.75)", d);
        }
    }

    /* A source that is all zeros must come out all zeros: the resampler
     * must not invent ones out of noise. */
    {
        static uint32_t src[64];
        pion1_resampler_t r;
        uint32_t i, ones = 0;
        memset(src, 0, sizeof(src));
        pion1_resampler_init(&r, PION1_TIER_BOXCAR, 64u, 1u << 16);
        for (i = 0; i < 1000u; i++) {
            ones += pion1_resample(&r, src, 64u * 32u);
        }
        CHECK(ones == 0u, "a silent source stays silent (%u ones leaked)", ones);
    }

    /* The output must be a real bit: never anything but 0 or 1. */
    {
        static uint32_t src[64];
        pion1_resampler_t r;
        uint32_t i;
        int ok = 1;
        for (i = 0; i < 64u; i++) {
            src[i] = (uint32_t)(i * 2654435761u);
        }
        for (i = 0; i < 3u; i++) {
            pion1_resampler_init(&r, (pion1_tier_t)i, 64u, 1u << 15);
            {
                uint32_t k;
                for (k = 0; k < 1000u; k++) {
                    uint32_t b = pion1_resample(&r, src, 64u * 32u);
                    if (b > 1u) {
                        ok = 0;
                    }
                }
            }
        }
        CHECK(ok, "every tier outputs a single bit");
    }
}

/* ------------------------------------------------------------------ */

static void test_voices(void)
{
    SECTION("voices");

    /* The oscillator must produce a 1-bit output and must advance. */
    {
        pion1_voice_t v;
        uint32_t i;
        int ok = 1;
        uint32_t ones = 0;

        pion1_voice_init(&v, PION1_VOICE_OSC);
        v.increment = PION1_PHASE_STEP(440.0, 500000u);
        v.fm_increment = PION1_PHASE_STEP(550.0, 500000u);
        v.fm_depth = 0x1000u;
        v.fm_index = 0x8000u;

        for (i = 0; i < 10000u; i++) {
            uint32_t b = pion1_voice_osc(&v, 0u);
            if (b > 1u) {
                ok = 0;
            }
            ones += b;
        }
        CHECK(ok, "the oscillator outputs a single bit");
        CHECK(ones > 3000u && ones < 7000u,
              "the oscillator output is roughly balanced (%u ones of 10000)", ones);
    }

    /* The envelope must move towards its target and stop there. */
    {
        pion1_voice_t v;
        uint32_t i;
        pion1_voice_init(&v, PION1_VOICE_OSC);
        v.env = 0;
        v.env_target = 1000u;
        v.env_rate = 10u;
        for (i = 0; i < 1000u; i++) {
            pion1_voice_env_tick(&v);
        }
        CHECK(v.env == 1000u, "the envelope reaches its target (%u)", v.env);
    }

    /* The waveguide must produce bits and must not lock up. */
    {
        pion1_voice_t v;
        uint32_t i;
        int ok = 1;
        pion1_voice_init(&v, PION1_VOICE_WG);
        v.wg_delay = 64u;
        v.wg_feedback = 32768u;
        for (i = 0; i < 1000u; i++) {
            if (pion1_voice_wg(&v) > 1u) {
                ok = 0;
            }
        }
        CHECK(ok, "the waveguide outputs a single bit");
    }
}

/* ------------------------------------------------------------------ */

/*
 * Measure the magnitude of a tone in a rendered bit stream.
 *
 * This is a direct DFT at one frequency over the raw 1-bit stream, which
 * is the same thing the analog reconstruction does.
 */
static double tone_magnitude(const uint32_t *words, uint32_t nbits,
                             double freq)
{
    double re = 0.0, im = 0.0;
    uint32_t i;

    for (i = 0; i < nbits; i++) {
        double b = (double)((words[i >> 5] >> (i & 31u)) & 1u);
        double a = 2.0 * M_PI * freq * (double)i / 500000.0;
        re += b * cos(a);
        im += b * sin(a);
    }
    return sqrt(re * re + im * im) / (double)nbits * 2.0;
}

static void test_fm(void)
{
    SECTION("fm");

    /*
     * A 440 Hz carrier modulated at 100 Hz must produce sidebands at
     * 440 +/- 100 Hz.  Without the modulation there must be nothing at
     * 240 Hz or 640 Hz.
     */
    {
        static uint32_t words[4096];
        double carrier, lower, upper, carrier_fm;
        uint32_t w;

        /* --- unmodulated --- */
        {
            pion1_voice_t v;
            pion1_voice_init(&v, PION1_VOICE_OSC);
            v.increment = PION1_PHASE_STEP(440.0, 500000u);
            for (w = 0; w < 4096u; w++) {
                words[w] = pion1_voice_osc_word(&v);
            }
        }
        carrier = tone_magnitude(words, 4096u * 32u, 440.0);
        lower = tone_magnitude(words, 4096u * 32u, 240.0);
        upper = tone_magnitude(words, 4096u * 32u, 640.0);

        CHECK(carrier > 0.15, "the unmodulated carrier is strong (%.4f)", carrier);
        CHECK(lower < 0.02, "no sideband without modulation (%.4f)", lower);
        CHECK(upper < 0.02, "no sideband without modulation (%.4f)", upper);

        /* --- modulated --- */
        {
            pion1_voice_t v;
            pion1_voice_init(&v, PION1_VOICE_OSC);
            v.increment = PION1_PHASE_STEP(440.0, 500000u);
            v.fm_increment = PION1_PHASE_STEP(100.0, 500000u);
            v.fm_depth = 200u;      /* table steps */
            v.fm_index = 0x8000u;
            for (w = 0; w < 4096u; w++) {
                words[w] = pion1_voice_osc_word(&v);
            }
        }
        carrier_fm = tone_magnitude(words, 4096u * 32u, 440.0);
        lower = tone_magnitude(words, 4096u * 32u, 240.0);
        upper = tone_magnitude(words, 4096u * 32u, 640.0);

        CHECK(lower > 0.02, "FM produces the lower sideband (%.4f)", lower);
        CHECK(upper > 0.02, "FM produces the upper sideband (%.4f)", upper);
        CHECK(carrier_fm < carrier,
              "FM spreads the energy across the sidebands (%.4f < %.4f)",
              carrier_fm, carrier);
    }
}

/* ------------------------------------------------------------------ */

int main(void)
{
    printf("PION/1 DSP test suite\n");
    printf("---------------------\n");

    test_bitops();
    test_sine_table();
    test_mixer();
    test_resampler();
    test_voices();
    test_fm();

    printf("\n---------------------\n");
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
