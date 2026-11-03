#include "ui/gfx.h"
#include "ui/atlas.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <SDL2/SDL.h>

/* ---- vertex stream -----------------------------------------------------------------------
   One vertex layout for every shader: position (layout units), uv, colour, the rectangle the
   fragment belongs to (centre + half size, so the SDF knows the corner distance), and the
   corner radius / edge softness / ring thickness. */
typedef struct {
  float x, y, u, v;
  uint32_t color;
  float cx, cy, hw, hh;
  float radius, soft, ring, pad;
} Vtx;
#define MAX_VTX 24576
static Vtx vtx[MAX_VTX];
static unsigned frameCounter;
static void pool_age(void);
static int tgtOn, scrDirty, midSwitch; /* development: offscreen targets begun after the screen was drawn to (each makes the GPU store the screen and load it again) */
static GLuint screenFbo;   /* what "the screen" is: 0, or an offscreen picture when the window is hidden (the Mac preview in the background) */
static int nvtx;

enum { P_SHAPE, P_IMAGE, P_ALPHA, P_PLAIN, P_PLAINIMG, P_BLUR, P_GLARE, P_LIQUID, P_N }; /* plain: no rounded corners, no soft edge: the cheapest fragment work (full-screen layers) */
typedef struct { GLuint prog; GLint aPos, aUv, aCol, aRect, aPar, uScale, uView, uTex; } Prog;
static Prog progs[P_N];
static int curProg = -1;
static GLuint curTex;
static int batchOpaque = 1, blendOn = 1;
static GLuint vbos[3]; static int vboCur; static GLsizeiptr vboOff, vboCap; static double flushMs; static int flushN; /* three streaming buffers in turn: the GPU may still read the last frame's */
static int surfW, surfH;
static float scl = 1.0f;
static int viewX, viewY, viewW, viewH; /* letterboxed UI viewport (GL bottom-left origin): 16:9 layout centred in the window */
static int drawCalls, quadsDrawn;

/* clip, opacity and transform stacks */
typedef struct { float x, y, w, h; } Clip;
static Clip clips[16]; static int nclip; static Clip curClip;
static float opac[16]; static int nopac; static float curOpac = 1.0f;
static float offx[16], offy[16]; static int noff; static float curOx, curOy;
static float scx[16], scy[16], sox[16], soy[16], scOx[16], scOy[16]; static int nsc; static float curSx = 1, curSy = 1, curSox, curSoy;

static const char *VS =
  "attribute vec2 aPos; attribute vec2 aUv; attribute vec4 aCol; attribute vec4 aRect; attribute vec4 aPar;\n"
  "uniform float uScale; uniform vec2 uView;\n"
  "varying vec2 vUv; varying vec4 vCol; varying vec4 vRect; varying vec4 vPar; varying vec2 vPos;\n"
  "void main(){ vUv=aUv; vCol=aCol; vRect=aRect; vPar=aPar; vPos=aPos;\n"
  "  vec2 p = aPos * uScale; gl_Position = vec4(p.x / uView.x * 2.0 - 1.0, 1.0 - p.y / uView.y * 2.0, 0.0, 1.0); }\n";
/* signed distance to the rounded rectangle, in layout units; soft = edge width (1 px of AA, or a blur) */
static const char *FS_COMMON =
  "varying vec2 vUv; varying vec4 vCol; varying vec4 vRect; varying vec4 vPar; varying vec2 vPos;\n"
  "uniform float uScale;\n"
  "float sdf(){ vec2 q = abs(vPos - vRect.xy) - (vRect.zw - vec2(vPar.x)); return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - vPar.x; }\n"
  "float cover(float d, float soft){ return 1.0 - smoothstep(-soft, soft, d); }\n";
static const char *FS_SHAPE =
  "void main(){ float d = sdf(); float s = max(vPar.y, 0.5 / uScale); float a;\n"
  "  if (vPar.z > 0.0) { float dd = abs(d + vPar.z * 0.5) - vPar.z * 0.5; a = cover(dd, s); }\n"
  "  else a = cover(d, s);\n"
  "  gl_FragColor = vec4(vCol.rgb, vCol.a * a); }\n";
static const char *FS_IMAGE =
  "uniform sampler2D uTex;\n"
  "void main(){ float d = sdf(); float s = max(vPar.y, 0.5 / uScale); float a = cover(d, s);\n"
  "  vec4 t = texture2D(uTex, vUv); gl_FragColor = vec4(t.rgb * vCol.rgb, t.a * vCol.a * a); }\n";
static const char *FS_PLAIN = "void main(){ gl_FragColor = vCol; }\n";
static const char *FS_PLAINIMG = "uniform sampler2D uTex;\nvoid main(){ gl_FragColor = texture2D(uTex, vUv) * vCol; }\n";
/* one pass of a 9-tap gaussian (5 fetches between texels, linear filtering does the rest); vPar.xy = the step */
static const char *FS_BLUR =
  "uniform sampler2D uTex;\n"
  "void main(){ vec2 d = vPar.xy; vec4 c = texture2D(uTex, vUv) * 0.2270270270;\n"
  "  c += (texture2D(uTex, vUv + d * 1.3846153846) + texture2D(uTex, vUv - d * 1.3846153846)) * 0.3162162162;\n"
  "  c += (texture2D(uTex, vUv + d * 3.2307692308) + texture2D(uTex, vUv - d * 3.2307692308)) * 0.0702702703;\n"
  "  gl_FragColor = c * vCol; }\n";
/* a glare: a soft slanted band of light across a rounded box (vPar.z = where the band is, 0 at the left edge, 1 at
   the right), inside the box's own soft edge */
/* glass catching the light: a wide soft sweep and a thin bright streak just behind it, both slanted, stronger towards
   the top as on a pane lit from above, a small glint in the top left corner (bright as the sweep passes it, then
   resting faint, drifting a little with it), and a faint rim along the edge (the glass's thickness); pos 0..1 across */
static const char *FS_GLARE =
  "void main(){ float d = sdf(); float a = cover(d, max(vPar.y, 0.5 / uScale));\n"
  "  vec2 rel = (vPos - (vRect.xy - vRect.zw)) / (2.0 * vRect.zw); float t = (rel.x + 0.45 * (1.0 - rel.y)) / 1.45, p = vPar.z;\n"
  "  float wide = 1.0 - smoothstep(0.0, 0.26, abs(t - p)); wide *= wide;\n"
  "  float thin = 1.0 - smoothstep(0.0, 0.035, abs(t - (p - 0.17)));\n"
  "  float top = mix(1.0, 0.35, rel.y), rim = (1.0 - smoothstep(0.0, 3.0 * uScale, -d)) * mix(1.0, 0.35, rel.y);\n"
  "  vec2 cg = (rel - vec2(0.12 + 0.05 * p + vPar.w, 0.09)) * vec2(1.0, vRect.w / vRect.z); cg = vec2(cg.x + cg.y * 0.45, cg.y - cg.x * 0.45);\n"
  "  float glint = exp(-dot(cg * vec2(9.0, 22.0), cg * vec2(9.0, 22.0))) * (0.35 + 0.65 * (1.0 - smoothstep(0.0, 0.3, abs(p - 0.12))));\n"
  "  float b = clamp((0.6 * wide + thin) * top + 1.1 * rim + 0.9 * glint, 0.0, 1.0);\n"
  "  gl_FragColor = vec4(vCol.rgb, vCol.a * a * b); }\n";
/* liquid glass over a blurred picture of what is behind it: towards the edge the picture is drawn in as by a lens (the
   pull grows as the square of the nearness to the edge), red pulled more and blue less (a wide fringe), the bent edge leaning violet,
   the colours made 60% richer and a little lighter; a moving spot of light (vCol.rg where on the box, vCol.b how strong) bends the picture under it as a small lens, with its own fringe. vPar = radius, uv per unit down, pull in units, uv per unit across */
