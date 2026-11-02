/* The Mac preview has no TV pipeline: playback is simulated (a clock that runs) so the player's
   screens can be worked on. For testing: a URL with "fail" in it errors 1.2 s after loading; SB_VID_SIZE=1920x1080
   sets the picture's size (1920x800 otherwise), SB_VID_DUR its length in seconds, SB_VID_START where the first load starts, SB_VID_SUBS=1 adds two subtitle
   tracks in the file. */
#include "core/video.h"
#include <SDL2/SDL.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
static VidState st = VID_IDLE; static double t0, pos, dur = 5400, pausedAt; static int live; static char url[4096]; static int cur = 0; static int fails; static int vw = 1920, vh = 800;
int video_init(const char *appId) { (void)appId; const char *e = getenv("SB_VID_SIZE"); if (e) sscanf(e, "%dx%d", &vw, &vh); if (getenv("SB_VID_DUR")) dur = atof(getenv("SB_VID_DUR")); return 1; }
void video_load(const char *u) { snprintf(url, sizeof url, "%s", u); st = VID_LOADING; t0 = SDL_GetTicks() / 1000.0 + 1.2; pos = 0; live = 0; fails = strstr(u, "fail") != NULL; fprintf(stderr, "[video] load %s\n", u); }
void video_load_timed(const char *base, double durSec) { char u[4200]; snprintf(u, sizeof u, "%s?t=0", base); video_load(u); if (durSec > 0) dur = durSec; }
void video_unload(void) { st = VID_IDLE; live = 0; url[0] = 0; }
void video_play(void) { if (st == VID_PAUSED) { st = VID_PLAYING; t0 = SDL_GetTicks() / 1000.0 - pos; } }
int video_has_media(void) { return live; }
void video_set_volume(int pct, int easeMs) { (void)pct; (void)easeMs; }
void video_pause(void) { if (st == VID_PLAYING) { st = VID_PAUSED; pausedAt = pos; } }
void video_seek(double s) { pos = s < 0 ? 0 : s > dur ? dur : s; t0 = SDL_GetTicks() / 1000.0 - pos; }
double video_time(void) { return pos; }
double video_duration(void) { return st == VID_IDLE || st == VID_LOADING ? 0 : dur; }
double video_buffered_end(void) { return pos + 90 > dur ? dur : pos + 90; }
VidState video_state(void) { return st; }
const char *video_error(void) { return ""; }
int video_live(void) { return live; }
int video_settling(void) { return 0; }
int video_width(void) { return live ? vw : 0; }
int video_height(void) { return live ? vh : 0; }
double video_fps(void) { return 0; }
int video_hdr(char *out, int cap) { if (cap > 0) out[0] = 0; return 0; }
/* SB_VID_AUDIO="dts:eng:6,ac3:fre:6": the file's audio tracks (codec:language:channels); SB_VID_NODTS=1: DTS and TrueHD
   tracks answer as the TV's error 200 does (picture on, no sound) */
int video_audio_tracks(VidTrack *out, int max) {
  const char *e = getenv("SB_VID_AUDIO");
  if (!e) { if (max < 2) return 0; memset(out, 0, 2 * sizeof *out); snprintf(out[0].lang, 16, "eng"); snprintf(out[0].name, 64, "English"); out[0].idx = 0; snprintf(out[1].lang, 16, "fre"); snprintf(out[1].name, 64, "French"); out[1].idx = 1; return 2; }
  if (st == VID_IDLE || st == VID_LOADING) return 0; /* the TV lists them once the stream is open */
  int n = 0; while (*e && n < max) { VidTrack *t = &out[n]; memset(t, 0, sizeof *t); int ch = 2;
    sscanf(e, "%15[^:,]:%15[^:,]:%d", t->codec, t->lang, &ch); t->channels = ch; snprintf(t->name, sizeof t->name, "%s %s", t->codec, ch == 6 ? "5.1" : ch == 8 ? "7.1" : "2.0"); t->idx = n++;
    const char *c = strchr(e, ','); if (!c) break; e = c + 1; }
  return n;
}
int video_audio_current(void) { return cur; }
void video_select_audio(int idx) { cur = idx; }
int video_text_tracks(VidTrack *out, int max) {
  if (!getenv("SB_VID_SUBS") || !live || max < 2) return 0; memset(out, 0, 2 * sizeof *out);
  snprintf(out[0].lang, 16, "eng"); snprintf(out[0].name, 64, "English"); out[0].idx = 0; snprintf(out[1].lang, 16, "fre"); snprintf(out[1].name, 64, "French"); out[1].idx = 1; return 2; }
void video_select_text(int idx) { (void)idx; }
void video_set_window(float x, float y, float w, float h) { static char last[96]; char t[96]; snprintf(t, sizeof t, "%.0f,%.0f %.0fx%.0f", x, y, w, h); if (strcmp(t, last)) { snprintf(last, sizeof last, "%s", t); fprintf(stderr, "[video] window %s\n", t); } }
void video_pump(void) {
  double now = SDL_GetTicks() / 1000.0;
  if (st == VID_LOADING && fails && now >= t0) { st = VID_ERROR; return; }
  if (st == VID_LOADING && now >= t0) { static int first = 1; double s0 = first && getenv("SB_VID_START") ? atof(getenv("SB_VID_START")) : 0; first = 0; st = VID_PLAYING; live = 1; t0 = now - s0; pos = s0; }
  if (st == VID_PLAYING) { pos = now - t0; if (pos >= dur) { pos = dur; st = VID_ENDED; } }
}

int video_partial_plane(void) { return 1; }
int video_set_crop(int sx, int sy, int sw, int sh, float dx, float dy, float dw, float dh) { fprintf(stderr, "[video] crop %d,%d %dx%d -> %.0f,%.0f %.0fx%.0f\n", sx, sy, sw, sh, dx, dy, dw, dh); return 1; }
void video_set_surface(int w, int h) { (void)w; (void)h; }
int video_audio_unsupported(void) {
  if (!getenv("SB_VID_NODTS") || st != VID_PLAYING || pos < 1) return 0;
  VidTrack t[16]; int n = video_audio_tracks(t, 16); return cur < n && (strstr(t[cur].codec, "dts") || strstr(t[cur].codec, "truehd"));
}

void video_render(void) {}
