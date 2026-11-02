/* webOS: the TV's own media pipeline (com.webos.media over luna-service2), the picture on the
   video plane under the app's transparent window. The libraries are opened at run time, so the
   binary has no hard dependency on them and the same build runs on every webOS version:
   webOS 3/4 bind the plane through libAcbAPI, webOS 5+ through SDL's exported window.

   What the pipeline needs, measured on the TVs (not in any public document):
   - load with windowId "window_id_dummy" on the ACB path (an empty id allocates a pipeline that never
     fetches), then notifyForeground, subscribe, and a plain play once the load answers;
   - the ACB told about each new media in steps from its own thread with pauses (every AcbAPI call is
     answered over the bus; sent back to back the video data arrives before the registration and the
     plane stays black with sound), with connectDass and setMediaVideoData (its JSON must carry the
     mediaId as "context" and the stream's real size/rate from videoInfo);
   - AcbAPI_initialize needs a real callback: the first ACB event jumps to it;
   - the plane's rectangle must stay inside the screen (a larger one blanks it): zoom is a source crop;
   - the rectangle is read in the drawable's pixels, not in 1920x1080 units; on a webOS 4.4 set with a 1920x1080
     drawable the TV still reports its full-screen window as 1280x720 and a crop to 0,0 1920x809 played sound over
     black: a partial rectangle is only safe where its space is known (the exported window), so the background
     trailer takes the whole screen on the ACB path;
   - "com.webos.pipeline.<id> is not running" (seen about a minute after some seeks) means the
     pipeline died: the same source is loaded again at the same moment with the same tracks;
   - errorCode 200 is "Audio Codec Not Supported": the picture goes on;
   - the TV's screen saver does not count the GL-composed video as video: its request is answered
     "no" while a film plays. */
#include "core/video.h"
#include "platform/plat.h"
#include <SDL2/SDL.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifndef SB_APP_ID
#define SB_APP_ID "tv.stremioorigin.app"
#endif

typedef struct LSHandle LSHandle; typedef struct LSMessage LSMessage;
typedef struct { void *pad0; int error_code; char *message; const char *file; int line; const char *func; char pad[64]; } LSErrorX;
typedef bool (*LSFilterFunc)(LSHandle *sh, LSMessage *reply, void *ctx);
static bool (*lsRegister)(const char *, LSHandle **, void *); static bool (*lsAttach)(LSHandle *, void *, void *);
static bool (*lsCall)(LSHandle *, const char *, const char *, LSFilterFunc, void *, unsigned long *, void *);
static bool (*lsErrInit)(void *); static void (*lsErrFree)(void *); static const char *(*lsPayload)(LSMessage *);
static void *(*gLoopNew)(void *, int); static void (*gLoopRun)(void *);
static long (*acbCreate)(void); static bool (*acbInit)(long, int, const char *, void *); static bool (*acbSink)(long, int); static bool (*acbMedia)(long, const char *);
static int (*acbState)(long, int, int, long *); static int (*acbWindow)(long, long, long, long, long, int, long *);
static int (*acbCustomWindow)(long, long, long, long, long, long, long, long, long, int, long *);
static int (*acbConnect)(long, int, long *); static int (*acbVideoData)(long, const char *, long *); static int (*acbAudioData)(long, const char *, long *);
static const char *(*sdlExpCreate)(int); static SDL_bool (*sdlExpSet)(const char *, SDL_Rect *, SDL_Rect *); static SDL_bool (*sdlExpCrop)(const char *, SDL_Rect *, SDL_Rect *, SDL_Rect *);

static LSHandle *bus; static void *loop; static long acb; static char expWin[64]; static int ready;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static volatile unsigned session;
/* shared with the glib thread (under mu) */
static struct {
  char mediaId[64]; VidState st; int live, loaded, ended, error, playing, pausedByUs, audioUnsup;
  char err[160]; double pos, dur, bufEnd, fps; int w, h, rate; long bits; char hdr[24], scan[24];
  VidTrack aud[16]; int naud; VidTrack txt[24]; int ntxt; int curAud, curTxt;
  int bindWanted, died;
} S;
static char url[8192];
static struct { int on; char base[4096]; double off, dur; } timed; /* video_load_timed: seeks are loads, times offset */
/* the requested plane, in layout units; the crop pair (-1 = none) */
static int winX, winY, winW = 1920, winH = 1080; static int cropSX = -1, cropSY, cropSW, cropSH, cropDX, cropDY, cropDW, cropDH; static int noCrop;
static int surfW = 1920, surfH = 1080;
/* the main thread's own (seek debounce, recovery) */
static int seekWait; static Uint32 seekWaitAt; /* a seek sent, its seekDone not yet seen */
static double seekTarget = -1; static Uint32 seekAt; static int seekRefusedTries; static Uint32 seekRetryAt;
static struct { int on; double at; int aud, txt; } resume;

