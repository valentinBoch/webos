#include "ui/resample_hq.h"
#include <math.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* Separable Lanczos3. Tap positions/weights are generated only once per axis
   (never inside the per-pixel loop), with an expanded support when shrinking. */
typedef struct { int pos; float weight; } Tap;
typedef struct { int n; Tap *taps; } Axis;
static float clampf_(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static int clampi_(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static float lanczos3(float x) {
  float a = fabsf(x);
  if (a < 0.00001f) return 1;
  if (a >= 3) return 0;
  const float pi = 3.14159265358979323846f;
  float p = pi * x;
  return (sinf(p) / p) * (sinf(p / 3) / (p / 3));
}
static void axis_free(Axis *axes, int n) {
  if (!axes) return;
  for (int i = 0; i < n; i++) free(axes[i].taps);
  free(axes);
}
static Axis *axis_make(int srcSize, float start, float len, int dstSize) {
  Axis *axes = calloc((size_t)dstSize, sizeof *axes);
  if (!axes) return NULL;
  float scale = len / dstSize, stretch = fmaxf(1, scale);
  float radius = 3 * stretch;
  int low = clampi_((int)floorf(start), 0, srcSize - 1);
  int high = clampi_((int)ceilf(start + len) - 1, low, srcSize - 1);
  for (int p = 0; p < dstSize; p++) {
    float center = start + (p + 0.5f) * scale - 0.5f;
    int first = clampi_((int)ceilf(center - radius), low, high);
    int last = clampi_((int)floorf(center + radius), first, high);
    int count = last - first + 1;
    if (count < 1 || count > 192) { axis_free(axes, dstSize); return NULL; }
    axes[p].taps = malloc((size_t)count * sizeof(Tap));
    if (!axes[p].taps) { axis_free(axes, dstSize); return NULL; }
    float sum = 0;
    for (int i = 0; i < count; i++) {
      float weight = lanczos3(((first + i) - center) / stretch);
      axes[p].taps[i] = (Tap){ first + i, weight };
      sum += weight;
    }
    if (fabsf(sum) < 0.00001f) { /* Degenerate subpixel crop: nearest legal pixel. */
      axes[p].n = 1;
      axes[p].taps[0] = (Tap){ clampi_((int)floorf(center + 0.5f), low, high), 1 };
    } else {
      axes[p].n = count;
      for (int i = 0; i < count; i++) axes[p].taps[i].weight /= sum;
    }
  }
  return axes;
}

unsigned char *sb_resample_lanczos3_rgba(const unsigned char *src, int w, int h,
                                         float cx, float cy, float cw, float ch,
                                         int nw, int nh) {
  if (!src || w < 1 || h < 1 || nw < 1 || nh < 1 ||
      !isfinite(cx) || !isfinite(cy) || !isfinite(cw) || !isfinite(ch) ||
      cx < 0 || cy < 0 || cw <= 0 || ch <= 0 ||
      cx + cw > w + .001f || cy + ch > h + .001f ||
      (size_t)nw > SIZE_MAX / 4 / (size_t)nh ||
      (size_t)nw > SIZE_MAX / (size_t)h / (4 * sizeof(float))) return NULL;
  Axis *cols = axis_make(w, cx, cw, nw);
  Axis *rows = axis_make(h, cy, ch, nh);
  if (!cols || !rows) { axis_free(cols, nw); axis_free(rows, nh); return NULL; }
  /* Horizontal first minimizes work on the second axis for downscaling cards.
     Colours are premultiplied before convolution to avoid dark fringes. */
  float *tmp = malloc((size_t)nw * (size_t)h * 4 * sizeof(float));
  unsigned char *out = malloc((size_t)nw * nh * 4);
  if (!tmp || !out) { free(tmp); free(out); axis_free(cols, nw); axis_free(rows, nh); return NULL; }
  for (int y = 0; y < h; y++) for (int x = 0; x < nw; x++) {
    float *d = tmp + ((size_t)y * nw + x) * 4;
    d[0] = d[1] = d[2] = d[3] = 0;
    const Axis *a = cols + x;
    for (int k = 0; k < a->n; k++) {
      const Tap *t = &a->taps[k];
      const unsigned char *s = src + ((size_t)y * w + t->pos) * 4;
      float alpha = s[3] / 255.f, wt = t->weight;
      d[0] += wt * alpha * s[0]; d[1] += wt * alpha * s[1]; d[2] += wt * alpha * s[2];
      d[3] += wt * s[3];
    }
  }
  for (int y = 0; y < nh; y++) for (int x = 0; x < nw; x++) {
    float val[4] = { 0 };
    const Axis *a = rows + y;
    for (int k = 0; k < a->n; k++) {
      const Tap *t = &a->taps[k];
      const float *s = tmp + ((size_t)t->pos * nw + x) * 4;
      for (int c = 0; c < 4; c++) val[c] += t->weight * s[c];
    }
    unsigned char *d = out + ((size_t)y * nw + x) * 4;
    float alpha = clampf_(val[3], 0, 255);
    d[3] = (unsigned char)(alpha + 0.5f);
    for (int c = 0; c < 3; c++) {
      /* Clamp negative Lanczos lobes against alpha to avoid ringing/colour halos. */
      float v = alpha > 0.5f ? clampf_(val[c], 0, alpha) * 255 / alpha : 0;
      d[c] = (unsigned char)(v + 0.5f);
    }
  }
  free(tmp); axis_free(cols, nw); axis_free(rows, nh);
  return out;
}

void sb_unsharp_mild_rgba(unsigned char *px, int w, int h) {
  if (!px || w < 3 || h < 3 || (size_t)w > SIZE_MAX / 4 / (size_t)h) return;
  size_t bytes = (size_t)w * h * 4;
  unsigned char *base = malloc(bytes);
  if (!base) return; /* An allocation failure never makes artwork unavailable. */
  memcpy(base, px, bytes);
  static const int weights[3][3] = { { 1, 2, 1 }, { 2, 4, 2 }, { 1, 2, 1 } };
  for (int y = 1; y < h - 1; y++) for (int x = 1; x < w - 1; x++) {
    unsigned char *d = px + ((size_t)y * w + x) * 4;
    const unsigned char *s = base + ((size_t)y * w + x) * 4;
    if (s[3] != 255) continue;
    int allOpaque = 1;
    int sums[3] = { 0 };
    for (int j = -1; j <= 1; j++) for (int i = -1; i <= 1; i++) {
      const unsigned char *p = base + ((size_t)(y + j) * w + x + i) * 4;
      if (p[3] != 255) allOpaque = 0;
      for (int c = 0; c < 3; c++) sums[c] += weights[j + 1][i + 1] * p[c];
    }
    if (!allOpaque) continue;
    for (int c = 0; c < 3; c++) {
      float detail = (float)s[c] - sums[c] / 16.f;
      float delta = clampf_(detail * 0.15f, -8, 8);
      d[c] = (unsigned char)(clampf_(s[c] + delta, 0, 255) + 0.5f);
    }
  }
  free(base);
}
