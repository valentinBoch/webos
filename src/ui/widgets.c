#include "ui/widgets.h"
#include "ui/sound.h"
#include "ui/shared.h"
#include "ui/gfx.h"
#include "ui/text.h"
#include "ui/img.h"
#include "ui/icon.h"
#include "ui/atlas.h"
#include "core/app.h"
#include "core/keys.h"
#include "core/i18n.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>
static char markId[64]; static double markAt; /* the title just marked as watched: its badge pops in (card_mark_pop) */

void glass_rrect(float x, float y, float w, float h, float r, uint32_t base, float sheenTop, float sheenBottom, float edge) {
  gfx_rrect(x, y, w, h, r, base);
  if (sheenTop > 0 || sheenBottom > 0) gfx_rrect_v(x, y, w, h, r, WHITE_A((int)(sheenTop * 255 + 0.5f)), WHITE_A((int)(sheenBottom * 255 + 0.5f)));
  if (edge > 0) gfx_ring(x, y, w, h, r, 1, WHITE_A((int)(edge * 255 + 0.5f)));
}

/* ---- the meta line ---- */
static void meta_parse(const char *ml, const char *genres, const char *name, MetaParts *p) {
  memset(p, 0, sizeof *p); if (!ml) ml = "";
  if (name && *name) snprintf(p->name, sizeof p->name, "%s", name);
  /* IMDb 8.5 */
  const char *im = strstr(ml, "IMDb"); if (!im) im = strstr(ml, "imdb");
  char noImdb[256]; snprintf(noImdb, sizeof noImdb, "%s", ml);
  if (im) { const char *q = im + 4; while (*q == ' ') q++; int i = 0; while ((isdigit((unsigned char)*q) || *q == '.') && i < 15) p->imdb[i++] = *q++; p->imdb[i] = 0;
    if (!i) p->imdb[0] = 0; else { size_t a = im - ml; memmove(noImdb + a, noImdb + a + (q - im), strlen(noImdb + a + (q - im)) + 1); } }
  /* a year or a span */
  for (const char *q = ml; *q; q++) { if (isdigit((unsigned char)q[0]) && isdigit((unsigned char)q[1]) && isdigit((unsigned char)q[2]) && isdigit((unsigned char)q[3]) && !isdigit((unsigned char)q[4]) && (q == ml || !isdigit((unsigned char)q[-1]))) {
      int i = 0; for (int k = 0; k < 4; k++) p->year[i++] = q[k]; const char *r = q + 4; while (*r == ' ') r++;
      if (*r == '-' || (unsigned char)r[0] == 0xe2) { const char *d = r; if ((unsigned char)*d == 0xe2) d += 3; else d++; while (*d == ' ') d++;
        p->year[i++] = '-'; if (isdigit((unsigned char)d[0]) && isdigit((unsigned char)d[1]) && isdigit((unsigned char)d[2]) && isdigit((unsigned char)d[3])) for (int k = 0; k < 4; k++) p->year[i++] = d[k]; }
      p->year[i] = 0; break; } }
  int rt = sb_runtime_min(noImdb); if (rt) sb_fmt_runtime(rt, p->rt, sizeof p->rt);
  if (!p->imdb[0] && !p->year[0] && !p->rt[0] && *ml) snprintf(p->rest, sizeof p->rest, "%s", ml);
  if (genres && *genres) snprintf(p->genres, sizeof p->genres, "%s", genres);
}
#define MDOT 38.0f  /* 16 + 6 + 16 */
static float meta_draw_at(const MetaParts *p, float x, float y, int px, uint32_t color, int draw) {
  float cx = x, badgePx = px * 0.74f; int first = 1;
  float lh = text_line_height(px);
  #define SEP() do { if (!first) { if (draw) gfx_circle(cx + 16 + 3, y + lh / 2 - 2, 3, WHITE_A(107)); cx += MDOT; } first = 0; } while (0)
  if (p->name[0]) { SEP(); float w = draw ? text_draw(p->name, cx, y, px, 800, WHITE_A(255), 0, 0, ALIGN_LEFT) : text_width(p->name, px, 800, 0); cx += w; }
  if (p->imdb[0]) { SEP(); float bw = text_width("IMDb", (int)badgePx, 800, -0.4f) + 14, bh = badgePx + 9;
    if (draw) { gfx_rrect(cx, y + lh / 2 - bh / 2 - 2, bw, bh, 5, RGB(0xf5, 0xc5, 0x18)); text_draw("IMDb", cx + 7, y + lh / 2 - bh / 2 - 2 + 4 - (text_line_height((int)badgePx) - badgePx) / 2, (int)badgePx, 800, RGB(0, 0, 0), -0.4f, 0, ALIGN_LEFT); }
    cx += bw + 10; float w = draw ? text_draw(p->imdb, cx, y, px, 700, WHITE_A(255), 0, 0, ALIGN_LEFT) : text_width(p->imdb, px, 700, 0); cx += w; }
  if (p->year[0]) { SEP(); float w = draw ? text_draw(p->year, cx, y, px, 600, color, 0.2f, 0, ALIGN_LEFT) : text_width(p->year, px, 600, 0.2f); cx += w; }
  if (p->rt[0]) { SEP(); float w = draw ? text_draw(p->rt, cx, y, px, 600, color, 0.2f, 0, ALIGN_LEFT) : text_width(p->rt, px, 600, 0.2f); cx += w; }
  if (p->rest[0]) { SEP(); float w = draw ? text_draw(p->rest, cx, y, px, 600, color, 0.2f, 0, ALIGN_LEFT) : text_width(p->rest, px, 600, 0.2f); cx += w; }
  if (p->genres[0]) { SEP(); float w = draw ? text_draw(p->genres, cx, y, px, 600, WHITE_A(179), 0.2f, 0, ALIGN_LEFT) : text_width(p->genres, px, 600, 0.2f); cx += w; }
  #undef SEP
  return cx - x;
}
static float meta_place(const MetaParts *p, float w, float x, float y, int px, uint32_t color, int align) {
  if (align == ALIGN_START) align = app_rtl() ? ALIGN_RIGHT : ALIGN_LEFT; if (align == ALIGN_END) align = app_rtl() ? ALIGN_LEFT : ALIGN_RIGHT;
  float x0 = align == ALIGN_CENTER ? x - w / 2 : align == ALIGN_RIGHT ? x - w : x;
  meta_draw_at(p, x0, y, px, color, 1); return w;
}
float meta_draw(const char *ml, const char *genres, const char *name, float x, float y, int px, uint32_t color, int align) {
  MetaParts p; meta_parse(ml, genres, name, &p);
  return meta_place(&p, meta_draw_at(&p, 0, 0, px, 0, 0), x, y, px, color, align);
}
void meta_prep(MetaParts *p, const char *ml, const char *genres, const char *name, int px) { meta_parse(ml, genres, name, p); p->px = px; p->lg = i18n_gen(); p->w = meta_draw_at(p, 0, 0, px, 0, 0); }
int meta_stale(const MetaParts *p) { return p->lg != i18n_gen(); }
float meta_draw_p(const MetaParts *p, float x, float y, uint32_t color, int align) { return meta_place(p, p->w, x, y, p->px, color, align); }

