#include "platform/plat.h"
#include "ui/text.h"
#include "ui/wrapbreak.h"
#include <SDL2/SDL.h>
#include "ui/gfx.h"
#include "ui/atlas.h"
#include "core/i18n.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H
#include FT_GLYPH_H
#include FT_OUTLINE_H
#include <hb.h>
#include <hb-ft.h>
#include <hb-ot.h>
#include <fribidi.h>
#include "stb_image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <pthread.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

/* ---- fonts ---------------------------------------------------------------------------------
   One FT_Face per (family, weight): the variable fonts are set to that weight once, and glyph
   rendering never changes the coordinates again (changing them per glyph is what makes variable
   fonts slow). Sizes are set per line with FT_Set_Pixel_Sizes. */
enum { FAM_LATIN = 0, FAM_ARABIC = 1, FAM_FB = 2 };   /* fam >= FAM_FB: the fallback file fam - FAM_FB */
enum { FID_NONE = -1, FID_TAG = -2 };                  /* a run's font: no font has it (dropped); a flag drawn as a tag */
/* bitmap: 1 = colour PNG glyphs only (drawn as a mask), -1 = FreeType could not open it; ftpx: the FT size of a fallback face */
typedef struct { int fam, weight; FT_Face face; hb_font_t *hb; int px, ftpx, bitmap; } Font;
#define MAX_FONTS 16
#define MAX_FB 64
#define CPC_N 1024
/* one font set per thread: the drawing thread's, and the text thread's (FreeType faces are not shared across threads).
   fb: this thread's fallback fonts (fbState 0 untried, 1 ready, -1 unusable); cpc: which font a lone codepoint takes */
typedef struct { FT_Library lib; Font f[MAX_FONTS]; int n; Font fb[MAX_FB]; signed char fbState[MAX_FB]; Font *inter; uint32_t cpcKey[CPC_N]; short cpcFont[CPC_N]; } FontSet;
static FontSet mainSet; static _Thread_local FontSet *FS; static _Thread_local int shapeRtl;
#define fonts (FS->f)
#define nfonts (FS->n)
#define ft (FS->lib)
static unsigned char *fontData[2]; static long fontLen[2];
static float sscale = 1.0f;  /* surface pixels per layout unit: glyphs are rasterised at that resolution */
static int uiRtl;
static int budgetLines;      /* lines rasterised this frame */
#define LINES_PER_FRAME 6
#define TEXT_MS 10.0
static double statLineMs, statWidthMs; static int statRaster, statShape, statLookups;
void text_stats(double *lineMs, double *widthMs, int *raster, int *shape, int *lookups) { *lineMs = statLineMs; *widthMs = statWidthMs; *raster = statRaster; *shape = statShape; *lookups = statLookups; statLineMs = statWidthMs = 0; statRaster = statShape = statLookups = 0; }

static Font *font_get(int fam, int weight) {
  int i; for (i = 0; i < nfonts; i++) if (fonts[i].fam == fam && fonts[i].weight == weight) return &fonts[i];
  if (nfonts >= MAX_FONTS || !fontData[fam]) return NULL;
  Font *f = &fonts[nfonts];
  if (FT_New_Memory_Face(ft, fontData[fam], fontLen[fam], 0, &f->face)) return NULL;
  /* the weight axis of the variable font */
  FT_MM_Var *mm = NULL;
  if (!FT_Get_MM_Var(f->face, &mm) && mm) {
    FT_Fixed coords[8]; unsigned a;
    for (a = 0; a < mm->num_axis && a < 8; a++) {
      coords[a] = mm->axis[a].def;
      if (mm->axis[a].tag == FT_MAKE_TAG('w', 'g', 'h', 't')) { FT_Fixed w = (FT_Fixed)weight << 16; if (w < mm->axis[a].minimum) w = mm->axis[a].minimum; if (w > mm->axis[a].maximum) w = mm->axis[a].maximum; coords[a] = w; }
    }
    FT_Set_Var_Design_Coordinates(f->face, mm->num_axis < 8 ? mm->num_axis : 8, coords);
    FT_Done_MM_Var(ft, mm);
  }
  f->fam = fam; f->weight = weight; f->px = 0; f->ftpx = 0; f->bitmap = 0;
  f->hb = hb_ft_font_create_referenced(f->face);
  hb_ft_font_set_load_flags(f->hb, FT_LOAD_DEFAULT | FT_LOAD_NO_HINTING);
  nfonts++;
  return f;
}
static void font_size(Font *f, int px) { /* px in surface pixels */
  if (f->px == px) return;
  if (f->fam >= FAM_FB) { hb_font_set_scale(f->hb, px * 64, px * 64); hb_font_set_ppem(f->hb, (unsigned)px, (unsigned)px); } /* HarfBuzz's own metrics: positions in 26.6 pixels, as hb-ft gives */
  else { FT_Set_Pixel_Sizes(f->face, 0, px); hb_ft_font_changed(f->hb); }
  f->px = px;
}

/* ---- fallback fonts ------------------------------------------------------------------------
   What Inter and Noto Sans Arabic lack (emoji, symbols, CJK, Korean, Hebrew, Thai, Indic scripts...)
   comes from the system's fonts, found by name at start (a directory listing: nothing is opened) and
   mapped only when a character first needs one. The TV's wide fonts are 5-13 MB; mapped pages cost
   nothing until touched and are never copied into the heap. The mapping and the HarfBuzz face are
   shared by both threads (HarfBuzz faces are safe to read from several); each thread makes its own
   hb_font for shaping and coverage, and its own FT_Face only when it rasterises a glyph.
   Colour-only emoji fonts (CBDT, sbix) have no outlines: their PNGs are decoded and drawn as an alpha
   mask, so they stay in the one text batch, tinted like the text (outline emoji fonts come first). */
