#include "platform/plat.h"
#include "ui/img.h"
#include <SDL2/SDL.h>
#include "ui/gfx.h"
#include "core/net.h"
#include "core/store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <math.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_GIF
#include "stb_image.h"
#include "ui/palette.h"
#include "stb_image_write.h"
#include "src/webp/decode.h"
#include "ui/resample_hq.h"

/* area-average resample of the part (cx, cy, cw, ch) of the source to nw x nh (the pictures only ever
   shrink: posters arrive at 500x750 for a 264x396 card; a smaller source repeats pixels) */
static unsigned char *resample(const unsigned char *src, int w, int h, float cx, float cy, float cw, float ch, int nw, int nh) {
  unsigned char *out = malloc((size_t)nw * nh * 4); if (!out) return NULL;
  for (int y = 0; y < nh; y++) { int y0 = (int)(cy + y * ch / nh), y1 = (int)(cy + (y + 1) * ch / nh); if (y1 <= y0) y1 = y0 + 1; if (y0 >= h) y0 = h - 1; if (y1 > h) y1 = h;
    for (int x = 0; x < nw; x++) { int x0 = (int)(cx + x * cw / nw), x1 = (int)(cx + (x + 1) * cw / nw); if (x1 <= x0) x1 = x0 + 1; if (x0 >= w) x0 = w - 1; if (x1 > w) x1 = w;
      unsigned r = 0, g = 0, b = 0, a = 0, n = 0;
      for (int yy = y0; yy < y1; yy++) { const unsigned char *p = src + ((size_t)yy * w + x0) * 4; for (int xx = x0; xx < x1; xx++, p += 4) { r += p[0]; g += p[1]; b += p[2]; a += p[3]; n++; } }
      unsigned char *o = out + ((size_t)y * nw + x) * 4; if (!n) n = 1; o[0] = (unsigned char)(r / n); o[1] = (unsigned char)(g / n); o[2] = (unsigned char)(b / n); o[3] = (unsigned char)(a / n); } }
  return out;
}
static unsigned char *downscale(const unsigned char *src, int w, int h, int nw, int nh) { return resample(src, w, h, 0, 0, (float)w, (float)h, nw, nh); }
/* rounded corners into the alpha (1 px of smoothing, the colour is kept so filtering leaves no dark rim) */
static void round_corners(unsigned char *px, int w, int h, float r) {
  if (r <= 0) return; if (r > w / 2.0f) r = w / 2.0f; if (r > h / 2.0f) r = h / 2.0f;
  int n = (int)ceilf(r) + 1;
  for (int y = 0; y < n && y < h; y++) for (int x = 0; x < n && x < w; x++) {
    float dx = r - (x + 0.5f), dy = r - (y + 0.5f); if (dx < 0 || dy < 0) continue;
    float d = sqrtf(dx * dx + dy * dy) - r, k = 0.5f - d; if (k >= 1) continue; if (k < 0) k = 0;
    int xs[2] = { x, w - 1 - x }, ys[2] = { y, h - 1 - y };
    for (int i = 0; i < 2; i++) for (int j = 0; j < 2; j++) { unsigned char *p = px + ((size_t)ys[j] * w + xs[i]) * 4; p[3] = (unsigned char)(p[3] * k + 0.5f); } }
}

typedef struct Img {
  char *url; unsigned hash; int w, h;           /* the cache key (a shaped variant carries its shape in it) and the requested size */
  char *src; int fit; float radius;             /* the picture's own address; fit -1 = as it is, else the shape (img_get_shaped) */
  int state;                                    /* 0 queued, 1 loading, 2 ready, 3 failed */
  unsigned char *px; int pw, ph, pch;           /* decoded pixels waiting for upload */
  GLuint tex; float aspect; int bytes;
  GLuint upTex; int upRow;                      /* a large picture uploading in bands over several frames */
  int urgent;                                   /* the picture the screen waits on (a backdrop): it goes up first, in bigger bands */
  int pin;                                      /* never evicted (title art: small, wanted at once) */
  struct Img *dnext; int inDone;                /* decoded, waiting for upload (inDone: in that list now) */
  int wantPal, palAlpha, palReady; Palette pal;       /* the colours of the picture, asked for by the accent */
  int wantTex;                                  /* someone draws it (a picture only asked for its colours is never uploaded) */
  unsigned lastUse;
  int fails; double failAt;                     /* failed loads so far, and when the last one failed: asked again, it tries once more after a wait */
  struct Img *next;
} Img;
#define BUCKETS 1024
static Img *table[BUCKETS];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
/* what to load, newest first: what the screen asks for now goes before what a fast scroll asked for and left behind
   (a first-come queue showed placeholders while passed titles loaded); title art has its own stack, served first */
