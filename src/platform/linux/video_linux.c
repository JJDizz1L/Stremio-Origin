/* Linux desktop: libmpv's OpenGL render API. */
#include "core/video.h"
#include "core/store.h"
#include "platform/plat.h"
#include "ui/gfx.h"
#include <SDL2/SDL.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static mpv_handle *mpvH;
static mpv_render_context *mpvGL;
static VidState st = VID_IDLE;
static int liveFlag = 0;
static char urlBuf[8192];
static double pos = 0, dur = 0;
static int vw = 0, vh = 0;
static double fpsv = 0;
static char errBuf[256] = "";
static VidTrack aud[16]; static int naud = 0;
static VidTrack txt[24]; static int ntxt = 0;
static int curAud = -1, curTxt = -1;
static int surfW = 1920, surfH = 1080;
static int winX = 0, winY = 0, winW = 1920, winH = 1080;
static int cropOn = 0, cropSX = 0, cropSY = 0, cropSW = 0, cropSH = 0;
static int propCrop = -1; /* video-crop as mpv has it (1/0): always cleared, set on change only */
#include <math.h>
static int timedDur = 0;
static unsigned pumpN = 0;
static GfxTarget vidT; /* the frame, rendered by mpv at exactly the video rect's size, composited as one UI quad */
static double cacheAhead = 0; /* seconds mpv holds ahead of the playhead (demuxer-cache-duration) */
static int seeking = 0;       /* mpv's seeking flag: a seek asked, not yet done */
static double vFrom = 100, vTo = 100, vT0 = 0, vDur = 0; /* a volume fade in flight */
static int timedOn = 0; static char timedBase[4096]; static double timedOff = 0; /* one continuous conversion: seeks reload base?t= */
static double now_s(void) { return SDL_GetTicks() / 1000.0; }
static void vol_apply(double v) { if (!mpvH) return; mpv_set_property(mpvH, "volume", MPV_FORMAT_DOUBLE, &v); }
static void vol_fade_tick(void) {
  if (vDur <= 0 || !mpvH) return;
  double k = (now_s() - vT0) / vDur;
  if (k >= 1) { vDur = 0; vol_apply(vTo); return; }
  k = k * k * (3 - 2 * k); vol_apply(vFrom + (vTo - vFrom) * k);
}

static void *get_proc_address(void *ctx, const char *name) { (void)ctx; return SDL_GL_GetProcAddress(name); }

static void update_tracks(void) {
  if (!mpvH) return;
  mpv_node node;
  if (mpv_get_property(mpvH, "track-list", MPV_FORMAT_NODE, &node) < 0) return;
  naud = 0; ntxt = 0; curAud = -1; curTxt = -1;
  if (node.format == MPV_FORMAT_NODE_ARRAY && node.u.list) {
    for (int i = 0; i < node.u.list->num; i++) {
      mpv_node *t = &node.u.list->values[i];
      if (t->format != MPV_FORMAT_NODE_MAP || !t->u.list) continue;
      const char *type = NULL; int id = 0; int selected = 0; const char *lang = NULL; const char *title = NULL; const char *codec = NULL; int channels = 0;
      for (int k = 0; k < t->u.list->num; k++) {
        const char *key = t->u.list->keys[k];
        mpv_node *v = &t->u.list->values[k];
        if (!strcmp(key, "type") && v->format == MPV_FORMAT_STRING) type = v->u.string;
        else if (!strcmp(key, "id") && v->format == MPV_FORMAT_INT64) id = (int)v->u.int64;
        else if (!strcmp(key, "selected") && v->format == MPV_FORMAT_FLAG) selected = v->u.flag;
        else if (!strcmp(key, "lang") && v->format == MPV_FORMAT_STRING) lang = v->u.string;
        else if (!strcmp(key, "title") && v->format == MPV_FORMAT_STRING) title = v->u.string;
        else if (!strcmp(key, "codec") && v->format == MPV_FORMAT_STRING) codec = v->u.string;
        else if (!strcmp(key, "demux-channel-count") && v->format == MPV_FORMAT_INT64) channels = (int)v->u.int64;
      }
      if (!type) continue;
      if (!strcmp(type, "audio") && naud < 16) {
        VidTrack *tr = &aud[naud++];
        memset(tr, 0, sizeof *tr);
        tr->idx = id;
        snprintf(tr->lang, sizeof tr->lang, "%s", lang ? lang : "");
        snprintf(tr->name, sizeof tr->name, "%s", title ? title : (lang ? lang : "audio"));
        snprintf(tr->codec, sizeof tr->codec, "%s", codec ? codec : "");
        tr->channels = channels > 0 ? channels : 2;
        if (selected) curAud = naud - 1;
      } else if (!strcmp(type, "sub") && ntxt < 24) {
        VidTrack *tr = &txt[ntxt++];
        memset(tr, 0, sizeof *tr);
        tr->idx = id;
        snprintf(tr->lang, sizeof tr->lang, "%s", lang ? lang : "");
        snprintf(tr->name, sizeof tr->name, "%s", title ? title : (lang ? lang : "sub"));
        if (selected) curTxt = ntxt - 1;
      }
    }
  }
  mpv_free_node_contents(&node);
  if (naud > 0 && curAud < 0) curAud = 0;
}

