/* glob.h -- pathname pattern matching for the MyOS shell
 * (Phase 16 inc.5). ONE source, two consumers: user/hello.c
 * includes this header for the shell's globbing, and
 * scripts/glob_host_test.c includes it for the host test --
 * there is no second copy of the matcher.
 *
 * glob_match(pat, mask, name) matches one cooked word
 * (pat) against one candidate name. mask, when non-NULL,
 * carries one byte per pattern character: a '*', '?' or
 * '[' is a pattern character ONLY where its mask byte is 1
 * (glob-eligible: it reached the word from unquoted text
 * or from an unquoted expansion's value, per the mask
 * expand_str produces). A NULL mask means every character
 * is eligible. An ineligible pattern character matches
 * literally, as does every other character.
 *
 * Pattern language:
 *   *     any sequence of characters, including empty
 *   ?     exactly one character
 *   [...] a character class: single characters and ranges
 *         (a-z); a '!' as the FIRST character negates the
 *         class (POSIX; '^' is an ordinary member); a ']'
 *         immediately after '[' or '[!' is a literal
 *         member. A '[' with no closing ']' is a literal
 *         '[' and matching continues after it.
 * Class members are data: a '*' or '?' inside a class
 * matches itself. Eligibility is a property of the '['
 * that opens the class, with one refinement: the class's
 * own syntax characters ('!' negation, '-' range, the
 * closing ']') count only when THEY are eligible, so a
 * quoted '!' / '-' / ']' stays member data. There is no
 * '**' recursion and no
 * brace expansion: two stars are two stars, which this
 * algorithm treats as equivalent to one. A '/' is an
 * ordinary literal character.
 *
 * The matcher is iterative star-backtracking: an eligible
 * '*' records a retry point; when the element after it
 * fails, the star absorbs one more name character and the
 * element is retried. Elements are matched by
 * glob_elem(), which returns the number of PATTERN
 * characters the element consumes (0 = no match), so a
 * multi-character element (a class) retries atomically. */
#ifndef MYOS_GLOB_H
#define MYOS_GLOB_H

static int glob_elig(const unsigned char *mask, long i)
{
    return mask == 0 || mask[i] != 0;
}

/* Length of the class starting at pat[i] (pat[i] == '['),
 * including both brackets, or 0 if it has no closing ']'.
 * The class's three syntax characters consult the mask
 * (Phase 16 inc.5, review catch): the negating '!' counts
 * only when eligible, a ']' closes only when eligible,
 * and a '-' forms a range only when eligible -- a quoted
 * '!' / ']' / '-' is member data, not syntax, so quoted
 * text can never become syntax. A ']' right after '[' or
 * '[!' is a member either way (position, not syntax). */
static long glob_class_len(const char *pat,
                           const unsigned char *mask, long i)
{
    long j = i + 1;
    if (pat[j] == '!' && glob_elig(mask, j)) j++;
    if (pat[j] == ']') j++;
    while (pat[j] && !(pat[j] == ']' && glob_elig(mask, j))) j++;
    return pat[j] ? (j - i + 1) : 0;
}

/* Does ch fall in the class pat[i..i+len-1]? */
static int glob_class_match(const char *pat,
                            const unsigned char *mask,
                            long i, long len, char ch)
{
    long j = i + 1, end = i + len - 1; /* pat[end] == ']' */
    int neg = 0, hit = 0;
    if (pat[j] == '!' && glob_elig(mask, j)) { neg = 1; j++; }
    while (j < end) {
        if (pat[j + 1] == '-' && glob_elig(mask, j + 1) && j + 2 < end) {
            if (ch >= pat[j] && ch <= pat[j + 2]) hit = 1;
            j += 3;
        } else {
            if (ch == pat[j]) hit = 1;
            j++;
        }
    }
    return neg ? !hit : hit;
}

/* Match the single pattern element at pat[i] against the
 * single character ch. Returns the number of pattern
 * characters consumed (> 0) on a match, 0 on no match.
 * '*' is NOT an element (the caller handles it). */
static long glob_elem(const char *pat, const unsigned char *mask,
                      long i, char ch)
{
    char c = pat[i];
    if (!c) return 0;
    if (c == '?' && glob_elig(mask, i)) return ch ? 1 : 0;
    if (c == '[' && glob_elig(mask, i)) {
        long len = glob_class_len(pat, mask, i);
        if (len > 0)
            return glob_class_match(pat, mask, i, len, ch) ? len : 0;
        /* No closing ']': the '[' is a literal character. */
        return ch == '[' ? 1 : 0;
    }
    return c == ch ? 1 : 0;
}

static int glob_match(const char *pat, const unsigned char *mask,
                      const char *name)
{
    long p = 0, n = 0;
    long star_p = -1, star_n = 0;
    while (name[n]) {
        if (pat[p] == '*' && glob_elig(mask, p)) {
            star_p = p++;
            star_n = n;
            continue;
        }
        long adv = glob_elem(pat, mask, p, name[n]);
        if (adv > 0) { p += adv; n++; continue; }
        if (star_p >= 0) {
            p = star_p + 1;
            n = ++star_n;
            continue;
        }
        return 0;
    }
    while (pat[p] == '*' && glob_elig(mask, p)) p++;
    return pat[p] == '\0';
}

#endif /* MYOS_GLOB_H */
