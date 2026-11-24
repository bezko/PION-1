/* PION/1 -- variable-rate reader for 1-bit source material. */

#include "resample.h"
#include "bitops.h"

void pion1_resampler_init(pion1_resampler_t *r, pion1_tier_t tier,
                          uint32_t boxcar_len, uint32_t step_16_16)
{
    r->tier = tier;
    r->boxcar_len = boxcar_len ? boxcar_len : 1u;
    r->step_16_16 = step_16_16;
    pion1_resampler_reset(r);
}

void pion1_resampler_reset(pion1_resampler_t *r)
{
    r->acc = 0;
    r->err = 0;
    r->err_frac = 0;
    r->window_sum = 0;
    r->win_start = 0;
    r->out = 0;
    r->primed = 0;
    r->primed_before = 0;
}

/* Read the source bit at an absolute position, wrapping in a 2^n ring. */
static inline uint32_t src_bit(const uint32_t *src, uint32_t mask, uint32_t pos)
{
    return (src[(pos >> 5) & mask] >> (pos & 31u)) & 1u;
}

/*
 * Slide the boxcar window to a new start position and return the number of
 * ones in it.
 *
 * The window covers L bits starting at new_start.  It is maintained
 * incrementally: the bits that leave the window at the front are
 * subtracted and the bits that enter at the back are added.  That keeps the
 * cost proportional to the number of bits the window moved rather than to
 * L, so a long window is nearly free when the read rate is close to 1.
 *
 * The count is signed because the subtraction can transiently take it
 * below zero when the window has just been re-primed.
 */
static int32_t window_slide(pion1_resampler_t *r, const uint32_t *src,
                            uint32_t mask, uint32_t new_start, uint32_t L)
{
    int32_t sum;

    if (!r->primed) {
        uint32_t i;
        sum = 0;
        for (i = 0; i < L; i++) {
            sum += (int32_t)src_bit(src, mask, new_start + i);
        }
        r->window_sum = (uint32_t)sum;
        r->win_start = new_start;
        r->primed = 1;
        return sum;
    }

    {
        uint32_t old_start = r->win_start;
        uint32_t old_end = old_start + L;
        uint32_t new_end = new_start + L;
        uint32_t i;

        sum = (int32_t)r->window_sum;

        if (new_start < old_start) {
            /*
             * The 32-bit source position wrapped.  The incremental update
             * is no longer valid, so recount the window from scratch.
             */
            sum = 0;
            for (i = 0; i < L; i++) {
                sum += (int32_t)src_bit(src, mask, new_start + i);
            }
        } else {
            /* Bits leaving the window: [old_start, new_start) */
            for (i = old_start; i < new_start; i++) {
                sum -= (int32_t)src_bit(src, mask, i);
            }
            /* Bits entering the window: [old_end, new_end) */
            for (i = old_end; i < new_end; i++) {
                sum += (int32_t)src_bit(src, mask, i);
            }
        }

        r->window_sum = (uint32_t)sum;
        r->win_start = new_start;
        return sum;
    }
}

double pion1_resample_analog(pion1_resampler_t *r, const uint32_t *src,
                              uint32_t src_len)
{
    uint32_t mask = (src_len >> 5) - 1u;
    uint32_t pos = r->acc >> 16;
    uint32_t L = r->boxcar_len;
    double level;

    switch (r->tier) {
    case PION1_TIER_CRUDE:
        level = src_bit(src, mask, pos) ? 1.0 : -1.0;
        break;

    case PION1_TIER_REMOD: {
        uint32_t frac = r->acc & 0xFFFFu;
        int32_t count = window_slide(r, src, mask, pos, L);
        int32_t delta = (int32_t)src_bit(src, mask, pos + L) -
                        (int32_t)src_bit(src, mask, pos);
        int64_t interp = ((int64_t)count << 16) + ((int64_t)delta * (int64_t)frac);
        interp >>= 16;
        level = 2.0 * (double)interp / (double)L - 1.0;
        break;
    }

    case PION1_TIER_BOXCAR:
    default: {
        int32_t count = window_slide(r, src, mask, pos, L);
        level = 2.0 * (double)count / (double)L - 1.0;
        break;
    }
    }

    r->acc += r->step_16_16;
    return level;
}