static Img *stk[1024], *pstk[128]; static int sn, psn, qn;
static Img *urg;   /* the one picture the screen waits on (the backdrop): taken before both stacks */
static void q_push(Img *im, int pri) { if (pri && psn < 128) pstk[psn++] = im; else stk[sn++] = im; qn++; } /* under mu; room checked by the caller */
/* decoded pictures waiting for upload: a list without a limit (a full array dropped them, and a dropped picture held
   its pixels and never showed: a backdrop waiting for one never changed again) */
static Img *doneHead; static int dn;
static void done_push(Img *im) { if (im->inDone) return; im->inDone = 1; im->dnext = doneHead; doneHead = im; dn++; }   /* under mu; never twice (it would point to itself and the list never end) */
static Img *done_pop(void) { /* an urgent one first (a backdrop is never kept behind posters), else the newest */
  Img **pp = &doneHead; for (Img **q = &doneHead; *q; q = &(*q)->dnext) if ((*q)->urgent) { pp = q; break; }
  Img *im = *pp; if (im) { *pp = im->dnext; im->dnext = NULL; im->inDone = 0; dn--; } return im; }
#ifdef SB_WEBOS
static unsigned frameNo; static long usedBytes, budgetBytes = 40L * 1024 * 1024; /* the TVs have little memory to spare */
#else
static unsigned frameNo; static long usedBytes, budgetBytes = 96L * 1024 * 1024;
#endif
static char cacheDir[1024];
#define WORKERS 6   /* mostly waiting on the network; the decoding itself is limited to two at a time (dec_enter) */

static unsigned fnv(const char *s) { unsigned h = 2166136261u; while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; } return h; }
static Img *find(const char *url, unsigned h) { for (Img *i = table[h % BUCKETS]; i; i = i->next) if (i->hash == h && !strcmp(i->url, url)) return i; return NULL; }

/* ---- worker: fetch (cache first), decode, scale ---- */
/* the picture as it is drawn is kept too, re-encoded small (a poster at its card size is ~15 KB of JPEG, logos a
   small PNG for their transparency): the next visit decodes that in a few ms instead of the full-size original */
typedef struct { unsigned char *b; size_t n, cap; } WBuf;
static void wbuf_put(void *ctx, void *data, int size) { WBuf *w = ctx; if (w->n + size > w->cap) { w->cap = (w->n + size) * 2; w->b = realloc(w->b, w->cap); } memcpy(w->b + w->n, data, size); w->n += size; }
/* Separate cache so the high-quality Continue Watching cards never load an old
   JPEG90/area-average shaped texture. Other card caches remain unchanged. */
