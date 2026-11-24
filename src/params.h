/*
 * PION/1 -- shared parameter block.
 *
 * Core 0 owns the UI and writes parameters; core 1 reads them.  The block
 * is double buffered: core 0 fills the inactive buffer and then publishes
 * it with a single atomic store of the sequence number.  Core 1 picks up a
 * new buffer only when the sequence number changes, so it never sees a
 * half-written set of parameters.
 *
 * The block lives in RAM (not flash) and is aligned so that the publish
 * store is atomic: on the RP2040 a naturally aligned 32-bit store is
 * atomic with respect to other cores.
 */
#ifndef PION1_PARAMS_H
#define PION1_PARAMS_H

#include <stdint.h>

#define PION1_N_PARAMS 16
#define PION1_PARAM_BANKS 2

/* Parameter indices. */
enum {
    PION1_P_MASTER = 0,     /* master level, 0..65535 */
    PION1_P_WAVE1_FREQ = 1,
    PION1_P_WAVE1_LEVEL = 2,
    PION1_P_WAVE1_FM_FREQ = 3,
    PION1_P_WAVE1_FM_DEPTH = 4,
    PION1_P_WAVE1_SHAPE = 5,
    PION1_P_SAMP_RATE = 6,  /* playback rate of the captured stream */
    PION1_P_SAMP_LEVEL = 7,
    PION1_P_SAMP_TIER = 8,
    PION1_P_BOXCAR_LEN = 9,
    PION1_P_WG_FREQ = 10,
    PION1_P_WG_LEVEL = 11,
    PION1_P_WG_FEEDBACK = 12,
    PION1_P_IDLE = 13,      /* idle pattern for unused mixer slots */
    PION1_P_TAIL = 14,      /* unused, reserved */
    PION1_P_VERSION = 15
};

typedef struct {
    uint32_t seq;                 /* sequence number of the live bank */
    uint32_t generation;          /* bumped on every publish */
    uint32_t params[PION1_N_PARAMS];
    uint32_t reserved[4];
} pion1_params_t;

/*
 * The live block.  Declared in params.c so that both cores agree on one
 * instance and the linker places it in a known location.
 */
extern pion1_params_t pion1_params;

/*
 * Publish the staged bank.  Called by core 0 only.
 *
 * The sequence number is bumped *after* the data is written and the publish
 * is done with the sequence number as the last thing written, so a reader
 * that sees a new sequence number also sees the new data.  On a single
 * core this is trivially true; the store is also the release barrier.
 */
void pion1_params_publish(void);

/*
 * Read the live parameters.  Called by core 1 only.  Returns a pointer to
 * a stable snapshot; the pointer is valid until the next publish.
 */
const uint32_t *pion1_params_read(void);

/*
 * Staging helpers, core 0 only.  They edit the bank that the next
 * pion1_params_publish() will copy into the live block.
 */
void pion1_params_stage_set(uint32_t index, uint32_t value);
uint32_t pion1_params_stage_get(uint32_t index);

#endif /* PION1_PARAMS_H */
