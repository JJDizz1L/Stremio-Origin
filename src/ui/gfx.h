/* The renderer: every screen draws itself each frame with these calls, in a 1920x1080 layout
   space that is scaled to the real surface. Quads are batched into one stream per shader
   (shapes with rounded corners and soft edges, textured quads, text) and flushed when the
   shader, texture or clip changes. Only GLES2-level GL is used, so the same code runs on the TV
   (GLES2) and the Mac preview (OpenGL 2.1). */
#ifndef SB_GFX_H
#define SB_GFX_H
#include <stdint.h>
#include "ui/gl.h"

#define SB_W 1920.0f
#define SB_H 1080.0f

/* colours are 0xAARRGGBB */
#define RGBA(r, g, b, a) ((uint32_t)(((a) & 255) << 24 | ((r) & 255) << 16 | ((g) & 255) << 8 | ((b) & 255)))
#define RGB(r, g, b) RGBA(r, g, b, 255)
#define WHITE_A(a) RGBA(255, 255, 255, (a))
#define BLACK_A(a) RGBA(0, 0, 0, (a))
static inline uint32_t col_alpha(uint32_t c, float k) { /* multiply the alpha */
  float a = ((c >> 24) & 255) * k; if (a < 0) a = 0; if (a > 255) a = 255;
  return (c & 0x00ffffffu) | ((uint32_t)(a + 0.5f) << 24); }
static inline uint32_t col_mix(uint32_t a, uint32_t b, float t) {
  uint32_t o = 0; for (int s = 0; s < 32; s += 8) { float x = (a >> s) & 255, y = (b >> s) & 255; o |= (uint32_t)(x + (y - x) * t + 0.5f) << s; }
  return o; }

typedef struct { float x, y, w, h; } SbRect;
static inline SbRect rect(float x, float y, float w, float h) { SbRect r = { x, y, w, h }; return r; }

int  gfx_init(int surfaceW, int surfaceH);
void gfx_resize(int surfaceW, int surfaceH);
void gfx_begin(uint32_t clear);   /* clears to the colour (alpha included: the video shows through alpha 0) */
void gfx_end(void);               /* flushes what is left */
void gfx_flush(void);
void gfx_invalidate_state(void);
float gfx_scale(void);            /* surface pixels per layout unit */
void  gfx_view_rect(int *x, int *y, int *w, int *h); /* the letterboxed UI viewport in surface pixels (GL bottom-left origin) */
void  gfx_surface_to_layout(float *x, float *y);     /* drawable pixels (top-left origin) -> 1920x1080 layout */
void  gfx_layout_to_surface(float x, float y, float w, float h, int *px, int *py, int *pw, int *ph); /* layout -> drawable pixels (top-left origin) */
void  gfx_xform_point(float *x, float *y);   /* layout -> surface pixels, through the offset and scale stacks */

/* shapes (SDF rounded rectangles) */
void gfx_rect(float x, float y, float w, float h, uint32_t color);
void gfx_rrect(float x, float y, float w, float h, float radius, uint32_t color);
void gfx_rrect_v(float x, float y, float w, float h, float radius, uint32_t top, uint32_t bottom); /* vertical gradient */
void gfx_rrect_h(float x, float y, float w, float h, float radius, uint32_t left, uint32_t right);
void gfx_ring(float x, float y, float w, float h, float radius, float thickness, uint32_t color);  /* outline, drawn inside */
void gfx_ring_out(float x, float y, float w, float h, float radius, float thickness, uint32_t color); /* outline outside the rect */
void gfx_shadow(float x, float y, float w, float h, float radius, float blur, uint32_t color);
void gfx_glare_at(float x, float y, float w, float h, float r, float pos, uint32_t c, float shift); /* the glint moved along the top */
void gfx_glare(float x, float y, float w, float h, float radius, float pos, uint32_t color); /* a soft slanted band of light across a rounded box; pos 0..1 from its left edge to its right */
void gfx_shadow_under(float x, float y, float w, float h, float r, float blur, uint32_t c, float bx, float by, float bw, float bh, float br); /* the shadow of an opaque box drawn over it */
float gfx_opacity_now(void);
void gfx_circle(float cx, float cy, float r, uint32_t color);

