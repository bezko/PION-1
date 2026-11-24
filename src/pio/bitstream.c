/* PION/1 -- PIO bitstream engine (see bitstream.h). */

#include "bitstream.h"

#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/structs/dma_debug.h"
#include "hardware/gpio.h"
#include "pico/platform.h"

/* Generated from src/pio/pion1.pio by pico_generate_pio_header(). */
#include "pion1.pio.h"

/*
 * The DMA rings.
 *
 * They live in RAM rather than flash: the DMA reads and writes them at
 * 500 kHz and the CPU writes them as well.
 */
#if defined(__not_in_flash)
__not_in_flash("pion1_buffers") static uint32_t pion1_capture_ring[PION1_CAPTURE_WORDS];
__not_in_flash("pion1_buffers") static uint32_t pion1_playback_ring[PION1_PLAYBACK_WORDS];
#else
static uint32_t pion1_capture_ring[PION1_CAPTURE_WORDS];
static uint32_t pion1_playback_ring[PION1_PLAYBACK_WORDS];
#endif

static int pion1_off_capture = -1;
static int pion1_off_playback = -1;
static int pion1_off_modclock = -1;

/* The word index the capture DMA is currently reading. */
static volatile uint32_t pion1_capture_read_word = 0;

/*
 * Playback ring bookkeeping.
 *
 * The DMA reads from the ring in circular mode, so the DMA's read address
 * is the next word it will fetch.  We keep our own write pointer and a fill
 * count; the fill count is what tells us whether there is room to push.
 */
static volatile uint32_t pion1_playback_write_word = 0;
static volatile uint32_t pion1_playback_fill = 0;

void pion1_bitstream_init(void)
{
    pio_gpio_init(pio0, PION1_PIN_CAPTURE_IN);
    pio_gpio_init(pio0, PION1_PIN_CAPTURE_CLK);
    pio_gpio_init(pio0, PION1_PIN_PLAYBACK_OUT);

    pion1_off_capture = pio_add_program(pio0, &pion1_capture_program);
    pion1_off_playback = pio_add_program(pio0, &pion1_playback_program);
    pion1_off_modclock = pio_add_program(pio0, &pion1_modclock_program);

    /* ---------------- capture SM ---------------- */
    {
        pio_sm_config c = pion1_capture_program_get_default_config(pion1_off_capture);
        sm_config_set_in_pins(&c, PION1_PIN_CAPTURE_IN);
        sm_config_set_sideset_pins(&c, PION1_PIN_CAPTURE_CLK);
        sm_config_set_sideset(&c, 1, false, false);
        sm_config_set_in_shift(&c, true, true, 32);   /* shift right, autopush 32 */
        sm_config_set_clkdiv(&c, PION1_CLK_CAPTURE);
        pio_sm_init(pio0, PION1_SM_CAPTURE, pion1_off_capture, &c);
    }

    /* ---------------- playback SM ---------------- */
    {
        pio_sm_config c = pion1_playback_program_get_default_config(pion1_off_playback);
        sm_config_set_out_pins(&c, PION1_PIN_PLAYBACK_OUT, 1);
        sm_config_set_out_shift(&c, true, true, 32);  /* shift right, autopull 32 */
        sm_config_set_clkdiv(&c, PION1_CLK_PLAYBACK);
        pio_sm_init(pio0, PION1_SM_PLAYBACK, pion1_off_playback, &c);
    }

    /* ---------------- modulator clock SM (optional) ---------------- */
    {
        pio_sm_config c = pion1_modclock_program_get_default_config(pion1_off_modclock);
        sm_config_set_sideset_pins(&c, PION1_PIN_CAPTURE_CLK);
        sm_config_set_sideset(&c, 1, false, false);
        sm_config_set_clkdiv(&c, 125.0f);
        pio_sm_init(pio0, PION1_SM_MODCLOCK, pion1_off_modclock, &c);
    }

    /* ---------------- capture DMA ---------------- */
    {
        dma_channel_config cfg = dma_channel_get_default_config(PION1_DMA_CAPTURE);
        channel_config_set_transfer_data_size(&cfg, DMA_SIZE_32);
        channel_config_set_read_increment(&cfg, true);
        channel_config_set_write_increment(&cfg, true);
        channel_config_set_dreq(&cfg, DREQ_PIO0_RX0);
        /* Circular: the read address wraps at the end of the ring. */
        channel_config_set_ring(&cfg, false, PION1_RING_SIZE_BITS);
        dma_channel_configure(PION1_DMA_CAPTURE, &cfg,
                              pion1_capture_ring, /* write */
                              pion1_capture_ring, /* read */
                              PION1_CAPTURE_WORDS, false);
    }

    /* ---------------- playback DMA ---------------- */
    {
        dma_channel_config cfg = dma_channel_get_default_config(PION1_DMA_PLAYBACK);
        channel_config_set_transfer_data_size(&cfg, DMA_SIZE_32);
        channel_config_set_read_increment(&cfg, true);
        channel_config_set_write_increment(&cfg, true);
        channel_config_set_dreq(&cfg, DREQ_PIO0_TX0);
        /* Circular on the read side so the ring wraps. */
        channel_config_set_ring(&cfg, true, PION1_RING_SIZE_BITS);
        dma_channel_configure(PION1_DMA_PLAYBACK, &cfg,
                              &pio0->txf[PION1_SM_PLAYBACK], /* write to the FIFO */
                              pion1_playback_ring,           /* read from the ring */
                              PION1_PLAYBACK_WORDS, false);
    }
}

