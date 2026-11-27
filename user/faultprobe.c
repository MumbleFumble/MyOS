/* faultprobe — deliberately hostile ring-3 program
 * (Phase 17 inc.2). Each mode performs one faulting or
 * forbidden operation from user mode; the kernel's fault
 * path is expected to kill this task with EXIT_FAULT while
 * the rest of the system runs on. Mode "ok" is the control:
 * it prints and exits 0.
 *
 *   unmapped  write to 0x50000000000 (the shell pf probe)
 *   null      write through a null pointer (#PF, cr2=0)
 *   div0      integer divide by zero (#DE)
 *   ud2       ud2 instruction (#UD)
 *   gp        hlt at ring 3 (#GP)
 *   ss        push with a non-canonical stack pointer (#SS)
 */
#include "../src/lib/syscall.h"

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void puts_u(const char *s) { sys_write(1, s, slen(s)); }
static int streq(const char *a, const char *b)
{
    long i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return a[i] == b[i];
}

void _start(long argc, char **argv)
{
    const char *mode = (argc > 1) ? argv[1] : "null";
    puts_u("faultprobe: running mode ");
    puts_u(mode);
    puts_u("\r\n");

    if (streq(mode, "ok")) {
        puts_u("faultprobe: ok mode, exiting cleanly\r\n");
        sys_exit(0);
    } else if (streq(mode, "unmapped")) {
        *(volatile char *)0x50000000000UL = 42;
    } else if (streq(mode, "null")) {
        *(volatile char *)0 = 42;
    } else if (streq(mode, "div0")) {
        volatile long z = 0;
        volatile long q = 1234;
        volatile long r;
        r = q / z;              /* #DE at runtime */
        (void)r;
    } else if (streq(mode, "ud2")) {
        __asm__ volatile("ud2");
    } else if (streq(mode, "gp")) {
        __asm__ volatile("hlt");   /* privileged at ring 3: #GP */
    } else if (streq(mode, "ss")) {
        /* Non-canonical RSP: the push faults with #SS. */
        __asm__ volatile("mov $0x8000000000000000, %%rsp\n\t"
                         "push %%rax"
                         ::: "memory");
    } else {
        puts_u("faultprobe: unknown mode\r\n");
        sys_exit(1);
    }

    /* Reached only if the fault did NOT happen. */
    puts_u("faultprobe: SURVIVED (fault path broken)\r\n");
    sys_exit(1);
}