static void log_payload(const char *what, const char *p) { if (!p) return; size_t n = strlen(p); while (n && (p[n - 1] == '\n' || p[n - 1] == ' ')) n--; fprintf(stderr, "[video] %s %.*s\n", what, (int)(n > 600 ? 600 : n), p); }
static bool log_reply(LSHandle *h, LSMessage *m, void *u) { (void)h; (void)u; log_payload("reply", lsPayload(m)); return true; }
static int call_uri(const char *uri, const char *payload, LSFilterFunc cb, void *ctx) {
  if (!bus) return 0; char err[256]; memset(err, 0, sizeof err); if (lsErrInit) lsErrInit(err); unsigned long tok = 0;
  int ok = lsCall(bus, uri, payload, cb ? cb : log_reply, ctx, &tok, err);
  if (!ok) fprintf(stderr, "[video] %s failed: %s\n", uri, ((LSErrorX *)err)->message ? ((LSErrorX *)err)->message : "?");
  if (lsErrFree) lsErrFree(err); return ok;
}
static int call(const char *method, const char *payload, LSFilterFunc cb, void *ctx) { char uri[128]; snprintf(uri, sizeof uri, "luna://com.webos.media/%s", method); return call_uri(uri, payload, cb, ctx); }
/* the first value of key that is a number ({"currentTime":{"currentTime":8580}}: the outer one is an object) */
static double num_of(const char *p, const char *key) {
  size_t n = strlen(key); const char *q = p;
  while ((q = strstr(q, key))) { const char *v = q + n; while (*v == ' ') v++; if ((*v >= '0' && *v <= '9') || *v == '-' || *v == '.') return atof(v); q += n; }
  return -1;
}
static double num_in(const char *p, const char *end, const char *key) { /* the same, only inside [p, end] */
  size_t n = strlen(key); const char *q = p;
  while ((q = strstr(q, key)) && q <= end) { const char *v = q + n; while (*v == ' ') v++; if ((*v >= '0' && *v <= '9') || *v == '-' || *v == '.') return atof(v); q += n; }
  return -1;
}
static void str_in(const char *p, const char *end, const char *key, char *out, size_t cap) {
  out[0] = 0; const char *q = strstr(p, key); if (!q || (end && q > end)) return; q += strlen(key); size_t i = 0; while (*q && *q != '"' && i + 1 < cap) out[i++] = *q++; out[i] = 0;
}
static const char *obj_end(const char *o) { int d = 0, inStr = 0; for (; *o; o++) { if (inStr) { if (*o == '\\' && o[1]) o++; else if (*o == '"') inStr = 0; continue; } if (*o == '"') inStr = 1; else if (*o == '{') d++; else if (*o == '}') { if (--d == 0) return o; } } return NULL; }
static SDL_Rect to_surface(int x, int y, int w, int h) { SDL_Rect r = { x * surfW / 1920, y * surfH / 1080, w * surfW / 1920, h * surfH / 1080 }; return r; }
/* the ACB path (webOS 3/4) reads a partial rectangle in the TV's own video space, 1280x720 on the sets measured, not in
   the drawable's pixels: a 1920-wide rectangle there falls outside it and the picture blanks while the sound plays.
   A full-screen one is flagged as such and is not affected. SB_PLANE=WxH overrides (development) */
static SDL_Rect to_plane(int x, int y, int w, int h) {
  static int pw, ph; if (!pw) { const char *e = getenv("SB_PLANE"); if (!e || sscanf(e, "%dx%d", &pw, &ph) != 2 || pw <= 0 || ph <= 0) { pw = 1280; ph = 720; } fprintf(stderr, "[video] partial rectangles in a %dx%d video space\n", pw, ph); }
  SDL_Rect r = { x * pw / 1920, y * ph / 1080, w * pw / 1920, h * ph / 1080 }; return r;
}
static int screensaverHeld(void) { return S.mediaId[0] && S.playing && !S.pausedByUs && !S.ended && !S.error; }