/* ---- the waiting look ----
   Every picture or text still on its way shows the same thing: the app's surface (#1e1c27, lit a little from the
   top as the 158 degree sheen does, a faint lit edge like the glass panels) and wide soft gleams, leaning 22 degrees,
   that drift across the whole screen without a pause on one clock, 1400 units apart: neighbouring placeholders glow
   as parts of the same light (each is a window onto it), and there is always light moving somewhere. A gleam is a
   low (8.5%) lavender-white swell with an S-curve falloff and no core or edge, like light on frosted glass. Only the
   shape differs. Everything is drawn from the shared alpha pages, so the placeholders of a view, their light and the
   text around them go out in one batch: the surface is a nine-slice rounded shape; the light is one small baked
   tile (one period) sampled at screen coordinates (per-vertex uv: one affine map per tile), drawn over the shape's
   middle cross; each corner square gets the shape's own corner mask tinted with the light's value at that spot
   (it hardly changes over a corner). Big corners (circles) are cut into tiles so the light stays smooth. */
#define PH_P 1400.0f    /* one period of the light on screen */
#define PH_VH 1320.0f   /* the tile's height: the screen and a margin */
#define PH_Y0 (-120.0f)
#define PH_TW 256
#define PH_TH 32
#define PH_LEAN 0.404f  /* tan 22 degrees: the 158 degree sheen's angle, as the light's lean */
#define PH_SPEED 760.0f /* units a second: a light crosses a poster in about a third of a second, one every 1.8 s */
#define PH_PEAK 0.085f  /* the light's strength at its middle: low, a gleam on frosted glass, never a white band */
#define PH_HALF 440.0f  /* half the light's width */
#define PH_TINT RGB(0xdc, 0xd8, 0xf2) /* a cool lavender white: the light takes the surface's hue */
void gfx_xform_point(float *x, float *y); /* layout -> screen with the current transform (gfx.c) */
static float ph_profile(float d) { float x = 1 - fabsf(d) / PH_HALF; if (x <= 0) return 0; return x * x * x * (x * (x * 6 - 15) + 10); } /* one wide soft swell: an S-curve (smootherstep) from the middle out, no core, no edge */
/* the light's value (0..1) at a point of the tile (X in 0..PH_P, Y in 0..PH_VH); periodic in X */
static float ph_tile(float X, float Y) { float d = fmodf(X + PH_LEAN * (Y - PH_VH / 2) - PH_P / 2, PH_P); if (d < 0) d += PH_P; return ph_profile(d - PH_P / 2); }
static AtlasSlot phStrip; static int phStripOk;
/* small masks of our own in the shared alpha pages (kept until evicted here): the placeholder's corner, the hold
   line's quarter band and its round end. Each repeats its inner row and column once, so a piece sampled at the seam
   with the straight parts reads the same on both sides */
enum { KM_CORNER, KM_BAND, KM_CAPX, KM_CAPY };
#define KM_N 32
static struct { int kind, p, q; AtlasSlot s; } km[KM_N]; static int nkm;
static int ph_page(void) { return phStripOk && atlas_valid(&phStrip) ? phStrip.page : 0; }
static float km_cover(float d) { float t = clampf(d + .5f, 0, 1); return 1 - t * t * (3 - 2 * t); } /* the shapes' 1 px soft edge */
/* KM_CORNER: a filled quarter, radius p px. KM_BAND: a quarter ring, outer radius p/4 px, q/4 px wide. KM_CAPX/Y: a
   half disc of radius 16 px, flat side at x = 0 (CAPX, bulging to +x) or y = 0 (CAPY, bulging to +y) */
