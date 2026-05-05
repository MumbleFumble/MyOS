#pragma once
#include <stdint.h>

void keyboard_init(void);

/* Returns the next ASCII character from the keyboard ring buffer,
 * or 0 if the buffer is empty (non-blocking). */
char keyboard_getchar(void);

/* Blocks until a character is available, then returns it. */
char keyboard_getchar_wait(void);