/* ---- the video plane on webOS 3/4 ---- */
static void acb_callback(long h, long task, long event, long appState, long playState, int reply) { (void)h; (void)task; fprintf(stderr, "[video] acb event=%ld app=%ld play=%ld reply=%d\n", event, appState, playState, reply); }
static void video_data_json(char *vd, size_t n, const char *mid) {
  int w, h, rate; long bits; char hdr[24], scan[24];
  pthread_mutex_lock(&mu); w = S.w ? S.w : 1920; h = S.h ? S.h : 1080; rate = S.rate ? S.rate : 24; bits = S.bits; snprintf(hdr, sizeof hdr, "%s", S.hdr[0] ? S.hdr : "none"); snprintf(scan, sizeof scan, "%s", S.scan); pthread_mutex_unlock(&mu);
  if (strcasecmp(hdr, "HDR10") && strcasecmp(hdr, "DolbyVision")) snprintf(hdr, sizeof hdr, "none"); /* SDR keeps the values that always played */
  snprintf(vd, n, "{\"content\":\"movie\",\"context\":\"%s\",\"video\":{\"adaptive\":false,\"bitRate\":%ld,\"data3D\":{\"currentPattern\":\"2d\",\"originalPattern\":\"2d\",\"typeLR\":\"LR\"},"
    "\"frameRate\":%d.0,\"hdrType\":\"%s\",\"height\":%d,\"width\":%d,"
    "\"mediaSei\":{\"displayPrimariesX0\":0,\"displayPrimariesX1\":0,\"displayPrimariesX2\":0,\"displayPrimariesY0\":0,\"displayPrimariesY1\":0,\"displayPrimariesY2\":0,\"maxContentLightLevel\":0,\"maxDisplayMasteringLuminance\":0,\"maxPicAverageLightLevel\":0,\"minDisplayMasteringLuminance\":0,\"whitePointX\":0,\"whitePointY\":0},"
    "\"mediaVui\":{\"colorPrimaries\":2,\"matrixCoeffs\":2,\"transferCharacteristics\":2,\"videoFullRangeFlag\":false},"
    "\"hfr\":false,\"path\":\"network\",\"pixelAspectRatio\":{\"height\":1,\"width\":1},\"rotation\":\"0\",\"scanType\":\"%s\",\"specificRendering\":\"none\"}}",
    mid, bits > 0 ? bits : 8000000L, rate, hdr, h, w, strstr(scan, "inter") ? "VIDEO_INTERLACED" : "VIDEO_PROGRESSIVE");
}
static void sleep_ms(int ms) { struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&t, NULL); }
static void *bind_plane(void *u) {
  unsigned mine = (unsigned)(uintptr_t)u; char mid[64]; pthread_mutex_lock(&mu); snprintf(mid, sizeof mid, "%s", S.mediaId); pthread_mutex_unlock(&mu); long task = 0;
#define ALIVE() (session == mine && acb)
  fprintf(stderr, "[video] bind %s\n", mid);
  if (!ALIVE()) return NULL; fprintf(stderr, "[video] bind mediaId=%d\n", acbMedia(acb, mid)); sleep_ms(200);
  if (!ALIVE()) return NULL; fprintf(stderr, "[video] bind loaded=%d\n", acbState(acb, 1, 1, &task)); sleep_ms(200);
  if (!ALIVE()) return NULL; fprintf(stderr, "[video] bind connect=%d\n", acbConnect(acb, 0, &task)); sleep_ms(200);
  if (!ALIVE()) return NULL;
  { char vd[2048]; video_data_json(vd, sizeof vd, mid); fprintf(stderr, "[video] bind videoData=%d\n", acbVideoData(acb, vd, &task));
    if (acbAudioData) { char ad[160]; snprintf(ad, sizeof ad, "{\"context\":\"%s\",\"audio\":{\"immersive\":\"none\"}}", mid); fprintf(stderr, "[video] bind audioData=%d\n", acbAudioData(acb, ad, &task)); } }
  sleep_ms(300);
  if (!ALIVE()) return NULL;
  if (cropSX >= 0 && acbCustomWindow) { SDL_Rect d = to_plane(cropDX, cropDY, cropDW, cropDH); fprintf(stderr, "[video] bind crop=%d\n", acbCustomWindow(acb, cropSX, cropSY, cropSW, cropSH, d.x, d.y, d.w, d.h, cropDX == 0 && cropDY == 0 && cropDW == 1920 && cropDH == 1080, &task)); }
  else { SDL_Rect d = to_plane(winX, winY, winW, winH); fprintf(stderr, "[video] bind window=%d\n", acbWindow(acb, d.x, d.y, d.w, d.h, winX == 0 && winY == 0 && winW == 1920 && winH == 1080, &task)); }
  if (!ALIVE()) return NULL; fprintf(stderr, "[video] bind playing=%d\n", acbState(acb, 1, S.pausedByUs ? 3 : 2, &task));
#undef ALIVE
  return NULL;
}
static void apply_plane(void) { /* the window or the crop, now (after the bind, or on the "playing" event) */
  long task = 0;
  if (expWin[0]) {
    SDL_Rect org = { 0, 0, S.w > 0 ? S.w : 1920, S.h > 0 ? S.h : 1080 };
    if (cropSX >= 0 && sdlExpCrop) { SDL_Rect src = { cropSX, cropSY, cropSW, cropSH }, dst = to_surface(cropDX, cropDY, cropDW, cropDH); if (sdlExpCrop(expWin, &org, &src, &dst)) return; noCrop = 1; cropSX = -1; }
    SDL_Rect dst = to_surface(winX, winY, winW, winH); if (sdlExpSet) sdlExpSet(expWin, &org, &dst); /* the whole frame as the source, never NULL (some TVs drop the Wayland connection) */
    return;
  }
  if (!acb || !S.mediaId[0]) return;
  if (cropSX >= 0 && acbCustomWindow) { SDL_Rect d = to_plane(cropDX, cropDY, cropDW, cropDH); int r = acbCustomWindow(acb, cropSX, cropSY, cropSW, cropSH, d.x, d.y, d.w, d.h, cropDX == 0 && cropDY == 0 && cropDW == 1920 && cropDH == 1080, &task);
    fprintf(stderr, "[video] plane crop %d,%d %dx%d -> %d,%d %dx%d: %d\n", cropSX, cropSY, cropSW, cropSH, d.x, d.y, d.w, d.h, r); if (r) return; noCrop = 1; cropSX = -1; }
  SDL_Rect d = to_plane(winX, winY, winW, winH); int r = acbWindow(acb, d.x, d.y, d.w, d.h, winX == 0 && winY == 0 && winW == 1920 && winH == 1080, &task);
  fprintf(stderr, "[video] plane window %d,%d %dx%d: %d\n", d.x, d.y, d.w, d.h, r);
}

/* ---- the pipeline's events ---- */
static void parse_tracks(const char *p) {
  S.naud = 0; S.ntxt = 0;
  const char *a = strstr(p, "\"audioTrackInfo\"");
  if (a) { const char *o = strchr(a, '['); while (o && *o && S.naud < 16) { o++; while (*o == ' ' || *o == ',' || *o == '\n') o++; if (*o != '{') break; const char *e = obj_end(o); if (!e) break;
      VidTrack *t = &S.aud[S.naud]; memset(t, 0, sizeof *t); str_in(o, e, "\"language\":\"", t->lang, sizeof t->lang); if (!strcmp(t->lang, "(null)")) t->lang[0] = 0;
      char codec[16]; str_in(o, e, "\"codec\":\"", codec, sizeof codec); char imm[16]; str_in(o, e, "\"immersive\":\"", imm, sizeof imm);
      double ch = num_in(o, e, "\"channels\":");
      snprintf(t->name, sizeof t->name, "%s%s%s", codec, ch == 6 ? " 5.1" : ch == 8 ? " 7.1" : ch == 2 ? " 2.0" : "", strcasecmp(imm, "ATMOS") ? "" : " Atmos");
      snprintf(t->codec, sizeof t->codec, "%s", codec); t->channels = ch > 0 ? (int)ch : 0;
      t->idx = S.naud; S.naud++; o = e; } }
  const char *s = strstr(p, "\"subtitleTrackInfo\"");
  if (s) { const char *o = strchr(s, '['); while (o && *o && S.ntxt < 24) { o++; while (*o == ' ' || *o == ',' || *o == '\n') o++; if (*o != '{') break; const char *e = obj_end(o); if (!e) break;
      VidTrack *t = &S.txt[S.ntxt]; memset(t, 0, sizeof *t); str_in(o, e, "\"language\":\"", t->lang, sizeof t->lang); if (!strcmp(t->lang, "(null)")) t->lang[0] = 0; /* this pipeline never names them: the file's own tracks do (mkvsubs) */
      double tn = num_in(o, e, "\"trackNum\":"); t->idx = tn >= 0 ? (int)tn : S.ntxt;
      S.ntxt++; o = e; } }
  fprintf(stderr, "[video] tracks: audio %d, subtitles %d\n", S.naud, S.ntxt);
}
static double posAt; /* (s, SDL clock) when S.pos was last set; under mu */
/* the position stands still from a seek or a buffering start until the picture runs again: the TV's own time has moved
   on since (reported twice, differently), with nothing pending and no buffering; under mu */
