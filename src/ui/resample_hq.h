#ifndef SB_RESAMPLE_HQ_H
#define SB_RESAMPLE_HQ_H
/* High-quality, alpha-aware Lanczos3 resize for the Continue Watching cards.
   Returns newly allocated RGBA pixels; caller owns them. */
unsigned char *sb_resample_lanczos3_rgba(const unsigned char *src, int w, int h,
                                         float cx, float cy, float cw, float ch,
                                         int nw, int nh);
/* Conservative unsharp mask: RGB only, local contrast capped, alpha preserved. */
void sb_unsharp_mild_rgba(unsigned char *px, int w, int h);
#endif
