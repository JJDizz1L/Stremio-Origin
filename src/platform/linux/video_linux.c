/* Linux desktop: libmpv's OpenGL render API. */
#include "core/video.h"
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
static char cropCur[128] = ""; /* last video-crop sent to mpv: only re-sent when the aspect changes */
static int timedDur = 0;
static unsigned pumpN = 0;

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
      const char *type = NULL; int id = 0; int selected = 0; const char *lang = NULL; const char *title = NULL; int channels = 0;
      for (int k = 0; k < t->u.list->num; k++) {
        const char *key = t->u.list->keys[k];
        mpv_node *v = &t->u.list->values[k];
        if (!strcmp(key, "type") && v->format == MPV_FORMAT_STRING) type = v->u.string;
        else if (!strcmp(key, "id") && v->format == MPV_FORMAT_INT64) id = (int)v->u.int64;
        else if (!strcmp(key, "selected") && v->format == MPV_FORMAT_FLAG) selected = v->u.flag;
        else if (!strcmp(key, "lang") && v->format == MPV_FORMAT_STRING) lang = v->u.string;
        else if (!strcmp(key, "title") && v->format == MPV_FORMAT_STRING) title = v->u.string;
        else if (!strcmp(key, "demux-channel-count") && v->format == MPV_FORMAT_INT64) channels = (int)v->u.int64;
      }
      if (!type) continue;
      if (!strcmp(type, "audio") && naud < 16) {
        VidTrack *tr = &aud[naud++];
        memset(tr, 0, sizeof *tr);
        tr->idx = id;
        snprintf(tr->lang, sizeof tr->lang, "%s", lang ? lang : "");
        snprintf(tr->name, sizeof tr->name, "%s", title ? title : (lang ? lang : "audio"));
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
      default: break;
    }
  }
  if (liveFlag && naud == 0 && ntxt == 0) update_tracks();
  if (liveFlag || st == VID_PLAYING || st == VID_PAUSED || st == VID_BUFFERING) {
    double t;
    if (mpv_get_property(mpvH, "time-pos", MPV_FORMAT_DOUBLE, &t) == 0) pos = t;
    double d;
    if (mpv_get_property(mpvH, "duration", MPV_FORMAT_DOUBLE, &d) == 0 && d > 0) dur = d;
    if (timedDur > 0 && dur <= 0) dur = timedDur;
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
  }
}

int video_init(const char *appId) {
  (void)appId;
  mpvH = mpv_create();
  if (!mpvH) return 0;
  mpv_set_option_string(mpvH, "vo", "libmpv");
  mpv_set_option_string(mpvH, "hwdec", "auto");
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
  st = VID_LOADING; liveFlag = 0; pos = 0; dur = 0; naud = 0; ntxt = 0; curAud = -1; curTxt = -1; cropOn = 0; cropCur[0] = 0; timedDur = 0;
  const char *cmd[] = {"loadfile", urlBuf, "replace", NULL};
  mpv_command(mpvH, cmd);
  mpv_set_property_string(mpvH, "sid", "no"); /* in case a previous file left a subtitle selected */
}

void video_load_timed(const char *base, double durSec) {
  char u[8192];
  snprintf(u, sizeof u, "%s?t=0", base);
  video_load(u);
  if (durSec > 0) { timedDur = (int)durSec; dur = durSec; }
}

void video_unload(void) {
  if (!mpvH) return;
  const char *cmd[] = {"stop", NULL};
  mpv_command(mpvH, cmd);
  st = VID_IDLE; liveFlag = 0; pos = 0; dur = 0; naud = 0; ntxt = 0; curAud = -1; curTxt = -1; cropOn = 0; cropCur[0] = 0; timedDur = 0;
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
  mpv_set_property(mpvH, "time-pos", MPV_FORMAT_DOUBLE, &sec);
}

double video_time(void) { return pos; }
double video_duration(void) { return dur; }
double video_buffered_end(void) { double b = pos + 60; if (dur > 0 && b > dur) b = dur; return b; }
VidState video_state(void) { return st; }
const char *video_error(void) { return errBuf; }
int video_live(void) { return liveFlag; }
int video_has_media(void) { return st != VID_IDLE; }
void video_set_volume(int pct, int easeMs) { (void)easeMs; if (!mpvH) return; double v = pct; mpv_set_property(mpvH, "volume", MPV_FORMAT_DOUBLE, &v); }
int video_settling(void) { return 0; }
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
  /* video_pump() (player tick) already refreshed events/properties this frame: don't round-trip IPC twice */
  char want[128];
  if (cropOn) snprintf(want, sizeof want, "%dx%d+%d+%d", cropSW, cropSH, cropSX, cropSY);
  else want[0] = 0;
  if (strcmp(want, cropCur)) { /* aspect changed only: re-setting video-crop every frame reconfigures the VO and flickers */
    snprintf(cropCur, sizeof cropCur, "%s", want);
    mpv_set_property_string(mpvH, "video-crop", want);
  }
  int sx, syTop, sw, sh; /* drawable pixels, top-left origin, inside the letterboxed UI viewport */
  gfx_layout_to_surface((float)winX, (float)winY, (float)winW, (float)winH, &sx, &syTop, &sw, &sh);
  if (sw < 1) sw = 1; if (sh < 1) sh = 1;
  int bottom = surfH - (syTop + sh);

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

  glViewport(sx, bottom, sw, sh);
  mpv_opengl_fbo fbo = { .fbo = (unsigned)prevFbo, .w = sw, .h = sh };
  mpv_render_param params[] = {
    {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
    {MPV_RENDER_PARAM_FLIP_Y, &(int){1}},
    {0}
  };
  mpv_render_context_render(mpvGL, params);

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
}