static int hold; static double lastCT = -1, holdAt;
static void hold_on(void) { hold = 1; lastCT = -1; holdAt = SDL_GetTicks() / 1000.0; }
static double pos_now(void);
static bool on_event(LSHandle *h, LSMessage *m, void *ctx) {
  (void)h; const char *p = lsPayload(m); if (!p) return true; if ((unsigned)(uintptr_t)ctx != session) return true;
  { static Uint32 lastClock; int clock = strstr(p, "\"currentTime\"") || strstr(p, "\"bufferRange\"") || strstr(p, "streamingInfo") || strstr(p, "subtitlePosition");
    if (!clock || SDL_GetTicks() - lastClock > 30000 || strstr(p, "rror\"")) { if (clock) lastClock = SDL_GetTicks(); log_payload("ev", p); } } /* the clock events come every ~200 ms: one of them every 30 s in the log */
  pthread_mutex_lock(&mu);
  if (strstr(p, "sourceInfo")) parse_tracks(p);
  if (strstr(p, "videoInfo")) { double v;
    if ((v = num_of(p, "\"width\":")) > 0) S.w = (int)v; if ((v = num_of(p, "\"height\":")) > 0) S.h = (int)v;
    if ((v = num_of(p, "\"frameRate\":")) > 0) { S.rate = (int)v; S.fps = v; } if ((v = num_of(p, "\"bitRate\":")) > 0) S.bits = (long)v;
    str_in(p, NULL, "\"hdrType\":\"", S.hdr, sizeof S.hdr); str_in(p, NULL, "\"scanType\":\"", S.scan, sizeof S.scan); }
  if (strstr(p, "loadCompleted")) { S.loaded = 1; if (acb) S.bindWanted = 1; }
  if (strstr(p, "bufferRange")) { double e = num_of(p, "\"endTime\":"); if (e >= 0) S.bufEnd = e + (timed.on ? timed.off : 0); }
  if (strstr(p, "bufferingStart")) { if (S.st == VID_PLAYING || S.st == VID_LOADING) S.st = VID_BUFFERING; if (!hold) { S.pos = pos_now(); posAt = SDL_GetTicks() / 1000.0; hold_on(); } }
  if (strstr(p, "bufferingEnd")) { if (S.st == VID_BUFFERING) S.st = S.pausedByUs ? VID_PAUSED : VID_PLAYING; }
  if (strstr(p, "\"playing\"")) { S.pos = pos_now(); posAt = SDL_GetTicks() / 1000.0; S.playing = 1; S.live = 1; S.st = VID_PLAYING; if (acb || expWin[0]) S.bindWanted |= 2; } /* the plane's rectangle again with the stream running */
  if (strstr(p, "\"paused\"")) { S.pos = pos_now(); posAt = SDL_GetTicks() / 1000.0; S.playing = 0; if (S.pausedByUs) S.st = VID_PAUSED; }
  if (strstr(p, "endOfStream")) { S.playing = 0; S.ended = 1; S.st = VID_ENDED; }
  if (strstr(p, "seekDone")) { seekRefusedTries = 0; seekWait = 0; }
  if (strstr(p, "errorText") && !strstr(p, "\"No Error\"")) {
    double code = num_of(p, "\"errorCode\":"); char txt[96]; str_in(p, NULL, "\"errorText\":\"", txt, sizeof txt);
    if (code == 200) S.audioUnsup = 1;                                        /* the picture goes on without this audio */
    else if (strstr(p, "is not running")) S.died = 1;                         /* the pipeline died: loaded again by video_pump */
    else if (strstr(p, "seek Failure") || code == 108 || code == 107) { if (seekRefusedTries < 3) { seekRefusedTries++; seekRetryAt = SDL_GetTicks() + 800u * seekRefusedTries; } }
    else if (code != 0) { size_t l = S.error ? strlen(S.err) : 0; if (l < sizeof S.err - 8) snprintf(S.err + l, sizeof S.err - l, "%s%.0f %s", l ? "; " : "", code, txt); S.error = 1; S.playing = 0; S.st = VID_ERROR; } /* every code of the failure ("202 Media Not Found; 100 Playing error; 203 ..."): the player reads the reason from them */
  }
  { double v = num_of(p, "\"currentTime\":"); int wait = seekWait && SDL_GetTicks() - seekWaitAt < 8000; /* (a seek on its way: the old place may still be reported) */
    if (v >= 0 && seekTarget < 0 && !wait) { v /= 1000.0;
      if (hold && S.st != VID_BUFFERING && lastCT >= 0 && v - lastCT > 0.05 && v - lastCT < 2) hold = 0; /* the picture runs again (moving on as playback does, not a jump) */
      if (timed.on) v += timed.off;
      lastCT = v; S.pos = v; posAt = SDL_GetTicks() / 1000.0; } }
  { double v = num_of(p, "\"duration\":"); if (timed.on) S.dur = timed.dur; else if (v > 0) S.dur = v / 1000.0; }
  pthread_mutex_unlock(&mu);
  return true;
}
static void start_load(void);
static int txtPending = -2;         /* a subtitle choice made before the pipeline had loaded: sent once it has (-2 none) */
static bool on_load(LSHandle *h, LSMessage *m, void *ctx) {
  (void)h; const char *p = lsPayload(m); log_payload("load", p);
  char mid[64] = ""; if (p) str_in(p, NULL, "\"mediaId\":\"", mid, sizeof mid);
  if ((unsigned)(uintptr_t)ctx != session) { if (mid[0]) { char b[128]; snprintf(b, sizeof b, "{\"mediaId\":\"%s\"}", mid); call("unload", b, NULL, NULL); } return true; } /* a cancelled load still made a pipeline: free the decoder */
  if (!mid[0]) { pthread_mutex_lock(&mu); S.error = 1; S.st = VID_ERROR; snprintf(S.err, sizeof S.err, "load failed"); pthread_mutex_unlock(&mu); return true; }
  pthread_mutex_lock(&mu); snprintf(S.mediaId, sizeof S.mediaId, "%s", mid); pthread_mutex_unlock(&mu);
  char b[128];
  snprintf(b, sizeof b, "{\"connectionId\":\"%s\"}", mid); call("notifyForeground", b, NULL, NULL);
  snprintf(b, sizeof b, "{\"mediaId\":\"%s\"}", mid); call("subscribe", b, on_event, ctx);
  snprintf(b, sizeof b, "{\"mediaId\":\"%s\"}", mid); call("play", b, NULL, NULL); /* the pipeline picks its default tracks itself (choosing before sourceInfo can stall it) */
  return true;
}
/* the TV asks before its screen saver starts; the answer goes back with the request's own timestamp */
static int ss_answer(const char *p, char *out, size_t cap) {
  if (!p || !strstr(p, "\"state\":\"Active\"")) return 0;
  const char *t = strstr(p, "\"timestamp\":"); if (!t) return 0; t += 12; while (*t == ' ') t++;
  char ts[64]; int k = 0; while (*t && *t != ',' && *t != '}' && k < 63) ts[k++] = *t++; ts[k] = 0; /* sent back exactly as it came */
  pthread_mutex_lock(&mu); int hold = screensaverHeld(); pthread_mutex_unlock(&mu);
  snprintf(out, cap, "{\"clientName\":\"%s\",\"ack\":%s,\"timestamp\":%s}", SB_APP_ID, hold ? "false" : "true", ts);
  fprintf(stderr, "[video] screen saver asked: %s\n", hold ? "held (a film plays)" : "let through");
  return 1;
}
static int failed(const char *p) { const char *v = p ? strstr(p, "\"returnValue\"") : NULL; if (!v) return 0; v += 13; while (*v == ' ' || *v == ':') v++; return !strncmp(v, "false", 5); }
#define SS_RESPONSE "luna://com.webos.service.tvpower/power/responseScreenSaverRequest"
#define SS_REGISTER "luna://com.webos.service.tvpower/power/registerScreenSaverRequest"
/* 1: the media handle may not reach the power service (a developer-mode role lets com.webos.media.client.*
   names talk to com.webos.media only): video_pump asks again through the app's own luna handle */