static void process_events(void) {
  if (!mpvH) return;
  while (1) {
    mpv_event *ev = mpv_wait_event(mpvH, 0);
    if (!ev || ev->event_id == MPV_EVENT_NONE) break;
    switch (ev->event_id) {
      case MPV_EVENT_FILE_LOADED:
        update_tracks();
        break;
      case MPV_EVENT_PLAYBACK_RESTART:
        liveFlag = 1;
        if (st == VID_LOADING || st == VID_IDLE) st = VID_PLAYING;
        break;
      case MPV_EVENT_END_FILE: {
        mpv_event_end_file *e = ev->data;
        if (e && e->reason == MPV_END_FILE_REASON_EOF) st = VID_ENDED;
        else if (e && e->reason == MPV_END_FILE_REASON_ERROR) {
          st = VID_ERROR;
          snprintf(errBuf, sizeof errBuf, "%s", mpv_error_string(e->error));
        }
        break;
      }
      case MPV_EVENT_LOG_MESSAGE: {
        mpv_event_log_message *m = ev->data; /* mpv's own warnings/errors, for the player's status line */
        if (m && m->text && (!strcmp(m->level, "error") || !strcmp(m->level, "fatal"))) {
          snprintf(errBuf, sizeof errBuf, "%.200s", m->text);
          size_t L = strlen(errBuf); while (L && (errBuf[L - 1] == '\n' || errBuf[L - 1] == ' ')) errBuf[--L] = 0;
        }
        break;
      }
      default: break;
    }
  }
  if (liveFlag && naud == 0 && ntxt == 0) update_tracks();
  if (liveFlag || st == VID_PLAYING || st == VID_PAUSED || st == VID_BUFFERING) {
    double t;
    if (mpv_get_property(mpvH, "time-pos", MPV_FORMAT_DOUBLE, &t) == 0) pos = t;
    double d;
    if (mpv_get_property(mpvH, "duration", MPV_FORMAT_DOUBLE, &d) == 0 && d > 0) dur = d;
    if (timedDur > 0 && (dur <= 0 || timedOn)) dur = timedDur; /* conversions report their own length */
    int p;
    if (mpv_get_property(mpvH, "pause", MPV_FORMAT_FLAG, &p) == 0) {
      if (p && st == VID_PLAYING) st = VID_PAUSED;
      if (!p && st == VID_PAUSED) st = VID_PLAYING;
    }
  }
  pumpN++;
  if (vw == 0 || vh == 0 || (pumpN % 30) == 0) { /* size/fps change rarely: don't round-trip IPC for them every frame */
    int64_t w, h;
    if (mpv_get_property(mpvH, "width", MPV_FORMAT_INT64, &w) == 0) vw = (int)w;
    if (mpv_get_property(mpvH, "height", MPV_FORMAT_INT64, &h) == 0) vh = (int)h;
    double f;
    if (mpv_get_property(mpvH, "container-fps", MPV_FORMAT_DOUBLE, &f) == 0 && f > 0) fpsv = f;
    else if (fpsv <= 0 && mpv_get_property(mpvH, "estimated-vf-fps", MPV_FORMAT_DOUBLE, &f) == 0 && f > 0) fpsv = f;
  }
  { double cb; if (mpv_get_property(mpvH, "demuxer-cache-duration", MPV_FORMAT_DOUBLE, &cb) == 0 && cb >= 0) cacheAhead = cb; } /* seconds buffered ahead */
  { int s; if (mpv_get_property(mpvH, "seeking", MPV_FORMAT_FLAG, &s) == 0) seeking = s; }
  vol_fade_tick();
}