static const AtlasSlot *km_get(int kind, int p, int q) {
  for (int i = 0; i < nkm; i++) if (km[i].kind == kind && km[i].p == p && km[i].q == q) { if (atlas_valid(&km[i].s)) { atlas_touch(&km[i].s); return &km[i].s; } km[i].kind = -1; }
  int R = kind == KM_CORNER ? p : kind == KM_BAND ? (int)ceilf(p / 4.0f) + 1 : 16; if (R < 1 || R > 200) return NULL;
  int k = -1; for (int i = 0; i < nkm; i++) if (km[i].kind < 0) { k = i; break; }
  if (k < 0) { if (nkm < KM_N) k = nkm++; else { k = 0; atlas_release(&km[0].s); } }
  int W = kind == KM_CAPY ? 2 * R : R + 1, H = kind == KM_CAPX ? 2 * R : R + 1;
  unsigned char *px = malloc((size_t)W * H); if (!px) return NULL;
  for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { float v;
    if (kind == KM_CAPX || kind == KM_CAPY) { float fx = kind == KM_CAPX ? fmaxf(x - 1 + .5f, 0) : x + .5f - R, fy = kind == KM_CAPY ? fmaxf(y - 1 + .5f, 0) : y + .5f - R; /* row/column 0 repeats the flat side */
      v = km_cover(sqrtf(fx * fx + fy * fy) - R); }
    else { float qx = fmaxf(R - (x + .5f), 0), qy = fmaxf(R - (y + .5f), 0), d = sqrtf(qx * qx + qy * qy);
      if (kind == KM_CORNER) v = km_cover(d - R); else { float ro = p / 4.0f, ri = ro - q / 4.0f; v = km_cover(fmaxf(d - ro, ri - d)); } }
    px[y * W + x] = (unsigned char)(v * 255 + .5f); }
  int ok = atlas_put(px, W, H, ph_page(), 1, &km[k].s); free(px);
  if (ok) { if (kind == KM_CAPX) { km[k].s.x += 1; km[k].s.w = R; } else if (kind == KM_CAPY) { km[k].s.y += 1; km[k].s.h = R; } else km[k].s.w = km[k].s.h = R; }
  km[k].kind = ok ? kind : -1; km[k].p = p; km[k].q = q; return ok ? &km[k].s : NULL;
}
static const AtlasSlot *ph_mask(int R) { return R >= 1 && R <= 160 ? km_get(KM_CORNER, R, 0) : NULL; }
static const AtlasSlot *ph_strip_slot(void) {
  if (phStripOk && atlas_valid(&phStrip)) { atlas_touch(&phStrip); return &phStrip; }
  static unsigned char px[PH_TW * PH_TH];
  for (int y = 0; y < PH_TH; y++) for (int x = 0; x < PH_TW; x++) /* texel centres on the tile's ends: the samples at a seam match the next tile's */
    px[y * PH_TW + x] = (unsigned char)(ph_tile((float)x / (PH_TW - 1) * PH_P, (y + .5f) / PH_TH * PH_VH) * 255 + .5f);
  for (int i = 0; i < nkm; i++) if (km[i].kind >= 0) { atlas_release(&km[i].s); km[i].kind = -1; } /* the masks follow it to its page */
  phStripOk = atlas_put(px, PH_TW, PH_TH, 0, 1, &phStrip);
  return phStripOk ? &phStrip : NULL;
}
/* where the lights are now: the screen x where a tile starts (tiles repeat every PH_P) */
static float ph_phase(void) { return fmodf((float)fmod(app_now() * PH_SPEED, PH_P), PH_P); }
/* the surface breathes very slightly, once per light passing */
static float ph_breath(void) { float p = (float)fmod(app_now() * PH_SPEED / PH_P, 1.0); return 0.5f - 0.5f * cosf(p * 6.2832f); }
/* a screen x in the light's own frame (mirrored layouts: the lights drift the other way) */
static float ph_lx(float X, int rtl) { return rtl ? SB_W - X : X; }
/* the light over one rectangle of a placeholder (layout units), uv taken at its screen position, cut at the tiles' seams */
static void ph_light_rect(GLuint tex, const AtlasSlot *s, float ph, int rtl, float x, float y, float w, float h, uint32_t col) {
  if (w <= 0.01f || h <= 0.01f) return;
  float X0 = x, Y0 = y, X1 = x + w, Y1 = y + h; gfx_xform_point(&X0, &Y0); gfx_xform_point(&X1, &Y1);
  if (X1 - X0 < 0.01f || Y1 - Y0 < 0.01f) return;
  float sx = (X1 - X0) / w, sy = (Y1 - Y0) / h;
  float cy0 = Y0 > PH_Y0 ? Y0 : PH_Y0, cy1 = Y1 < PH_Y0 + PH_VH ? Y1 : PH_Y0 + PH_VH; if (cy1 - cy0 < 0.01f) return;
  float su0, sv0, su1, sv1; atlas_uv(s, &su0, &sv0, &su1, &sv1);
  float hu = 0.5f / ATLAS_W, hv = 0.5f / ATLAS_H; su0 += hu; su1 -= hu; sv0 += hv; sv1 -= hv; /* texel centres: never the empty border */
  float v0 = sv0 + (sv1 - sv0) * (cy0 - PH_Y0) / PH_VH, v1 = sv0 + (sv1 - sv0) * (cy1 - PH_Y0) / PH_VH;
  /* in the light's frame the rectangle spans [a, b]; tile k covers [ph + k P, ph + (k + 1) P] */
  float a = ph_lx(X0, rtl), b = ph_lx(X1, rtl); if (a > b) { float t = a; a = b; b = t; }
  for (float t0 = ph + floorf((a - ph) / PH_P) * PH_P; t0 < b; t0 += PH_P) {
    float p0 = a > t0 ? a : t0, p1 = b < t0 + PH_P ? b : t0 + PH_P; if (p1 - p0 < 0.01f) continue;
    float u0 = su0 + (su1 - su0) * (p0 - t0) / PH_P, u1 = su0 + (su1 - su0) * (p1 - t0) / PH_P;
    float q0 = ph_lx(p0, rtl), q1 = ph_lx(p1, rtl); /* back to the screen; mirrored, the uv runs right to left */
    if (rtl) { float t = q0; q0 = q1; q1 = t; t = u0; u0 = u1; u1 = t; }
    gfx_alpha_tex_uv(tex, x + (q0 - X0) / sx, y + (cy0 - Y0) / sy, (q1 - q0) / sx, (cy1 - cy0) / sy, col, u0, v0, u1, v1); }
}
/* the light's value at a layout point */
static float ph_at(float ph, int rtl, float x, float y) {
  gfx_xform_point(&x, &y); float X = ph_lx(x, rtl) - ph; X = fmodf(X, PH_P); if (X < 0) X += PH_P;
  return ph_tile(X, clampf(y - PH_Y0, 0, PH_VH));
}
#define PH_SURF RGB(0x1e, 0x1c, 0x27)
void ph_surface(float x, float y, float w, float h, float r) { gfx_rrect_v(x, y, w, h, r, col_mix(PH_SURF, 0xffffffffu, 0.062f), col_mix(PH_SURF, 0xffffffffu, 0.014f)); if (w > 8 && h > 8) gfx_ring(x, y, w, h, r, 1, WHITE_A(12)); }
void ph_box(float x, float y, float w, float h, float r) {
  if (w <= 0 || h <= 0) return;
  if (r > w / 2) r = w / 2; if (r > h / 2) r = h / 2; if (r < 0) r = 0;
  float br = ph_breath() * 0.012f, top = 0.062f + br, bot = 0.014f + br;
  gfx_rrect_v(x, y, w, h, r, col_mix(PH_SURF, 0xffffffffu, top), col_mix(PH_SURF, 0xffffffffu, bot));
  if (w > 8 && h > 8) gfx_ring(x, y, w, h, r, 1, WHITE_A(12 + (int)(6 * ph_breath()))); /* the glass's thin lit edge (edges only: little to fill) */
  float ph = ph_phase(); int rtl = app_rtl();
  const AtlasSlot *s = ph_strip_slot(); if (!s) return; GLuint tex = atlas_tex(s->page);
  float scl = gfx_scale(); int R = (int)(r * scl + 0.5f); float rq = R / scl; /* the radius the nine-slice corners are drawn at */
  uint32_t col = col_alpha(PH_TINT, PH_PEAK);
  ph_light_rect(tex, s, ph, rtl, x + rq, y, w - 2 * rq, h, col);                /* the middle cross */
  ph_light_rect(tex, s, ph, rtl, x, y + rq, rq, h - 2 * rq, col);
  ph_light_rect(tex, s, ph, rtl, x + w - rq, y + rq, rq, h - 2 * rq, col);
  if (R < 1) return;
  const AtlasSlot *m = ph_mask(R); if (!m) return; GLuint mt = atlas_tex(m->page);
  float mu0, mv0, mu1, mv1; atlas_uv(m, &mu0, &mv0, &mu1, &mv1);
  int n = (int)ceilf(rq / 40); if (n < 1) n = 1; if (n > 4) n = 4; float tile = rq / n;  /* corner tiles of at most 40 units */
  for (int c = 0; c < 4; c++) { int fx = c & 1, fy = c >> 1; float ox = fx ? x + w - rq : x, oy = fy ? y + h - rq : y;
    for (int j = 0; j < n; j++) for (int i = 0; i < n; i++) {
      float tx = ox + i * tile, ty = oy + j * tile, v = ph_at(ph, rtl, tx + tile / 2, ty + tile / 2); if (v < 0.004f) continue;
      /* the mask is the top-left corner: mirrored for the others; tile i,j of the square maps to its own part of it */
      float a0 = (float)i / n, a1 = (float)(i + 1) / n, b0 = (float)j / n, b1 = (float)(j + 1) / n;
      float u0 = fx ? mu0 + (mu1 - mu0) * (1 - a0) : mu0 + (mu1 - mu0) * a0, u1 = fx ? mu0 + (mu1 - mu0) * (1 - a1) : mu0 + (mu1 - mu0) * a1;
      float v0 = fy ? mv0 + (mv1 - mv0) * (1 - b0) : mv0 + (mv1 - mv0) * b0, v1 = fy ? mv0 + (mv1 - mv0) * (1 - b1) : mv0 + (mv1 - mv0) * b1;
      gfx_alpha_tex_uv(mt, tx, ty, tile, tile, col_alpha(PH_TINT, PH_PEAK * v), u0, v0, u1, v1); } }
}
void ph_circle(float cx, float cy, float rad) { ph_box(cx - rad, cy - rad, 2 * rad, 2 * rad, rad); }
/* text on its way: n rounded bars at the lines' places, the last one shorter */
void ph_text(float x, float y, float w, int n, int px, float lineH, int align) {
  if (align == ALIGN_START) align = app_rtl() ? ALIGN_RIGHT : ALIGN_LEFT; if (align == ALIGN_END) align = app_rtl() ? ALIGN_LEFT : ALIGN_RIGHT;
  float bh = px * 0.62f, lh = lineH > 0 ? lineH : text_line_height(px);
  for (int i = 0; i < n; i++) { float bw = (n > 1 && i == n - 1) ? w * 0.58f : w, bx = align == ALIGN_CENTER ? x - bw / 2 : align == ALIGN_RIGHT ? x - bw : x;
    ph_box(bx, y + i * lh + (lh - bh) / 2, bw, bh, bh / 2); }
}
/* a picture in its box, or the waiting look under it until it has faded in (the picture's corners are its own) */
int ph_picture(GLuint tex, float x, float y, float w, float h, float r, uint32_t tint) {
  if (ph_hold()) tex = 0;
  if (!tex || gfx_tex_fade(tex) < 1) ph_box(x, y, w, h, r);
  if (tex) gfx_image(tex, x, y, w, h, 0, tint, 0, 0, 1, 1);
  return tex != 0;
}
/* development: SB_PH_HOLD=1 keeps every placeholder up (pictures never shown) to look at the waiting look */
int ph_hold(void) { static int h = -1; if (h < 0) h = getenv("SB_PH_HOLD") != NULL; return h; }