static volatile int ssRefused;
static bool on_screensaver(LSHandle *h, LSMessage *m, void *u) {
  (void)h; (void)u; const char *p = lsPayload(m);
  if (failed(p)) { if (!ssRefused) { log_payload("screen saver request refused on the media handle:", p); ssRefused = 1; } return true; }
  char b[256]; if (ss_answer(p, b, sizeof b)) call_uri(SS_RESPONSE, b, NULL, NULL);
  return true;
}
static void on_screensaver_app(const char *p, void *ud) { /* main thread, from plat_luna_pump */
  (void)ud; if (failed(p)) { log_payload("screen saver request refused:", p); return; }
  char b[256]; if (ss_answer(p, b, sizeof b)) plat_luna_send(SS_RESPONSE, b);
}

static void *loop_thread(void *u) { (void)u; gLoopRun(loop); return NULL; }
#define SYM(lib, var, name) do { *(void **)(&var) = dlsym(lib, name); if (!var) { fprintf(stderr, "[video] no %s\n", name); return 0; } } while (0)
int video_init(const char *ignored) {
  (void)ignored; if (ready) return 1;
  void *L = dlopen("libluna-service2.so.3", RTLD_NOW); if (!L) L = dlopen("libluna-service2.so", RTLD_NOW);
  void *G = dlopen("libglib-2.0.so.0", RTLD_NOW);
  if (!L || !G) { fprintf(stderr, "[video] luna/glib not found\n"); return 0; }
  SYM(L, lsRegister, "LSRegister"); SYM(L, lsAttach, "LSGmainAttach"); SYM(L, lsCall, "LSCall"); SYM(L, lsPayload, "LSMessageGetPayload");
  *(void **)(&lsErrInit) = dlsym(L, "LSErrorInit"); *(void **)(&lsErrFree) = dlsym(L, "LSErrorFree");
  SYM(G, gLoopNew, "g_main_loop_new"); SYM(G, gLoopRun, "g_main_loop_run");
  void *A = dlopen("libAcbAPI.so.1", RTLD_NOW);
  if (A) {
    SYM(A, acbCreate, "AcbAPI_create"); SYM(A, acbInit, "AcbAPI_initialize"); SYM(A, acbSink, "AcbAPI_setSinkType"); SYM(A, acbMedia, "AcbAPI_setMediaId");
    SYM(A, acbState, "AcbAPI_setState"); SYM(A, acbWindow, "AcbAPI_setDisplayWindow"); SYM(A, acbConnect, "AcbAPI_connectDass"); SYM(A, acbVideoData, "AcbAPI_setMediaVideoData");
    *(void **)(&acbCustomWindow) = dlsym(A, "AcbAPI_setCustomDisplayWindow");   /* not on every firmware: then no zoom, never no picture */
    *(void **)(&acbAudioData) = dlsym(A, "AcbAPI_setMediaAudioData");          /* missing on webOS 3.4 */
    acb = acbCreate();
    if (!acb || !acbInit(acb, 0, SB_APP_ID, (void *)acb_callback)) { fprintf(stderr, "[video] ACB initialise failed\n"); acb = 0; } /* player type 0 and sink 0: 10/1 sent the picture to the PIP plane */
    else acbSink(acb, 0);
  } else { /* webOS 5+: the exported window (dlopen(NULL): the SDL already loaded, not a second copy of it) */
    void *me = dlopen(NULL, RTLD_NOW);
    *(void **)(&sdlExpCreate) = dlsym(me, "SDL_webOSCreateExportedWindow"); *(void **)(&sdlExpSet) = dlsym(me, "SDL_webOSSetExportedWindow"); *(void **)(&sdlExpCrop) = dlsym(me, "SDL_webOSExportedSetCropRegion");
    if (sdlExpCreate) { const char *id = sdlExpCreate(0); if (id && *id) snprintf(expWin, sizeof expWin, "%s", id); }
    fprintf(stderr, "[video] no libAcbAPI: exported window %s\n", expWin[0] ? expWin : "(none)");
    if (!expWin[0]) return 0;
  }
  /* the name must match the app's LS2 role (com.webos.media.client.*); after a crash the hub can hold the
     plain name a while: the pid variant */
  char name[192]; snprintf(name, sizeof name, "com.webos.media.client.%s", SB_APP_ID); char err[256]; memset(err, 0, sizeof err); if (lsErrInit) lsErrInit(err);
  if (!lsRegister(name, &bus, err)) {
    if (lsErrFree) lsErrFree(err); memset(err, 0, sizeof err); if (lsErrInit) lsErrInit(err);
    snprintf(name, sizeof name, "com.webos.media.client.%s.%d", SB_APP_ID, (int)getpid());
    if (!lsRegister(name, &bus, err)) { fprintf(stderr, "[video] LSRegister refused: %s\n", ((LSErrorX *)err)->message ? ((LSErrorX *)err)->message : "?"); if (lsErrFree) lsErrFree(err); bus = NULL; return 0; }
  }
  if (lsErrFree) lsErrFree(err);
  loop = gLoopNew(NULL, 0); memset(err, 0, sizeof err); if (lsErrInit) lsErrInit(err);
  if (!lsAttach(bus, loop, err)) { fprintf(stderr, "[video] attach failed\n"); if (lsErrFree) lsErrFree(err); return 0; }
  if (lsErrFree) lsErrFree(err);
  pthread_t th; if (pthread_create(&th, NULL, loop_thread, NULL)) return 0; pthread_detach(th);
  ready = 1;
  { char b[160]; snprintf(b, sizeof b, "{\"subscribe\":true,\"clientName\":\"%s\"}", SB_APP_ID); if (!call_uri(SS_REGISTER, b, on_screensaver, NULL)) ssRefused = 1; }
  fprintf(stderr, "[video] ready as %s (%s)\n", name, acb ? "ACB plane" : "exported window");
  return 1;
}
static void stop_session(void) {
  pthread_mutex_lock(&mu); char mid[64]; snprintf(mid, sizeof mid, "%s", S.mediaId); session++; memset(&S, 0, sizeof S); S.st = VID_IDLE; S.curTxt = -1; pthread_mutex_unlock(&mu);
  seekTarget = -1; seekRefusedTries = 0; seekRetryAt = 0; seekWait = 0; hold = 0; lastCT = -1;
  if (mid[0]) { char b[128]; snprintf(b, sizeof b, "{\"mediaId\":\"%s\"}", mid); call("unload", b, NULL, NULL); }
}
void video_unload(void) { resume.on = 0; timed.on = 0; url[0] = 0; stop_session(); }
static void start_load(void) {
  if (!ready && !video_init(NULL)) { pthread_mutex_lock(&mu); S.st = VID_ERROR; S.error = 1; snprintf(S.err, sizeof S.err, "no media service"); pthread_mutex_unlock(&mu); return; }
  stop_session(); txtPending = -2; pthread_mutex_lock(&mu); S.st = VID_LOADING; S.curTxt = -1; if (timed.on) { S.pos = timed.off; S.dur = timed.dur; } pthread_mutex_unlock(&mu); unsigned mine = session;
  cropSX = -1; noCrop = 0;
  char payload[8192 + 512]; const char *win = expWin[0] ? expWin : "window_id_dummy";
  int n = snprintf(payload, sizeof payload, "{\"payload\":{\"option\":{\"useSeekableRanges\":true,\"appId\":\"%s\",\"bufferControl\":{\"userBufferCtrl\":false},\"windowId\":\"%s\"}},\"uri\":\"%s\",\"type\":\"media\"}", SB_APP_ID, win, url);
  if (n >= (int)sizeof payload) { pthread_mutex_lock(&mu); S.st = VID_ERROR; S.error = 1; snprintf(S.err, sizeof S.err, "address too long"); pthread_mutex_unlock(&mu); return; }
  if (expWin[0]) apply_plane(); /* the exported window gets its frame and place before the load */
  if (!call("load", payload, on_load, (void *)(uintptr_t)mine)) { pthread_mutex_lock(&mu); S.st = VID_ERROR; S.error = 1; snprintf(S.err, sizeof S.err, "no media service"); pthread_mutex_unlock(&mu); }
}
void video_load(const char *u) { resume.on = 0; timed.on = 0; snprintf(url, sizeof url, "%s", u ? u : ""); start_load(); }
void video_load_timed(const char *base, double durSec) {
  resume.on = 0; timed.on = 1; timed.off = 0; timed.dur = durSec; snprintf(timed.base, sizeof timed.base, "%s", base); snprintf(url, sizeof url, "%s?t=0", base); start_load();
}
static void simple(const char *method) { char mid[64]; pthread_mutex_lock(&mu); snprintf(mid, sizeof mid, "%s", S.mediaId); pthread_mutex_unlock(&mu); if (!mid[0]) return; char b[160]; snprintf(b, sizeof b, "{\"mediaId\":\"%s\"}", mid); call(method, b, NULL, NULL); }
void video_play(void) { pthread_mutex_lock(&mu); S.pausedByUs = 0; if (S.st == VID_PAUSED) S.st = VID_PLAYING; pthread_mutex_unlock(&mu); simple("play"); }
/* the stream's own volume (0-100), reached over easeMs (the media service's own ease: no steps of ours) */
int video_has_media(void) { pthread_mutex_lock(&mu); int h = S.mediaId[0] != 0; pthread_mutex_unlock(&mu); return h; }
void video_set_volume(int pct, int easeMs) {
  char mid[64]; pthread_mutex_lock(&mu); snprintf(mid, sizeof mid, "%s", S.mediaId); pthread_mutex_unlock(&mu); if (!mid[0]) return;
  if (pct < 0) pct = 0; if (pct > 100) pct = 100; char b[200];
  if (easeMs > 0) snprintf(b, sizeof b, "{\"mediaId\":\"%s\",\"volume\":%d,\"ease\":{\"duration\":%d,\"type\":\"Linear\"}}", mid, pct, easeMs);
  else snprintf(b, sizeof b, "{\"mediaId\":\"%s\",\"volume\":%d}", mid, pct);
  call("setVolume", b, NULL, NULL);
}
void video_pause(void) { pthread_mutex_lock(&mu); S.pausedByUs = 1; S.st = VID_PAUSED; pthread_mutex_unlock(&mu); simple("pause"); }
/* presses add up: the shown position moves at once, the pipeline gets one seek 350 ms after the last (four
   seeks in under a second were followed by a dead pipeline) */