enum { C_EMOJI = 1, C_SYM = 2, C_HAN = 4, C_KANA = 8, C_HANGUL = 16, C_GEN = 32, C_COLOR = 64, C_TAIL = 128 };
typedef struct { const char *name; int cls; hb_script_t sc[3]; } FbSpec;
typedef struct { char path[320]; int cls; hb_script_t sc[3]; int state, bitmap, index; const unsigned char *data; size_t len; hb_face_t *face; } FbFile;
static FbFile fbf[MAX_FB]; static int nfb; static pthread_mutex_t fbmu = PTHREAD_MUTEX_INITIALIZER;
static hb_unicode_funcs_t *uf;
/* the TV's /usr/share/fonts, matched by name prefix; files not named here are tried last */
static const FbSpec TVFONTS[] = {
  { "NotoEmoji-Regular", C_EMOJI }, { "DroidEmoji", C_EMOJI }, { "NotoColorEmoji", C_EMOJI | C_COLOR },
  { "LG_Display_Dingbat", C_SYM },
  { "LG_Display-Regular", C_SYM | C_HANGUL | C_GEN, { HB_SCRIPT_HEBREW, HB_SCRIPT_GREEK, HB_SCRIPT_CYRILLIC } },
  { "LG_Display_JP", C_KANA | C_HAN },
  { "DroidSansFallback", C_HAN | C_KANA | C_HANGUL | C_SYM | C_GEN },
  { "NanumGothic", C_HANGUL },
  { "DroidSansThai", 0, { HB_SCRIPT_THAI } }, { "NotoSansKhmer", 0, { HB_SCRIPT_KHMER } }, { "NotoSansLao", 0, { HB_SCRIPT_LAO } },
  { "NotoSansMyanmar", 0, { HB_SCRIPT_MYANMAR } }, { "NotoSansSinhala", 0, { HB_SCRIPT_SINHALA } },
  { "LG_Display_Devanagari", 0, { HB_SCRIPT_DEVANAGARI } }, { "LG_Display_Bengali", 0, { HB_SCRIPT_BENGALI } }, { "LG_Display_Tamil", 0, { HB_SCRIPT_TAMIL } },
  { "LG_Display_Telugu", 0, { HB_SCRIPT_TELUGU } }, { "LG_Display_Gujarati", 0, { HB_SCRIPT_GUJARATI } }, { "LG_Display_Gurmukhi", 0, { HB_SCRIPT_GURMUKHI } },
  { "LG_Display_Kannada", 0, { HB_SCRIPT_KANNADA } }, { "LG_Display_Malayalam", 0, { HB_SCRIPT_MALAYALAM } }, { "LG_Display_Oriya", 0, { HB_SCRIPT_ORIYA } },
  { "DroidSansArmenian", 0, { HB_SCRIPT_ARMENIAN } }, { "DroidSansGeorgian", 0, { HB_SCRIPT_GEORGIAN } }, { "DroidSansHebrew", 0, { HB_SCRIPT_HEBREW } },
  { "DroidSansEthiopic", 0, { HB_SCRIPT_ETHIOPIC } }, { "DroidNaskh", 0, { HB_SCRIPT_ARABIC } },
  { "Tinos", C_GEN, { HB_SCRIPT_HEBREW, HB_SCRIPT_GREEK, HB_SCRIPT_CYRILLIC } }, { "DroidSans.", C_GEN }, { "LG_Display-Light", C_GEN },
};
/* the Mac preview: the equivalent macOS fonts, by exact name */
static const FbSpec MACFONTS[] = {
  { "Apple Symbols.ttf", C_SYM }, { "Apple Color Emoji.ttc", C_EMOJI | C_COLOR },
  { "PingFang.ttc", C_HAN }, { "Hiragino Sans GB.ttc", C_HAN }, { "\xe3\x83\x92\xe3\x83\xa9\xe3\x82\xae\xe3\x83\x8e\xe8\xa7\x92\xe3\x82\xb4\xe3\x82\xb7\xe3\x83\x83\xe3\x82\xaf W3.ttc", C_KANA | C_HAN },
  { "AppleSDGothicNeo.ttc", C_HANGUL }, { "ThonburiUI.ttc", 0, { HB_SCRIPT_THAI } }, { "SFHebrew.ttf", 0, { HB_SCRIPT_HEBREW } },
  { "Kohinoor.ttc", 0, { HB_SCRIPT_DEVANAGARI } }, { "KohinoorBangla.ttc", 0, { HB_SCRIPT_BENGALI } }, { "KohinoorTelugu.ttc", 0, { HB_SCRIPT_TELUGU } },
  { "NotoSansArmenian.ttc", 0, { HB_SCRIPT_ARMENIAN } }, { "SFGeorgian.ttf", 0, { HB_SCRIPT_GEORGIAN } }, { "NotoSansMyanmar.ttc", 0, { HB_SCRIPT_MYANMAR } },
  { "Supplemental/Tamil Sangam MN.ttc", 0, { HB_SCRIPT_TAMIL } }, { "Supplemental/Khmer Sangam MN.ttf", 0, { HB_SCRIPT_KHMER } },
  { "Supplemental/Lao Sangam MN.ttf", 0, { HB_SCRIPT_LAO } }, { "Supplemental/Sinhala Sangam MN.ttc", 0, { HB_SCRIPT_SINHALA } },
  { "Supplemental/Gujarati Sangam MN.ttc", 0, { HB_SCRIPT_GUJARATI } }, { "Supplemental/Gurmukhi Sangam MN.ttc", 0, { HB_SCRIPT_GURMUKHI } },
  { "Supplemental/Kannada Sangam MN.ttc", 0, { HB_SCRIPT_KANNADA } }, { "Supplemental/Malayalam Sangam MN.ttc", 0, { HB_SCRIPT_MALAYALAM } },
  { "Supplemental/Oriya Sangam MN.ttc", 0, { HB_SCRIPT_ORIYA } }, { "Supplemental/Arial Unicode.ttf", C_SYM | C_GEN },
};
static void fb_add(const char *dir, const char *name, int cls, const hb_script_t *sc) {
  if (nfb >= MAX_FB) return; FbFile *f = &fbf[nfb++]; snprintf(f->path, sizeof f->path, "%s/%s", dir, name); f->cls = cls; if (sc) memcpy(f->sc, sc, sizeof f->sc);
}
static int cmp_name(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }
static void fb_init(void) {
  const char *dir = getenv("SB_FONTDIR"); /* development: another folder laid out like the TV's */
  int exact = 0;
#ifdef SB_MAC
  if (!dir) { dir = "/System/Library/Fonts"; exact = 1; }
#endif
  if (!dir) dir = "/usr/share/fonts";
  if (exact) { for (unsigned i = 0; i < sizeof MACFONTS / sizeof *MACFONTS; i++) { char p[400]; snprintf(p, sizeof p, "%s/%s", dir, MACFONTS[i].name); if (!access(p, R_OK)) fb_add(dir, MACFONTS[i].name, MACFONTS[i].cls, MACFONTS[i].sc); } return; }
  static char names[96][96]; int nn = 0; DIR *d = opendir(dir); struct dirent *e;
  /* LG's jails mount /usr/share/fonts read-only for every app; should a firmware leave it out, the bundled
     faces still cover Latin, Greek, Cyrillic and Arabic, and other scripts show as boxes */
  if (!d) { fprintf(stderr, "[text] system fonts %s not readable (%s): bundled fonts only\n", dir, strerror(errno)); return; }
  while ((e = readdir(d)) && nn < 96) { const char *x = strrchr(e->d_name, '.');
    if (!x || (strcasecmp(x, ".ttf") && strcasecmp(x, ".otf") && strcasecmp(x, ".ttc")) || strstr(e->d_name, "LastResort") || strlen(e->d_name) >= 96) continue;
    snprintf(names[nn++], 96, "%s", e->d_name); }
  closedir(d); qsort(names, nn, sizeof names[0], cmp_name);
  char used[96] = { 0 };
  for (unsigned i = 0; i < sizeof TVFONTS / sizeof *TVFONTS; i++) { size_t l = strlen(TVFONTS[i].name);
    for (int k = 0; k < nn; k++) if (!used[k] && !strncmp(names[k], TVFONTS[i].name, l)) { used[k] = 1; fb_add(dir, names[k], TVFONTS[i].cls, TVFONTS[i].sc); break; } }
  for (int k = 0; k < nn; k++) if (!used[k]) fb_add(dir, names[k], C_TAIL, NULL);
}
static int face_has_table(hb_face_t *f, hb_tag_t t) { hb_blob_t *b = hb_face_reference_table(f, t); unsigned n = hb_blob_get_length(b); hb_blob_destroy(b); return n > 0; }
static int face_weight_off(hb_face_t *f) { /* how far from an upright regular this face is (collections hold several weights) */
  hb_blob_t *b = hb_face_reference_table(f, HB_TAG('O','S','/','2')); unsigned n; const unsigned char *d = (const unsigned char *)hb_blob_get_data(b, &n);
  int off = 1000; if (n >= 64) { int w = d[4] << 8 | d[5], sel = d[62] << 8 | d[63]; off = abs(w - 400) + (sel & 1 ? 500 : 0); }
  hb_blob_destroy(b); return off;
}
static FbFile *fb_file(int i) { /* maps the file the first time any thread needs it */
  FbFile *f = &fbf[i]; pthread_mutex_lock(&fbmu);
  if (!f->state) { f->state = -1; int fd = open(f->path, O_RDONLY); struct stat st;
    if (fd >= 0 && !fstat(fd, &st) && st.st_size > 64) { void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
      if (m != MAP_FAILED) { hb_blob_t *b = hb_blob_create((const char *)m, (unsigned)st.st_size, HB_MEMORY_MODE_READONLY, NULL, NULL);
        unsigned nf = hb_face_count(b), idx = 0; int bestOff = 1 << 30;
        for (unsigned k = 0; k < nf && k < 32 && nf > 1; k++) { hb_face_t *t = hb_face_create(b, k); int o = face_weight_off(t); hb_face_destroy(t); if (o < bestOff) { bestOff = o; idx = k; } }
        hb_face_t *hf = hb_face_create(b, idx); hb_blob_destroy(b); f->index = (int)idx;
        int outl = face_has_table(hf, HB_TAG('g','l','y','f')) || face_has_table(hf, HB_TAG('C','F','F',' ')) || face_has_table(hf, HB_TAG('C','F','F','2'));
        int bm = hb_ot_color_has_png(hf); /* Apple's emoji font carries empty outlines beside its PNGs */
        if (hb_face_get_glyph_count(hf) > 1 && (outl || bm)) { f->data = m; f->len = (size_t)st.st_size; f->face = hf; f->bitmap = bm; f->state = 1; }
        else { hb_face_destroy(hf); munmap(m, (size_t)st.st_size); } } }
    if (fd >= 0) close(fd);
    fprintf(stderr, "[text] fallback font %s%s\n", f->path, f->state > 0 ? (f->bitmap ? " (colour, as a mask)" : "") : ": unusable"); }
  int ok = f->state > 0; pthread_mutex_unlock(&fbmu); return ok ? f : NULL;
}
static Font *fb_font(int i) { /* this thread's font on fallback file i */
  if (!FS->fbState[i]) { FS->fbState[i] = -1; FbFile *ff = fb_file(i);
    if (ff) { Font *f = &FS->fb[i]; memset(f, 0, sizeof *f); f->fam = FAM_FB + i; f->bitmap = ff->bitmap; f->hb = hb_font_create(ff->face); FS->fbState[i] = 1; } }
  return FS->fbState[i] > 0 ? &FS->fb[i] : NULL;
}
static FT_Face ft_sized(Font *f, int px) { /* the FreeType face at px, for rasterising (NULL: no outlines) */
  if (f->fam < FAM_FB) { font_size(f, px); return f->face; }
  if (f->bitmap) return NULL;
  if (!f->face) { FbFile *ff = &fbf[f->fam - FAM_FB]; if (FT_New_Memory_Face(FS->lib, (const FT_Byte *)ff->data, (FT_Long)ff->len, ff->index, &f->face)) { f->face = NULL; f->bitmap = -1; return NULL; } f->ftpx = 0; }
  if (f->ftpx != px) { FT_Set_Pixel_Sizes(f->face, 0, px); f->ftpx = px; }
  return f->face;
}
static int font_has(Font *f, uint32_t c) {
  if (f->fam >= FAM_FB) { hb_codepoint_t g = 0; return hb_font_get_nominal_glyph(f->hb, c, &g) && g; }
  return FT_Get_Char_Index(f->face, c) != 0;
}
static long read_file(const char *path, unsigned char **out) {
  FILE *fp = fopen(path, "rb"); if (!fp) return 0;
  fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
  *out = malloc(n); if (fread(*out, 1, n, fp) != (size_t)n) { free(*out); *out = NULL; n = 0; } fclose(fp); return n;
}
static void *text_worker(void *a); static int textThread; static double syncMs; static void text_take(void);
int text_init(const char *assetDir, float surfaceScale) {
  char p[1024];
  FS = &mainSet; shapeRtl = uiRtl;
  if (FT_Init_FreeType(&ft)) return 0;
  sscale = surfaceScale;
  snprintf(p, sizeof p, "%s/fonts/InterVariable.ttf", assetDir); fontLen[0] = read_file(p, &fontData[0]);
  snprintf(p, sizeof p, "%s/fonts/NotoSansArabic.ttf", assetDir); fontLen[1] = read_file(p, &fontData[1]);
  if (!fontLen[0]) { fprintf(stderr, "[text] no font at %s\n", p); return 0; }
  if (!(mainSet.inter = font_get(FAM_LATIN, 400))) return 0;
  uf = hb_unicode_funcs_get_default(); fb_init();
  if (!getenv("SB_TEXT_SYNC")) { pthread_t th; if (!pthread_create(&th, NULL, text_worker, NULL)) { pthread_detach(th); textThread = 1; } } /* SB_TEXT_SYNC: everything on the drawing thread (for comparison) */
  return 1;
}
void text_set_rtl(int r) { uiRtl = !!r; shapeRtl = uiRtl; }
int text_rtl(void) { return uiRtl; }
static unsigned frameNo;
static Uint64 frameT0;
void text_frame_begin(void) { budgetLines = 0; frameNo++; atlas_frame(); frameT0 = SDL_GetPerformanceCounter(); syncMs = 0; text_take(); }

