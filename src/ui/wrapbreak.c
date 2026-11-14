#include "ui/wrapbreak.h"
#include <stdlib.h>

int wrapbreak_fit(const uint32_t *cps, int at, int end, float maxW,
                  WrapWidth width, WrapContinuation continuation, void *context) {
  if (!cps || !width || !continuation || end <= at) return at;
  int first = at + 1;
  while (first < end && continuation(cps, first)) first++;
  int *bounds = malloc((size_t)(end - at) * sizeof *bounds);
  if (!bounds) return first; /* still advances by a complete grapheme */
  int n = 0;
  for (int p = first; p <= end; p++)
    if (p == end || !continuation(cps, p)) bounds[n++] = p;
  int best = -1, lo = 0, hi = n - 1;
  while (lo <= hi) {
    int mid = lo + (hi - lo) / 2;
    if (width(cps, at, bounds[mid], context) <= maxW) { best = mid; lo = mid + 1; }
    else hi = mid - 1;
  }
  int fit = best >= 0 ? bounds[best] : first;
  free(bounds);
  return fit;
}