void video_seek(double sec) { if (sec < 0) sec = 0; pthread_mutex_lock(&mu); S.pos = sec; posAt = SDL_GetTicks() / 1000.0; hold_on(); pthread_mutex_unlock(&mu); seekTarget = sec; seekAt = SDL_GetTicks() + 350; }
static void seek_now(double sec) {
  if (timed.on) { /* the stream starts again from there, with the tracks it had */
    pthread_mutex_lock(&mu); int aud = S.curAud, txt = S.curTxt; pthread_mutex_unlock(&mu);
    timed.off = sec; snprintf(url, sizeof url, "%s?t=%.3f", timed.base, sec); fprintf(stderr, "[video] the stream again from %.0f s\n", sec);
    resume.on = 1; resume.at = 0; resume.aud = aud; resume.txt = txt; start_load(); return; }
  seekWait = 1; seekWaitAt = SDL_GetTicks(); char mid[64]; pthread_mutex_lock(&mu); snprintf(mid, sizeof mid, "%s", S.mediaId); pthread_mutex_unlock(&mu); if (!mid[0]) return; char b[160]; snprintf(b, sizeof b, "{\"mediaId\":\"%s\",\"position\":%d}", mid, (int)(sec * 1000)); call("seek", b, NULL, NULL); }
/* the TV reports the time only now and then (on some sets every few seconds): between reports it runs on with the
   clock while playing, and stands where it was when paused (a paused frame asked again lands on the right second), while
   buffering and from a seek until the picture moves again (a set that reports rarely: let go after 10 s of play) */
