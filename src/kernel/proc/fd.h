#pragma once
#include <stdint.h>

/* -----------------------------------------------------------------------
 * Per-fd operations table + shared open-file descriptions
 * (Phase 14 inc.1 vtable; inc.2 shared descriptions for dup2).
 *
 * A task's fd table holds POINTERS to struct file descriptions.
 * A NULL slot is a free fd — the single free-slot test. Several
 * slots (in one task, via dup2) may point at the SAME description:
 * they share its offset, and the description's backend close
 * runs exactly once, when the last alias is released.
 *
 * read/write/close dispatch through the description's ops table.
 * A NULL op pointer means "no backend operation" (the console
 * has no close); whether close() on a slot is allowed at all is
 * a per-slot property (the fd_pinned mask in struct task), not
 * a property of the description.
 *
 * refcount: pool descriptions (file fds) count their aliases;
 * 0 marks a free pool entry. Console descriptions are static
 * shared objects flagged FILE_STATIC: they are never counted,
 * never closed, never freed.
 * ----------------------------------------------------------------------- */
struct vfs_node;
struct file;
struct task;

#define FILE_STATIC 0x1u   /* static shared description (console) */
#define FILE_PIPE_R 0x2u   /* pipe read-end description */
#define FILE_PIPE_W 0x4u   /* pipe write-end description */

/* Would-block sentinel for non-blocking pipe I/O (Phase 14
 * inc.3): distinct from -1 (error) and 0 (EOF) so a caller
 * can tell "empty/full but still open" apart from both.
 * Mirrored in src/lib/syscall.h for user code. */
#define E_WOULDBLOCK (-2)

struct fd_ops {
    int64_t (*read)(struct file *f, uint8_t *buf, uint64_t count);
    int64_t (*write)(struct file *f, const uint8_t *buf, uint64_t count);
    int64_t (*close)(struct file *f);
};

struct pipe;

struct file {
    union {
        struct vfs_node *node;  /* file descriptions */
        struct pipe     *pipe;  /* pipe end descriptions */
    };
    const struct fd_ops *ops;      /* dispatch table; NULL only on a free pool entry */
    uint64_t             offset;   /* byte position, shared by all aliases */
    int32_t              refcount; /* aliases; pool entries: 0 = free */
    uint32_t             flags;    /* FILE_STATIC for the console descriptions */
};

/* Initialise a task's fd table: all slots free except fds
 * 0/1/2, which point at the shared console descriptions and
 * are pinned (close() on them fails, as it always has).
 * Defined in syscall.c. Called from every task-creation path
 * before the task can run. */
void fd_init_std(struct task *t);

/* Release every fd slot of a dying task (Phase 18 inc.2):
 * description refcounts drop and backend closes run at
 * the last alias, so a dead pipe writer delivers EOF.
 * Called on every death path (exit, fault kill, SYS_KILL).
 * Defined in syscall.c. */
void fd_teardown_task(struct task *t);