/* Decoding (Settings > Playback > Decoding): anything this mpv takes on Linux. Unknown values fall back to auto. */
static const char *HWDEC_OK[] = { "auto", "auto-safe", "auto-copy", "no", "nvdec", "nvdec-copy", "vaapi", "vaapi-copy", "vdpau", "cuda", "vulkan", "qsv", NULL };
static const char *hwdec_mode(void) {
  const char *m = store_get("hwdecMode", "auto");
  for (int i = 0; HWDEC_OK[i]; i++) if (!strcmp(m, HWDEC_OK[i])) return m;
  return "auto";
}
void video_set_hwdec(const char *mode) { /* live for the next title played (the running file keeps its decoder) */
  if (!mpvH || !mode) return;
  const char *m = mode; int ok = 0;
  for (int i = 0; HWDEC_OK[i]; i++) if (!strcmp(m, HWDEC_OK[i])) { ok = 1; break; }
  if (!ok) m = "auto";
  if (mpv_set_property_string(mpvH, "hwdec", m) == 0) fprintf(stderr, "[video] hwdec %s\n", m);
}

int video_init(const char *appId) {
  (void)appId;
  mpvH = mpv_create();
  if (!mpvH) return 0;
  mpv_set_option_string(mpvH, "vo", "libmpv");
  mpv_set_option_string(mpvH, "hwdec", hwdec_mode());
  mpv_set_option_string(mpvH, "sid", "no");        /* the app draws its own UI/subs on top: no auto subs, no OSD */
  mpv_set_option_string(mpvH, "sub-auto", "no");
  mpv_set_option_string(mpvH, "osd-level", "0");
  if (mpv_initialize(mpvH) < 0) { mpv_destroy(mpvH); mpvH = NULL; return 0; }
  mpv_request_log_messages(mpvH, "warn");
  mpv_render_param params[] = {
    {MPV_RENDER_PARAM_API_TYPE, MPV_RENDER_API_TYPE_OPENGL},
    {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &(mpv_opengl_init_params){ .get_proc_address = get_proc_address }},
    {0}
  };
  if (mpv_render_context_create(&mpvGL, mpvH, params) < 0) { mpv_destroy(mpvH); mpvH = NULL; return 0; }
  return 1;
}

void video_load(const char *u) {
  if (!mpvH) return;
  snprintf(urlBuf, sizeof urlBuf, "%s", u);
  st = VID_LOADING; liveFlag = 0; pos = 0; dur = 0; naud = 0; ntxt = 0; curAud = -1; curTxt = -1; cropOn = 0; timedDur = 0;
  timedOn = 0; timedBase[0] = 0; timedOff = 0; cacheAhead = 0; seeking = 0;
  const char *cmd[] = {"loadfile", urlBuf, "replace", NULL};
  mpv_command(mpvH, cmd);
  mpv_set_property_string(mpvH, "sid", "no"); /* in case a previous file left a subtitle selected */
}

void video_load_timed(const char *base, double durSec) {
  if (!mpvH) return;
  snprintf(timedBase, sizeof timedBase, "%s", base); timedOn = 1; timedOff = 0;
  char u[8192]; snprintf(u, sizeof u, "%s?t=0", base);
  video_load(u);
  timedOn = 1; snprintf(timedBase, sizeof timedBase, "%s", base); /* video_load clears it */
  if (durSec > 0) { timedDur = (int)durSec; dur = durSec; }
}

void video_unload(void) {
  if (!mpvH) return;
  const char *cmd[] = {"stop", NULL};
  mpv_command(mpvH, cmd);
  gfx_target_free(&vidT);
  st = VID_IDLE; liveFlag = 0; pos = 0; dur = 0; naud = 0; ntxt = 0; curAud = -1; curTxt = -1; cropOn = 0; timedDur = 0;
  timedOn = 0; timedBase[0] = 0; timedOff = 0; cacheAhead = 0; seeking = 0;
}

void video_play(void) {
  if (!mpvH) return;
  int p = 0; mpv_set_property(mpvH, "pause", MPV_FORMAT_FLAG, &p);
  if (st == VID_PAUSED) st = VID_PLAYING;
}

void video_pause(void) {
  if (!mpvH) return;
  int p = 1; mpv_set_property(mpvH, "pause", MPV_FORMAT_FLAG, &p);
  if (st == VID_PLAYING) st = VID_PAUSED;
}

void video_seek(double sec) {
  if (!mpvH) return;
  if (timedOn && timedBase[0]) { /* one continuous conversion: each seek loads it again there */
    if (sec < 0) sec = 0; timedOff = sec; pos = 0; /* mpv restarts at 0: the time reads from the offset on */
    char u[8192]; snprintf(u, sizeof u, "%s?t=%lld", timedBase, (long long)sec);
    snprintf(urlBuf, sizeof urlBuf, "%s", u);
    const char *cmd[] = {"loadfile", urlBuf, "replace", NULL};
    mpv_command(mpvH, cmd); st = VID_BUFFERING;
    return;
  }
  mpv_set_property(mpvH, "time-pos", MPV_FORMAT_DOUBLE, &sec);
}

