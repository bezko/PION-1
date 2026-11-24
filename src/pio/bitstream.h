/*
 * PION/1 -- PIO bitstream engine.
 *
 * Two PIO state machines move 1-bit data:
 *
 *   SM0 (capture)  samples the comparator/flip-flop output and pushes 32-bit
 *                  words into a DMA ring.
 *   SM1 (playback) pops 32-bit words from a DMA ring and drives them out
 *                  one bit per cycle.
 *
 * Both run at exactly 500 kHz (one bit every 2 us) and are started in the
 * same cycle, so the two paths share one timebase.
 *
 * Timing
 * ------
 * The system clock is 125 MHz, so clkdiv = 250 gives a 500 kHz instruction
 * rate -- exactly one instruction per bit.
 *
 * The capture SM runs TWO instructions per bit with clkdiv = 125 (1 MHz
 * instruction rate):
 *
 *   instruction 0:  side-set 1  -> CLK high   (rising edge latches D)
 *   instruction 1:  side-set 0  -> CLK low, and sample the settled Q
 *
 * The sample happens one instruction (1 us) after the clock edge, which is
 * far longer than a 74HC74's t_co (~40 ns) and than the PIO's 2-stage input
 * synchroniser delay (~24 ns at 125 MHz).  See docs/HARDWARE.md for the
 * full timing budget.
 *
 * The playback SM runs ONE instruction per bit with clkdiv = 250.
 */
#ifndef PION1_BITSTREAM_H
#define PION1_BITSTREAM_H

#include <stdint.h>
#include "pico/types.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/structs/dma_debug.h"
/* Bit rate of the 1-bit stream: exactly 500 kHz. */
#define PION1_FS_HZ 500000u

/* Words per second at the bit rate (32 bits per word). */
#define PION1_WORDS_PER_SEC (PION1_FS_HZ / 32u)

/* Capture ring size: 512 words = 16 kbit = 32 ms of buffer. */
#define PION1_CAPTURE_WORDS 512u
#define PION1_CAPTURE_BITS (PION1_CAPTURE_WORDS * 32u)

/* Playback ring size: 512 words. */
#define PION1_PLAYBACK_WORDS 512u

/*
 * The DMA ring wraps on a 2^n BYTE boundary, so a 512-word (2048 byte) ring
 * needs n = 11.
 */
#define PION1_RING_SIZE_BITS 11u

/* PIO state machine assignment. */
#define PION1_SM_CAPTURE 0u
#define PION1_SM_PLAYBACK 1u
#define PION1_SM_MODCLOCK 2u

/* Default pin assignment (see docs/HARDWARE.md). */
#define PION1_PIN_CAPTURE_IN 0u   /* GP0  -- comparator / flip-flop Q */
#define PION1_PIN_CAPTURE_CLK 1u  /* GP1  -- flip-flop clock (side-set) */
#define PION1_PIN_PLAYBACK_OUT 2u /* GP2  -- 1-bit output to the amplifier */

/* Clocks. */
#define PION1_CLK_CAPTURE 125.0f  /* 1 MHz instruction rate, 2 instr/bit */
#define PION1_CLK_PLAYBACK 250.0f /* 500 kHz instruction rate, 1 instr/bit */

/* The capture ring and the playback ring live in bitstream.c, next to the
 * code that owns them.  They are placed in RAM rather than flash because
 * the DMA reads and writes them at 500 kHz and the CPU writes them too. */

/* DMA channels. */
#define PION1_DMA_CAPTURE 0u
#define PION1_DMA_PLAYBACK 1u

/* Set up the PIO programs, the state machines and the DMA channels. */
void pion1_bitstream_init(void);

/* Start (or restart) both state machines in the same cycle. */
void pion1_bitstream_start(void);

/* Stop both state machines. */
void pion1_bitstream_stop(void);

/*
 * The capture ring's write position, in bits.
 *
 * The DMA runs in circular mode over the whole ring, so the amount of data
 * captured is the difference between the current and the initial read
 * position.  Reading the DMA's current read address gives the position
 * directly.
 */
uint32_t pion1_capture_bits_available(void);

/* Pointer to the capture ring and its length in bits. */
const uint32_t *pion1_capture_ring_ptr(void);
uint32_t pion1_capture_ring_bits(void);

/* The playback ring's read position, in words. */
uint32_t pion1_playback_words_pending(void);

/* Write one word into the playback ring.  Returns false if the ring is full. */
int pion1_playback_push(uint32_t word);

#endif /* PION1_BITSTREAM_H */