/* Inter's metrics scaled: ascent .969, descent .242 of the em (so the line box is ~1.21 em);
   Arabic lines use the same box so mixed layouts don't jump. Line height in the CSS was set per
   element; the box here is just where the texture starts and ends. */
int text_line_height(int px) { return (int)(px * 1.21f + 0.999f); }

/* ---- unicode helpers --------------------------------------------------------------------- */
/* -1: a byte that is not UTF-8 and means nothing; a stray byte from 0xA0 up is read as Latin-1 (some addons send it raw) */
static int utf8_decode(const unsigned char *s, int n, int *i) {
  unsigned c = s[*i];
  if (c < 0x80) { (*i)++; return c; }
  int len = c >= 0xf8 ? 0 : c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc2 ? 2 : 0;
  if (len && *i + len <= n) {
    unsigned cp = len == 4 ? c & 7 : len == 3 ? c & 15 : c & 31; int k;
    for (k = 1; k < len && (s[*i + k] & 0xc0) == 0x80; k++) cp = (cp << 6) | (s[*i + k] & 63);
    if (k == len && cp >= (len == 4 ? 0x10000u : len == 3 ? 0x800u : 0x80u) && cp <= 0x10ffff) { *i += len; return (int)cp; }
  }
  (*i)++; return c >= 0xa0 ? (int)c : -1;
}
static int is_arabic(int cp) { return (cp >= 0x0600 && cp <= 0x06ff) || (cp >= 0x0750 && cp <= 0x077f) || (cp >= 0x08a0 && cp <= 0x08ff) || (cp >= 0xfb50 && cp <= 0xfdff) || (cp >= 0xfe70 && cp <= 0xfeff); }
/* ---- clusters and the font each one takes --------------------------------------------------- */
static int is_ri(uint32_t c) { return c >= 0x1F1E6 && c <= 0x1F1FF; }   /* regional indicators: two make a flag */
static int is_vs(uint32_t c) { return (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xE0100 && c <= 0xE01EF); }
static int is_ign(uint32_t c) { return c == 0x200D || c == 0x200C || is_vs(c) || (c >= 0xE0020 && c <= 0xE007F) || (c >= 0x200B && c <= 0x200F) || (c >= 0x202A && c <= 0x202E) || (c >= 0x2060 && c <= 0x206F) || c == 0xFEFF || c == 0x034F || c == 0x061C; }
static int is_mark(uint32_t c) { if (c < 0x300) return 0; hb_unicode_general_category_t g = hb_unicode_general_category(uf, c);
  return g == HB_UNICODE_GENERAL_CATEGORY_NON_SPACING_MARK || g == HB_UNICODE_GENERAL_CATEGORY_SPACING_MARK || g == HB_UNICODE_GENERAL_CATEGORY_ENCLOSING_MARK; }
/* continues the cluster before it: variation selectors, skin tones, tag letters, the keycap, combining marks */
static int is_ext(uint32_t c) { return c >= 0x300 && (is_vs(c) || (c >= 0x1F3FB && c <= 0x1F3FF) || (c >= 0xE0020 && c <= 0xE007F) || is_mark(c)); }
static int cluster_end(const uint32_t *c, int n, int i) { /* ZWJ sequences, keycaps, flags and marks stay with their base */
  int j = i + 1;
  if (is_ri(c[i])) return j < n && is_ri(c[j]) ? j + 1 : j;
  while (j < n) { if (is_ext(c[j])) j++; else if (c[j] == 0x200D && j + 1 < n) j += 2; else break; }
  return j;
}
static int cluster_cont(const uint32_t *c, int i) { /* c[i] (i > 0) belongs to the cluster before it */
  if (is_ri(c[i])) { int k = 0; while (i - 1 - k >= 0 && is_ri(c[i - 1 - k])) k++; return k & 1; }
  return is_ext(c[i]) || c[i] == 0x200D || c[i - 1] == 0x200D;
}
static int vs_base(uint32_t b) { return b == '#' || b == '*' || (b >= '0' && b <= '9') || b == 0xA9 || b == 0xAE || b >= 0x203C; } /* can take an emoji variation */
static int emoji_default(uint32_t c) { /* emoji presentation without U+FE0F (close to Unicode's Emoji_Presentation; the rest take text fonts first) */
  if (c < 0x231A) return 0;
  if ((c >= 0x1F300 && c <= 0x1F6FF) || (c >= 0x1F900 && c <= 0x1FAFF) || c == 0x1F004 || c == 0x1F0CF || c == 0x1F18E || (c >= 0x1F191 && c <= 0x1F19A) || (c >= 0x1F201 && c <= 0x1F251)) return 1;
  if (c >= 0x2648 && c <= 0x2653) return 1;
  static const unsigned short L[] = { 0x231A, 0x231B, 0x23E9, 0x23EA, 0x23EB, 0x23EC, 0x23F0, 0x23F3, 0x25FD, 0x25FE, 0x2614, 0x2615, 0x267F, 0x2693, 0x26A1, 0x26AA, 0x26AB, 0x26BD, 0x26BE,
    0x26C4, 0x26C5, 0x26CE, 0x26D4, 0x26EA, 0x26F2, 0x26F3, 0x26F5, 0x26FA, 0x26FD, 0x2705, 0x270A, 0x270B, 0x2728, 0x274C, 0x274E, 0x2753, 0x2754, 0x2755, 0x2757, 0x2795, 0x2796, 0x2797, 0x27B0, 0x27BF,
    0x2B1B, 0x2B1C, 0x2B50, 0x2B55 };
  for (unsigned i = 0; i < sizeof L / sizeof *L; i++) if (L[i] == c) return 1;
  return 0;
}
static int cp_class(uint32_t c, int emoji, hb_script_t *sc) {
  *sc = hb_unicode_script(uf, c);
  if (emoji) return C_EMOJI;
  if (*sc == HB_SCRIPT_HAN || *sc == HB_SCRIPT_BOPOMOFO) return C_HAN;
  if (*sc == HB_SCRIPT_HIRAGANA || *sc == HB_SCRIPT_KATAKANA) return C_KANA;
  if (*sc == HB_SCRIPT_HANGUL) return C_HANGUL;
  if ((c >= 0x3000 && c <= 0x303F) || (c >= 0x3200 && c <= 0x33FF) || (c >= 0xFE30 && c <= 0xFE4F) || (c >= 0xFF00 && c <= 0xFFEF)) return C_HAN; /* CJK punctuation, enclosed and full-width forms */
  if (*sc == HB_SCRIPT_COMMON || *sc == HB_SCRIPT_INHERITED || *sc == HB_SCRIPT_UNKNOWN) return C_SYM;
  return 0; /* a script: the fonts named for it, then the general ones */
}
/* the order fallback files are tried in for a class: 0 its own, 1 the other picture fonts, 2 general, 3 colour emoji (a mask), 4 unnamed files */
static int fb_rank(const FbFile *f, int cls, hb_script_t sc) {
  if (f->cls & C_COLOR) return 3;
  if (f->cls & C_TAIL) return 4;
  if ((f->cls & cls) || (sc && (f->sc[0] == sc || f->sc[1] == sc || f->sc[2] == sc))) return 0;
  if ((cls & (C_EMOJI | C_SYM)) && (f->cls & (C_EMOJI | C_SYM))) return 1;
  return f->cls & C_GEN ? 2 : -1;
}
static int fb_pick(const uint32_t *req, int nreq, int cls, hb_script_t sc) { /* the first font with the whole cluster, else the first with its base */
  int baseOnly = FID_NONE;
  for (int rank = 0; rank <= 4; rank++) for (int i = 0; i < nfb; i++) {
    if (FS->fbState[i] < 0 || fb_rank(&fbf[i], cls, sc) != rank) continue;
    Font *f = fb_font(i); if (!f || !font_has(f, req[0])) continue;
    int all = 1; for (int k = 1; k < nreq && all; k++) all = font_has(f, req[k]);
    if (all) return FAM_FB + i;
    if (baseOnly == FID_NONE) baseOnly = FAM_FB + i;
  }
  return baseOnly;
}
static int pick_font(const uint32_t *c, int i, int j) {
  uint32_t b = c[i];
  if (j == i + 1 && b < 0x80) return FAM_LATIN;
  if (is_ri(b)) return FID_TAG;
  if (is_arabic((int)b)) return FAM_ARABIC;
  uint32_t req[8]; int nreq = 0, emoji = 0; req[nreq++] = b;
  for (int k = i + 1; k < j; k++) { uint32_t d = c[k];
    if (d == 0xFE0F || d == 0x200D || d == 0x20E3 || (d >= 0x1F3FB && d <= 0x1F3FF) || (d >= 0xE0020 && d <= 0xE007F)) emoji = 1;
    if (!is_ign(d) && nreq < 8) req[nreq++] = d; }
  if (!emoji) emoji = emoji_default(b);
  if (j > i + 1 && c[i + 1] == 0xFE0E) emoji = 0;
  unsigned key = 0, slot = 0;
  if (nreq == 1 && j == i + 1) { key = (b << 1 | (unsigned)emoji) + 1; slot = (b * 2654435761u) >> 22 & (CPC_N - 1); if (FS->cpcKey[slot] == key) return FS->cpcFont[slot]; }
  Font *in = FS->inter; int allIn = in != NULL;
  for (int k = 0; k < nreq && allIn; k++) allIn = font_has(in, req[k]);
  int fid;
  if (allIn && !emoji) fid = FAM_LATIN;
  else { hb_script_t sc; int cls = cp_class(b, emoji, &sc); fid = fb_pick(req, nreq, cls, sc); if (fid == FID_NONE && in && font_has(in, b)) fid = FAM_LATIN; }
  if (key) { FS->cpcKey[slot] = key; FS->cpcFont[slot] = (short)fid; }
  return fid;
}