static void resume_small_save(const Img *im, const unsigned char *px, int w, int h) {
  if (!strncmp(im->src, "file://", 7) || w * h > 1280 * 720) return;
  int opaque = 1;
  for (long i = 3, n = (long)w * h * 4; i < n; i += 4)
    if (px[i] != 255) { opaque = 0; break; }
  WBuf b = { 0 }; int ok = 0;
  if (opaque) {
    unsigned char *rgb = malloc((size_t)w * h * 3);
    if (!rgb) return;
    for (long i = 0, n = (long)w * h; i < n; i++) {
      rgb[3 * i] = px[4 * i]; rgb[3 * i + 1] = px[4 * i + 1]; rgb[3 * i + 2] = px[4 * i + 2];
    }
    ok = stbi_write_jpg_to_func(wbuf_put, &b, w, h, 3, rgb, 96);
    free(rgb);
  } else ok = stbi_write_png_to_func(wbuf_put, &b, w, h, 4, px, w * 4);
  if (ok && b.n) {
    char key[2400];
    snprintf(key, sizeof key, "sm-resume-lanczos3-v1:%dx%d:%s", im->w, im->h, im->url);
    cache_write(key, b.b, b.n);
  }
  free(b.b);
}
static void small_save(const Img *im, const unsigned char *px, int w, int h) {
  if (!strncmp(im->src, "file://", 7) || w * h > 1280 * 720) return;
  int opaque = 1; for (long i = 3, n = (long)w * h * 4; i < n; i += 4) if (px[i] != 255) { opaque = 0; break; }
  WBuf b = { 0 }; int ok;
  if (opaque) { unsigned char *rgb = malloc((size_t)w * h * 3); for (long i = 0, n = (long)w * h; i < n; i++) { rgb[i * 3] = px[i * 4]; rgb[i * 3 + 1] = px[i * 4 + 1]; rgb[i * 3 + 2] = px[i * 4 + 2]; }
    ok = stbi_write_jpg_to_func(wbuf_put, &b, w, h, 3, rgb, 90); free(rgb); }
  else ok = stbi_write_png_to_func(wbuf_put, &b, w, h, 4, px, w * 4);
  if (ok && b.n) { char key[2400]; snprintf(key, sizeof key, "sm:%dx%d:%s", im->w, im->h, im->url); cache_write(key, b.b, b.n); }
  free(b.b);
}
static int resume_small_load(Img *im) {
  char key[2400];
  snprintf(key, sizeof key, "sm-resume-lanczos3-v1:%dx%d:%s", im->w, im->h, im->url);
  size_t len = 0; unsigned char *data = cache_read(key, 14 * 86400, &len);
  if (!data) return 0;
  int w, h, ch;
  unsigned char *px = stbi_load_from_memory(data, (int)len, &w, &h, &ch, 4);
  free(data);
  if (!px) return 0;
  if (w != im->w || h != im->h) { free(px); return 0; }
  if (im->wantPal && !im->palReady) {
    int st = 1; while ((w / st) * (h / st) > 40000) st++;
    palette_of(px, w, h, im->palAlpha, st, &im->pal); im->palReady = 1;
  }
  round_corners(px, w, h, im->radius);
  im->aspect = (float)w / h;
  im->px = px; im->pw = w; im->ph = h; im->pch = 4;
  return 1;
}
static int small_load(Img *im) {
  char key[2400]; snprintf(key, sizeof key, "sm:%dx%d:%s", im->w, im->h, im->url);
  size_t len = 0; unsigned char *data = cache_read(key, 14 * 86400, &len); if (!data) return 0;
  int w, h, ch; unsigned char *px = stbi_load_from_memory(data, (int)len, &w, &h, &ch, 4); free(data);
  if (!px) return 0;
  if (im->wantPal && !im->palReady) { int st = 1; while ((w / st) * (h / st) > 40000) st++; palette_of(px, w, h, im->palAlpha, st, &im->pal); im->palReady = 1; }
  if (im->fit >= 0) { round_corners(px, w, h, im->radius); im->aspect = (float)im->w / im->h; } else im->aspect = (float)w / h;
  im->px = px; im->pw = w; im->ph = h; im->pch = 4; return 1;
}
static pthread_mutex_t decMu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t decCv = PTHREAD_COND_INITIALIZER; static int decBusy;
static void dec_enter(void) { pthread_mutex_lock(&decMu); while (decBusy >= 2) pthread_cond_wait(&decCv, &decMu); decBusy++; pthread_mutex_unlock(&decMu); }
static void dec_leave(void) { pthread_mutex_lock(&decMu); decBusy--; pthread_cond_signal(&decCv); pthread_mutex_unlock(&decMu); }
static void load_(Img *im);
static void decode_part(Img *im, unsigned char *data, size_t len, int fromNet, const char *key);
static double mono_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void load(Img *im) { dec_enter(); int ok = im->fit == 3 ? resume_small_load(im) : small_load(im); dec_leave(); if (!ok) load_(im); }
static void load_(Img *im) {
  char key[2048]; snprintf(key, sizeof key, "img:%s", im->src);
  size_t len = 0; unsigned char *data = cache_read(key, 14 * 86400, &len);
  int fromNet = 0;
  if (!data && !strncmp(im->src, "file://", 7)) { FILE *f = fopen(im->src + 7, "rb"); if (f) { fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); if (n > 0) { data = malloc(n); len = fread(data, 1, n, f); } fclose(f); } } /* a bundled picture */
  else if (!data) { int st; data = net_fetch_sync(im->src, &len, &st, 20000); fromNet = 1; }
  if (!data || len < 64) { fprintf(stderr, "[img] failed %s\n", im->src); free(data); im->fails++; im->failAt = mono_s(); im->state = 3; return; }
  dec_enter(); decode_part(im, data, len, fromNet, key); dec_leave();
}
static void decode_part(Img *im, unsigned char *data, size_t len, int fromNet, const char *key) {
  int w, h, ch = 4; unsigned char *px;
  if (len > 12 && !memcmp(data, "RIFF", 4) && !memcmp(data + 8, "WEBP", 4)) { px = WebPDecodeRGBA(data, len, &w, &h); } /* metahub serves WebP */
  else px = stbi_load_from_memory(data, (int)len, &w, &h, &ch, 4);
  if (px && fromNet) cache_write(key, data, len);
  free(data);
  if (!px) { fprintf(stderr, "[img] undecodable %s\n", im->src); im->fails++; im->failAt = mono_s(); im->state = 3; return; }
  if (im->wantPal && !im->palReady) { int st = 1; while ((w / st) * (h / st) > 40000) st++; palette_of(px, w, h, im->palAlpha, st, &im->pal); im->palReady = 1; }
  if (im->fit == 3) { /* only wide Continue Watching cards: Lanczos3, mild unsharp, JPEG96 */
    float ba = (float)im->w / im->h, ta = (float)w / h;
    float cx = 0, cy = 0, cw = (float)w, ch = (float)h;
    if (ta > ba) { cw = h * ba; cx = (w - cw) / 2; }
    else if (ta < ba) { ch = w / ba; cy = (h - ch) / 2; }
    unsigned char *out = sb_resample_lanczos3_rgba(px, w, h, cx, cy, cw, ch, im->w, im->h);
    if (out) {
      free(px);
      sb_unsharp_mild_rgba(out, im->w, im->h);
      resume_small_save(im, out, im->w, im->h);
      round_corners(out, im->w, im->h, im->radius);
      im->px = out; im->pw = im->w; im->ph = im->h; im->pch = 4; im->aspect = ba;
      return;
    }
    /* Allocation failure: keep the existing area/bilinear path. */
  }
  if (im->fit >= 0) { /* the exact box: crop (or stretch), scale, round the corners */
    float cx = 0, cy = 0, cw = (float)w, ch = (float)h, ba = (float)im->w / im->h, ta = (float)w / h;
    if (im->fit > 0) { if (ta > ba) { cw = h * ba; cx = (w - cw) / 2; } else if (ta < ba) { ch = w / ba; cy = im->fit == 2 ? 0 : (h - ch) / 2; } }
    unsigned char *out = resample(px, w, h, cx, cy, cw, ch, im->w, im->h);
    free(px); if (!out) { im->state = 3; return; }
    small_save(im, out, im->w, im->h); /* before the corners: they are put back on load */
    round_corners(out, im->w, im->h, im->radius);
    im->px = out; im->pw = im->w; im->ph = im->h; im->pch = 4; im->aspect = ba; return;
  }
  /* scale down to the shown size (cover: the smaller of the two scale factors keeps it covering) */
  int tw = im->w, th = im->h; float sx = (float)tw / w, sy = (float)th / h, s = sx > sy ? sx : sy;
  if (s < 1.0f) {
    int nw = (int)(w * s + 0.5f), nh = (int)(h * s + 0.5f); if (nw < 1) nw = 1; if (nh < 1) nh = 1;
    unsigned char *out = downscale(px, w, h, nw, nh);
    if (out) { free(px); px = out; w = nw; h = nh; }
  }
  small_save(im, px, w, h);
  im->px = px; im->pw = w; im->ph = h; im->pch = 4; im->aspect = (float)w / h;
}
static void *worker(void *a) { plat_thread_background(10); (void)a;
  for (;;) {
    pthread_mutex_lock(&mu);
    while (!qn) pthread_cond_wait(&cv, &mu);
    Img *im = urg ? urg : psn ? pstk[--psn] : stk[--sn]; qn--; if (im == urg) urg = NULL;
    if (im->state == 1 || im->inDone) { pthread_mutex_unlock(&mu); continue; } /* queued twice (urgent and in a queue): another worker has it already */
    /* asked for, then nobody asked again for 1.5 s (scrolled past): not loaded now; the next ask queues it again */
    if (!im->pin && !im->wantPal && frameNo - im->lastUse > 90) { im->state = 2; im->wantTex = 0; pthread_mutex_unlock(&mu); continue; }
    im->state = 1;
    pthread_mutex_unlock(&mu);
    load(im);
    pthread_mutex_lock(&mu);
    if (im->state != 3) im->state = 2;
    done_push(im);
    pthread_mutex_unlock(&mu);
  }
  return NULL;
}
int img_init(const char *dir) {
  snprintf(cacheDir, sizeof cacheDir, "%s", dir);
  for (int i = 0; i < WORKERS; i++) { pthread_t t; pthread_create(&t, NULL, worker, NULL); pthread_detach(t); }
  return 1;
}
static float drawScale = 1.0f;
void img_set_draw_scale(float sc) { drawScale = sc > 0 && sc < 1 ? sc : 1.0f; }
void img_set_budget_mb(int mb) { budgetBytes = (long)mb * 1024 * 1024; }
int img_budget_mb(void) { return (int)(budgetBytes / (1024 * 1024)); }
void img_queue_stats(int *queued, int *waiting) { pthread_mutex_lock(&mu); *queued = qn; *waiting = dn; pthread_mutex_unlock(&mu); }
int img_used_mb(void) { return (int)(usedBytes / 1048576); }
void img_frame(void) { frameNo++; }

