/*
 * PION/1 -- voices.
 *
 * Every voice turns a set of parameters into one 32-bit word per output
 * word.  The output word holds 32 bits of a 1-bit stream at 500 kHz, i.e.
 * the voice's contribution to the mixer for that 64 us slice of time.
 *
 * Voice types
 * -----------
 *   PION1_VOICE_OSC   -- sine oscillator read from the 1-bit quarter-sine
 *                        table, with optional FM and optional wave shaping.
 *   PION1_VOICE_SAMP  -- resampled playback of the captured bit stream.
 *   PION1_VOICE_WG    -- Karplus-Strong style waveguide in the bit domain.
 *
 * All voices are driven by a 32-bit phase accumulator at the *output* word
 * rate (15.625 kHz for the PIO bit rate of 500 kHz), except the sampler,
 * which reads the capture ring at its own rate.
 */
#ifndef PION1_VOICES_H
#define PION1_VOICES_H

#include <stdint.h>
#include "resample.h"

typedef enum {
    PION1_VOICE_OSC = 0,
    PION1_VOICE_SAMP = 1,
    PION1_VOICE_WG = 2
} pion1_voice_type_t;

/* Wave-shaping modes applied to the oscillator output. */
typedef enum {
    PION1_SHAPE_NONE = 0,
    PION1_SHAPE_MAJORITY = 1,   /* majority gate over a short window */
    PION1_SHAPE_COMB = 2        /* comb filter over the bit stream */
} pion1_shape_t;

typedef struct {
    /* --- oscillator --- */
    uint32_t phase;        /* 32-bit phase accumulator */
    uint32_t increment;    /* phase step per output word */
    uint32_t fm_phase;     /* modulator phase */
    uint32_t fm_increment; /* modulator step */
    uint32_t fm_depth;     /* peak modulation depth, in sine table steps */
    uint32_t fm_index;     /* modulation index */

    /* --- envelope --- */
    uint32_t env;          /* current level, 0..65535 */
    uint32_t env_target;
    uint32_t env_rate;     /* units of env moved per output word */

    /* --- shaping --- */
    uint32_t shape;
    uint32_t shape_param;  /* window length (majority) / comb delay (comb) */
    uint32_t shape_hist;   /* history for the comb */
    uint32_t shape_hist2;

    /* --- waveguide --- */
    uint32_t wg_phase;
    uint32_t wg_delay;     /* delay line length in bits */
    uint32_t wg_pos;
    uint32_t wg_feedback;  /* 0..65535, scales the feedback */
    uint32_t wg_buf[256];  /* bit delay line, LSB-first */
} pion1_voice_t;

/* Initialise a voice to a silent state. */
void pion1_voice_init(pion1_voice_t *v, pion1_voice_type_t type);

/*
 * Render one output word for an oscillator voice.
 *
 * The oscillator reads the 1-bit sine table at `phase`, applies FM, then
 * optional wave shaping.  The envelope is applied by the caller (through the
 * mixer density), not here.
 */
uint32_t pion1_voice_osc(pion1_voice_t *v, uint32_t fm_mod);

/*
 * Render one full output word (32 bits) for an oscillator voice.
 *
 * The oscillator runs at the *bit* rate (500 kHz), so a word covers 32
 * consecutive samples and the phase has to advance 32 times per word.
 * Rendering a whole word here keeps that bookkeeping in one place: the
 * caller never has to know that a "word" is 32 samples.
 *
 * The FM modulator advances in step with the carrier.
 */
uint32_t pion1_voice_osc_word(pion1_voice_t *v);

/* Render one output word for a waveguide voice. */
uint32_t pion1_voice_wg(pion1_voice_t *v);

/* Advance the envelope one step. */
void pion1_voice_env_tick(pion1_voice_t *v);

#endif /* PION1_VOICES_H */