/* ---- shaping a line into glyph positions ---------------------------------------------------
   The text goes through FriBidi for the embedding levels, is split into clusters (a ZWJ emoji
   sequence, a keycap, a flag, a letter with its marks), each cluster takes the first font that has
   all of it, runs of one level, one font and one script are shaped by HarfBuzz, and the runs are
   placed in visual order. Characters no font has are dropped, never drawn as boxes. */
typedef struct { Font *font; unsigned gid; float x, y; short gpx; unsigned char bold; } Glyph;   /* x,y: surface px, pen position; gpx: its own size (0 = the line's) */
typedef struct { float x, y, w, h; } Box;   /* a flag's tag outline: x from the line start, y from the baseline (surface px) */
typedef struct { Glyph *g; int n, cap; float width; int cutAt; Box *box; int nbox; } Shaped;
static void glyph_push(Shaped *s, Font *f, unsigned gid, float x, float y, int bold) {
  if (s->n == s->cap) { s->cap = s->cap ? s->cap * 2 : 64; s->g = realloc(s->g, s->cap * sizeof *s->g); }
  Glyph *g = &s->g[s->n++]; g->font = f; g->gid = gid; g->x = x; g->y = y; g->gpx = 0; g->bold = (unsigned char)bold;
}
static void shaped_free(Shaped *s) { free(s->g); free(s->box); }
/* shapes cps[a..b) with one font in one direction, appending glyphs at pen x; returns the advance.
   bold: a fallback face drawn for a heavy weight (it has one weight only): its outlines are thickened */