/* a full queue leaves the picture waiting (state 5), asked again on its next request, never failed for good */
static void enqueue_at(Img *im, int front) { pthread_mutex_lock(&mu);
  if (sn < 1024) { q_push(im, front); im->state = 0; pthread_cond_signal(&cv); } else im->state = 5;
  pthread_mutex_unlock(&mu); }
static void enqueue(Img *im) { enqueue_at(im, 0); }
static Img *request_as(const char *key, const char *src, int w, int h, int fit, float radius) {
  unsigned hs = fnv(key); Img *im = find(key, hs);
  if (im) { im->lastUse = frameNo; if (im->state == 5) enqueue(im);
    if (im->state == 3 && im->fails > 0 && im->fails < 6 && mono_s() - im->failAt > 1.5 * (1 << (im->fails - 1))) { fprintf(stderr, "[img] again (%d) %s\n", im->fails, im->src); enqueue(im); } /* failed (a network blip): asked for again, it tries again after 1.5, 3, 6, 12, 24 s */
    if (!im->wantTex) { im->wantTex = 1; if (im->state == 2 && !im->tex && !im->px) { im->state = 0; enqueue(im); } } /* it was loaded for its colours only: now load it to draw */
    return im; }
  if (drawScale < 1) { w = (int)(w * drawScale + 0.999f); h = (int)(h * drawScale + 0.999f); radius *= drawScale; }
  im = calloc(1, sizeof *im); im->url = strdup(key); im->src = key == src ? im->url : strdup(src); im->hash = hs; im->w = w; im->h = h; im->fit = fit; im->radius = radius; im->lastUse = frameNo;
  im->wantTex = 1;
  im->next = table[hs % BUCKETS]; table[hs % BUCKETS] = im;
  enqueue(im);
  return im;
}
static Img *request(const char *url, int w, int h) { return request_as(url, url, w, h, -1, 0); }
/* cache keys are built for every label / picture in every frame: printf's float formatting was a tenth of the
   drawing thread's time on the TV */
