#pragma once

#include <stdint.h>

/* C entry point for the #PF stub (vector 14). err_code is the value the
 * CPU pushed; the faulting linear address is read from CR2 inside. */
void page_fault_handler(uint64_t err_code);

/* Installs the IDT gate for vector 14. Call after idt_init(). */
void exceptions_init(void);

/* -----------------------------------------------------------------------
 * Page-fault telemetry (Phase 2, increment 3).
 *
 * Every #PF is recorded in a ring buffer before the handler decides
 * whether to demand-map or halt. Userspace drains via SYS_PF_DRAIN.
 * ----------------------------------------------------------------------- */
#define PF_TRACE_RING_SIZE 256

struct pf_record {
    uint64_t tick;      /* timer tick at fault */
    uint64_t cr2;       /* faulting linear address */
    uint64_t err_code;  /* CPU error code */
    uint32_t pid;       /* faulting task pid (0 if kernel/supervisor) */
    uint32_t resolved;  /* 1 if demand-mapped, 0 if halted */
};                      /* 32 bytes */

/* Drain up to max records (oldest first). Returns count copied. */
int pf_trace_drain(struct pf_record *buf, int max);

/* -----------------------------------------------------------------------
 * Page replacement policy interface (Phase 2, increment 4).
 *
 * The demand region is limited to DEMAND_QUOTA frames. When a fault
 * needs a frame and none is free, the active policy picks a victim
 * frame to evict (unmap and reuse).
 * ----------------------------------------------------------------------- */
#define DEMAND_QUOTA 4

struct demand_frame {
    uint64_t virt;   /* page-aligned VA, 0 = slot free */
    uint64_t phys;   /* backing frame */
    uint64_t seq;    /* allocation sequence number (FIFO order) */
    uint64_t cr3;    /* owning address space */
    uint32_t freq;   /* faults for this page (learned policy feature) */
    uint32_t _pad;
};

/* Returns victim frame index in [0, n), or -1 if none. */
typedef int (*pf_evict_fn)(struct demand_frame *frames, int n);

struct pf_policy {
    const char *name;
    pf_evict_fn pick_victim;
};

/* Baseline policies. */
extern struct pf_policy pf_policy_fifo;
extern struct pf_policy pf_policy_learned;

/* Select the active eviction policy (default: FIFO). */
void pf_set_policy(struct pf_policy *p);
uint64_t pf_current_seq(void);