static float shape_run(Shaped *out, Font *f, const uint32_t *cps, int a, int b, int rtl, int px, float tracking, float penX, hb_script_t sc, int bold) {
  font_size(f, px);
  hb_buffer_t *buf = hb_buffer_create();
  hb_buffer_add_codepoints(buf, cps, b, a, b - a);
  hb_buffer_set_direction(buf, rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
  if (f->fam >= FAM_FB) { hb_buffer_set_script(buf, sc ? sc : HB_SCRIPT_COMMON); hb_buffer_set_flags(buf, HB_BUFFER_FLAG_REMOVE_DEFAULT_IGNORABLES); }
  else hb_buffer_set_script(buf, f->fam == FAM_ARABIC ? HB_SCRIPT_ARABIC : HB_SCRIPT_LATIN);
  hb_buffer_set_language(buf, hb_language_get_default());
  hb_feature_t feats[2] = { { HB_TAG('c','v','1','1'), 1, 0, (unsigned)-1 }, { HB_TAG('s','s','0','1'), 1, 0, (unsigned)-1 } }; /* the web app's Inter features */
  hb_shape(f->hb, buf, f->fam == FAM_LATIN ? feats : NULL, f->fam == FAM_LATIN ? 2 : 0);
  unsigned n = hb_buffer_get_length(buf);
  hb_glyph_info_t *gi = hb_buffer_get_glyph_infos(buf, NULL); hb_glyph_position_t *gp = hb_buffer_get_glyph_positions(buf, NULL);
  float x = penX;
  for (unsigned i = 0; i < n; i++) {
    if (!gi[i].codepoint) continue; /* a character this font lacks: nothing (no box, no gap) */
    glyph_push(out, f, gi[i].codepoint, x + gp[i].x_offset / 64.0f, -gp[i].y_offset / 64.0f, bold);
    x += gp[i].x_advance / 64.0f + tracking;
  }
  hb_buffer_destroy(buf);
  return x - penX;
}
/* a flag (two regional indicators) as its country code in small capitals inside a thin rounded outline: the
   emoji fonts this TV has show flags as boxes or not at all, and a monochrome flag says nothing */
static float shape_tag(Shaped *out, const uint32_t *cps, int a, int b, int px, float tracking, float pen) {
  uint32_t l[2]; int k = 0; for (int i = a; i < b && k < 2; i++) l[k++] = 'A' + (cps[i] - 0x1F1E6);
  Font *f = font_get(FAM_LATIN, 600); if (!f) f = font_get(FAM_LATIN, 400); if (!f) return 0;
  int tpx = (int)(px * 0.56f + 0.5f); if (tpx < 6) tpx = 6;
  float gap = px * 0.1f, pad = px * 0.17f, capH = tpx * 0.727f, h = capH + px * 0.3f, mid = -px * 0.36f; /* centred on the capitals of the text */
  int g0 = out->n;
  float tw = shape_run(out, f, l, 0, k, 0, tpx, tpx * 0.04f, pen + gap + pad, 0, 0) - tpx * 0.04f;
  for (int i = g0; i < out->n; i++) { out->g[i].gpx = (short)tpx; out->g[i].y = mid + capH / 2; }
  out->box = realloc(out->box, (out->nbox + 1) * sizeof *out->box);
  Box *bx = &out->box[out->nbox++]; bx->x = pen + gap; bx->y = mid - h / 2; bx->w = tw + 2 * pad; bx->h = h;
  return tw + 2 * pad + 2 * gap + tracking;
}
static int strong_script(hb_script_t sc) { return sc && sc != HB_SCRIPT_COMMON && sc != HB_SCRIPT_INHERITED && sc != HB_SCRIPT_UNKNOWN; }
/* the whole line: cps are logical order; returns total advance; fills `out` in visual order */
static float shape_line(Shaped *out, const uint32_t *cps, int n, int px, int weight, float tracking) {
  if (n <= 0) return 0;
  FriBidiCharType *types = malloc(n * sizeof *types); FriBidiBracketType *brk = malloc(n * sizeof *brk);
  FriBidiLevel *levels = malloc(n * sizeof *levels);
  FriBidiParType base = shapeRtl ? FRIBIDI_PAR_RTL : FRIBIDI_PAR_LTR;
  fribidi_get_bidi_types((const FriBidiChar *)cps, n, types);
  /* Arabic UI: a line with no right-to-left letter at all (an addon's stream name, a Latin title) is laid out left to
     right, as the page does with unicode-bidi: plaintext; otherwise its emoji and numbers jump to the far end */
  if (base == FRIBIDI_PAR_RTL) { int i = 0; while (i < n && !FRIBIDI_IS_RTL(types[i])) i++; if (i == n) base = FRIBIDI_PAR_LTR; }
  fribidi_get_bracket_types((const FriBidiChar *)cps, n, types, brk);
  if (!fribidi_get_par_embedding_levels_ex(types, brk, n, &base, levels)) { for (int i = 0; i < n; i++) levels[i] = 0; }
  /* runs: same level, same font, and (for fallback fonts) one script; a Latin line is one cluster per letter, all Inter */
  typedef struct { int a, b, fid; hb_script_t sc; FriBidiLevel lv; } Run;
  Run *runs = malloc(n * sizeof *runs); int nr = 0;
  for (int i = 0; i < n;) {
    int j = cluster_end(cps, n, i), fid = pick_font(cps, i, j); uint32_t c = cps[i]; hb_script_t sc = 0;
    /* spaces and punctuation join the run before them, so words don't split fonts at every space (not after emoji or flags) */
    if (nr && j == i + 1 && (c == ' ' || c == '.' || c == ',' || c == ':' || (c >= '0' && c <= '9')) && runs[nr - 1].lv == levels[i] && runs[nr - 1].b == i) {
      int pf = runs[nr - 1].fid;
      if (pf == FAM_LATIN || pf == FAM_ARABIC) fid = pf;
      else if (pf >= FAM_FB && strong_script(runs[nr - 1].sc) && font_has(&FS->fb[pf - FAM_FB], c)) fid = pf;
    }
    if (fid == FID_NONE) { i = j; continue; }
    if (fid >= FAM_FB) { sc = hb_unicode_script(uf, c); if (!strong_script(sc)) sc = HB_SCRIPT_COMMON; }
    Run *p = nr ? &runs[nr - 1] : NULL;
    if (p && p->lv == levels[i] && p->fid == fid && fid != FID_TAG && p->b == i) {
      if (fid < FAM_FB || sc == HB_SCRIPT_COMMON || p->sc == sc) { p->b = j; i = j; continue; }
      if (p->sc == HB_SCRIPT_COMMON) { p->sc = sc; p->b = j; i = j; continue; }
    }
    runs[nr].a = i; runs[nr].b = j; runs[nr].fid = fid; runs[nr].sc = sc; runs[nr].lv = levels[i]; nr++; i = j;
  }
  /* visual order of the runs: reorder an index list by the runs' levels */
  FriBidiLevel *rl = malloc((nr + 1) * sizeof *rl); FriBidiStrIndex *map = malloc((nr + 1) * sizeof *map);
  for (int r = 0; r < nr; r++) { rl[r] = runs[r].lv; map[r] = r; }
  if (nr && !fribidi_reorder_line(0, types, nr, 0, base, rl, NULL, map)) for (int r = 0; r < nr; r++) map[r] = r;
  float pen = 0;
  for (int k = 0; k < nr; k++) {
    Run *r = &runs[map[k]];
    if (r->fid == FID_TAG) { pen += shape_tag(out, cps, r->a, r->b, px, tracking, pen); continue; }
    Font *f = r->fid >= FAM_FB ? fb_font(r->fid - FAM_FB) : font_get(r->fid, weight); if (!f) f = font_get(FAM_LATIN, weight); if (!f) continue;
    pen += shape_run(out, f, cps, r->a, r->b, r->lv & 1, px, tracking, pen, r->sc, weight >= 600 && f->fam >= FAM_FB);
  }
  free(types); free(brk); free(levels); free(runs); free(rl); free(map);
  out->width = pen;
  return pen;
}
/* clean: what is shaped. Tabs and newlines become spaces; control characters, zero-width spaces, BOMs, soft
   hyphens, lone surrogates, non-characters and the replacement character go (some addons send them), and a
   variation selector after plain text (which it cannot change) goes too */
static uint32_t *to_cps(const char *utf8, int *n, int clean) {
  int len = (int)strlen(utf8), i = 0, k = 0; uint32_t *c = malloc((len + 1) * sizeof *c);
  while (i < len) { int cp = utf8_decode((const unsigned char *)utf8, len, &i); if (cp < 0) continue;
    if (clean) {
      if (cp == '\t' || cp == '\n' || cp == '\r') cp = ' ';
      else if (cp < 0x20 || (cp >= 0x7f && cp < 0xa0) || cp == 0xad || cp == 0x200b || cp == 0xfeff || cp == 0x2060 || (cp >= 0xd800 && cp <= 0xdfff) || (cp >= 0xfff9 && cp <= 0xfffd) || (cp & 0xffff) >= 0xfffe) continue;
      else if ((cp == 0xfe0f || cp == 0xfe0e) && !(k && vs_base(c[k - 1]))) continue;
    }
    c[k++] = (uint32_t)cp; }
  *n = k; return c;
}
static int cps_to_utf8(const uint32_t *c, int n, char *out, int cap) {
  int k = 0;
  for (int i = 0; i < n && k + 4 < cap; i++) { uint32_t cp = c[i];
    if (cp < 0x80) out[k++] = (char)cp;
    else if (cp < 0x800) { out[k++] = (char)(0xc0 | (cp >> 6)); out[k++] = (char)(0x80 | (cp & 63)); }
    else if (cp < 0x10000) { out[k++] = (char)(0xe0 | (cp >> 12)); out[k++] = (char)(0x80 | ((cp >> 6) & 63)); out[k++] = (char)(0x80 | (cp & 63)); }
    else { out[k++] = (char)(0xf0 | (cp >> 18)); out[k++] = (char)(0x80 | ((cp >> 12) & 63)); out[k++] = (char)(0x80 | ((cp >> 6) & 63)); out[k++] = (char)(0x80 | (cp & 63)); } }
  out[k] = 0; return k;
}

/* ---- the line cache ----------------------------------------------------------------------- */
typedef struct {
  char *key; unsigned hash; SbLine line; unsigned lastUse; int used;
  AtlasSlot slot;                  /* where the line sits in the shared pages (page -1: a texture of its own) */
} Entry;
#define CACHE_N 1024
static Entry cache[CACHE_N]; static unsigned cacheCount;
static unsigned fnv(const char *s) { unsigned h = 2166136261u; while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; } return h; }
static Entry *cache_find(const char *key, unsigned h, int *slot) {
  unsigned i = h % CACHE_N;
  for (int n = 0; n < CACHE_N; n++, i = (i + 1) % CACHE_N) {
    if (!cache[i].used) { if (slot) *slot = (int)i; return NULL; }
    if (cache[i].hash == h && !strcmp(cache[i].key, key)) { cache[i].lastUse = frameNo; atlas_touch(&cache[i].slot); return &cache[i]; }
  }
  if (slot) *slot = -1; return NULL;
}
static void cache_drop(int i) {
  Entry *e = &cache[i]; if (!e->used) return;
  if (e->slot.page < 0) gfx_tex_free(e->line.tex); else atlas_release(&e->slot);
  free(e->key); memset(e, 0, sizeof *e); cacheCount--;
  /* close the gap so the entries probed past it stay reachable */
  int j = i;
  for (;;) { j = (j + 1) % CACHE_N; if (!cache[j].used) break;
    int k = (int)(cache[j].hash % CACHE_N);
    if ((j > i && (k <= i || k > j)) || (j < i && (k <= i && k > j))) { cache[i] = cache[j]; memset(&cache[j], 0, sizeof cache[j]); i = j; } }
}
static void cache_evict(void) { /* the least recently drawn line goes */
  unsigned oldest = 0xffffffffu; int at = -1;
  for (int i = 0; i < CACHE_N; i++) if (cache[i].used && cache[i].lastUse < oldest) { oldest = cache[i].lastUse; at = i; }
  if (at >= 0) cache_drop(at);
}
void text_gc(void) { for (int i = 0; i < CACHE_N; i++) if (cache[i].used && frameNo - cache[i].lastUse > 600) cache_drop(i); }

/* rasterises shaped glyphs into an alpha bitmap and uploads it */
/* a colour glyph (CBDT / sbix PNG) as an alpha mask: coverage times a little of its brightness, so the drawing
   inside an emoji survives when it is tinted like the text; box-filtered down to the glyph's size */
