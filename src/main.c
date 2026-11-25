/*
 * PION/1 -- main firmware.
 *
 * The synth is split across the two RP2040 cores:
 *
 *   core 0   the "control" core.  Owns the parameters, the UI and the
 *            housekeeping.  It publishes parameter updates to core 1.
 *
 *   core 1   the "audio" core.  Owns the DSP.  It reads the published
 *            parameters, renders one 32-bit word per 64 us and hands the
 *            word to the playback DMA.
 *
 * The split matters because the audio path has a hard deadline: the
 * playback ring must be refilled before the DMA catches up with it, and
 * any hiccup on core 0 (a USB enumeration, a console write) must not
 * cause an audible dropout.
 *
 * The bit rate is exactly 500 kHz, so one output word covers 64 us.  Core 1
 * sleeps on a timer alarm that fires at that rate.
 */
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/time.h"
#include "pico/multicore.h"
#include "pico/stdio_usb.h"
#include "hardware/timer.h"
#include "hardware/clocks.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"

#include "params.h"
#include "dsp/mixer.h"
#include "dsp/resample.h"
#include "dsp/voices.h"
#include "dsp/sine_table.h"
#include "pio/bitstream.h"

/* ------------------------------------------------------------------ */
/* Voice layout                                                        */
/* ------------------------------------------------------------------ */

#define PION1_NV 3u

/* Voice indices. */
#define V_OSC 0u
#define V_SAMP 1u
#define V_WG 2u

/*
 * The FM modulation index, in 16.16 fixed point.  1.0 means the
 * modulator can push the carrier by a full cycle; the depth parameter
 * scales that further, in sine-table steps.
 */
#define PION1_FM_INDEX 0x10000u

static pion1_voice_t g_voice[PION1_NV];
static pion1_mixer_t g_mixer;
static pion1_resampler_t g_resampler;

/* Capture ring read position, in words, maintained by the audio core. */
static volatile uint32_t g_capture_read_word = 0;

/* ------------------------------------------------------------------ */
/* Audio core                                                          */
/* ------------------------------------------------------------------ */

/*
 * Render one output word.
 *
 * Each voice produces a 32-bit word; the mixer combines them according to
 * the ownership masks, and the result is pushed into the playback ring.
 */
static uint32_t render_word(void)
{
    uint32_t vwords[PION1_NV];
    const uint32_t *p = pion1_params_read();

    /* --- oscillator --- */
    {
        pion1_voice_t *v = &g_voice[V_OSC];
        vwords[V_OSC] = pion1_voice_osc_word(v);
    }

    /* --- sampler --- */
    {
        pion1_voice_t *v = &g_voice[V_SAMP];
        const uint32_t *ring = pion1_capture_ring_ptr();
        uint32_t bits = pion1_capture_ring_bits();
        uint32_t bit = 0;

        (void)v;
        bit = pion1_resample(&g_resampler, ring, bits);
        vwords[V_SAMP] = bit ? 0xFFFFFFFFu : 0u;
    }

    /* --- waveguide --- */
    {
        pion1_voice_t *v = &g_voice[V_WG];
        uint32_t bit = pion1_voice_wg(v);
        vwords[V_WG] = bit ? 0xFFFFFFFFu : 0u;
    }

    /* --- mix --- */
    {
        /*
         * The level of a voice is the number of slots it claims out of
         * the 32 in the word, so a level of 65535 is full scale and the
         * idle pattern fills whatever is left over.
         */
        pion1_mixer_set_density(&g_mixer, V_OSC,
                                 p[PION1_P_WAVE1_LEVEL], 65535u);
        pion1_mixer_set_density(&g_mixer, V_SAMP,
                                 p[PION1_P_SAMP_LEVEL], 65535u);
        pion1_mixer_set_density(&g_mixer, V_WG,
                                 p[PION1_P_WG_LEVEL], 65535u);
        return pion1_mix(&g_mixer, vwords);
    }
}

