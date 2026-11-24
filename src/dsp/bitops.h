/*
 * PION/1 -- bit-domain primitives.
 *
 * The whole synth works on packed 1-bit streams.  Throughout the project a
 * "word" is 32 bits, LSB-first: bit 0 is the *earliest* sample, bit 31 the
 * latest.  That matches the RP2040 PIO shift direction used by
 * src/pio/pion1.pio (in pins,1 shifts right, out pins,1 shifts right), so a
 * word read from the capture DMA ring and a word handed to the playback DMA
 * ring use the same convention with no bit reversal anywhere in the chain.
 *
 * Nothing in this header may depend on the Pico SDK: it is compiled both for
 * the firmware (Cortex-M0+) and for the host test harness on the Pi.
 */
#ifndef PION1_BITOPS_H
#define PION1_BITOPS_H

#include <stdint.h>

/* Byte-sized lookup tables.  The RP2040 has no POPCNT instruction, so we use
 * a 256-entry table rather than libgcc's __popcountdi2 helper call. */
extern const uint8_t pion1_popcnt8[256];
extern const uint8_t pion1_rev_byte[256]; /* reverse the bit order of a byte */
extern const uint8_t pion1_rev_word[256]; /* reverse the byte order of a word */

static inline uint32_t pion1_popcount32(uint32_t v)
{
    return (uint32_t)pion1_popcnt8[v & 0xFFu] +
           (uint32_t)pion1_popcnt8[(v >> 8) & 0xFFu] +
           (uint32_t)pion1_popcnt8[(v >> 16) & 0xFFu] +
           (uint32_t)pion1_popcnt8[(v >> 24) & 0xFFu];
}

static inline uint32_t pion1_popcount64(uint64_t v)
{
    return pion1_popcount32((uint32_t)(v & 0xFFFFFFFFu)) +
           pion1_popcount32((uint32_t)(v >> 32));
}

/* Rotate right.  n is taken modulo 32 so callers can pass a free-running
 * counter without masking it themselves. */
static inline uint32_t pion1_ror32(uint32_t v, uint32_t n)
{
    n &= 31u;
    return n ? ((v >> n) | (v << (32 - n))) : v;
}

static inline uint32_t pion1_rol32(uint32_t v, uint32_t n)
{
    n &= 31u;
    return n ? ((v << n) | (v >> (32 - n))) : v;
}

/* Reverse the bit order inside a 32-bit word (bit i <-> bit 31-i). */
static inline uint32_t pion1_bitrev32(uint32_t w)
{
    return ((uint32_t)pion1_rev_byte[w & 0xFFu] << 24) |
           ((uint32_t)pion1_rev_byte[(w >> 8) & 0xFFu] << 16) |
           ((uint32_t)pion1_rev_byte[(w >> 16) & 0xFFu] << 8) |
           ((uint32_t)pion1_rev_byte[(w >> 24) & 0xFFu]);
}

/*
 * Count the ones in n bits starting at bit offset a0.
 *
 * src[0] is the word that contains bit a0; the range is allowed to run into
 * src[1].  Requires a0 < 32; the range is silently clamped to the 64 bits
 * that are actually available.
 *
 * This is the workhorse of the boxcar decimator: the resampler slides a
 * window of L source bits along the capture ring and needs the number of ones
 * in the moving window without re-scanning it from scratch.
 */
static inline uint32_t pion1_popcnt_range(const uint32_t *src, uint32_t a0, uint32_t n)
{
    uint64_t v, mask;

    if (n == 0u) {
        return 0u;
    }
    a0 &= 31u;
    if (n > (64u - a0)) {
        n = 64u - a0;
    }
    v = (uint64_t)src[0] | ((uint64_t)src[1] << 32);
    if (n >= 64u) {
        mask = ~(uint64_t)0;
    } else {
        mask = (((uint64_t)1 << n) - 1u) << a0;
    }
    v &= mask;
    return pion1_popcount64(v);
}

/* Extract a single bit from an LSB-first word. */
static inline uint32_t pion1_bit(const uint32_t *src, uint32_t index)
{
    return (src[index >> 5] >> (index & 31u)) & 1u;
}

/* Set a single bit in an LSB-first word. */
static inline void pion1_set_bit(uint32_t *dst, uint32_t index, uint32_t value)
{
    uint32_t sh = index & 31u;
    if (value) {
        dst[index >> 5] |= (1u << sh);
    } else {
        dst[index >> 5] &= ~(1u << sh);
    }
}

#endif /* PION1_BITOPS_H */
