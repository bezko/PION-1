/*
 * PION/1 -- variable-rate reader for 1-bit source material.
 *
 * The sampler voice reads a captured 1-bit stream at an arbitrary, usually
 * fractional, rate.  Three quality tiers are provided:
 *
 *   PION1_TIER_CRUDE    -- pick the nearest source bit.  Cheap, aliases
 *                          badly, but useful for debugging and for material
 *                          that is already band-limited.
 *
 *   PION1_TIER_BOXCAR   -- average the source over a sliding window of L
 *                          bits and re-quantise the resulting *density* back
 *                          to a bit with a first-order feedback loop.  The
 *                          window kills the aliases the crude tier lets
 *                          through, and the feedback loop keeps the output a
 *                          true 1-bit stream so the rest of the synth stays
 *                          in the bit domain.
 *
 *   PION1_TIER_REMOD    -- as the boxcar tier, but the window position is
 *                          interpolated fractionally (so the decimation
 *                          phase is not quantised to whole source bits) and
 *                          the feedback accumulator keeps 16.16 resolution.
 *                          Lower quantisation noise at the cost of a slightly
 *                          wider noise band.
 *
 * All three tiers emit exactly one bit per call, so a voice is a pure
 * function of "how many source bits to advance per output bit".
 *
 * Position bookkeeping
 * --------------------
 * The resampler keeps a 16.16 source position.  The integer part advances by
 * a varying amount (floor/ceil of the rate) and the fractional part carries
 * the remainder, so the *average* rate is exact.
 */
#ifndef PION1_RESAMPLE_H
#define PION1_RESAMPLE_H

#include <stdint.h>

typedef enum {
    PION1_TIER_CRUDE = 0,
    PION1_TIER_BOXCAR = 1,
    PION1_TIER_REMOD = 2
} pion1_tier_t;

typedef struct {
    /* --- configuration --- */
    pion1_tier_t tier;
    uint32_t boxcar_len;   /* window length L, in source bits */
    uint32_t step_16_16;   /* source bits consumed per output bit, 16.16 */

    /* --- state --- */
    uint32_t acc;          /* source position, 16.16 */
    int32_t err;           /* boxcar feedback accumulator, count domain */
    int64_t err_frac;      /* remod feedback accumulator, 16.16 */
    uint32_t window_sum;   /* ones in the current boxcar window */
    uint32_t win_start;    /* absolute start of the current window */
    uint32_t out;          /* previous output bit */
    uint32_t primed;       /* window_sum is valid */
    uint32_t primed_before;/* the feedback integrator has been seeded */
} pion1_resampler_t;

/* Initialise a resampler.  step_16_16 is in source bits per output bit. */
void pion1_resampler_init(pion1_resampler_t *r, pion1_tier_t tier,
                          uint32_t boxcar_len, uint32_t step_16_16);

/* Reset the internal state, keeping the configuration. */
void pion1_resampler_reset(pion1_resampler_t *r);

/*
 * Produce one output bit.
 *
 * src is the source ring buffer holding src_len bits.  src_len must be a
 * power of two so that the wrap is a mask.  The resampler keeps its own
 * absolute position inside the ring.
 *
 * Returns the output bit (0 or 1).
 */
uint32_t pion1_resample(pion1_resampler_t *r, const uint32_t *src,
                         uint32_t src_len);

/*
 * Produce one *analog* output sample.
 *
 * This is the decimator used at the output stage: it reads the 1-bit
 * source at a fractional rate and returns the reconstructed level, in the
 * range -1.0 .. +1.0.  It is the numerical equivalent of what the analog
 * low-pass in the output stage does to the bit stream.
 *
 * The tier selects how the level is reconstructed:
 *
 *   CRUDE    the nearest source bit, as +-1.
 *   BOXCAR   the mean of the L-bit window, scaled to -1 .. +1.
 *   REMOD    the mean of the L-bit window with the window position
 *            interpolated by the fractional part of the accumulator.
 *
 * The source position advances by step_16_16 source bits per call, so the
 * average output rate is exactly the requested one.
 */
double pion1_resample_analog(pion1_resampler_t *r, const uint32_t *src,
                              uint32_t src_len);

#endif /* PION1_RESAMPLE_H */