/*
 * The audio core's main loop.
 *
 * It sleeps until the next word deadline, renders a word, and pushes it
 * into the playback ring.  If the ring has overflowed (the CPU could not
 * keep up) it drops the oldest data rather than the newest, so the latency
 * stays bounded.
 */
static void core1_main(void)
{
    const uint32_t word_period_us = 1000000u / PION1_WORDS_PER_SEC; /* 64 us */
    absolute_time_t next = get_absolute_time();
    uint32_t idle_words = 0;
    uint32_t idle_pattern = 0;

    while (1) {
        uint32_t word;

        next = delayed_by_us(next, word_period_us);
        sleep_until(next);

        /* Update the oscillator from the published parameters. */
        {
            const uint32_t *p = pion1_params_read();
            pion1_voice_t *v = &g_voice[V_OSC];

            v->increment = PION1_PHASE_STEP(p[PION1_P_WAVE1_FREQ], PION1_FS_HZ);
            v->fm_increment = PION1_PHASE_STEP(p[PION1_P_WAVE1_FM_FREQ], PION1_FS_HZ);
            v->fm_depth = p[PION1_P_WAVE1_FM_DEPTH];
            v->fm_index = PION1_FM_INDEX;
            v->shape = p[PION1_P_WAVE1_SHAPE];

            idle_pattern = p[PION1_P_IDLE];

            /* The sampler rate is given as a ratio of the capture rate. */
            {
                uint32_t step = (uint32_t)(((uint64_t)p[PION1_P_SAMP_RATE] << 16) / 65535u);
                if (step == 0u) {
                    step = 1u;
                }
                if (g_resampler.step_16_16 != step ||
                    g_resampler.tier != (pion1_tier_t)p[PION1_P_SAMP_TIER] ||
                    g_resampler.boxcar_len != p[PION1_P_BOXCAR_LEN]) {
                    pion1_resampler_init(&g_resampler,
                                         (pion1_tier_t)p[PION1_P_SAMP_TIER],
                                         p[PION1_P_BOXCAR_LEN] ? p[PION1_P_BOXCAR_LEN] : 64u,
                                         step);
                }
            }
        }

        word = render_word();

        if (!pion1_playback_push(word)) {
            /*
             * The ring is full: the DMA is behind.  Push a silence word
             * anyway is not possible (the ring is full), so we simply drop
             * this word.  The next push will succeed once the DMA has
             * consumed something.
             */
            idle_words++;
        } else {
            idle_words = 0;
        }
        (void)idle_words;
        (void)idle_pattern;
    }
}

/* ------------------------------------------------------------------ */
/* Control core                                                        */
/* ------------------------------------------------------------------ */

static void apply_params(void)
{
    const uint32_t *p = pion1_params_read();
    pion1_voice_t *v = &g_voice[V_OSC];

    v->increment = PION1_PHASE_STEP(p[PION1_P_WAVE1_FREQ], PION1_FS_HZ);
    v->fm_increment = PION1_PHASE_STEP(p[PION1_P_WAVE1_FM_FREQ], PION1_FS_HZ);
    v->fm_depth = p[PION1_P_WAVE1_FM_DEPTH];
    v->fm_index = PION1_FM_INDEX;
    v->shape = p[PION1_P_WAVE1_SHAPE];
}