double video_time(void) { return timedOn ? timedOff + pos : pos; }
double video_duration(void) { return dur; }
double video_buffered_end(void) { double t = video_time(), b = t + (cacheAhead > 0 ? cacheAhead : 60); if (dur > 0 && b > dur) b = dur; return b; }
VidState video_state(void) { return st; }
const char *video_error(void) { return errBuf; }
int video_live(void) { return liveFlag; }
int video_has_media(void) { return st != VID_IDLE; }
void video_set_volume(int pct, int easeMs) {
  if (!mpvH) return;
  if (pct < 0) pct = 0; if (pct > 100) pct = 100;
  if (easeMs <= 0) { vFrom = vTo = pct; vDur = 0; vol_apply((double)pct); return; }
  vFrom = vDur > 0 ? vFrom + (vTo - vFrom) * fmin(1, (now_s() - vT0) / vDur) : vTo; /* from where it is now */
  vTo = pct; vT0 = now_s(); vDur = easeMs / 1000.0;
}
int video_settling(void) { return seeking; }
int video_width(void) { return vw; }
int video_height(void) { return vh; }
double video_fps(void) { return fpsv; }
int video_hdr(char *out, int cap) {
  if (cap <= 0 || !mpvH) { if (cap > 0) out[0] = 0; return 0; }
  mpv_node node;
  if (mpv_get_property(mpvH, "video-params", MPV_FORMAT_NODE, &node) < 0) { out[0] = 0; return 0; }
  const char *gamma = NULL, *primaries = NULL;
  if (node.format == MPV_FORMAT_NODE_MAP && node.u.list) {
    for (int i = 0; i < node.u.list->num; i++) {
      const char *key = node.u.list->keys[i];
      mpv_node *v = &node.u.list->values[i];
      if (v->format != MPV_FORMAT_STRING) continue;
      if (!strcmp(key, "gamma")) gamma = v->u.string;
      else if (!strcmp(key, "primaries")) primaries = v->u.string;
    }
  }
  mpv_free_node_contents(&node);
  int isHdr = 0;
  if (gamma && (strstr(gamma, "pq") || strstr(gamma, "st2084") || strstr(gamma, "smp") )) { snprintf(out, (size_t)cap, "%s", "HDR10"); isHdr = 1; }
  else if (primaries && strstr(primaries, "2020") && gamma && strstr(gamma, "pq")) { snprintf(out, (size_t)cap, "%s", "HDR10"); isHdr = 1; }
  else snprintf(out, (size_t)cap, "%s", "none");
  return isHdr;
}
int video_audio_tracks(VidTrack *out, int max) { int n = naud < max ? naud : max; for (int i = 0; i < n; i++) out[i] = aud[i]; return naud; }
int video_audio_current(void) { return curAud; }
void video_select_audio(int idx) {
  if (!mpvH) return;
  if (idx < 0 || idx >= naud) { mpv_set_property_string(mpvH, "aid", "no"); curAud = -1; return; }
  char buf[32]; snprintf(buf, sizeof buf, "%d", aud[idx].idx); mpv_set_property_string(mpvH, "aid", buf); curAud = idx;
}
int video_text_tracks(VidTrack *out, int max) { int n = ntxt < max ? ntxt : max; for (int i = 0; i < n; i++) out[i] = txt[i]; return ntxt; }
void video_select_text(int idx) {
  if (!mpvH) return;
  if (idx < 0 || idx >= ntxt) { mpv_set_property_string(mpvH, "sid", "no"); curTxt = -1; return; }
  char buf[32]; snprintf(buf, sizeof buf, "%d", txt[idx].idx); mpv_set_property_string(mpvH, "sid", buf); curTxt = idx;
}
void video_set_window(float x, float y, float w, float h) {
  winX = (int)x; winY = (int)y; winW = (int)w; winH = (int)h; cropOn = 0;
}
int video_partial_plane(void) { return 1; }
int video_set_crop(int sx, int sy, int sw, int sh, float dx, float dy, float dw, float dh) {
  cropOn = 1; cropSX = sx; cropSY = sy; cropSW = sw; cropSH = sh;
  winX = (int)dx; winY = (int)dy; winW = (int)dw; winH = (int)dh;
  return 1;
}
void video_set_surface(int w, int h) { if (w > 0 && h > 0) { surfW = w; surfH = h; } }
int video_audio_unsupported(void) { return 0; }
void video_pump(void) { process_events(); }

