/*
 * PION/1 -- mask-select mixer.
 *
 * The mixer combines N voices, each of which produces one 32-bit word per
 * output word (32 bits at 500 kHz = 15.625 kHz word rate).  Each voice
 * owns a set of *slots* within the 32-bit word via a mask.
 *
 * Three properties make this work:
 *
 *   1. Ownership masks are disjoint: each slot belongs to exactly one
 *      voice, so voices never fight over the same bit and the mix is a
 *      true sum.
 *
 *   2. The masks rotate by one bit per output word.  The *relative*
 *      arrangement between voices is preserved (so the mix stays
 *      coherent) but the absolute slot assignment drifts.  This turns the
 *      aliasing tones a static mask would produce into broadband noise,
 *      which is far less objectionable.
 *
 *   3. The slot share of each voice follows its level, so the level
 *      control is expressed in the bit domain: a louder voice takes more
 *      slots, a quieter one gives slots back to the idle pattern.  This is
 *      classic bit-stealing, and it is the only way to control the level
 *      of a 1-bit signal without an amplitude field.
 *
 * Because the slot share carries the level, a voice that owns every slot
 * contributes at full amplitude, while two voices at equal level each
 * contribute at half amplitude.  The slot assignment interleaves the
 * voices so that each one's bits are spread evenly through the word,
 * which is what lets the analog reconstruction see them as separate
 * partials rather than as time-shifted copies.
 */
#ifndef PION1_MIXER_H
#define PION1_MIXER_H

#include <stdint.h>

#define PION1_MAX_VOICES 16
#define PION1_SLOTS 32u

typedef struct {
    uint32_t mask;      /* ownership mask after rotation */
    uint32_t base;      /* ownership mask before rotation */
    uint32_t level;     /* requested level, 0..65535 */
} pion1_voice_mix_t;

typedef struct {
    pion1_voice_mix_t voice[PION1_MAX_VOICES];
    uint32_t n_voices;
    uint32_t rotate;    /* rotation amount applied this word */
    uint32_t idle;      /* pattern used for slots nobody owns */
} pion1_mixer_t;

/*
 * Build the mixer with n_voices voices and the given idle pattern.
 */
void pion1_mixer_init(pion1_mixer_t *m, uint32_t n_voices, uint32_t idle);

/*
 * Set the level of a voice, 0..65535.
 *
 * The level is the voice's *share of the slots*: the fraction of the
 * output word that the voice is allowed to occupy.  All the levels are
 * normalised, so a single voice at full level owns every slot and any
 * number of voices can be active at once.
 */
void pion1_mixer_set_level(pion1_mixer_t *m, uint32_t voice, uint32_t level);

/* Convenience wrapper: set the level from a fraction. */
void pion1_mixer_set_density(pion1_mixer_t *m, uint32_t voice,
                              uint32_t num, uint32_t den);

/*
 * Mix one output word.
 *
 * voice_words[i] holds the 32-bit word produced by voice i.  The result is
 * the sum of the masked contributions, with unowned slots filled from the
 * idle pattern.
 *
 * The masks are recomputed and rotated as a side effect, so the caller
 * must call this exactly once per output word.
 */
uint32_t pion1_mix(pion1_mixer_t *m, const uint32_t *voice_words);

#endif /* PION1_MIXER_H */