static void blit_png(unsigned char *bmp, int w, int hgt, Font *f, unsigned gid, int px, float x, float base) {
  font_size(f, px);
  hb_glyph_extents_t ex; if (!hb_font_get_glyph_extents(f->hb, gid, &ex) || ex.width <= 0 || ex.height >= 0) return;
  hb_blob_t *pb = hb_ot_color_glyph_reference_png(f->hb, gid); unsigned len = 0; const char *d = hb_blob_get_data(pb, &len);
  int pw = 0, ph = 0, ch; unsigned char *p = len ? stbi_load_from_memory((const unsigned char *)d, (int)len, &pw, &ph, &ch, 4) : NULL; hb_blob_destroy(pb);
  if (!p) return;
  /* bitmap strikes put their own origin anywhere (Apple's sit on the baseline, a full em tall): fit the picture to
     the line box instead, centred, so colour and outline emoji line up with the text alike */
  float gw = ex.width / 64.0f, gh = -ex.height / 64.0f, th = px * 1.0f, k = th / gh; if (gw * k > px * 1.3f) k = px * 1.3f / gw;
  float x0 = x + ex.x_bearing / 64.0f * k, y0 = base - px * 0.97f + (px * 1.21f - gh * k) / 2; gw *= k; gh *= k;
  float kx = pw / gw, ky = ph / gh;
  for (int y = (int)floorf(y0); y < (int)ceilf(y0 + gh); y++) { if (y < 0 || y >= hgt) continue;
    int sy0 = (int)((y - y0) * ky), sy1 = (int)ceilf((y + 1 - y0) * ky); if (sy0 < 0) sy0 = 0; if (sy1 > ph) sy1 = ph; if (sy1 <= sy0) continue;
    for (int xx = (int)floorf(x0); xx < (int)ceilf(x0 + gw); xx++) { if (xx < 0 || xx >= w) continue;
      int sx0 = (int)((xx - x0) * kx), sx1 = (int)ceilf((xx + 1 - x0) * kx); if (sx0 < 0) sx0 = 0; if (sx1 > pw) sx1 = pw; if (sx1 <= sx0) continue;
      unsigned acc = 0, cnt = 0;
      for (int sy = sy0; sy < sy1; sy++) for (int sx = sx0; sx < sx1; sx++) { const unsigned char *q = p + ((size_t)sy * pw + sx) * 4;
        unsigned lum = (q[0] * 54u + q[1] * 183u + q[2] * 19u) >> 8; acc += q[3] * (100u + lum * 155u / 255u) / 255u; cnt++; }
      unsigned v = acc / cnt + bmp[y * w + xx]; bmp[y * w + xx] = v > 255 ? 255 : (unsigned char)v; } }
  stbi_image_free(p);
}
static void raster_box(unsigned char *bmp, int w, int hgt, const Box *b, float x0, float y0, float stroke) { /* a rounded outline, anti-aliased */
  float r = b->h * 0.24f, cx = x0 + b->w / 2, cy = y0 + b->h / 2;
  for (int y = (int)floorf(y0); y < (int)ceilf(y0 + b->h); y++) { if (y < 0 || y >= hgt) continue;
    for (int x = (int)floorf(x0); x < (int)ceilf(x0 + b->w); x++) { if (x < 0 || x >= w) continue;
      float qx = fabsf(x + 0.5f - cx) - (b->w / 2 - r), qy = fabsf(y + 0.5f - cy) - (b->h / 2 - r);
      float d = sqrtf(fmaxf(qx, 0) * fmaxf(qx, 0) + fmaxf(qy, 0) * fmaxf(qy, 0)) + fminf(fmaxf(qx, qy), 0) - r; /* < 0 inside */
      float cov = 0.5f - (fabsf(d + stroke / 2) - stroke / 2); if (cov <= 0) continue; if (cov > 1) cov = 1;
      unsigned v = (unsigned)(cov * 225) + bmp[y * w + x]; bmp[y * w + x] = v > 255 ? 255 : (unsigned char)v; } }
}
static unsigned char *raster_bitmap(Shaped *s, int px, int *ow, int *oh, int *oasc) { /* any thread: glyphs into an alpha bitmap */
  int asc = (int)(px * 0.97f + 0.5f), hgt = (int)(px * 1.21f + 0.999f);
  int w = (int)ceilf(s->width) + 4; if (w < 1) w = 1; if (w > 4096) w = 4096;
  unsigned char *bmp = calloc((size_t)w * hgt, 1);
  for (int i = 0; i < s->nbox; i++) raster_box(bmp, w, hgt, &s->box[i], s->box[i].x + 2, asc + s->box[i].y, px / 18.0f < 1 ? 1 : px / 18.0f);
  for (int i = 0; i < s->n; i++) {
    Glyph *g = &s->g[i]; int gpx = g->gpx ? g->gpx : px;
    if (g->font->bitmap == 1) { blit_png(bmp, w, hgt, g->font, g->gid, gpx, g->x + 2, asc + g->y); continue; }
    FT_Face fc = ft_sized(g->font, gpx); if (!fc) continue;
    if (FT_Load_Glyph(fc, g->gid, FT_LOAD_DEFAULT | FT_LOAD_NO_HINTING | (g->font->fam >= FAM_FB ? FT_LOAD_NO_BITMAP : 0) | (g->bold ? 0 : FT_LOAD_RENDER))) continue;
    if (g->bold) { if (fc->glyph->format == FT_GLYPH_FORMAT_OUTLINE) FT_Outline_Embolden(&fc->glyph->outline, gpx * 64 / 26); if (FT_Render_Glyph(fc->glyph, FT_RENDER_MODE_NORMAL)) continue; }
    FT_Bitmap *b = &fc->glyph->bitmap;
    int gx = (int)floorf(g->x + 0.5f) + fc->glyph->bitmap_left + 2, gy = asc - fc->glyph->bitmap_top + (int)g->y;
    for (unsigned r = 0; r < b->rows; r++) { int y = gy + (int)r; if (y < 0 || y >= hgt) continue;
      for (unsigned c = 0; c < b->width; c++) { int x = gx + (int)c; if (x < 0 || x >= w) continue;
        unsigned v = b->buffer[r * b->pitch + c] + bmp[y * w + x]; bmp[y * w + x] = v > 255 ? 255 : (unsigned char)v; } }
  }
  *ow = w; *oh = hgt; *oasc = asc; return bmp;
}
static void upload_line(unsigned char *bmp, int w, int hgt, int asc, SbLine *out, AtlasSlot *slot) { /* the drawing thread: into the shared pages */
  if (atlas_put(bmp, w, hgt, -1, 1, slot)) { out->tex = atlas_tex(slot->page); atlas_uv(slot, &out->u0, &out->v0, &out->u1, &out->v1); }
  else { slot->page = -1; out->tex = gfx_tex_create(bmp, w, hgt, 1, 1); out->u0 = out->v0 = 0; out->u1 = out->v1 = 1; }
  out->w = w; out->h = hgt; out->baseline = asc;
}
static int rasterise(Shaped *s, int px, SbLine *out, AtlasSlot *slot) { int w, h, asc; unsigned char *bmp = raster_bitmap(s, px, &w, &h, &asc); upload_line(bmp, w, h, asc, out, slot); free(bmp); return 1; }

/* cut with an ellipsis so the shaped width fits maxW (surface px); returns the new cps count */
static int fit_ellipsis(uint32_t *cps, int n, int px, int weight, float tracking, float maxW, Shaped *s) {
  const uint32_t ell = 0x2026;
  int lo = 0, hi = n, best = 0;
  while (lo <= hi) { /* binary search on the prefix length */
    int mid = (lo + hi) / 2; uint32_t save = cps[mid]; cps[mid] = ell;
    Shaped t = { 0 }; float w = shape_line(&t, cps, mid + 1, px, weight, tracking); shaped_free(&t); cps[mid] = save;
    if (w <= maxW) { best = mid; lo = mid + 1; } else hi = mid - 1;
  }
  while (best > 0 && cluster_cont(cps, best)) best--; /* never inside a cluster (half a flag, a broken ZWJ sequence) */
  /* don't cut right after a space */
  while (best > 0 && cps[best - 1] == ' ') best--;
  cps[best] = ell; s->n = 0; s->nbox = 0; shape_line(s, cps, best + 1, px, weight, tracking);
  return best + 1;
}

/* ---- the text thread ---------------------------------------------------------------------------
   Bulk lines (a screen opening with dozens of labels) are shaped and rasterised here with its own fonts; the drawing
   thread only uploads the finished bitmaps. A line the frame can afford (the first two, or while under TEXT_SYNC_MS of
   text work) is still made at once, so a small change shows in the same frame. */
#define TEXT_SYNC_MS 2.5
#define TQ 256
typedef struct { char *key; unsigned hash; char *utf8; int px, weight, rtl; float tracking, maxW; } TReq;
typedef struct { char *key; unsigned hash; unsigned char *bmp; int w, h, asc, cut; float width; } TRes;
static TReq tq[TQ]; static int tqh, tqn; static TRes tr[TQ]; static int trn;
static pthread_mutex_t tmu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t tcv = PTHREAD_COND_INITIALIZER;
static struct { char *key; unsigned hash; } pend[TQ]; static int npend;   /* asked of the thread, not back yet (drawing thread only) */
static int pend_has(const char *k, unsigned h) { for (int i = 0; i < npend; i++) if (pend[i].hash == h && !strcmp(pend[i].key, k)) return 1; return 0; }
static void pend_del(const char *k, unsigned h) { for (int i = 0; i < npend; i++) if (pend[i].hash == h && !strcmp(pend[i].key, k)) { free(pend[i].key); pend[i] = pend[--npend]; return; } }
static int fit_ellipsis(uint32_t *cps, int n, int px, int weight, float tracking, float maxW, Shaped *s);
static void *text_worker(void *a) { plat_thread_background(4); (void)a;
  static FontSet ws; FS = &ws; if (FT_Init_FreeType(&ws.lib)) return NULL; ws.inter = font_get(FAM_LATIN, 400);
  for (;;) {
    pthread_mutex_lock(&tmu); while (!tqn) pthread_cond_wait(&tcv, &tmu); TReq q = tq[tqh]; tqh = (tqh + 1) % TQ; tqn--; pthread_mutex_unlock(&tmu);
    shapeRtl = q.rtl;
    int n; uint32_t *cps = to_cps(q.utf8, &n, 1); int spx = (int)(q.px * sscale + 0.5f); float str = q.tracking * sscale;
    Shaped sh = { 0 }; shape_line(&sh, cps, n, spx, q.weight, str); int cut = 0;
    if (q.maxW > 0 && sh.width > q.maxW * sscale) { fit_ellipsis(cps, n, spx, q.weight, str, q.maxW * sscale, &sh); cut = 1; }
    TRes r; memset(&r, 0, sizeof r); r.key = q.key; r.hash = q.hash; r.cut = cut; r.width = sh.width; r.bmp = raster_bitmap(&sh, spx, &r.w, &r.h, &r.asc);
    shaped_free(&sh); free(cps); free(q.utf8);
    pthread_mutex_lock(&tmu); if (trn < TQ) tr[trn++] = r; else { free(r.key); free(r.bmp); } pthread_mutex_unlock(&tmu);
  }
  return NULL;
}
static int text_ask(const char *key, unsigned h, const char *utf8, int px, int weight, float tracking, float maxW) {
  if (npend >= TQ) return 0;
  pthread_mutex_lock(&tmu); if (tqn >= TQ) { pthread_mutex_unlock(&tmu); return 0; }
  TReq *q = &tq[(tqh + tqn) % TQ]; q->key = strdup(key); q->hash = h; q->utf8 = strdup(utf8); q->px = px; q->weight = weight; q->rtl = uiRtl; q->tracking = tracking; q->maxW = maxW; tqn++;
  pthread_cond_signal(&tcv); pthread_mutex_unlock(&tmu);
  pend[npend].key = strdup(key); pend[npend].hash = h; npend++;
  return 1;
}
static void text_take(void) { /* the thread's finished lines into the cache (uploads are a few KB each) */
  TRes got[TQ]; int n; pthread_mutex_lock(&tmu); n = trn; memcpy(got, tr, n * sizeof *got); trn = 0; pthread_mutex_unlock(&tmu);
  for (int i = 0; i < n; i++) { TRes *r = &got[i]; pend_del(r->key, r->hash); int slot;
    if (!cache_find(r->key, r->hash, &slot) && !(cacheCount >= CACHE_N - 8 || slot < 0 ? (cache_evict(), cache_find(r->key, r->hash, &slot), slot < 0) : 0)) {
      SbLine line = { 0 }; AtlasSlot aslot; upload_line(r->bmp, r->w, r->h, r->asc, &line, &aslot);
      line.width = r->width / sscale; line.w = (int)ceilf(line.w / sscale); line.h = (int)ceilf(line.h / sscale); line.baseline = (int)(line.baseline / sscale + 0.5f); line.ellipsis = r->cut;
      Entry *e = &cache[slot]; e->key = r->key; r->key = NULL; e->hash = r->hash; e->line = line; e->lastUse = frameNo; e->used = 1; e->slot = aslot; cacheCount++; statRaster++; }
    free(r->key); free(r->bmp); }
}
/* cache keys are built for every label / picture in every frame: printf's float formatting was a tenth of the
   drawing thread's time on the TV */