static char *key_int(char *p, long v) { char t[24]; int n = 0; unsigned long u = v < 0 ? (unsigned long)(-v) : (unsigned long)v; do { t[n++] = (char)('0' + u % 10); u /= 10; } while (u); if (v < 0) *p++ = '-'; while (n) *p++ = t[--n]; return p; }
static long key_round(float f) { return (long)(f >= 0 ? f + 0.5f : f - 0.5f); }
static const char *shaped_key(char *b, size_t cap, const char *url, int w, int h, float radius, int fit) {
  char *q = b; q = key_int(q, w); *q++ = 'x'; q = key_int(q, h); *q++ = ':'; q = key_int(q, fit); *q++ = ':'; q = key_int(q, key_round(radius * 10)); *q++ = '|';
  size_t n = strlen(url), room = cap - (size_t)(q - b) - 1; if (n > room) n = room; memcpy(q, url, n); q[n] = 0; return b; }
GLuint img_get_shaped(const char *url, int w, int h, float radius, int fit) {
  if (!url || !*url || w <= 0 || h <= 0) return 0;
  char k[2300]; Img *im = request_as(shaped_key(k, sizeof k, url, w, h, radius, fit), url, w, h, fit, radius);
  return im->tex;
}
int img_shaped_loaded(const char *url, int w, int h, float radius, int fit) { char k[2300]; shaped_key(k, sizeof k, url, w, h, radius, fit); Img *im = find(k, fnv(k)); return im && im->tex; }
int img_shaped_failed(const char *url, int w, int h, float radius, int fit) { char k[2300]; shaped_key(k, sizeof k, url, w, h, radius, fit); Img *im = find(k, fnv(k)); return im && im->state == 3; }
GLuint img_get(const char *url, int w, int h, float *aspect) {
  if (!url || !*url) return 0;
  Img *im = request(url, w, h);
  if (aspect) *aspect = im->aspect > 0 ? im->aspect : (float)w / h;
  return im->tex;
}
void img_prefetch(const char *url, int w, int h) { if (url && *url && usedBytes < budgetBytes * 85 / 100) request(url, w, h); } /* looking ahead only while there is room: it never pushes out what is on screen */
/* the address a plain picture on the screen was loaded from (the player shows the very logo the page showed) */
const char *img_src_of(GLuint tex) { if (!tex) return NULL; for (int b = 0; b < BUCKETS; b++) for (Img *i = table[b]; i; i = i->next) if (i->tex == tex && i->fit < 0) return i->src; return NULL; }
/* the picture the screen is waiting on (the backdrop of the focused title): out of the stacks into the slot taken
   next, so a fast scroll's posters, asked after it, never hold it back; the one it replaces goes back on top */
