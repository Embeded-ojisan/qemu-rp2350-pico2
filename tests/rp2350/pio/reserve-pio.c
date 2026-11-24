/*
 * Select PIO0 for the official hello_pio example through SDK allocation.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "hardware/pio.h"

static void __attribute__((constructor)) reserve_other_pios(void)
{
    const uint sm_mask = (1u << NUM_PIO_STATE_MACHINES) - 1;

    /* The SDK's automatic allocator tries the highest-numbered PIO first. */
    pio_claim_sm_mask(pio1, sm_mask);
    pio_claim_sm_mask(pio2, sm_mask);
}
