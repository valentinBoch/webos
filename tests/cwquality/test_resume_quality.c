#include "ui/resample_hq.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void same_colour(int w, int h, int tw, int th) {
  unsigned char *src = malloc((size_t)w * h * 4);
  assert(src);
  for (int i = 0; i < w * h; i++) {
    src[i * 4] = 31; src[i * 4 + 1] = 121;
    src[i * 4 + 2] = 213; src[i * 4 + 3] = 255;
  }
  unsigned char *out = sb_resample_lanczos3_rgba(src, w, h, 0, 0, w, h, tw, th);
  assert(out);
  for (int i = 0; i < tw * th; i++) {
    assert(abs(out[i * 4] - 31) <= 1);
    assert(abs(out[i * 4 + 1] - 121) <= 1);
    assert(abs(out[i * 4 + 2] - 213) <= 1);
    assert(out[i * 4 + 3] == 255);
  }
  sb_unsharp_mild_rgba(out, tw, th);
  for (int i = 0; i < tw * th; i++) {
    assert(abs(out[i * 4] - 31) <= 1);
    assert(abs(out[i * 4 + 1] - 121) <= 1);
    assert(abs(out[i * 4 + 2] - 213) <= 1);
    assert(out[i * 4 + 3] == 255);
  }
  free(src); free(out);
}

static void alpha_and_crop(void) {
  const unsigned char src[] = {
    255, 0, 0, 255,   0, 0, 0, 0,     0, 0, 200, 255,
    255, 0, 0, 255,   0, 0, 0, 0,     0, 0, 200, 255,
    255, 0, 0, 255,   0, 0, 0, 0,     0, 0, 200, 255
  };
  unsigned char *out = sb_resample_lanczos3_rgba(src, 3, 3, 0, 0, 2, 3, 8, 6);
  assert(out);
  int translucent = 0;
  for (int i = 0; i < 8 * 6; i++) {
    unsigned char *p = out + i * 4;
    if (p[3] > 0 && p[3] < 255) {
      translucent++;
      assert(p[0] >= 245 && p[1] <= 2 && p[2] <= 2); /* no dark fringe from transparent pixel */
    }
  }
  assert(translucent);
  unsigned char *before = malloc(8 * 6 * 4); assert(before);
  memcpy(before, out, 8 * 6 * 4);
  sb_unsharp_mild_rgba(out, 8, 6);
  for (int i = 0; i < 8 * 6; i++)
    assert(out[i * 4 + 3] == before[i * 4 + 3]);
  free(before); free(out);
}

static void sharpness_is_bounded(void) {
  unsigned char px[9 * 9 * 4];
  for (int i = 0; i < 81; i++) {
    unsigned char v = i % 9 < 4 ? 70 : 180;
    px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = v;
    px[i * 4 + 3] = 255;
  }
  unsigned char orig[sizeof px]; memcpy(orig, px, sizeof px);
  sb_unsharp_mild_rgba(px, 9, 9);
  int altered = 0;
  for (size_t i = 0; i < sizeof px; i += 4) {
    for (int c = 0; c < 3; c++) {
      assert(abs((int)px[i + c] - orig[i + c]) <= 8);
      if (px[i + c] != orig[i + c]) altered = 1;
    }
    assert(px[i + 3] == orig[i + 3]);
  }
  assert(altered);
}

static void bounds(void) {
  unsigned char src[] = { 55, 62, 73, 255 };
  assert(!sb_resample_lanczos3_rgba(src, 1, 1, -1, 0, 1, 1, 2, 2));
  assert(!sb_resample_lanczos3_rgba(src, 1, 1, 0, 0, NAN, 1, 2, 2));
  assert(!sb_resample_lanczos3_rgba(src, 1, 1, 0, 0, 1, 1, 0, 2));
  assert(!sb_resample_lanczos3_rgba(src, 1, 1, 0, 0, 1, 1, 2, 0));
  assert(!sb_resample_lanczos3_rgba(src, 1, 1, 0, 0, 2, 1, 2, 2));
  same_colour(1, 1, 5, 5);
}

static void realistic_card(void) {
  const int w = 1920, h = 1080, tw = 500, th = 281;
  unsigned char *src = malloc((size_t)w * h * 4);
  assert(src);
  for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
    unsigned char *p = src + ((size_t)y * w + x) * 4;
    p[0] = (unsigned char)((x * 127 / w) + ((x + y) % 37));
    p[1] = (unsigned char)(y * 180 / h);
    p[2] = (unsigned char)((x / 16 + y / 16) % 2 ? 220 : 40);
    p[3] = 255;
  }
  clock_t before = clock();
  unsigned char *out = sb_resample_lanczos3_rgba(src, w, h, 0, 0, w, h, tw, th);
  assert(out);
  sb_unsharp_mild_rgba(out, tw, th);
  double seconds = (double)(clock() - before) / CLOCKS_PER_SEC;
  printf("1920x1080 -> 500x281 Lanczos3 + mild unsharp: %.3fs CPU\n", seconds);
  for (int i = 0; i < tw * th; i++) assert(out[i * 4 + 3] == 255);
  free(src); free(out);
}
int main(void) {
  same_colour(13, 11, 7, 6);
  same_colour(3, 4, 20, 14);
  alpha_and_crop();
  sharpness_is_bounded();
  bounds();
  realistic_card();
  puts("PASS Lanczos3, premultiplied alpha, crop, bounded unsharp, realistic card");
  return 0;
}