/* ---- cards ---- */
static char cardKey[80]; /* the card drawn now: "poster:<id>" (a poster flies to the title page's cover) */
static void card_art(const char *src, float x, float y, float w, float h, float r, int wide, int loadArt, int *shown) {
  *shown = 0;
  int fit = wide ? 3 : 0; GLuint tex = 0;                 /* posters fill the card as they are; wide art is cover-cropped */
  if (src && *src && (loadArt || img_shaped_loaded(src, (int)w, (int)h, r, fit))) tex = img_get_shaped(src, (int)w, (int)h, r, fit);
  if (!src || !*src) { ph_surface(x, y, w, h, r); return; }
  if (!tex && img_shaped_failed(src, (int)w, (int)h, r, fit)) { ph_surface(x, y, w, h, r); *shown = -1; return; } /* nothing is coming: the surface stays still */
  if (tex && !wide && cardKey[0] && !ph_hold() && gfx_tex_fade(tex) >= 1) { sx_image(cardKey, tex, x, y, w, h, 0, WHITE_A(255), 0, 0, 1, 1); *shown = 1; return; } /* named: it can fly */
  if (ph_picture(tex, x, y, w, h, r, WHITE_A(255))) *shown = 1; /* corners are in the picture: the plain program */
}
void card_prep(CardPrep *cp, const Item *it, int wide, const char *artOverride) {
  cp->lg = i18n_gen(); cp->lbl[0] = cp->left[0] = 0; cp->leftW = 0;
  if (artOverride) { Item *tmp = malloc(sizeof *tmp); if (tmp) { *tmp = *it; snprintf(tmp->poster, sizeof tmp->poster, "%s", artOverride); sb_poster_of(tmp, cp->art, sizeof cp->art); free(tmp); } else cp->art[0] = 0;
    snprintf(cp->pre, sizeof cp->pre, "%s", artOverride); } /* the card shows the address as a poster; the fetch ahead asks for it as given */
  else { if (wide) sb_bg_small(it, cp->art, sizeof cp->art); else sb_poster_of(it, cp->art, sizeof cp->art); snprintf(cp->pre, sizeof cp->pre, "%s", cp->art); }
  if (!wide) return;
  const char *c = strrchr(it->videoId, ':'); int ep = 0, se = 0;
  if (c) { ep = atoi(c + 1); const char *p = c - 1; while (p > it->videoId && *p != ':') p--; if (*p == ':') se = atoi(p + 1); }
  if (c && se >= 0) snprintf(cp->lbl, sizeof cp->lbl, "%s  S%d E%d", it->name, se, ep); else snprintf(cp->lbl, sizeof cp->lbl, "%s", it->name);
  if (it->left > 0) { if (it->left < 1) snprintf(cp->left, sizeof cp->left, "%s", T("Less than a minute left")); else { char r[32]; sb_fmt_runtime((int)(it->left + 0.5f) < 1 ? 1 : (int)(it->left + 0.5f), r, sizeof r); snprintf(cp->left, sizeof cp->left, "%s", T1("{t} left", "t", r)); }
    cp->leftW = text_width(cp->left, 22, 700, 0.2f) + 26; }
}
static int card_body(const Item *it, const CardPrep *cp, float x, float y, float w, float h, int wide, int label, int loadArt) {
  int shown; CardPrep own;
  if (!cp) { card_prep(&own, it, wide, NULL); cp = &own; } /* a card without its own: resolved here, as before */
  card_art(cp->art, x, y, w, h, CARD_R, wide, loadArt, &shown);
  gfx_clip_push(x, y, w, h);
  if (shown < 0) text_draw_wrapped(it->name[0] ? it->name : it->id, app_rtl() ? x + w - 16 : x + 16, y + 16, w - 32, 6, 28, 400, WHITE_A(230), 0, ALIGN_START, 28 * 1.3f); /* the title shows only when the art failed */
  if (label && !wide) { float lh = text_line_height(25), gh = 34 + lh + 13; gfx_rrect_v(x, y + h - gh, w, gh, 0, BLACK_A(0), BLACK_A(235)); text_draw(it->name, app_rtl() ? x + w - 15 : x + 15, y + h - 13 - lh, 25, 600, WHITE_A(255), 0, w - 30, ALIGN_START); }
  /* on a poster: watched, a disc with a check, always in the top corner; saved to the Library (not on the Library's own
     page) a disc of the same make with a bookmark, beside it: the accent lit from above, a fine light edge, a soft
     shadow, the marks in the accent's own contrast colour */
  int watched = !wide && lib_item_watched(it), inLib = !wide && SCR != SCR_LIB && lib_has(it->id), rtl = app_rtl();
  float bx = rtl ? x + 12 : x + w - 12 - 42, by = y + 12;
  if (inLib) { float sx = watched ? (rtl ? bx + 42 + 8 : bx - 8 - 42) : bx; /* saved: a disc of the same make, a bookmark in it, beside the check (or in the corner alone) */
    gfx_shadow(sx, by + 3, 42, 42, 21, 12, BLACK_A(120)); gfx_circle(sx + 21, by + 21, 21, ACC | 0xff000000u);
    gfx_rrect_v(sx, by, 42, 42, 21, WHITE_A(70), WHITE_A(0)); gfx_ring(sx, by, 42, 42, 21, 1.5f, WHITE_A(70));
    icon_draw("saved", sx + 10, by + 10, 22, ACC_FG); }
  if (watched) { float pk = 1; double pt = app_now() - markAt; /* just marked: it pops in */
    if (markId[0] && !strcmp(markId, it->id) && pt < 0.45) { float u = (float)(pt / 0.45); pk = 0.55f + 0.45f * ease_apply(EASE_ROW, u); float ra = 1 - u; gfx_ring(bx - 14 * u, by - 14 * u, 42 + 28 * u, 42 + 28 * u, 21 + 14 * u, 2, col_alpha(ACC2, 0.5f * ra)); }
    gfx_scale_push(pk, pk, bx + 21, by + 21);
    gfx_shadow(bx, by + 3, 42, 42, 21, 12, BLACK_A(120)); gfx_circle(bx + 21, by + 21, 21, ACC | 0xff000000u);
    gfx_rrect_v(bx, by, 42, 42, 21, WHITE_A(70), WHITE_A(0)); gfx_ring(bx, by, 42, 42, 21, 1.5f, WHITE_A(70));
    icon_draw("check", bx + 9, by + 9, 24, ACC_FG); gfx_scale_pop(); }
  if (wide) {
    /* the name over a fade (12 15 26 padding: the inset progress line sits under it), the time left in a chip */
    float lh = text_line_height(26), gh = 12 + lh + 26;
    gfx_rrect_v(x, y + h - gh, w, gh, 0, BLACK_A(0), BLACK_A(224));
    text_draw(cp->lbl, app_rtl() ? x + w - 15 : x + 15, y + h - 26 - lh, 26, 600, WHITE_A(255), 0, w - 30, ALIGN_START);
    if (it->left > 0) { float tw = cp->leftW, th = 10 + text_line_height(22), bx = app_rtl() ? x + 12 : x + w - 12 - tw;
      gfx_rrect(bx, y + 12, tw, th, 11, RGBA(12, 11, 17, 199)); text_draw(cp->left, bx + 13, y + 12 + 5, 22, 700,
 WHITE_A(255), 0.2f, 0, ALIGN_LEFT); }
    if (it->progress > 0) { float bw = w - 28, pw = bw * (it->progress > 1 ? 1 : it->progress); /* inset and rounded, never edge to edge; the accent a touch lighter */
      gfx_rrect(x + 14, y + h - 13 - 6, bw, 6, 3, WHITE_A(61)); if (pw >= 1) gfx_rrect(app_rtl() ? x + 14 + bw - pw : x + 14, y + h - 13 - 6, pw, 6, 3, col_mix(ACC, 0xffffffffu, 0.2f)); }
  }
  gfx_clip_pop();
  return shown;
}
int card_draw_wh(const Item *it, float x, float y, float w, float h, int wide, int label, int loadArt) { return card_body(it, NULL, x, y, w, h, wide, label, loadArt); }
int card_draw_prep(const Item *it, const CardPrep *cp, float x, float y, float w, float h, int wide, int label, int loadArt) { return card_body(it, cp, x, y, w, h, wide, label, loadArt); }
void card_draw(const Item *it, float x, float y, int wide, int label, int loadArt) { card_body(it, NULL, x, y, wide ? WIDE_W : CARD_W, wide ? WIDE_H : CARD_H, wide, label, loadArt); }
void card_mark_pop(const char *id) { snprintf(markId, sizeof markId, "%s", id ? id : ""); markAt = app_now(); }
void focus_pop(FocusPop *f) { timer_start(&f->pop, 0.2f); f->which ^= 1; }
void focus_tick(FocusPop *f, float dt) { timer_tick(&f->pop, dt); }
/* ---- the focus lift ---- */
#define LIFT_N 32
#define LIFT 0.09f       /* how much it grows (clearly: the focused card must stand out at a glance) */
#define LIFT_DUR 0.24f   /* in and out */
#define GLARE_DUR 0.55f  /* the glare's one pass */
typedef struct { unsigned long long key; Tween k; double t0; unsigned seen; int on; } LiftE;
static LiftE LT[LIFT_N]; static unsigned liftFrame; static unsigned long long liftCur; static float liftDim = 1;
/* the lift's own shadow and glass, weaker (a row melting over a background trailer: the card fades as the row does,
   with no dark block of its own over the picture) */