static const char *FS_LIQUID =
  "uniform sampler2D uTex;\n"
  "void main(){ float d = sdf(); float a = cover(d, 0.6 / uScale);\n"
  "  vec2 rel = vPos - vRect.xy; float band = min(vRect.z, vRect.w);\n"
  "  float e = clamp(1.0 + d / (band * 0.95), 0.0, 1.0); e = e * e * (3.0 - 2.0 * e); e *= e;\n" /* (the edge ramp: one clamp, no smoothstep call) */
  "  vec2 n = rel / max(vRect.zw, vec2(1.0)); n *= inversesqrt(dot(n, n) + 0.0001);\n"
  "  vec2 off = n * (vPar.z * e) * vec2(vPar.w, vPar.y);\n"
  "  vec2 sp = (vRect.xy - vRect.zw) + vCol.rg * 2.0 * vRect.zw; vec2 ds = vPos - sp; float sr = max(vRect.w, 1.0) * 1.5;\n"
  "  float q = clamp(1.0 - dot(ds, ds) / (sr * sr) * 0.25, 0.0, 1.0); q *= q; float lens = vCol.b * q * q; off += ds * lens * 0.75 * vec2(vPar.w, vPar.y);\n" /* (the spot: (1 - t/4)^4 for exp(-t), no exp on this GPU) */
  "  vec3 c = vec3(texture2D(uTex, vUv - off * 1.4).r, texture2D(uTex, vUv - off).g, texture2D(uTex, vUv - off * 0.6).b);\n"
  "  float l = dot(c, vec3(0.299, 0.587, 0.114)); c = clamp(mix(vec3(l), c, 1.6 + 0.5 * lens) * (1.08 + 0.15 * lens) + 0.025 + 0.02 * lens, 0.0, 1.0);\n"
  "  c = mix(c, c * vec3(1.03, 0.94, 1.1) + vec3(0.012, 0.0, 0.03), e * 0.45);\n"
  "  gl_FragColor = vec4(c, vCol.a * a); }\n";
static const char *FS_ALPHA =
  "uniform sampler2D uTex;\n"
  "void main(){ float t = texture2D(uTex, vUv).a; gl_FragColor = vec4(vCol.rgb, vCol.a * t); }\n";

static GLuint shader(GLenum type, const char *a, const char *b, const char *c) {
  const char *src[3] = { a, b ? b : "", c ? c : "" };
  GLuint s = glCreateShader(type); glShaderSource(s, 3, src, NULL); glCompileShader(s);
  GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) { char log[1024]; glGetShaderInfoLog(s, sizeof log, NULL, log); fprintf(stderr, "[gfx] shader: %s\n", log); }
  return s;
}
static int program(Prog *p, const char *fs) {
  GLuint v = shader(GL_VERTEX_SHADER, SB_GLSL_PRECISION, VS, NULL), f = shader(GL_FRAGMENT_SHADER, SB_GLSL_PRECISION, FS_COMMON, fs);
  p->prog = glCreateProgram(); glAttachShader(p->prog, v); glAttachShader(p->prog, f); glLinkProgram(p->prog);
  GLint ok = 0; glGetProgramiv(p->prog, GL_LINK_STATUS, &ok);
  if (!ok) { char log[1024]; glGetProgramInfoLog(p->prog, sizeof log, NULL, log); fprintf(stderr, "[gfx] link: %s\n", log); return 0; }
  p->aPos = glGetAttribLocation(p->prog, "aPos"); p->aUv = glGetAttribLocation(p->prog, "aUv"); p->aCol = glGetAttribLocation(p->prog, "aCol");
  p->aRect = glGetAttribLocation(p->prog, "aRect"); p->aPar = glGetAttribLocation(p->prog, "aPar");
  p->uScale = glGetUniformLocation(p->prog, "uScale"); p->uView = glGetUniformLocation(p->prog, "uView"); p->uTex = glGetUniformLocation(p->prog, "uTex");
  return 1;
}

int gfx_init(int w, int h) {
  if (!program(&progs[P_SHAPE], FS_SHAPE) || !program(&progs[P_IMAGE], FS_IMAGE) || !program(&progs[P_ALPHA], FS_ALPHA) || !program(&progs[P_PLAIN], FS_PLAIN) || !program(&progs[P_PLAINIMG], FS_PLAINIMG) || !program(&progs[P_BLUR], FS_BLUR) || !program(&progs[P_GLARE], FS_GLARE) || !program(&progs[P_LIQUID], FS_LIQUID)) return 0;
  glGenBuffers(3, vbos); vboCap = (GLsizeiptr)sizeof(Vtx) * MAX_VTX; for (int i = 0; i < 3; i++) { glBindBuffer(GL_ARRAY_BUFFER, vbos[i]); glBufferData(GL_ARRAY_BUFFER, vboCap, NULL, GL_STREAM_DRAW); }
  glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glEnable(GL_BLEND);
  glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  gfx_resize(w, h);
  fprintf(stderr, "[gfx] %s | %s | %s\n", glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION));
  return 1;
}
void gfx_resize(int w, int h) {
  surfW = w; surfH = h;
  float s = fminf((float)w / SB_W, (float)h / SB_H); /* uniform scale: the whole 16:9 layout stays visible at any window shape */
  if (s <= 0) s = 1.0f;
  scl = s;
  viewW = (int)(SB_W * s + 0.5f); viewH = (int)(SB_H * s + 0.5f);
  viewX = (w - viewW) / 2; viewY = (h - viewH) / 2;
  glViewport(viewX, viewY, viewW, viewH);
}
void gfx_view_rect(int *x, int *y, int *w, int *h) { if (x) *x = viewX; if (y) *y = viewY; if (w) *w = viewW; if (h) *h = viewH; }
void gfx_surface_to_layout(float *x, float *y) {
  float oyTop = (float)(surfH - viewY - viewH); /* == viewY when centred, kept explicit for clarity */
  *x = (*x - (float)viewX) / scl; *y = (*y - oyTop) / scl;
}
void gfx_layout_to_surface(float x, float y, float w, float h, int *px, int *py, int *pw, int *ph) {
  float oyTop = (float)(surfH - viewY - viewH);
  if (px) *px = (int)(viewX + x * scl + 0.5f); if (py) *py = (int)(oyTop + y * scl + 0.5f);
  if (pw) *pw = (int)(w * scl + 0.5f); if (ph) *ph = (int)(h * scl + 0.5f);
}
float gfx_scale(void) { return scl; }
int gfx_draw_calls(void) { return drawCalls; }
static long poolBytes, texBytes; static int npool, texCount;
void gfx_mem(long *bytes, int *count, long *pooled) { if (bytes) *bytes = texBytes; if (count) *count = texCount; if (pooled) *pooled = poolBytes; }
/* SB_FILL: how much of the screen each kind of quad covers (the GPU's fill; offscreen pictures counted apart) */
int gfx_in_target(void);
static int fillOn = -1; static double fillA[P_N + 1], fillFrames;
static void fill_add(int prog, float x0, float y0, float x1, float y1) {
  if (nclip) { if (x0 < curClip.x) x0 = curClip.x; if (y0 < curClip.y) y0 = curClip.y; if (x1 > curClip.x + curClip.w) x1 = curClip.x + curClip.w; if (y1 > curClip.y + curClip.h) y1 = curClip.y + curClip.h; }
  if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > SB_W) x1 = SB_W; if (y1 > SB_H) y1 = SB_H; if (x1 <= x0 || y1 <= y0) return;
  fillA[gfx_in_target() ? P_N : prog] += (double)(x1 - x0) * (y1 - y0) / ((double)SB_W * SB_H);
  { static int big = -1; if (big < 0) big = getenv("SB_FILL_BIG") ? atoi(getenv("SB_FILL_BIG")) : 0; extern unsigned app_frame(void); /* development: SB_FILL_BIG=<frame> lists that frame's big quads */
    if (big > 0 && (int)app_frame() == big && !gfx_in_target()) { double ar = (double)(x1 - x0) * (y1 - y0) / ((double)SB_W * SB_H); if (ar > 0.05) fprintf(stderr, "[fillbig] prog %d %.0f,%.0f %.0fx%.0f %.2f screens\n", prog, x0, y0, x1 - x0, y1 - y0, ar); } } }
