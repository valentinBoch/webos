#include "ui/wrapbreak.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int continuation(const uint32_t *c, int p) {
  return c[p] == 0x0301 || c[p] == 0x200d || c[p - 1] == 0x200d;
}
static float width(const uint32_t *c, int a, int b, void *context) {
  (void)context;
  float w = 0;
  for (int i = a; i < b; i++)
    w += c[i] == 0x0301 || c[i] == 0x200d ? 0 : c[i] == 0x1f600 ? 3 : 1;
  return w;
}
static void test_long_release(void) {
  const char *src = "Dexter.Resurrection.S02E01.MULTI.DV.HDR.2160p.WEB.DDP5.1.H265";
  uint32_t cps[128]; int n = (int)strlen(src);
  for (int i = 0; i < n; i++) cps[i] = (unsigned char)src[i];
  int at = 0, lines = 0;
  while (at < n) {
    int end = wrapbreak_fit(cps, at, n, 15, width, continuation, NULL);
    assert(end > at && end <= n && width(cps, at, end, NULL) <= 15);
    at = end; lines++;
  }
  assert(lines > 1);
}
static void test_no_split_combining_or_emoji(void) {
  uint32_t accented[] = { 'e', 0x0301, 't', 'e' };
  assert(wrapbreak_fit(accented, 0, 4, 1, width, continuation, NULL) == 2);
  assert(wrapbreak_fit(accented, 2, 4, 1, width, continuation, NULL) == 3);
  uint32_t joined[] = { 0x1f600, 0x200d, 0x1f600, 'x' };
  assert(wrapbreak_fit(joined, 0, 4, 2, width, continuation, NULL) == 3);
  assert(wrapbreak_fit(joined, 0, 4, 6, width, continuation, NULL) == 3);
}
static void test_regular_and_oversized_cluster(void) {
  uint32_t short_word[] = { 'a', 'b', 'c' };
  assert(wrapbreak_fit(short_word, 0, 3, 10, width, continuation, NULL) == 3);
  assert(wrapbreak_fit(short_word, 0, 3, 2, width, continuation, NULL) == 2);
  assert(wrapbreak_fit(short_word, 0, 3, 0.1f, width, continuation, NULL) == 1);
  assert(wrapbreak_fit(short_word, 2, 2, 3, width, continuation, NULL) == 2);
}
int main(void) {
  test_long_release();
  test_no_split_combining_or_emoji();
  test_regular_and_oversized_cluster();
  puts("wrapbreak: all regression cases passed");
  return 0;
}