void lift_fade(float a) { liftDim = a < 0 ? 0 : a > 1 ? 1 : a; }
unsigned long long lift_key(const void *owner, int index) { return (unsigned long long)(uintptr_t)owner * 1000003ull + (unsigned)index + 1; }
static LiftE *lift_find(unsigned long long key, int make) {
  LiftE *free_ = NULL;
  for (int i = 0; i < LIFT_N; i++) { if (LT[i].key == key) return &LT[i]; if (!LT[i].key && !free_) free_ = &LT[i]; }
  if (!make) return NULL;
  if (!free_) { free_ = &LT[0]; for (int i = 1; i < LIFT_N; i++) if (LT[i].k.v < free_->k.v) free_ = &LT[i]; } /* full: the one most settled */
  memset(free_, 0, sizeof *free_); free_->key = key; return free_;
}
void lift_tick(float dt) {
  liftFrame++;
  for (int i = 0; i < LIFT_N; i++) { LiftE *e = &LT[i]; if (!e->key) continue;
    if (e->seen + 2 < liftFrame || (!e->on && !e->k.on && e->k.v <= 0.0001f)) { e->key = 0; continue; } /* gone from the screen, or settled back */
    tween_tick(&e->k, dt); }
}
float lift_begin(unsigned long long key, int focused, float hold, float x, float y, float w, float h, float r) { return lift_begin_z(key, focused, hold, x, y, w, h, r, LIFT); }
float lift_begin_z(unsigned long long key, int focused, float hold, float x, float y, float w, float h, float r, float zoom) {
  LiftE *e = lift_find(key, focused); float k = 0;
  if (e) { e->seen = liftFrame;
    if (focused && !e->on) { e->on = 1; tween_to(&e->k, 1, LIFT_DUR, GLIDE_EASE); e->t0 = app_now(); }
    else if (!focused && e->on) { e->on = 0; tween_to(&e->k, 0, LIFT_DUR, GLIDE_EASE); }
    if (focused) liftCur = key;
    k = e->k.v; }
  float hk = hold > 0 ? ease_apply(EASE_IN_OUT, hold) : 0, s = (1 + zoom * k) * (1 - 0.04f * hk); /* held: it sinks a little while the line runs */
  gfx_scale_push(s, s, x + w / 2, y + h / 2);
  if (k * liftDim > 0.002f) gfx_shadow(x, y + 14, w, h, r, 34, BLACK_A((int)(160 * k * liftDim)));
  return k;
}
void lift_end(unsigned long long key, float x, float y, float w, float h, float r) {
  LiftE *e = lift_find(key, 0);
  if (e && e->k.v * liftDim > 0.002f) { float k = e->k.v * liftDim;
    gfx_rrect_v(x, y, w, h, r, WHITE_A((int)(24 * k)), WHITE_A(0)); /* the glass: light from above, faint, in the shape itself */
    float p = (float)((app_now() - e->t0) / GLARE_DUR);
    /* the glare's one pass, then it rests, faint, near the far edge for as long as the focus stays: the focused card is
       plain to see */
    if (e->on) { float q = ease_apply(EASE_IN_OUT, p < 0 ? 0 : p > 1 ? 1 : p), pos = -0.2f + 1.0f * q, al = 78 * q + 110 * sinf(3.14159f * q); /* bright as it crosses, settling to its faint rest */
      gfx_glare(x, y, w, h, r, pos, WHITE_A((int)(al * k))); } }
  gfx_scale_pop();
}
/* a remote key drawn as on the remote, then what it does: OK, Back and Down round, the colour keys a small rounded block
   with two dots; y is the line's middle. Returns the width taken. */