void pion1_bitstream_start(void)
{
    /* Both SMs are enabled in the same cycle so they share one timebase. */
    pio_enable_sm_mask_in_sync(pio0,
                               (1u << PION1_SM_CAPTURE) |
                               (1u << PION1_SM_PLAYBACK));

    /* Start the DMA channels. */
    dma_channel_start(PION1_DMA_CAPTURE);
    dma_channel_start(PION1_DMA_PLAYBACK);
}

void pion1_bitstream_stop(void)
{
    dma_channel_abort(PION1_DMA_CAPTURE);
    dma_channel_abort(PION1_DMA_PLAYBACK);
    pio_sm_set_enabled(pio0, PION1_SM_CAPTURE, false);
    pio_sm_set_enabled(pio0, PION1_SM_PLAYBACK, false);
}

const uint32_t *pion1_capture_ring_ptr(void)
{
    return pion1_capture_ring;
}

uint32_t pion1_capture_ring_bits(void)
{
    return PION1_CAPTURE_BITS;
}

uint32_t pion1_capture_bits_available(void)
{
    /*
     * In circular mode with RING_SEL = read, the DMA's read address wraps
     * back to the start of the ring when it reaches the end.  The address
     * that the DMA will read NEXT is therefore the oldest sample that has
     * not yet been consumed by the CPU.
     *
     * The number of words captured since the start is the distance from the
     * current read position back to the start of the ring... but only if we
     * know where the CPU last read up to.  We track that separately: the
     * DSP core records the read position it last consumed.
     */
    uint32_t read_addr = (uint32_t)(uintptr_t)dma_hw->ch[PION1_DMA_CAPTURE].read_addr;
    uint32_t read_word;

    /* The address is within the ring; convert it to a word index. */
    read_word = (read_addr - (uint32_t)(uintptr_t)pion1_capture_ring) >> 2;
    read_word &= (PION1_CAPTURE_WORDS - 1u);

    pion1_capture_read_word = read_word;

    /*
     * Words captured = (read_word - cpu_read_word) mod ring.
     * The CPU-side read position lives in the DSP core; the ring position
     * helper below returns the raw position and the DSP core does the
     * arithmetic.
     */
    return read_word;
}

uint32_t pion1_playback_words_pending(void)
{
    uint32_t read_addr = (uint32_t)(uintptr_t)dma_hw->ch[PION1_DMA_PLAYBACK].read_addr;
    uint32_t read_word;

    read_word = (read_addr - (uint32_t)(uintptr_t)pion1_playback_ring) >> 2;
    read_word &= (PION1_PLAYBACK_WORDS - 1u);
    return read_word;
}

int pion1_playback_push(uint32_t word)
{
    uint32_t write_word = pion1_playback_write_word;

    if (pion1_playback_fill >= PION1_PLAYBACK_WORDS) {
        return 0;   /* full */
    }

    pion1_playback_ring[write_word] = word;
    pion1_playback_write_word = (write_word + 1u) & (PION1_PLAYBACK_WORDS - 1u);
    pion1_playback_fill++;
    return 1;
}
