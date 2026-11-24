#pragma once
#include <stdint.h>

/* -----------------------------------------------------------------------
 * Per-fd operations table (Phase 14 inc.1).
 *
 * Every open fd — console included — dispatches read/write/close
 * through its ops table; the syscall handlers no longer special-case
 * fd numbers. A NULL op pointer means "not supported on this fd"
 * and the dispatch returns -1, which reproduces the pre-vtable
 * behaviour exactly (write to stdin, read from stdout, closing the
 * console). A NULL ops pointer in a slot means the slot is free:
 * that is the single free-slot test everywhere.
 *
 * struct file is embedded per-slot in struct task (sched.h). The
 * ops receive the slot itself, so the backend's handle (node) and
 * the offset live next to the dispatch pointer; a future dup2/pipes
 * step can point two slots at shared state without changing these
 * signatures.
 * ----------------------------------------------------------------------- */
struct vfs_node;
struct file;

struct fd_ops {
    int64_t (*read)(struct file *f, uint8_t *buf, uint64_t count);
    int64_t (*write)(struct file *f, const uint8_t *buf, uint64_t count);
    int64_t (*close)(struct file *f);
};

struct file {
    struct vfs_node     *node;   /* backing node (file fds); NULL for console */
    const struct fd_ops *ops;    /* NULL = free slot */
    uint64_t             offset; /* byte position for the next read/write */
};

/* Zero all slots and install the console ops on fds 0/1/2.
 * Defined in syscall.c, where the console ops live. Called from
 * every task-creation path before the task can run. */
void fd_init_std(struct file *fds);