static char *key_int(char *p, long v) { char t[24]; int n = 0; unsigned long u = v < 0 ? (unsigned long)(-v) : (unsigned long)v; do { t[n++] = (char)('0' + u % 10); u /= 10; } while (u); if (v < 0) *p++ = '-'; while (n) *p++ = t[--n]; return p; }
static long key_round(float f) { return (long)(f >= 0 ? f + 0.5f : f - 0.5f); }
static char keybuf[4200];
static void build_key(char *b, size_t cap, const char *utf8, int px, int weight, float tracking, float maxW) {
  char *q = b; q = key_int(q, px); *q++ = '|'; q = key_int(q, weight); *q++ = '|'; q = key_int(q, key_round(tracking * 100)); *q++ = '|'; q = key_int(q, key_round(maxW)); *q++ = '|'; q = key_int(q, uiRtl); *q++ = '|';
  size_t n = strlen(utf8), room = cap - (size_t)(q - b) - 1; if (n > room) n = room; memcpy(q, utf8, n); q[n] = 0;
}
static const SbLine *text_line_(const char *utf8, int px, int weight, float tracking, float maxW);
const SbLine *text_line(const char *utf8, int px, int weight, float tracking, float maxW) { Uint32 t0 = SDL_GetTicks(); const SbLine *l = text_line_(utf8, px, weight, tracking, maxW); statLineMs += SDL_GetTicks() - t0; statLookups++; return l; }
static const SbLine *text_line_(const char *utf8, int px, int weight, float tracking, float maxW) {
  if (!utf8) utf8 = "";
  build_key(keybuf, sizeof keybuf, utf8, px, weight, tracking, maxW);
  unsigned h = fnv(keybuf); int slot;
  Entry *e = cache_find(keybuf, h, &slot);
  if (e && e->slot.page >= 0 && !atlas_valid(&e->slot)) { int at = (int)(e - cache); cache_drop(at); e = cache_find(keybuf, h, &slot); } /* its page was wiped */
  if (e) return &e->line;
  if (pend_has(keybuf, h)) return NULL; /* on its way from the text thread */
  if (textThread && budgetLines >= 2 && syncMs > TEXT_SYNC_MS) { text_ask(keybuf, h, utf8, px, weight, tracking, maxW); return NULL; } /* the rest goes to the text thread */
  if (!textThread && budgetLines >= LINES_PER_FRAME && (double)(SDL_GetPerformanceCounter() - frameT0) * 1000.0 / (double)SDL_GetPerformanceFrequency() > TEXT_MS) return NULL;
  budgetLines++; Uint64 ts0 = SDL_GetPerformanceCounter();
  if (cacheCount >= CACHE_N - 8 || slot < 0) { cache_evict(); cache_find(keybuf, h, &slot); if (slot < 0) return NULL; }
  int n; uint32_t *cps = to_cps(utf8, &n, 1);
  int spx = (int)(px * sscale + 0.5f); float str = tracking * sscale;
  Shaped s = { 0 }; shape_line(&s, cps, n, spx, weight, str); statShape++; statRaster++;
  int cut = 0;
  if (maxW > 0 && s.width > maxW * sscale) { fit_ellipsis(cps, n, spx, weight, str, maxW * sscale, &s); cut = 1; }
  SbLine line = { 0 }; AtlasSlot aslot; rasterise(&s, spx, &line, &aslot);
  line.width = s.width / sscale; line.w = (int)ceilf(line.w / sscale); line.h = (int)ceilf(line.h / sscale); line.baseline = (int)(line.baseline / sscale + 0.5f); line.ellipsis = cut;
  shaped_free(&s); free(cps);
  e = &cache[slot]; e->key = strdup(keybuf); e->hash = h; e->line = line; e->lastUse = frameNo; e->used = 1; e->slot = aslot; cacheCount++;
  syncMs += (double)(SDL_GetPerformanceCounter() - ts0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
  return &e->line;
}
/* widths alone: a drawn line already knows its own; anything else (the prefixes wrapping tries, labels measured for
   layout) is shaped once and kept here, never rasterised (asking for a line here used to rasterise every prefix) */
#define WC_N 4096
typedef struct { unsigned hash; char *key; float w; } WEnt;
static WEnt wcache[WC_N];
float text_width(const char *utf8, int px, int weight, float tracking) {
  if (!utf8 || !*utf8) return 0;
  static char wk[4200]; build_key(wk, sizeof wk, utf8, px, weight, tracking, 0); unsigned h = fnv(wk); int slot;
  Entry *e = cache_find(wk, h, &slot); if (e) return e->line.width;
  WEnt *we = &wcache[h & (WC_N - 1)]; if (we->key && we->hash == h && !strcmp(we->key, wk)) return we->w;
  Uint32 t0 = SDL_GetTicks(); int n; uint32_t *cps = to_cps(utf8, &n, 1); Shaped s = { 0 };
  float w = shape_line(&s, cps, n, (int)(px * sscale + 0.5f), weight, tracking * sscale) / sscale; shaped_free(&s); free(cps); statShape++; statWidthMs += SDL_GetTicks() - t0;
  free(we->key); we->key = strdup(wk); we->hash = h; we->w = w; return w;
}
float text_draw(const char *utf8, float x, float y, int px, int weight, uint32_t color, float tracking, float maxW, int align) {
  if (!utf8 || !*utf8) return 0;
  const SbLine *l = text_line(utf8, px, weight, tracking, maxW);
  if (!l) return 0;
  { int r = uiRtl && !gfx_mirrored(); if (align == ALIGN_START) align = r ? ALIGN_RIGHT : ALIGN_LEFT; else if (align == ALIGN_END) align = r ? ALIGN_LEFT : ALIGN_RIGHT; } /* (mirrored: laid out left to right, the mirror turns it) */
  float dx = align == ALIGN_CENTER ? -l->width / 2 : align == ALIGN_RIGHT ? -l->width : 0;
  /* the bitmap is 2 px wider on the left (glyph overhang) */
  gfx_alpha_tex_uv(l->tex, x + dx - 2.0f / sscale, y, (float)l->w, (float)l->h, color, l->u0, l->v0, l->u1, l->v1);
  return l->width;
}

/* ---- wrapping ------------------------------------------------------------------------------- */
/* Chinese and Japanese break between characters (not before closing punctuation or small kana, not after opening
   punctuation); Korean breaks at spaces like Latin */
static int is_cjk_brk(uint32_t c) { return (c >= 0x2E80 && c <= 0x9FFF && !(c >= 0x3130 && c <= 0x318F)) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFF00 && c <= 0xFFEF) || (c >= 0x20000 && c <= 0x3FFFF); }
static int no_brk_before(uint32_t c) { static const unsigned short L[] = { 0x3001, 0x3002, 0xFF0C, 0xFF0E, 0x30FB, 0xFF1A, 0xFF1B, 0xFF1F, 0xFF01, 0x30FC, 0x300D, 0x300F, 0xFF09, 0x3011, 0x3009, 0x300B, 0x3015,
    0x3041, 0x3043, 0x3045, 0x3047, 0x3049, 0x3063, 0x3083, 0x3085, 0x3087, 0x30A1, 0x30A3, 0x30A5, 0x30A7, 0x30A9, 0x30C3, 0x30E3, 0x30E5, 0x30E7, 0x2026 };
  for (unsigned i = 0; i < sizeof L / sizeof *L; i++) if (L[i] == c) return 1; return c == ')' || c == '.' || c == ',' || c == '!' || c == '?' || c == ':' || c == ';'; }