static double pos_now(void) {
  if (hold && S.st == VID_PLAYING && seekTarget < 0 && !seekWait && SDL_GetTicks() / 1000.0 - holdAt > 10) { hold = 0; posAt = SDL_GetTicks() / 1000.0; }
  double p = S.pos; if (S.playing && !hold && posAt > 0) { p += SDL_GetTicks() / 1000.0 - posAt; if (S.dur > 0 && p > S.dur) p = S.dur; } return p; }
double video_time(void) { pthread_mutex_lock(&mu); double p = pos_now(); pthread_mutex_unlock(&mu); return p; }
double video_duration(void) { pthread_mutex_lock(&mu); double d = S.dur; pthread_mutex_unlock(&mu); return d; }
double video_buffered_end(void) { pthread_mutex_lock(&mu); double b = S.bufEnd; pthread_mutex_unlock(&mu); return b; }
VidState video_state(void) { pthread_mutex_lock(&mu); VidState s = S.st; pthread_mutex_unlock(&mu); return s; }
const char *video_error(void) { return S.err; }
int video_live(void) { pthread_mutex_lock(&mu); int l = S.live; pthread_mutex_unlock(&mu); return l; }
int video_settling(void) { return seekTarget >= 0 || (seekWait && SDL_GetTicks() - seekWaitAt < 8000); }
int video_width(void) { return S.w; }
int video_height(void) { return S.h; }
double video_fps(void) { pthread_mutex_lock(&mu); double f = S.fps; pthread_mutex_unlock(&mu); return f; }
int video_hdr(char *out, int cap) { pthread_mutex_lock(&mu); snprintf(out, (size_t)cap, "%s", S.hdr); pthread_mutex_unlock(&mu); return out[0] != 0; }
int video_audio_unsupported(void) { return S.audioUnsup; }
int video_audio_tracks(VidTrack *out, int max) { pthread_mutex_lock(&mu); int n = S.naud < max ? S.naud : max; memcpy(out, S.aud, n * sizeof *out); pthread_mutex_unlock(&mu); return n; }
int video_audio_current(void) { return S.curAud; }
void video_select_audio(int idx) {
  char mid[64]; pthread_mutex_lock(&mu); snprintf(mid, sizeof mid, "%s", S.mediaId); if (idx != S.curAud) S.audioUnsup = 0; S.curAud = idx; pthread_mutex_unlock(&mu); if (!mid[0]) return;
  char b[192]; snprintf(b, sizeof b, "{\"mediaId\":\"%s\",\"type\":\"audio\",\"index\":%d}", mid, idx); call("selectTrack", b, NULL, NULL);
}
int video_text_tracks(VidTrack *out, int max) { pthread_mutex_lock(&mu); int n = S.ntxt < max ? S.ntxt : max; memcpy(out, S.txt, n * sizeof *out); pthread_mutex_unlock(&mu); return n; }
void video_select_text(int idx) {
  char mid[64]; pthread_mutex_lock(&mu); snprintf(mid, sizeof mid, "%s", S.mediaId); S.curTxt = idx; int loaded = S.loaded; pthread_mutex_unlock(&mu);
  if (!mid[0] || !loaded) { txtPending = idx; return; } txtPending = -2; char b[192];
  fprintf(stderr, "[video] subtitles: %s%d\n", idx < 0 ? "off" : "track ", idx < 0 ? 0 : idx);
  if (idx < 0) { snprintf(b, sizeof b, "{\"mediaId\":\"%s\",\"enable\":false}", mid); call("setSubtitleEnable", b, NULL, NULL); return; }
  snprintf(b, sizeof b, "{\"mediaId\":\"%s\",\"enable\":true}", mid); call("setSubtitleEnable", b, NULL, NULL);
  snprintf(b, sizeof b, "{\"mediaId\":\"%s\",\"type\":\"text\",\"index\":%d}", mid, idx); call("selectTrack", b, NULL, NULL);
}
void video_set_surface(int w, int h) { if (w > 0 && h > 0) { surfW = w; surfH = h; fprintf(stderr, "[video] plane rectangles in a %dx%d surface\n", w, h); } }
void video_set_window(float x, float y, float w, float h) {
  int ix = (int)x, iy = (int)y, iw = (int)w, ih = (int)h;
  if (ix < 0) { iw += ix; ix = 0; } if (iy < 0) { ih += iy; iy = 0; } if (ix + iw > 1920) iw = 1920 - ix; if (iy + ih > 1080) ih = 1080 - iy; /* inside the screen, always */
  if (iw < 1 || ih < 1) return;
  if (ix == winX && iy == winY && iw == winW && ih == winH && cropSX < 0) return;
  winX = ix; winY = iy; winW = iw; winH = ih; cropSX = -1;
  pthread_mutex_lock(&mu); int have = S.mediaId[0] != 0; pthread_mutex_unlock(&mu);
  if (have || expWin[0]) apply_plane();
}
int video_partial_plane(void) { return expWin[0] != 0; } /* the ACB path: whole screen only */
int video_set_crop(int sx, int sy, int sw, int sh, float dx, float dy, float dw, float dh) {
  if (noCrop || (!acbCustomWindow && !sdlExpCrop)) { video_set_window(dx, dy, dw, dh); return 0; }
  int ix = (int)dx, iy = (int)dy, iw = (int)dw, ih = (int)dh;
  if (ix < 0) { iw += ix; ix = 0; } if (iy < 0) { ih += iy; iy = 0; } if (ix + iw > 1920) iw = 1920 - ix; if (iy + ih > 1080) ih = 1080 - iy;
  if (sw < 2 || sh < 2 || iw < 1 || ih < 1) return 0;
  if (sx == cropSX && sy == cropSY && sw == cropSW && sh == cropSH && ix == cropDX && iy == cropDY && iw == cropDW && ih == cropDH) return 1;
  cropSX = sx; cropSY = sy; cropSW = sw; cropSH = sh; cropDX = ix; cropDY = iy; cropDW = iw; cropDH = ih; winX = ix; winY = iy; winW = iw; winH = ih;
  pthread_mutex_lock(&mu); int have = S.mediaId[0] != 0; pthread_mutex_unlock(&mu);
  if (have) apply_plane();
  return !noCrop;
}
void video_pump(void) {
  if (ssRefused == 1) { ssRefused = 2; char b[160]; snprintf(b, sizeof b, "{\"subscribe\":true,\"clientName\":\"%s\"}", SB_APP_ID);
    fprintf(stderr, "[video] screen saver requests through the app's luna handle: %s\n", plat_luna_subscribe(SS_REGISTER, b, on_screensaver_app, NULL) ? "asked" : "not possible"); }
  int bind = 0, again = 0, died = 0; double pos = 0; int aud = 0, txt = -1, loaded = 0;
  pthread_mutex_lock(&mu);
  if (S.bindWanted & 1) bind = acb != 0; if (S.bindWanted & 2) again = 1; S.bindWanted = 0;
  if (S.died) { S.died = 0; died = 1; pos = S.pos; aud = S.curAud; txt = S.curTxt; }
  loaded = S.loaded;
  pthread_mutex_unlock(&mu);
  if (bind) { pthread_t th; if (!pthread_create(&th, NULL, bind_plane, (void *)(uintptr_t)session)) pthread_detach(th); } /* every loadCompleted binds the plane again */
  else if (again) apply_plane();
  if (died && url[0]) { fprintf(stderr, "[video] the pipeline died: loading the same source again at %.0f s\n", pos); resume.on = 1; resume.at = pos; resume.aud = aud; resume.txt = txt; start_load(); return; }
  if (txtPending != -2 && loaded && !resume.on) video_select_text(txtPending);
  if (resume.on && loaded) { /* tracks first, then the position: a track change restarts the decode */
    resume.on = 0; if (resume.aud > 0) video_select_audio(resume.aud); if (resume.txt >= 0) video_select_text(resume.txt); if (resume.at > 1) { seek_now(resume.at); }
  }
  if (seekTarget >= 0 && SDL_TICKS_PASSED(SDL_GetTicks(), seekAt)) { double t = seekTarget; seekTarget = -1; seek_now(t); }
  if (seekRetryAt && SDL_TICKS_PASSED(SDL_GetTicks(), seekRetryAt)) { seekRetryAt = 0; double t = video_time(); fprintf(stderr, "[video] seek refused: again (%d)\n", seekRefusedTries); seek_now(t); }
}

void video_render(void) {}