/* textures */
void gfx_tex_recycle(GLuint t, int w, int h, int ch, int linear); /* a picture's texture let go: kept a moment for the next one of its size */
GLuint gfx_tex_create(const unsigned char *rgba, int w, int h, int channels, int linear); /* channels 1 (alpha), 3, 4 */
void   gfx_tex_free(GLuint tex);
/* a picture that just arrived fades in over 0.2 s wherever it is drawn (never pops in); gfx_tex_fade is its
   opacity now (1 = settled), so a placeholder under it can stay until it is */
void   gfx_tex_fade_in(GLuint tex);
float  gfx_tex_fade(GLuint tex);
/* an image in a rounded rectangle; uv = the part of the texture shown (cover-crop), tint = colour multiplier */
/* liquid glass: tex a blurred picture of what is behind (u0..v1 the part under this box), drawn in towards the edges
   by up to `pull` units with a colour fringe, its colours richer */
void gfx_liquid_spot(float u, float v, float k); /* the next liquid quad: a moving spot of light at (u, v) of the box (0..1) bending the picture under it, k 0..1 */
void gfx_liquid(GLuint tex, float x, float y, float w, float h, float r, float pull, float alpha, float u0, float v0, float u1, float v1);
void gfx_image(GLuint tex, float x, float y, float w, float h, float radius, uint32_t tint, float u0, float v0, float u1, float v1);
/* the same turned in depth (rx about its horizontal middle, ry about its vertical one, degrees), seen from dist layout units */
/* an alpha texture (text) coloured */
void gfx_alpha_tex_rot(GLuint tex, float x, float y, float w, float h, float deg, uint32_t c, float u0, float v0, float u1, float v1); /* turned about its centre */
void gfx_alpha_tex(GLuint tex, float x, float y, float w, float h, uint32_t color);
void gfx_alpha_tex_uv(GLuint tex, float x, float y, float w, float h, uint32_t color, float u0, float v0, float u1, float v1);
void gfx_tex_update(GLuint tex, const unsigned char *px, int x, int y, int w, int h, int channels); /* replaces a part of a texture */

/* offscreen pictures: draw once into a texture, then show it as one quad (the texture is upside
   down: show it with v0 = 1, v1 = 0) */
typedef struct { GLuint tex, fbo; int w, h; } GfxTarget;
int  gfx_target_create(GfxTarget *t, int w, int h);   /* 0 when the driver cannot draw into textures */
void gfx_target_free(GfxTarget *t);
unsigned gfx_frame(void);                             /* frames begun so far */
void gfx_target_begin(GfxTarget *t, uint32_t clear);  /* what is drawn next goes into t, in layout units */
void gfx_target_end(void);
void gfx_blur_pass(GfxTarget *dst, unsigned src, int srcW, int srcH, int vertical, float spread); /* one gaussian pass of a target into another */
void gfx_set_screen_fbo(GLuint fbo);                  /* frames go to this framebuffer instead of the window (0 = the window) */

/* state */
void gfx_clip_push(float x, float y, float w, float h);  /* intersects with the current clip */
void gfx_clip_pop(void);
/* mirrored drawing (a left-to-right picture shown right to left): where everything lands is flipped about the
   vertical line at cx; pictures and words keep their own way round. Not nested */
void gfx_mirror_push(float cx);
void gfx_mirror_pop(void);
int  gfx_mirrored(void);
void gfx_opacity_push(float k);   /* multiplies the alpha of everything drawn until the pop */
void gfx_opacity_pop(void);
void gfx_offset_push(float dx, float dy); /* translates everything drawn until the pop */
void gfx_offset_pop(void);
void gfx_scale_push(float sx, float sy, float ox, float oy); /* scales around (ox,oy), layout units */
void gfx_scale_pop(void);

/* stats for the overlay / logs */
int gfx_draw_calls(void);
int gfx_quads(void);
double gfx_flush_ms(int reset);
void gfx_mem(long *bytes, int *count, long *pooled);
void gfx_pool_clear(void);   /* lets go of the textures kept for reuse */ /* the textures made and alive (pooled ones included), for the memory line */
void gfx_fill_report(char *out, int cap); /* SB_FILL=1: the screen area drawn per frame, by kind (development) */
/* a radial glow (the box gx,gy,gw,gh) seen only inside the rounded box x,y,w,h: glows in a rounded picture keep its corners */
void gfx_glow_round(float gx, float gy, float gw, float gh, uint32_t c, float x, float y, float w, float h, float r);
GLuint gfx_radial_tex(void);     /* a soft disc, alpha only, 128x128: glows and vignettes as one textured quad */   /* milliseconds spent in buffer uploads + draw calls since the last reset */
#endif