void gfx_fill_report(char *out, int cap) { /* screens' worth per frame: shape, image, alpha, plain, plainimg, blur, glare, liquid | offscreen */
  double n = fillFrames > 0 ? fillFrames : 1, t = 0; for (int i = 0; i < P_N; i++) t += fillA[i];
  snprintf(out, cap, "fill %.2f screens/frame: shape %.2f image %.2f alpha %.2f plain %.2f plainimg %.2f blur %.2f glare %.2f liquid %.3f | offscreen %.2f", t / n, fillA[P_SHAPE] / n, fillA[P_IMAGE] / n, fillA[P_ALPHA] / n, fillA[P_PLAIN] / n, fillA[P_PLAINIMG] / n, fillA[P_BLUR] / n, fillA[P_GLARE] / n, fillA[P_LIQUID] / n, fillA[P_N] / n);
  memset(fillA, 0, sizeof fillA); fillFrames = 0; }
int gfx_quads(void) { return quadsDrawn; }

static void use(int p) {
  Prog *P = &progs[p]; glUseProgram(P->prog);
  glUniform1f(P->uScale, scl); glUniform2f(P->uView, (float)viewW, (float)viewH);
  if (P->uTex >= 0) glUniform1i(P->uTex, 0);
  glEnableVertexAttribArray(P->aPos); glVertexAttribPointer(P->aPos, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)0);
  glEnableVertexAttribArray(P->aUv); glVertexAttribPointer(P->aUv, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)8);
  glEnableVertexAttribArray(P->aCol); glVertexAttribPointer(P->aCol, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vtx), (void *)16);
  glEnableVertexAttribArray(P->aRect); glVertexAttribPointer(P->aRect, 4, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)20);
  glEnableVertexAttribArray(P->aPar); glVertexAttribPointer(P->aPar, 4, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *)36);
}
void gfx_flush(void) {
  if (!nvtx) return;
  if (curProg < 0) return;
  use(curProg);
  if (curProg != P_SHAPE && curProg != P_PLAIN) { glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, curTex); }
  int wantBlend = !(curProg == P_PLAIN && batchOpaque); if (wantBlend != blendOn) { blendOn = wantBlend; if (blendOn) glEnable(GL_BLEND); else glDisable(GL_BLEND); } /* opaque fills skip the read-modify-write */
  static int logFrame0 = -1; if (logFrame0 < 0) logFrame0 = getenv("SB_FLUSHLOG") ? atoi(getenv("SB_FLUSHLOG")) : 0; if (logFrame0 && (int)frameCounter == logFrame0) glFinish();
  Uint32 t0 = SDL_GetTicks(); GLsizeiptr bytes = nvtx * (GLsizeiptr)sizeof(Vtx);
  static int clientVtx = -1; if (clientVtx < 0) {
#ifdef SB_WEBOS
    clientVtx = getenv("SB_VBO") == NULL; /* the driver maps new memory for every buffer update: client arrays are three times cheaper per draw */
#else
    clientVtx = getenv("SB_CLIENTVTX") != NULL;
#endif
  }
  if (clientVtx) { glBindBuffer(GL_ARRAY_BUFFER, 0); Prog *P = &progs[curProg]; glVertexAttribPointer(P->aPos, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), &vtx[0].x); glVertexAttribPointer(P->aUv, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), &vtx[0].u); glVertexAttribPointer(P->aCol, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vtx), &vtx[0].color); glVertexAttribPointer(P->aRect, 4, GL_FLOAT, GL_FALSE, sizeof(Vtx), &vtx[0].cx); glVertexAttribPointer(P->aPar, 4, GL_FLOAT, GL_FALSE, sizeof(Vtx), &vtx[0].radius); glDrawArrays(GL_TRIANGLES, 0, nvtx); }
  else {
  if (vboOff + bytes > vboCap) { glBufferData(GL_ARRAY_BUFFER, vboCap, NULL, GL_STREAM_DRAW); vboOff = 0; } /* full: orphan and start again */
  glBufferSubData(GL_ARRAY_BUFFER, vboOff, bytes, vtx);
  glDrawArrays(GL_TRIANGLES, (GLint)(vboOff / (GLsizeiptr)sizeof(Vtx)), nvtx); vboOff += bytes; }
  static int logFrame = -1; if (logFrame < 0) logFrame = getenv("SB_FLUSHLOG") ? atoi(getenv("SB_FLUSHLOG")) : 0;
  if (logFrame && (int)frameCounter == logFrame) { glFinish(); double area = 0; for (int i = 0; i + 5 < nvtx; i += 6) { float x0 = vtx[i].x, x1 = x0, y0 = vtx[i].y, y1 = y0; for (int j = 1; j < 6; j++) { if (vtx[i+j].x < x0) x0 = vtx[i+j].x; if (vtx[i+j].x > x1) x1 = vtx[i+j].x; if (vtx[i+j].y < y0) y0 = vtx[i+j].y; if (vtx[i+j].y > y1) y1 = vtx[i+j].y; } area += (x1 - x0) * (y1 - y0); }
    fprintf(stderr, "[flush] prog %d tex %u quads %d area %.0fk blend %d %u ms\n", curProg, (unsigned)curTex, nvtx / 6, area / 1000, blendOn, (unsigned)(SDL_GetTicks() - t0)); }
  if (!tgtOn) scrDirty = 1;
  flushMs += SDL_GetTicks() - t0; flushN++;
  drawCalls++; quadsDrawn += nvtx / 6; nvtx = 0;
}
static void apply_clip(void) {
  gfx_flush();
  if (nclip) { int x = viewX + (int)floorf(curClip.x * scl), y = viewY + (int)floorf((SB_H - curClip.y - curClip.h) * scl), w = (int)ceilf((curClip.x + curClip.w) * scl) - (x - viewX), h = (int)ceilf((SB_H - curClip.y) * scl) - (y - viewY);
    if (w < 0) w = 0; if (h < 0) h = 0;
    glEnable(GL_SCISSOR_TEST); glScissor(x, y, w, h); }
  else glDisable(GL_SCISSOR_TEST);
}
void gfx_invalidate_state(void) { curProg = -1; curTex = 0; blendOn = -1; glDisable(GL_SCISSOR_TEST); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glActiveTexture(GL_TEXTURE0); } /* someone else drew with raw GL: the next batch sets everything again */
double gfx_flush_ms(int reset) { double v = flushN ? flushMs : 0; if (reset) { flushMs = 0; flushN = 0; } return v; }
void gfx_set_screen_fbo(GLuint fbo) { screenFbo = fbo; }
void gfx_begin(uint32_t c) {
  glBindFramebuffer(GL_FRAMEBUFFER, screenFbo);
  /* letterbox bars: full-window black first, then the UI viewport clears to its own colour (transparent when video shows through) */
  glViewport(0, 0, surfW, surfH);
  glDisable(GL_SCISSOR_TEST);
  glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
  glViewport(viewX, viewY, viewW, viewH);
  scrDirty = 0; frameCounter++; pool_age(); vboCur = (vboCur + 1) % 3; glBindBuffer(GL_ARRAY_BUFFER, vbos[vboCur]); vboOff = 0; /* this frame's buffer: the GPU finished with it two frames ago */
  nvtx = 0; curProg = -1; curTex = 0; blendOn = -1; glEnable(GL_BLEND);
  glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  nclip = 0; nopac = 0; curOpac = 1; noff = 0; curOx = curOy = 0; nsc = 0; curSx = curSy = 1; drawCalls = quadsDrawn = 0; fillFrames++;
  glDisable(GL_SCISSOR_TEST);
  static int noClear = -1; if (noClear < 0) noClear = getenv("SB_NOCLEAR") != NULL;
  glClearColor(((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f, ((c >> 24) & 255) / 255.0f);
  if (!noClear) glClear(GL_COLOR_BUFFER_BIT);
}
void gfx_end(void) { gfx_flush(); glDisable(GL_SCISSOR_TEST); }

/* ---- quads ------------------------------------------------------------------------------- */
static uint32_t to_abgr(uint32_t c) { /* 0xAARRGGBB -> bytes R,G,B,A in memory (little endian) */
  float a = ((c >> 24) & 255) * curOpac;
  return ((uint32_t)(a + 0.5f) << 24) | ((c & 255) << 16) | (c & 0xff00) | ((c >> 16) & 255);
}
static int mirN; static float mirM; /* mirrored: where things go is flipped about a vertical line, the things themselves (pictures, words) are not */
static void xform(float *x, float *y) { *x = (*x - curSox) * curSx + curSox + curOx; *y = (*y - curSoy) * curSy + curSoy + curOy; }
void gfx_mirror_push(float cx) { float y = 0; xform(&cx, &y); mirM = cx; mirN = 1; } /* (the line taken where it is now: transforms pushed later move with the picture) */
void gfx_mirror_pop(void) { mirN = 0; }
int gfx_mirrored(void) { return mirN; }
static void mir(float *x0, float *x1) { if (mirN) { float a = 2 * mirM - *x1, b = 2 * mirM - *x0; *x0 = a; *x1 = b; } }
static int sdfFull; static float sdfR[4]; static void mir(float *x0, float *x1);
static float quadPad; /* the next quad's fourth parameter (vPar.w), then 0 again */
static void quad(int prog, GLuint tex, float x, float y, float w, float h, float radius, float soft, float ring, float grow,
                 uint32_t c00, uint32_t c10, uint32_t c01, uint32_t c11, float u0, float v0, float u1, float v1) {
  if (prog != curProg || (prog != P_SHAPE && prog != P_PLAIN && tex != curTex) || nvtx + 6 > MAX_VTX) { gfx_flush(); curProg = prog; curTex = tex; batchOpaque = 1; }
  if ((c00 >> 24) != 255 || (c10 >> 24) != 255 || (c01 >> 24) != 255 || (c11 >> 24) != 255 || curOpac < 0.999f) batchOpaque = 0;
  /* the rectangle in the current transform */
  float x0 = x, y0 = y, x1 = x + w, y1 = y + h; xform(&x0, &y0); xform(&x1, &y1);
  if (mirN) { mir(&x0, &x1); uint32_t t = c00; c00 = c10; c10 = t; t = c01; c01 = c11; c11 = t; } /* (a sideways gradient turns with it) */
  if (fillOn) { if (fillOn < 0) fillOn = getenv("SB_FILL") != NULL; if (fillOn) { fill_add(prog, x0 - grow, y0 - grow, x1 + grow, y1 + grow); } }
  float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f, hw = (x1 - x0) * 0.5f, hh = (y1 - y0) * 0.5f;
  if (sdfFull) { float a0 = sdfR[0], b0 = sdfR[1], a1 = sdfR[0] + sdfR[2], b1 = sdfR[1] + sdfR[3]; xform(&a0, &b0); xform(&a1, &b1); mir(&a0, &a1); /* a piece of a bigger rounded box: its shape is the whole box's */
    cx = (a0 + a1) * 0.5f; cy = (b0 + b1) * 0.5f; hw = (a1 - a0) * 0.5f; hh = (b1 - b0) * 0.5f; }
  radius *= curSx; if (radius > hw) radius = hw; if (radius > hh) radius = hh;
  /* the quad is grown by `grow` (soft edges / outside rings) while the SDF rect stays the real one */
  float gx0 = x0 - grow, gy0 = y0 - grow, gx1 = x1 + grow, gy1 = y1 + grow;
  Vtx *q = &vtx[nvtx]; nvtx += 6;
  uint32_t k00 = to_abgr(c00), k10 = to_abgr(c10), k01 = to_abgr(c01), k11 = to_abgr(c11);
  float pd = quadPad; quadPad = 0;
  Vtx a = { gx0, gy0, u0, v0, k00, cx, cy, hw, hh, radius, soft, ring, pd };
  Vtx b = { gx1, gy0, u1, v0, k10, cx, cy, hw, hh, radius, soft, ring, pd };
  Vtx d = { gx0, gy1, u0, v1, k01, cx, cy, hw, hh, radius, soft, ring, pd };
  Vtx e = { gx1, gy1, u1, v1, k11, cx, cy, hw, hh, radius, soft, ring, pd };
  q[0] = a; q[1] = b; q[2] = d; q[3] = b; q[4] = e; q[5] = d;
}
/* an alpha picture turned about its centre (a turning icon: one picture, turned by the GPU) */
static int invisible(uint32_t c);
void gfx_alpha_tex_rot(GLuint tex, float x, float y, float w, float h, float deg, uint32_t c, float u0, float v0, float u1, float v1) {
  if (!tex || invisible(c)) return;
  if (P_ALPHA != curProg || tex != curTex || nvtx + 6 > MAX_VTX) { gfx_flush(); curProg = P_ALPHA; curTex = tex; batchOpaque = 1; }
  batchOpaque = 0;
  float x0 = x, y0 = y, x1 = x + w, y1 = y + h; xform(&x0, &y0); xform(&x1, &y1); if (mirN) { mir(&x0, &x1); deg = -deg; }
  float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f, hw = (x1 - x0) * 0.5f, hh = (y1 - y0) * 0.5f, cs = cosf(deg * 3.14159265f / 180), sn = sinf(deg * 3.14159265f / 180);
  uint32_t k = to_abgr(c); Vtx *q = &vtx[nvtx]; nvtx += 6;
  float px[4] = { -hw, hw, -hw, hw }, py[4] = { -hh, -hh, hh, hh }, uu[4] = { u0, u1, u0, u1 }, vv[4] = { v0, v0, v1, v1 }; Vtx v4[4];
  for (int i = 0; i < 4; i++) { Vtx t = { cx + px[i] * cs - py[i] * sn, cy + px[i] * sn + py[i] * cs, uu[i], vv[i], k, cx, cy, hw, hh, 0, 0, 0, 0 }; v4[i] = t; }
  q[0] = v4[0]; q[1] = v4[1]; q[2] = v4[2]; q[3] = v4[1]; q[4] = v4[3]; q[5] = v4[2];
}
static int invisible(uint32_t c) { return ((c >> 24) & 255) == 0 || curOpac <= 0.002f; }
static int clipped_out(float x, float y, float w, float h) {
  if (!nclip) return 0;
  float x0 = x, y0 = y, x1 = x + w, y1 = y + h; xform(&x0, &y0); xform(&x1, &y1);
  return x1 < curClip.x || y1 < curClip.y || x0 > curClip.x + curClip.w || y0 > curClip.y + curClip.h;
}

/* ---- rounded shapes from corner pieces -------------------------------------------------------
   A shape is nine pieces in the shared alpha pages: four corners rasterised once per size (the
   same signed distance and edge as the shader version), four edges that stretch the corners'
   straight end, and the middle. They draw with the plain alpha program from the page the text is
   on, so shapes and text of a screen go out in one batch. The shader version stays as the fallback. */
enum { CK_FILL, CK_RING, CK_SHADOW };
typedef struct { int kind, r, t, b, want; AtlasSlot slot; int n, e; unsigned use; } Corner;
#define NCORNERS 256
static Corner corners[NCORNERS]; static int ncorners, stickyPage; static unsigned cornerTick;
static float smooth01(float e0, float e1, float x) { float t = (x - e0) / (e1 - e0); if (t < 0) t = 0; if (t > 1) t = 1; return t * t * (3 - 2 * t); }
static const Corner *corner_get(int kind, float rL, float tL, float bL) {
  int R = (int)(rL * scl + 0.5f), T4 = (int)(tL * scl * 4 + 0.5f), B4 = (int)(bL * scl * 4 + 0.5f);
  int page = (curProg == P_ALPHA && nvtx) ? atlas_page_of(curTex) : -1; if (page < 0) page = stickyPage;
  cornerTick++;
  Corner *c = NULL;
  for (int i = 0; i < ncorners; i++) { Corner *k = &corners[i];
    if (k->kind == kind && k->r == R && k->t == T4 && k->b == B4 && (k->want == page || k->slot.page == page) && atlas_valid(&k->slot)) { c = k; break; } }
  if (c) { c->use = cornerTick; atlas_touch(&c->slot); return c; }
  /* rasterise: the bitmap starts e px outside the shape's corner (the soft edge has room), the arc centre is at e + R */
  float T = T4 / 4.0f, Bpx = B4 / 4.0f, soft = kind == CK_SHADOW ? (Bpx > 0.5f ? Bpx : 0.5f) : 0.5f;
  int e = kind == CK_SHADOW ? (int)ceilf(Bpx + scl) : 1;
  int inner = R; if (kind == CK_RING && (int)ceilf(T) > inner) inner = (int)ceilf(T); if ((int)ceilf(soft) > inner) inner = (int)ceilf(soft);
  int n = e + inner + 1; if (n > 400) return NULL;
  int N = n + 1;                                   /* one more row and column repeat the straight end: sampling at the seam sees the same value on both sides */
  unsigned char *bmp = malloc((size_t)N * N);
  for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) {
    if (x == n || y == n) continue;
    float qx = (e + R) - (x + 0.5f), qy = (e + R) - (y + 0.5f), mx = qx > 0 ? qx : 0, my = qy > 0 ? qy : 0;
    float d = sqrtf(mx * mx + my * my) + fminf(fmaxf(qx, qy), 0) - R;
    if (kind == CK_RING) d = fabsf(d + T * 0.5f) - T * 0.5f;
    float a = 1 - smooth01(-soft, soft, d);
    bmp[y * N + x] = (unsigned char)(a * 255 + 0.5f); }
  for (int y = 0; y < n; y++) bmp[y * N + n] = bmp[y * N + n - 1];
  for (int x = 0; x < N; x++) bmp[n * N + x] = bmp[(n - 1) * N + x];
  if (ncorners < NCORNERS) c = &corners[ncorners++];
  else { c = &corners[0]; for (int i = 1; i < NCORNERS; i++) if (corners[i].use < c->use) c = &corners[i]; }
  memset(c, 0, sizeof *c);
  int ok = atlas_put(bmp, N, N, page, 0, &c->slot); free(bmp);
  c->slot.w = n; c->slot.h = n;                    /* the repeated row and column are only there for the filtering */
  if (!ok) { c->slot.page = -1; return NULL; }
  c->kind = kind; c->r = R; c->t = T4; c->b = B4; c->want = page; c->n = n; c->e = e; c->use = cornerTick;
  return c;
}
static uint32_t bilerp(uint32_t c00, uint32_t c10, uint32_t c01, uint32_t c11, float fx, float fy) {
  if (c00 == c10 && c00 == c01 && c00 == c11) return c00;
  return col_mix(col_mix(c00, c10, fx), col_mix(c01, c11, fx), fy);
}
/* the nine pieces over the rect grown by the corner's margin; colours are spread over the grown rect like the shader version's vertices */
static float holeX, holeY, holeW, holeH; static int holeOn; /* an opaque box over a shadow: its middle piece under it is not drawn */
static int nine(int kind, float x, float y, float w, float h, float r, float t, float blur, uint32_t c00, uint32_t c10, uint32_t c01, uint32_t c11) {
  static int off = -1; if (off < 0) off = getenv("SB_SDFSHAPES") != NULL; if (off) return 0;
  float hw = w * 0.5f, hh = h * 0.5f; if (r > hw) r = hw; if (r > hh) r = hh; if (r < 0) r = 0;
  const Corner *k = corner_get(kind, r, t, blur); if (!k) return 0;
  GLuint tex = atlas_tex(k->slot.page); stickyPage = k->slot.page;
  float m = k->e / scl, cs = k->n / scl;
  float gx = x - m, gy = y - m, gw = w + 2 * m, gh = h + 2 * m;
  float csx = cs < gw / 2 ? cs : gw / 2, csy = cs < gh / 2 ? cs : gh / 2;
  float su0, sv0, su1, sv1; atlas_uv(&k->slot, &su0, &sv0, &su1, &sv1);
  float du = (su1 - su0), dv = (sv1 - sv0);
  float uIn = su0 + du * (csx / cs), vIn = sv0 + dv * (csy / cs);        /* how far into the corner piece the shown part reaches */
  float uEdge = su1 - 0.5f / ATLAS_W, vEdge = sv1 - 0.5f / ATLAS_H;        /* the straight end of the piece */
  float xs[4] = { gx, gx + csx, gx + gw - csx, gx + gw }, ys[4] = { gy, gy + csy, gy + gh - csy, gy + gh };
  float us[3][2] = { { su0, uIn }, { uEdge, uEdge }, { uIn, su0 } }, vs[3][2] = { { sv0, vIn }, { vEdge, vEdge }, { vIn, sv0 } };
  for (int j = 0; j < 3; j++) for (int i = 0; i < 3; i++) {
    float qw = xs[i + 1] - xs[i], qh = ys[j + 1] - ys[j]; if (qw <= 0.0001f || qh <= 0.0001f) continue;
    if (i == 1 && j == 1 && kind == CK_RING) continue;                     /* a ring has no middle */
    if (i == 1 && j == 1 && holeOn && xs[1] >= holeX && ys[1] >= holeY && xs[2] <= holeX + holeW && ys[2] <= holeY + holeH) continue; /* hidden under the box it shades */
    float fx0 = (xs[i] - gx) / gw, fx1 = (xs[i + 1] - gx) / gw, fy0 = (ys[j] - gy) / gh, fy1 = (ys[j + 1] - gy) / gh;
    quad(P_ALPHA, tex, xs[i], ys[j], qw, qh, 0, 0, 0, 0,
         bilerp(c00, c10, c01, c11, fx0, fy0), bilerp(c00, c10, c01, c11, fx1, fy0), bilerp(c00, c10, c01, c11, fx0, fy1), bilerp(c00, c10, c01, c11, fx1, fy1),
         us[i][0], vs[j][0], us[i][1], vs[j][1]);
  }
  return 1;
}
/* square fills: small ones join the alpha batch (one texel that is fully on), big ones keep the cheapest program */
static void plain(float x, float y, float w, float h, uint32_t c00, uint32_t c10, uint32_t c01, uint32_t c11) {
  if (w * h < 250000.0f && atlas_pages()) { int page = (curProg == P_ALPHA && nvtx) ? atlas_page_of(curTex) : -1; if (page < 0) page = stickyPage;
    float u, v; atlas_solid_uv(page, &u, &v); quad(P_ALPHA, atlas_tex(page), x, y, w, h, 0, 0, 0, 0, c00, c10, c01, c11, u, v, u, v); return; }
  quad(P_PLAIN, 0, x, y, w, h, 0, 0, 0, 0, c00, c10, c01, c11, 0, 0, 1, 1);
}
void gfx_rect(float x, float y, float w, float h, uint32_t c) { if (invisible(c) || clipped_out(x, y, w, h)) return; plain(x, y, w, h, c, c, c, c); }
void gfx_rrect(float x, float y, float w, float h, float r, uint32_t c) { if (invisible(c) || clipped_out(x, y, w, h)) return;
  if (r <= 0) { plain(x, y, w, h, c, c, c, c); return; }
  if (!nine(CK_FILL, x, y, w, h, r, 0, 0, c, c, c, c)) quad(P_SHAPE, 0, x, y, w, h, r, 0, 0, 1, c, c, c, c, 0, 0, 1, 1); }
void gfx_rrect_v(float x, float y, float w, float h, float r, uint32_t t, uint32_t b) { if ((invisible(t) && invisible(b)) || clipped_out(x, y, w, h)) return;
  if (r <= 0) { plain(x, y, w, h, t, t, b, b); return; }
  if (!nine(CK_FILL, x, y, w, h, r, 0, 0, t, t, b, b)) quad(P_SHAPE, 0, x, y, w, h, r, 0, 0, 1, t, t, b, b, 0, 0, 1, 1); }
void gfx_rrect_h(float x, float y, float w, float h, float r, uint32_t l, uint32_t rt) { if ((invisible(l) && invisible(rt)) || clipped_out(x, y, w, h)) return;
  if (r <= 0) { plain(x, y, w, h, l, rt, l, rt); return; }
  if (!nine(CK_FILL, x, y, w, h, r, 0, 0, l, rt, l, rt)) quad(P_SHAPE, 0, x, y, w, h, r, 0, 0, 1, l, rt, l, rt, 0, 0, 1, 1); }
void gfx_ring(float x, float y, float w, float h, float r, float t, uint32_t c) { if (invisible(c) || clipped_out(x, y, w, h)) return;
  if (!nine(CK_RING, x, y, w, h, r, t, 0, c, c, c, c)) quad(P_SHAPE, 0, x, y, w, h, r, 0, t, 1, c, c, c, c, 0, 0, 1, 1); }
void gfx_glare_at(float x, float y, float w, float h, float r, float pos, uint32_t c, float shift) { quadPad = shift; gfx_glare(x, y, w, h, r, pos, c); quadPad = 0; } /* its glint moved along (0 the corner, 1 the far end) */
void gfx_glare(float x, float y, float w, float h, float r, float pos, uint32_t c) { if (invisible(c) || clipped_out(x, y, w, h)) return; batchOpaque = 0; quad(P_GLARE, 0, x, y, w, h, r, 0, pos, 1, c, c, c, c, 0, 0, 1, 1); }
void gfx_ring_out(float x, float y, float w, float h, float r, float t, uint32_t c) { gfx_ring(x - t, y - t, w + 2 * t, h + 2 * t, r + t, t, c); }
float gfx_opacity_now(void) { return curOpac; }
void gfx_shadow(float x, float y, float w, float h, float r, float blur, uint32_t c) { if (invisible(c) || clipped_out(x - blur, y - blur, w + 2 * blur, h + 2 * blur)) return;
  if (!nine(CK_SHADOW, x, y, w, h, r, 0, blur, c, c, c, c)) quad(P_SHAPE, 0, x, y, w, h, r, blur, 0, blur + 1, c, c, c, c, 0, 0, 1, 1); }
/* the shadow of an opaque box (bx, by, bw, bh, corner radius br) drawn right over it: the part of the shadow the box
   hides is left out, only while nothing fades (a box seen through would show it) */
void gfx_shadow_under(float x, float y, float w, float h, float r, float blur, uint32_t c, float bx, float by, float bw, float bh, float br) {
  if (curOpac >= 0.999f) { holeOn = 1; holeX = bx + br; holeY = by + br; holeW = bw - 2 * br; holeH = bh - 2 * br; }
  gfx_shadow(x, y, w, h, r, blur, c); holeOn = 0; }
void gfx_circle(float cx, float cy, float r, uint32_t c) { gfx_rrect(cx - r, cy - r, 2 * r, 2 * r, r, c); }

/* pictures' textures kept a moment after they are let go, for the next picture of the same size: making one costs
   this TV's driver 10-17 ms for a backdrop (its memory mapped anew), filling a kept one only the copy. One is taken
   again only 3 frames after it was last drawn (the GPU has finished with it by then: no copy, no wait) */
/* every texture made here, with its size: the memory line in the log sums them, and a texture let go is pooled by
   its real size, never by what a caller believes it is */
#define TREG 8192
static struct { GLuint t; int w, h, ch; } treg[TREG];
static void treg_add(GLuint t, int w, int h, int ch) { if (!t) return; unsigned i = t % TREG; for (int k = 0; k < TREG; k++, i = (i + 1) % TREG) if (!treg[i].t || treg[i].t == t) {
    if (treg[i].t == t) { texBytes -= (long)treg[i].w * treg[i].h * treg[i].ch; texCount--; }
    treg[i].t = t; treg[i].w = w; treg[i].h = h; treg[i].ch = ch; texBytes += (long)w * h * ch; texCount++; return; } }
static int treg_find(GLuint t) { unsigned i = t % TREG; for (int k = 0; k < TREG; k++, i = (i + 1) % TREG) { if (treg[i].t == t) return (int)i; if (!treg[i].t) return -1; } return -1; }
static void treg_del(GLuint t) { int i = treg_find(t); if (i < 0) return; texBytes -= (long)treg[i].w * treg[i].h * treg[i].ch; texCount--; treg[i].t = 0;
  /* (open addressing: the run after it is put back so later lookups still find their entries) */
  for (unsigned j = ((unsigned)i + 1) % TREG; treg[j].t; j = (j + 1) % TREG) { GLuint u = treg[j].t; int w = treg[j].w, h = treg[j].h, ch = treg[j].ch; treg[j].t = 0; texBytes -= (long)w * h * ch; texCount--; treg_add(u, w, h, ch); } }
static void gl_del(GLuint t) { treg_del(t); glDeleteTextures(1, &t); }
#define POOL_N 8
#define POOL_BYTES (12L * 1024 * 1024)
static struct { GLuint t; int w, h, ch, lin; unsigned at; } pool[POOL_N];
static void pool_drop(int i) { gl_del(pool[i].t); poolBytes -= (long)pool[i].w * pool[i].h * pool[i].ch; pool[i] = pool[--npool]; }
static GLuint pool_take(int w, int h, int ch, int lin) {
  for (int i = 0; i < npool; i++) if (pool[i].w == w && pool[i].h == h && pool[i].ch == ch && pool[i].lin == lin && frameCounter - pool[i].at >= 3) {
    GLuint t = pool[i].t; poolBytes -= (long)w * h * ch; pool[i] = pool[--npool]; return t; }
  return 0;
}
void gfx_tex_recycle(GLuint t, int w, int h, int ch, int linear) {
  if (!t) return; if (curTex == t) gfx_flush(); extern void gfx_tex_fade_forget(GLuint); gfx_tex_fade_forget(t);
  { int r = treg_find(t); if (r < 0) { glDeleteTextures(1, &t); return; } w = treg[r].w; h = treg[r].h; ch = treg[r].ch; } /* (its real size) */
  for (int i = 0; i < npool; i++) if (pool[i].t == t) return; /* (already kept) */
  long b = (long)w * h * ch; if (b < 64 * 1024 || b > POOL_BYTES / 2) { gl_del(t); return; } /* (small ones are cheap to make) */
  while (npool && (npool == POOL_N || poolBytes + b > POOL_BYTES)) { int o = 0; for (int i = 1; i < npool; i++) if (pool[i].at < pool[o].at) o = i; pool_drop(o); }
  pool[npool].t = t; pool[npool].w = w; pool[npool].h = h; pool[npool].ch = ch; pool[npool].lin = linear; pool[npool].at = frameCounter; npool++; poolBytes += b;
}
void gfx_pool_clear(void) { while (npool) pool_drop(npool - 1); } /* (the memory guard) */
static void pool_age(void) { for (int i = 0; i < npool; i++) if (frameCounter - pool[i].at > 600) { pool_drop(i); i--; } } /* none kept past 10 s */
GLuint gfx_tex_create(const unsigned char *px, int w, int h, int ch, int linear) {
  GLenum f = ch == 1 ? GL_ALPHA : ch == 3 ? GL_RGB : GL_RGBA;
  { GLuint t = pool_take(w, h, ch, linear); if (t) { glBindTexture(GL_TEXTURE_2D, t); if (curTex == t) curTex = 0;
      if (px) { glPixelStorei(GL_UNPACK_ALIGNMENT, 1); glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, f, GL_UNSIGNED_BYTE, px); } return t; } }
  GLuint t; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, f, w, h, 0, f, GL_UNSIGNED_BYTE, px); treg_add(t, w, h, ch);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  if (curTex == t) curTex = 0; /* a freed id reused: the batch must rebind */
  return t;
}
#define FADE_N 4096
#define FADE_MS 200.0f
static struct { GLuint tex; Uint32 t0; } fades[FADE_N]; static int fadeOff; /* fadeOff: drawing into an offscreen picture (a bake must not keep a half-faded image) */
void gfx_tex_fade_in(GLuint t) { if (!t) return; fades[t % FADE_N].tex = t; fades[t % FADE_N].t0 = SDL_GetTicks(); }
float gfx_tex_fade(GLuint t) {
  if (!t || fadeOff) return 1; unsigned i = t % FADE_N; if (fades[i].tex != t) return 1;
  float x = (SDL_GetTicks() - fades[i].t0) / FADE_MS; if (x >= 1) { fades[i].tex = 0; return 1; }
  return 1 - (1 - x) * (1 - x); /* ease out */
}
void gfx_tex_free(GLuint t) { if (!t) return; if (curTex == t) gfx_flush(); if (fades[t % FADE_N].tex == t) fades[t % FADE_N].tex = 0; for (int i = 0; i < npool; i++) if (pool[i].t == t) { poolBytes -= (long)pool[i].w * pool[i].h * pool[i].ch; pool[i] = pool[--npool]; break; } gl_del(t); }
void gfx_tex_fade_forget(GLuint t) { if (fades[t % FADE_N].tex == t) fades[t % FADE_N].tex = 0; }
static float spotU, spotV, spotK; /* the next liquid quad's moving spot (then none again) */
void gfx_liquid_spot(float u, float v, float k) { spotU = u; spotV = v; spotK = k; }
void gfx_liquid(GLuint tex, float x, float y, float w, float h, float r, float pull, float alpha, float u0, float v0, float u1, float v1) {
  { static int no = -1; if (no < 0) no = getenv("SB_NOLIQ") != NULL; if (no) { spotK = 0; return; } } /* (development: the glass without its bend, to measure what it costs) */
  if (!tex || alpha <= 0.002f || curOpac <= 0.002f || clipped_out(x, y, w, h)) { spotK = 0; return; } /* (an invisible piece of glass costs nothing: the bar lifted away over a trailer) */
  float sx = (u1 - u0) / w, sy = (v1 - v0) / h; /* uv per layout unit, across and down (down may be negative) */
  int cr = (int)(fminf(fmaxf(spotU, 0), 1) * 255), cg = (int)(fminf(fmaxf(spotV, 0), 1) * 255), cb = (int)(fminf(fmaxf(spotK, 0), 1) * 255); uint32_t c = RGBA(cr, cg, cb, (int)(255 * alpha)); spotK = 0;
  quadPad = sx * (curSx != 0 ? 1.0f / curSx : 1); quad(P_LIQUID, tex, x, y, w, h, r, sy * (curSy != 0 ? 1.0f / curSy : 1), pull * curSx, 0, c, c, c, c, u0, v0, u1, v1);
}
void gfx_image(GLuint tex, float x, float y, float w, float h, float r, uint32_t tint, float u0, float v0, float u1, float v1) {
  if (!tex || invisible(tint) || clipped_out(x, y, w, h)) return;
  { float f = gfx_tex_fade(tex); if (f < 1) tint = col_alpha(tint, f); }
  float m = r + 2; /* a big rounded picture (a sheet's glass): the rounded program only on its rim, the plain one inside (the same pixels: coverage is 1 there) */
  if (r > 0 && w > 4 * m && h > 4 * m && w * h > 120000.0f && (tint >> 24) == 255 && curOpac >= 0.999f) { /* (opaque only: a fading one would show the pieces' 1 px overlaps) */
    sdfFull = 1; sdfR[0] = x; sdfR[1] = y; sdfR[2] = w; sdfR[3] = h;
    float pr[4][4] = { { x, y, w, m }, { x, y + h - m, w, m }, { x, y + m, m, h - 2 * m }, { x + w - m, y + m, m, h - 2 * m } };
    for (int i = 0; i < 4; i++) { float px = pr[i][0], py = pr[i][1], pw = pr[i][2], ph = pr[i][3];
      quad(P_IMAGE, tex, px, py, pw, ph, r, 0, 0, 1, tint, tint, tint, tint, u0 + (u1 - u0) * (px - x) / w, v0 + (v1 - v0) * (py - y) / h, u0 + (u1 - u0) * (px + pw - x) / w, v0 + (v1 - v0) * (py + ph - y) / h); }
    sdfFull = 0;
    quad(P_PLAINIMG, tex, x + m, y + m, w - 2 * m, h - 2 * m, 0, 0, 0, 0, tint, tint, tint, tint, u0 + (u1 - u0) * m / w, v0 + (v1 - v0) * m / h, u1 - (u1 - u0) * m / w, v1 - (v1 - v0) * m / h);
    return; }
  quad(r > 0 ? P_IMAGE : P_PLAINIMG, tex, x, y, w, h, r, 0, 0, r > 0 ? 1 : 0, tint, tint, tint, tint, u0, v0, u1, v1);
}
/* a picture turned in depth: rx about its horizontal middle, ry about its vertical middle (degrees), seen from dist
   layout units in front, drawn as a fine grid of quads so it keeps its perspective */