void img_urgent(const char *url, int w, int h) {
  if (!url || !*url) return; Img *im = request(url, w, h); im->urgent = 1; if (im->state != 0 || im == urg) return;
  pthread_mutex_lock(&mu);
  int found = 0; for (int i = sn - 1; i >= 0 && !found; i--) if (stk[i] == im) { memmove(stk + i, stk + i + 1, (size_t)(sn - i - 1) * sizeof *stk); sn--; found = 1; }
  for (int i = psn - 1; i >= 0 && !found; i--) if (pstk[i] == im) { memmove(pstk + i, pstk + i + 1, (size_t)(psn - i - 1) * sizeof *pstk); psn--; found = 1; }
  if (found) { if (urg) { if (sn < 1024) stk[sn++] = urg; else pstk[psn++] = urg; } urg = im; } /* (a picture already taken by a worker is not in them: nothing to do) */
  pthread_mutex_unlock(&mu);
}
/* title art: decoded before the posters waiting in the queue, and kept */
void img_prefetch_pri(const char *url, int w, int h) {
  if (!url || !*url) return; Img *im = find(url, fnv(url));
  if (!im) { im = request(url, w, h); if (im->state == 0) { pthread_mutex_lock(&mu); /* move it from the back to the front */
      if (sn && stk[sn - 1] == im) { sn--; qn--; } pthread_mutex_unlock(&mu); enqueue_at(im, 1); } }
  else im->lastUse = frameNo;
  /* the last 32 kept (a logo at its drawn size is ~360 KB: about 12 MB of the picture budget at most) */
  static Img *pinned[32]; static int pn; if (!im->pin) { if (pinned[pn]) pinned[pn]->pin = 0; pinned[pn] = im; pn = (pn + 1) % 32; im->pin = 1; }
}
/* the colours of a picture: 1 when known (out filled), 0 while it loads, -1 when it cannot be loaded */
int img_palette(const char *url, int w, int h, int alpha, Palette *out) {
  if (!url || !*url) return -1;
  Img *im = find(url, fnv(url));
  if (!im) { im = request(url, w, h); im->wantTex = 0; } /* only its colours: decoded, never uploaded */
  if (im->state == 3) return -1;
  if (im->palReady) { *out = im->pal; return 1; }
  if (!im->wantPal) { im->wantPal = 1; im->palAlpha = alpha;
    if ((im->state == 2 || im->tex) && !im->px) { /* already decoded without it: load again for the colours (not while its pixels wait to go up) */
      pthread_mutex_lock(&mu); if (sn < 1024) { q_push(im, 0); pthread_cond_signal(&cv); } pthread_mutex_unlock(&mu); } }
  return 0;
}
int img_failed(const char *url) { Img *im = find(url, fnv(url)); return im && im->state == 3; }
int img_loaded(const char *url) { Img *im = find(url, fnv(url)); return im && im->tex; }

