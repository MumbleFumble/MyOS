#include "elf.h"
#include "../mem/vmm.h"
#include "../mem/pmm.h"

/* User stack: 16KB mapped just below 0x10001000000 (1TB + 16MB) */
#define USER_STACK_TOP    0x10001000000UL
#define USER_STACK_PAGES  4

#define SEG_FLAGS  (PAGE_PRESENT | PAGE_RW | PAGE_USER)
#define ARGV_MAX   16       /* max arguments */
#define ARG_MAXLEN 128      /* max length per argument */

static void memzero(uint64_t vaddr, uint64_t n)
{
    uint8_t *p = (uint8_t *)vaddr;
    for (uint64_t i = 0; i < n; i++) p[i] = 0;
}

static void memcopy(uint64_t dst, const uint8_t *src, uint64_t n)
{
    uint8_t *d = (uint8_t *)dst;
    for (uint64_t i = 0; i < n; i++) d[i] = src[i];
}

static uint64_t elf_strlen(const char *s)
{
    uint64_t n = 0;
    while (s[n]) n++;
    return n;
}

/*
 * Write bytes into a user virtual address by translating to physical first.
 * Only works if the backing frame is identity-mapped (phys < 512MB).
 */
static void write_user(uint64_t cr3, uint64_t uva, const void *src, uint64_t len)
{
    const uint8_t *s = (const uint8_t *)src;
    while (len > 0) {
        uint64_t phys = vmm_virt_to_phys(cr3, uva);
        if (!phys) return;
        uint64_t page_rem = PAGE_SIZE - (uva & 0xFFF);
        uint64_t chunk    = len < page_rem ? len : page_rem;
        uint8_t *dst = (uint8_t *)phys;
        for (uint64_t i = 0; i < chunk; i++) dst[i] = s[i];
        uva += chunk; s += chunk; len -= chunk;
    }
}