float key_hint(float x, float y, int key, const char *label, uint32_t col, int alignRight) {
  float kw = key == K_RED || key == K_GREEN ? 42 : 38, kh = key == K_RED || key == K_GREEN ? 24 : 38, lw = label && *label ? 10 + text_width(label, 22, 600, 0) : 0, tot = kw + lw;
  float kx = alignRight ? x - tot : x, ky = y - kh / 2, a = ((col >> 24) & 255) / 255.0f;
  if (key == K_RED || key == K_GREEN) { uint32_t c = key == K_RED ? RGB(0xef, 0x3b, 0x3b) : RGB(0x2f, 0xd6, 0x6b);
    gfx_rrect(kx, ky, kw, kh, 7, col_alpha(c | 0xff000000u, a)); gfx_rrect_v(kx, ky, kw, kh, 7, WHITE_A((int)(50 * a)), WHITE_A(0));
    gfx_circle(kx + kw / 2 - 7, y, 2.4f, RGBA(60, 0, 0, (int)(150 * a))); gfx_circle(kx + kw / 2 + 7, y, 2.4f, RGBA(60, 0, 0, (int)(150 * a))); }
  else { gfx_circle(kx + kw / 2, y, kw / 2, WHITE_A((int)(30 * a))); gfx_ring(kx, ky, kw, kh, kw / 2, 1.5f, WHITE_A((int)(90 * a)));
    /* the key's mark: drawn as a shape (not a font's glyph), so it sits exactly in the middle at any size */
    const char *ic = key == K_OK ? "k_ok" : key == K_DOWN ? "k_down" : key == K_UP ? "k_up" : key == K_RIGHT ? "k_right" : key == K_LEFT ? "k_left" : "k_back"; float is = key == K_OK ? 22 : 20;
    icon_draw(ic, kx + kw / 2 - is / 2, y - is / 2, is, col); }
  if (lw > 0) text_draw(label, kx + kw + 10, y - text_line_height(22) / 2.0f, 22, 600, col, 0, 0, ALIGN_LEFT);
  return tot;
}
int focus_pops(double *last) { double now = app_now(), was = *last; *last = now; return !app_key_held() && now - was > 0.3; }
/* the hold line (as the page draws it: a 6 px stroke on a path 7 px outside the box with corners of radius r + 7,
   a faint 6 px track under it, a 12 px halo at .32, round ends, from the top centre clockwise). The stroke is built
   from pieces of our own masks in the shared pages: straight parts stretch the quarter band's straight end, the
   corners show the quarter band itself, revealed up to the angle reached (the first half of a corner cut across the
   incoming side, the second half across the outgoing side), and the ends are half discs. Every edge is the shapes'
   soft 1 px edge: no clipping, no square corners, one batch. */