/* uploads a few finished decodes per frame (each upload is a copy into GPU memory) */
/* uploads take about 3/4 MB a frame: the TV's driver rearranges every texel on the CPU as it uploads (a 1280x720
   backdrop in one go was a 25-30 ms frame), so a large picture goes up in bands of rows over a few frames and is
   only shown once complete */
#define UP_BUDGET (320L * 1024) /* this TV converts ~100 KB a millisecond for its GPU: ~3 ms a frame */
#define UP_URGENT (768L * 1024) /* a backdrop: in a few frames, as before (its fade waits for the whole picture) */
static int upLog = -1; /* SB_UPLOG: each upload's size and time (development) */
static void up_log(const char *what, long bytes, Uint64 t0) { if (upLog < 0) upLog = getenv("SB_UPLOG") != NULL; if (!upLog) return; double ms = (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / SDL_GetPerformanceFrequency(); fprintf(stderr, "[up] %s %ld KB %.1f ms\n", what, bytes / 1024, ms); }
static Img *upCur;
static void up_done(Img *im) { im->tex = im->upTex; im->upTex = 0; gfx_tex_fade_in(im->tex); im->bytes = im->pw * im->ph * im->pch; usedBytes += im->bytes; free(im->px); im->px = NULL; im->lastUse = frameNo; }
static long up_band(Img *im, long budget) { /* rows up to the budget, in whole 16-row blocks; what was sent */
  long row = (long)im->pw * im->pch; int rows = (int)(budget / row); rows = rows < 16 ? 16 : rows & ~15; if (im->upRow + rows > im->ph) rows = im->ph - im->upRow;
  Uint64 u0 = SDL_GetPerformanceCounter(); gfx_tex_update(im->upTex, im->px + (size_t)im->upRow * row, 0, im->upRow, im->pw, rows, im->pch); up_log("band", (long)rows * row, u0); im->upRow += rows;
  if (im->upRow >= im->ph) { up_done(im); upCur = NULL; }
  return rows * row;
}
void img_pump(void) {
  long sent = 0;
  if (upCur) { if (!upCur->wantTex) { gfx_tex_recycle(upCur->upTex, upCur->pw, upCur->ph, upCur->pch, 1); upCur->upTex = 0; free(upCur->px); upCur->px = NULL; upCur->state = 0; upCur = NULL; } else { sent += up_band(upCur, upCur->urgent ? UP_URGENT : UP_BUDGET); if (upCur) return; } }
  Uint64 t0 = SDL_GetPerformanceCounter(); double hz = (double)SDL_GetPerformanceFrequency();
  for (int n = 0; n < 6 && sent < UP_BUDGET; n++) {
    if (sent && (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / hz > 3.0) return; /* first uploads also allocate: time counts too */
    pthread_mutex_lock(&mu); Img *im = done_pop(); pthread_mutex_unlock(&mu);
    if (!im) return;
    if (im->px && (im->tex || !im->wantTex)) { free(im->px); im->px = NULL; continue; } /* a reload for the colours only, or nobody draws it */
    if (!im->px) continue;
    long bytes = (long)im->pw * im->ph * im->pch;
    long cap = im->urgent ? UP_URGENT : UP_BUDGET;
    if (bytes <= cap - sent || (!sent && bytes <= cap + cap / 4)) { sent += bytes; Uint64 u0 = SDL_GetPerformanceCounter(); im->upTex = gfx_tex_create(im->px, im->pw, im->ph, im->pch, 1); up_log("whole", bytes, u0); up_done(im); continue; }
    if (sent) { pthread_mutex_lock(&mu); done_push(im); pthread_mutex_unlock(&mu); return; } /* next frame, first */
    { Uint64 u0 = SDL_GetPerformanceCounter(); im->upTex = gfx_tex_create(NULL, im->pw, im->ph, im->pch, 1); up_log("alloc", bytes, u0); } im->upRow = 0; upCur = im; up_band(im, cap); return;
  }
}
void img_release_unused(int keepMb) {
  long keep = keepMb > 0 ? (long)keepMb * 1024 * 1024 : budgetBytes;
  if (usedBytes <= keep) return;
  /* drop the least recently used textures until under the budget; keep what was drawn in the last few frames */
  for (int pass = 0; pass < 8 && usedBytes > keep; pass++) {
    unsigned oldest = 0xffffffffu;
    for (int b = 0; b < BUCKETS; b++) for (Img *i = table[b]; i; i = i->next) if (i->tex && !i->pin && i->lastUse < oldest) oldest = i->lastUse;
    if (oldest == 0xffffffffu || frameNo - oldest < (pass < 4 ? 120u : 3u)) return; /* what was on screen in the last 2 s stays (a step back never reloads it); only a real overflow takes more */
    for (int b = 0; b < BUCKETS; b++) for (Img *i = table[b]; i; i = i->next)
      if (i->tex && !i->pin && i->lastUse <= oldest + 30) { gfx_tex_recycle(i->tex, i->pw, i->ph, i->pch, 1); i->tex = 0; usedBytes -= i->bytes; i->bytes = 0;
        /* loaded again (from its small copy) only when it is asked for again: queueing it at once made a loop under
           memory pressure (decode, upload, evict, decode...) that filled the queue, and a new backdrop could not get in */
        i->state = 2; i->wantTex = 0; }
  }
}
void img_cover_uv(float ta, float ba, int top, float *u0, float *v0, float *u1, float *v1) {
  *u0 = 0; *v0 = 0; *u1 = 1; *v1 = 1;
  if (ta > ba) { float k = ba / ta; *u0 = (1 - k) / 2; *u1 = *u0 + k; }  /* texture wider than the box: crop the sides */
  else if (ta < ba) { float k = ta / ba; if (top) { *v0 = 0; *v1 = k; } else { *v0 = (1 - k) / 2; *v1 = *v0 + k; } }
}
