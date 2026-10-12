#pragma once

#include <stdint.h>

/* C entry point for the #PF stub (vector 14). err_code is the value the
 * CPU pushed; the faulting linear address is read from CR2 inside. */
void page_fault_handler(uint64_t err_code);

/* Installs the IDT gate for vector 14. Call after idt_init(). */
void exceptions_init(void);
