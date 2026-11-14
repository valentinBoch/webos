/* Hard-wrap oversized words without splitting a Unicode grapheme cluster. */
#ifndef SB_WRAPBREAK_H
#define SB_WRAPBREAK_H
#include <stdint.h>

/* Width of cps[at:end], in layout pixels. */
typedef float (*WrapWidth)(const uint32_t *cps, int at, int end, void *context);
/* True if cps[index] continues the grapheme cluster before it. */
typedef int (*WrapContinuation)(const uint32_t *cps, int index);

/* Returns a boundary in (at, end], preferably the longest fitting prefix.
   An oversized single cluster is kept whole so the caller always progresses. */
int wrapbreak_fit(const uint32_t *cps, int at, int end, float maxW,
                  WrapWidth width, WrapContinuation continuation, void *context);
#endif