typedef struct { GLuint tex; float u0, v0, u1, v1, Q; } Band;
static int band_get(Band *b, int kind, float ro, float wid, float scl) {
  const AtlasSlot *s = km_get(kind, (int)(ro * scl * 4 + .5f), (int)(wid * scl * 4 + .5f)); if (!s) return 0;
  b->tex = atlas_tex(s->page); atlas_uv(s, &b->u0, &b->v0, &b->u1, &b->v1); b->Q = s->w / scl; return 1;
}
/* a piece of a corner's quarter (s along the incoming direction, t against the outgoing one, both 0..Q from the arc's
   centre (cx, cy)); the band is symmetric about its diagonal, so whichever of s, t runs along x takes u */
static void band_corner(const Band *b, float cx, float cy, int dix, int diy, int dox, int doy, float s0, float s1, float t0, float t1, uint32_t col) {
  if (s1 - s0 < 0.01f || t1 - t0 < 0.01f) return;
  float xa = cx + s0 * dix - t0 * dox, ya = cy + s0 * diy - t0 * doy, xb = cx + s1 * dix - t1 * dox, yb = cy + s1 * diy - t1 * doy;
  /* mask coordinate (0 = outer, 1 = centre) of each layout edge */
  float ka = dix ? 1 - s0 / b->Q : 1 - t0 / b->Q, kb = dix ? 1 - s1 / b->Q : 1 - t1 / b->Q;   /* along x */
  float la = dix ? 1 - t0 / b->Q : 1 - s0 / b->Q, lb = dix ? 1 - t1 / b->Q : 1 - s1 / b->Q;   /* along y */
  float x0 = xa < xb ? xa : xb, x1 = xa < xb ? xb : xa, y0 = ya < yb ? ya : yb, y1 = ya < yb ? yb : ya;
  float kx0 = xa < xb ? ka : kb, kx1 = xa < xb ? kb : ka, ly0 = ya < yb ? la : lb, ly1 = ya < yb ? lb : la;
  gfx_alpha_tex_uv(b->tex, x0, y0, x1 - x0, y1 - y0, col, b->u0 + (b->u1 - b->u0) * kx0, b->v0 + (b->v1 - b->v0) * ly0, b->u0 + (b->u1 - b->u0) * kx1, b->v0 + (b->v1 - b->v0) * ly1);
}
/* a straight part between two points on one side (side 0 top, 1 right, 2 bottom, 3 left; c = the side's arc-centre line) */
static void band_side(const Band *b, int side, float c, float p0, float p1, uint32_t col) {
  if (p1 < p0) { float t = p0; p0 = p1; p1 = t; } if (p1 - p0 < 0.01f) return;
  float Q = b->Q;
  if (side == 0) gfx_alpha_tex_uv(b->tex, p0, c - Q, p1 - p0, Q, col, b->u1, b->v0, b->u1, b->v1);
  else if (side == 2) gfx_alpha_tex_uv(b->tex, p0, c, p1 - p0, Q, col, b->u1, b->v1, b->u1, b->v0);
  else if (side == 1) gfx_alpha_tex_uv(b->tex, c, p0, Q, p1 - p0, col, b->u1, b->v1, b->u0, b->v1);
  else gfx_alpha_tex_uv(b->tex, c - Q, p0, Q, p1 - p0, col, b->u0, b->v1, b->u1, b->v1);
}
/* a round end: a half disc on a flat side from (ax, ay) to (bx, by) (axis-aligned), bulging dep units toward dir (0 +x, 1 -x, 2 +y, 3 -y) */
static void band_cap(float ax, float ay, float bx, float by, float dep, int dir, uint32_t col) {
  const AtlasSlot *s = km_get(dir < 2 ? KM_CAPX : KM_CAPY, 0, 0); if (!s) return; GLuint t = atlas_tex(s->page);
  float u0, v0, u1, v1; atlas_uv(s, &u0, &v0, &u1, &v1);
  float lo, hi;
  if (dir < 2) { lo = ay < by ? ay : by; hi = ay < by ? by : ay; if (hi - lo < 0.01f) return;
    if (dir == 0) gfx_alpha_tex_uv(t, ax, lo, dep, hi - lo, col, u0, v0, u1, v1); else gfx_alpha_tex_uv(t, ax - dep, lo, dep, hi - lo, col, u1, v0, u0, v1); }
  else { lo = ax < bx ? ax : bx; hi = ax < bx ? bx : ax; if (hi - lo < 0.01f) return;
    if (dir == 2) gfx_alpha_tex_uv(t, lo, ay, hi - lo, dep, col, u0, v0, u1, v1); else gfx_alpha_tex_uv(t, lo, ay - dep, hi - lo, dep, col, u0, v1, u1, v0); }
}
/* the stroke of width wid on the path rc = r + C around the box, revealed over the fraction p of its length */
static void hold_stroke(float x, float y, float w, float h, float r, float C, float wid, float p, uint32_t col) {
  float scl = gfx_scale(), rc = r + C, ro = rc + wid / 2, ri = rc - wid / 2, hw = wid / 2; Band b; if (!band_get(&b, KM_BAND, ro, wid, scl)) return;
  float sw = w - 2 * r > 0 ? w - 2 * r : 0, sh = h - 2 * r > 0 ? h - 2 * r : 0, arc = 1.5707963f * rc;
  float L = 2 * sw + 2 * sh + 4 * arc, done = p * L;
  /* the corners: centre, incoming and outgoing directions (clockwise from the top right) */
  const float ccx[4] = { x + w - r, x + w - r, x + r, x + r }, ccy[4] = { y + r, y + h - r, y + h - r, y + r };
  const int di[4][2] = { { 1, 0 }, { 0, 1 }, { -1, 0 }, { 0, -1 } }, dq[4][2] = { { 0, 1 }, { -1, 0 }, { 0, -1 }, { 1, 0 } };
  /* the end: its flat side (ax, ay)-(bx, by) and the way it faces */
  float ex0 = x + w / 2, ey0 = y - C - hw, ex1 = ex0, ey1 = y - C + hw; int edir = 0;
  band_cap(x + w / 2, y - C - hw, x + w / 2, y - C + hw, hw, 1, col); /* the start, facing back */
  /* the right half of the top */
  { float len = sw / 2 < done ? sw / 2 : done; band_side(&b, 0, y + r, x + w / 2, x + w / 2 + len, col); done -= len; ex0 = ex1 = x + w / 2 + len; }
  #define DIR(dx, dy) ((dx) > 0 ? 0 : (dx) < 0 ? 1 : (dy) > 0 ? 2 : 3)
  for (int k = 0; k < 4 && done > 0; k++) {
    float cx = ccx[k], cy = ccy[k]; int dix = di[k][0], diy = di[k][1], dox = dq[k][0], doy = dq[k][1];
    float a = done < arc ? done / arc * 1.5707963f : 1.5707963f, S45 = rc * 0.70710678f; done -= arc < done ? arc : done;
    if (a <= 0.78539816f) { float s0 = rc * sinf(a); band_corner(&b, cx, cy, dix, diy, dox, doy, 0, s0, 0, b.Q, col);
      /* cut across the incoming direction at s0: the band meets it from t0 to t1 */
      float t0 = sqrtf(fmaxf(ri * ri - s0 * s0, 0)), t1 = sqrtf(fmaxf(ro * ro - s0 * s0, 0));
      ex0 = cx + s0 * dix - t0 * dox; ey0 = cy + s0 * diy - t0 * doy; ex1 = cx + s0 * dix - t1 * dox; ey1 = cy + s0 * diy - t1 * doy; edir = DIR(dix, diy); }
    else { float t0 = rc * cosf(a); band_corner(&b, cx, cy, dix, diy, dox, doy, 0, S45, 0, b.Q, col); band_corner(&b, cx, cy, dix, diy, dox, doy, S45, b.Q, t0, b.Q, col);
      /* cut across the outgoing direction at t0: the band meets it from s0 to s1 */
      float s0 = sqrtf(fmaxf(ri * ri - t0 * t0, 0)), s1 = sqrtf(fmaxf(ro * ro - t0 * t0, 0));
      ex0 = cx + s0 * dix - t0 * dox; ey0 = cy + s0 * diy - t0 * doy; ex1 = cx + s1 * dix - t0 * dox; ey1 = cy + s1 * diy - t0 * doy; edir = DIR(dox, doy); }
    if (done <= 0) break;
    /* the side after this corner (the last one: the left half of the top) */
    float side = k == 3 ? sw / 2 : k == 0 || k == 2 ? sh : sw, len = side < done ? side : done; done -= len;
    if (k == 0) { band_side(&b, 1, x + w - r, y + r, y + r + len, col); ex0 = x + w + C - hw; ex1 = x + w + C + hw; ey0 = ey1 = y + r + len; edir = 2; }
    else if (k == 1) { band_side(&b, 2, y + h - r, x + w - r - len, x + w - r, col); ex0 = ex1 = x + w - r - len; ey0 = y + h + C - hw; ey1 = y + h + C + hw; edir = 1; }
    else if (k == 2) { band_side(&b, 3, x + r, y + h - r - len, y + h - r, col); ex0 = x - C - hw; ex1 = x - C + hw; ey0 = ey1 = y + h - r - len; edir = 3; }
    else { band_side(&b, 0, y + r, x + r, x + r + len, col); ex0 = ex1 = x + r + len; ey0 = y - C - hw; ey1 = y - C + hw; edir = 0; }
  }
  #undef DIR
  if (p < 0.999f) band_cap(ex0, ey0, ex1, ey1, hw, edir, col); /* the moving end, round (in a corner the cut runs along an axis: the half disc spans the band there) */
}
static float holdRect[5]; /* the box the line last ran around, on screen (layout units): what opens from the hold grows out of it */
int hold_trace_rect(float *x, float *y, float *w, float *h, float *r) { if (holdRect[2] <= 0) return 0; *x = holdRect[0]; *y = holdRect[1]; *w = holdRect[2]; *h = holdRect[3]; *r = holdRect[4]; return 1; }
void hold_trace_draw(float x, float y, float w, float h, float r, float p) {
  { float ax = x, ay = y, bx = x + w, by = y + h; gfx_xform_point(&ax, &ay); gfx_xform_point(&bx, &by); holdRect[0] = ax; holdRect[1] = ay; holdRect[2] = bx - ax; holdRect[3] = by - ay; holdRect[4] = r * (bx - ax) / w; } /* (layout units already) */
  float a = p < 0.12f ? p / 0.12f : 1; /* fades in */
  gfx_opacity_push(a);
  gfx_ring_out(x - 4, y - 4, w + 8, h + 8, r + 4, 6, WHITE_A(61)); /* the track */
  hold_stroke(x, y, w, h, r, 7, 12, p, col_alpha(ACC2, 0.32f));      /* the halo under the line */
  hold_stroke(x, y, w, h, r, 7, 6, p, ACC2);
  gfx_opacity_pop();
}