uint32_t pion1_resample(pion1_resampler_t *r, const uint32_t *src,
                         uint32_t src_len)
{
    uint32_t mask = (src_len >> 5) - 1u;   /* ring of src_len bits */
    uint32_t pos = r->acc >> 16;
    uint32_t L = r->boxcar_len;

    switch (r->tier) {
    case PION1_TIER_CRUDE: {
        uint32_t b = src_bit(src, mask, pos);
        r->acc += r->step_16_16;
        r->out = b;
        return b;
    }

    case PION1_TIER_BOXCAR: {
        /*
         * The window is L bits long and starts at the current position.
         * We need the count of ones in it, then re-quantise that count to
         * a bit with a first-order feedback loop:
         *
         *   err += (count - L/2) - (out ? +L/2 : -L/2)
         *   out  = (err >= 0)
         *
         * In the count domain a full-scale window of L ones is L, so the
         * output bit contributes +-L/2 to the comparison, exactly like the
         * window average does.  Averaged over time the loop therefore
         * drives the *density* of the output bits towards the density of
         * the input window, which is what preserves the signal level.
         */
        int32_t count = window_slide(r, src, mask, pos, L);
        int32_t x = count - (int32_t)(L >> 1);
        int32_t fb = r->out ? (int32_t)(L >> 1) : -(int32_t)(L >> 1);

        if (!r->primed_before) {
            /*
             * First call: start the integrator at the current deviation so
             * that the very first output bit already reflects the input
             * density instead of a transient.
             */
            r->err = x;
            r->primed_before = 1;
        } else {
            r->err += x - fb;
        }

        /* Keep the accumulator in a range that cannot overflow. */
        if (r->err > (int32_t)0x3FFFFFFF) {
            r->err = (int32_t)0x3FFFFFFF;
        } else if (r->err < -(int32_t)0x3FFFFFFF) {
            r->err = -(int32_t)0x3FFFFFFF;
        }

        r->out = (r->err >= 0) ? 1u : 0u;
        r->acc += r->step_16_16;
        return r->out;
    }

    case PION1_TIER_REMOD: {
        /*
         * Same feedback structure as the boxcar tier, but the window
         * position is interpolated: the window count is blended between
         * the count at the current position and the count one bit further
         * on, weighted by the fractional part of the accumulator.  That
         * keeps the decimation phase from being quantised to whole source
         * bits.
         *
         * The difference between the two counts is cheap to obtain:
         *
         *   count(pos+1) - count(pos) = bit[pos+L] - bit[pos]
         *
         * so the blend needs only two extra bit reads and no extra
         * bookkeeping.
         */
        uint32_t frac = r->acc & 0xFFFFu;
        int32_t count = window_slide(r, src, mask, pos, L);
        int32_t delta;
        int64_t count_interp;
        int64_t x, fb;

        delta = (int32_t)src_bit(src, mask, pos + L) -
                (int32_t)src_bit(src, mask, pos);

        count_interp = ((int64_t)count << 16) + ((int64_t)delta * (int64_t)frac);
        count_interp >>= 16;

        x = count_interp - (int64_t)(L >> 1);
        fb = r->out ? (int64_t)(L >> 1) : -(int64_t)(L >> 1);

        if (!r->primed_before) {
            r->err_frac = x << 16;
            r->primed_before = 1;
        } else {
            r->err_frac += (x - fb) << 16;
        }

        if (r->err_frac > ((int64_t)1 << 40)) {
            r->err_frac = ((int64_t)1 << 40);
        } else if (r->err_frac < -((int64_t)1 << 40)) {
            r->err_frac = -((int64_t)1 << 40);
        }

        r->out = (r->err_frac >= 0) ? 1u : 0u;
        r->acc += r->step_16_16;
        return r->out;
    }

    default:
        r->acc += r->step_16_16;
        return r->out;
    }
}
