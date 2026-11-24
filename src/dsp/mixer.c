/* PION/1 -- mask-select mixer. */

#include "mixer.h"
#include "bitops.h"

/*
 * Rebuild the base ownership masks from the current levels.
 *
 * The slot allocation is *absolute*: a voice at 50% level wants half of the
 * 32 slots, whatever else is playing.  That is what makes the idle
 * pattern meaningful -- it is the floor of the signal, and a voice that
 * is quiet simply gives its unused slots back to it.  This is ordinary
 * bit-stealing, the same trick a bit-crusher uses.
 *
 * When the voices ask for more slots than exist (their levels sum above
 * 100%), the allocation is scaled down so the masks stay disjoint.  A
 * single voice can therefore never claim more than the whole word.
 *
 * The slots are distributed evenly rather than in contiguous blocks.  That
 * matters for two reasons:
 *
 *   - the analog front end reconstructs the signal from the *density* of
 *     the bits, so a voice whose bits are clumped together appears as a
 *     low-frequency burst rather than as a steady partial;
 *   - the masks rotate by one bit per word, so a blocky allocation would
 *     rotate as a block and the slot assignment would stay effectively
 *     fixed, which is exactly the static-mask case the rotation exists to
 *     avoid.
 *
 * The distribution uses a deficit scheduler: each slot goes to whichever
 * voice is currently the most "in debt", and a voice's debt advances by
 * 32 when it wins a slot.  That spreads the slots of every voice as
 * evenly as the 32 slots allow while keeping the totals exact.
 */
static void rebuild_masks(pion1_mixer_t *m)
{
    uint32_t v;
    uint32_t total_level = 0;
    uint32_t used = 0;
    uint32_t counts[PION1_MAX_VOICES];
    uint32_t debt[PION1_MAX_VOICES];
    uint32_t masks[PION1_MAX_VOICES];
    uint32_t requested = 0;
    uint32_t i;

    for (v = 0; v < m->n_voices; v++) {
        counts[v] = 0;
        masks[v] = 0;
        debt[v] = 0;
        total_level += m->voice[v].level;
    }

    if (total_level == 0u) {
        for (v = 0; v < m->n_voices; v++) {
            m->voice[v].base = 0;
        }
        return;
    }

    /*
     * Convert the levels into slot counts.
     *
     * The level is *absolute*: a voice at 65535 wants all 32 slots, a
     * voice at 32768 wants 16 of them, and a voice at 0 wants none.
     * That makes the idle pattern the floor of the signal -- a quiet
     * voice simply hands its unused slots back to it.
     *
     * The running remainder spreads the rounding error over the voices
     * instead of letting it accumulate on one of them.
     */
    for (v = 0; v < m->n_voices; v++) {
        uint64_t num = (uint64_t)m->voice[v].level * 32u + used;
        counts[v] = (uint32_t)(num / 65535u);
        used = (uint32_t)(num % 65535u);
    }
    {
        uint32_t assigned = 0;
        for (v = 0; v < m->n_voices; v++) {
            assigned += counts[v];
        }
        requested = assigned;
    }

    /*
     * Absolute allocation: scale the request so that it uses the number
     * of slots the voices actually asked for, capped at the 32 slots that
     * exist.  With the levels summing to 65535 the request is exactly 32
     * slots; with the levels summing to half of that the request is 16
     * slots and the idle pattern fills the rest.
     */
    if (requested > 32u) {
        uint64_t scale_num = 32u;
        uint64_t scale_den = requested;
        uint32_t new_used = 0;
        uint32_t new_assigned = 0;

        for (v = 0; v < m->n_voices; v++) {
            uint64_t num = (uint64_t)counts[v] * scale_num + new_used;
            counts[v] = (uint32_t)(num / scale_den);
            new_used = (uint32_t)(num % scale_den);
        }
        for (v = 0; v < m->n_voices; v++) {
            new_assigned += counts[v];
        }
        if (new_assigned > 32u) {
            /* Rounding pushed us over; take the difference off the
             * greediest voice. */
            uint32_t best = 0;
            for (v = 0; v < m->n_voices; v++) {
                if (counts[v] > counts[best]) {
                    best = v;
                }
            }
            counts[best] -= (new_assigned - 32u);
        }
    }

    /* Distribute the slots with the deficit scheduler. */
    for (i = 0; i < 32u; i++) {
        uint32_t pick = m->n_voices;
        uint32_t best_debt = 0;

        for (v = 0; v < m->n_voices; v++) {
            if (counts[v] == 0u) {
                continue;
            }
            if (pick == m->n_voices || debt[v] > best_debt) {
                pick = v;
                best_debt = debt[v];
            }
        }
        if (pick == m->n_voices) {
            break;      /* no voice wants any more slots */
        }
        masks[pick] |= (1u << i);
        counts[pick]--;
        debt[pick] += 32u;
    }

    for (v = 0; v < m->n_voices; v++) {
        m->voice[v].base = masks[v];
    }
}

void pion1_mixer_init(pion1_mixer_t *m, uint32_t n_voices, uint32_t idle)
{
    uint32_t v;

    if (n_voices == 0u || n_voices > PION1_MAX_VOICES) {
        n_voices = (n_voices == 0u) ? 1u : PION1_MAX_VOICES;
    }
    m->n_voices = n_voices;
    m->rotate = 0;
    m->idle = idle;

    for (v = 0; v < PION1_MAX_VOICES; v++) {
        m->voice[v].mask = 0;
        m->voice[v].base = 0;
        m->voice[v].level = 0;
    }
}

void pion1_mixer_set_level(pion1_mixer_t *m, uint32_t voice, uint32_t level)
{
    if (voice >= m->n_voices) {
        return;
    }
    if (level > 65535u) {
        level = 65535u;
    }
    if (m->voice[voice].level != level) {
        m->voice[voice].level = level;
        /*
         * Rebuild straight away rather than deferring it, so that the
         * masks are always consistent with the levels.  The rebuild is
         * cheap (32 slots) and the level changes are rare compared to
         * the word rate, so there is no reason to complicate the
         * interface with a "dirty" flag that the caller has to think
         * about.
         */
        rebuild_masks(m);
    }
}

void pion1_mixer_set_density(pion1_mixer_t *m, uint32_t voice,
                              uint32_t num, uint32_t den)
{
    uint32_t level;

    if (den == 0u) {
        return;
    }
    if (num > den) {
        num = den;
    }
    level = (uint32_t)(((uint64_t)num * 65535u) / den);
    pion1_mixer_set_level(m, voice, level);
}

uint32_t pion1_mix(pion1_mixer_t *m, const uint32_t *voice_words)
{
    uint32_t out = 0;
    uint32_t owned = 0;
    uint32_t v;

    /*
     * Apply the rotation to the base masks.  Every voice rotates by the
     * same amount, so the arrangement *between* the voices is preserved
     * while their absolute position in the word drifts.  Over successive
     * words that turns the aliasing tones of a fixed mask into noise.
     */
    for (v = 0; v < m->n_voices; v++) {
        m->voice[v].mask = pion1_ror32(m->voice[v].base, m->rotate);
    }

    for (v = 0; v < m->n_voices; v++) {
        out |= voice_words[v] & m->voice[v].mask;
        owned |= m->voice[v].mask;
    }

    /*
     * Slots that nobody owns fall back to the idle pattern.  That is
     * important: leaving them at zero would silence the output whenever
     * the voices do not fill the word, which is the normal case.
     */
    out |= m->idle & ~owned;

    m->rotate = (m->rotate + 1u) & 31u;

    return out;
}
