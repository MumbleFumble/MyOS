#include <stdint.h>
#include "exceptions.h"

/* -----------------------------------------------------------------------
 * Baseline page replacement policies (Phase 2, increment 4).
 *
 * FIFO: evict the frame with the lowest allocation sequence number
 * (the one that has been resident the longest).
 * ----------------------------------------------------------------------- */

static int fifo_pick_victim(struct demand_frame *frames, int n)
{
    int victim = -1;
    uint64_t oldest = (uint64_t)-1;
    for (int i = 0; i < n; i++) {
        if (frames[i].virt != 0 && frames[i].seq < oldest) {
            oldest = frames[i].seq;
            victim = i;
        }
    }
    return victim;
}

struct pf_policy pf_policy_fifo = {
    .name = "fifo",
    .pick_victim = fifo_pick_victim,
};