void gfx_alpha_tex_uv(GLuint tex, float x, float y, float w, float h, uint32_t c, float u0, float v0, float u1, float v1) {
  if (!tex || invisible(c) || clipped_out(x, y, w, h)) return;
  quad(P_ALPHA, tex, x, y, w, h, 0, 0, 0, 0, c, c, c, c, u0, v0, u1, v1);
}
void gfx_tex_update(GLuint t, const unsigned char *px, int x, int y, int w, int h, int ch) {
  gfx_flush(); /* what is queued draws first (each flush binds its own texture again) */
  GLenum f = ch == 1 ? GL_ALPHA : ch == 3 ? GL_RGB : GL_RGBA;
  glBindTexture(GL_TEXTURE_2D, t); glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, f, GL_UNSIGNED_BYTE, px);
}
void gfx_alpha_tex(GLuint tex, float x, float y, float w, float h, uint32_t c) {
  if (!tex || invisible(c) || clipped_out(x, y, w, h)) return;
  quad(P_ALPHA, tex, x, y, w, h, 0, 0, 0, 0, c, c, c, c, 0, 0, 1, 1);
}

/* ---- offscreen pictures ------------------------------------------------------------------- */
int gfx_target_create(GfxTarget *t, int w, int h) {
  memset(t, 0, sizeof *t);
  glGenTextures(1, &t->tex); glBindTexture(GL_TEXTURE_2D, t->tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL); treg_add(t->tex, w, h, 4);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glGenFramebuffers(1, &t->fbo); glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
  int ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  glBindFramebuffer(GL_FRAMEBUFFER, screenFbo);
  if (curTex == t->tex) curTex = 0;
  if (!ok) { fprintf(stderr, "[gfx] offscreen target %dx%d unavailable\n", w, h); gfx_target_free(t); return 0; }
  t->w = w; t->h = h; return 1;
}
void gfx_target_free(GfxTarget *t) { if (t->fbo) glDeleteFramebuffers(1, &t->fbo); if (t->tex) gfx_tex_free(t->tex); memset(t, 0, sizeof *t); }
static struct { int on, surfW, surfH, nclip, nopac, noff, nsc, viewX, viewY, viewW, viewH; float scl, curOpac, curOx, curOy, curSx, curSy, curSox, curSoy; Clip curClip, clips[16]; float opac[16], offx[16], offy[16], scx[16], scy[16], sox[16], soy[16], scOx[16], scOy[16]; } saved;
/* a blur pass: src (a target's texture, w x h texels) drawn over the whole of dst, blurred along one axis; spread
   scales the step (1 = adjacent texels) */