static int no_brk_after(uint32_t c) { return c == 0x300C || c == 0x300E || c == 0xFF08 || c == 0x3010 || c == 0x3008 || c == 0x300A || c == 0x3014 || c == '('; }
static int brk_cjk(const uint32_t *c, int at, int p) { /* a break between c[p-1] and c[p] inside CJK text */
  return p > at && (is_cjk_brk(c[p - 1]) || is_cjk_brk(c[p])) && c[p] != ' ' && !no_brk_before(c[p]) && !no_brk_after(c[p - 1]) && !(c[p] < 0x80 && c[p - 1] < 0x80);
}
typedef struct { int px, weight; float tracking; } WrapMeasure;
static float wrap_width(const uint32_t *cps, int at, int end, void *context) {
  const WrapMeasure *m = context; char tmp[1024];
  cps_to_utf8(cps + at, end - at, tmp, sizeof tmp);
  return text_width(tmp, m->px, m->weight, m->tracking);
}
static int wrap_lines(const char *utf8, float maxW, int maxLines, int px, int weight, float tracking, char lines[][1024], int *ellipsisLast) {
  int n; uint32_t *cps = to_cps(utf8, &n, 0); int at = 0, nl = 0; *ellipsisLast = 0;
  int *cand = malloc((n + 1) * sizeof *cand);
  while (at < n && nl < maxLines) {
    while (at < n && (cps[at] == ' ' || cps[at] == '\n')) at++;
    if (at >= n) break;
    /* where this line may end: spaces, the paragraph's end, and between CJK characters */
    int stop = at; while (stop < n && cps[stop] != '\n') stop++;
    int nc = 0;
    for (int p = at + 1; p <= stop; p++) {
      /* Prefer separators in release filenames, URLs and paths; never split a grapheme cluster. */
      if (p == stop || (!cluster_cont(cps, p) &&
          (cps[p] == ' ' || brk_cjk(cps, at, p) ||
           cps[p - 1] == '.' || cps[p - 1] == '_' ||
           cps[p - 1] == '-' || cps[p - 1] == '/'))) cand[nc++] = p;
    }
    int fitEnd = -1; char tmp[1024];
    { /* Prefer the longest natural break which fits, otherwise hard-wrap an oversized token. */
      int lo = 0, hi = nc - 1, best = -1;
      while (lo <= hi) { int mid = lo + (hi - lo) / 2; cps_to_utf8(cps + at, cand[mid] - at, tmp, sizeof tmp);
        if (text_width(tmp, px, weight, tracking) <= maxW) { best = mid; lo = mid + 1; } else hi = mid - 1; }
      if (best >= 0) fitEnd = cand[best];
      else { WrapMeasure m = { px, weight, tracking };
        fitEnd = wrapbreak_fit(cps, at, cand[0], maxW, wrap_width, cluster_cont, &m); }
    }
    if (fitEnd <= at) fitEnd = stop > at ? stop : at + 1;
    cps_to_utf8(cps + at, fitEnd - at, lines[nl], 1024); nl++;
    at = fitEnd; if (at < n && cps[at] == '\n') at++;
    if (nl == maxLines && at < n) { *ellipsisLast = 1; }
  }
  free(cand); free(cps); return nl;
}
/* a paragraph is laid out once: wrapped text is drawn (and its lines counted) every frame */
#define WR_N 512
typedef struct { unsigned hash; char *key; int nl, ell; char *buf; } WrEnt;
static WrEnt wrcache[WR_N];
static int wrap_cached(const char *utf8, float maxW, int maxLines, int px, int weight, float tracking, const char **lines, int *ell) {
  static char k[4300]; char *q = k; q = key_int(q, maxLines); *q++ = '/'; build_key(q, sizeof k - (size_t)(q - k), utf8, px, weight, tracking, maxW);
  unsigned h = fnv(k); WrEnt *w = &wrcache[h & (WR_N - 1)];
  if (!(w->key && w->hash == h && !strcmp(w->key, k))) {
    static char tmp[12][1024]; int e = 0, nl = wrap_lines(utf8, maxW, maxLines, px, weight, tracking, tmp, &e);
    size_t total = 0; for (int i = 0; i < nl; i++) total += strlen(tmp[i]) + 1;
    free(w->key); free(w->buf); w->key = strdup(k); w->hash = h; w->nl = nl; w->ell = e; w->buf = malloc(total ? total : 1);
    char *b = w->buf; for (int i = 0; i < nl; i++) { size_t L = strlen(tmp[i]) + 1; memcpy(b, tmp[i], L); b += L; } }
  const char *b = w->buf; for (int i = 0; i < w->nl; i++) { lines[i] = b; b += strlen(b) + 1; }
  *ell = w->ell; return w->nl;
}
int text_wrap_count(const char *utf8, float maxW, int maxLines, int px, int weight, float tracking) {
  const char *lines[12]; int ell; if (!utf8 || !*utf8) return 0;
  return wrap_cached(utf8, maxW, maxLines > 12 ? 12 : maxLines, px, weight, tracking, lines, &ell);
}
int text_wrapped_ready(const char *utf8, float maxW, int maxLines, int px, int weight, float tracking) {
  const char *lines[12]; int ell, ok = 1; if (!utf8 || !*utf8) return 1;
  int nl = wrap_cached(utf8, maxW, maxLines > 12 ? 12 : maxLines, px, weight, tracking, lines, &ell);
  for (int i = 0; i < nl; i++) if (!text_line(lines[i], px, weight, tracking, (i == nl - 1 && ell) ? maxW : 0)) ok = 0; /* asks for every missing one */
  return ok;
}
int text_draw_wrapped(const char *utf8, float x, float y, float maxW, int maxLines, int px, int weight, uint32_t color, float tracking, int align, float lineHeight) {
  const char *lines[12]; int ell; if (!utf8 || !*utf8) return 0;
  int nl = wrap_cached(utf8, maxW, maxLines > 12 ? 12 : maxLines, px, weight, tracking, lines, &ell);
  /* a paragraph shows whole: lines past the frame's budget come from the text thread a frame or two later, and a
     first line drawn alone made the rest pop in under it; nothing is drawn until every line is in */
  int miss = 0; for (int i = 0; i < nl; i++) if (!text_line(lines[i], px, weight, tracking, (i == nl - 1 && ell) ? maxW : 0)) miss = 1;
  if (miss) return nl;
  float lh = lineHeight > 0 ? lineHeight : text_line_height(px);
  int rr = uiRtl && !gfx_mirrored(), al = align == ALIGN_START ? (rr ? ALIGN_RIGHT : ALIGN_LEFT) : align == ALIGN_END ? (rr ? ALIGN_LEFT : ALIGN_RIGHT) : align;
  for (int i = 0; i < nl; i++) /* x is the anchor, as in text_draw: the left edge, the centre or the right edge */
    text_draw(lines[i], x, y + i * lh, px, weight, color, tracking, (i == nl - 1 && ell) ? maxW : 0, al);
  return nl;
}

/* ---- upper case ------------------------------------------------------------------------------ */
static uint32_t upper_cp(uint32_t c, int turkish) {
  if (c >= 'a' && c <= 'z') return turkish && c == 'i' ? 0x130 : c - 32;
  if (turkish && c == 0x131) return 'I';
  if ((c >= 0xE0 && c <= 0xFE && c != 0xF7)) return c - 0x20;
  if (c == 0xFF) return 0x178;
  if ((c >= 0x100 && c <= 0x137 && (c & 1)) || (c >= 0x14A && c <= 0x177 && (c & 1))) return c - 1;
  if ((c >= 0x139 && c <= 0x148 && !(c & 1)) || (c >= 0x179 && c <= 0x17E && !(c & 1))) return c - 1;
  if (c >= 0x430 && c <= 0x44F) return c - 0x20;
  if (c >= 0x450 && c <= 0x45F) return c - 0x50;
  if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return c - 0x20;
  return c;
}
void text_upper(char *s, size_t cap) {
  if (!s || !cap) return; int tr = !strcmp(i18n_lang(), "tr");
  size_t n = strlen(s); char *out = malloc(n * 2 + 1); size_t o = 0; int i = 0;
  while (i < (int)n) { int d = utf8_decode((const unsigned char *)s, (int)n, &i); if (d < 0) continue; uint32_t c = upper_cp((uint32_t)d, tr);
    if (c < 0x80) out[o++] = (char)c; else if (c < 0x800) { out[o++] = (char)(0xC0 | (c >> 6)); out[o++] = (char)(0x80 | (c & 63)); }
    else if (c < 0x10000) { out[o++] = (char)(0xE0 | (c >> 12)); out[o++] = (char)(0x80 | ((c >> 6) & 63)); out[o++] = (char)(0x80 | (c & 63)); }
    else { out[o++] = (char)(0xF0 | (c >> 18)); out[o++] = (char)(0x80 | ((c >> 12) & 63)); out[o++] = (char)(0x80 | ((c >> 6) & 63)); out[o++] = (char)(0x80 | (c & 63)); } } /* emoji are four bytes */
  out[o] = 0; if (o >= cap) { o = cap - 1; while (o > 0 && ((unsigned char)out[o] & 0xC0) == 0x80) o--; out[o] = 0; } /* never cut a character in half */
  memcpy(s, out, o + 1); free(out);
}