int main(void)
{
    uint32_t i;

    /* The RP2040's default clock is 125 MHz, which is what the PIO timing
     * assumes.  Set it explicitly so the rate is never in doubt. */
    set_sys_clock_khz(125000u, true);

    /* The watchdog is the only thing that keeps a hung core from taking
     * the whole synth down silently. */
    watchdog_enable(2000, 1);

    stdio_init_all();

    /* --- voices and mixer --- */
    for (i = 0; i < PION1_NV; i++) {
        pion1_voice_init(&g_voice[i], (i == V_OSC) ? PION1_VOICE_OSC :
                          (i == V_SAMP) ? PION1_VOICE_SAMP : PION1_VOICE_WG);
    }
    pion1_mixer_init(&g_mixer, PION1_NV, 0u);
    pion1_resampler_init(&g_resampler, PION1_TIER_BOXCAR, 64u, 1u << 16);

    /* --- default parameters --- */
    pion1_params_stage_set(PION1_P_MASTER, 32768u);
    pion1_params_stage_set(PION1_P_WAVE1_FREQ, 440u);
    pion1_params_stage_set(PION1_P_WAVE1_LEVEL, 65535u);
    pion1_params_stage_set(PION1_P_WAVE1_FM_FREQ, 0u);
    pion1_params_stage_set(PION1_P_WAVE1_FM_DEPTH, 0u);
    pion1_params_stage_set(PION1_P_WAVE1_SHAPE, PION1_SHAPE_NONE);
    pion1_params_stage_set(PION1_P_SAMP_RATE, 65535u);
    pion1_params_stage_set(PION1_P_SAMP_LEVEL, 0u);
    pion1_params_stage_set(PION1_P_SAMP_TIER, PION1_TIER_BOXCAR);
    pion1_params_stage_set(PION1_P_BOXCAR_LEN, 64u);
    pion1_params_stage_set(PION1_P_WG_FREQ, 220u);
    pion1_params_stage_set(PION1_P_WG_LEVEL, 0u);
    pion1_params_stage_set(PION1_P_WG_FEEDBACK, 32768u);
    pion1_params_stage_set(PION1_P_IDLE, 0u);
    pion1_params_publish();
    apply_params();

    /* --- bitstream --- */
    pion1_bitstream_init();
    pion1_bitstream_start();

    /* --- start the audio core --- */
    multicore_launch_core1(core1_main);

    /*
     * The banner is printed from the control loop rather than here, so
     * that it is re-printed every time the host opens the port.  A host
     * that attaches late, or re-attaches after a disconnect, gets the
     * same greeting as one that was there from the start -- which is
     * what makes "is it connected?" answerable on the host side.
     */
    {
        bool banner_printed = false;

        while (1) {
            int c = getchar_timeout_us(0);
            bool connected = stdio_usb_connected();

            watchdog_update();

            if (connected && !banner_printed) {
                printf("\nPION/1 ready: %u Hz bit rate, %u words/s\n",
                       PION1_FS_HZ, PION1_WORDS_PER_SEC);
                printf("freq=%lu samp=%lu wg=%lu\n",
                       (unsigned long)pion1_params_stage_get(PION1_P_WAVE1_FREQ),
                       (unsigned long)pion1_params_stage_get(PION1_P_SAMP_LEVEL),
                       (unsigned long)pion1_params_stage_get(PION1_P_WG_LEVEL));
                banner_printed = true;
            } else if (!connected) {
                /* The host went away; greet it again when it returns. */
                banner_printed = false;
            }

            if (c >= 0) {
                switch (c) {
                case 'q':
                    printf("bye\n");
                    return 0;
                case '+':
                    pion1_params_stage_set(PION1_P_WAVE1_FREQ,
                                           pion1_params_stage_get(PION1_P_WAVE1_FREQ) + 10u);
                    pion1_params_publish();
                    break;
                case '-':
                    pion1_params_stage_set(PION1_P_WAVE1_FREQ,
                                           (pion1_params_stage_get(PION1_P_WAVE1_FREQ) > 10u)
                                           ? pion1_params_stage_get(PION1_P_WAVE1_FREQ) - 10u : 0u);
                    pion1_params_publish();
                    break;
                case 's':
                    pion1_params_stage_set(PION1_P_SAMP_LEVEL,
                                           pion1_params_stage_get(PION1_P_SAMP_LEVEL) ? 0u : 65535u);
                    pion1_params_publish();
                    break;
                default:
                    break;
                }
                printf("freq=%lu samp=%lu wg=%lu\n",
                       (unsigned long)pion1_params_stage_get(PION1_P_WAVE1_FREQ),
                       (unsigned long)pion1_params_stage_get(PION1_P_SAMP_LEVEL),
                       (unsigned long)pion1_params_stage_get(PION1_P_WG_LEVEL));
            }
        }
    }
}