void video_render(void) {
  if (!mpvGL || !liveFlag) return;
  /* video_pump() (player tick) already refreshed events/properties this frame: don't round-trip IPC twice.
     mpv is never asked to crop (see below): the property stays cleared. */
  /* mpv fills (0,0,w,h) of whatever FBO is bound (fbo.w/h must be that FBO's real
     size), so a sub-rectangle video is rendered into its own FBO and composited as
     one UI quad: direct-to-screen would sink it to the window's bottom-left.
     The crop itself is a UV sub-rectangle, not mpv's video-crop (which rotates
     cropped output 180 degrees): full frame in the FBO, source rect sampled out. */
  int sx, syTop, sw, sh; /* drawable pixels, top-left origin, inside the letterboxed UI viewport */
  gfx_layout_to_surface((float)winX, (float)winY, (float)winW, (float)winH, &sx, &syTop, &sw, &sh);
  if (sw < 2 || sh < 2) return;
  float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
  int fw = sw, fh = sh;
  if (cropOn && vw > 0 && vh > 0 && cropSW > 0 && cropSH > 0) {
    float s = fmaxf((float)sw / cropSW, (float)sh / cropSH); /* cover: the crop region at full dst resolution */
    if (s < 0.05f) s = 0.05f;
    fw = (int)(vw * s + 0.5f); fh = (int)(vh * s + 0.5f);
    if (fw < 2) fw = 2; if (fh < 2) fh = 2;
    /* the FBO holds the frame upright (same orientation as a decoded picture): sample the source rect straight out */
    u0 = cropSX * s / fw; u1 = (cropSX + cropSW) * s / fw;
    v0 = cropSY * s / fh; v1 = (cropSY + cropSH) * s / fh;
  }
  if (cropOn != propCrop) { propCrop = cropOn; mpv_set_property_string(mpvH, "video-crop", ""); } /* never cropped in mpv */
  if (fw != vidT.w || fh != vidT.h) {
    gfx_target_free(&vidT);
    if (!gfx_target_create(&vidT, fw, fh)) { vidT.tex = 0; return; }
  }

  GLint prevFbo = 0, prevVp[4] = {0,0,0,0}, prevArrayBuf = 0, prevProg = 0, prevTex = 0, prevActive = 0;
  GLboolean prevBlend = GL_FALSE, prevScissor = GL_FALSE, prevDepth = GL_FALSE, prevCull = GL_FALSE;
  GLint prevBlendSrc = 0, prevBlendDst = 0;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
  glGetIntegerv(GL_VIEWPORT, prevVp);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArrayBuf);
  glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
  prevBlend = glIsEnabled(GL_BLEND); prevScissor = glIsEnabled(GL_SCISSOR_TEST);
  prevDepth = glIsEnabled(GL_DEPTH_TEST); prevCull = glIsEnabled(GL_CULL_FACE);
  glGetIntegerv(GL_BLEND_SRC, &prevBlendSrc); glGetIntegerv(GL_BLEND_DST, &prevBlendDst);
#ifdef GL_VERTEX_ARRAY_BINDING
  GLint prevVao = 0; glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
#else
  GLint prevVao = 0;
#endif

  gfx_target_begin(&vidT, 0); /* transparent: mpv draws the frame over it */
  mpv_opengl_fbo fbo = { .fbo = (int)vidT.fbo, .w = vidT.w, .h = vidT.h };
  mpv_render_param params[] = {
    {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
    {MPV_RENDER_PARAM_FLIP_Y, &(int){0}},
    {0}
  };
  mpv_render_context_render(mpvGL, params);
  gfx_target_end();

  glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
  glViewport(prevVp[0], prevVp[1], prevVp[2], prevVp[3]);
  glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevArrayBuf);
#ifdef GL_VERTEX_ARRAY_BINDING
  glBindVertexArray((GLuint)prevVao);
#endif
  glUseProgram((GLuint)prevProg);
  glActiveTexture((GLenum)prevActive); glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
  if (prevBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
  glBlendFunc((GLenum)prevBlendSrc, (GLenum)prevBlendDst);
  if (prevScissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
  if (prevDepth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
  if (prevCull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
  gfx_invalidate_state(); /* the UI's next batch re-asserts program, texture and blend from scratch */
  gfx_image(vidT.tex, (float)winX, (float)winY, (float)winW, (float)winH, 0, WHITE_A(255), u0, v0, u1, v1);
}