/* Text: FreeType rasterising, HarfBuzz shaping, FriBidi ordering. A line of text is shaped and
   drawn once into an alpha texture and kept in a cache keyed by its text, size, weight and
   width limit; drawing a cached line is one quad. Inter is the UI face; Arabic script falls
   back to Noto Sans Arabic per run, so mixed lines (an Arabic title with a Latin name) work.
   Anything else (emoji, symbols, CJK, Korean, Hebrew, Thai, Indic...) takes the first system font
   that has it, per cluster (the TV's /usr/share/fonts, macOS's fonts in the preview), mapped on
   first need; emoji are drawn monochrome in the text colour, flags as a country-code tag, and a
   character no font has is dropped (never a box). */
#ifndef SB_TEXT_H
#define SB_TEXT_H
#include <stdint.h>
#include "ui/gl.h"
#include <stddef.h>

enum { ALIGN_LEFT = 0, ALIGN_CENTER = 1, ALIGN_RIGHT = 2, ALIGN_START = 3, ALIGN_END = 4 }; /* START/END follow the UI direction */

typedef struct {
  GLuint tex;        /* alpha texture of the whole line (0 while it is being made) */
  int w, h;          /* texture size in layout units (the line box: ascent + descent) */
  float width;       /* advance width of the text in layout units */
  int baseline;      /* baseline offset from the top of the box */
  int ellipsis;      /* 1 when the text was cut to fit */
  float u0, v0, u1, v1; /* where the line sits in its texture */
} SbLine;

int  text_init(const char *assetDir, float surfaceScale);
void text_set_scale(float surfaceScale); /* window resized: new glyphs rasterise at the new resolution (old cache ages out) */
void text_set_rtl(int rtl);      /* the UI direction: START/END alignment, base paragraph direction */
int  text_rtl(void);
void text_frame_begin(void);     /* resets the per-frame rasterising budget */
void text_gc(void);              /* drops lines not drawn lately (call when a screen changes) */

/* layout metrics for a font size: the line box height and where the baseline sits */
int  text_line_height(int px);

/* Measures without drawing (shapes; cached too) */
float text_width(const char *utf8, int px, int weight, float tracking);

/* A line (shaped, rasterised, cached). maxW > 0 cuts the text with an ellipsis to fit.
   Returns NULL only when the per-frame budget is spent (draw again next frame). */
/* upper case in place for the app's languages (Latin with accents, Polish, Turkish dotted i, Cyrillic, Greek); cap = the buffer size */
void text_upper(char *s, size_t cap);
const SbLine *text_line(const char *utf8, int px, int weight, float tracking, float maxW);

/* Draws a single line at (x, y = top of the line box). Returns the drawn width. */
float text_draw(const char *utf8, float x, float y, int px, int weight, uint32_t color, float tracking, float maxW, int align);

/* Wraps into at most maxLines lines of maxW; the last line gets an ellipsis when cut. Draws them
   lineHeight apart (0 = the font's). Returns the number of lines drawn. */
int text_draw_wrapped(const char *utf8, float x, float y, float maxW, int maxLines, int px, int weight, uint32_t color, float tracking, int align, float lineHeight);
/* whether every line of the wrap is rasterised (asks for the missing ones; drawing shows a paragraph only whole) */
int text_wrapped_ready(const char *utf8, float maxW, int maxLines, int px, int weight, float tracking);
/* how many lines the wrap would take (for layout), without drawing */
int text_wrap_count(const char *utf8, float maxW, int maxLines, int px, int weight, float tracking);

void text_stats(double *lineMs, double *widthMs, int *raster, int *shape, int *lookups); /* since the last call */
#endif