/* ---- the ring ---- */
void ring_to(Ring *r, float x, float y, int instant) {
  if (!r->shown || instant) { tween_set(&r->x, x); tween_set(&r->y, y); r->shown = 1; timer_start(&r->pop, 0.32f); r->moved = app_now(); return; } /* appears in place */
  if (r->x.to == x && r->y.to == y) return;
  tween_to(&r->x, x, 0.3f, EASE_OUT); tween_to(&r->y, y, 0.3f, EASE_OUT); if (focus_pops(&r->moved)) timer_start(&r->pop, 0.32f); /* lands with a small pop; held, it only glides */
}
void ring_hide(Ring *r) { r->shown = 0; }
void ring_tick(Ring *r, float dt) { tween_tick(&r->x, dt); tween_tick(&r->y, dt); timer_tick(&r->pop, dt); }

/* ---- the pill ---- */
void pill_to(Pill *p, float x, float y, float w, float h) {
  float d = p->dur > 0 ? p->dur : GLIDE_DUR;
  if (!p->shown) { tween_set(&p->x, x); tween_set(&p->y, y); tween_set(&p->w, w); tween_set(&p->h, h); p->shown = 1; tween_to(&p->a, 1, 0.2f, EASE_OUT); return; }
  tween_to(&p->x, x, d, GLIDE_EASE); tween_to(&p->y, y, d, GLIDE_EASE); tween_to(&p->w, w, d, GLIDE_EASE); tween_to(&p->h, h, d, GLIDE_EASE); tween_to(&p->a, 1, 0.2f, EASE_OUT);
}
void pill_hide(Pill *p) { if (p->shown) { p->shown = 0; tween_to(&p->a, 0, 0.2f, EASE_OUT); } }
void pill_tick(Pill *p, float dt) { tween_tick(&p->x, dt); tween_tick(&p->y, dt); tween_tick(&p->w, dt); tween_tick(&p->h, dt); tween_tick(&p->a, dt); }
