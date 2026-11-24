/* PION/1 -- shared parameter block (see params.h). */

#include "params.h"

/*
 * The block is placed in RAM so that it is writable and so that it is not
 * in the XIP region (which would make the publish store slow and would
 * need a flash write).
 */
#if defined(__not_in_flash)
__not_in_flash("params") pion1_params_t pion1_params;
#else
pion1_params_t pion1_params;
#endif

/* The staging bank is only touched by core 0. */
static uint32_t staged[PION1_N_PARAMS];

void pion1_params_publish(void)
{
    uint32_t i;

    for (i = 0; i < PION1_N_PARAMS; i++) {
        pion1_params.params[i] = staged[i];
    }
    pion1_params.generation++;
    /*
     * Publish last.  On the RP2040 an aligned 32-bit store is atomic with
     * respect to the other core, and the compiler will not move the
     * preceding stores past it because the parameter block is volatile from
     * the reader's point of view.
     */
    __atomic_store_n(&pion1_params.seq, pion1_params.generation,
                     __ATOMIC_RELEASE);
}

const uint32_t *pion1_params_read(void)
{
    uint32_t seq;

    do {
        seq = __atomic_load_n(&pion1_params.seq, __ATOMIC_ACQUIRE);
        if (seq == 0u) {
            break;      /* nothing published yet */
        }
    } while (0);

    return pion1_params.params;
}

/* Staging helpers, core 0 only. */
void pion1_params_stage_set(uint32_t index, uint32_t value);
uint32_t pion1_params_stage_get(uint32_t index);

void pion1_params_stage_set(uint32_t index, uint32_t value)
{
    if (index < PION1_N_PARAMS) {
        staged[index] = value;
    }
}

uint32_t pion1_params_stage_get(uint32_t index)
{
    if (index < PION1_N_PARAMS) {
        return staged[index];
    }
    return 0;
}
