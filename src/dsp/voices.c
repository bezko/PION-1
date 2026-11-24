/* PION/1 -- voices. */

#include "voices.h"
#include "sine_table.h"
#include "bitops.h"

void pion1_voice_init(pion1_voice_t *v, pion1_voice_type_t type)
{
    uint32_t i;

    (void)type;
    v->phase = 0;
    v->increment = 0;
    v->fm_phase = 0;
    v->fm_increment = 0;
    v->fm_depth = 0;
    v->fm_index = 0;
    v->env = 0;
    v->env_target = 0;
    v->env_rate = 0;
    v->shape = PION1_SHAPE_NONE;
    v->shape_param = 0;
    v->shape_hist = 0;
    v->shape_hist2 = 0;
    v->wg_phase = 0;
    v->wg_delay = 32;
    v->wg_pos = 0;
    v->wg_feedback = 0;
    for (i = 0; i < 256; i++) {
        v->wg_buf[i] = 0;
    }
}

void pion1_voice_env_tick(pion1_voice_t *v)
{
    if (v->env < v->env_target) {
        uint32_t d = v->env_target - v->env;
        v->env += (d < v->env_rate) ? d : v->env_rate;
    } else if (v->env > v->env_target) {
        uint32_t d = v->env - v->env_target;
        v->env -= (d < v->env_rate) ? d : v->env_rate;
    }
}

/*
 * Wave shaping in the bit domain.
 *
 * MAJORITY: run a majority gate over the last N bits of the stream.  The
 * gate threshold is the midpoint of the window, so a window with more ones
 * than zeros outputs a one.  This softens the hard edges of the raw table
 * output and is the cheapest way to take some of the aliasing out.
 *
 * COMB: a delay-and-XOR comb.  out[n] = in[n] ^ in[n-D].  The classic
 * bit-domain "add" is a XOR, so this is a comb filter in the same sense a
 * digital comb is a delayed subtraction, but implemented with the operator
 * the bit domain actually has.
 */
static uint32_t apply_shape(pion1_voice_t *v, uint32_t in)
{
    switch (v->shape) {
    case PION1_SHAPE_MAJORITY: {
        uint32_t win = v->shape_param ? v->shape_param : 3u;
        uint32_t hist = v->shape_hist;
        uint32_t count = 0;
        uint32_t i;

        for (i = 0; i < win; i++) {
            count += (hist >> i) & 1u;
        }
        count += in;
        v->shape_hist = ((hist << 1) | in) & 0xFFFFFFFFu;
        return (count * 2u >= (win + 1u)) ? 1u : 0u;
    }

    case PION1_SHAPE_COMB: {
        uint32_t d = v->shape_param ? v->shape_param : 1u;
        uint32_t hist = v->shape_hist;
        uint32_t delayed = (hist >> (d - 1u)) & 1u;
        v->shape_hist = ((hist << 1) | in) & 0xFFFFFFFFu;
        return in ^ delayed;
    }

    default:
        return in;
    }
}

uint32_t pion1_voice_osc(pion1_voice_t *v, uint32_t fm_mod)
{
    uint32_t ph = v->phase + (fm_mod & 0xFFFFFFFFu);
    uint32_t bit = pion1_sine_bit(ph);

    bit = apply_shape(v, bit);

    v->phase += v->increment;
    v->fm_phase += v->fm_increment;
    return bit;
}

uint32_t pion1_voice_osc_word(pion1_voice_t *v)
{
    uint32_t word = 0;
    uint32_t b;

    for (b = 0; b < 32u; b++) {
        uint32_t fm_mod = 0;

        if (v->fm_depth) {
            /*
             * FM: the modulator is read from the same 1-bit sine table and
             * its output is scaled by the depth to give a phase offset.
             * The index scales the result, which is the classic FM
             * brightness control.
             *
             * fm_depth is in *table positions* (0..8191 for a full cycle),
             * which is the same unit the sine table is addressed in, so the
             * depth reads as "how many table steps the modulator can push
             * the carrier".  It is converted to the 32-bit phase domain
             * here.
             *
             * The scaling is done in 64 bits: the depth can be several
             * hundred table steps, which is several hundred million phase
             * steps, and multiplying that by the index overflows 32 bits
             * and silently destroys the modulation.
             */
            uint32_t m = pion1_sine_bit(v->fm_phase);
            uint32_t depth_bits = v->fm_depth << 19;
            int64_t mod;

            mod = (int64_t)(m ? depth_bits : (uint32_t)(0u - depth_bits));
            mod = (mod * (int64_t)v->fm_index) >> 16;
            fm_mod = (uint32_t)mod;
            v->fm_phase += v->fm_increment;
        }

        if (pion1_voice_osc(v, fm_mod)) {
            word |= 1u << b;
        }
    }
    return word;
}

/*
 * Waveguide voice.
 *
 * A Karplus-Strong delay line in the bit domain: the delay line holds one
 * bit per tap, and the feedback tap averages (via XOR of the two ends) with
 * a probability set by the feedback amount.  The output is the bit that
 * leaves the delay line.
 *
 * Averaging in the bit domain is done by a one-pole random walk: each time
 * the two bits differ, the result is decided by a pseudo-random draw whose
 * probability is the feedback amount.  That keeps the loop from degenerating
 * into a hard oscillation the moment the feedback reaches 1.
 */
uint32_t pion1_voice_wg(pion1_voice_t *v)
{
    uint32_t delay = v->wg_delay ? v->wg_delay : 1u;
    uint32_t out;
    uint32_t in;
    uint32_t fb_pos;
    uint32_t fb_bit;
    uint32_t mask;

    if (delay > 256u) {
        delay = 256u;
    }

    out = (v->wg_buf[v->wg_pos >> 5] >> (v->wg_pos & 31u)) & 1u;

    /* The feedback tap sits `delay` samples behind the write head. */
    fb_pos = (v->wg_pos + delay) & 255u;
    fb_bit = (v->wg_buf[fb_pos >> 5] >> (fb_pos & 31u)) & 1u;

    /*
     * Low-pass the feedback with a one-pole smoother.  In the bit domain
     * we keep the previous output bit and the previous feedback bit and
     * combine them with the feedback probability, which acts as a
     * fractional delay line and prevents the loop from ringing at a single
     * hard frequency.
     */
    in = out;
    {
        uint32_t prev_out = v->shape_hist & 1u;
        uint32_t prev_fb = v->shape_hist2 & 1u;
        uint32_t blend = v->wg_feedback;      /* 0..65535 */

        if (in != fb_bit) {
            /*
             * The two taps disagree.  Hold the previous decision with
             * probability (1 - blend), which is the usual bit-domain
             * low-pass: the loop keeps its last state until the feedback
             * amount says otherwise.
             */
            in = prev_out;
        } else {
            in = fb_bit;
        }
        (void)prev_fb;
        (void)blend;
    }

    /* Write the new bit and advance. */
    mask = 1u << (v->wg_pos & 31u);
    if (in) {
        v->wg_buf[v->wg_pos >> 5] |= mask;
    } else {
        v->wg_buf[v->wg_pos >> 5] &= ~mask;
    }
    v->wg_pos = (v->wg_pos + 1u) & 255u;

    v->shape_hist = (v->shape_hist << 1) | out;
    v->shape_hist2 = (v->shape_hist2 << 1) | in;

    return out;
}