void gfx_blur_pass(GfxTarget *dst, GLuint src, int w, int h, int vertical, float spread) {
  gfx_target_begin(dst, 0);
  float dx = vertical ? 0 : spread / (float)w, dy = vertical ? spread / (float)h : 0, LH = dst->h * SB_W / (float)dst->w;
  if (P_BLUR != curProg || src != curTex || nvtx + 6 > MAX_VTX) { gfx_flush(); curProg = P_BLUR; curTex = src; batchOpaque = 1; }
  uint32_t k = to_abgr(0xFFFFFFFFu); Vtx *q = &vtx[nvtx]; nvtx += 6;
  Vtx a = { 0, 0, 0, 1, k, 0, 0, 0, 0, dx, dy, 0, 0 }, b = { SB_W, 0, 1, 1, k, 0, 0, 0, 0, dx, dy, 0, 0 }, d = { 0, LH, 0, 0, k, 0, 0, 0, 0, dx, dy, 0, 0 }, e = { SB_W, LH, 1, 0, k, 0, 0, 0, 0, dx, dy, 0, 0 };
  q[0] = a; q[1] = b; q[2] = d; q[3] = b; q[4] = e; q[5] = d;
  gfx_target_end();
}
int gfx_mid_switches(void) { int n = midSwitch; midSwitch = 0; return n; }
void gfx_target_begin(GfxTarget *t, uint32_t c) {
  gfx_flush();
  if (scrDirty) { midSwitch++; if (getenv("SB_MIDLOG")) fprintf(stderr, "[mid] %ux%u at frame %u\n", t->w, t->h, frameCounter); }
  tgtOn = 1; saved.on = 1; saved.surfW = surfW; saved.surfH = surfH; saved.scl = scl; saved.viewX = viewX; saved.viewY = viewY; saved.viewW = viewW; saved.viewH = viewH; saved.nclip = nclip; saved.nopac = nopac; saved.noff = noff; saved.nsc = nsc;
  saved.curOpac = curOpac; saved.curOx = curOx; saved.curOy = curOy; saved.curSx = curSx; saved.curSy = curSy; saved.curSox = curSox; saved.curSoy = curSoy; saved.curClip = curClip;
  memcpy(saved.clips, clips, sizeof clips); memcpy(saved.opac, opac, sizeof opac); memcpy(saved.offx, offx, sizeof offx); memcpy(saved.offy, offy, sizeof offy); memcpy(saved.scx, scx, sizeof scx); memcpy(saved.scy, scy, sizeof scy); memcpy(saved.sox, sox, sizeof sox); memcpy(saved.soy, soy, sizeof soy); memcpy(saved.scOx, scOx, sizeof scOx); memcpy(saved.scOy, scOy, sizeof scOy);
  nclip = 0; nopac = 0; noff = 0; nsc = 0; curOpac = 1; curOx = curOy = 0; curSx = curSy = 1; curSox = curSoy = 0;
  fadeOff = 1;
  /* a bake's draws assume blending on (a picture at half strength over a clear): the previous frame's last batch may
     have left it off (an opaque fill skips the read-modify-write), which baked translucent layers instead of dimmed
     opaque ones — the paused film showing through the screensaver */
  glEnable(GL_BLEND); glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA); blendOn = 1;
  glBindFramebuffer(GL_FRAMEBUFFER, t->fbo); surfW = t->w; surfH = t->h; scl = (float)t->w / SB_W; glViewport(0, 0, t->w, t->h);
  glDisable(GL_SCISSOR_TEST);
  glClearColor(((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f, ((c >> 24) & 255) / 255.0f); glClear(GL_COLOR_BUFFER_BIT);
}
int gfx_in_target(void) { return saved.on; }
void gfx_target_end(void) {
  if (!saved.on) return;
  gfx_flush(); saved.on = 0; tgtOn = 0; fadeOff = 0;
  glBindFramebuffer(GL_FRAMEBUFFER, screenFbo); surfW = saved.surfW; surfH = saved.surfH; scl = saved.scl; viewX = saved.viewX; viewY = saved.viewY; viewW = saved.viewW; viewH = saved.viewH; glViewport(viewX, viewY, viewW, viewH);
  nclip = saved.nclip; nopac = saved.nopac; noff = saved.noff; nsc = saved.nsc; curOpac = saved.curOpac; curOx = saved.curOx; curOy = saved.curOy;
  curSx = saved.curSx; curSy = saved.curSy; curSox = saved.curSox; curSoy = saved.curSoy; curClip = saved.curClip;
  memcpy(clips, saved.clips, sizeof clips); memcpy(opac, saved.opac, sizeof opac); memcpy(offx, saved.offx, sizeof offx); memcpy(offy, saved.offy, sizeof offy); memcpy(scx, saved.scx, sizeof scx); memcpy(scy, saved.scy, sizeof scy); memcpy(sox, saved.sox, sizeof sox); memcpy(soy, saved.soy, sizeof soy); memcpy(scOx, saved.scOx, sizeof scOx); memcpy(scOy, saved.scOy, sizeof scOy);
  apply_clip();
}

/* ---- state stacks ------------------------------------------------------------------------- */
void gfx_xform_point(float *x, float *y) { xform(x, y); } /* layout -> screen, with the offset and scale stacks (for the pointer's rectangles) */
void gfx_clip_push(float x, float y, float w, float h) {
  float x0 = x, y0 = y, x1 = x + w, y1 = y + h; xform(&x0, &y0); xform(&x1, &y1); mir(&x0, &x1);
  Clip c = { x0, y0, x1 - x0, y1 - y0 };
  if (nclip) { float ax = c.x > curClip.x ? c.x : curClip.x, ay = c.y > curClip.y ? c.y : curClip.y;
    float bx = (c.x + c.w) < (curClip.x + curClip.w) ? (c.x + c.w) : (curClip.x + curClip.w), by = (c.y + c.h) < (curClip.y + curClip.h) ? (c.y + c.h) : (curClip.y + curClip.h);
    c.x = ax; c.y = ay; c.w = bx - ax; c.h = by - ay; }
  if (nclip < 16) clips[nclip] = curClip; nclip++; curClip = c; apply_clip();
}
void gfx_clip_pop(void) { if (!nclip) return; nclip--; if (nclip < 16) curClip = clips[nclip]; apply_clip(); }
void gfx_opacity_push(float k) { if (nopac < 16) opac[nopac] = curOpac; nopac++; curOpac *= k; }
void gfx_opacity_pop(void) { if (!nopac) return; nopac--; if (nopac < 16) curOpac = opac[nopac]; }
void gfx_offset_push(float dx, float dy) { if (noff < 16) { offx[noff] = curOx; offy[noff] = curOy; } noff++; curOx += dx * curSx; curOy += dy * curSy; }
void gfx_offset_pop(void) { if (!noff) return; noff--; if (noff < 16) { curOx = offx[noff]; curOy = offy[noff]; } }
/* the transform is p' = p * S + O (curSox/curSoy stay 0): a scale about (ox, oy) in the current space composes as
   O += o * (1 - s) * S, S *= s, so scales nest at any depth (a card's pop inside a screen's zoom) */
void gfx_scale_push(float sx, float sy, float ox, float oy) {
  if (nsc < 16) { scx[nsc] = curSx; scy[nsc] = curSy; sox[nsc] = curSox; soy[nsc] = curSoy; scOx[nsc] = curOx; scOy[nsc] = curOy; } nsc++;
  curOx += ox * (1 - sx) * curSx; curOy += oy * (1 - sy) * curSy; curSx *= sx; curSy *= sy;
}
void gfx_scale_pop(void) { if (!nsc) return; nsc--; if (nsc < 16) { curSx = scx[nsc]; curSy = scy[nsc]; curSox = sox[nsc]; curSoy = soy[nsc]; curOx = scOx[nsc]; curOy = scOy[nsc]; } }

/* the same disc in white with its alpha, for the rounded image program */
static GLuint radial_rgba(void) {
  static GLuint t; if (t) return t; const int N = 128; unsigned char *px = malloc((size_t)N * N * 4);
  for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) { float dx = (x + 0.5f) / N * 2 - 1, dy = (y + 0.5f) / N * 2 - 1, d = sqrtf(dx * dx + dy * dy); float a = d >= 1 ? 0 : (1 - d) * (1 - d) * (3 - 2 * (1 - d)); unsigned char *q = px + (y * N + x) * 4; q[0] = q[1] = q[2] = 255; q[3] = (unsigned char)(a * 255 + 0.5f); }
  t = gfx_tex_create(px, N, N, 4, 1); free(px); return t;
}
void gfx_glow_round(float gx, float gy, float gw, float gh, uint32_t c, float x, float y, float w, float h, float r) {
  if (gw <= 0 || gh <= 0) return; /* the box's own quad, the glow's texture mapped onto it (clamped: nothing past the disc) */
  float x0 = fmaxf(x, gx), y0 = fmaxf(y, gy), x1 = fminf(x + w, gx + gw), y1 = fminf(y + h, gy + gh); if (x1 <= x0 || y1 <= y0) return;
  gfx_image(radial_rgba(), x, y, w, h, r, c, (x - gx) / gw, (y - gy) / gh, (x + w - gx) / gw, (y + h - gy) / gh);
}
/* a soft disc as a small alpha texture (drawn stretched): a radial glow costs a plain textured quad, not a soft SDF per pixel */
GLuint gfx_radial_tex(void) {
  static GLuint t; if (t) return t; const int N = 128; unsigned char *px = malloc((size_t)N * N);
  for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) { float dx = (x + 0.5f) / N * 2 - 1, dy = (y + 0.5f) / N * 2 - 1, d = sqrtf(dx * dx + dy * dy); float a = d >= 1 ? 0 : (1 - d) * (1 - d) * (3 - 2 * (1 - d)); px[y * N + x] = (unsigned char)(a * 255 + 0.5f); }
  t = gfx_tex_create(px, N, N, 1, 1); free(px); return t;
}
unsigned gfx_frame(void) { return frameCounter; }
