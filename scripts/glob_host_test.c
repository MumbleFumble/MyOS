/* glob_host_test.c -- host test for the shell's glob
 * matcher. Compiles the SAME source the shell uses
 * (user/glob.h, included below); there is no twin.
 * Build and run:
 *   gcc -std=c11 -Wall -Wextra -o /tmp/glob_host_test \
 *       scripts/glob_host_test.c && /tmp/glob_host_test
 * Prints one line per group and PASS at the end; any
 * failed case prints the case and exits 1. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../user/glob.h"

static int fails;

static void t(const char *pat, const char *name, int want)
{
    int got = glob_match(pat, NULL, name);
    if (got != want) {
        printf("FAIL: glob_match(\"%s\", \"%s\") = %d, want %d\n",
               pat, name, got, want);
        fails++;
    }
}

/* Mask case: mask is one byte per pattern char, 1 =
 * eligible. Built from a string of '0'/'1' for brevity. */
static void tm(const char *pat, const char *mask01,
               const char *name, int want)
{
    unsigned char mask[64];
    size_t i;
    for (i = 0; pat[i]; i++) mask[i] = (unsigned char)(mask01[i] == '1');
    int got = glob_match(pat, mask, name);
    if (got != want) {
        printf("FAIL: glob_match(\"%s\", mask %s, \"%s\") = %d, want %d\n",
               pat, mask01, name, got, want);
        fails++;
    }
}

int main(void)
{
    /* '*' basics, including matching empty */
    t("*", "", 1); t("*", "anything", 1);
    t("*.txt", "a.txt", 1); t("*.txt", "a.bin", 0);
    t("*.txt", ".txt", 1);            /* * matches empty */
    t("a*", "a", 1); t("*a", "ba", 1); t("*a", "ab", 0);
    t("a*c", "abc", 1); t("a*c", "ac", 1); t("a*c", "ab", 0);
    t("**", "abc", 1); t("**", "", 1);      /* two stars = one */
    t("a**c", "abbbc", 1);
    t("*a*a*", "banana", 1); t("*a*b*", "banana", 0);

    /* '?' is exactly one character */
    t("?", "x", 1); t("?", "", 0); t("?", "xy", 0);
    t("a?c", "abc", 1); t("a?c", "ac", 0);
    t("???", "abc", 1); t("???", "ab", 0);

    /* classes, ranges, negation */
    t("[abc]", "b", 1); t("[abc]", "d", 0); t("[abc]", "ab", 0);
    t("[a-z]", "m", 1); t("[a-z]", "M", 0); t("[a-z]", "5", 0);
    t("[0-9]", "7", 1); t("[0-9a-f]", "c", 1); t("[0-9a-f]", "g", 0);
    t("[!a-z]", "M", 1); t("[!a-z]", "m", 0); t("[!0-9]", "x", 1);
    t("[!abc]", "d", 1); t("[!abc]", "a", 0);
    t("^[a]", "^", 0);                       /* ^ is not negation */
    t("[^a]", "^", 1); t("[^a]", "a", 1); t("[^a]", "b", 0);
    t("[]a]", "]", 1); t("[]a]", "a", 1); t("[]a]", "b", 0);
    t("[!]a]", "b", 1); t("[!]a]", "]", 0); t("[!]a]", "a", 0);
    t("[a-]", "-", 1); t("[a-]", "a", 1); t("[-a]", "-", 1);
    t("[*]", "*", 1); t("[*]", "x", 0);      /* members are data */
    t("[?]", "?", 1);
    t("file[0-9].txt", "file7.txt", 1);
    t("file[0-9].txt", "filex.txt", 0);
    t("*[a-c]x", "zzbx", 1); t("*[a-c]x", "zzdx", 0);
    t("*[a-c]x", "ax", 1);

    /* unterminated '[' is a literal '[' */
    t("[", "[", 1); t("[", "a", 0);
    t("[ab", "[ab", 1); t("[ab", "a", 0); t("[ab", "b", 0);
    t("a[", "a[", 1); t("a[bc", "a[bc", 1);
    t("[!", "[!", 1); t("[!]", "[!]", 1);

    /* empty pattern / empty name edges */
    t("", "", 1); t("", "a", 0);
    t("abc", "abc", 1); t("abc", "abd", 0); t("abc", "ab", 0);

    /* pattern characters in the NAME are ordinary */
    t("a", "a", 1); t("*", "*", 1); t("\\*", "*", 0);
    t("literal", "literal", 1);

    /* '/' is an ordinary literal (flat FS: never matches) */
    t("a/b", "a/b", 1); t("*/b", "a/b", 1); t("a/*.txt", "a/b.txt", 1);

    /* mask: ineligible pattern characters are literal */
    tm("*.txt", "01111", "*.txt", 1);   /* * literal */
    tm("*.txt", "01111", "a.txt", 0);
    tm("*.txt", "11111", "a.txt", 1);   /* * eligible */
    tm("a?c", "101", "a?c", 1);         /* ? literal */
    tm("a?c", "101", "abc", 0);
    tm("a?c", "111", "abc", 1);
    tm("[a]b", "0111", "[a]b", 1);      /* [ literal */
    tm("[a]b", "0111", "ab", 0);
    tm("[a]b", "1111", "ab", 1);
    tm("*a", "01", "*a", 1);            /* literal star, then a */
    tm("g*.txt", "111111", "gold.txt", 1);
    tm("g*.txt", "101111", "g*.txt", 1); /* star from a quoted value */
    tm("*", "0", "x", 0);               /* ineligible star is literal */
    tm("*", "0", "*", 1);
    t("*a", "aba", 1);                  /* pattern exhausted: backtrack */

    /* class syntax consults the mask: a quoted ']' does not
     * close, a quoted '!' does not negate, a quoted '-'
     * does not form a range */
    tm("[a]b]", "11011", "]", 1);       /* first ] is data */
    tm("[a]b]", "11011", "b", 1);
    tm("[a]b]", "11111", "]", 0);       /* first ] closes: {a}b] */
    tm("[!a]", "1011", "!", 1);         /* ! is a member */
    tm("[!a]", "1011", "b", 0);
    tm("[!a]", "1111", "b", 1);         /* ! negates */
    tm("[a-z]", "11011", "-", 1);       /* - is a member */
    tm("[a-z]", "11011", "m", 0);
    tm("[a-z]", "11111", "m", 1);       /* - forms the range */

    if (fails) {
        printf("glob host test: %d FAILURES\n", fails);
        return 1;
    }
    printf("glob host test: all cases PASS\n");
    return 0;
}