int elf_load(const uint8_t *data, uint64_t size,
             const char *cmdline, elf_result_t *out)
{
    if (size < sizeof(Elf64_Ehdr))
        return -1;

    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)data;

    /* Validate magic and class */
    if (*(uint32_t *)eh->e_ident != ELF_MAGIC)   return -1;
    if (eh->e_ident[4] != ELF_CLASS64)            return -1;
    if (eh->e_ident[5] != ELF_DATA_LE)            return -1;
    if (eh->e_type    != ELF_TYPE_EXEC)            return -1;
    if (eh->e_machine != ELF_MACH_X64)            return -1;

    /* Create a new address space */
    uint64_t cr3 = vmm_create_address_space();
    if (!cr3) return -1;

    /* Process PT_LOAD segments */
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph = (const Elf64_Phdr *)(data + eh->e_phoff
                                                     + i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_memsz == 0)      continue;

        /* Align vaddr down to page boundary */
        uint64_t vaddr  = ph->p_vaddr & ~(PAGE_SIZE - 1);
        uint64_t offset = ph->p_vaddr - vaddr;
        uint64_t map_sz = ph->p_memsz + offset;
        uint64_t npages = (map_sz + PAGE_SIZE - 1) / PAGE_SIZE;

        /* Allocate and map pages for this segment */
        for (uint64_t p = 0; p < npages; p++) {
            uint64_t frame = pmm_alloc_page();
            if (!frame) return -1;

            /* Zero the frame (it lives in the identity-mapped region) */
            memzero(frame, PAGE_SIZE);

            vmm_map_page_in(cr3,
                            frame,
                            vaddr + p * PAGE_SIZE,
                            SEG_FLAGS);

            /* Copy file data into the first filesz bytes */
            uint64_t page_virt  = vaddr + p * PAGE_SIZE;
            uint64_t page_start = page_virt;           /* virtual == physical here */
            uint64_t page_end   = page_start + PAGE_SIZE;

            uint64_t seg_file_start = ph->p_vaddr;
            uint64_t seg_file_end   = ph->p_vaddr + ph->p_filesz;

            /* Clamp copy range to this page */
            uint64_t copy_start = seg_file_start > page_start ? seg_file_start : page_start;
            uint64_t copy_end   = seg_file_end   < page_end   ? seg_file_end   : page_end;

            if (copy_start < copy_end) {
                uint64_t dst_frame_off = copy_start - page_start;
                uint64_t src_off       = copy_start - ph->p_vaddr + ph->p_offset;
                memcopy(frame + dst_frame_off,
                        data + src_off,
                        copy_end - copy_start);
            }
        }
    }

    /* Allocate user stack pages */
    uint64_t stack_base = USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE;
    for (uint32_t p = 0; p < USER_STACK_PAGES; p++) {
        uint64_t frame = pmm_alloc_page();
        if (!frame) return -1;
        memzero(frame, PAGE_SIZE);
        vmm_map_page_in(cr3, frame, stack_base + p * PAGE_SIZE, SEG_FLAGS);
    }

    /* -------------------------------------------------------------------
     * Build the System V AMD64 ABI initial stack frame.
     *
     * Layout (high → low, stack grows down):
     *   [strings: argv[0]\0 argv[1]\0 ...]   <- USER_STACK_TOP - string_area
     *   [null terminator for envp]
     *   [null terminator for argv]
     *   [argv[n-1] ptr]
     *   ...
     *   [argv[0] ptr]
     *   [argc]                                <- initial RSP
     * ------------------------------------------------------------------- */

    /* Parse cmdline into up to ARGV_MAX tokens */
    char argv_strs[ARGV_MAX][ARG_MAXLEN];
    int  argc = 0;

    if (cmdline && cmdline[0]) {
        int ci = 0;
        int in_tok = 0;
        for (int i = 0; ; i++) {
            char c = cmdline[i];
            if (c == ' ' || c == '\0') {
                if (in_tok) {
                    argv_strs[argc - 1][ci] = '\0';
                    in_tok = 0; ci = 0;
                }
                if (c == '\0') break;
            } else {
                if (!in_tok) {
                    if (argc >= ARGV_MAX) break;
                    argc++;
                    in_tok = 1; ci = 0;
                }
                if (ci < ARG_MAXLEN - 1)
                    argv_strs[argc - 1][ci++] = c;
            }
        }
    }

    /* Write strings into top of user stack and record their user virtual addresses */
    uint64_t str_ptr = USER_STACK_TOP;
    uint64_t argv_uva[ARGV_MAX];
    for (int i = argc - 1; i >= 0; i--) {
        uint64_t slen = elf_strlen(argv_strs[i]) + 1;  /* include null */
        str_ptr -= slen;
        /* Align down to 8 bytes */
        str_ptr &= ~7UL;
        write_user(cr3, str_ptr, argv_strs[i], slen);
        argv_uva[i] = str_ptr;
    }

    /* Align stack pointer to 16 bytes before pushing pointers */
    str_ptr &= ~15UL;

    /* Push: null (envp[0]), null (argv[argc]), argv[argc-1]..argv[0], argc */
    /* We build the frame from high to low in a local buffer then write_user */
    /* Frame size: (argc + 3) * 8 bytes (argv[], null, envp null, argc) */
    int frame_slots = argc + 3;  /* argc val + argv[0..n-1] + null + envp null */
    str_ptr -= (uint64_t)frame_slots * 8;
    /* Align RSP to 16 bytes (SysV: RSP+8 must be 16-aligned at entry, but for simplicity align RSP to 16) */
    str_ptr &= ~15UL;

    uint64_t rsp = str_ptr;

    /* Write argc */
    uint64_t val = (uint64_t)argc;
    write_user(cr3, rsp, &val, 8);
    /* Write argv[0..argc-1] */
    for (int i = 0; i < argc; i++) {
        write_user(cr3, rsp + 8 + (uint64_t)i * 8, &argv_uva[i], 8);
    }
    /* Write null terminator for argv */
    val = 0;
    write_user(cr3, rsp + 8 + (uint64_t)argc * 8, &val, 8);
    /* Write null for envp[0] */
    write_user(cr3, rsp + 8 + (uint64_t)(argc + 1) * 8, &val, 8);

    out->cr3    = cr3;
    out->entry  = eh->e_entry;
    out->ustack = rsp;   /* initial RSP points at argc */
    return 0;
}
