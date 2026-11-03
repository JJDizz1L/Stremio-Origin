#include "screens/player.h"
#include "screens/board.h"
#include "core/app.h"
#include "core/keys.h"
#include "core/i18n.h"
#include "core/json.h"
#include "core/net.h"
#include "core/store.h"
#include "core/subs.h"
#include "core/video.h"
#include "ui/shared.h"
#include "ui/gfx.h"
#include "ui/text.h"
#include "ui/img.h"
#include "ui/icon.h"
#include "ui/widgets.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>
#include <time.h>
#include "core/p2p.h"
#include "core/mkvsubs.h"
#include "core/best.h"
#include "core/trailer.h"
#include "core/tmdb.h"
#include "core/parental.h"
#include "ui/glass.h"
#include "core/rating.h"
#include "core/hproxy.h"
#include "core/addons.h"
#include "core/subsync.h"
#include "ui/asssubs.h"

/* ---- the stream's text, read for the info chips: resolution, HDR, audio ---- */
static void lower(const char *s, char *out, size_t cap) { size_t i = 0; for (; s[i] && i + 1 < cap; i++) out[i] = (char)tolower((unsigned char)s[i]); out[i] = 0; }
static int has_word(const char *s, const char *w) { const char *p = s; size_t n = strlen(w); while ((p = strstr(p, w))) { int a = p == s || !isalnum((unsigned char)p[-1]), b = !isalnum((unsigned char)p[n]); if (a && b) return 1; p += n; } return 0; }
static const char *res_of(const char *label) {
  char s[640]; lower(label, s, sizeof s);
  if (strstr(s, "2160p") || has_word(s, "4k") || has_word(s, "uhd")) return "4K"; if (strstr(s, "1440p") || has_word(s, "2k")) return "1440p"; if (strstr(s, "1080p") || has_word(s, "fhd")) return "1080p";
  if (strstr(s, "720p")) return "720p"; if (strstr(s, "576p") || strstr(s, "480p") || has_word(s, "sd")) return "SD"; return "";
}
static const char *res_of_video(int w, int h) { if (!w || !h) return ""; if (w >= 3200 || h >= 1800) return "4K"; if (w >= 2400 || h >= 1300) return "1440p"; if (w >= 1700 || h >= 950) return "1080p"; if (w >= 1150 || h >= 650) return "720p"; return "SD"; }
static void hdr_of(const char *label, char *out, size_t cap) {
  char s[640]; lower(label, s, sizeof s); for (char *c = s; *c; c++) if (*c == '.' || *c == '_') *c = ' ';
  int dv = has_word(s, "dv") || has_word(s, "dovi") || strstr(s, "dolby vision"); out[0] = 0;
  if (dv) snprintf(out, cap, "DV");
  const char *h = strstr(s, "hdr10+") || strstr(s, "hdr10 plus") || has_word(s, "hdr10p") ? "HDR10+" : has_word(s, "hdr10") ? "HDR10" : (has_word(s, "hdr") && !dv) ? "HDR" : has_word(s, "hlg") ? "HLG" : "";
  if (*h) { size_t l = strlen(out); snprintf(out + l, cap - l, "%s%s", l ? " · " : "", h); }
}
static void audio_of(const char *label, char *out, size_t cap) {
  char s[640]; lower(label, s, sizeof s); for (char *c = s; *c; c++) if (*c == '.' || *c == '_') *c = ' ';
  const char *au = "";
  if (strstr(s, "truehd")) au = "TrueHD"; else if (strstr(s, "dts-x") || strstr(s, "dts x") || strstr(s, "dtsx")) au = "DTS:X"; else if (strstr(s, "dts-hd ma") || strstr(s, "dts-hd-ma") || strstr(s, "dts hd ma") || strstr(s, "dts-ma") || strstr(s, "dtshd ma")) au = "DTS-HD MA";
  else if (strstr(s, "dts-hd") || strstr(s, "dts hd") || strstr(s, "dtshd")) au = "DTS-HD"; else if (has_word(s, "dts")) au = "DTS";
  else if (has_word(s, "ddp") || strstr(s, "dd+") || strstr(s, "e-ac-3") || strstr(s, "e-ac3") || strstr(s, "eac3") || strstr(s, "dolby digital plus")) au = "EAC3"; else if (has_word(s, "dd") || strstr(s, "ac-3") || has_word(s, "ac3") || strstr(s, "dolby digital")) au = "AC3";
  else if (has_word(s, "aac") || strstr(s, "aac2")) au = "AAC"; else if (has_word(s, "opus")) au = "Opus"; else if (has_word(s, "flac")) au = "FLAC"; else if (has_word(s, "mp3")) au = "MP3";
  char ch[8] = ""; for (const char *p = s; *p; p++) if (p[0] >= '1' && p[0] <= '9' && (p[1] == '.' || p[1] == ' ') && (p[2] == '0' || p[2] == '1') && !isdigit((unsigned char)p[3]) && (p == s || (!isdigit((unsigned char)p[-1]) && p[-1] != '.'))) { snprintf(ch, sizeof ch, "%c.%c", p[0], p[2]); break; }
  int atmos = strstr(s, "atmos") != NULL;
  if (*au && atmos) snprintf(out, cap, "%s Atmos%s%s", au, ch[0] ? " " : "", ch); else if (!*au && atmos) snprintf(out, cap, "Atmos"); else if (*au) snprintf(out, cap, "%s%s%s", au, ch[0] ? " " : "", ch); else out[0] = 0;
}
static void fmt_time(double s, char *out, size_t cap) { int t = s < 0 ? 0 : (int)s, h = t / 3600, m = (t % 3600) / 60, x = t % 60; if (h) snprintf(out, cap, "%d:%02d:%02d", h, m, x); else snprintf(out, cap, "%d:%02d", m, x); }
static const char *lang_name(const char *c) { const char *n = subs_lang_name(c); return n ? T(n) : ""; }
#define lang_match subs_lang_match

/* ---- motion: the page's CSS transitions (duration, cubic-bezier, delay), retargeted from where they are ---- */
static const float C_EASE[4] = { .25f, .1f, .25f, 1 }, C_OUT[4] = { .2f, .8f, .2f, 1 }, C_SEEK[4] = { .23f, 1, .32f, 1 }, C_TRIN[4] = { .2f, .9f, .25f, 1 },
  C_TROUT[4] = { .4f, 0, 1, 1 }, C_LGIN[4] = { .4f, 0, .2f, 1 }, C_INOUT[4] = { .42f, 0, .58f, 1 };
static float bez(const float *c, float x) {
  if (x <= 0) return 0; if (x >= 1) return 1; if (!c) return x;
  float t = x; for (int i = 0; i < 8; i++) { float u = 1 - t, b = 3 * u * u * t * c[0] + 3 * u * t * t * c[2] + t * t * t - x, d = 3 * u * u * c[0] + 6 * u * t * (c[2] - c[0]) + 3 * t * t * (1 - c[2]);
    if (fabsf(b) < 1e-4f || fabsf(d) < 1e-6f) break; t -= b / d; if (t < 0) t = 0; if (t > 1) t = 1; }
  float u = 1 - t; return 3 * u * u * t * c[1] + 3 * u * t * t * c[3] + t * t * t;
}
typedef struct { float from, to, v, t, dur; const float *c; int on; } Mo;
static void mo_set(Mo *m, float v) { m->from = m->to = m->v = v; m->on = 0; }
static void mo_to(Mo *m, float to, float dur, const float *c, float delay) {
  if (m->to == to && (m->on || m->v == to)) return;
  m->from = m->v; m->to = to; dur *= ANIM_SPEED; delay *= ANIM_SPEED; m->t = -delay; m->dur = dur > 0 ? dur : 1e-4f; m->c = c; m->on = 1; if (dur <= 0 && delay <= 0) { m->v = to; m->on = 0; }
}
static void mo_tick(Mo *m, float dt) { if (!m->on) return; m->t += dt; if (m->t < 0) return; float x = m->t / m->dur; if (x >= 1) { m->v = m->to; m->on = 0; return; } m->v = m->from + (m->to - m->from) * bez(m->c, x); }
/* a keyframe entrance started at `at` (0 = none running): 0..1 */
static float kf(double at, float dur, const float *c, float delay) { if (!at) return 1; return bez(c, (float)((app_now() - at - delay * ANIM_SPEED) / (dur * ANIM_SPEED))); }

/* ---- state ---- */
#define MAXBTN 8
#define MAXROW 64
typedef struct { int k; /* 0 note 1 skel 2 ep 3 src 4 chip; ls: a subtitle row's list, 1 OpenSubtitles 2 the addons' */ char lb[200], sub[120], ov[300], th[512], vl[48]; int on, spin, tool; int i, ls; const cJSON *v; Stream *st; char u[1024]; float d, hh, ww, vw; uint32_t sw; } TItem; /* hh, ww, vw: measured on first draw (items are made fresh, zeroed) */
typedef struct { char title[64], vl[32]; TItem *items; int n; } TCol;
static float fill_ease(double t);
typedef struct { int k; /* 0 none 1 intro 2 recap 3 outro */ int big, nbtn, idx, autoN; double t0, adur; struct { const char *id; char lb[48]; double to; int main; } b[2]; } PPrompt;
static struct {
  int open, closing, live, failed, started; double openedAt;
  char type[16], id[64], vid[80], name[256], logo[512], bg[512], poster[512], sname[600], saddon[96], url[SB_URL_MAX], alt[SB_URL_MAX]; int altTried, p2pRetried; double lastPos; int isTrailer, isHls;
  cJSON *meta; const cJSON *videos; int season, episode; char epTitle[256], year[8], genre[64];
  struct { int has; char vid[80], se[32], title[256], thumb[512], ov[600]; } next; int sim; double startAt;
  int mode; /* 0 video 1 osd 2 tray 3 prompt */ int row; /* 0 btns 1 bar */ int bIdx;
  struct { const char *id; const char *icon; char lb[48]; int big, nx; float x0, x, y, w, lw; Mo dx; } btns[MAXBTN]; int nbtn, nL;
  /* the page's classes as motions: the title block, bar and buttons, the clock, the shades, the paused label, the spinner,
     the loading line, the seek pill, the knob, the prompt, the subtitles' lift */
  Mo in, artA, ttlA, ttlX, ttlY, barA, barY, btnA, btnY, topR, topY, dim, dimT, vig, pzA, bufA, lnA, skA, skS, knA, prA, prY, prB, subB, ccS, pgA, pgY;
  struct { ParGuide g; int age, certAge, guideIn, certIn, asked; unsigned sess; double at, askAt; } pg; /* the viewer notice: an age and what for, as the picture comes, gone in 5 s */
  Mo dlyA, dlyR; int dlyF; double dlyAt; /* the delay card (mode 4): its showing, its focus ring's place, the focused button, when the value last changed */
  double hideAt; int osd;
  /* the info chips: kept per place, each with its own entrance */
  struct { char t[100], old[100]; const char *icon; int bold, sep; double at, xAt; float delay, oldW; } chip[8]; int nchip; /* old, xAt, oldW: the words before a change, crossfading */
  double seekT, seekCommitAt, lastKeyAt; int seekDir, rep, skm, skOn; char seekTxt[48]; double seekTxtUntil;
  /* the focus pill under the buttons and its name */
  Mo focX, focY, focW, focH, focA, tipA; char tip[48]; double tipAt, focFadeAt; int focSeen;
  int ldd, exitFade; char trName[128], p2pLine[160]; Mo p2pA;
  
  /* prompts (shown: the one drawn, kept while it fades out) */ PPrompt prompt, shown; int dismissed[4];
  struct { int has; double s, e; } intro, recap, outro; int skipsAsked, skipsOk;
  /* the skip sources being asked (merged by rank when they have answered) */
  struct { unsigned tok; int pending, sent, nsrc, isEp, se, en, chapWait; double quickAt, endAt, d; struct { char url[440]; int kind, tries, live; double retryAt; } job[3]; struct { int rank; struct { int has; double s, e; } g[3]; } src[4]; } sk;
  /* next episode from the closest source */
  int nextTried; struct { unsigned tok; int done, has; double bs, until; char vid[80], info[700], url[SB_URL_MAX], addon[96], binge[96]; Stream best; } nx;
  int ended; double endGoAt; double metaRetryAt;
  /* the paused screensaver, the file size, the picture size */
  double liveT0, steadyAt; /* the first frame came / playing steadily since */
  int ssOn; double ssAt, ssCheckAt, lastAct; double size; char sizeTxt[24]; unsigned sizeTok; int httpSt, sizeAns, canNext; double errAt; int lastVw, lastVh; double aspRefreshAt;
  /* the pointer: the time bubble over the bar */ int hvOn, barDown; float hvP; Mo hvA;
  int extTry, embKnown, embWaitOver; double extRetryAt, embWaitAt;
  /* the stream as its addon gave it (headers, group, file facts); the subtitles addons installed for this video, the
     language chosen in their tab */
  Stream st; int nSubAddons; char subLang[24]; unsigned ytTok;
  /* the addons' subtitles (the stream's own, then each subtitles addon's), apart from the built-in list in ext */
  SubEntry *adx; int nAdx, adxLoading, adxTry; double adxRetryAt; unsigned adxReq;
  /* tray */ struct { int kind; /* 0 none 1 eps 2 src 3 aud 4 subs 5 epsrc */ char tabs[40][48]; int ntabs, tab, seasonList[40], season; int zone; /* 0 row 1 tabs 2 cols */ TItem row[MAXROW]; int n, idx, onBtn; TCol cols[4]; int ncols, c, r[4]; char title[300], epvid[80]; int backIdx, backTab; Mo open, openA; float ox, oy; Mo rowX, tabsX, colY[4], ringX, ringY, ringA, plY[4], plA[4]; float ringW, ringH; int loading; int armed; } tray;
  /* the sources tray's lists: one per addon serving the video (in the user's order), each asked when first seen */
  struct { char vid[80]; struct { char name[96]; Stream *items; float *h; int n, st, open; unsigned tok; Mo fa, bh, ia; float bhT; int leaving, seen, touched; } g[16]; int ng, on; } srcs; /* fa: how open its list is (alpha, slide), bh: its list's height as laid out now, ia: its rows fading in as they come */
  /* subs */ SubEntry *ext; int next_; int nExt, extLoading; char curSub[1100]; struct { int list; char addon[64], lang[24]; } pick; Cue *cues; int ncues; double subDelay; VidTrack emb[16]; int nEmb; int autoSubDone; unsigned subReq; unsigned trSubTok; int locDone;
  int curAud; int autoAudDone; int didResume;
  /* sound this TV can't decode (DTS, TrueHD): when the DTS track started playing, the codecs refused on this stream (error
     200), what was said */ struct { double playAt; char bad[4][16]; int nbad, said, gaveUp, switches;
    /* the sound converted on this device (hproxy): 0 no, 1 opening, 2 playing it; the file's audio track converted, the
       moment to come back to, since when, the picture is HEVC; a first load held back until the file's tracks are read */
    int conv, convOrd, hevc; double convAt, convT0, okAt, deferUntil;
    /* the same conversion for a Dolby Vision profile 7 picture: dv the picture goes out as 8.1, copyA the audio track as
       it is, dvTried started once already, dvSeen what the TV showed was checked */
    int dv, copyA, dvTried, dvSeen; double dvAt; } dts;
  int assPlain; /* styled subtitles were too slow here: plain for the rest of this film */ double lastSave; int marked; char aspect[8]; const char *res; char infoKey[256]; int pausedShown; double pausedAt;
  Mo volA; double volAt; /* the volume pill: shown only while the volume is being changed */
  /* torrents: resolved by the engine first; the file's own subtitles and chapters from the stream itself */
  int isP2p, resolving; double posTick; int mkvTracksOk, chapWant;
  Nudge nudge; double nameAt;
} P;
static MkvSession *mkv;            /* the subtitle reader of the stream playing (outside P: P starts over per source) */
static unsigned p2pTok;
static void close_player(int goNext, int goSim);
static void show_osd(int keepFocus);
static void hide_osd(void);
static void render_tray(int fresh);
static void tray_origin(void);
static double now_(void) { return app_now(); }
/* where a looping motion (a spinner, the loading line) is in its turn: worked out in double precision, as a float clock
   steps visibly once the app has been running a while */
static float clock_ph(double period) { return (float)fmod(now_() / period, 1.0); }
int player_open(void) { return P.open; }
int player_art_full(void) { return P.open && P.artA.v > 0.98f; } /* the loading art covers the screen */
float player_cover(void) { return P.open ? P.in.v : 0; }
/* the page under the player: while it covers it only partly, and while a logo flies between them over the art
   (the page's copy is the one that flies back) */
int player_page_under(void) { return P.open && (P.in.v < 0.999f || (sx_flying() && !P.live)); }
int player_live(void) { extern int app_bg_video(void); return (P.open && P.live && !P.closing && P.artA.v < 0.02f) || app_bg_video(); } /* (or the boards' background trailer) */
static int paused(void) { return video_state() == VID_PAUSED; }
int player_paused(void) { return P.open && !P.closing && P.live && paused(); } /* (the app's screensaver may come over a paused film) */
static double dur(void) { return video_duration(); }

/* ---- the art screen: the title's logo breathing over its cover, a loading line, a status ---- */
static char status[400];
/* a line of status words: it fades and slides a few px as it comes and goes, and crossfades when its words change */
typedef struct { char cur[400], old[400]; double at; } XLine;
static XLine stX[3];
/* a light line on the loading screen, sparingly: at most one per loading, only once the real status has stayed the
   same for 4 s; it shows for 3.5 s and fades back to the status; never once the picture runs, never on a failure */
static const char *const WAIT_LINES[] = {
  "Warming up the pixels\xe2\x80\xa6", "Asking the internet nicely\xe2\x80\xa6", "Popcorn status: ready. Stream status: almost.",
  "Untangling the cables\xe2\x80\xa6", "Convincing the bits to line up\xe2\x80\xa6", "Finding the remote. Oh, you have it.",
  "Rewinding the tape\xe2\x80\xa6 just kidding", "Dimming the lights\xe2\x80\xa6", "Good things come to those who buffer",
  "Polishing the frames\xe2\x80\xa6", "Telling the actors to get ready\xe2\x80\xa6", "Silencing phones in the theatre\xe2\x80\xa6",
  "Loading the plot twist\xe2\x80\xa6", "Almost there. Probably.", "Feeding the hamsters that run the servers\xe2\x80\xa6",
  "Adjusting the volume knob for drama\xe2\x80\xa6", "Rolling the opening credits\xe2\x80\xa6", "This is the part where you grab a snack" };
#define NWAIT ((int)(sizeof WAIT_LINES / sizeof *WAIT_LINES))
static unsigned loadGen; /* bumps with each new stream (prepare) */
static int same_but_digits(const char *a, const char *b) { /* "Finding peers… 4s" and "… 5s": the same message */
  while (*a || *b) { while (*a >= '0' && *a <= '9') a++; while (*b >= '0' && *b <= '9') b++; if (*a != *b) return 0; if (*a) { a++; b++; } } return 1; }
static const char *loading_line(const char *st) {
  static unsigned seen; static int used; static double since, showAt; static char last[400]; static const char *pick; double now = app_now();
  if (seen != loadGen) { seen = loadGen; used = 0; showAt = 0; since = now; last[0] = 0; }
  if (P.live || P.failed) return st;
  if (!same_but_digits(st, last)) { snprintf(last, sizeof last, "%s", st); since = now; }
  if (showAt) { if (now - showAt < 3.5) return pick; showAt = 0; since = now; return st; }
  if (!used && now - since > 4) { used = 1; showAt = now; pick = T(WAIT_LINES[(unsigned)(now * 1000) % NWAIT]); return pick; }
  return st;
}
static void xline_draw(XLine *x, const char *t, float tx, float y, int px, uint32_t col, float maxW, int al) {
  if (strcmp(t, x->cur)) { if (same_but_digits(t, x->cur) && x->cur[0]) snprintf(x->cur, sizeof x->cur, "%s", t); /* a counter ticking: in place, no fade */
    else { snprintf(x->old, sizeof x->old, "%s", x->cur); snprintf(x->cur, sizeof x->cur, "%s", t); x->at = app_now(); } }
  float k = clampf((float)((app_now() - x->at) / 0.26), 0, 1); k = 1 - (1 - k) * (1 - k) * (1 - k);
  if (k < 1 && x->old[0]) { gfx_opacity_push(1 - k); text_draw(x->old, tx, y - 6 * k, px, 600, col, 0, maxW, al); gfx_opacity_pop(); }
  if (x->cur[0]) { if (k < 1) gfx_opacity_push(k); text_draw(x->cur, tx, y + 6 * (1 - k), px, 600, col, 0, maxW, al); if (k < 1) gfx_opacity_pop(); }
}
static void set_status(const char *t) { snprintf(status, sizeof status, "%s", t ? t : ""); }
static void fail(const char *t) { fprintf(stderr, "[player] %s | %s %s %s\n", t, P.type, P.id, P.vid); P.failed = 1; video_unload(); P.live = 0;
  P.canNext = P.st.url[0] && strcmp(P.type, "other") && !P.isTrailer; if (P.canNext) best_skip(P.st.url); /* OK picks the best of the others */
  char s[400]; snprintf(s, sizeof s, "%s  ·  %s", t, P.canNext ? T("OK: try the next stream  ·  Back: close") : T("Press Back to close")); set_status(s); }
/* why a stream did not play, in words: what its server answered, the torrent's peers, then the TV's own codes
   ("202 Media Not Found; 203 AV Type Not Founded") read with what the stream's text says it is */
static int tv_code(const char *tv, int lo, int hi) { for (const char *p = tv; p && *p; p = strstr(p, "; "), p = p ? p + 2 : NULL) { int k = atoi(p); if (k >= lo && k <= hi) return 1; } return 0; }
static const char *fail_reason(void) {
  const char *tv = video_error(); char lab[1100]; snprintf(lab, sizeof lab, "%s %s", P.sname, P.st.filename); for (char *c = lab; *c; c++) *c = (char)tolower((unsigned char)*c);
  const TvCaps *c = best_caps(); int http = !P.isP2p && !strncmp(P.url, "http", 4) && P.sizeTok;
  fprintf(stderr, "[player] why: tv \"%s\", %s %d, peers \"%s\"\n", tv, P.isP2p ? "torrent" : "http", P.httpSt, P.p2pLine);
  if (http && P.httpSt == 0) return T("The stream’s server didn’t answer");
  if (http && (P.httpSt == 401 || P.httpSt == 403)) return T("The stream’s server refused it: check your debrid account");
  if (http && (P.httpSt == 404 || P.httpSt == 410)) return T("The stream’s server can’t find this file");
  if (http && P.httpSt == 429) return T("The stream’s server is busy: try again in a moment");
  if (http && P.httpSt >= 500) return T("The stream’s server has a problem");
  if (http && P.httpSt >= 400) return T("The stream’s server didn’t send the video");
  if (P.isP2p && (!P.p2pLine[0] || atoi(P.p2pLine) < 2)) return T("Not enough peers yet");
  if (strstr(tv, "Network") || tv_code(tv, 300, 399)) return T("The connection to the stream dropped");
  int hevc = has_word(lab, "x265") || has_word(lab, "h265") || strstr(lab, "h.265") || strstr(lab, "h 265") || has_word(lab, "hevc"), ten = strstr(lab, "10bit") || strstr(lab, "10 bit") || strstr(lab, "10-bit") || has_word(lab, "hdr") || has_word(lab, "hdr10");
  char hdr[64]; best_hdr_of(P.sname, hdr, sizeof hdr); const char *res = best_res_of(P.sname);
  if (c && has_word(lab, "av1") && !c->av1) return T("This TV can’t play AV1 video");
  if (c && hevc && ten && !c->hdr) return T("This TV can’t play HEVC 10-bit video");
  if (c && strstr(hdr, "DV") && !strstr(hdr, "HDR") && !c->dv) return T("This TV can’t play Dolby Vision video");
  if (c && res && !strcmp(res, "4K") && c->maxRes && c->maxRes < 2160) return T("This TV can’t play 4K video");
  int hdrS = has_word(lab, "hdr") || has_word(lab, "hdr10") || strstr(lab, "hdr10+") || has_word(lab, "hlg");
  if (c && hdrS && !c->hdr) return T("This TV can’t show HDR video");
  if (video_audio_unsupported() || (c && ((strstr(lab, "truehd") && !c->truehd) || ((has_word(lab, "dts") || strstr(lab, "dts-hd") || strstr(lab, "dts-x")) && !c->dts))))
    if (tv_code(tv, 200, 200) || tv_code(tv, 203, 203) || video_audio_unsupported()) return T("This TV can’t play this video’s sound (DTS or TrueHD)");
  if (tv_code(tv, 201, 201) || tv_code(tv, 203, 203) || strstr(tv, "Codec")) return T("The video format isn’t supported by this TV");
  if (tv_code(tv, 202, 202)) return T("The TV couldn’t open the video");
  return T("Playback error");
}

/* ---- library / progress ---- */
/* episodes: a series, or any title whose metadata lists videos and where a video (not the title itself) plays (anime
   and channel addons give their own types: "anime", "tv"...; their ids may be "kitsu:123:5", with no season) */
static int episodic(void) {
  if (!strcmp(P.type, "series")) return 1;
  if (!strcmp(P.type, "movie") || !strcmp(P.type, "other") || !P.vid[0] || !strcmp(P.vid, P.id)) return 0;
  const cJSON *v = P.meta ? js_arr(P.meta, "videos") : NULL; return v && cJSON_GetArraySize(v) > 1;
}
static Item lib_item(void) { Item it; memset(&it, 0, sizeof it); snprintf(it.id, sizeof it.id, "%s", P.id); snprintf(it.type, sizeof it.type, "%s", P.type); snprintf(it.name, sizeof it.name, "%s", P.name); snprintf(it.poster, sizeof it.poster, "%s", P.poster); snprintf(it.background, sizeof it.background, "%s", P.bg); snprintf(it.logo, sizeof it.logo, "%s", P.logo); return it; }
static void save_progress(int push) { double d = dur(); if (!P.id[0] || P.isTrailer || d <= 0 || getenv("SB_PLAY_NOSAVE")) return; /* (SB_PLAY_NOSAVE: development plays leave the library alone) */ Item it = lib_item(); lib_set_progress(P.id, &it, P.vid, video_time() * 1000, d * 1000, push); P.lastSave = now_(); }
static void mark_watched(void) {
  if (P.isTrailer || P.marked || getenv("SB_PLAY_NOSAVE")) return; P.marked = 1; Item it = lib_item(); cJSON *l = lib_get(P.id); if (!l) l = lib_new_item(&it);
  if (episodic() && P.videos) { const char *ids[1] = { P.vid }; lib_set_eps_watched(l, P.videos, ids, 1, 1); } else lib_set_movie_watched(l, 1);
}
static char resumeMsg[80];
static void try_resume(void) {
  if (P.didResume || P.isTrailer) return; P.didResume = 1;
  if (P.startAt < 0) return; /* "Watch from the beginning" */
  if (P.startAt > 0) { video_seek(P.startAt); return; } /* another source: same moment */
  cJSON *l = lib_get(P.id); const cJSON *st = js_obj(l, "state"); if (!st) return;
  if (!strcmp(js_str(st, "video_id", ""), P.vid) && js_num(st, "timeOffset", 0) > 5000) { double sec = js_num(st, "timeOffset", 0) / 1000, d = js_num(st, "duration", 0) / 1000;
    if (!d || sec < d - 30) { video_seek(sec); char ft[32]; fmt_time(sec, ft, sizeof ft); snprintf(resumeMsg, sizeof resumeMsg, "%s%s", T("Resuming from "), ft); } } /* said as the picture comes up, not while it loads behind the art */
}

/* ---- subtitles ---- */
static const char *want_sub(void) { return store_get("subsLang", "eng"); }
static int size_idx(void) { int i = atoi(store_get("subtitleSize", "2")); return i < 0 || i > 6 ? 2 : i; }
static const int SUB_SIZES[] = { 72, 80, 100, 120, 140, 160, 180 };
static const char *SUB_COLORS[] = { "#ffffff", "#ffe600", "#00e5ff" }, *SUB_BGS[] = { "0", "1", ".6" };
static int bg_idx(void) { const char *v = store_get("subsBgStyle", "0"); for (int i = 0; i < 3; i++) if (!strcmp(SUB_BGS[i], v)) return i; return 0; }
static int color_idx(void) { const char *v = store_get("subsColor", "#ffffff"); for (int i = 0; i < 3; i++) if (!strcasecmp(SUB_COLORS[i], v)) return i; return -1; }
static uint32_t hexcol(const char *h) { return 0xff000000u | (unsigned)strtoul(h + 1, NULL, 16); }
static void refresh_tray(int kind) { if (P.tray.kind == kind) render_tray(0); }
static void sync_stop(int keepTimes);
static unsigned cueTimesGen; /* bumped when the lines' times change in place (Auto-sync): their index is made again */
/* The chosen subtitle's progress. A token per choice (an answer for an earlier choice is dropped); while its lines
   load, the tray shows a spinner on it; a file that cannot be read says so. Each choice and its first line on screen
   go to the log ([subs]), so the TV's log tells what happened. */
static struct { unsigned tok; double at, checkAt; int wait, tv, mkv, failed, shown, retry, ready; } SL;
static int embKind[16];            /* the listed embedded tracks: 1 plain text, 2 SubStation, 0 unknown (the TV's list) */
/* an embedded track's name in the lists: its own title, else its language, else "Track 3" and what it is (some files
   name and tag none of their tracks: a bare "#" said nothing) */
static void emb_label(int i, char *out, size_t cap) {
  const VidTrack *t = &P.emb[i]; const char *ln = lang_name(t->lang); char num[8]; snprintf(num, sizeof num, "%d", i + 1);
  if (t->name[0] && strcmp(t->name, "#")) snprintf(out, cap, "%s", t->name);
  else if (*ln) snprintf(out, cap, "%s", ln);
  else if (t->lang[0] && strcmp(t->lang, "und")) snprintf(out, cap, "%s", t->lang);
  else snprintf(out, cap, "%s", T1("Track {n}", "n", num));
  if (!(t->name[0] && strcmp(t->name, "#")) && i < 16 && embKind[i]) { size_t l = strlen(out); snprintf(out + l, cap - l, " · %s", embKind[i] == 2 ? "ASS" : T("Text")); } /* (the format's own short name: the column is narrow) */
}
static void sub_begin(void) { SL.tok++; SL.at = now_(); SL.checkAt = 0; SL.wait = 1; SL.tv = SL.mkv = SL.failed = SL.shown = SL.retry = SL.ready = 0; }
static void sub_reset(void) { unsigned t = SL.tok + 1; memset(&SL, 0, sizeof SL); SL.tok = t; memset(embKind, 0, sizeof embKind); }
static int tv_has_text(int n) { VidTrack t[24]; int k = video_text_tracks(t, 24); for (int i = 0; i < k; i++) if (t[i].idx == n) return 1; return 0; }
static void subs_off(void) { sync_stop(0); snprintf(P.curSub, sizeof P.curSub, "off"); subs_free(P.cues, P.ncues); P.cues = NULL; P.ncues = 0; sub_begin(); SL.wait = 0; video_select_text(-1); if (mkv) mkv_select(mkv, -1); fprintf(stderr, "[subs] off\n"); refresh_tray(4); }
static void set_ext_sub(int list, const SubEntry *e);
static void on_sub_cues(int status_, Cue *cues, int n, void *ud) {
  if (!P.open || (unsigned)(uintptr_t)ud != SL.tok || strncmp(P.curSub, "ext:", 4)) { subs_free(cues, n); return; } /* another choice since */
  if (n > 0) { subs_free(P.cues, P.ncues); P.cues = cues; P.ncues = n; SL.wait = 0; fprintf(stderr, "[subs] external: %d lines read in %.1f s\n", n, now_() - SL.at); refresh_tray(4); return; }
  subs_free(cues, n);
  if (status_ <= 0 && !SL.retry) { SL.retry = 1; fprintf(stderr, "[subs] external: no answer, asking once more\n"); subs_load(P.curSub + 4, on_sub_cues, (void *)(uintptr_t)SL.tok); return; } /* one network hiccup */
  fprintf(stderr, "[subs] external: failed (HTTP %d, %s)\n", status_, status_ >= 200 && status_ < 300 ? "nothing readable in it" : "no file");
  toast(T("Couldn’t load these subtitles"), 3000, NULL);
  snprintf(P.curSub, sizeof P.curSub, "off"); SL.wait = 0; refresh_tray(4); /* the tray tells the truth: nothing is on */
}
/* a downloaded subtitle is known by the list it came from and its identity there (the same file can be in both lists,
   or twice in one from two addons): only that row is ticked, whatever the lists become after */
static int sub_is(int list, const SubEntry *e) { return list && P.pick.list == list && !strncmp(P.curSub, "ext:", 4) && !strcmp(e->url, P.curSub + 4) && !strcmp(e->addon, P.pick.addon) && !strcmp(e->lang, P.pick.lang); }
static int sub_row(int list, const SubEntry *l, int n) { for (int i = 0; i < n; i++) if (sub_is(list, &l[i])) return i; return -1; }
static void set_ext_sub(int list, const SubEntry *e) {
  const char *u = e->url; sync_stop(0); snprintf(P.curSub, sizeof P.curSub, "ext:%s", u); P.pick.list = list; snprintf(P.pick.addon, sizeof P.pick.addon, "%s", e->addon); snprintf(P.pick.lang, sizeof P.pick.lang, "%s", e->lang); subs_free(P.cues, P.ncues); P.cues = NULL; P.ncues = 0; video_select_text(-1); if (mkv) mkv_select(mkv, -1);
  sub_begin(); fprintf(stderr, "[subs] chose external %s\n", u); subs_load(u, on_sub_cues, (void *)(uintptr_t)SL.tok); refresh_tray(4);
}
static void set_emb_sub(int n) {
  sync_stop(0); snprintf(P.curSub, sizeof P.curSub, "emb:%d", n); subs_free(P.cues, P.ncues); P.cues = NULL; P.ncues = 0; sub_begin();
  if (P.mkvTracksOk && mkv) { SL.mkv = 1; video_select_text(-1); mkv_select(mkv, n); fprintf(stderr, "[subs] chose embedded #%d: read from the file\n", n); } /* shown once its lines are read */
  else { SL.wait = 0; SL.tv = 1; video_select_text(n); fprintf(stderr, "[subs] chose embedded #%d: drawn by the TV\n", n); }
  refresh_tray(4);
}
/* the reader's progress for the chosen embedded track, a few times a second: ready (the TV lets go if it was drawing
   meanwhile), still reading (a plain text track goes to the TV after 6 s, when the TV lists it), or given up (the TV
   draws it when it can, else the viewer is told) */
static void sub_check(void) {
  if (!SL.mkv || strncmp(P.curSub, "emb:", 4) || now_() < SL.checkAt) return; SL.checkAt = now_() + .25;
  int n = atoi(P.curSub + 4), st = mkv_track_status(mkv, n), k = -1; for (int i = 0; i < P.nEmb; i++) if (P.emb[i].idx == n) k = i;
  if (st > 0 && SL.wait) { SL.wait = 0; if (SL.tv && !SL.failed) { SL.tv = 0; video_select_text(-1); } if (!SL.ready) fprintf(stderr, "[subs] embedded #%d ready in %.1f s\n", n, now_() - SL.at); SL.ready = 1; refresh_tray(4); }
  else if (!st && !SL.wait && !SL.failed) { SL.wait = 1; refresh_tray(4); } /* after a seek: reading the new spot */
  else if (st < 0 && !SL.failed) { SL.failed = 1; SL.wait = 0;
    if (tv_has_text(n)) { if (!SL.tv) video_select_text(n); SL.tv = 1; fprintf(stderr, "[subs] embedded #%d: the file can't be read here (%s): the TV draws it\n", n, mkv_error(mkv)); }
    else { fprintf(stderr, "[subs] embedded #%d: the file can't be read here (%s)\n", n, mkv_error(mkv)); toast(T("Subtitle extraction failed"), 3000, NULL); snprintf(P.curSub, sizeof P.curSub, "off"); mkv_select(mkv, -1); }
    refresh_tray(4); }
  else if (!st && SL.wait && !SL.tv && !SL.ready && now_() - SL.at > 6 && k >= 0 && embKind[k] == 1 && tv_has_text(n)) { SL.tv = 1; video_select_text(n); fprintf(stderr, "[subs] embedded #%d still being read: the TV draws it meanwhile\n", n); }
}
/* an embedded ASS / SSA track read from the file is drawn styled (libass) while the setting says so; plain otherwise,
   and from the moment renders get too slow here */
static void ass_sync(void) {
  int n = -1, k = -1;
  if (!strncmp(P.curSub, "emb:", 4) && SL.mkv && !SL.tv && mkv && !P.assPlain && strcmp(store_get("subsAssStyle", "styled"), "plain")) { n = atoi(P.curSub + 4); for (int i = 0; i < P.nEmb; i++) if (P.emb[i].idx == n) k = i; }
  int want = k >= 0 && embKind[k] == 2;
  static int openN = -1; static const void *openMkv;
  if (!want) { if (asssubs_on()) { asssubs_close(); openN = -1; } return; }
  if (!asssubs_on() || openN != n || openMkv != mkv) { asssubs_open(mkv, n, P.url, P.emb[k].lang); openN = n; openMkv = mkv; }
  if (asssubs_too_slow()) { P.assPlain = 1; asssubs_close(); openN = -1; mkv_style(mkv, n, 0); toast(T("Styled subtitles are too heavy here: shown plain"), 4000, NULL); return; }
  /* the picture's rectangle: fitted inside the screen (Fill and Zoom: the screen) */
  float vx = 0, vy = 0, vw = SB_W, vh = SB_H; int w = video_width(), h = video_height();
  if (w > 0 && h > 0 && !strcmp(P.aspect, "fit")) { float a = (float)w / h; if (a > SB_W / SB_H) { vh = SB_W / a; vy = (SB_H - vh) / 2; } else { vw = SB_H * a; vx = (SB_W - vw) / 2; } }
  VidState vs = video_state();
  asssubs_tick(video_time() - P.subDelay, vs == VID_PLAYING && !video_settling(), video_fps(), vx, vy, vw, vh, w, h, SUB_SIZES[size_idx()] / 100.0f);
}
static void sync_auto_arm(void);
static void auto_sub(void) {
  const char *w = want_sub(); if (P.autoSubDone || strcmp(P.curSub, "off")) return; if (!*w) return;
  int em = -1; for (int j = 0; j < P.nEmb; j++) if (lang_match(P.emb[j].lang, w) && (em < 0 || (P.emb[em].forced && !P.emb[j].forced))) em = j;
  if (em >= 0) { P.autoSubDone = 1; set_emb_sub(P.emb[em].idx); return; } /* embedded wins: it is in sync with the file */
  if (!P.embKnown && !P.embWaitOver) { if (!P.embWaitAt) P.embWaitAt = now_() + 8; return; } /* the file's own tracks get 8 s to show up first */
  for (int i = 0; i < P.nExt; i++) if (lang_match(P.ext[i].lang, w)) { P.autoSubDone = 1; set_ext_sub(1, &P.ext[i]); sync_auto_arm(); return; }
  for (int i = 0; i < P.nAdx; i++) if (lang_match(P.adx[i].lang, w)) { P.autoSubDone = 1; set_ext_sub(2, &P.adx[i]); sync_auto_arm(); return; }
}
/* a trailer's captions (IMDb's or YouTube's): handed over with the trailer, read when the CC button turns them on */
static TrailerSub trSubs[TRAILER_MAX_SUBS], trNext[TRAILER_MAX_SUBS]; static int trSubsN, trNextN;
void player_trailer_subs(const TrailerSub *subs, int n) { if (n > TRAILER_MAX_SUBS) n = TRAILER_MAX_SUBS; if (n < 0 || !subs) n = 0; if (n) memcpy(trNext, subs, n * sizeof *subs); trNextN = n; }
static void on_tr_cues(Cue *cues, int n, void *ud) {
  if (!P.open || (unsigned)(uintptr_t)ud != P.trSubTok || strncmp(P.curSub, "ext:", 4)) { subs_free(cues, n); return; }
  subs_free(P.cues, P.ncues); P.cues = cues; P.ncues = n; SL.wait = 0; fprintf(stderr, "[subs] trailer captions: %d lines\n", n);
  if (!n) toast(T("Couldn’t load these subtitles"), 3000, NULL);
}
/* one press on (the language the viewer reads, else the first), one press off */
static void cc_toggle(void) {
  if (!trSubsN) return;
  if (strcmp(P.curSub, "off")) { subs_off(); return; }
  const char *w = want_sub(); int pick = 0; for (int i = 0; i < trSubsN; i++) if (lang_match(trSubs[i].lang, w)) { pick = i; break; }
  snprintf(P.curSub, sizeof P.curSub, "ext:%s", trSubs[pick].url); P.pick.list = 0; subs_free(P.cues, P.ncues); P.cues = NULL; P.ncues = 0; sub_begin();
  P.trSubTok++; trailer_sub_load(&trSubs[pick], on_tr_cues, (void *)(uintptr_t)P.trSubTok);
}
/* the built-in OpenSubtitles list; no answer: asked again 15, 30 and 45 s later */
static void on_subs(SubEntry *subs, int n, int ok, void *ud) {
  if (!P.open || (unsigned)(uintptr_t)ud != P.subReq) { free(subs); return; }
  if (!ok && P.extTry < 3) { free(subs); P.extRetryAt = now_() + (P.extTry + 1) * 15; return; }
  free(P.ext); P.ext = subs; P.nExt = n; P.extLoading = 0; if (n) auto_sub(); refresh_tray(4);
}
/* the addons' lists (the stream's own, every subtitles addon's): each answer replaces the list with all that came so
   far; nothing from anyone: asked again 15, 30 and 45 s later */
static void on_adx(SubEntry *subs, int n, int final, int ok, void *ud) {
  if (!P.open || (unsigned)(uintptr_t)ud != P.adxReq) { free(subs); return; }
  if (final && !ok && !n && P.adxTry < 3 && P.nSubAddons) { free(subs); P.adxRetryAt = now_() + (P.adxTry + 1) * 15; return; }
  free(P.adx); P.adx = subs; P.nAdx = n; P.adxLoading = !final; if (n) auto_sub(); refresh_tray(4);
}
static int hashable(const char *u) { return !strncmp(u, "http", 4) && !strstr(u, ".m3u8"); }
static char artName[256], artPoster[512], artBg[512], artLogo[512]; /* the next play's title and pictures as handed over (a phone's) */
static SubEntry castSubs[16]; static int castSubsN = -1; /* the next play's subtitles as a phone sent them (-1: none sent) */
static void load_ext_subs(int n) { if (!P.type[0] || !P.vid[0] || P.isTrailer) return; P.extLoading = 1;
  if (castSubsN >= 0) { int k = castSubsN; castSubsN = -1; SubEntry *l = calloc(16, sizeof *l); memcpy(l, castSubs, sizeof castSubs); P.subReq++; on_subs(l, k, 1, (void *)(uintptr_t)P.subReq); return; } /* cast: the phone's own list */
  if (getenv("SB_PLAY_SUBS")) { SubEntry *l = calloc(16, sizeof *l); int k = 0; char b[4096]; snprintf(b, sizeof b, "%s", getenv("SB_PLAY_SUBS")); /* development: "url,url,..." as the external list */
    for (char *u = strtok(b, ","); u && k < 16; u = strtok(NULL, ",")) { snprintf(l[k].lang, sizeof l[k].lang, "eng"); snprintf(l[k].name, sizeof l[k].name, "English"); snprintf(l[k].url, sizeof l[k].url, "%s", u); l[k].n = k + 1; l[k].from = SUB_FALLBACK; k++; }
    P.subReq++; on_subs(l, k, 1, (void *)(uintptr_t)P.subReq); return; }
  P.extTry = n; P.subReq++; subs_list(P.type, P.vid, on_subs, (void *)(uintptr_t)P.subReq); }
static void load_addon_subs(int n) { if (!P.type[0] || !P.vid[0] || P.isTrailer || getenv("SB_PLAY_SUBS")) return;
  P.adxTry = n; P.adxReq++; P.nSubAddons = subs_addons_for(P.type, P.vid);
  SubQuery q; memset(&q, 0, sizeof q); snprintf(q.type, sizeof q.type, "%s", P.type); snprintf(q.vid, sizeof q.vid, "%s", P.vid); snprintf(q.url, sizeof q.url, "%s", P.url);
  snprintf(q.filename, sizeof q.filename, "%s", P.st.filename); snprintf(q.hash, sizeof q.hash, "%s", P.st.hash); q.size = P.st.size > 0 ? P.st.size : P.size; q.hashable = hashable(P.url);
  SubEntry *own = NULL; int nown = sb_stream_subs(&P.st, &own);
  if (!P.nSubAddons && !nown) { free(own); return; } /* nobody to ask */
  P.adxLoading = 1; subs_find(&q, own, nown, on_adx, (void *)(uintptr_t)P.adxReq); free(own); }
/* the file's subtitle tracks: read from the stream itself when it is a Matroska file (named, styled, delayed like the
   others: listed as soon as its head is read, their lines follow); else the ones the TV's pipeline reports, which it
   draws itself (their numbers are the same: the order among the file's subtitle tracks). Asked every second until
   settled; the tray is redrawn only when the list changes */
static void load_emb_tracks(void) {
  int st = mkv ? mkv_state(mkv) : MKV_FALLBACK; unsigned sig = 0; for (int i = 0; i < P.nEmb; i++) sig = sig * 31 + (unsigned)P.emb[i].idx + 7; sig = sig * 3 + (unsigned)P.nEmb + (unsigned)P.embKnown * 101;
  if (mkv && st != MKV_FALLBACK && !mkv_listed(mkv)) return; /* the file's head is still being read */
  if (mkv && st != MKV_FALLBACK) { MkvTrack t[16]; int n = mkv_tracks(mkv, t, 16), k = 0;
    for (int i = 0; i < n && k < 16; i++) { if (!t[i].text) continue; embKind[k] = strstr(t[i].codec, "ASS") || strstr(t[i].codec, "SSA") ? 2 : 1; VidTrack *v = &P.emb[k++]; memset(v, 0, sizeof *v); snprintf(v->lang, sizeof v->lang, "%s", t[i].lang); snprintf(v->name, sizeof v->name, "%s", t[i].name); v->idx = t[i].n; v->forced = t[i].forced; }
    P.nEmb = k; P.embKnown = 1; P.mkvTracksOk = 1; }
  else { P.nEmb = video_text_tracks(P.emb, 16); memset(embKind, 0, sizeof embKind); P.mkvTracksOk = 0; if (P.nEmb || video_live()) P.embKnown = 1; }
  unsigned sig2 = 0; for (int i = 0; i < P.nEmb; i++) sig2 = sig2 * 31 + (unsigned)P.emb[i].idx + 7; sig2 = sig2 * 3 + (unsigned)P.nEmb + (unsigned)P.embKnown * 101;
  if (sig2 != sig) fprintf(stderr, "[subs] %d embedded track(s) %s\n", P.nEmb, P.mkvTracksOk ? "read from the file" : "from the TV");
  if (!P.autoSubDone) auto_sub(); if (sig2 != sig) refresh_tray(4);
}

/* ---- Auto-sync: the subtitle on (a downloaded one) lined up with the file's own subtitle tracks, read near the
   playhead by the Matroska reader (any language: only when lines are spoken matters). They are waited for while they
   are being read (a torrent brings them in as its pieces arrive), up to 45 s; the matching then runs on a thread of
   its own (core/subsync). The answer applies to the lines' original times (running it again never adds up), the delay
   goes back to 0, Reset undoes it. This TV hands its sound to the screen's own decoder, so there is no audio to match
   against: a file with no text tracks (most HTTP and debrid files) is matched against another subtitle made for this
   file instead, found and read by the job itself; nothing of the kind: nothing to sync against. ---- */
static struct { SubSync *job; int phase; /* 0 idle, 1 waiting for the reference, 2 matching */ int stage; double t0, nextAt, center; char st[160]; double *base; int nbase;
  int quiet, autoWant; unsigned autoTok; double autoAt; /* quiet: the check of a subtitle the app chose by itself, unseen unless it fixes something */ } SY;
static void sync_stop(int keepTimes) {
  if (SY.job) { subsync_drop(SY.job); SY.job = NULL; } SY.phase = 0; SY.st[0] = 0; SY.quiet = 0;
  if (!keepTimes) { free(SY.base); SY.base = NULL; SY.nbase = 0; }
}
static void sync_end(const char *why) { if (SY.job) { subsync_drop(SY.job); SY.job = NULL; } SY.phase = 0; if (!SY.quiet) snprintf(SY.st, sizeof SY.st, "%s", why); fprintf(stderr, "[subsync] %s%s\n", SY.quiet ? "(by itself) " : "", why); SY.quiet = 0; refresh_tray(4); }
static void sync_undo(void) { /* Reset: the lines' own times again */
  if (SY.base && SY.nbase == P.ncues) { for (int i = 0; i < P.ncues; i++) { P.cues[i].s = SY.base[2 * i]; P.cues[i].e = SY.base[2 * i + 1]; } cueTimesGen++; }
  sync_stop(0);
}
static void sync_run(void) {
  if (SY.phase && SY.quiet) { SY.quiet = 0; snprintf(SY.st, sizeof SY.st, "%s", SY.phase == 2 ? (SY.stage ? T("Looking for a matching subtitle…") : T("Syncing…")) : T("Syncing…")); refresh_tray(4); return; } /* the check the app was making by itself: now shown */
  if (SY.phase) { sync_stop(1); refresh_tray(4); return; } /* pressed again while it runs: it stops */
  if (strncmp(P.curSub, "ext:", 4) || P.isTrailer) return;
  SY.phase = 1; SY.t0 = now_(); SY.nextAt = 0; SY.center = video_time(); snprintf(SY.st, sizeof SY.st, "%s", T("Syncing…")); refresh_tray(4);
  fprintf(stderr, "[subsync] asked at %.0f s\n", SY.center);
}
/* the subtitle's lines with their original times (not the ones a sync before set), for the job to copy */
static Cue *sync_xs(void) {
  Cue *xs = malloc((size_t)(P.ncues ? P.ncues : 1) * sizeof *xs); if (!xs) return NULL; int orig = SY.base && SY.nbase == P.ncues;
  for (int i = 0; i < P.ncues; i++) { xs[i] = P.cues[i]; if (orig) { xs[i].s = SY.base[2 * i]; xs[i].e = SY.base[2 * i + 1]; } }
  return xs;
}
static const char *sync_wait_txt(int stage) { return stage ? T("Looking for a matching subtitle…") : T("Syncing…"); }
/* no text track to match against: every other subtitle listed for this video goes to the job, which keeps those made
   for this file (by hash, carried by the stream, by file name) and asks OpenSubtitles with the file's hash */
static void sync_by_ref(const char *why) {
  int cap = P.nExt + P.nAdx, n = 0; SyncRef *rs = calloc((size_t)(cap ? cap : 1), sizeof *rs); if (!rs) { sync_end(T("Couldn’t sync")); return; }
  for (int l = 1; l <= 2; l++) { const SubEntry *L = l == 1 ? P.ext : P.adx; int nl = l == 1 ? P.nExt : P.nAdx;
    for (int i = 0; i < nl; i++) { const SubEntry *e = &L[i]; if (sub_is(l, e)) continue; SyncRef *r = &rs[n++];
      snprintf(r->url, sizeof r->url, "%s", e->url); snprintf(r->lang, sizeof r->lang, "%s", e->lang); snprintf(r->rel, sizeof r->rel, "%s", e->rel[0] ? e->rel : e->label);
      snprintf(r->from, sizeof r->from, "%s", e->addon); r->hm = e->hm; r->own = e->from == SUB_STREAM; r->fps = e->fps; } }
  SubQuery q; memset(&q, 0, sizeof q); snprintf(q.type, sizeof q.type, "%s", P.type); snprintf(q.vid, sizeof q.vid, "%s", P.vid); snprintf(q.url, sizeof q.url, "%s", P.url);
  snprintf(q.filename, sizeof q.filename, "%s", P.st.filename); snprintf(q.hash, sizeof q.hash, "%s", P.st.hash); q.size = P.st.size > 0 ? P.st.size : P.size; q.hashable = hashable(P.url);
  if (!q.hash[0]) subs_hash_of(P.url, q.hash, &q.size); q.fps = video_fps();
  int canHash = (q.hash[0] || q.hashable) && q.type[0] && q.vid[0];
  fprintf(stderr, "[sync] %s: another subtitle made for this file as the reference (%d listed, hash %s)\n", why, n, q.hash[0] ? q.hash : canHash ? "to work out" : "unknown");
  if (!n && !canHash) { free(rs); sync_end(T("Nothing to sync against")); return; }
  Cue *xs = sync_xs(); if (!xs) { free(rs); sync_end(T("Couldn’t sync")); return; }
  SY.job = subsync_start_ref(xs, P.ncues, SY.center, &q, P.curSub + 4, P.pick.list ? P.pick.lang : "", rs, n); free(xs); free(rs); /* (it keeps its own copies) */
  if (!SY.job) { sync_end(T("Couldn’t sync")); return; }
  SY.phase = 2; SY.stage = 1; if (!SY.quiet) snprintf(SY.st, sizeof SY.st, "%s", sync_wait_txt(1)); refresh_tray(4);
}
static void sync_auto_arm(void) { if (!P.nEmb && !P.isTrailer) { SY.autoWant = 1; SY.autoTok = SL.tok; SY.autoAt = now_(); } } /* (a file with subtitles of its own has them chosen instead) */
/* the subtitle the app chose by itself at the start, for a file with no subtitles of its own: checked once its lines are
   in and the lists have answered (or 15 s on), quietly; lined up only when clearly out (half a second or more, or the
   wrong speed) and the viewer has set no delay, then said in a toast */
static void sync_auto_tick(void) {
  if (!SY.autoWant || SY.phase) return;
  if (SL.tok != SY.autoTok || strncmp(P.curSub, "ext:", 4) || P.isTrailer || P.subDelay != 0) { SY.autoWant = 0; return; } /* another choice since, or a delay of the viewer's */
  if (!P.ncues || SL.wait || now_() - SY.autoAt < 3 || ((P.extLoading || P.adxLoading) && now_() - SY.autoAt < 15)) return;
  SY.autoWant = 0; SY.quiet = 1; SY.phase = 1; SY.t0 = now_(); SY.nextAt = 0; SY.center = video_time();
  fprintf(stderr, "[sync] checking the subtitle chosen at the start, by itself (at %.0f s)\n", SY.center);
}
static void sync_tick(void) {
  sync_auto_tick(); if (!SY.phase) return;
  if (SY.phase == 2) { SyncResult r; if (!subsync_poll(SY.job, &r)) { int sg = subsync_stage(SY.job); if (sg != SY.stage) { SY.stage = sg; if (!SY.quiet) { snprintf(SY.st, sizeof SY.st, "%s", sync_wait_txt(sg)); refresh_tray(4); } } return; }
    subsync_drop(SY.job); SY.job = NULL;
    if (r.st == SYNC_NOREF) { sync_end(T("Nothing to sync against")); return; }
    if (r.st == SYNC_NOSUB) { sync_end(T("Couldn’t sync")); return; }
    if (r.st != SYNC_OK) { sync_end(T("Not sure: unchanged")); return; }
    double at = video_time(), sh = at * (r.f - 1) + r.d; /* the shift where it is watched (a speed fix moves it along the film) */
    if (SY.quiet && (P.subDelay != 0 || (fabs(sh) < 0.5 && fabs(r.f - 1) < 0.0005))) { fprintf(stderr, "[sync] (by itself) %s: left as it is\n", P.subDelay != 0 ? "a delay was set meanwhile" : "already in sync"); sync_end(""); return; }
    if (!SY.base || SY.nbase != P.ncues) { free(SY.base); SY.base = malloc((size_t)(P.ncues ? P.ncues : 1) * 2 * sizeof *SY.base); SY.nbase = SY.base ? P.ncues : 0;
      for (int i = 0; i < SY.nbase; i++) { SY.base[2 * i] = P.cues[i].s; SY.base[2 * i + 1] = P.cues[i].e; } }
    if (!SY.base) { sync_end(T("Couldn’t sync")); return; }
    for (int i = 0; i < P.ncues; i++) { P.cues[i].s = SY.base[2 * i] * r.f + r.d; P.cues[i].e = SY.base[2 * i + 1] * r.f + r.d; } cueTimesGen++;
    P.subDelay = 0;
    char n[24]; snprintf(n, sizeof n, "%s%.1f", sh >= 0 ? "+" : "", sh); char st[160]; snprintf(st, sizeof st, "%s %s%s%s", T("Synced"), T1("{n} s", "n", n), fabs(r.f - 1) > 0.001 ? "  ·  " : "", fabs(r.f - 1) > 0.001 ? T("speed fixed") : "");
    int q = SY.quiet; SY.quiet = 0; sync_end(st); toast(q ? T1("Subtitles synced {n} s", "n", n) : SY.st, 3000, NULL); return; }
  if (now_() < SY.nextAt) return; SY.nextAt = now_() + 0.5;
  /* waiting for the file's own lines near the playhead */
  int st = mkv ? mkv_state(mkv) : MKV_FALLBACK, listed = mkv && st != MKV_FALLBACK && mkv_listed(mkv);
  if (st == MKV_FALLBACK) { sync_by_ref("not a file whose tracks can be read"); return; }
  MkvTrack tr[16]; int nt = listed ? mkv_tracks(mkv, tr, 16) : 0, ntext = 0;
  double lo = SY.center - SYNC_BEHIND_S, hi = SY.center + SYNC_AHEAD_S; int best = -1, bestN = 0;
  for (int i = 0; i < nt; i++) { if (!tr[i].text) continue; ntext++; int c = mkv_lines_in(mkv, tr[i].n, lo, hi, NULL, 0, NULL, 0); if (c > bestN) { bestN = c; best = tr[i].n; } }
  if (listed && !ntext) { sync_by_ref("no text track in the file"); return; } /* the file has no text tracks at all */
  if (bestN >= 12) {
    int cap = bestN < 3000 ? bestN : 3000, tcap = cap * 96; MkvLine *ml = malloc((size_t)cap * sizeof *ml); char *tb = malloc((size_t)tcap); SyncLine *rl = malloc((size_t)cap * sizeof *rl);
    Cue *xs = sync_xs();
    if (!ml || !tb || !rl || !xs) { free(ml); free(tb); free(rl); free(xs); sync_end(T("Couldn’t sync")); return; }
    int k = mkv_lines_in(mkv, best, lo, hi, ml, cap, tb, tcap); if (k > cap) k = cap;
    for (int i = 0; i < k; i++) { rl[i].s = ml[i].s; rl[i].e = ml[i].e; rl[i].t = ml[i].t; }
    fprintf(stderr, "[subsync] reference: track #%d, %d lines near %.0f s after %.1f s\n", best, bestN, SY.center, now_() - SY.t0);
    SY.job = subsync_start(xs, P.ncues, rl, k, SY.center); free(ml); free(tb); free(rl); free(xs); /* (it keeps its own copies) */
    if (!SY.job) { sync_end(T("Couldn’t sync")); return; }
    SY.phase = 2; return; }
  /* its own tracks are the best reference by far: while they are being read keep waiting, up to 45 s */
  double limit = listed && st == MKV_HARVESTING ? 45 : 12;
  if (now_() - SY.t0 > limit) { fprintf(stderr, "[subsync] reference: %d lines near %.0f s after %.0f s\n", bestN, SY.center, now_() - SY.t0);
    if (ntext) sync_end(T("Still reading the film’s subtitles: try again in a minute")); else sync_by_ref("the file's tracks were not read in time"); }
}

/* ---- audio ---- */
static void set_audio(int i) { P.curAud = i; video_select_audio(i); P.infoKey[0] = 0; }
/* ---- sound this TV can't decode: DTS where it was seen failing (or not yet seen playing), TrueHD always (passed
   through to a receiver only), and any codec the TV refused on this stream (error 200: the picture plays on) ---- */
static int aud_kind(const char *codec) { /* 1 DTS, 2 TrueHD, 0 another; the TV's names ("dts") and Matroska's ("A_DTS") alike */
  char c[32]; lower(codec, c, sizeof c); return strstr(c, "dts") ? 1 : (strstr(c, "truehd") || strstr(c, "mlp")) ? 2 : 0; }
static int aud_bad(const VidTrack *t) {
  int k = aud_kind(t->codec), dm = best_dts_mode(); if (k == 2 || (k == 1 && (dm == 1 || (dm == 0 && best_learned("dts") == 0) || getenv("SB_DTS_CONVERT")))) return 1; /* DTS: Convert always; Auto once the TV was seen failing it; Off left to the TV */ /* (SB_DTS_CONVERT: development, a TV that plays DTS converts it anyway) */
  for (int i = 0; i < P.dts.nbad; i++) if (t->codec[0] && !strcasecmp(P.dts.bad[i], t->codec)) return 1;
  return 0;
}
/* the TV's list, its codecs filled from the file's own tracks (same order) where the TV leaves them out */
static int aud_list(VidTrack *ts, int max) {
  int n = video_audio_tracks(ts, max); if (!n || !mkv_listed(mkv)) return n;
  MkvAudio ma[16]; int m = mkv_audio_tracks(mkv, ma, 16);
  if (m == n) for (int i = 0; i < n; i++) if (!ts[i].codec[0]) { snprintf(ts[i].codec, sizeof ts[i].codec, "%s", ma[i].codec); if (!ts[i].channels) ts[i].channels = ma[i].channels; }
  return n;
}
static void aud_label(const VidTrack *t, char *out, size_t cap) { /* "AC3 5.1" */
  char c[16]; size_t i = 0; const char *s = t->codec; if (!strncmp(s, "A_", 2)) s += 2; for (; s[i] && i + 1 < sizeof c; i++) c[i] = (char)toupper((unsigned char)s[i]); c[i] = 0;
  int ch = t->channels; snprintf(out, cap, "%s%s", c, ch == 6 ? " 5.1" : ch == 8 ? " 7.1" : ch == 2 ? " 2.0" : ch == 1 ? " 1.0" : "");
}
static void auto_audio(void) {
  if (P.autoAudDone || strcmp(store_get("enableDefaultAudioTrack", "false"), "true")) return; const char *want = store_get("defaultAudioTrack", ""); if (!*want) return;
  VidTrack ts[16]; int n = aud_list(ts, 16); if (!n) return; int pick = -1;
  for (int i = 0; i < n; i++) if (lang_match(ts[i].lang, want) && (pick < 0 || (aud_bad(&ts[pick]) && !aud_bad(&ts[i])))) pick = i; /* in that language, one whose sound plays here first */
  if (pick < 0 && strcmp(want, "eng")) for (int i = 0; i < n; i++) if (lang_match(ts[i].lang, "eng") && (pick < 0 || (aud_bad(&ts[pick]) && !aud_bad(&ts[i])))) pick = i; /* not in the file: English, when the file has it */
  if (pick >= 0) { P.autoAudDone = 1; set_audio(ts[pick].idx); }
}
/* No track this TV plays: the file is converted on the TV itself (video copied, the sound as Dolby Digital 5.1) and
   played from a local HLS stream through the same pipeline, from the same moment; the subtitles still read the file.
   Not for a stream that isn't a plain http file, nor HEVC where this TV showed it can't play it from HLS. tvIdx: the
   track in the TV's list (the same order as the file's), -1 the first DTS / TrueHD one. 1 started */
static int dv7_wanted(void);
static int dts_convert(int tvIdx, const char *why) {
  if (P.isTrailer || P.isHls || strncmp(P.url, "http", 4) || !hls_conv_available()) return 0;
  char vc[32] = ""; int hevc = mkv_video_codec(mkv, vc, sizeof vc) ? strstr(vc, "HEVC") != NULL : has_word(P.sname, "hevc") || has_word(P.sname, "x265") || has_word(P.sname, "h265");
  if (hevc && best_learned("hlsHevcOk") == 0) { fprintf(stderr, "[dts] no conversion: HEVC from local HLS failed on this TV before\n"); return 0; }
  int ord = tvIdx; MkvAudio ma[16]; int m = mkv_audio_tracks(mkv, ma, 16);
  if (ord >= m || (ord >= 0 && !aud_kind(ma[ord].codec))) ord = -1; /* (the TV's list and the file's disagree: the first DTS / TrueHD one) */
  P.dts.conv = 1; P.dts.convOrd = ord; P.dts.hevc = hevc; P.dts.convT0 = now_(); P.dts.convAt = P.live ? video_time() : P.startAt;
  P.dts.dv = dv7_wanted(); P.dts.copyA = 0; P.dts.dvTried = 1; /* a profile 7 picture: 8.1 in the same run */
  hls_conv_start(P.url, ord, (mkv_has_index(mkv) == 0 ? HC_NOINDEX : 0) | (P.dts.dv ? HC_DV81 : 0));
  fprintf(stderr, "[dts] path c: converting audio track %d (%s, %s%s) at %.0f s\n", ord, why, hevc ? "HEVC" : vc[0] ? vc : "video", P.dts.dv ? ", Dolby Vision 7 as 8.1" : "", P.dts.convAt);
  toast(T("Converting the sound for this TV…"), 4000, NULL);
  return 1;
}
/* ---- Dolby Vision profile 7 (Blu-ray remuxes: a base layer, an enhancement layer and the RPU in one track). A TV
   with Dolby Vision shows such a file as HDR10 only; converted here to single-layer profile 8.1 (the enhancement
   layer dropped, each RPU rewritten, the base layer as it is) it shows as Dolby Vision, through the same pipeline from
   local HLS, with the audio track copied (converted when this TV can't decode it). Only for an http file whose head
   says profile 7, on a TV with Dolby Vision, with the setting on, and unless this TV has shown that HEVC from local
   HLS fails or that the converted picture still shows as HDR10. Anything that fails plays the file as before ---- */
static int dv7_wanted(void) {
  if (P.isTrailer || P.isHls || strncmp(P.url, "http", 4) || !hls_conv_available()) return 0;
  if (!strcmp(store_get("dv7Convert", "true"), "false") || best_learned("hlsHevcOk") == 0 || best_learned("dv81Hls") == 0) return 0;
  const TvCaps *c = best_caps(); return c && c->dv && mkv_dovi(mkv, NULL) == 7;
}
static int aud_copyable(const char *codec) { /* the tracks the stream can carry as they are */
  static const char *const OK[] = { "A_AC3", "A_EAC3", "A_AAC", "A_DTS", "A_TRUEHD", "A_MPEG/L3", "A_MPEG/L2" };
  for (int i = 0; i < 7; i++) if (!strncmp(codec, OK[i], strlen(OK[i]))) return 1; return 0;
}
static int mkv_aud_bad(const MkvAudio *a) { VidTrack v; memset(&v, 0, sizeof v); snprintf(v.codec, sizeof v.codec, "%s", a->codec); return aud_bad(&v); }
/* the track that goes with the converted picture: the default-audio language, else the file's default, else the first;
   one this TV can't decode gives way to one it plays in the same language, else it is converted. -1 none fits */
static int dv7_audio(int *copy) {
  MkvAudio ma[16]; int m = mkv_audio_tracks(mkv, ma, 16), pick = -1; if (!m) return -1;
  const char *want = !strcmp(store_get("enableDefaultAudioTrack", "false"), "true") ? store_get("defaultAudioTrack", "") : "";
  for (int i = 0; i < m && pick < 0; i++) if (*want && lang_match(ma[i].lang, want)) pick = i;
  for (int i = 0; i < m && pick < 0; i++) if (ma[i].def) pick = i;
  if (pick < 0) pick = 0;
  if (mkv_aud_bad(&ma[pick]) || !aud_copyable(ma[pick].codec))
    for (int i = 0; i < m; i++) if (!mkv_aud_bad(&ma[i]) && aud_copyable(ma[i].codec) && !strcasecmp(ma[i].lang, ma[pick].lang)) { pick = i; break; }
  if (!mkv_aud_bad(&ma[pick]) && aud_copyable(ma[pick].codec)) { *copy = 1; return pick; }
  if (aud_kind(ma[pick].codec)) { *copy = 0; return pick; } /* DTS / TrueHD this TV can't decode: as Dolby Digital */
  return -1;
}
static int conv_flags(int ord) { /* another audio track while converting */
  MkvAudio ma[16]; int m = mkv_audio_tracks(mkv, ma, 16), copy = P.dts.dv && ord >= 0 && ord < m && !mkv_aud_bad(&ma[ord]) && aud_copyable(ma[ord].codec);
  return (mkv_has_index(mkv) == 0 ? HC_NOINDEX : 0) | (P.dts.dv ? HC_DV81 : 0) | (copy ? HC_COPYAUDIO : 0);
}
static int dv7_convert(const char *why) {
  if (P.dts.conv || P.dts.dvTried || !dv7_wanted()) return 0;
  P.dts.dvTried = 1; int copy = 1, ord = dv7_audio(&copy);
  if (ord < 0) { fprintf(stderr, "[dv] profile 7, but no audio track can go with it: played as it is\n"); return 0; }
  P.dts.conv = 1; P.dts.convOrd = ord; P.dts.dv = 1; P.dts.copyA = copy; P.dts.hevc = 1; P.dts.convT0 = now_(); P.dts.convAt = P.live ? video_time() : P.startAt;
  hls_conv_start(P.url, ord, (mkv_has_index(mkv) == 0 ? HC_NOINDEX : 0) | HC_DV81 | (copy ? HC_COPYAUDIO : 0));
  fprintf(stderr, "[dv] profile 7 (%s): the picture as 8.1, audio track %d %s, from %.0f s\n", why, ord, copy ? "copied" : "as Dolby Digital", P.dts.convAt);
  return 1;
}
/* A stream whose text says Dolby Vision from a Blu-ray (a remux, FEL / MEL) on a TV with Dolby Vision: the first load
   waits for the file's head (a moment), so a profile 7 picture starts converted instead of switching after a second */
static int dv7_defer_wanted(void) {
  if (P.isTrailer || P.isHls || strncmp(P.url, "http", 4) || !hls_conv_available()) return 0;
  if (!strcmp(store_get("dv7Convert", "true"), "false") || best_learned("hlsHevcOk") == 0 || best_learned("dv81Hls") == 0) return 0;
  const TvCaps *c = best_caps(); if (!c || !c->dv) return 0;
  char l[1100]; snprintf(l, sizeof l, "%s %s", P.sname, P.st.filename); for (char *q = l; *q; q++) *q = (char)tolower((unsigned char)*q);
  int dv = has_word(l, "dv") || has_word(l, "dovi") || strstr(l, "dolby vision") || has_word(l, "dolbyvision");
  return dv && (strstr(l, "remux") || strstr(l, "bluray") || strstr(l, "blu-ray") || has_word(l, "bdmv") || has_word(l, "fel") || has_word(l, "mel") || has_word(l, "p7") || has_word(l, "dvp7"));
}
/* while converting: the stream once it is ready, then whether the TV plays it (HEVC in HLS: learned once) */
static void dts_conv_tick(void) {
  if (P.dts.conv == 1) {
    char url[256], why[160]; HlsConvInfo ci; int st = hls_conv_poll(url, sizeof url, why, sizeof why, &ci);
    if (!st) return;
    if (st < 0 && P.dts.dv && P.dts.copyA) { /* the picture only: the file as it is, nothing said */
      P.dts.conv = 0; P.dts.dv = 0; fprintf(stderr, "[dv] the converter can't take this file (%s): played as it is\n", why);
      if (video_state() == VID_IDLE) { P.live = 0; P.liveT0 = 0; P.didResume = 0; video_load(P.url); }
      return; }
    if (st < 0 && P.dts.dv) { /* the picture couldn't be converted: the sound alone */
      fprintf(stderr, "[dv] the converter can't take this picture (%s): the sound alone\n", why); P.dts.dv = 0; hls_conv_start(P.url, P.dts.convOrd, conv_flags(P.dts.convOrd)); return; }
    if (st < 0) { P.dts.conv = 0; P.dts.gaveUp = 1; fprintf(stderr, "[dts] conversion failed: %s\n", why); toast(T("This TV can’t play this video’s sound (DTS or TrueHD)"), 6000, NULL);
      if (video_state() == VID_IDLE) { P.live = 0; P.liveT0 = 0; P.didResume = 0; video_load(P.url); } /* the picture as before, without its sound */
      return; }
    P.dts.conv = 2; P.dts.hevc = strstr(ci.vcodec, "hevc") != NULL; P.dts.convT0 = now_(); P.dts.okAt = 0;
    double at = P.live && video_has_media() ? video_time() : P.dts.convAt; if (at < 1) at = P.startAt;
    P.live = 0; P.liveT0 = 0; P.didResume = 0; P.startAt = at > 1 ? at : P.startAt; P.curAud = -1; P.infoKey[0] = 0;
    fprintf(stderr, "[dts] playing the converted stream (%s, %s; %s) from %.0f s\n", ci.indexed ? "segments" : "one stream", ci.vcodec, ci.path, at);
    P.dts.dvSeen = 0; P.dts.dvAt = 0;
    if (ci.indexed) video_load(url); else video_load_timed(url, ci.dur);
    return;
  }
  if (P.dts.conv == 2 && P.dts.hevc && best_learned("hlsHevcOk") < 0) { /* HEVC in local HLS: plays here or not, learned once */
    if (video_state() == VID_PLAYING && video_live()) { if (!P.dts.okAt) P.dts.okAt = now_(); else if (now_() - P.dts.okAt >= 3) best_learn("hlsHevcOk", 1); } else P.dts.okAt = 0; }
  if (P.dts.conv == 2 && P.dts.dv && !P.dts.dvSeen) { /* the converted picture: Dolby Vision on screen, or HDR10 all the same (then not converted again here) */
    if (video_state() == VID_PLAYING && video_live()) { if (!P.dts.dvAt) P.dts.dvAt = now_();
      else if (now_() - P.dts.dvAt >= 3) { char h[32]; if (video_hdr(h, sizeof h)) { P.dts.dvSeen = 1; int yes = strcasestr(h, "dolby") != NULL;
          fprintf(stderr, "[dv] the TV shows the converted picture as %s\n", h); if (yes || strcasestr(h, "hdr")) best_learn("dv81Hls", yes); }
        else if (now_() - P.dts.dvAt >= 15) P.dts.dvSeen = 1; } } /* (this TV doesn't say) */
    else P.dts.dvAt = 0; }
}
static void dts_conv_failed(const char *why) { /* the converted stream didn't play: today's reason, and HEVC from HLS is not tried again here */
  fprintf(stderr, "[dts] the converted stream didn't play (%s)\n", why);
  if (P.dts.dv) { /* the converted picture: not converted again on this TV; from here the file as it is, or (DTS) the sound alone converted */
    best_learn("dv81Hls", 0); hls_conv_stop(); P.dts.dv = 0;
    double at = P.lastPos > 1 ? P.lastPos : P.dts.convAt; P.startAt = at > 1 ? at : P.startAt; P.live = 0; P.liveT0 = 0; P.didResume = 0; P.curAud = -1;
    if (P.dts.copyA) { P.dts.conv = 0; video_load(P.url); return; }
    P.dts.conv = 1; P.dts.convAt = P.startAt; P.dts.convT0 = now_(); hls_conv_start(P.url, P.dts.convOrd, conv_flags(P.dts.convOrd));
    fprintf(stderr, "[dts] the sound alone converted from %.0f s\n", P.startAt); return; }
  if (P.dts.hevc) best_learn("hlsHevcOk", 0);
  hls_conv_stop(); P.dts.conv = 0; P.dts.gaveUp = 1; fail(T("This TV can’t play this video’s sound (DTS or TrueHD)"));
}
/* Once a second while playing: the track the TV plays is checked against what this TV decodes. DTS still unknown here
   is left to the TV (3 s of play without an error 200 teaches "yes", the error "no"); a track that can't play gives way
   to one that does, in the language of the default-audio setting, else the same language, else the first; with none,
   the reason is said once and the picture plays on */
static void dts_tick(void) {
  if (P.dts.conv) { dts_conv_tick(); return; }
  if (!P.dts.dvTried && mkv_listed(mkv) && dv7_convert("read in the file's head")) return;
  if (P.isTrailer || P.dts.gaveUp) return;
  VidTrack ts[16]; int n = aud_list(ts, 16); if (!n) return; /* only after the TV listed them: choosing earlier stalls it */
  int cur = P.curAud >= 0 ? P.curAud : video_audio_current(), ci = 0; for (int i = 0; i < n; i++) if (ts[i].idx == cur) ci = i;
  VidTrack *t = &ts[ci]; int kind = aud_kind(t->codec), unsup = video_audio_unsupported();
  if (kind == 1 && best_dts_mode() == 2) return; /* DTS sound Off: the TV does with it whatever it can, nothing here steps in */
  if (unsup) { if (kind == 1 && !best_dts_mode()) { best_learn("dts", 0); fprintf(stderr, "[dts] auto: this TV refused DTS, converting from now on\n"); } int known = 0; for (int i = 0; i < P.dts.nbad; i++) if (!strcasecmp(P.dts.bad[i], t->codec)) known = 1;
    if (!known && t->codec[0] && P.dts.nbad < 4) snprintf(P.dts.bad[P.dts.nbad++], sizeof P.dts.bad[0], "%s", t->codec); }
  if (kind == 1 && !unsup && !best_dts_mode() && best_learned("dts") < 0 && video_state() == VID_PLAYING) { if (!P.dts.playAt) P.dts.playAt = now_(); else if (now_() - P.dts.playAt >= 3) { best_learn("dts", 1); fprintf(stderr, "[dts] auto: this TV plays DTS itself\n"); } }
  else P.dts.playAt = 0;
  if (!unsup && !aud_bad(t)) return;
  const char *want = !strcmp(store_get("enableDefaultAudioTrack", "false"), "true") ? store_get("defaultAudioTrack", "") : "";
  int pick = -1, score = -1;
  for (int i = 0; i < n && P.dts.switches < 4; i++) { if (i == ci || aud_bad(&ts[i])) continue; /* (tracks the TV doesn't name: a few tries at most) */
    int sc = (*want && lang_match(ts[i].lang, want)) ? 2 : (t->lang[0] && !strcasecmp(ts[i].lang, t->lang)) ? 1 : 0; if (sc > score) { score = sc; pick = i; } }
  const char *what = kind == 2 ? "TrueHD" : "DTS";
  if (pick >= 0) {
    char lb[32], msg[160]; aud_label(&ts[pick], lb, sizeof lb);
    snprintf(msg, sizeof msg, "%s", kind == 2 ? T1("Playing the {t} track: this TV can’t play TrueHD", "t", lb) : T1("Playing the {t} track: this TV can’t play DTS", "t", lb));
    fprintf(stderr, "[dts] path b: %s track %d (%s) can't play here%s, track %d (%s %s) instead\n", what, ci, t->codec, unsup ? " (error 200)" : "", pick, ts[pick].codec, ts[pick].lang);
    set_audio(ts[pick].idx); P.dts.switches++; if (!P.dts.said) { P.dts.said = 1; toast(msg, 4000, NULL); }
    return;
  }
  if (dts_convert(ci, unsup ? "error 200" : "known")) return;
  P.dts.gaveUp = 1;
  fprintf(stderr, "[dts] path c: only %s here (track %d, %s)%s: no track this TV plays\n", what, ci, t->codec, unsup ? ", error 200" : "");
  toast(T("This TV can’t play this video’s sound (DTS or TrueHD)"), 6000, NULL);
}

/* ---- skips: intro / recap / credits. Sources, best first, merged per segment: the file's own chapters (a reader
   hooks in below), TheIntroDB and IntroDB by IMDb id, AniSkip for anime from Kitsu-based addons. A segment that
   doesn't fit this file's length is dropped and the next source's is used. One source with something useful
   gives the others 2.5 s more; whatever answered by 14 s is used. API answers are kept 6 h; errors never ---- */
void (*player_chapters_hook)(const char *url, int p2p);
enum { SK_TIDB, SK_IDB, SK_KITSU, SK_ANI };
static struct { char url[440]; double at; char *body; } skCache[120]; static int skCacheN; static unsigned skTok;
static const char *sk_cached(const char *u) { for (int i = 0; i < skCacheN; i++) if (!strcmp(skCache[i].url, u)) return now_() - skCache[i].at < 6 * 3600 ? skCache[i].body : NULL; return NULL; }
static void sk_keep(const char *u, const char *body) {
  int i = 0; for (; i < skCacheN; i++) if (!strcmp(skCache[i].url, u)) break;
  if (i == skCacheN) { if (skCacheN < 120) skCacheN++; else { free(skCache[0].body); memmove(skCache, skCache + 1, sizeof skCache[0] * 119); i = 119; skCache[i].body = NULL; } }
  free(skCache[i].body); snprintf(skCache[i].url, sizeof skCache[i].url, "%s", u); skCache[i].body = strdup(body); skCache[i].at = now_();
}
static int sk_fits(int k, double s0, double e0) { double d = P.sk.d; if (!d) return 1; if (s0 >= d || e0 > d + 30) return 0; return k == 2 ? s0 >= d * 0.5 : s0 < d * 0.4 && e0 - s0 >= 3 && e0 - s0 <= 420; }
static void sk_seg(int *has, double *s, double *e, double a, double b) { *has = a >= 0 && b > a; if (*has) { *s = a; *e = b; } }
static void sk_add(int rank, const int has[3], const double ss[3], const double ee[3]) {
  if (P.sk.nsrc >= 4) return; int i = P.sk.nsrc++; P.sk.src[i].rank = rank; int any = 0;
  for (int k = 0; k < 3; k++) { P.sk.src[i].g[k].has = has[k]; P.sk.src[i].g[k].s = ss[k]; P.sk.src[i].g[k].e = ee[k]; any |= has[k]; }
  for (int j = i; j > 0 && P.sk.src[j - 1].rank > P.sk.src[j].rank; j--) { __typeof__(P.sk.src[0]) t = P.sk.src[j]; P.sk.src[j] = P.sk.src[j - 1]; P.sk.src[j - 1] = t; }
  if (any && !P.sk.quickAt) P.sk.quickAt = now_() + 2.5;
}
static void sk_finish(void) {
  if (P.sk.sent) return; P.sk.sent = 1; P.sk.tok = 0; double d = P.sk.d; int has[3] = { 0 }; double ss[3] = { 0 }, ee[3] = { 0 };
  for (int k = 0; k < 3; k++) for (int j = 0; j < P.sk.nsrc; j++) { const __typeof__(P.sk.src[0].g[0]) *g = &P.sk.src[j].g[k]; if (g->has && sk_fits(k, g->s, g->e)) { has[k] = 1; ss[k] = g->s; ee[k] = g->e; break; } }
  if (has[2] && d && ee[2] > d) ee[2] = d;
  /* credits learned from this show's other episodes: how long before the end they start */
  if (episodic()) { char lk[96]; snprintf(lk, sizeof lk, "creditsOff_%s", P.id);
    if (has[2] && d && d - ss[2] > 10 && d - ss[2] < 600) { char v[16]; snprintf(v, sizeof v, "%d", (int)(d - ss[2] + 0.5)); store_set(lk, v); }
    else if (!has[2] && d > 600) { double off = atof(store_get(lk, "0")); if (off > 10) { has[2] = 1; ss[2] = d - off; ee[2] = d; } } }
  fprintf(stderr, "[player] skips from %d sources: intro %.0f-%.0f recap %.0f-%.0f credits %.0f-%.0f\n", P.sk.nsrc, has[0] ? ss[0] : -1, ee[0], has[1] ? ss[1] : -1, ee[1], has[2] ? ss[2] : -1, ee[2]);
  if (!has[0] && !has[1] && !has[2]) return;
  P.intro.has = has[0]; P.intro.s = ss[0]; P.intro.e = ee[0]; P.recap.has = has[1]; P.recap.s = ss[1]; P.recap.e = ee[1]; P.outro.has = has[2]; P.outro.s = ss[2]; P.outro.e = ee[2]; P.skipsOk = 1;
}
static void sk_done(void) { if (--P.sk.pending <= 0) sk_finish(); }
static void sk_ask(int i);
static double sk_ms(const cJSON *a, const char *k, double nul) { const cJSON *v = cJSON_GetObjectItemCaseSensitive(a, k); return !v || cJSON_IsNull(v) ? nul : cJSON_IsNumber(v) ? v->valuedouble / 1000 : -1; }
static double sk_sec(const cJSON *o, const char *k, double nul) { const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k); return !v || cJSON_IsNull(v) ? nul : cJSON_IsNumber(v) ? v->valuedouble : cJSON_IsString(v) ? atof(v->valuestring) : -1; }
/* one source's answer (j NULL: nothing usable) */
static void sk_answer(int i, const cJSON *j) {
  int kind = P.sk.job[i].kind, has[3] = { 0 }; double ss[3] = { 0 }, ee[3] = { 0 }, d = P.sk.d;
  if (kind == SK_TIDB) {
    if (!j || (P.sk.isEp && cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(j, "episode")) && ((int)js_num(j, "season", -1) != P.sk.se || (int)js_num(j, "episode", -1) != P.sk.en))) { sk_done(); return; } /* another episode: not ours */
    static const char *K[3] = { "intro", "recap", "credits" };
    for (int k = 0; k < 3; k++) { const cJSON *l = js_arr(j, K[k]), *a = l ? cJSON_GetArrayItem(l, 0) : NULL; if (!cJSON_IsObject(a)) continue; double s0 = sk_ms(a, "start_ms", 0); sk_seg(&has[k], &ss[k], &ee[k], s0, sk_ms(a, "end_ms", k == 2 ? (d ? d : s0 + 600) : -1)); }
    sk_add(1, has, ss, ee);
  } else if (kind == SK_IDB) {
    if (!j) { sk_done(); return; }
    static const char *K[3] = { "intro", "recap", "outro" };
    for (int k = 0; k < 3; k++) { const cJSON *o = js_obj(j, K[k]); if (!o) continue; double s0 = sk_sec(o, "start_sec", -1); sk_seg(&has[k], &ss[k], &ee[k], s0, sk_sec(o, "end_sec", k == 2 ? (d ? d : s0 + 600) : -1)); }
    sk_add(2, has, ss, ee);
  } else if (kind == SK_KITSU) { /* the MyAnimeList id, then its skip times (the same job) */
    const cJSON *dt = js_arr(j, "data"), *m0 = dt ? cJSON_GetArrayItem(dt, 0) : NULL; const char *mal = js_str(js_obj(m0, "attributes"), "externalId", NULL);
    if (!mal) { sk_done(); return; }
    char enc[64]; size_t o = 0; for (const char *c = mal; *c && o + 4 < sizeof enc; c++) { if (isalnum((unsigned char)*c) || strchr("-_.~", *c)) enc[o++] = *c; else o += snprintf(enc + o, sizeof enc - o, "%%%02X", (unsigned char)*c); } enc[o] = 0;
    P.sk.job[i].kind = SK_ANI; P.sk.job[i].tries = 0; snprintf(P.sk.job[i].url, sizeof P.sk.job[i].url, "https://api.aniskip.com/v2/skip-times/%s/%d?types=op&types=ed&types=recap&types=mixed-op&types=mixed-ed&episodeLength=0", enc, P.sk.en); sk_ask(i); return;
  } else {
    const cJSON *r = js_bool(j, "found", 0) ? js_arr(j, "results") : NULL, *x;
    if (r) cJSON_ArrayForEach(x, r) { const cJSON *iv = js_obj(x, "interval"); const char *t = js_str(x, "skipType", ""); int hs; double a, b; sk_seg(&hs, &a, &b, sk_sec(iv, "startTime", -1), sk_sec(iv, "endTime", -1)); if (!hs) continue;
      int k = (!strcmp(t, "op") || !strcmp(t, "mixed-op")) ? 0 : (!strcmp(t, "ed") || !strcmp(t, "mixed-ed")) ? 2 : !strcmp(t, "recap") ? 1 : -1; if (k >= 0 && !has[k]) { has[k] = 1; ss[k] = a; ee[k] = b; } }
    sk_add(3, has, ss, ee);
  }
  sk_done();
}
/* a real answer, or a real "nothing for this title" (kept too); anything else is asked again twice, 1.5 s apart */
static void on_sk(int st, const unsigned char *body, size_t len, void *ud) {
  (void)len; unsigned v = (unsigned)(uintptr_t)ud; int i = v & 3; if (!P.open || !P.sk.tok || v >> 2 != P.sk.tok || !P.sk.job[i].live) return;
  cJSON *j = st > 0 ? net_json(body) : NULL; const cJSON *err = cJSON_GetObjectItemCaseSensitive(j, "error");
  int real = cJSON_IsObject(j) && !err && (cJSON_GetObjectItemCaseSensitive(j, "imdb_id") || cJSON_GetObjectItemCaseSensitive(j, "tmdb_id") || cJSON_GetObjectItemCaseSensitive(j, "found") || (P.sk.job[i].kind == SK_KITSU && js_arr(j, "data")));
  int miss = 0; if (cJSON_IsString(err)) { char e[96]; lower(err->valuestring, e, sizeof e); miss = strstr(e, "not found") != NULL; }
  if (!real && !miss && ++P.sk.job[i].tries < 3) { cJSON_Delete(j); P.sk.job[i].retryAt = now_() + 1.5; return; }
  if (real || miss) sk_keep(P.sk.job[i].url, real ? (const char *)body : "{}");
  P.sk.job[i].live = 0; sk_answer(i, real ? j : NULL); cJSON_Delete(j);
}
static void sk_ask(int i) {
  const char *c = sk_cached(P.sk.job[i].url);
  if (c) { cJSON *j = cJSON_Parse(c); P.sk.job[i].live = 0; int ok = cJSON_IsObject(j) && cJSON_GetArraySize(j) > 0; sk_answer(i, ok ? j : NULL); cJSON_Delete(j); return; }
  P.sk.job[i].live = 1; P.sk.job[i].retryAt = 0; net_get(P.sk.job[i].url, on_sk, (void *)(uintptr_t)((P.sk.tok << 2) | (unsigned)i));
}
static void sk_job(const char *url, int kind) { int i = 0; while (i < 3 && P.sk.job[i].url[0]) i++; if (i >= 3) return; snprintf(P.sk.job[i].url, sizeof P.sk.job[i].url, "%s", url); P.sk.job[i].kind = kind; P.sk.pending++; sk_ask(i); }
static void sk_tick(void) {
  if (!P.sk.tok || P.sk.sent) return; double n = now_();
  for (int i = 0; i < 3; i++) if (P.sk.job[i].live && P.sk.job[i].retryAt && n >= P.sk.job[i].retryAt) sk_ask(i);
  if (!P.sk.sent && ((P.sk.quickAt && n >= P.sk.quickAt) || n >= P.sk.endAt)) sk_finish();
}
static void load_skips(void) {
  if (P.isTrailer || P.skipsAsked || !P.id[0]) return; P.skipsAsked = 1;
  const char *ts = getenv("SB_PLAY_SKIPS"); /* development: "intro:s-e,recap:s-e,outro:s-e" instead of the sources (the page's skipsForTest) */
  if (ts) { for (const char *c = ts; c && *c; c = strchr(c, ',') ? strchr(c, ',') + 1 : NULL) { double a = 0, b = 0; if (sscanf(strchr(c, ':') ? strchr(c, ':') + 1 : c, "%lf-%lf", &a, &b) < 2) continue;
      __typeof__(P.intro) *g = !strncmp(c, "intro", 5) ? &P.intro : !strncmp(c, "recap", 5) ? &P.recap : &P.outro; g->has = 1; g->s = a; g->e = b; } P.skipsOk = 1; return; }
  memset(&P.skOn, 0, sizeof P.skOn); P.sk.tok = ++skTok & 0x3fffffff; if (!P.sk.tok) P.sk.tok = ++skTok; P.sk.d = (int)(dur() + 0.5); P.sk.endAt = now_() + 14; P.sk.pending = 1; /* 1 = still adding jobs */
  /* 0. the file's own chapters, when a reader is there */
  if (player_chapters_hook && !P.isHls) { P.sk.chapWait = 1; P.sk.pending++; player_chapters_hook(P.url, P.isP2p); }
  char u[440]; const char *vid = P.vid; int ne = 0; const char *c = vid; while ((c = strchr(c, ':'))) { ne++; c++; }
  if (!strncmp(vid, "kitsu:", 6)) { const char *a = vid + 6; const char *b = strchr(a, ':'); P.sk.en = b ? atoi(b + 1) : 1; char kid[32]; snprintf(kid, sizeof kid, "%.*s", b ? (int)(b - a) : (int)strlen(a), a);
    if (atoi(kid) > 0) { snprintf(u, sizeof u, "https://kitsu.io/api/edge/anime/%d/mappings?filter%%5BexternalSite%%5D=myanimelist%%2Fanime", atoi(kid)); sk_job(u, SK_KITSU); } }
  const char *imdb = !strncmp(P.id, "tt", 2) ? P.id : !strncmp(vid, "tt", 2) ? vid : NULL;
  if (imdb) { char im[24]; int k = 2; while (k < 22 && isdigit((unsigned char)imdb[k])) k++; snprintf(im, sizeof im, "%.*s", k, imdb);
    P.sk.isEp = !strcmp(P.type, "series") && ne >= 2; if (P.sk.isEp) { const char *l = strrchr(vid, ':'), *pp = l - 1; while (pp > vid && *pp != ':') pp--; P.sk.se = atoi(pp + 1); P.sk.en = atoi(l + 1); }
    if (P.sk.isEp) snprintf(u, sizeof u, "https://api.theintrodb.org/v3/media?imdb_id=%s&season=%d&episode=%d", im, P.sk.se, P.sk.en); else snprintf(u, sizeof u, "https://api.theintrodb.org/v3/media?imdb_id=%s", im); sk_job(u, SK_TIDB);
    if (P.sk.isEp) snprintf(u, sizeof u, "https://api.introdb.app/segments?imdb_id=%s&season=%d&episode=%d", im, P.sk.se, P.sk.en); else snprintf(u, sizeof u, "https://api.introdb.app/segments?imdb_id=%s&is_movie=true", im); sk_job(u, SK_IDB); }
  if (P.sk.tok) sk_done();
}
/* the chapters a reader found in this file: the best skip source ("Opening", "Recap", "End Credits"...); n <= 0: none */
static int ch_word(const char *n, const char *const *w) {
  for (; *w; w++) { size_t l = strlen(*w); for (const char *p = n; (p = strstr(p, *w)); p++) { int a = p == n || !(isalnum((unsigned char)p[-1]) || p[-1] == '_'), b = !(isalnum((unsigned char)p[l]) || p[l] == '_'); if (a && b) return 1; } }
  return 0;
}
void player_chapters(const char *url, const PlayerChapter *list, int n) {
  if (!P.open || !P.sk.chapWait || P.sk.sent || strcmp(url, P.url)) return; P.sk.chapWait = 0;
  static const char *const IN[] = { "intro", "opening", "op", "title sequence", "main title", "main titles", "theme song", "générique", "generique", "genérique", "génerique", "vorspann", NULL };
  static const char *const RE[] = { "recap", "previously", "résumé", "resume", "résume", "resumé", "rückblick", "ruckblick", "summary", NULL };
  static const char *const CR[] = { "credit", "credits", "end credits", "endcredits", "ending", "ed", "outro", "closing", "end title", "end titles", "générique de fin", "generique de fin", "abspann", NULL };
  int has[3] = { 0 }; double ss[3] = { 0 }, ee[3] = { 0 };
  for (int i = 0; i < n; i++) { char nm[128]; lower(list[i].name, nm, sizeof nm); if (list[i].e <= list[i].s) continue;
    const char *q = nm; static const char *const NUM[] = { "chapter", "chapitre", "kapitel", "capítulo", "capitulo", "capitolo", "hoofdstuk", NULL };
    for (int k = 0; NUM[k]; k++) if (!strncmp(q, NUM[k], strlen(NUM[k]))) { q += strlen(NUM[k]); break; }
    while (*q == ' ' || *q == '\t') q++; if (*q) { const char *d = q; while (isdigit((unsigned char)*d)) d++; if (d > q && !*d) continue; } /* "Chapter 3": no name */
    if (strstr(nm, "cold open") || strstr(nm, "teaser")) continue;
    int k = ch_word(nm, IN) && !has[0] ? 0 : ch_word(nm, RE) && !has[1] ? 1 : ch_word(nm, CR) && !has[2] ? 2 : -1;
    if (k < 0) continue; has[k] = 1; ss[k] = list[i].s; ee[k] = list[i].e;
    if (k == 2 && i + 1 < n) { char nx[128]; lower(list[i + 1].name, nx, sizeof nx); const char *pc = strstr(nx, "post"); /* credits, then a preview / post-credits scene: stop there */
      if (strstr(nx, "preview") || strstr(nx, "next episode") || strstr(nx, "next time") || strstr(nx, "stinger") || strstr(nx, "bonus") || (pc && (!strncmp(pc + 4, "credits", 7) || (pc[4] && !strncmp(pc + 5, "credits", 7))))) ee[k] = list[i + 1].s; } }
  if (n > 0) sk_add(0, has, ss, ee);
  sk_done();
}

/* ---- buttons ---- */
static void build_btns(void) {
  const char *fid = P.nbtn && P.bIdx < P.nbtn ? P.btns[P.bIdx].id : NULL; /* the focus stays on the same button when the row changes */
  P.nbtn = 0; int i = 0;
  #define B(ID, IC, LB, BIG) do { P.btns[i].id = ID; P.btns[i].icon = IC; snprintf(P.btns[i].lb, 48, "%s", LB); P.btns[i].big = BIG; P.btns[i].nx = 0; i++; } while (0)
  B("pp", "p_pause", "", 1); if (!P.isTrailer && P.next.has) { B("nxt", "p_next", T("Next episode"), 0); P.btns[i - 1].nx = 1; }
  if (!P.isTrailer) { B("beg", "restart", T("Play from the beginning"), 0); P.btns[i - 1].nx = 1; } /* films and episodes alike */
  P.nL = i;
  if (!P.isTrailer && episodic() && P.videos && cJSON_GetArraySize(P.videos)) B("eps", "p_eps", T("Episodes"), 0);
  if (!P.isTrailer) B("src", "p_src", T("Sources"), 0);
  if (P.isTrailer) { if (trSubsN) B("cc", "p_subs", T("Subtitles"), 0); } /* a trailer's captions: one press on, one off */
  else B("subs", "p_subs", T("Subtitles"), 0);
  if (!P.isTrailer) B("aud", "p_aud", T("Audio"), 0);
  const char *am = !strcmp(P.aspect, "fill") ? "Fill the screen" : !strcmp(P.aspect, "zoom") ? "Zoom" : "Original"; B("asp", "p_asp", T(am), 0);
  #undef B
  P.nbtn = i; if (P.bIdx >= i) P.bIdx = 0; if (fid) for (int k = 0; k < i; k++) if (!strcmp(P.btns[k].id, fid)) P.bIdx = k;
  for (int k = 0; k < i; k++) { P.btns[k].lw = P.btns[k].lb[0] ? text_width(P.btns[k].lb, 24, 700, 0) + 22 : 0; P.btns[k].w = P.btns[k].big ? 94 : 78; }
  /* where they sit before any slides aside (76 / 92 plus a 1 px edge): the left group from the left, the right group from the right */
  float x = 72; for (int k = 0; k < P.nL; k++) { if (P.btns[k].nx) x += 8; P.btns[k].x0 = x; P.btns[k].y = SB_H - 54 - 94 + (94 - P.btns[k].w) / 2; x += P.btns[k].w + (P.btns[k].big ? 20 : 0); }
  x = SB_W - 72; for (int k = i - 1; k >= P.nL; k--) { x -= P.btns[k].w; P.btns[k].x0 = x; P.btns[k].y = SB_H - 54 - 94 + 8; x -= 18; }
  for (int k = 0; k < i; k++) { P.btns[k].x = P.btns[k].x0 + P.btns[k].dx.v; }
}
static int btn_index(const char *id) { for (int i = 0; i < P.nbtn; i++) if (!strcmp(P.btns[i].id, id)) return i; return -1; }

/* ---- OSD ---- */
static void arm_hide(void) { P.hideAt = now_() + 5; }
static void chips_in(void);
static void show_osd(int keepFocus) {
  if (P.mode == 0) { P.mode = 1; if (!keepFocus) { P.row = 0; P.bIdx = 0; } }
  int was = P.osd; P.skm = 0; P.osd = 1; arm_hide();
  if (!was) chips_in(); /* the info chips come in again, one after the other */
}
static void hide_osd(void) { P.mode = 0; P.osd = 0; }
static void toggle(void) { if (paused()) video_play(); else { video_pause(); save_progress(1); } }

/* ---- seeking: presses add up and commit 1.2 s after the last one ---- */
static void seek_by(int dir) {
  double d = dur(); if (d <= 0) return; double n = now_(); int step = atoi(store_get("seekStep", "10")); if (!step) step = 10;
  P.rep = (n - P.lastKeyAt < 0.4 && dir == P.seekDir) ? P.rep + 1 : 0; P.lastKeyAt = n; P.seekDir = dir; step *= P.rep < 4 ? 1 : P.rep < 10 ? 3 : 6;
  double base = P.seekT >= 0 ? P.seekT : video_time(); P.seekT = base + dir * step; if (P.seekT < 0) P.seekT = 0; if (P.seekT > d - 1) P.seekT = d - 1;
  if (P.mode == 0) P.skm = 1; else arm_hide();
  int dd = (int)(P.seekT - video_time() + (P.seekT >= video_time() ? 0.5 : -0.5)), a = dd < 0 ? -dd : dd; char m[16], s[16]; snprintf(m, sizeof m, "%d", a / 60); snprintf(s, sizeof s, "%d", a % 60);
  if (a >= 60) snprintf(P.seekTxt, sizeof P.seekTxt, "%s%s%s%s", dd >= 0 ? "+" : "-", T1("{m}m", "m", m), a % 60 ? " " : "", a % 60 ? T1("{s}s", "s", s) : ""); else snprintf(P.seekTxt, sizeof P.seekTxt, "%s%s", dd >= 0 ? "+" : "-", T1("{s}s", "s", s));
  P.skOn = 1; /* the pill stays put while it is up: only its number changes */ P.seekTxtUntil = 0; P.seekCommitAt = n + 1.2;
}
static void commit_seek(void) { if (P.seekT < 0) return; double t = P.seekT; P.seekT = -1; video_seek(t); P.seekTxtUntil = now_() + 0.9; }
static void cancel_seek(void) { P.seekT = -1; P.skm = 0; P.skOn = 0; P.seekTxtUntil = 0; }

/* ---- picture size, three states always in this order: Original (the whole picture), Fill (fills the screen: a file
   of another shape is cropped to it; a 16:9 file, which already fills it, gets a light crop) and Zoom (a 16:9 file
   with cinema bars baked in loses them; any other shape is cropped further). Chosen per title (a show keeps it from
   episode to episode), else the default from Settings ---- */
static void aspect_key(char *k, size_t cap) { snprintf(k, cap, "aspect_%.*s", (int)strcspn(P.id, ":"), P.id); }
static void aspect_load(void) { char k[96]; aspect_key(k, sizeof k); const char *a = store_get(k, ""); if (!*a) a = store_get("aspectMode", "fill"); snprintf(P.aspect, sizeof P.aspect, "%s", strcmp(a, "stretch") ? a : "fit"); }
/* the web sizes the picture's box (Fill / Zoom go past the screen); a TV's plane blanks for a rectangle that leaves
   the screen, so the part of that box on the screen is shown as a crop of the frame instead */
static void apply_aspect(void) {
  int vw = video_width(), vh = video_height(); float W = SB_W, H = SB_H; P.lastVw = vw; P.lastVh = vh;
  if (!vw || !vh) { video_set_window(0, 0, W, H); return; }
  float fit = fminf(W / vw, H / vh), fill = fmaxf(W / vw, H / vh), k = fit; int wide = fabsf((float)vw / vh - W / H) < 0.03f;
  if (!strcmp(P.aspect, "fill")) k = wide ? fit * 1.12f : fill; else if (!strcmp(P.aspect, "zoom")) k = wide ? fit * 2.39f / (W / H) : fill * 1.15f;
  float fw = roundf(vw * fit), fh = roundf(vh * fit), fx = roundf((W - fw) / 2), fy = roundf((H - fh) / 2);
  if (k == fit) { video_set_window(fx, fy, fw, fh); return; }
  float w = roundf(vw * k), h = roundf(vh * k), x = roundf((W - w) / 2), y = roundf((H - h) / 2);
  float dx = fmaxf(0, x), dy = fmaxf(0, y), dw = fminf(W, x + w) - dx, dh = fminf(H, y + h) - dy;
  int sx = (int)roundf((dx - x) / k), sy = (int)roundf((dy - y) / k), sw = (int)roundf(dw / k), sh = (int)roundf(dh / k);
  if (sx + sw > vw) sw = vw - sx; if (sy + sh > vh) sh = vh - sy;
  if (!video_set_crop(sx, sy, sw, sh, dx, dy, dw, dh)) video_set_window(fx, fy, fw, fh); /* no crop on this TV: the picture stays whole */
}
static void next_aspect(void) {
  static const char *A[3] = { "fit", "fill", "zoom" }; static const char *N[3] = { "Original", "Fill the screen", "Zoom" }; int i = 0; for (int k = 0; k < 3; k++) if (!strcmp(A[k], P.aspect)) i = k;
  int n = (i + 1) % 3; char key[96]; aspect_key(key, sizeof key);
  snprintf(P.aspect, sizeof P.aspect, "%s", A[n]); store_set(key, P.aspect); apply_aspect();
  /* paused: the TV shows the new size with its next frame, when play resumes (asking for the same frame again was a
     seek: the stream buffered, a trailer jumped back) */
  int b = btn_index("asp"); if (b >= 0) { snprintf(P.btns[b].lb, 48, "%s", T(N[n])); P.btns[b].lw = text_width(P.btns[b].lb, 24, 700, 0) + 22; } /* the button names the mode it is in */
}

/* ---- prompts: skip intro / recap, the credits card ---- */
static int in_seg(int has, double s, double e, double t, double tail) { return has && t >= s && t < e - tail; }
static void paint_prompt(void) { if (P.prompt.k) P.shown = P.prompt; }
static void go_next(void);
static void next_auto(void);
static void dismiss_prompt(void) { if (P.prompt.k) P.dismissed[P.prompt.k] = 1; P.prompt.k = 0; if (P.mode == 3) P.mode = 0; paint_prompt(); }
static void prompt_go(int i) {
  if (!P.prompt.k || i >= P.prompt.nbtn) return; const char *id = P.prompt.b[i].id;
  if (!strcmp(id, "skip")) { P.dismissed[P.prompt.k] = 1; double to = P.prompt.b[i].to; P.prompt.k = 0; if (P.mode == 3) P.mode = 0; paint_prompt(); video_seek(to); }
  else if (!strcmp(id, "credits")) dismiss_prompt();
  else go_next();
}
static void check_prompt(void) {
  double d = dur(); if (d <= 0 || P.seekT >= 0) return; double t = video_time(); int want = 0, big = 0;
  if (in_seg(P.intro.has, P.intro.s, P.intro.e, t, 2) && !P.dismissed[1]) want = 1;
  else if (in_seg(P.recap.has, P.recap.s, P.recap.e, t, 2) && !P.dismissed[2]) want = 2;
  else if (!P.dismissed[3] && (P.next.has || P.sim)) { int credits = P.outro.has ? t >= P.outro.s : (d > 600 && d - t <= 90 && d - t > 2); if (credits) { want = 3; big = 1; } }
  if (want == P.prompt.k) return;
  P.prompt.k = want; P.prompt.big = big; P.prompt.autoN = 0; P.prompt.nbtn = 0;
  if (want == 1 || want == 2) { P.prompt.b[0].id = "skip"; snprintf(P.prompt.b[0].lb, 48, "%s", T(want == 1 ? "Skip intro" : "Skip recap")); P.prompt.b[0].to = want == 1 ? P.intro.e : P.recap.e; P.prompt.b[0].main = 0; P.prompt.nbtn = 1;
    /* the pill fills up and skips by itself when full (about 6 s, less when the part ends sooner); OK skips now, a pause stops it */
    if (!paused()) { double e = want == 1 ? P.intro.e : P.recap.e, dd = e - t - 1; P.prompt.autoN = 1; P.prompt.t0 = now_(); P.prompt.adur = dd < 2.5 ? 2.5 : dd > 6 ? 6 : dd; } }
  else if (want == 3) { P.prompt.b[0].id = "credits"; snprintf(P.prompt.b[0].lb, 48, "%s", T("Watch credits")); P.prompt.b[0].main = 0; P.prompt.b[1].id = P.next.has ? "next" : "sim"; snprintf(P.prompt.b[1].lb, 48, "%s", T(P.next.has ? "Next episode" : "Something similar")); P.prompt.b[1].main = 1; P.prompt.nbtn = 2;
    /* credits + autoplay on: "Next episode" fills up and plays by itself unless you pick something */
    if (P.next.has && strcmp(store_get("bingeWatch", "true"), "false") && strcmp(store_get("enableNextVideo", "true"), "false")) { P.prompt.autoN = 1; P.prompt.t0 = now_(); P.prompt.adur = 10; } }
  P.prompt.idx = P.prompt.nbtn ? P.prompt.nbtn - 1 : 0;
  if (want && big && P.mode == 0) P.mode = 3; /* the credits card takes the focus */
  if (!want && P.mode == 3) P.mode = 0;
  paint_prompt();
}

/* ---- next episode: played straight away from the source most like the current one (same addon, resolution, HDR,
   audio, release group, debrid vs torrent); the art covers the switch. With no source at all: the title page's list ---- */
static char closeVid[80];
static void play_stream(const Stream *s, const char *vid, double startAt);
static void prepare(const char *vid, double startAt, int keepSubs);
static int can_play(const Stream *s) { return !(s->p2p || !strncmp(s->url, "p2p://", 6)) || p2p_available(); } /* torrents need the engine library beside the app */
static void rel_group(const char *s, char *out, size_t cap) { /* "-GROUP" at the end of a word (before .mkv / .mp4 / .avi) */
  out[0] = 0; for (const char *p = s; (p = strchr(p, '-')); p++) { const char *q = p + 1; while (isalnum((unsigned char)*q)) q++; size_t n = q - p - 1; if (n < 2 || n > 12) continue;
    const char *r = q; if (*r == '.' && (!strncasecmp(r + 1, "mkv", 3) || !strncasecmp(r + 1, "mp4", 3) || !strncasecmp(r + 1, "avi", 3))) r += 4;
    if (*r && !isspace((unsigned char)*r)) { if (r == q) continue; r = q; if (*r && !isspace((unsigned char)*r)) continue; }
    snprintf(out, cap, "%.*s", (int)n, p + 1); for (char *c = out; *c; c++) *c = (char)tolower((unsigned char)*c); return; }
}
static const char *src_kind(const char *label) {
  char s[700]; lower(label, s, sizeof s); if (strstr(s, "remux")) return "remux"; if (strstr(s, "bluray") || strstr(s, "blu-ray") || strstr(s, "bdrip") || strstr(s, "brrip")) return "bluray";
  int nf = 0; for (const char *p = s; (p = strstr(p, "nf")); p++) if (!isalnum((unsigned char)p[2]) && p[2] != '_') nf = 1;
  if (strstr(s, "webdl") || strstr(s, "web-dl") || strstr(s, "webrip") || has_word(s, "web") || strstr(s, "amzn") || nf || strstr(s, "dsnp") || strstr(s, "hmax") || strstr(s, "atvp")) return "web";
  return strstr(s, "hdtv") ? "tv" : "";
}
static int cached_flag(const char *label) { char s[700]; lower(label, s, sizeof s); if (strstr(s, "cached") || strstr(s, "\xe2\x9a\xa1")) return 1;
  for (const char *p = s; (p = strchr(p, '[')); p++) { int n = 0; while (p[1 + n] >= 'a' && p[1 + n] <= 'z') n++; if (n >= 2 && n <= 3 && p[1 + n] == '+' && p[2 + n] == ']') return 1; } return 0; }
static void info_hash(const char *u, char *out) { out[0] = 0; const char *p = NULL; if (!strncmp(u, "p2p://", 6)) p = u + 6; else if ((p = strstr(u, "ih="))) p += 3; else if ((p = strstr(u, "/torrents/"))) p += 10;
  if (!p) return; int n = 0; while (n < 40 && isxdigit((unsigned char)p[n])) n++; if (n == 40) { for (int i = 0; i < 40; i++) out[i] = (char)tolower((unsigned char)p[i]); out[40] = 0; } }
static int likeness(const Stream *s) {
  char b[700]; sb_stream_info(s, b, sizeof b); const char *a = P.nx.info; int sc = 0;
  if (P.nx.addon[0] && !strcmp(s->addon, P.nx.addon)) sc += 20;
  static const char *RS[5] = { "SD", "720p", "1080p", "1440p", "4K" }; const char *r1 = res_of(a), *r2 = res_of(b);
  if (*r1 && !strcmp(r1, r2)) sc += 30; else if (*r1 && *r2) { int i1 = 0, i2 = 0; for (int i = 0; i < 5; i++) { if (!strcmp(RS[i], r1)) i1 = i; if (!strcmp(RS[i], r2)) i2 = i; } sc -= 12 * abs(i1 - i2); }
  char h1[32], h2[32]; hdr_of(a, h1, sizeof h1); hdr_of(b, h2, sizeof h2); if (!strcmp(h1, h2)) sc += 12;
  char a1[48], a2[48]; audio_of(a, a1, sizeof a1); audio_of(b, a2, sizeof a2); a1[strcspn(a1, " ")] = 0; a2[strcspn(a2, " ")] = 0; if (*a1 && !strcmp(a1, a2)) sc += 10;
  char g1[16], g2[16]; rel_group(a, g1, sizeof g1); rel_group(b, g2, sizeof g2); if (*g1 && !strcmp(g1, g2)) sc += 25;
  const char *k1 = src_kind(a), *k2 = src_kind(b); if (*k1 && !strcmp(k1, k2)) sc += 10;
  int c1 = cached_flag(a), c2 = cached_flag(b); if (c1 == c2) sc += c1 ? 15 : 5;
  int p1 = !strncmp(P.nx.url, "p2p://", 6), p2 = !strncmp(s->url, "p2p://", 6); sc += p1 == p2 ? 15 : -15;
  char x1[48], x2[48]; info_hash(P.nx.url, x1); info_hash(s->url, x2); if (*x1 && !strcmp(x1, x2)) sc += 60; /* the same season pack: the next file of the very same torrent */
  return sc;
}
static unsigned nxTok;
static void nx_finish(void) {
  if (!P.nx.tok || P.nx.done) return; P.nx.done = 1; P.nx.tok = 0;
  if (P.nx.has) { Stream b = P.nx.best; play_stream(&b, P.nx.vid, 0); }
  else { snprintf(closeVid, sizeof closeVid, "%s", P.nx.vid); close_player(1, 0); }
}
static void nx_part(StreamGroup *g, int gi, void *ud) {
  (void)gi; if (P.open && P.nx.tok && (unsigned)(uintptr_t)ud == P.nx.tok && !P.nx.done) {
    /* the same bingeGroup is the addon's own word that it is the same kind of file: taken at once (the official rule) */
    if (P.nx.binge[0]) for (int i = 0; i < g->n; i++) { const Stream *s = &g->items[i]; if (s->url[0] && can_play(s) && !strcmp(s->binge, P.nx.binge)) { P.nx.best = *s; P.nx.has = 1; P.nx.bs = 1000; free(g->items); fprintf(stderr, "[player] next episode: same binge group (%s)\n", P.nx.binge); nx_finish(); return; } }
    for (int i = 0; i < g->n; i++) { const Stream *s = &g->items[i]; if (!s->url[0] || !can_play(s)) continue; int v = likeness(s); if (!P.nx.has || v > P.nx.bs) { P.nx.bs = v; P.nx.best = *s; P.nx.has = 1; } }
    free(g->items); if (P.nx.has && P.nx.bs >= 95) nx_finish(); return; } /* same addon, quality, group: no need to wait for the slow ones */
  free(g->items);
}
static void nx_done(int total, void *ud) { (void)total; if (P.open && P.nx.tok && (unsigned)(uintptr_t)ud == P.nx.tok) nx_finish(); }
static void next_auto(void) {
  if (!P.next.has) { go_next(); return; }
  mark_watched(); save_progress(1);
  char nv[80], info[700], url[SB_URL_MAX], addon[96], binge[96]; snprintf(nv, sizeof nv, "%s", P.next.vid); snprintf(info, sizeof info, "%s", P.sname); snprintf(url, sizeof url, "%s", P.st.url[0] ? P.st.url : P.url); snprintf(addon, sizeof addon, "%s", P.saddon); snprintf(binge, sizeof binge, "%s", P.st.binge);
  prepare(nv, 0, 0);
  snprintf(P.nx.vid, sizeof P.nx.vid, "%s", nv); snprintf(P.nx.info, sizeof P.nx.info, "%s", info); snprintf(P.nx.binge, sizeof P.nx.binge, "%s", binge); snprintf(P.nx.url, sizeof P.nx.url, "%s", url); snprintf(P.nx.addon, sizeof P.nx.addon, "%s", addon);
  set_status(T("Finding the closest source\xe2\x80\xa6")); P.nx.tok = ++nxTok; if (!P.nx.tok) P.nx.tok = ++nxTok; P.nx.until = now_() + 15;
  sb_streams(P.type, nv, nx_part, nx_done, (void *)(uintptr_t)P.nx.tok);
}
static void go_next(void) {
  if (P.next.has && !P.nextTried) { P.nextTried = 1; next_auto(); }
  else if (P.next.has) { mark_watched(); snprintf(closeVid, sizeof closeVid, "%s", P.next.vid); close_player(1, 0); }
  else if (P.sim) close_player(0, 1);
}

/* ---- trays ---- */
static int focusable(const TItem *it) { return it && it->k != 0 && it->k != 1; }
static int first_on(const TItem *l, int n) { for (int i = 0; i < n; i++) if (l[i].on && focusable(&l[i])) return i; for (int i = 0; i < n; i++) if (focusable(&l[i])) return i; return -1; }
static int season_of(const char *vid) { const char *c = strrchr(vid, ':'); if (!c) return 1; const char *p = c - 1; while (p > vid && *p != ':') p--; return *p == ':' ? atoi(p + 1) : 1; }
static TItem *add_item(TItem *list, int *n, int max, int k) { if (*n >= max) return NULL; TItem *it = &list[(*n)++]; memset(it, 0, sizeof *it); it->k = k; return it; }
static TItem *col_add(TCol *c, int k) { c->items = realloc(c->items, (c->n + 1) * sizeof *c->items); TItem *it = &c->items[c->n++]; memset(it, 0, sizeof *it); it->k = k; return it; }
static void cols_clear(void) { for (int i = 0; i < 4; i++) { free(P.tray.cols[i].items); P.tray.cols[i].items = NULL; P.tray.cols[i].n = 0; } P.tray.ncols = 0; }
/* ---- the sources tray: a header per addon serving the video, in the user's order; each addon is asked only when its
   header comes into view or is opened (no wait for the slowest), and everything is let go when the tray closes ---- */
typedef struct { int k; /* 0 a note, 1 waiting, 3 a stream, 5 an addon's header */ int g, i; float y, h; } SRow;
static struct { SRow *r; int n, cap, idx; Mo y, plY, plH, plA; float tot; } SR;
static unsigned srcAsk;
/* the trays' small motions, all tweens on quads already drawn: the tab pill gliding (and lighting up when focused),
   the contents swapping tabs (out and in, sliding the way of the move), the column's backdrop following the focus,
   a list fading in when what it lists changes, the tick fading in where the choice went, a value fading up when it
   changes, the lit dot of a value that cycles gliding to its place */
static const float C_GLIDE[4] = { .25f, 1, .5f, 1 }; /* GLIDE_EASE's curve, for the Mo motions */
static struct {
  Mo tabX, tabW, tabF; int tabSeen; Mo swA; int swDir, swPend; Mo colBX, colBA; int colBSeen; int quiet;
  unsigned colSig[4], rowSig; Mo colA[4], rowA; int onIdx[4], rowOn; Mo chkA[4], rowChk; int vkC; char vkU[8]; Mo vkA;
  Mo dot[4]; int dotSeen[4]; /* 0 the picture size, 1 subtitle size, 2 colour, 3 background */
  Mo swK[2]; int swFrom[2], swCur[2], swSeen[2]; /* the colour and background swatches: the one before, the one now, how far it has come */
} TM;
static void tm_tick(float dt) {
  Mo *m[] = { &TM.tabX, &TM.tabW, &TM.tabF, &TM.swA, &TM.colBX, &TM.colBA, &TM.rowA, &TM.rowChk, &TM.vkA, &TM.dot[0], &TM.dot[1], &TM.dot[2], &TM.dot[3], &TM.swK[0], &TM.swK[1] };
  for (size_t i = 0; i < sizeof m / sizeof *m; i++) mo_tick(m[i], dt); for (int i = 0; i < 4; i++) { mo_tick(&TM.colA[i], dt); mo_tick(&TM.chkA[i], dt); }
}
static void tm_reset(void) { /* a tray opening: everything where it is, nothing gliding in from an old place */
  TM.tabSeen = TM.colBSeen = TM.swPend = 0; mo_set(&TM.swA, 1); mo_set(&TM.colBA, 0); mo_set(&TM.tabF, 0); mo_set(&TM.vkA, 1); TM.vkU[0] = 0;
  for (int i = 0; i < 4; i++) { mo_set(&TM.colA[i], 1); mo_set(&TM.chkA[i], 1); TM.onIdx[i] = -2; TM.colSig[i] = 0; } mo_set(&TM.rowA, 1); mo_set(&TM.rowChk, 1); TM.rowOn = -2; TM.rowSig = 0;
  for (int i = 1; i < 4; i++) TM.dotSeen[i] = 0; TM.swSeen[0] = TM.swSeen[1] = 0;
}
static unsigned str_sig(unsigned h, const char *s) { while (*s) h = h * 31 + (unsigned char)*s++; return h * 31 + 7; }
static int item_on(const TItem *it) { return strncmp(it->u, "lang:", 5) ? it->on && !it->tool : it->d > 0; }
/* after each rebuild: a list whose items changed fades in, a tick that moved fades in at its new place */
static void tm_after(void) {
  for (int c = 0; c < 4; c++) { unsigned h = 1; int on = -1; const TCol *col = &P.tray.cols[c]; if (c < P.tray.ncols) for (int i = 0; i < col->n; i++) { h = str_sig(h * 13 + (unsigned)col->items[i].k, col->items[i].lb); if (!col->items[i].tool) h = str_sig(h, col->items[i].sub); /* (a tool's line under it is its state, not the list) */ if (on < 0 && item_on(&col->items[i])) on = i; }
    if (!TM.quiet && TM.colSig[c] && h != TM.colSig[c]) { mo_set(&TM.colA[c], 0); mo_to(&TM.colA[c], 1, .22f, C_OUT, 0); }
    if (!TM.quiet && TM.onIdx[c] != -2 && on != TM.onIdx[c] && on >= 0) { mo_set(&TM.chkA[c], 0); mo_to(&TM.chkA[c], 1, .2f, C_EASE, 0); }
    TM.colSig[c] = h; TM.onIdx[c] = on; }
  unsigned h = 1; int on = -1; for (int i = 0; i < P.tray.n; i++) { h = str_sig(h * 13 + (unsigned)P.tray.row[i].k, P.tray.row[i].lb); if (on < 0 && P.tray.row[i].on) on = i; }
  if (!TM.quiet && TM.rowSig && h != TM.rowSig) { mo_set(&TM.rowA, 0); mo_to(&TM.rowA, 1, .22f, C_OUT, 0); }
  if (!TM.quiet && TM.rowOn != -2 && on != TM.rowOn && on >= 0) { mo_set(&TM.rowChk, 0); mo_to(&TM.rowChk, 1, .2f, C_EASE, 0); }
  TM.rowSig = h; TM.rowOn = on;
}
/* a value that cycles (the picture size, the subtitles' style): one dot per choice, the lit one gliding to the current */
static void cycle_dots(int which, int n, int cur, float x, float y, float gap, float r, uint32_t dim, uint32_t lit, int alignRight) {
  /* the current choice is a short pill, the others dots; as the choice moves the pill flows from one into the next (each
     dot's length follows how near the gliding choice is to it), the row staying centred on x (or ending at x) */
  if (!TM.dotSeen[which]) { mo_set(&TM.dot[which], (float)cur); TM.dotSeen[which] = 1; } else mo_to(&TM.dot[which], (float)cur, GLIDE_DUR * .9f, C_GLIDE, 0);
  float d = 2 * r, L = 3.4f * d, sp = gap - d, w[8], tot = 0; if (n > 8) n = 8;
  for (int i = 0; i < n; i++) { float k = 1 - fabsf(i - TM.dot[which].v); k = k < 0 ? 0 : k; w[i] = d + (L - d) * k; tot += w[i] + (i ? sp : 0); }
  float xx = alignRight ? x - tot : x - tot / 2;
  for (int i = 0; i < n; i++) { float k = 1 - fabsf(i - TM.dot[which].v); k = k < 0 ? 0 : k;
    gfx_rrect(xx, y - r, w[i], d, r, k > 0.01f ? col_mix(dim, lit, k) : dim); xx += w[i] + sp; }
}
/* the subtitles' colour and background as a round swatch: the colour itself; no background a small checkerboard, the
   transparent one that checkerboard darkened, solid plain black. A new choice comes in over the old one, growing a
   little into place as the old one fades (one short ease, nothing that repeats) */
static GLuint checkTex;
static void swatch_one(int which, int v, float cx, float cy, float r, float a) {
  if (a <= 0.004f) return; uint32_t A = (uint32_t)(a * 255 + .5f) << 24;
  if (which == 0) gfx_circle(cx, cy, r, (hexcol(SUB_COLORS[v]) & 0xffffff) | A);
  else if (v == 1) gfx_circle(cx, cy, r, A);
  else { if (!checkTex) { unsigned char px[4 * 4 * 4]; for (int i = 0; i < 16; i++) { unsigned char g = ((i & 3) + (i >> 2)) & 1 ? 150 : 236; px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = g; px[i * 4 + 3] = 255; } checkTex = gfx_tex_create(px, 4, 4, 4, 0); }
    uint32_t tint = v == 2 ? RGB(102, 102, 102) : RGB(255, 255, 255); /* (.6 black over it) */
    gfx_image(checkTex, cx - r, cy - r, 2 * r, 2 * r, r, (tint & 0xffffff) | A, 0, 0, 1, 1); }
}
static void swatch_draw(int which, int v, float cx, float cy, float r) {
  if (!TM.swSeen[which]) { TM.swSeen[which] = 1; TM.swCur[which] = TM.swFrom[which] = v; mo_set(&TM.swK[which], 1); }
  else if (v != TM.swCur[which]) { TM.swFrom[which] = TM.swCur[which]; TM.swCur[which] = v; mo_set(&TM.swK[which], 0); mo_to(&TM.swK[which], 1, .22f, C_OUT, 0); }
  float k = TM.swK[which].v;
  if (k < 1) swatch_one(which, TM.swFrom[which], cx, cy, r, 1 - k);
  swatch_one(which, v, cx, cy, r * (.84f + .16f * k), k < 1 ? fminf(1, k * 1.6f) : 1);
  gfx_ring(cx - r, cy - r, 2 * r, 2 * r, r, 1.5f, WHITE_A(64)); /* its edge, so black shows on the dark tray */
}
#define SRC_W 1000.0f        /* the tray */
#define SRC_RW (SRC_W - 62)  /* a row */
#define SRC_LH 600.0f        /* the list's window at most */
#define SRC_VH (fminf(SRC_LH, fmaxf(150.0f, SR.tot - 10))) /* the list's window: as tall as what is in it (groups folded, it shrinks), up to SRC_LH */
static int src_tray(void) { int ak = P.tray.kind < 0 ? -P.tray.kind : P.tray.kind; return ak == 2 || ak == 5; }
static void srcs_free(void) { for (int i = 0; i < P.srcs.ng; i++) { free(P.srcs.g[i].items); free(P.srcs.g[i].h); } memset(&P.srcs, 0, sizeof P.srcs); free(SR.r); SR.r = NULL; SR.n = SR.cap = 0; SR.idx = -1; }
static void src_rows(void);
static int src_find(unsigned tok) { if (!tok) return -1; for (int i = 0; i < P.srcs.ng; i++) if (P.srcs.g[i].tok == tok) return i; return -1; }
/* found stream sources, kept for the session (text only): the sources tray shows them at once
   instead of asking the addons again. Memory only: gone when the app closes, fresh next start. */
#define SCACHE_G 16
#define SCACHE_N 64
static struct { char key[160]; int ng; struct { char name[96]; Stream *items; int n; } g[SCACHE_G]; } scache[SCACHE_N];
static int scacheN;
static void scache_key(char *o, size_t n) { snprintf(o, n, "%s|%s", P.type, P.srcs.vid); }
static int scache_find(const char *key) { for (int i = 0; i < scacheN; i++) if (!strcmp(scache[i].key, key)) return i; return -1; }
static void scache_store(int gi) { /* a finished group lands in the session cache */
  __typeof__(P.srcs.g[0]) *g = &P.srcs.g[gi];
  if (!g->items || !g->n) return;
  char key[160]; scache_key(key, sizeof key); int e = scache_find(key);
  if (e < 0) { if (scacheN >= SCACHE_N) return; e = scacheN++; snprintf(scache[e].key, sizeof scache[e].key, "%s", key); }
  int s = -1; for (int i = 0; i < scache[e].ng; i++) if (!strcmp(scache[e].g[i].name, g->name)) s = i;
  if (s < 0) { if (scache[e].ng >= SCACHE_G) return; s = scache[e].ng++; snprintf(scache[e].g[s].name, sizeof scache[e].g[s].name, "%s", g->name); }
  free(scache[e].g[s].items);
  scache[e].g[s].items = malloc(sizeof(Stream) * (size_t)g->n); if (!scache[e].g[s].items) { scache[e].g[s].n = 0; return; }
  memcpy(scache[e].g[s].items, g->items, sizeof(Stream) * (size_t)g->n); scache[e].g[s].n = g->n; /* Stream is flat text: a copy is a copy */
}
static void scache_install(void) { /* the tray's groups take what an earlier visit found */
  char key[160]; scache_key(key, sizeof key); int e = scache_find(key); if (e < 0) return;
  for (int i = 0; i < P.srcs.ng; i++) { __typeof__(P.srcs.g[0]) *g = &P.srcs.g[i];
    for (int s = 0; s < scache[e].ng; s++) if (!strcmp(scache[e].g[s].name, g->name) && scache[e].g[s].n > 0) {
      Stream *cp = malloc(sizeof(Stream) * (size_t)scache[e].g[s].n); if (!cp) break;
      memcpy(cp, scache[e].g[s].items, sizeof(Stream) * (size_t)scache[e].g[s].n);
      free(g->items); g->items = cp; g->n = scache[e].g[s].n;
      free(g->h); g->h = calloc(g->n ? g->n : 1, sizeof(float)); g->st = 2; break; } }
}
static void on_src_part(StreamGroup *g, int gi, void *ud) { (void)gi; int k = P.open ? src_find((unsigned)(uintptr_t)ud) : -1; if (k < 0) { free(g->items); return; } /* a list for a tray since closed: dropped */
  free(P.srcs.g[k].items); free(P.srcs.g[k].h); P.srcs.g[k].items = g->items; P.srcs.g[k].n = g->n; P.srcs.g[k].h = calloc(g->n ? g->n : 1, sizeof(float)); /* the list is ours now; its rows measured once, as they come */
  if (!P.srcs.g[k].h) { free(g->items); P.srcs.g[k].items = NULL; P.srcs.g[k].n = 0; } }
static void on_src_done(int total, void *ud) { (void)total; int k = P.open ? src_find((unsigned)(uintptr_t)ud) : -1; if (k < 0) return; P.srcs.g[k].tok = 0; P.srcs.g[k].st = P.srcs.g[k].n ? 2 : 3;
  if (P.srcs.g[k].st == 2) scache_store(k); /* found sources stay for the session */
  __typeof__(P.srcs.g[0]) *g = &P.srcs.g[k]; int any = 0; for (int i = 0; i < g->n; i++) if (g->items[i].url[0]) any = 1;
  if (!any && g->open && !g->touched) { g->open = 0; g->leaving = g->seen; if (g->seen) mo_to(&g->fa, 0, .2f, C_EASE, 0); } /* nothing from it: its header alone, folded (opened again on OK) */
  else if (g->seen) { mo_set(&g->ia, 0); mo_to(&g->ia, 1, .24f, C_OUT, 0); } /* its rows fade in where the waiting ones were */
  if (src_tray()) src_rows(); }
/* a folded list's rows go once it has closed */
static void src_tick(float dt) { int gone = 0;
  for (int i = 0; i < P.srcs.ng; i++) { __typeof__(P.srcs.g[0]) *g = &P.srcs.g[i]; mo_tick(&g->fa, dt); mo_tick(&g->bh, dt); mo_tick(&g->ia, dt);
    if (g->leaving && !g->fa.on && !g->bh.on) { g->leaving = 0; gone = 1; } }
  if (gone && src_tray()) src_rows(); }
static void src_ask(int k) { if (k < 0 || k >= P.srcs.ng || P.srcs.g[k].st) return; P.srcs.g[k].st = 1; if (!++srcAsk) ++srcAsk; P.srcs.g[k].tok = srcAsk;
  src_rows(); sb_streams_one(P.type, P.srcs.vid, k, on_src_part, on_src_done, (void *)(uintptr_t)srcAsk); } /* (an addon that is gone answers at once: empty) */
static void srcs_begin(const char *vid) {
  if (P.srcs.on && !strcmp(P.srcs.vid, vid)) return; srcs_free(); snprintf(P.srcs.vid, sizeof P.srcs.vid, "%s", vid); P.srcs.on = 1;
  char names[16][96]; int n = sb_stream_addons(P.type, vid, names, 16);
  for (int i = 0; i < n; i++) { snprintf(P.srcs.g[i].name, sizeof P.srcs.g[i].name, "%s", names[i][0] ? names[i] : T("Addon")); P.srcs.g[i].open = 1; } P.srcs.ng = n;
  scache_install(); /* found earlier this session: already there, no asking again */
}
static int src_on(const Stream *s) { char info[700]; sb_stream_info(s, info, sizeof info); return !strcmp(s->url, P.url) || !strcmp(s->url, P.st.url) || !strcmp(s->url, P.alt) || (P.sname[0] && !strncmp(info, P.sname, 200) && !strcmp(s->addon, P.saddon)); }
/* a stream row's height: its name (up to 3 lines) and its words (up to 6), each cut with an ellipsis past that */
#define SN_PX 27      /* a stream's name: 27 px on 34 */
#define SN_LH 34.0f
#define SD_PX 24      /* its words: 24 px on 33, at most SD_MAX lines */
#define SD_LH 33.0f
#define SD_MAX 6
static float src_row_h(const Stream *s) {
  int nl = text_wrap_count(s->name[0] ? s->name : s->addon, SRC_RW - 44 - 150, 3, SN_PX, 700, 0), dl = s->title[0] ? text_wrap_count(s->title, SRC_RW - 44, SD_MAX, SD_PX, 400, 0) : 0;
  return 18 + (nl ? nl : 1) * SN_LH + (dl ? 8 + dl * SD_LH : 0) + 18; }
static SRow *sr_add(int k, int g, int i) { if (SR.n >= SR.cap) { int c = SR.cap ? SR.cap * 2 : 64; SRow *r = realloc(SR.r, c * sizeof *r); if (!r) return NULL; SR.r = r; SR.cap = c; } SRow *x = &SR.r[SR.n++]; memset(x, 0, sizeof *x); x->k = k; x->g = g; x->i = i; return x; }
static int src_focusable(int i) { return i >= 0 && i < SR.n && (SR.r[i].k == 5 || (SR.r[i].k == 3 && P.srcs.g[SR.r[i].g].open)); } /* (a folding list's rows are only drawn) */
static void src_rows(void) {
  int fk = -1, fg = -1, fi = -1; if (src_focusable(SR.idx)) { fk = SR.r[SR.idx].k; fg = SR.r[SR.idx].g; fi = SR.r[SR.idx].i; }
  SR.n = 0;
  for (int g = 0; g < P.srcs.ng; g++) { sr_add(5, g, -1); if (!P.srcs.g[g].open && !P.srcs.g[g].leaving) continue; int st = P.srcs.g[g].st;
    if (st <= 1) { sr_add(1, g, 0); sr_add(1, g, 1); }
    else { int any = 0; for (int i = 0; i < P.srcs.g[g].n; i++) if (P.srcs.g[g].items[i].url[0]) { sr_add(3, g, i); any = 1; } if (!any) sr_add(0, g, 0); } }
  if (!P.srcs.ng) sr_add(0, -1, 0);
  /* where each row ends up; a folding list's rows stay under their header (they take no room) */
  float y = 0, body = 0; int bg = -1;
  for (int i = 0; i <= SR.n; i++) { SRow *r = i < SR.n ? &SR.r[i] : NULL;
    if ((!r || r->k == 5) && bg >= 0) { __typeof__(P.srcs.g[0]) *g = &P.srcs.g[bg]; g->bhT = g->open ? body : 0; if (g->open) y += body;
      if (!g->seen) { mo_set(&g->bh, g->bhT); mo_set(&g->fa, g->open); mo_set(&g->ia, 1); g->seen = 1; } else mo_to(&g->bh, g->bhT, GLIDE_DUR, C_GLIDE, 0); bg = -1; }
    if (!r) break;
    if (r->k == 5 && i) y += 14;
    if (r->k == 3) { float *h = &P.srcs.g[r->g].h[r->i]; if (*h <= 0) *h = src_row_h(&P.srcs.g[r->g].items[r->i]); r->h = *h; } else r->h = r->k == 5 ? 60 : r->k == 1 ? 18 + SN_LH + 8 + SD_LH + 18 : 60;
    if (r->k == 5) { r->y = y; y += r->h + 10; bg = r->g; body = 0; }
    else if (bg >= 0) { r->y = y + body; body += r->h + 10; }
    else { r->y = y; y += r->h + 10; } }
  SR.tot = y;
  SR.idx = -1; if (fk >= 0) for (int i = 0; i < SR.n; i++) if (SR.r[i].k == fk && SR.r[i].g == fg && SR.r[i].i == fi) { SR.idx = i; break; }
  if (SR.idx < 0 && fg >= 0) for (int i = 0; i < SR.n; i++) if (SR.r[i].k == 5 && SR.r[i].g == fg) { SR.idx = i; break; } /* its row went: its addon's header */
  if (SR.idx < 0) for (int i = 0; i < SR.n; i++) if (SR.r[i].k == 3 && src_on(&P.srcs.g[SR.r[i].g].items[SR.r[i].i])) { SR.idx = i; break; } /* fresh: the one playing, else the first */
  if (SR.idx < 0) for (int i = 0; i < SR.n; i++) if (src_focusable(i)) { SR.idx = i; break; }
}
static void src_choose(int i) {
  if (!src_focusable(i)) return; SRow r = SR.r[i];
  if (r.k == 5) { __typeof__(P.srcs.g[0]) *g = &P.srcs.g[r.g]; g->open = !g->open; g->touched = 1; g->leaving = !g->open; /* a header: its list slides and fades open or folds away, the rows below glide */
    mo_to(&g->fa, g->open, g->open ? .26f : .2f, g->open ? C_OUT : C_EASE, 0); if (g->open) src_ask(r.g); src_rows(); return; }
  int same = P.tray.kind == 2; char v[80]; snprintf(v, sizeof v, "%s", same ? P.vid : P.tray.epvid); Stream s = P.srcs.g[r.g].items[r.i]; play_stream(&s, v, same ? video_time() : 0);
}
static void tray_data(void) {
  struct { int kind; } t = { P.tray.kind }; P.tray.ntabs = 0; P.tray.n = 0; cols_clear(); P.tray.title[0] = 0;
  if (t.kind == 4) { /* subtitles: four columns; with subtitles addons installed (or subtitles the stream carried), a second tab lists theirs by language */
    TCol *c1 = &P.tray.cols[0], *c2 = &P.tray.cols[1], *c3 = &P.tray.cols[2], *c4 = &P.tray.cols[3]; P.tray.ncols = 4;
    int addonsTab = 0; if (!P.isTrailer && (P.nSubAddons > 0 || P.nAdx > 0)) { P.tray.ntabs = 2; snprintf(P.tray.tabs[0], 48, "%s", T("Subtitles")); snprintf(P.tray.tabs[1], 48, "%s", T("Addons")); if (P.tray.tab > 1) P.tray.tab = 0; addonsTab = P.tray.tab == 1; } else P.tray.tab = 0;
    snprintf(c3->title, 64, "%s", T("Style")); snprintf(c4->title, 64, "%s", T("Delay"));
    { char dl[24]; snprintf(dl, sizeof dl, "%s%.2f", P.subDelay > 0 ? "+" : "", P.subDelay); snprintf(c4->vl, 32, "%s", T1("{n} s", "n", dl)); }
    TItem *x;
    if (!addonsTab) {
      snprintf(c1->title, 64, "%s", P.isTrailer ? T("Subtitles") : T("In the file")); snprintf(c2->title, 64, "%s", P.isTrailer ? T("Languages") : "OpenSubtitles");
      x = col_add(c1, 4); snprintf(x->lb, 200, "%s", T("Off")); x->on = !strcmp(P.curSub, "off"); snprintf(x->u, 1024, "off");
      if (!P.isTrailer) { if (!P.embKnown && !P.nEmb) { col_add(c1, 1); col_add(c1, 1); } /* the file's head is still being read */ else if (!P.nEmb) { x = col_add(c1, 0); snprintf(x->lb, 200, "%s", T("None in this file")); } else for (int i = 0; i < P.nEmb; i++) { x = col_add(c1, 4); emb_label(i, x->lb, 200); x->i = P.emb[i].idx; char k[16]; snprintf(k, sizeof k, "emb:%d", P.emb[i].idx); x->on = !strcmp(P.curSub, k); x->spin = x->on && SL.wait; snprintf(x->u, 1024, "%s", k); } }
      /* the built-in OpenSubtitles list (the addons' are in their own tab) */
      int onExt = sub_row(1, P.ext, P.nExt);
      for (int i = 0; i < P.nExt; i++) { int same = 0; for (int j = 0; j < P.nExt; j++) if (!strcmp(P.ext[j].lang, P.ext[i].lang)) same++; x = col_add(c2, 4); if (same > 1) snprintf(x->lb, 200, "%s %d", T(P.ext[i].name), P.ext[i].n); /* the page's two spaces show as one */ else snprintf(x->lb, 200, "%s", T(P.ext[i].name)); x->on = i == onExt; x->spin = x->on && SL.wait; x->ls = 1; x->i = i; snprintf(x->u, 1024, "%s", P.ext[i].url); }
      if (!c2->n) { if (P.extLoading && !P.isTrailer) { col_add(c2, 1); col_add(c2, 1); col_add(c2, 1); } else { x = col_add(c2, 0); snprintf(x->lb, 200, "%s", T("None found")); } }
    } else {
      /* the addons' tab: their languages (the one you read first, then by name), and the chosen language's subtitles
         from every addon, each with its addon's name */
      const char *w = want_sub(); char keys[64][24], names[64][64], onKey[24] = ""; int cnt[64], nk = 0, onAdx = sub_row(2, P.adx, P.nAdx);
      if (onAdx >= 0) subs_lang_canon(P.adx[onAdx].lang, onKey, sizeof onKey);
      for (int i = 0; i < P.nAdx; i++) { const SubEntry *e = &P.adx[i]; char k[24]; subs_lang_canon(e->lang, k, sizeof k); int f = -1; for (int q = 0; q < nk; q++) if (!strcmp(keys[q], k)) f = q;
        if (f < 0 && nk < 64) { f = nk++; snprintf(keys[f], 24, "%s", k); snprintf(names[f], 64, "%s", T(e->name)); cnt[f] = 0; } if (f >= 0) cnt[f]++; }
      for (int a = 1; a < nk; a++) for (int b = a; b > 0; b--) { int pa = lang_match(keys[b - 1], w), pb = lang_match(keys[b], w); if (pb > pa || (pb == pa && strcasecmp(names[b - 1], names[b]) > 0)) { char t1[24], t2[64]; int tc = cnt[b]; snprintf(t1, 24, "%s", keys[b]); snprintf(t2, 64, "%s", names[b]); snprintf(keys[b], 24, "%s", keys[b - 1]); snprintf(names[b], 64, "%s", names[b - 1]); cnt[b] = cnt[b - 1]; snprintf(keys[b - 1], 24, "%s", t1); snprintf(names[b - 1], 64, "%s", t2); cnt[b - 1] = tc; } else break; }
      int have = 0; for (int q = 0; q < nk; q++) if (!strcmp(keys[q], P.subLang)) have = 1;
      if (!have) { int pick = -1; /* the language of the subtitle on, else the one you read, else the first */
        if (onKey[0]) for (int q = 0; q < nk; q++) if (!strcmp(keys[q], onKey)) pick = q;
        if (pick < 0 && nk) pick = 0; if (pick >= 0) snprintf(P.subLang, sizeof P.subLang, "%s", keys[pick]); }
      snprintf(c1->title, 64, "%s", T("Language")); c2->title[0] = 0;
      for (int q = 0; q < nk; q++) { x = col_add(c1, 4); snprintf(x->lb, 200, "%s", names[q]); snprintf(x->vl, 48, "%d", cnt[q]); snprintf(x->u, 1024, "lang:%s", keys[q]); x->on = !strcmp(keys[q], P.subLang); if (x->on) snprintf(c2->title, 64, "%s", names[q]);
        if (onKey[0] && !strcmp(onKey, keys[q])) x->d = 1; } /* d: the tick (a subtitle of this language is on); on: the language shown beside */
      if (!nk) { if (P.adxLoading) { col_add(c1, 1); col_add(c1, 1); col_add(c1, 1); } else { x = col_add(c1, 0); snprintf(x->lb, 200, "%s", T("None found")); } }
      if (!c2->title[0]) snprintf(c2->title, 64, "%s", T("Subtitles"));
      for (int i = 0; i < P.nAdx; i++) { const SubEntry *e = &P.adx[i]; char k[24]; if (strcmp(subs_lang_canon(e->lang, k, sizeof k), P.subLang)) continue;
        int same = 0; for (int j = 0; j < P.nAdx; j++) { char k2[24]; if (P.adx[j].order == e->order && !strcmp(subs_lang_canon(P.adx[j].lang, k2, sizeof k2), k)) same++; }
        x = col_add(c2, 4); if (e->label[0]) snprintf(x->lb, 200, "%s", e->label); else if (same > 1) snprintf(x->lb, 200, "%s %d", T(e->name), e->n); else snprintf(x->lb, 200, "%s", T(e->name));
        snprintf(x->sub, 120, "%s", e->addon); x->on = i == onAdx; x->spin = x->on && SL.wait; x->ls = 2; x->i = i; snprintf(x->u, 1024, "%s", e->url); }
      if (!c2->n && nk) { x = col_add(c2, 0); snprintf(x->lb, 200, "%s", T("None found")); }
      else if (!c2->n && P.adxLoading) { col_add(c2, 1); col_add(c2, 1); }
    }
    int ci = color_idx(); x = col_add(c3, 4); x->tool = 1; snprintf(x->lb, 200, "%s", T("Size")); snprintf(x->vl, 48, "%d%%", SUB_SIZES[size_idx()]); snprintf(x->u, 1024, "size");
    x = col_add(c3, 4); x->tool = 1; snprintf(x->lb, 200, "%s", T("Colour")); snprintf(x->u, 1024, "color"); /* colour and background: shown by their swatch (drawn with the column) */
    x = col_add(c3, 4); x->tool = 1; snprintf(x->lb, 200, "%s", T("Background")); snprintf(x->u, 1024, "bg"); (void)ci;
    if (!P.isTrailer && !strncmp(P.curSub, "ext:", 4)) { x = col_add(c4, 4); x->tool = 1; snprintf(x->lb, 200, "%s", T("Auto-sync")); snprintf(x->sub, 120, "%s", SY.st); snprintf(x->u, 1024, "sync"); } /* a downloaded subtitle: lined up with the file's own */
    x = col_add(c4, 4); x->tool = 1; snprintf(x->lb, 200, "%s", T("Adjust")); snprintf(x->sub, 120, "%s", T("While you watch")); snprintf(x->u, 1024, "dlyopen"); /* the small card at the top: the film stays in view */
    x = col_add(c4, 4); x->tool = 1; x->d = 0; snprintf(x->lb, 200, "%s", T("Reset")); snprintf(x->u, 1024, "dly");
    if (P.isTrailer) { /* one list: Off + the languages */ TCol *m = c1; for (int i = 0; i < c2->n; i++) if (c2->items[i].k == 4) { TItem *y = col_add(m, 4); *y = c2->items[i]; } free(c2->items); P.tray.cols[1] = P.tray.cols[2]; P.tray.cols[2] = P.tray.cols[3]; memset(&P.tray.cols[3], 0, sizeof P.tray.cols[3]); P.tray.ncols = 3; }
  } else if (t.kind == 3 && P.dts.conv) { snprintf(P.tray.title, 300, "%s", T("Audio")); MkvAudio ma[16]; int m = mkv_audio_tracks(mkv, ma, 16), on = P.dts.convOrd;
    /* converted here: the file's own tracks (another one is converted from the same moment) */
    if (on < 0) for (int i = 0; i < m; i++) if (aud_kind(ma[i].codec)) { on = i; break; }
    /* (with the picture converted too: every track the stream can carry, the ones this TV can't decode as Dolby Digital) */
    for (int i = 0; i < m; i++) { int asIs = P.dts.dv && !mkv_aud_bad(&ma[i]) && aud_copyable(ma[i].codec); if (!asIs && !aud_kind(ma[i].codec)) continue;
      TItem *x = add_item(P.tray.row, &P.tray.n, MAXROW, 4); const char *ln = lang_name(ma[i].lang);
      if (*ln) snprintf(x->lb, 200, "%s", ln); else if (ma[i].lang[0]) snprintf(x->lb, 200, "%s", ma[i].lang); else { char k[8]; snprintf(k, sizeof k, "%d", i + 1); snprintf(x->lb, 200, "%s %s", T("Audio"), k); }
      if (asIs) { VidTrack v; memset(&v, 0, sizeof v); snprintf(v.codec, sizeof v.codec, "%s", ma[i].codec); v.channels = ma[i].channels; aud_label(&v, x->sub, 120); }
      else snprintf(x->sub, 120, "%s", ma[i].channels >= 6 ? T("Dolby Digital 5.1 (converted)") : T("Dolby Digital 2.0 (converted)"));
      x->i = 1000 + i; x->on = i == on; }
    if (!P.tray.n) { TItem *x = add_item(P.tray.row, &P.tray.n, MAXROW, 4); snprintf(x->lb, 200, "%s", T("Dolby Digital 5.1 (converted)")); x->i = 1000 + on; x->on = 1; }
  } else if (t.kind == 3) { snprintf(P.tray.title, 300, "%s", T("Audio")); VidTrack ts[16]; int n = video_audio_tracks(ts, 16); int sel = P.curAud >= 0 ? P.curAud : video_audio_current();
    /* the language is the name; the format (AC3 5.1, EAC3 Atmos...) the line under it */
    for (int i = 0; i < n; i++) { TItem *x = add_item(P.tray.row, &P.tray.n, MAXROW, 4); const char *ln = lang_name(ts[i].lang);
      if (*ln) snprintf(x->lb, 200, "%s", ln); else if (ts[i].lang[0]) snprintf(x->lb, 200, "%s", ts[i].lang); else { char k[8]; snprintf(k, sizeof k, "%d", i + 1); snprintf(x->lb, 200, "%s %s", T("Audio"), k); }
      if (ts[i].name[0] && strcmp(ts[i].name, x->lb)) snprintf(x->sub, 120, "%s", ts[i].name); x->i = ts[i].idx; x->on = ts[i].idx == sel; }
    if (!n) { TItem *x = add_item(P.tray.row, &P.tray.n, MAXROW, 0); snprintf(x->lb, 200, "%s", T("None")); }
  } else if (t.kind == 1) { /* episodes: seasons on top, episode cards in a row */
    int ss[40], ns = 0; const cJSON *v; cJSON_ArrayForEach(v, P.videos) { int sn = cJSON_GetObjectItem(v, "season") ? (int)js_num(v, "season", 1) : 1; int f = 0; for (int i = 0; i < ns; i++) if (ss[i] == sn) f = 1; if (!f && ns < 40) ss[ns++] = sn; }
    for (int i = 1; i < ns; i++) for (int j = i; j > 0; j--) { int a = ss[j - 1], b = ss[j]; if ((a == 0 && b != 0) || (a != 0 && b != 0 && a > b)) { int tmp = ss[j]; ss[j] = ss[j - 1]; ss[j - 1] = tmp; } else break; }
    int found = 0; for (int i = 0; i < ns; i++) if (ss[i] == P.tray.season) found = 1; if (!found && ns) P.tray.season = ss[0];
    for (int i = 0; i < ns; i++) { P.tray.seasonList[i] = ss[i]; if (ss[i] == 0) snprintf(P.tray.tabs[i], 48, "%s", T("Specials")); else { char n[8]; snprintf(n, sizeof n, "%d", ss[i]); snprintf(P.tray.tabs[i], 48, "%s", T1("Season {n}", "n", n)); } if (ss[i] == P.tray.season) P.tray.tab = i; } P.tray.ntabs = ns;
    const cJSON *eps[400]; int ne = 0; cJSON_ArrayForEach(v, P.videos) { int sn = cJSON_GetObjectItem(v, "season") ? (int)js_num(v, "season", 1) : 1; if (sn == P.tray.season && ne < 400) eps[ne++] = v; }
    for (int i = 1; i < ne; i++) for (int j = i; j > 0 && js_num(eps[j - 1], "episode", 0) > js_num(eps[j], "episode", 0); j--) { const cJSON *tmp = eps[j]; eps[j] = eps[j - 1]; eps[j - 1] = tmp; }
    time_t nowT = time(NULL);
    for (int i = 0; i < ne && P.tray.n < MAXROW; i++) { const cJSON *e = eps[i]; TItem *x = add_item(P.tray.row, &P.tray.n, MAXROW, 2); x->v = e; int en = (int)js_num(e, "episode", 0); const char *nm = js_str(e, "name", js_str(e, "title", NULL));
      if (nm) snprintf(x->lb, 200, "%d. %s", en, nm); else { char n[8]; snprintf(n, sizeof n, "%d", en); snprintf(x->lb, 200, "%d. %s", en, T1("Episode {n}", "n", n)); }
      const char *rel = js_str(e, "released", js_str(e, "firstAired", "")); int y, m, d; char ds[32] = ""; if (sscanf(rel, "%d-%d-%d", &y, &m, &d) == 3) snprintf(ds, sizeof ds, "%d/%d/%d", d, m, y);
      int up = 0; if (strlen(rel) >= 10) { struct tm tm; memset(&tm, 0, sizeof tm); if (sscanf(rel, "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) == 3) { tm.tm_year -= 1900; tm.tm_mon--; up = timegm(&tm) > nowT; } }
      if (up) snprintf(x->sub, 120, "%s", T1("Upcoming · {d}", "d", ds)); else snprintf(x->sub, 120, "%s", ds);
      snprintf(x->ov, 300, "%s", js_str(e, "overview", js_str(e, "description", "")));
      const char *th = js_str(e, "thumbnail", NULL); if (th && *th) snprintf(x->th, 512, "%s", th); else if (!strncmp(P.id, "tt", 2) && cJSON_GetObjectItem(e, "episode")) snprintf(x->th, 512, "https://episodes.metahub.space/%s/%d/%d/w300.jpg", P.id, P.tray.season, en);
      { char *p = strstr(x->th, "image.tmdb.org/t/p/"); if (p) { p += 19; char *q = strchr(p, '/'); if (q) { char rest[512]; snprintf(rest, sizeof rest, "%s", q); snprintf(p, 512 - (p - x->th), "w300%s", rest); } } }
      x->on = !strcmp(js_str(e, "id", ""), P.vid); }
  } else if (t.kind == 2 || t.kind == 5) {
    if (t.kind == 2) snprintf(P.tray.title, 300, "%s", T("Sources")); P.tray.ntabs = 0; P.tray.tab = 0;
    srcs_begin(t.kind == 5 ? P.tray.epvid : P.vid); src_rows(); if (P.srcs.ng) src_ask(0);
  }
}
static void render_tray(int fresh) {
  if (!P.tray.kind) return;
  if (src_tray()) { if (fresh) SR.idx = -1; tray_data(); P.tray.zone = 0; return; } /* the sources: their own list */ char keepU[1024] = ""; char keepLb[200] = ""; int keepK = -1;
  { TItem *cur = NULL; if (P.tray.ncols) cur = P.tray.c < P.tray.ncols && P.tray.r[P.tray.c] >= 0 && P.tray.r[P.tray.c] < P.tray.cols[P.tray.c].n ? &P.tray.cols[P.tray.c].items[P.tray.r[P.tray.c]] : NULL; else cur = P.tray.idx >= 0 && P.tray.idx < P.tray.n ? &P.tray.row[P.tray.idx] : NULL;
    if (cur && !fresh) { snprintf(keepU, sizeof keepU, "%s", cur->u); snprintf(keepLb, sizeof keepLb, "%s", cur->lb); keepK = cur->k; } }
  tray_data();
  if (P.tray.ncols) {
    if (fresh) for (int i = 0; i < P.tray.ncols; i++) P.tray.r[i] = first_on(P.tray.cols[i].items, P.tray.cols[i].n);
    for (int i = 0; i < P.tray.ncols; i++) { if (P.tray.r[i] < 0 || P.tray.r[i] >= P.tray.cols[i].n || !focusable(&P.tray.cols[i].items[P.tray.r[i]])) P.tray.r[i] = first_on(P.tray.cols[i].items, P.tray.cols[i].n);
      if (!fresh && keepK >= 0) for (int j = 0; j < P.tray.cols[i].n; j++) if (P.tray.cols[i].items[j].k == keepK && !strcmp(P.tray.cols[i].items[j].u, keepU) && !strcmp(P.tray.cols[i].items[j].lb, keepLb)) { P.tray.c = i; P.tray.r[i] = j; } }
    if (fresh || P.tray.c >= P.tray.ncols || P.tray.r[P.tray.c] < 0) { P.tray.c = 0; for (int i = 0; i < P.tray.ncols; i++) if (P.tray.r[i] >= 0) { P.tray.c = i; break; } }
    if (P.tray.zone != 1 || !P.tray.ntabs) P.tray.zone = 2; /* (on the tabs: they keep it) */
  } else {
    int idx = -1; if (!fresh && keepK >= 0) for (int i = 0; i < P.tray.n; i++) if (P.tray.row[i].k == keepK && !strcmp(P.tray.row[i].lb, keepLb) && (keepK != 3 || (P.tray.row[i].st && !strcmp(P.tray.row[i].st->url, keepU)))) { idx = i; break; }
    if (idx < 0 && !fresh && P.tray.idx >= 0 && P.tray.idx < P.tray.n && focusable(&P.tray.row[P.tray.idx])) idx = P.tray.idx;
    if (idx < 0) idx = first_on(P.tray.row, P.tray.n);
    P.tray.idx = idx; if (P.tray.zone != 1 || !P.tray.ntabs) P.tray.zone = idx >= 0 ? 0 : (P.tray.ntabs ? 1 : 0);
  }
  tm_after();
}
static int subTab; /* the subtitle tab last used (kept while the app runs) */
static void open_tray(int kind) {
  if (kind != 2 && kind != 5) srcs_free(); else { mo_set(&SR.y, 0); mo_set(&SR.plA, 0); }
  P.mode = 2; memset(&P.tray.row, 0, sizeof P.tray.row); P.tray.kind = kind; P.tray.idx = -1; P.tray.zone = 0; P.tray.onBtn = 0; P.tray.n = 0; P.tray.ntabs = 0;
  if (kind == 1) P.tray.season = season_of(P.vid);
  if (kind == 4) { load_emb_tracks(); P.tray.tab = subTab; P.tray.zone = 1; } /* the subtitles open on their tabs (kept when there are any) */
  tm_reset(); TM.quiet = 1; render_tray(1); TM.quiet = 0;
  tray_origin();
  /* it starts small at its button and grows (the closing motion is set by the classes, see css()) */
  mo_set(&P.tray.open, 0.12f); mo_set(&P.tray.openA, 0); mo_set(&P.tray.rowX, 0); mo_set(&P.tray.tabsX, 0); for (int i = 0; i < 4; i++) mo_set(&P.tray.colY[i], 0); mo_set(&P.tray.ringA, 0); P.tray.ringW = 0; for (int i = 0; i < 4; i++) mo_set(&P.tray.plA[i], 0);
}
static void tray_origin(void) { int ak = P.tray.kind < 0 ? -P.tray.kind : P.tray.kind; int b = btn_index(ak == 1 || ak == 5 ? "eps" : ak == 2 ? "src" : ak == 3 ? "aud" : "subs");
  P.tray.ox = b >= 0 ? P.btns[b].x0 + P.btns[b].w / 2 : SB_W - 72 - 39; P.tray.oy = SB_H - 54 - 47; } /* its button's centre, measured again on close: the tray may have grown since */
static void dly_open(void);
static void close_tray(void) { if (P.tray.kind == 4) subTab = P.tray.tab; P.mode = 1; P.tray.kind = -P.tray.kind; tray_origin(); arm_hide(); }
static void tray_back(void) {
  if (P.tray.onBtn) { close_tray(); return; }
  if (P.tray.zone == 1 && P.tray.idx >= 0) { P.tray.zone = 0; return; }
  if (P.tray.kind == 5) { srcs_free(); P.tray.kind = 1; P.tray.tab = P.tray.backTab; P.tray.season = P.tray.seasonList[P.tray.tab]; P.tray.zone = 0; render_tray(1); P.tray.idx = P.tray.backIdx; return; }
  close_tray();
}
static void adjust(const char *k) {
  if (!strcmp(k, "size")) { char v[4]; snprintf(v, sizeof v, "%d", (size_idx() + 1) % 7); store_set("subtitleSize", v); }
  else if (!strcmp(k, "color")) { int ci = color_idx(); store_set("subsColor", SUB_COLORS[((ci < 0 ? 0 : ci) + 1) % 3]); }
  else if (!strcmp(k, "bg")) store_set("subsBgStyle", SUB_BGS[(bg_idx() + 1) % 3]);
  render_tray(0);
}
static void choose(TItem *it) {
  if (!it) return;
  if (it->k == 4 && P.tray.kind == 4) {
    if (!strncmp(it->u, "lang:", 5)) { snprintf(P.subLang, sizeof P.subLang, "%s", it->u + 5); render_tray(0); if (P.tray.ncols > 1) { int r = first_on(P.tray.cols[1].items, P.tray.cols[1].n); if (r >= 0) { P.tray.c = 1; P.tray.r[1] = r; } } } /* its subtitles: the focus goes over */
    else if (!strcmp(it->u, "off")) subs_off(); else if (!strncmp(it->u, "emb:", 4)) set_emb_sub(it->i); else if (it->tool) { if (strcmp(it->u, "sync")) { TM.vkC = P.tray.c; snprintf(TM.vkU, sizeof TM.vkU, "%s", it->u); mo_set(&TM.vkA, 0); mo_to(&TM.vkA, 1, .2f, C_OUT, 0); } if (!strcmp(it->u, "sync")) sync_run(); else if (!strcmp(it->u, "dlyopen")) { dly_open(); return; } else if (!strcmp(it->u, "dly")) { P.subDelay = it->d ? (double)((int)((P.subDelay + it->d) * 10 + (P.subDelay + it->d >= 0 ? 0.5 : -0.5))) / 10 : 0; if (!it->d) sync_undo(); render_tray(0); } else adjust(it->u); }
    else if (it->ls) { const SubEntry *l = it->ls == 1 ? P.ext : P.adx; int n = it->ls == 1 ? P.nExt : P.nAdx, k = it->i; /* the row's entry (found again by its file if the list moved) */
      if (k < 0 || k >= n || strcmp(l[k].url, it->u)) for (k = 0; k < n && strcmp(l[k].url, it->u); k++) ;
      if (k < n) set_ext_sub(it->ls, &l[k]); }
  } else if (it->k == 4 && P.tray.kind == 3 && it->i >= 1000) { int ord = it->i - 1000; /* another track converted, from here */
    if (P.dts.conv == 2 && ord != P.dts.convOrd && ord >= 0) { P.dts.convOrd = ord; P.dts.conv = 1; P.dts.convAt = video_time(); P.dts.convT0 = now_();
      int fl = conv_flags(ord); P.dts.copyA = !!(fl & HC_COPYAUDIO);
      video_unload(); P.live = 0; P.liveT0 = 0; hls_conv_start(P.url, ord, fl); fprintf(stderr, "[dts] audio track %d instead (%s)\n", ord, P.dts.copyA ? "copied" : "converted"); }
    render_tray(0); }
  else if (it->k == 4 && P.tray.kind == 3) { set_audio(it->i); render_tray(0); }
  else if (it->k == 2) { /* an episode: its sources, right here */
    const cJSON *v = it->v; P.tray.backIdx = P.tray.idx; P.tray.backTab = P.tray.tab; P.tray.kind = 5; snprintf(P.tray.epvid, sizeof P.tray.epvid, "%s", js_str(v, "id", "")); P.tray.idx = -1; P.tray.tab = 0; P.tray.zone = 0;
    snprintf(P.tray.title, 300, "S%d:E%d  ·  %s", (int)js_num(v, "season", 0), (int)js_num(v, "episode", 0), js_str(v, "name", js_str(v, "title", ""))); render_tray(1);
  } else if (it->k == 3) { int same = P.tray.kind == 2; const char *vid = same ? P.vid : P.tray.epvid; Stream s = *it->st; play_stream(&s, vid, same ? video_time() : 0); }
}
static TItem *tray_cur(void) { if (P.tray.ncols) return P.tray.c < P.tray.ncols && P.tray.r[P.tray.c] >= 0 && P.tray.r[P.tray.c] < P.tray.cols[P.tray.c].n ? &P.tray.cols[P.tray.c].items[P.tray.r[P.tray.c]] : NULL; return P.tray.idx >= 0 && P.tray.idx < P.tray.n ? &P.tray.row[P.tray.idx] : NULL; }
static int step_in(const TItem *l, int n, int from, int d) { for (int i = from + d; i >= 0 && i < n; i += d) if (focusable(&l[i])) return i; return -1; }
/* another tab: the pill glides there at once; what it shows slides out the other way, then the new one slides in */
static void tray_tab(int to) {
  if (to == P.tray.tab || to < 0 || to >= P.tray.ntabs) return;
  TM.swDir = to > P.tray.tab ? 1 : -1; P.tray.tab = to; if (P.tray.kind == 1) P.tray.season = P.tray.seasonList[to]; P.tray.idx = -1;
  TM.swPend = 1; mo_to(&TM.swA, 0, .1f, C_EASE, 0);
}
static void tray_swap_step(void) {
  if (!TM.swPend || TM.swA.on || TM.swA.v > 0.001f) return; TM.swPend = 0;
  if (P.tray.kind <= 0) { mo_set(&TM.swA, 1); return; }
  int z = P.tray.zone; TM.quiet = 1; render_tray(1); TM.quiet = 0; if (z == 1 && P.tray.ntabs) P.tray.zone = 1;
  mo_to(&TM.swA, 1, .18f, C_OUT, 0);
}
static int tray_key(int k) {
  if (P.tray.onBtn) { if (k == K_OK) close_tray(); else if (k == K_UP) P.tray.onBtn = 0; else if (k == K_LEFT || k == K_RIGHT) { close_tray(); P.bIdx = k == K_RIGHT ? (P.bIdx + 1 < P.nbtn ? P.bIdx + 1 : P.bIdx) : (P.bIdx > 0 ? P.bIdx - 1 : 0); } else if (k == K_BACK) close_tray(); return 1; }
  if (src_tray()) {
    if (k == K_UP || k == K_DOWN) { int d = k == K_DOWN ? 1 : -1, j = -1; for (int i = SR.idx + d; i >= 0 && i < SR.n; i += d) if (src_focusable(i)) { j = i; break; }
      if (j >= 0) { SR.idx = j; if (SR.r[j].k == 5 && P.srcs.g[SR.r[j].g].open) src_ask(SR.r[j].g); } else if (k == K_DOWN) P.tray.onBtn = 1; else nudge_start(&P.nudge, k); }
    else if (k == K_OK) src_choose(SR.idx);
    else if (k == K_BACK) tray_back();
    else nudge_start(&P.nudge, k);
    return 1; }
  if (P.tray.zone == 1) {
    if ((k == K_LEFT && P.tray.tab > 0) || (k == K_RIGHT && P.tray.tab < P.tray.ntabs - 1)) tray_tab(P.tray.tab + (k == K_RIGHT ? 1 : -1));
    else if ((k == K_DOWN || k == K_OK) && P.tray.ncols) P.tray.zone = 2;
    else if ((k == K_DOWN || k == K_OK) && P.tray.idx >= 0) P.tray.zone = 0;
    else if (k == K_BACK) tray_back(); else if (k != K_OK) nudge_start(&P.nudge, k);
    return 1; }
  if (P.tray.zone == 2) { TCol *col = &P.tray.cols[P.tray.c];
    if (k == K_UP || k == K_DOWN) { int n = step_in(col->items, col->n, P.tray.r[P.tray.c], k == K_DOWN ? 1 : -1); if (n >= 0) P.tray.r[P.tray.c] = n; else if (k == K_DOWN) P.tray.onBtn = 1; else if (P.tray.ntabs) P.tray.zone = 1; else nudge_start(&P.nudge, k);
      if (n >= 0 && P.tray.kind == 4 && !strncmp(col->items[n].u, "lang:", 5)) { snprintf(P.subLang, sizeof P.subLang, "%s", col->items[n].u + 5); render_tray(0); } } /* the language under the focus: its subtitles beside it */
    else if (k == K_LEFT || k == K_RIGHT) { int c0 = P.tray.c; for (int c = P.tray.c + (k == K_RIGHT ? 1 : -1); c >= 0 && c < P.tray.ncols; c += k == K_RIGHT ? 1 : -1) if (P.tray.r[c] >= 0) { P.tray.c = c; break; } if (P.tray.c == c0) nudge_start(&P.nudge, k); }
    else if (k == K_OK) choose(tray_cur()); else if (k == K_BACK) tray_back();
    return 1; }
  if (k == K_LEFT) { int a = step_in(P.tray.row, P.tray.n, P.tray.idx, -1); if (a >= 0) P.tray.idx = a; else nudge_start(&P.nudge, k); }
  else if (k == K_RIGHT) { int b = step_in(P.tray.row, P.tray.n, P.tray.idx, 1); if (b >= 0) P.tray.idx = b; else nudge_start(&P.nudge, k); }
  else if (k == K_UP) { if (P.tray.ntabs) P.tray.zone = 1; else nudge_start(&P.nudge, k); }
  else if (k == K_DOWN) P.tray.onBtn = 1;
  else if (k == K_OK && tray_cur()) choose(tray_cur());
  else if (k == K_BACK) tray_back();
  return 1;
}

/* ---- keys ---- */
/* from the very start: the skips and the credits card are offered again */
static void from_start(void) { cancel_seek(); memset(P.dismissed, 0, sizeof P.dismissed); P.prompt.k = 0; paint_prompt(); P.ended = 0; P.endGoAt = 0; video_seek(0); if (paused()) video_play(); show_osd(1); }
static struct { float x[4], y, w[4], h; } DG; /* the card's buttons, for the pointer */
static void dly_draw(void);
/* ---- the delay card: the subtitles' timing set while watching. A small card at the top with - and + (a quarter of a
   second each), the delay between them, play / pause and Done; the picture and the lines stay in view, so a change is
   seen at once. Back or Done closes it (mode 4) ---- */
#define DLY_STEP 0.25
static void dly_open(void) { close_tray(); P.osd = 0; P.mode = 4; P.dlyF = 0; mo_set(&P.dlyR, 0); mo_to(&P.dlyA, 1, .28f, C_OUT, 0); }
static void dly_close(void) { P.mode = 0; mo_to(&P.dlyA, 0, .2f, C_EASE, 0); }
static void dly_change(double d) { double v = P.subDelay + d; v = floor(v / DLY_STEP + 0.5) * DLY_STEP; if (v > 120) v = 120; if (v < -120) v = -120; P.subDelay = fabs(v) < 1e-6 ? 0 : v; P.dlyAt = now_(); }
static void dly_act(int i) { P.dlyF = i; mo_to(&P.dlyR, (float)i, .18f, C_OUT, 0);
  if (i == 0) dly_change(-DLY_STEP); else if (i == 1) dly_change(DLY_STEP); else if (i == 2) toggle(); else dly_close(); }
static void dly_key(int k) {
  if (k == K_LEFT) { if (P.dlyF > 0) { P.dlyF--; mo_to(&P.dlyR, (float)P.dlyF, .18f, C_OUT, 0); } else nudge_start(&P.nudge, k); }
  else if (k == K_RIGHT) { if (P.dlyF < 3) { P.dlyF++; mo_to(&P.dlyR, (float)P.dlyF, .18f, C_OUT, 0); } else nudge_start(&P.nudge, k); }
  else if (k == K_OK) dly_act(P.dlyF);
  else if (k == K_UP || k == K_DOWN) nudge_start(&P.nudge, k);
}
static void act(const char *id) {
  if (!strcmp(id, "pp")) toggle(); else if (!strcmp(id, "beg")) from_start(); else if (!strcmp(id, "asp")) next_aspect(); else if (!strcmp(id, "nxt")) next_auto(); else if (!strcmp(id, "cc")) cc_toggle();
  else open_tray(!strcmp(id, "eps") ? 1 : !strcmp(id, "src") ? 2 : !strcmp(id, "aud") ? 3 : 4);
  arm_hide();
}
static void back(void) {
  if (P.closing) return;
  if (P.mode == 2) { tray_back(); return; }
  if (P.mode == 4) { dly_close(); return; }
  if (P.prompt.k && P.prompt.autoN) { P.prompt.autoN = 0; paint_prompt(); return; } /* a countdown running (skip, next episode): Back stops it, the controls up or not; the prompt stays */
  if (P.mode == 3) { dismiss_prompt(); return; }
  if (P.seekT >= 0) { cancel_seek(); return; }
  if (P.mode == 1 && !paused()) { hide_osd(); return; }
  close_player(0, 0);
}
static void ss_exit(void) { P.ssOn = 0; P.lastAct = now_(); }
static void try_next(void);
void player_key(int k, int down) {
  if (!down || !P.open) return; app_touch();
  P.lastAct = now_(); if (P.ssOn) { ss_exit(); return; } /* the key that wakes the screensaver does nothing else */
  if (P.failed && P.canNext && k == K_OK) { try_next(); return; }
  if (!P.started || !P.live || P.failed) { if (k == K_BACK || k == K_EXIT) close_player(0, 0); return; } /* the art is up (resolving): only Back does anything */
  P.hvOn = 0;
  if (P.mode == 4 && (k == K_PLAY || k == K_PAUSE || k == K_PLAYPAUSE)) { if (k == K_PLAYPAUSE || (k == K_PLAY) == paused()) toggle(); return; } /* the card stays: the controls don't come */
  if (k == K_PLAY) { if (paused()) video_play(); show_osd(1); return; }
  if (k == K_PAUSE) { if (!paused()) { video_pause(); save_progress(1); } show_osd(1); return; }
  if (k == K_PLAYPAUSE) { toggle(); show_osd(1); return; }
  if (k == K_STOP || k == K_EXIT) { close_player(0, 0); return; }
  if (k == K_REW) { seek_by(-1); return; } if (k == K_FF) { seek_by(1); return; }
  if (k == K_BACK) { back(); return; }
  if (P.mode == 2) { tray_key(k); return; }
  if (P.mode == 4) { dly_key(k); return; }
  if (P.mode == 3) {
    if (P.prompt.k == 3 && P.next.has && (k == K_LEFT || k == K_RIGHT)) { nudge_start(&P.nudge, k); return; } /* the card: OK or BACK */
    if ((k == K_LEFT && P.prompt.idx == 0) || (k == K_RIGHT && P.prompt.idx >= P.prompt.nbtn - 1)) nudge_start(&P.nudge, k);
    else if (k == K_LEFT) P.prompt.idx--; else if (k == K_RIGHT) P.prompt.idx++;
    else if (k == K_OK) prompt_go(P.prompt.idx);
    else if (k == K_UP || k == K_DOWN) { P.mode = 0; show_osd(0); }
    return; }
  if (P.mode == 0) {
    if (k == K_OK) { if (P.seekT >= 0) commit_seek(); else if (P.prompt.k) prompt_go(P.prompt.idx); else { toggle(); show_osd(0); } return; }
    if (k == K_LEFT) { seek_by(-1); return; } if (k == K_RIGHT) { seek_by(1); return; }
    if (k == K_UP || k == K_DOWN) { if (P.seekT >= 0) commit_seek(); show_osd(0); return; }
    return; }
  arm_hide();
  if (P.row == 1) {
    if (k == K_LEFT) seek_by(-1); else if (k == K_RIGHT) seek_by(1); else if (k == K_OK) { if (P.seekT >= 0) commit_seek(); else toggle(); }
    else if (k == K_DOWN) { if (P.seekT >= 0) commit_seek(); P.row = 0; }
    else if (k == K_UP) { if (P.prompt.k) { P.mode = 3; } else nudge_start(&P.nudge, k); }
    return; }
  if ((k == K_LEFT && P.bIdx == 0) || (k == K_RIGHT && P.bIdx == P.nbtn - 1)) nudge_start(&P.nudge, k); /* the row's end */
  else if (k == K_LEFT) P.bIdx--; else if (k == K_RIGHT) P.bIdx++;
  else if (k == K_UP) P.row = 1; else if (k == K_DOWN) hide_osd(); else if (k == K_OK) act(P.btns[P.bIdx].id);
}

/* ---- open / start / close ---- */
static void fill_from_meta(void) {
  const cJSON *m = P.meta; if (!m) return;
  snprintf(P.name, sizeof P.name, "%s", js_str(m, "name", P.name)); const char *lg = js_str(m, "logo", NULL); if (lg && *lg) snprintf(P.logo, sizeof P.logo, "%s", lg);
  if (!P.poster[0]) snprintf(P.poster, sizeof P.poster, "%s", js_str(m, "poster", "")); snprintf(P.year, sizeof P.year, "%.4s", js_str(m, "releaseInfo", js_str(m, "year", "")));
  const cJSON *gs = js_arr(m, "genres"); if (gs && cJSON_GetArraySize(gs) && cJSON_IsString(cJSON_GetArrayItem(gs, 0))) snprintf(P.genre, sizeof P.genre, "%s", cJSON_GetArrayItem(gs, 0)->valuestring);
  { const cJSON *vv = js_arr(m, "videos"); if (!strcmp(P.type, "series") || (vv && cJSON_GetArraySize(vv) > 1 && strcmp(P.type, "movie") && strcmp(P.type, "other"))) P.videos = (cJSON *)vv; }
  if (P.videos && strcmp(P.vid, P.id)) { P.season = P.episode = -1; /* from the video's own numbers; a series' id ("tt..:s:e") as the fallback (an anime id like "kitsu:123:5" is no season) */
    { const char *c = strrchr(P.vid, ':'); if (c && !strcmp(P.type, "series")) { const char *p = c - 1; while (p > P.vid && *p != ':') p--; if (*p == ':') { P.season = atoi(p + 1); P.episode = atoi(c + 1); } } }
    { const cJSON *v0; cJSON_ArrayForEach(v0, P.videos) if (!strcmp(js_str(v0, "id", ""), P.vid)) { if (js_num(v0, "season", -1) >= 0) P.season = (int)js_num(v0, "season", 0); if (js_num(v0, "episode", -1) >= 0) P.episode = (int)js_num(v0, "episode", 0); break; } }
    const cJSON *cur = NULL, *nxt = NULL, *first = NULL, *v; cJSON_ArrayForEach(v, P.videos) { if (!strcmp(js_str(v, "id", ""), P.vid)) cur = v; int s = (int)js_num(v, "season", -1), e = (int)js_num(v, "episode", -1); if (s == P.season && e == P.episode + 1) nxt = v; if (P.season >= 0 && s == P.season + 1 && (!first || e < (int)js_num(first, "episode", 0))) first = v; }
    if (!nxt && first) nxt = first; /* last episode of a season -> first of the next one */
    if (cur && !nxt && js_num(cur, "season", -1) < 0) { /* no seasons (anime, channels): the next in the list, by episode number when given, else in order */
      int ce = (int)js_num(cur, "episode", -1), seen = 0; const cJSON *after = NULL;
      cJSON_ArrayForEach(v, P.videos) { if (ce >= 0 && (int)js_num(v, "episode", -1) == ce + 1) { nxt = v; break; } if (seen && !after) after = v; if (v == cur) seen = 1; }
      if (!nxt) nxt = after; }
    if (nxt) { const char *rel = js_str(nxt, "released", ""); struct tm tm; memset(&tm, 0, sizeof tm); if (strlen(rel) >= 10 && sscanf(rel, "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) == 3) { tm.tm_year -= 1900; tm.tm_mon--; if (timegm(&tm) > time(NULL)) nxt = NULL; } } /* not out yet */
    if (cur) snprintf(P.epTitle, sizeof P.epTitle, "%s", js_str(cur, "name", js_str(cur, "title", "")));
    if (nxt) { P.next.has = 1; snprintf(P.next.vid, 80, "%s", js_str(nxt, "id", "")); if (js_num(nxt, "season", -1) >= 0) snprintf(P.next.se, 32, "S%d · E%d", (int)js_num(nxt, "season", 0), (int)js_num(nxt, "episode", 0)); else if (js_num(nxt, "episode", -1) >= 0) snprintf(P.next.se, 32, "E%d", (int)js_num(nxt, "episode", 0)); else P.next.se[0] = 0; snprintf(P.next.title, 256, "%s", js_str(nxt, "name", js_str(nxt, "title", ""))); snprintf(P.next.thumb, 512, "%s", js_str(nxt, "thumbnail", "")); snprintf(P.next.ov, 600, "%s", js_str(nxt, "overview", js_str(nxt, "description", ""))); }
  } else if (P.type[0] && strcmp(P.type, "other")) P.sim = 1;
}
/* the title's metadata: from memory at once, else from the disk cache (that answer can come back before the player
   is open) or the network; a series still missing its episodes or next episode asks again 20 s later */
static unsigned metaTok;
static void on_meta(cJSON *m, void *ud) {
  if ((unsigned)(uintptr_t)ud != metaTok || !m) return;
  if (P.meta && P.videos) return; /* nothing missing (and the open tray points into this copy) */
  if (P.meta) cJSON_Delete(P.meta); P.meta = cJSON_Duplicate(m, 1); fill_from_meta(); if (P.started) build_btns();
}
static void ask_meta(void) { metaTok++; if (!metaTok) metaTok++; sb_meta(P.type, P.id, on_meta, (void *)(uintptr_t)metaTok); }
static void on_size(int st, const unsigned char *body, size_t len, void *ud) {
  (void)len; if (!P.open || (unsigned)(uintptr_t)ud != P.sizeTok) return; P.sizeAns = 1; P.httpSt = st; if (st >= 400) fprintf(stderr, "[player] the stream's server answers %d\n", st); if (st <= 0) return; double b = atof((const char *)body); if (b <= 0) return; P.size = b;
  char n[16]; if (b >= 1e9) { snprintf(n, sizeof n, "%.*f", b >= 1e10 ? 0 : 1, b / 1e9); snprintf(P.sizeTxt, sizeof P.sizeTxt, "%s", T1("{n} GB", "n", n)); } else { snprintf(n, sizeof n, "%d", (int)(b / 1e6 + 0.5)); snprintf(P.sizeTxt, sizeof P.sizeTxt, "%s", T1("{n} MB", "n", n)); }
}
/* A stream whose text names only TrueHD, or only DTS on a TV seen failing it: the TV is not given it until the file's
   tracks are read (the subtitle reader's head read, a moment): a track it plays is chosen once it lists them, else the
   conversion starts without a silent first try. Every other stream loads at once, as always */
static int dts_defer_wanted(void) {
  if (P.isTrailer || P.isHls || strncmp(P.url, "http", 4) || !hls_conv_available()) return 0;
  BestRate r; if (!best_rate(&P.st, NULL, -1, 100, &r) || !r.mute) return 0;
  char au[48]; best_audio_of(P.sname, au, sizeof au); return strstr(au, "TrueHD") || best_learned("dts") == 0;
}
static void dts_defer_tick(void) {
  if (P.dts.conv) { P.dts.deferUntil = 0; return; } /* (started meanwhile, from the file's head) */
  int listed = mkv_listed(mkv); if (!listed && mkv_state(mkv) != MKV_FALLBACK && now_() < P.dts.deferUntil) return;
  P.dts.deferUntil = 0; MkvAudio ma[16]; int m = listed ? mkv_audio_tracks(mkv, ma, 16) : 0, playable = 0, first = -1;
  for (int i = 0; i < m; i++) { VidTrack v; memset(&v, 0, sizeof v); snprintf(v.codec, sizeof v.codec, "%s", ma[i].codec); if (!aud_bad(&v)) playable = 1; else if (first < 0) first = i; }
  fprintf(stderr, "[dts] the file's sound: %d track(s), %s\n", m, !m ? "not read: the TV tries it" : playable ? "one plays here: the TV loads it" : "none plays here");
  if (m && !playable && first >= 0 && dts_convert(first, "known before loading")) return;
  if (listed && dv7_convert("known before loading")) return;
  video_load(P.url);
}
/* the volume, set with the mouse wheel while a film plays (persisted across starts) */
static int playVol = -1;
static void vol_ensure(void);
static void start_video(void) {
  P.started = 1; P.isTrailer = !strcmp(P.type, "other"); P.isHls = strstr(P.url, ".m3u8") != NULL;
  best_caps_probe(NULL, NULL); /* (once: kept) */
  if (dts_defer_wanted() || dv7_defer_wanted()) P.dts.deferUntil = now_() + 4; else video_load(P.url);
  build_btns(); apply_aspect(); P.res = res_of(P.sname);
  if ((episodic() || (strcmp(P.type, "movie") && strcmp(P.type, "other") && strcmp(P.vid, P.id))) && !P.isTrailer && (!P.next.has || !P.videos)) { ask_meta(); P.metaRetryAt = now_() + 20; }
  if (!P.isHls && !P.isTrailer && !strncmp(P.url, "http", 4)) { static unsigned szTok; P.sizeTok = ++szTok; net_size(P.url, on_size, (void *)(uintptr_t)P.sizeTok); } /* asked once per stream */
  asssubs_close(); if (mkv) { mkv_close(mkv); mkv = NULL; }
  if (!P.isHls && !P.isTrailer && !strncmp(P.url, "http", 4)) mkv = mkv_open(P.url, P.startAt > 0 ? P.startAt : 0, P.isP2p); /* not Matroska: it gives up after one small read */
  load_ext_subs(0); load_addon_subs(0); load_emb_tracks();
  if (!P.isTrailer && P.vid[0]) { srcs_begin(P.vid); for (int i = 0; i < P.srcs.ng; i++) src_ask(i); } /* the sources found while it plays: the tray shows them at once */
  vol_ensure(); video_set_volume(playVol, 0); /* the remembered volume, not mpv's default */
}
static char nextAlt[SB_URL_MAX];
void player_fallback(const char *url) { snprintf(nextAlt, sizeof nextAlt, "%s", url ? url : ""); }
/* a new source / episode: the state starts over behind the title art (the art crossfades in when already playing) */
/* ---- the seek pill's time as a rolling clock: each character keeps a slot (digits as wide as the widest), only the
   ones that change roll: the old one slides down and fades, the new one comes in from above (the other way seeking
   back). A change coming faster than the roll shortens it rather than queueing ---- */
#define ROLL_MAX 24
typedef struct { char c[ROLL_MAX][5]; float w[ROLL_MAX]; int n; float tot; } RollStr;
static struct { char key[48]; RollStr cur, prev; Mo t; int dir; double at; float dw; } RL;
static void roll_split(const char *s, RollStr *o) {
  o->n = 0; o->tot = 0;
  while (*s && o->n < ROLL_MAX) { int k = 1; unsigned char b = (unsigned char)*s; if (b >= 0xf0) k = 4; else if (b >= 0xe0) k = 3; else if (b >= 0xc0) k = 2;
    char *c = o->c[o->n]; int i = 0; for (; i < k && s[i]; i++) c[i] = s[i]; c[i] = 0; s += i;
    o->w[o->n] = c[0] >= '0' && c[0] <= '9' && !c[1] ? RL.dw : text_width(c, 25, 600, 0) + .2f; o->tot += o->w[o->n]; o->n++; }
}
static float roll_layout(const char *txt, int dir) {
  if (RL.dw <= 0) for (char d = '0'; d <= '9'; d++) { char b[2] = { d, 0 }; float w = text_width(b, 25, 600, 0) + .2f; if (w > RL.dw) RL.dw = w; }
  if (strcmp(RL.key, txt)) {
    int first = !RL.key[0]; double now = now_(); float d = first ? 0 : (float)fmin(.18, fmax(.07, now - RL.at)); /* held keys: the roll keeps up */
    RL.prev = RL.cur; snprintf(RL.key, sizeof RL.key, "%s", txt); roll_split(txt, &RL.cur); RL.dir = dir < 0 ? -1 : 1; RL.at = now;
    if (first) { RL.prev = RL.cur; mo_set(&RL.t, 1); } else { mo_set(&RL.t, 0); mo_to(&RL.t, 1, d, C_OUT, 0); }
  }
  return RL.cur.tot;
}
static void roll_draw(float cx, float y, float lh, uint32_t col) {
  float x = cx + RL.cur.tot / 2, t = RL.t.v, dy = lh * .55f * RL.dir; int n = RL.cur.n > RL.prev.n ? RL.cur.n : RL.prev.n;
  for (int j = 0; j < n; j++) { int ic = RL.cur.n - 1 - j, ip = RL.prev.n - 1 - j; /* slots counted from the right: the seconds stay put */
    const char *a = ic >= 0 ? RL.cur.c[ic] : NULL, *b = ip >= 0 ? RL.prev.c[ip] : NULL; float w = a ? RL.cur.w[ic] : RL.prev.w[ip], mx = x - w / 2; x -= w;
    if (t >= 1 || (a && b && !strcmp(a, b))) { if (a) text_draw(a, mx, y, 25, 600, col, 0, 0, ALIGN_CENTER); continue; }
    if (b) text_draw(b, mx, y + dy * t, 25, 600, col_alpha(col, 1 - t), 0, 0, ALIGN_CENTER);
    if (a) text_draw(a, mx, y - dy * (1 - t), 25, 600, col_alpha(col, t), 0, 0, ALIGN_CENTER); }
}
static void draw_osd(void); static void draw_tray(void);
static void vol_ensure(void); static void draw_vol(void); void player_wheel(float dy);
/* another source or episode from inside the player: the controls as they are on screen at that moment, taken ONCE as
   a picture, fade off as the controls do (.2 s) while the art comes back over them (.45 s); freed once gone */
static GfxTarget handSnap; static Mo handA;
static void hand_snap(void) {
  if (!handSnap.fbo && !gfx_target_create(&handSnap, 1280, 720)) return;
  gfx_target_begin(&handSnap, RGBA(0, 0, 0, 0)); draw_osd(); draw_tray(); gfx_target_end();
  mo_set(&handA, 1); mo_to(&handA, 0, .2f, C_EASE, 0);
}
static void hand_tick(float dt) { mo_tick(&handA, dt); if (handSnap.fbo && !handA.on && handA.v <= 0.001f) gfx_target_free(&handSnap); }
static void hand_draw(void) { if (handSnap.fbo && handA.v > 0.001f) gfx_image(handSnap.tex, 0, 0, SB_W, SB_H, 0, WHITE_A((int)(255 * handA.v + .5f)), 0, 1, 1, 0); }
static void prepare(const char *vid, double startAt, int keepSubs) {
  resumeMsg[0] = 0; loadGen++;
  int again = P.open; char type[16]; snprintf(type, sizeof type, "%s", P.type);
  float artWas = again ? P.artA.v : 1; /* the art comes back from where it is (the page's .45 s), never at once */
  if (again && P.live && artWas < 0.999f) hand_snap();
  if (again) { video_unload(); hls_conv_stop(); P.live = 0; asssubs_close(); if (mkv) { mkv_close(mkv); mkv = NULL; } if (P.isP2p || P.resolving) p2p_player_closed(); }
  SubEntry *keep = NULL, *keepA = NULL; int nkeep = 0, nkeepA = 0; if (again && keepSubs && !strcmp(vid, P.vid)) { if (P.nExt) { keep = P.ext; nkeep = P.nExt; P.ext = NULL; } if (P.nAdx) { keepA = P.adx; nkeepA = P.nAdx; P.adx = NULL; } }
  cJSON *meta = P.meta; P.meta = NULL; subs_free(P.cues, P.ncues); free(P.ext); free(P.adx); srcs_free(); cols_clear();
  char id[64]; snprintf(id, sizeof id, "%s", P.id); char name[256]; snprintf(name, sizeof name, "%s", P.name); char poster[512]; snprintf(poster, sizeof poster, "%s", P.poster); char bg[512]; snprintf(bg, sizeof bg, "%s", P.bg); char logo[512]; snprintf(logo, sizeof logo, "%s", P.logo); unsigned subReq = P.subReq, adxReq = P.adxReq;
  char v[80]; snprintf(v, sizeof v, "%s", vid && *vid ? vid : id);
  sync_stop(0); memset(&P, 0, sizeof P); sub_reset(); P.subReq = subReq; P.adxReq = adxReq; P.open = 1; P.openedAt = now_(); P.seekT = -1; P.curAud = -1; snprintf(P.curSub, sizeof P.curSub, "off");
  snprintf(P.type, sizeof P.type, "%s", type); snprintf(P.id, sizeof P.id, "%s", id); snprintf(P.vid, sizeof P.vid, "%s", v); snprintf(P.name, sizeof P.name, "%s", name); snprintf(P.poster, sizeof P.poster, "%s", poster); snprintf(P.bg, sizeof P.bg, "%s", bg); snprintf(P.logo, sizeof P.logo, "%s", logo);
  P.meta = meta; P.ext = keep; P.nExt = nkeep; P.adx = keepA; P.nAdx = nkeepA; P.startAt = startAt; aspect_load(); set_status("");
  if (!strcmp(P.type, "series")) { const char *c = strrchr(P.vid, ':'); if (c && c > P.vid) { const char *q = c - 1; while (q > P.vid && *q != ':') q--; if (*q == ':') { P.season = atoi(q + 1); P.episode = atoi(c + 1); } } } /* known from the id, the metadata or not */
  memset(&P.pg.g, 0, sizeof P.pg.g); P.pg.age = 0; P.pg.certAge = -1; P.pg.guideIn = P.pg.certIn = 0; P.pg.asked = 0; P.pg.sess++; P.pg.at = P.pg.askAt = 0; mo_set(&P.pgA, 0); mo_set(&P.pgY, -10);
  mo_set(&P.artA, artWas); mo_set(&P.in, again ? 1 : 0); mo_set(&P.lnA, 1); mo_set(&P.prB, 104); mo_set(&P.subB, SB_H * 0.08f); mo_set(&P.ttlX, -30); mo_set(&P.barY, 18); mo_set(&P.btnY, 24); mo_set(&P.topY, -12); mo_set(&P.prY, 20); mo_set(&P.skS, 0); mo_set(&P.ccS, 1);
  fill_from_meta(); if (!P.logo[0] && !strncmp(P.id, "tt", 2)) snprintf(P.logo, sizeof P.logo, "https://images.metahub.space/logo/medium/%s/img", P.id);
  if (!P.bg[0]) { Item it = lib_item(); sb_bg_mid(&it, P.bg, sizeof P.bg); }
}
static void on_p2p(void *ud, const char *url, const char *err, uint64_t size) {
  if (!P.open || P.closing || (unsigned)(uintptr_t)ud != p2pTok) return;
  P.resolving = 0;
  if (!url) { fail(err ? err : T("No peers found for this torrent")); return; }
  snprintf(P.url, sizeof P.url, "%s", url); P.isP2p = 1; set_status("");
  if (size) { P.size = (double)size; char n[16]; if (P.size >= 1e9) { snprintf(n, sizeof n, "%.*f", P.size >= 1e10 ? 0 : 1, P.size / 1e9); snprintf(P.sizeTxt, sizeof P.sizeTxt, "%s", T1("{n} GB", "n", n)); } else { snprintf(n, sizeof n, "%d", (int)(P.size / 1e6 + 0.5)); snprintf(P.sizeTxt, sizeof P.sizeTxt, "%s", T1("{n} MB", "n", n)); } }
  start_video();
}
static void on_yt(const Trailer *t, const char *err, void *ud) {
  if (!P.open || P.closing || (unsigned)(uintptr_t)ud != p2pTok) return; P.resolving = 0; P.ytTok = 0;
  if (!t) { fail(T("Couldn’t play this YouTube video")); fprintf(stderr, "[player] YouTube: %s\n", err ? err : "?"); return; }
  snprintf(P.url, sizeof P.url, "%s", t->url); snprintf(P.alt, sizeof P.alt, "%s", t->alt); start_video();
}
static void play_stream(const Stream *s, const char *vid, double startAt) {
  if (!s->url[0]) { if (s->ext[0]) app_open_external(s->ext, s->name); return; } /* a page to open elsewhere: never played here */
  if (!can_play(s)) { toast(T("Couldn’t start the torrent engine"), 2500, NULL); return; }
  prepare(vid, startAt, 1);
  if (!strcmp(P.type, "other")) { memcpy(trSubs, trNext, sizeof trSubs); trSubsN = trNextN; snprintf(P.trName, sizeof P.trName, "%.*s", (int)strcspn(s->name, "\n"), s->name); }
  else trSubsN = 0;
  trNextN = 0;
  P.st = *s; snprintf(P.url, sizeof P.url, "%s", s->url); snprintf(P.alt, sizeof P.alt, "%s", nextAlt); nextAlt[0] = 0;
  if ((s->hdrs[0] || s->rhdrs[0]) && !strncmp(s->url, "http", 4)) { hproxy_url(s->url, s->hdrs, s->rhdrs, P.url, sizeof P.url); fprintf(stderr, "[player] the stream wants its own headers: through %s\n", P.url); } /* every request for it carries them */
  char info[700]; sb_stream_info(s, info, sizeof info); snprintf(P.sname, sizeof P.sname, "%s", info); snprintf(P.saddon, sizeof P.saddon, "%s", s->addon);
  if (strncmp(s->url, "p2p://", 6)) p2p_note_direct_play(); /* a direct title evicts a kept torrent cache */
  if (!P.isTrailer && strcmp(P.type, "other")) sb_remember_stream(P.id, P.vid, s); /* the torrent itself, resolved again next time */
  if (!strncmp(P.url, "ytid:", 5)) { /* a YouTube video: found as the trailers are, behind the title art */
    P.resolving = 1; set_status(""); p2pTok++; P.ytTok = trailer_find(NULL, P.url + 5, 0, on_yt, (void *)(uintptr_t)p2pTok); return; }
  if (!strncmp(P.url, "p2p://", 6)) { /* the engine finds the peers and the file while the title art shows how it goes */
    P.resolving = 1; set_status(""); p2pTok++; p2p_resolve(P.url, on_p2p, (void *)(uintptr_t)p2pTok); return; } /* the loading line alone while it is quick; a reason once it is slow */
  start_video();
}
/* "Play the best stream automatically": the title art comes up at once with one quiet line about the wait, Back
   cancels, and the pick plays through the normal path */
static unsigned bestTok, bestSess;
static void on_best_status(const char *t, void *ud) { if (P.open && !P.closing && !P.started && bestTok && (unsigned)(uintptr_t)ud == bestSess) set_status(t); }
static void on_best_done(const Stream *s, const BestRate *r, void *ud) {
  if (!P.open || P.closing || P.started || (unsigned)(uintptr_t)ud != bestSess) return; bestTok = 0;
  if (!s) { fail(T("No stream suits this TV. Pick one from the list")); return; }
  fprintf(stderr, "[best] %s\n", r ? r->sum : ""); if (r) set_status(r->sum);
  Stream c = *s; play_stream(&c, P.vid, 0);
}
/* the logo the page showed (its own address, which can differ from the details' one: a translated logo, a board's):
   the player shows that very picture, already loaded, so it always flies in and is never drawn again */
static char pageLogo[512], pageLogoId[64];
static void open_title(const char *type, const char *id) {
  if (bestTok) { best_choose_cancel(bestTok); bestTok = 0; }
  if (!P.open) { memset(&P, 0, sizeof P); sx_begin_quick(1); /* the page's logo moves into the player */
    char sk[80]; snprintf(sk, sizeof sk, "logo:%s", id); const char *u = img_src_of(sx_from_tex(sk));
    snprintf(pageLogo, sizeof pageLogo, "%s", u ? u : ""); snprintf(pageLogoId, sizeof pageLogoId, "%s", id); }
  if (P.open && strcmp(P.id, id)) { P.name[0] = P.poster[0] = P.bg[0] = P.logo[0] = P.epTitle[0] = P.year[0] = P.genre[0] = 0; P.season = P.episode = 0; if (P.meta) { cJSON_Delete(P.meta); P.meta = NULL; } P.videos = NULL; } /* another title while it is open (a phone casting): never the last one's art while this one loads */
  snprintf(P.type, sizeof P.type, "%s", type); snprintf(P.id, sizeof P.id, "%s", id);
  cJSON *m = sb_meta_cached(type, id); if (!m && !strcmp(type, "other")) { m = sb_meta_cached("movie", id); if (!m) m = sb_meta_cached("series", id); } /* a trailer: the title's own art */
  if (m) { if (P.meta) cJSON_Delete(P.meta); P.meta = cJSON_Duplicate(m, 1); } else if (strcmp(type, "other")) { if (P.meta) cJSON_Delete(P.meta); P.meta = NULL; P.videos = NULL; ask_meta(); }
  if (P.meta) { snprintf(P.name, sizeof P.name, "%s", js_str(P.meta, "name", "")); snprintf(P.poster, sizeof P.poster, "%s", js_str(P.meta, "poster", "")); Item it = lib_item(); snprintf(it.background, sizeof it.background, "%s", js_str(P.meta, "background", "")); sb_bg_mid(&it, P.bg, sizeof P.bg); }
  else { if (artBg[0]) snprintf(P.bg, sizeof P.bg, "%s", artBg); if (artLogo[0]) snprintf(P.logo, sizeof P.logo, "%s", artLogo); if (artPoster[0]) snprintf(P.poster, sizeof P.poster, "%s", artPoster); if (artName[0] && !P.name[0]) snprintf(P.name, sizeof P.name, "%s", artName); /* the pictures it was handed (a phone's) */
    Item it = lib_item(); if (!P.bg[0]) { it.background[0] = 0; sb_bg_mid(&it, P.bg, sizeof P.bg); } if (!P.logo[0]) sb_logo_of(&it, P.logo, sizeof P.logo); } /* else its pictures by its id while its details are on their way */
  artName[0] = artPoster[0] = artBg[0] = artLogo[0] = 0;
  { char pu[512], lu[512]; Item it = lib_item(); sb_poster_of(&it, pu, sizeof pu); sb_logo_of(&it, lu, sizeof lu); accent_scope(id, pu, lu); }
}
/* the title's art at once, before there is anything to play (a trailer being found): status lines on it, a failure on it,
   Back closes it; the play that follows starts behind the same art */
void player_prepare(const char *type, const char *id, const char *vid, const char *trailerName) {
  open_title(type, id); prepare(vid, 0, 0); snprintf(P.trName, sizeof P.trName, "%s", trailerName ? trailerName : "");
}
void player_status(const char *text) { if (P.open && !P.closing) set_status(text); }
void player_fail(const char *text) { if (P.open && !P.closing) fail(text); }
void player_close(void) { close_player(0, 0); }
int  player_waiting(void) { return P.open && !P.closing && !P.started && !P.failed; }
/* after a failure: the best of the other streams, behind the same art (the failed one is left out) */
static void try_next(void) {
  fprintf(stderr, "[player] trying the next stream\n"); prepare(P.vid, 0, 0);
  set_status(T("Choosing the best stream")); bestSess++;
  bestTok = best_choose(P.type, P.vid, best_dur(P.meta, P.type), on_best_status, on_best_done, (void *)(uintptr_t)bestSess);
}
void player_choose_best(const char *type, const char *id, const char *vid) {
  open_title(type, id); prepare(vid, 0, 0);
  set_status(T("Choosing the best stream")); bestSess++;
  bestTok = best_choose(type, P.vid, best_dur(P.meta, type), on_best_status, on_best_done, (void *)(uintptr_t)bestSess);
}
void player_play(const Stream *s, const char *type, const char *id, const char *vid, int fromStart) { open_title(type, id); play_stream(s, vid, fromStart ? -1 : 0); }
/* ---- cast (core/castlink.c): a phone plays a stream here and drives it ---- */
void player_next_subs(const char *const *urls, const char *const *langs, int n) {
  memset(castSubs, 0, sizeof castSubs); if (n > 16) n = 16;
  for (int i = 0; i < n; i++) { SubEntry *e = &castSubs[i]; snprintf(e->url, sizeof e->url, "%s", urls[i]); snprintf(e->lang, sizeof e->lang, "%s", langs[i] && *langs[i] ? langs[i] : "und"); snprintf(e->name, sizeof e->name, "%s", e->lang); e->n = i + 1; e->from = SUB_STREAM; }
  castSubsN = n;
}
void player_next_art(const char *name, const char *poster, const char *bg, const char *logo) {
  snprintf(artName, sizeof artName, "%s", name ? name : ""); snprintf(artPoster, sizeof artPoster, "%s", poster ? poster : ""); snprintf(artBg, sizeof artBg, "%s", bg ? bg : ""); snprintf(artLogo, sizeof artLogo, "%s", logo ? logo : ""); }
void player_play_at(const Stream *s, const char *type, const char *id, const char *vid, double pos) { open_title(type, id); play_stream(s, vid, pos > 0 ? pos : 0); }
void player_cast(int cmd, double pos) {
  if (!P.open || P.closing) return;
  if (cmd == 1 && !paused()) { video_pause(); save_progress(1); } else if (cmd == 2 && paused()) video_play();
  else if (cmd == 3 && P.live) video_seek(pos < 0 ? 0 : pos); else if (cmd == 4) close_player(0, 0);
}
/* the subtitles and audio as the trays list them, for a phone: ids it hands back as they are ("off", "emb:<track>",
   "ext1:<n>" the stream's and OpenSubtitles', "ext2:<n>" the addons', audio "<track>" or "conv:<n>" a converted one) */
char *player_cast_tracks(void) {
  if (!P.open || P.closing) return NULL;
  cJSON *j = cJSON_CreateObject(), *sl = cJSON_AddArrayToObject(j, "subs"), *al = cJSON_AddArrayToObject(j, "audio"); char id[48], lb[200];
  cJSON_AddStringToObject(j, "t", "tracks");
  for (int i = 0; i < P.nEmb; i++) { cJSON *o = cJSON_CreateObject(); snprintf(id, sizeof id, "emb:%d", P.emb[i].idx); emb_label(i, lb, sizeof lb);
    cJSON_AddStringToObject(o, "id", id); cJSON_AddStringToObject(o, "label", lb); cJSON_AddStringToObject(o, "lang", P.emb[i].lang); cJSON_AddStringToObject(o, "from", T("Embedded")); cJSON_AddItemToArray(sl, o); }
  char cur[48] = "off";
  if (!strncmp(P.curSub, "emb:", 4)) snprintf(cur, sizeof cur, "%s", P.curSub);
  for (int li = 1; li <= 2; li++) { const SubEntry *l = li == 1 ? P.ext : P.adx; int n = li == 1 ? P.nExt : P.nAdx;
    for (int i = 0; i < n; i++) { const SubEntry *e = &l[i]; cJSON *o = cJSON_CreateObject(); snprintf(id, sizeof id, "ext%d:%d", li, i); const char *ln = lang_name(e->lang);
      if (*ln) snprintf(lb, sizeof lb, "%s%s%s", ln, e->label[0] ? " · " : "", e->label); else snprintf(lb, sizeof lb, "%s", T(e->name));
      cJSON_AddStringToObject(o, "id", id); cJSON_AddStringToObject(o, "label", lb); cJSON_AddStringToObject(o, "lang", e->lang); cJSON_AddStringToObject(o, "from", e->addon[0] ? e->addon : "OpenSubtitles"); cJSON_AddItemToArray(sl, o);
      if (P.pick.list == li && !strncmp(P.curSub, "ext:", 4) && !strcmp(P.curSub + 4, e->url)) snprintf(cur, sizeof cur, "%s", id); } }
  cJSON_AddStringToObject(j, "sub", cur);
  if (P.dts.conv) { MkvAudio ma[16]; int m = mkv ? mkv_audio_tracks(mkv, ma, 16) : 0;
    for (int i = 0; i < m; i++) { cJSON *o = cJSON_CreateObject(); snprintf(id, sizeof id, "conv:%d", i); const char *ln = lang_name(ma[i].lang); snprintf(lb, sizeof lb, "%s", *ln ? ln : ma[i].lang[0] ? ma[i].lang : T("Audio"));
      cJSON_AddStringToObject(o, "id", id); cJSON_AddStringToObject(o, "label", lb); cJSON_AddStringToObject(o, "lang", ma[i].lang); cJSON_AddItemToArray(al, o); }
    snprintf(id, sizeof id, "conv:%d", P.dts.convOrd); cJSON_AddStringToObject(j, "aud", id); }
  else { VidTrack ts[16]; int n = video_audio_tracks(ts, 16), sel = P.curAud >= 0 ? P.curAud : video_audio_current();
    for (int i = 0; i < n; i++) { cJSON *o = cJSON_CreateObject(); snprintf(id, sizeof id, "%d", ts[i].idx); const char *ln = lang_name(ts[i].lang);
      if (*ln) snprintf(lb, sizeof lb, "%s", ln); else if (ts[i].lang[0]) snprintf(lb, sizeof lb, "%s", ts[i].lang); else snprintf(lb, sizeof lb, "%s %d", T("Audio"), i + 1);
      if (ts[i].name[0] && strcmp(ts[i].name, lb)) { size_t L = strlen(lb); snprintf(lb + L, sizeof lb - L, " · %s", ts[i].name); }
      cJSON_AddStringToObject(o, "id", id); cJSON_AddStringToObject(o, "label", lb); cJSON_AddStringToObject(o, "lang", ts[i].lang); cJSON_AddItemToArray(al, o); }
    snprintf(id, sizeof id, "%d", sel); cJSON_AddStringToObject(j, "aud", id); }
  cJSON_AddNumberToObject(j, "delay", (int)lround(P.subDelay * 1000));
  char *s = cJSON_PrintUnformatted(j); cJSON_Delete(j); return s;
}
/* a phone's pick, as the trays make it: kind "sub" (id or "off"), "audio" (id), "subdelay" (ms, absolute) */
void player_cast_track(const char *kind, const char *id, int ms) {
  if (!P.open || P.closing) return;
  if (!strcmp(kind, "subdelay")) { double v = ms / 1000.0; if (v > 120) v = 120; if (v < -120) v = -120; P.subDelay = fabs(v) < 1e-6 ? 0 : v; P.dlyAt = now_(); refresh_tray(4); return; }
  if (!strcmp(kind, "sub")) {
    if (!strcmp(id, "off")) subs_off();
    else if (!strncmp(id, "emb:", 4)) set_emb_sub(atoi(id + 4));
    else if (!strncmp(id, "ext1:", 5) || !strncmp(id, "ext2:", 5)) { int li = id[3] - '0', k = atoi(id + 5); const SubEntry *l = li == 1 ? P.ext : P.adx; int n = li == 1 ? P.nExt : P.nAdx; if (k >= 0 && k < n) set_ext_sub(li, &l[k]); }
    refresh_tray(4); return; }
  if (!strcmp(kind, "audio")) {
    if (!strncmp(id, "conv:", 5)) { int ord = atoi(id + 5);
      if (P.dts.conv == 2 && ord != P.dts.convOrd && ord >= 0) { P.dts.convOrd = ord; P.dts.conv = 1; P.dts.convAt = video_time(); P.dts.convT0 = now_();
        int fl = conv_flags(ord); P.dts.copyA = !!(fl & HC_COPYAUDIO); video_unload(); P.live = 0; P.liveT0 = 0; hls_conv_start(P.url, ord, fl); fprintf(stderr, "[dts] audio track %d instead (phone)\n", ord); } }
    else set_audio(atoi(id));
    refresh_tray(3); }
}
int player_cast_state(double *pos, double *du, char *title, int cap) {
  *pos = *du = 0; if (title && cap) title[0] = 0;
  if (!P.open || P.closing) return 0;
  if (title && cap) snprintf(title, (size_t)cap, "%s", P.name);
  if (P.failed) return 5;
  VidState vs = video_state(); if (!P.live) return vs == VID_ERROR ? 5 : 1;
  *pos = video_time(); *du = dur();
  return vs == VID_ENDED ? 4 : vs == VID_ERROR ? 5 : paused() ? 3 : vs == VID_LOADING || vs == VID_BUFFERING ? 1 : 2;
}
static void finish_close(void) { sync_stop(0); gfx_target_free(&handSnap); mo_set(&handA, 0); video_unload(); asssubs_close(); if (mkv) { mkv_close(mkv); mkv = NULL; } p2p_player_closed(); p2pTok++; metaTok++; P.open = 0; P.closing = 0; if (P.meta) cJSON_Delete(P.meta); P.meta = NULL; subs_free(P.cues, P.ncues); free(P.ext); free(P.adx); srcs_free(); cols_clear(); memset(&P, 0, sizeof P); sub_reset(); app_refresh_boards(); }
static int closeNext, closeSim, closeArt; static double closeAt, closeT0; /* closeArt: closed on the loading screen (its line and words fade as the logo flies back) */
static void close_player(int goNext, int goSim) {
  if (!P.open || P.closing) return; if (bestTok) { best_choose_cancel(bestTok); bestTok = 0; } P.closing = 1; closeArt = !P.live && P.artA.v > 0.999f; closeT0 = now_(); sx_begin_quick(0); /* the player's logo moves back to the page */ closeNext = goNext; closeSim = goSim; P.ssOn = 0; P.nx.tok = 0; P.sk.tok = 0;
  save_progress(1); video_pause();
  /* the title's own picture fades back over the paused frame, the video goes, then the cover fades onto the app */
  closeAt = now_() + (closeArt ? 0.01 : 0.26); /* the cover fades back over the paused frame (see css()); from the loading screen the art fades at once, under the logo on its way back */
}
static void close_step(void) {
  if (!P.closing || now_() < closeAt) return;
  if (closeAt > 0) { closeAt = 0; video_unload(); hls_conv_stop(); P.live = 0; P.exitFade = 1;
    if (closeNext && closeVid[0]) { DetailParams p; memset(&p, 0, sizeof p); p.it = lib_item(); snprintf(p.it.videoId, sizeof p.it.videoId, "%s", closeVid); p.openStreams = 1; closeVid[0] = 0; app_go(SCR_DETAIL, &p, 0); }
    else if (closeSim) { DetailParams p; memset(&p, 0, sizeof p); p.it = lib_item(); app_go(SCR_SIM, &p, 0); }
    else accent_scope(SCR == SCR_DETAIL ? P.id : NULL, P.poster, P.logo); }
  if (P.exitFade && P.in.v <= 0.001f && !P.in.on) finish_close();
}

/* ---- per frame ---- */
/* the info chips, grouped as the page groups them: [resolution] [HDR · audio format] [size] | [audio language + channels]
   [subtitles]. Kept per place: only a new chip comes in, each 0.05 s after the one before it (the subtitles' last); a
   chip whose words change (another subtitle, another audio track) crossfades them in place, its width gliding */
static float chip_delay(int i) { return i >= 1 ? 0.05f * (i < 6 ? i : 6) : 0; }
static void chips_in(void) { double n = now_(); for (int i = 0; i < P.nchip; i++) { P.chip[i].at = n; P.chip[i].delay = chip_delay(i); P.chip[i].xAt = 0; } }
static void info_update(void) {
  if (P.isTrailer) { P.nchip = 0; return; }
  char key[256]; snprintf(key, sizeof key, "%s|%d|%d%s|%s|%s|%d", P.res ? P.res : "", P.curAud, P.pick.list, P.pick.addon, P.curSub, P.sizeTxt, P.nEmb); if (!strcmp(key, P.infoKey)) return;
  int first = !P.infoKey[0]; snprintf(P.infoKey, sizeof P.infoKey, "%s", key);
  char hdr[32], au[48]; hdr_of(P.sname, hdr, sizeof hdr); audio_of(P.sname, au, sizeof au);
  char codec[48]; snprintf(codec, sizeof codec, "%s", au); char *sp = strrchr(codec, ' '), chs[8] = ""; if (sp && sp[1] >= '0' && sp[1] <= '9' && sp[2] == '.') { snprintf(chs, sizeof chs, "%s", sp + 1); *sp = 0; }
  char fmt[96]; snprintf(fmt, sizeof fmt, "%s%s%s%s", !strncmp(hdr, "DV", 2) ? "Dolby Vision" : hdr, !strncmp(hdr, "DV", 2) ? hdr + 2 : "", hdr[0] && codec[0] ? " · " : "", codec); /* the page's "  ·  " collapses to one space each side */
  char aud[96] = ""; { VidTrack ts[16]; int nt = video_audio_tracks(ts, 16); int sel = P.curAud >= 0 ? P.curAud : video_audio_current(); for (int i = 0; i < nt; i++) if (ts[i].idx == sel) { const char *ln = lang_name(ts[i].lang); snprintf(aud, sizeof aud, "%s", *ln ? ln : ts[i].name); } }
  if (chs[0]) { size_t l = strlen(aud); snprintf(aud + l, sizeof aud - l, "%s%s", l ? " " : "", chs); }
  char sub[140] = "";
  if (!strncmp(P.curSub, "emb:", 4)) { int nn = atoi(P.curSub + 4); for (int i = 0; i < P.nEmb; i++) if (P.emb[i].idx == nn) { char lb[120]; emb_label(i, lb, sizeof lb); snprintf(sub, sizeof sub, "%s · %s", lb, T("In the file")); } }
  else if (!strncmp(P.curSub, "ext:", 4)) { int i = sub_row(1, P.ext, P.nExt); if (i >= 0) { const char *ln = lang_name(P.ext[i].lang); snprintf(sub, sizeof sub, "%s · %s", *ln ? ln : T(P.ext[i].name), T("External")); }
    if ((i = sub_row(2, P.adx, P.nAdx)) >= 0) { const char *ln = lang_name(P.adx[i].lang); snprintf(sub, sizeof sub, "%s · %s", *ln ? ln : T(P.adx[i].name), P.adx[i].addon); } }
  struct { const char *t, *icon; int bold, sep; } nw[8]; int n = 0;
  if (P.res && *P.res) { nw[n].t = P.res; nw[n].icon = NULL; nw[n].bold = 1; nw[n++].sep = 0; }
  if (fmt[0]) { nw[n].t = fmt; nw[n].icon = NULL; nw[n].bold = 0; nw[n++].sep = 0; }
  if (P.sizeTxt[0]) { nw[n].t = P.sizeTxt; nw[n].icon = NULL; nw[n].bold = 0; nw[n++].sep = 0; }
  if (n && (aud[0] || sub[0])) { nw[n].t = "|"; nw[n].icon = NULL; nw[n].bold = 0; nw[n++].sep = 1; }
  if (aud[0]) { nw[n].t = aud; nw[n].icon = "p_spk"; nw[n].bold = 0; nw[n++].sep = 0; }
  if (sub[0]) { nw[n].t = sub; nw[n].icon = "p_cc"; nw[n].bold = 0; nw[n++].sep = 0; }
  double now = now_();
  for (int i = 0; i < n; i++) {
    int kept = i < P.nchip && P.chip[i].sep == nw[i].sep && P.chip[i].icon == nw[i].icon && !strcmp(P.chip[i].t, nw[i].t);
    int same = i < P.nchip && P.chip[i].sep == nw[i].sep && P.chip[i].icon == nw[i].icon && !nw[i].sep && P.chip[i].at && now - P.chip[i].at > P.chip[i].delay + .42; /* there and settled: only its words change */
    if (!kept && same) { int b = P.chip[i].bold; snprintf(P.chip[i].old, sizeof P.chip[i].old, "%s", P.chip[i].t); P.chip[i].oldW = text_width(P.chip[i].old, 24, b ? 800 : 600, b ? 0.5f : 0); P.chip[i].xAt = now; snprintf(P.chip[i].t, sizeof P.chip[i].t, "%s", nw[i].t); }
    else if (!kept) { snprintf(P.chip[i].t, sizeof P.chip[i].t, "%s", nw[i].t); P.chip[i].icon = nw[i].icon; P.chip[i].bold = nw[i].bold; P.chip[i].sep = nw[i].sep; P.chip[i].at = now; P.chip[i].delay = chip_delay(i); P.chip[i].xAt = 0; }
  }
  P.nchip = n; if (first) chips_in();
}
/* names and stories of the episodes in the app's language (TMDB), once they are known */
static void loc_apply(void) {
  if (P.locDone || P.isTrailer || strcmp(P.type, "series")) return;
  int st = tmdb_state(P.type, P.id, 0); if (st == TMDB_PENDING) return; P.locDone = 1; if (st != TMDB_READY) return;
  const TmdbLoc *L = tmdb_loc(P.type, P.id, 0); if (!L) return; const char *nm, *ov;
  if (P.season > 0 && P.episode > 0 && tmdb_episode(L, P.season, P.episode, &nm, &ov) && *nm) snprintf(P.epTitle, sizeof P.epTitle, "%s", nm);
  if (P.next.has) { int s = 0, e = 0; const char *c = strrchr(P.next.vid, ':'); if (c && c > P.next.vid) { const char *q = c - 1; while (q > P.next.vid && *q != ':') q--; s = atoi(q + 1); e = atoi(c + 1); }
    if (tmdb_episode(L, s, e, &nm, &ov)) { if (*nm) snprintf(P.next.title, sizeof P.next.title, "%s", nm); if (*ov) snprintf(P.next.ov, sizeof P.next.ov, "%s", ov); } }
}
/* a picture that stopped while the TV still says "playing": the time runs on between the TV's reports, so a frozen
   playhead shows as the time pulled back by each report (or, where it is not run on, as a time that does not move) */
static int stalled(VidState vs) {
  static double lastT = -1, backAt, sameSince; double t = video_time(), n = now_();
  if (vs != VID_PLAYING || P.seekT >= 0) { lastT = -1; backAt = 0; sameSince = 0; return 0; }
  if (lastT >= 0) { if (t < lastT - 0.25 && t > lastT - 5) backAt = n; /* (a bigger jump is a seek) */
    if (fabs(t - lastT) < 0.001) { if (!sameSince) sameSince = n; } else sameSince = 0; }
  lastT = t;
  return (backAt && n - backAt < 0.9) || (sameSince && n - sameSince > 0.5);
}
/* the viewer notice, asked as the picture comes (a trailer has none): IMDb's Parents Guide votes read as an age
   (any severe 16+, three severe 18+, moderate 13+, mild only 7+) and the reasons with how much; when the guide has
   nothing (or cannot be reached), the official rating's age alone (TMDB). Nothing usable within 5 s: no notice. */
static void pg_decide(void) {
  if (P.pg.at || !P.pg.asked || now_() - P.pg.askAt > 5) return; /* (shown once; never late) */
  if (P.pg.guideIn && P.pg.g.n) { int sev = 0, mod = 0; for (int i = 0; i < P.pg.g.n; i++) { sev += P.pg.g.lvl[i] == PG_SEVERE; mod += P.pg.g.lvl[i] == PG_MODERATE; }
    P.pg.age = sev >= 3 ? 18 : sev ? 16 : mod ? 13 : 7; P.pg.at = now_(); return; }
  if (P.pg.guideIn && P.pg.certIn && P.pg.certAge >= 0) { P.pg.age = P.pg.certAge; P.pg.at = now_(); } /* (the age alone, no reasons) */
}
static void on_pg(const ParGuide *g, void *ud) { if (!P.open || P.closing || (unsigned)(uintptr_t)ud != P.pg.sess) return; if (g) P.pg.g = *g; P.pg.guideIn = 1; fprintf(stderr, "[notice] guide: %d reasons\n", g ? g->n : -1); pg_decide(); }
static void on_age(const Rating *r, void *ud) { if (!P.open || P.closing || (unsigned)(uintptr_t)ud != P.pg.sess) return; P.pg.certAge = rating_age(r); P.pg.certIn = 1; fprintf(stderr, "[notice] rating \"%s\" -> age %d\n", r ? r->rating : "", P.pg.certAge); pg_decide(); }
static void pg_ask(void) { if (P.pg.asked || P.isTrailer || strncmp(P.id, "tt", 2)) return; P.pg.asked = 1; P.pg.askAt = now_();
  parental_get(P.id, on_pg, (void *)(uintptr_t)P.pg.sess); rating_get(P.type, P.id, on_age, (void *)(uintptr_t)P.pg.sess); }
/* the page's classes, as targets: each motion goes there with the duration, curve and delay the page gives that state */
static void css(void) {
  int tr = P.mode == 2 && P.tray.kind > 0, ss = P.ssOn, osd = P.osd && !ss, pz = P.pausedShown, skm = P.skm && !osd;
  if (osd && !tr) { mo_to(&P.ttlA, 1, .42f, C_OUT, 0); mo_to(&P.ttlX, 0, .42f, C_OUT, 0); mo_to(&P.ttlY, 0, .42f, C_OUT, 0); }
  else if (osd) { mo_to(&P.ttlA, 0, .42f, C_OUT, 0); mo_to(&P.ttlX, 0, .42f, C_OUT, 0); mo_to(&P.ttlY, 10, .42f, C_OUT, 0); } /* a tray takes its place: it sinks a little */
  else { mo_to(&P.ttlA, 0, .2f, C_EASE, 0); mo_to(&P.ttlX, -30, .2f, C_EASE, 0); mo_to(&P.ttlY, 0, .2f, C_EASE, 0); }
  if (osd) { mo_to(&P.barA, tr ? 0 : 1, .36f, C_EASE, .06f); mo_to(&P.barY, tr ? 10 : 0, .36f, C_OUT, .06f); mo_to(&P.btnA, 1, .36f, C_EASE, .12f); mo_to(&P.btnY, 0, .36f, C_OUT, .12f); }
  else { mo_to(&P.barA, skm ? 1 : 0, .2f, C_EASE, 0); mo_to(&P.barY, skm ? 0 : 18, .2f, C_EASE, 0); mo_to(&P.btnA, 0, .2f, C_EASE, 0); mo_to(&P.btnY, 24, .2f, C_EASE, 0); }
  if (ss) mo_to(&P.topR, 0, 1, C_EASE, 0); else { mo_to(&P.topR, osd ? 1 : 0, .3f, C_EASE, 0); mo_to(&P.topY, osd ? 0 : -12, .3f, C_EASE, 0); }
  if (ss) { mo_to(&P.dim, 0, 1, C_EASE, 0); mo_to(&P.dimT, 0, 1, C_EASE, 0); }
  else { mo_to(&P.dim, osd || pz ? 1 : skm ? .6f : 0, .3f, C_EASE, 0); mo_to(&P.dimT, osd || pz ? 1 : 0, .3f, C_EASE, 0); }
  mo_to(&P.vig, pz ? 1 : 0, .9f, C_EASE, 0); mo_to(&P.pzA, pz ? 1 : 0, .3f, C_EASE, 0);
  if (!P.closing && !ss && (!P.live || P.artA.v > 0.02f)) { mo_set(&P.dim, 0); mo_set(&P.dimT, 0); mo_set(&P.vig, 0); } /* the title's art fades straight into the picture: the shades come after it */
  if (P.closing) mo_to(&P.artA, 1, .2f, C_EASE, 0); else if (ss) mo_to(&P.artA, 1, 1.4f, C_EASE, 0); else mo_to(&P.artA, P.live ? 0 : 1, .45f, C_EASE, 0);
  if (P.exitFade) mo_to(&P.in, 0, .35f, C_EASE, 0); else mo_to(&P.in, 1, .22f, C_EASE, 0);
  mo_to(&P.lnA, P.live && !P.closing ? 0 : 1, .16f, C_EASE, 0); /* the line runs until the picture runs, then goes with the art */
  /* the spinner: loading, a seek on its way, or a picture that stopped moving; it comes after 0.3 s (a quick seek shows
     none) and goes as soon as the time moves again */
  VidState vs = video_state(); int wait = P.live && !paused() && (vs == VID_BUFFERING || vs == VID_LOADING || video_settling() || stalled(vs));
  if (wait) mo_to(&P.bufA, 1, .2f, C_EASE, .3f); else mo_to(&P.bufA, 0, .2f, C_EASE, 0);
  mo_to(&P.skA, P.skOn ? 1 : 0, .16f, C_EASE, 0); mo_to(&P.skS, P.skOn ? 1 : 0, .2f, C_SEEK, 0);
  mo_to(&P.knA, (P.mode == 1 && P.row == 1) || P.skOn ? 1 : 0, .15f, C_EASE, 0);

  mo_to(&P.prA, P.prompt.k ? 1 : 0, .3f, C_EASE, 0); mo_to(&P.prY, P.prompt.k ? 0 : 20, .3f, C_OUT, 0); mo_to(&P.prB, P.osd ? 440 : 104, .3f, C_EASE, 0);
  mo_to(&P.subB, tr ? SB_H * 0.62f : P.osd ? 420 : P.skm ? 200 : SB_H * 0.08f, .3f, C_EASE, 0);
  mo_to(&P.hvA, P.hvOn ? 1 : 0, .15f, C_EASE, 0); mo_to(&P.ccS, strcmp(P.curSub, "off") ? 0 : 1, .26f, C_OUT, 0);
  mo_to(&P.p2pA, P.isP2p && P.p2pLine[0] && osd ? 1 : 0, .3f, C_EASE, 0);
  { int on = P.pg.at > 0 && now_() - P.pg.at < 6.0; /* 6 s from when it comes, controls or not, then it fades; once only */
    mo_to(&P.pgA, on ? 1 : 0, on ? .5f : .55f, C_EASE, 0); mo_to(&P.pgY, on ? 0 : -14, on ? .6f : .55f, C_OUT, 0); }
  if (P.tray.kind > 0) { mo_to(&P.tray.open, 1, .38f, C_TRIN, 0); mo_to(&P.tray.openA, 1, .18f, C_EASE, 0); }
  else { mo_to(&P.tray.open, .12f, .26f, C_TROUT, 0); mo_to(&P.tray.openA, 0, .2f, C_EASE, .04f); }
}
static void foc_update(void) {
  int fi = P.bIdx; if (P.mode == 2 && P.tray.kind) { int ak = P.tray.kind < 0 ? -P.tray.kind : P.tray.kind; int b = btn_index(ak == 1 || ak == 5 ? "eps" : ak == 2 ? "src" : ak == 3 ? "aud" : "subs"); if (b >= 0) fi = b; }
  if (fi >= P.nbtn) fi = P.nbtn - 1; if (fi < 0) return;
  /* the focused button opens into a pill with its name: the others slide aside, the pill and the name follow (0.32 s) */
  int show = P.live && ((P.mode == 1 && P.row == 0) || P.mode == 2); float lw = show && P.btns[fi].lb[0] ? P.btns[fi].lw : 0;
  for (int k = 0; k < P.nbtn; k++) { float dx = 0; if (fi < P.nL && k < P.nL && k > fi) dx = lw; if (fi >= P.nL && k >= P.nL && k <= fi) dx = -lw; mo_to(&P.btns[k].dx, dx, .32f, C_OUT, 0); }
  float x = P.btns[fi].x0 + (fi >= P.nL ? -lw : 0), y = P.btns[fi].y, sz = P.btns[fi].w / 76;
  float fd = P.focSeen && fabsf(x - P.focX.to) > 260 ? .42f : .32f; /* across the gap between the two groups: the same glide, a little longer (never a jump) */
  if (!P.focSeen) { mo_set(&P.focX, x); mo_set(&P.focY, y); mo_set(&P.focW, (76 + lw) * sz); mo_set(&P.focH, 76 * sz); }
  else { mo_to(&P.focX, x, fd, C_OUT, 0); mo_to(&P.focY, y, fd, C_OUT, 0); mo_to(&P.focW, (76 + lw) * sz, fd, C_OUT, 0); mo_to(&P.focH, 76 * sz, fd, C_OUT, 0); }
  P.focSeen = 1; mo_to(&P.focA, show ? 1 : 0, .18f, C_EASE, 0);
  if (strcmp(P.tip, P.btns[fi].lb)) { snprintf(P.tip, sizeof P.tip, "%s", P.btns[fi].lb); P.tipAt = now_(); } /* a new name: its entrance again */
  mo_to(&P.tipA, lw > 0 ? 1 : 0, .14f, C_EASE, 0);
}
void player_tick(float dt) {
  hand_tick(dt);
  subs_pump(); /* subtitle files and lists, fetched on their own threads */
  if (!P.open) return; video_pump(); sync_tick();
  if (P.volA.v > 0.01f && now_() - P.volAt > 1.4) mo_to(&P.volA, 0, .3f, C_EASE, 0); /* the volume pill goes 1.4 s after the last change */
  css();
  Mo *mo[] = { &P.pgA, &P.pgY, &P.in, &P.artA, &P.ttlA, &P.ttlX, &P.ttlY, &P.barA, &P.barY, &P.btnA, &P.btnY, &P.topR, &P.topY, &P.dim, &P.dimT, &P.vig, &P.pzA, &P.bufA, &P.lnA, &P.skA, &P.skS, &P.knA, &P.volA,
    &P.prA, &P.prY, &P.prB, &P.subB, &P.ccS, &P.dlyA, &P.dlyR, &P.p2pA, &P.focX, &P.focY, &P.focW, &P.focH, &P.focA, &P.tipA, &P.hvA, &P.tray.open, &P.tray.openA, &P.tray.rowX, &P.tray.tabsX, &P.tray.ringX, &P.tray.ringY, &P.tray.ringA, &RL.t };
  if (P.skA.v <= 0.001f && !P.skOn) RL.key[0] = 0; /* the pill gone: its next time shows at once */
  for (size_t i = 0; i < sizeof mo / sizeof *mo; i++) mo_tick(mo[i], dt);
  for (int i = 0; i < 4; i++) { mo_tick(&P.tray.colY[i], dt); mo_tick(&P.tray.plY[i], dt); mo_tick(&P.tray.plA[i], dt); }
  for (int i = 0; i < P.nbtn; i++) { mo_tick(&P.btns[i].dx, dt); P.btns[i].x = P.btns[i].x0 + P.btns[i].dx.v; }
  nudge_tick(&P.nudge, dt);
  if (P.tray.kind < 0 && !P.tray.openA.on && P.tray.openA.v <= 0.001f) { P.tray.kind = 0; cols_clear(); srcs_free(); } /* put away: its lists go too */
  mo_tick(&SR.y, dt); mo_tick(&SR.plY, dt); mo_tick(&SR.plH, dt); mo_tick(&SR.plA, dt); tm_tick(dt); tray_swap_step(); src_tick(dt);
  if (!P.prompt.k && P.prA.v <= 0.001f) P.shown.k = 0;
  close_step(); if (P.closing) return;
  if (P.resolving) { const char *st = p2p_status(); if (strcmp(st, status)) set_status(st); return; } /* the art: nothing for a few seconds, then why it is slow ("Finding peers…", "Getting the file list…") */
  VidState vs = video_state();
  if (P.started && now_() - P.posTick >= 1.0) { P.posTick = now_(); double t = video_time(), b = video_buffered_end(); if (t > 0) P.lastPos = t;
    if (mkv) mkv_set_pos(mkv, t, b > t ? b - t : -1);
    dts_tick();
    if (!P.isTrailer && (!P.embKnown || !P.mkvTracksOk || mkv_state(mkv) == MKV_FALLBACK)) load_emb_tracks(); /* until the file's list settles (the TV's own list may come late) */
    if (P.isP2p) { p2p_pace(vs == VID_PLAYING, vs == VID_BUFFERING || vs == VID_LOADING, dur()); char ln[160]; int has = p2p_peer_line(ln, sizeof ln); snprintf(P.p2pLine, sizeof P.p2pLine, "%s", has ? ln : "");
      int waiting = (vs == VID_BUFFERING || vs == VID_LOADING) && !paused(); const char *why = p2p_wait_reason(waiting); /* one short reason under the ring once the wait is long */
      if (waiting && P.live && !strcmp(why, T("Waiting for the next pieces…"))) why = ""; /* mid-film the spinner says it alone */
      if (waiting && !P.failed) { if (strcmp(why, status)) set_status(why); } else if (vs == VID_PLAYING && status[0] && !P.failed) set_status(""); } }
  if (P.chapWant && P.sk.chapWait) { MkvChapter mc[64]; int n = mkv ? mkv_chapters(mkv, mc, 64) : 0; /* the file's chapters, once its head is read */
    if (n >= 0) { P.chapWant = 0; PlayerChapter pc[64]; for (int i = 0; i < n; i++) { snprintf(pc[i].name, sizeof pc[i].name, "%s", mc[i].name); pc[i].s = mc[i].s; pc[i].e = mc[i].e < 0 ? dur() : mc[i].e; } player_chapters(P.url, pc, n); } }
  if (P.dts.conv == 2 && !P.failed && (vs == VID_ERROR || (!video_live() && now_() - P.dts.convT0 > 25))) { dts_conv_failed(vs == VID_ERROR ? video_error() : "no picture after 25 s"); return; }
  if (P.dts.deferUntil) dts_defer_tick();
  if (vs == VID_ERROR && !P.failed && P.started && P.isP2p && !P.p2pRetried && p2p_moved_recently()) { /* the torrent's storage moved (a USB drive pulled out): the same stream once more, same moment */
    P.p2pRetried = 1; P.startAt = P.lastPos > 0 ? P.lastPos : 0; P.didResume = 0; P.live = 0; fprintf(stderr, "[player] stream error after the torrent storage moved: again at %.0f s\n", P.startAt); video_load(P.url); vs = video_state(); }
  if (vs == VID_ERROR && !P.failed && P.started) { if (P.alt[0] && !P.altTried) { P.altTried = 1; fprintf(stderr, "[player] error, the fallback URL\n"); video_load(P.alt); } else { if (!P.errAt) P.errAt = now_(); if (!P.sizeTok || P.sizeAns || now_() - P.errAt > 4) fail(fail_reason()); } } /* the first URL failed: the same file the other way; the reason waits for the server's answer (4 s at most) */
  if (P.nx.tok && !P.nx.done && now_() >= P.nx.until) nx_finish();
  if (P.metaRetryAt && now_() >= P.metaRetryAt) { P.metaRetryAt = 0; if (!P.next.has || !P.videos) ask_meta(); }
  if (P.extRetryAt && now_() >= P.extRetryAt) { P.extRetryAt = 0; load_ext_subs(P.extTry + 1); }
  if (P.adxRetryAt && now_() >= P.adxRetryAt) { P.adxRetryAt = 0; load_addon_subs(P.adxTry + 1); }
  if (P.embWaitAt && !P.embWaitOver && now_() >= P.embWaitAt) { P.embWaitOver = 1; auto_sub(); }
  sub_check(); ass_sync();
  sk_tick(); loc_apply();
  if (P.started && !P.ldd && dur() > 0) P.ldd = 1; /* the length is known: the loading line fades before the picture comes */
  /* the art stays until the picture really runs: the resume jump is made behind it, and it fades only once playing has
     been steady for a moment (never a frozen first frame, then a spinner) */
  if (!P.live && video_live()) {
    if (!P.liveT0) { P.liveT0 = now_(); P.steadyAt = 0; apply_aspect(); try_resume(); auto_audio(); load_emb_tracks(); }
    int steady = (video_state() == VID_PLAYING || paused()) && !video_settling(); if (!steady) P.steadyAt = 0; else if (!P.steadyAt) P.steadyAt = now_();
    if ((P.steadyAt && now_() - P.steadyAt > 0.35) || now_() - P.liveT0 > 15) { P.live = 1; pg_ask(); fprintf(stderr, "[player] picture running after %.1f s\n", now_() - P.liveT0); if (resumeMsg[0]) { toast(resumeMsg, 2500, NULL); resumeMsg[0] = 0; } } }
  if (!video_live()) P.liveT0 = 0;
  if (P.live && !P.skipsAsked && dur() > 0) load_skips(); /* once the length is known: segments are checked against it */
  if (P.live) {
    if (video_width() != P.lastVw || video_height() != P.lastVh) apply_aspect(); /* the picture's size became known or changed */
    if (P.aspRefreshAt && now_() >= P.aspRefreshAt) { P.aspRefreshAt = 0; if (paused()) video_seek(video_time()); }
    if (paused() && P.prompt.autoN && (P.prompt.k == 1 || P.prompt.k == 2)) { P.prompt.autoN = 0; paint_prompt(); } /* paused: no skipping by itself */
    /* the screensaver: paused and untouched for the time chosen in Settings, the controls fade away and the title's cover takes the screen */
    if (P.ssOn && !paused()) ss_exit();
    /* (the player has no screensaver of its own any more: the app's one comes over a paused film) */
    int pz = paused() && P.mode != 2; if (pz && !P.pausedShown) { if (!P.pausedAt) P.pausedAt = now_(); if (now_() - P.pausedAt > 0.35) { P.pausedShown = 1; show_osd(1); } }
    else if (!pz) { P.pausedAt = 0; P.pausedShown = 0; }
    if (P.seekT >= 0 && now_() >= P.seekCommitAt) commit_seek();
    if (P.seekTxtUntil && now_() >= P.seekTxtUntil) { P.seekTxtUntil = 0; P.skOn = 0; P.skm = 0; }
    if (P.mode == 1 && now_() >= P.hideAt && !paused() && P.seekT < 0) hide_osd();
    if (vs == VID_PLAYING && now_() - P.lastSave >= 10) { save_progress(0); if (dur() > 0 && video_time() / dur() > 0.95) mark_watched(); }
    if (vs == VID_ENDED && !P.ended) { P.ended = 1; save_progress(1); mark_watched(); if (strcmp(store_get("bingeWatch", "true"), "false") && strcmp(store_get("enableNextVideo", "true"), "false") && P.next.has) P.endGoAt = now_() + 0.6; else close_player(0, 0); }
    if (P.endGoAt && now_() >= P.endGoAt) { P.endGoAt = 0; go_next(); if (!P.open || P.closing) return; }
    check_prompt();
    if (P.prompt.autoN && (P.prompt.k == 1 || P.prompt.k == 2) && now_() - P.prompt.t0 >= P.prompt.adur && !paused()) prompt_go(0);
    else if (P.prompt.autoN && P.prompt.k == 3 && now_() - P.prompt.t0 >= 10) { P.prompt.autoN = 0; next_auto(); }
    if (!P.live) return; /* a new episode is being found */
    const char *r = res_of_video(video_width(), video_height()); if (*r && !P.isTrailer) P.res = r;
    info_update();
  }
  foc_update();
}

/* ---- drawing ---- */
/* the drawing's words measured, upper-cased or wrapped once per text (and language), not every frame */
typedef struct { char s[640]; int px, wt, n, lines; float tr, w, mw; unsigned lg; } TMemo;
static int tm_same(TMemo *m, const char *s, int px, int wt, float tr) { if (m->px == px && m->wt == wt && m->tr == tr && !strcmp(m->s, s)) return 1; snprintf(m->s, sizeof m->s, "%s", s); m->px = px; m->wt = wt; m->tr = tr; return 0; }
static float tm_width(TMemo *m, const char *s, int px, int wt, float tr) { if (!tm_same(m, s, px, wt, tr) || m->n != -1) { m->n = -1; m->w = text_width(s, px, wt, tr); } return m->w; }
static int tm_wrap(TMemo *m, const char *s, float maxW, int lines, int px, int wt, float tr) { if (!tm_same(m, s, px, wt, tr) || m->mw != maxW || m->lines != lines) { m->mw = maxW; m->lines = lines; m->n = text_wrap_count(s, maxW, lines, px, wt, tr); } return m->n; }
typedef struct { char src[200], up[200]; unsigned lg; } UMemo;
static const char *um_upper(UMemo *m, const char *s) { if (m->lg != i18n_gen() || strcmp(m->src, s)) { m->lg = i18n_gen(); snprintf(m->src, sizeof m->src, "%s", s); snprintf(m->up, sizeof m->up, "%s", s); text_upper(m->up, sizeof m->up); } return m->up; }
/* where the last frame put what the pointer can click (the tray's tabs, cards and columns, the prompt) */
static struct { int tray; float x, y, w, h; int ntab; float tabX[40], tabW[40], tabY, tabH; int nrow; float rowX[MAXROW], rowW[MAXROW], rowY, rowH;
  float colX[4], colW, colTop, colBot; int nit[4]; float itY[4][300], itH[4][300]; int prompt, card, npr; float prX[2], prY[2], prW[2], prH[2]; int src; float srcX, srcTop; } PG;
/* the paused vignette and the art's shade, baked once into small alpha pictures the GPU stretches, as the page draws them
   into tiny canvases: one quad each, no rings or bands */
static GLuint vigTex, shTex;
static void bake(void) {
  if (vigTex) return;
  int W = 384, H = 216; unsigned char *a = malloc((size_t)W * H);
  for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { float px = x + .5f, py = y + .5f, dx = (px - .6f * W) / (.744f * W), dy = (py - .4f * H) / (.62f * H), r = sqrtf(dx * dx + dy * dy);
    float ra = r <= .45f ? 0 : r >= 1 ? .6f : .6f * (r - .45f) / .55f, u = px / W, la = u < .55f ? .42f * (1 - u / .55f) : 0;
    a[y * W + x] = (unsigned char)((ra + la - ra * la) * 255 + .5f); }
  vigTex = gfx_tex_create(a, W, H, 1, 1);
  W = 256; H = 144; /* radial-gradient(ellipse at center): .28 in the middle, .92 from 78 % of the way to the corners */
  for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { float dx = (x + .5f - W / 2.0f) / (W / 2.0f * 1.41421f), dy = (y + .5f - H / 2.0f) / (H / 2.0f * 1.41421f), r = sqrtf(dx * dx + dy * dy);
    a[y * W + x] = (unsigned char)((r >= .78f ? .92f : .28f + .64f * r / .78f) * 255 + .5f); }
  shTex = gfx_tex_create(a, W, H, 1, 1); free(a);
}
static float txt_sh(const char *t, float x, float y, int px, int wt, uint32_t col, float ls, float maxW, int align, int sa) { /* text-shadow: 0 1px 0 */
  text_draw(t, x, y + 1, px, wt, BLACK_A(sa), ls, maxW, align); return text_draw(t, x, y, px, wt, col, ls, maxW, align); }
static const char *logo_url(void) { return pageLogo[0] && !strcmp(pageLogoId, P.id) ? pageLogo : P.logo; }
static int name_shown(GLuint logoTex) { /* the name only when there is no text art by 2.5 s (never for a few frames first) */
  const char *lu = logo_url(); if (logoTex) return 0; if (lu[0] && !img_failed(lu) && now_() - P.openedAt < 2.5) return 0;
  if (!P.nameAt) P.nameAt = now_(); return 1; }
static GLuint logo_fit(float mw, float mh, float *w, float *h) {
  const char *lu = logo_url(); float asp = 1; GLuint t = lu[0] ? img_get(lu, (int)mw, (int)mh, &asp) : 0; if (!t) { *w = *h = 0; return 0; }
  *w = mw; *h = mw / asp; if (*h > mh) { *h = mh; *w = mh * asp; } return t; }
static const char *trailer_name(void) { return P.trName[0] ? P.trName : T("Trailer"); }
static void draw_art(void) {
  float a = P.artA.v; if (a <= 0.001f) return; gfx_opacity_push(a); bake();
  int ss = P.ssOn && !P.closing; /* the screensaver: the cover comes back brighter and drifts very slowly (26 s each way); the logo alone */
  gfx_rect(0, 0, SB_W, SB_H, PAGE);
  float asp; GLuint bg = P.bg[0] ? img_get(P.bg, 1280, 720, &asp) : 0; if (bg) { float u0, v0, u1, v1; img_cover_uv(asp, SB_W / SB_H, 1, &u0, &v0, &u1, &v1);
    if (ss) { float ph = fmodf((float)(now_() - P.ssAt) / 26, 2), k = bez(C_INOUT, ph < 1 ? ph : 2 - ph), sc = 1.02f + 0.07f * k;
      gfx_scale_push(sc, sc, SB_W / 2, SB_H / 2); gfx_offset_push(SB_W * (-0.01f + 0.022f * k), SB_H * (-0.008f * k)); gfx_image(bg, 0, 0, SB_W, SB_H, 0, WHITE_A(199), u0, v0, u1, v1); gfx_offset_pop(); gfx_scale_pop(); }
    else gfx_image(bg, 0, 0, SB_W, SB_H, 0, WHITE_A(107), u0, v0, u1, v1); }
  gfx_alpha_tex(shTex, 0, 0, SB_W, SB_H, PAGE);
  float bk = !P.closing ? 1 : closeArt ? 1 - (float)((now_() - closeT0) / 0.22) : 0; /* closed while loading: the rest of the block fades as the logo leaves */
  if (!P.closing && sx_flying() && sx_since() < 0.5) { float q = (float)((sx_since() - 0.12) / 0.3); bk = q < 0 ? 0 : q > 1 ? 1 : q; bk = bk * bk * (3 - 2 * bk); } /* opened: it fades into place as the logo lands */
  if (!P.closing || bk > 0.001f) { /* the title block, centred as one column: art, trailer tag, loading line, status */
    float lw, lh; GLuint t = logo_fit(760, 260, &lw, &lh); int nm = name_shown(t), nl = 0;
    static TMemo nmW; if (nm) { nl = tm_wrap(&nmW, P.name, SB_W * 0.8f, 3, 78, 800, -1.5f); lh = nl * 78 * 1.05f; }
    int tag = P.isTrailer || !strcmp(P.type, "other"), ln = !ss && !P.failed; float L24 = text_line_height(24), L25 = text_line_height(25);
    float tot = lh + (tag && !ss ? 28 + L24 : 0) + (ln ? 49 : 0) + (ss ? 0 : 20 + fmaxf(34, L25)), y = SB_H / 2 - tot / 2;
    float o = 1; if (!P.live && !ss) { float p = fmodf((float)(now_() - P.openedAt) / 3.6f + .5f, 1); /* from full strength: it lands as it flew */ o = p < .5f ? .9f + .1f * bez(C_INOUT, p * 2) : 1 - .1f * bez(C_INOUT, (p - .5f) * 2); } /* breathing */
    if (P.closing) {} /* (the page's copy flies back) */
    else if (t) { char sk[80]; snprintf(sk, sizeof sk, "logo:%s", P.id); sx_image(sk, t, SB_W / 2 - lw / 2, y, lw, lh, 0, WHITE_A((int)(255 * o)), 0, 0, 1, 1); }
    else if (nm) { float k = kf(P.nameAt, .45f, C_LGIN, 0); { char sk[80]; snprintf(sk, sizeof sk, "logo:%s", P.id); sx_text_wrapped(sk, P.name, SB_W * 0.1f, y, SB_W * 0.8f, 3, 78, 800, WHITE_A((int)(255 * o * k)), -1.5f, ALIGN_CENTER, 78 * 1.05f); } }
    y += lh; if (bk < 1) gfx_opacity_push(bk);
    if (tag && !ss) { y += 28; static UMemo tgU; static TMemo tgW; const char *tg = um_upper(&tgU, trailer_name()); float tw = tm_width(&tgW, tg, 24, 700, 8), totW = 84 + 22 + tw + 14 + 84, x0 = SB_W / 2 - totW / 2, ly = y + L24 / 2 - 1;
      gfx_rrect_h(x0, ly, 84, 2, 0, col_alpha(ACC, 0), ACC2); text_draw(tg, x0 + 84 + 22, y, 24, 700, WHITE_A(255), 8, 0, ALIGN_LEFT); gfx_rrect_h(x0 + 84 + 22 + tw + 14, ly, 84, 2, 0, ACC2, col_alpha(ACC, 0)); y += L24; }
    if (ln) { y += 46; if (P.lnA.v > 0.001f) { float x0 = SB_W / 2 - 130, p = bez(C_INOUT, clock_ph(1.5)), sx = x0 - 117 + 380.25f * p; /* 45 % of the line, from -100 % to 225 % of itself */
        gfx_opacity_push(P.lnA.v); gfx_clip_push(x0, y, 260, 3); gfx_rrect_h(sx, y, 58.5f, 3, 2, col_alpha(ACC, 0), ACC2); gfx_rrect_h(sx + 58.5f, y, 58.5f, 3, 2, ACC2, col_alpha(ACC, 0)); gfx_clip_pop(); gfx_opacity_pop(); }
      y += 3; }
    if (!ss) { y += 20; xline_draw(&stX[0], loading_line(status), SB_W / 2, y, 25, P.failed ? RGB(0xff, 0xb9, 0xb9) : WHITE_A(168), SB_W - 200, ALIGN_CENTER); }
    if (bk < 1) gfx_opacity_pop(); }
  gfx_opacity_pop();
}
/* the subtitle style, read once while the settings stay as they are */
typedef struct { unsigned ser; int px; uint32_t col, ol; float bo; } SubStyle;
static const SubStyle *sub_style(void) {
  static SubStyle st; if (st.ser == store_serial() && st.ser) return &st;
  st.ser = store_serial(); st.px = (int)(38 * SUB_SIZES[size_idx()] / 100.0f + 0.5f); st.col = hexcol(store_get("subsColor", "#ffffff")); st.ol = hexcol(store_get("subsOutlineColor", "#000000")); st.bo = (float)atof(SUB_BGS[bg_idx()]);
  return &st;
}
/* the cue list's index, made once per list: whether it runs in start order, and the latest end so far at each cue */
typedef struct { const Cue *c; int n; const char *t0, *t1; int sorted; unsigned gen, tg; double *maxE; int cap; } CueIx;
static const CueIx *cue_ix(void) {
  static CueIx ix; int n = P.ncues;
  if (ix.c == P.cues && ix.n == n && n && ix.t0 == P.cues[0].t && ix.t1 == P.cues[n - 1].t && ix.tg == cueTimesGen) return &ix;
  ix.c = P.cues; ix.n = n; ix.t0 = n ? P.cues[0].t : NULL; ix.t1 = n ? P.cues[n - 1].t : NULL; ix.gen++; ix.tg = cueTimesGen; ix.sorted = 0;
  if (n > ix.cap) { double *m = realloc(ix.maxE, (size_t)n * sizeof *m); if (!m) return &ix; ix.maxE = m; ix.cap = n; }
  ix.sorted = 1; for (int i = 0; i < n; i++) { if (i && P.cues[i].s < P.cues[i - 1].s) ix.sorted = 0; ix.maxE[i] = i && ix.maxE[i - 1] > P.cues[i].e ? ix.maxE[i - 1] : P.cues[i].e; }
  return &ix;
}
static void draw_mkv_line(const MkvLine *l, float bx, float by, int px, uint32_t col, uint32_t ol, float bo, int align) {
  float lh = px * 1.35f; char buf[1024]; snprintf(buf, sizeof buf, "%s", l->t); char *lp[12]; int n = 0; for (char *c = strtok(buf, "\n"); c && n < 12; c = strtok(NULL, "\n")) lp[n++] = c;
  int an = l->an ? l->an : 2; float y = an >= 7 ? by : an >= 4 ? by - n * lh / 2 : by - n * lh; /* numpad: 7-9 hang from the point, 1-3 stand on it */
  if (l->color) col = 0xff000000u | (l->color & 0xffffff); /* a sign keeps its own colour */
  for (int i = 0; i < n; i++) { float tw = text_width(lp[i], px, 600, 0), x0 = align == ALIGN_CENTER ? bx - tw / 2 : align == ALIGN_RIGHT ? bx - tw : bx;
    if (bo > 0) gfx_rrect(x0 - 16, y - 2, tw + 32, lh + 4, 8, BLACK_A((int)(bo * 255)));
    for (int d = 0; d < 8; d++) { static const float ox[8] = { -2, 2, 0, 0, -1, 1, -1, 1 }, oy[8] = { 0, 0, -2, 2, -1, 1, 1, -1 }; text_draw(lp[i], bx + ox[d], y + oy[d] + (lh - text_line_height(px)) / 2, px, 600, ol, 0, 0, align); }
    text_draw(lp[i], bx, y + (lh - text_line_height(px)) / 2, px, 600, col, 0, 0, align); y += lh; }
}
static void draw_subs(void) {
  float bottom = P.subB.v; /* the page's bottom: 8 %, above the controls (420), the seek bar (200) or an open tray (62 %), gliding 0.3 s */
  if (asssubs_on()) { asssubs_draw(); return; } /* styled (libass) */
  if (!strncmp(P.curSub, "emb:", 4) && SL.mkv && !SL.tv && mkv) { /* the file's own lines (the TV draws them while it holds the track) */
    MkvLine ls[8]; static char tb[4096]; int n = mkv_lines_at(mkv, atoi(P.curSub + 4), video_time() - P.subDelay, ls, 8, tb, sizeof tb); if (!n) return;
    if (!SL.shown) { SL.shown = 1; fprintf(stderr, "[subs] first line on screen %.1f s after the choice: \"%.40s\"\n", now_() - SL.at, ls[0].t); }
    const SubStyle *sst = sub_style(); int px = sst->px; uint32_t col = sst->col, ol = sst->ol; float bo = sst->bo;
    float lh = px * 1.35f, stackY = SB_H - bottom;
    for (int i = n - 1; i >= 0; i--) { const MkvLine *l = &ls[i]; int an = l->an ? l->an : 2, al = an % 3 == 1 ? ALIGN_LEFT : an % 3 == 0 ? ALIGN_RIGHT : ALIGN_CENTER;
      if (l->x >= 0 && l->y >= 0) draw_mkv_line(l, l->x / 100 * SB_W, l->y / 100 * SB_H, px, col, ol, bo, al); /* placed by the file */
      else if (an >= 7) draw_mkv_line(l, al == ALIGN_LEFT ? 80 : al == ALIGN_RIGHT ? SB_W - 80 : SB_W / 2, 70, px, col, ol, bo, al);
      else if (an >= 4) draw_mkv_line(l, al == ALIGN_LEFT ? 80 : al == ALIGN_RIGHT ? SB_W - 80 : SB_W / 2, SB_H / 2, px, col, ol, bo, al);
      else { int nl = 1; for (const char *c = l->t; *c; c++) if (*c == '\n') nl++; draw_mkv_line(l, al == ALIGN_LEFT ? 80 : al == ALIGN_RIGHT ? SB_W - 80 : SB_W / 2, stackY, px, col, ol, bo, al); stackY -= nl * lh; } } /* the dialogue stacks up from the bottom */
    return; }
  if (!P.ncues || !strncmp(P.curSub, "emb", 3)) return; double t = video_time() - P.subDelay; const Cue *show[2]; int ns = 0;
  const CueIx *ix = cue_ix();
  if (!ix->sorted) { for (int i = 0; i < P.ncues; i++) if (t >= P.cues[i].s && t <= P.cues[i].e) { if (ns < 2) show[ns++] = &P.cues[i]; else { show[0] = show[1]; show[1] = &P.cues[i]; } } }
  else { /* the last two cues showing at t: those starting by t (a binary search), back from there while an earlier one can still last until t */
    int lo = 0, hi = P.ncues; while (lo < hi) { int mid = (lo + hi) / 2; if (P.cues[mid].s <= t) lo = mid + 1; else hi = mid; }
    const Cue *f[2]; int nf = 0;
    for (int i = lo - 1; i >= 0 && nf < 2; i--) { if (ix->maxE[i] < t) break; if (t >= P.cues[i].s && t <= P.cues[i].e) f[nf++] = &P.cues[i]; }
    if (nf == 2) { show[0] = f[1]; show[1] = f[0]; ns = 2; } else if (nf == 1) { show[0] = f[0]; ns = 1; } }
  if (!ns) return;
  if (!SL.shown) { SL.shown = 1; fprintf(stderr, "[subs] first line on screen %.1f s after the choice: \"%.40s\"\n", now_() - SL.at, show[0]->t); }
  const SubStyle *st = sub_style(); int px = st->px; uint32_t col = st->col, ol = st->ol; float bo = st->bo;
  float lh = px * 1.35f;
  /* the shown cues' lines, split and measured once while they stay up */
  static struct { const Cue *a, *b; unsigned gen; int px, lines; char buf[4096]; char *lp[12]; float mw; } SC;
  const Cue *ca = show[0], *cb = ns > 1 ? show[1] : NULL;
  if (SC.a != ca || SC.b != cb || SC.gen != ix->gen || SC.px != px) { SC.a = ca; SC.b = cb; SC.gen = ix->gen; SC.px = px;
    char all[4096] = ""; for (int i = 0; i < ns; i++) { size_t l = strlen(all); snprintf(all + l, sizeof all - l, "%s%s", i ? "\n" : "", show[i]->t); }
    SC.lines = 0; snprintf(SC.buf, sizeof SC.buf, "%s", all); for (char *c = strtok(SC.buf, "\n"); c && SC.lines < 12; c = strtok(NULL, "\n")) SC.lp[SC.lines++] = c;
    SC.mw = 0; for (int i = 0; i < SC.lines; i++) SC.mw = fmaxf(SC.mw, text_width(SC.lp[i], px, 600, 0)); }
  int lines = SC.lines; char **lp = SC.lp;
  float y = SB_H - bottom - lines * lh;
  if (bo > 0) { float mw = SC.mw; gfx_rrect(SB_W / 2 - mw / 2 - 16, y - 2, mw + 32, lines * lh + 4, 8, BLACK_A((int)(bo * 255))); } /* one box around the lines, as the page's span */
  for (int i = 0; i < lines; i++) {
    for (int d = 0; d < 8; d++) { static const float ox[8] = { -2, 2, 0, 0, -1, 1, -1, 1 }, oy[8] = { 0, 0, -2, 2, -1, 1, 1, -1 }; text_draw(lp[i], SB_W / 2 + ox[d], y + oy[d] + (lh - text_line_height(px)) / 2, px, 600, ol, 0, 0, ALIGN_CENTER); }
    text_draw(lp[i], SB_W / 2, y + (lh - text_line_height(px)) / 2, px, 600, col, 0, 0, ALIGN_CENTER); y += lh; }
}
/* the CC button of a trailer: the subtitles icon, and when they are off a slash drawn from its top-left (0.26 s) with a gap cut around it */
static void cc_icon(float x, float y, float sz, uint32_t col, uint32_t gap) {
  float k = P.ccS.v, u = sz / 24; icon_draw("p_subs", x, y, sz, col_alpha(col, k > 0.5f && col == WHITE_A(255) ? 0.8f : 1));
  if (k <= 0.001f) return;
  float g = (2.6f + 17.68f * k) * u; gfx_clip_push(x, y, g, g); icon_draw("p_ccgap", x, y, sz, gap); gfx_clip_pop();
  float c = (3.4f + 17.68f * k + 1) * u; gfx_clip_push(x, y, c, c); icon_draw("p_ccslash", x, y, sz, col); gfx_clip_pop();
}
static void draw_osd(void) {
  { float t = (float)now_(); lglass_light(0.018f * sinf(t * 0.157f) + 0.012f * sinf(t * 0.263f + 1.3f) + 0.008f * sinf(t * 0.411f + 4.1f)); } /* the glass's light wandering, slowly and a little */
  float W = SB_W, H = SB_H, bx = 72, rowTop = H - 54 - 94, L25 = text_line_height(25), barTop = rowTop - 30 - L25, trkC = barTop + L25 / 2, ttlBot = barTop - 32;
  bake();
  /* the shades: the bottom for the controls, the top for the clock, the vignette when paused */
  if (P.dim.v > 0.001f) { gfx_opacity_push(P.dim.v); gfx_rrect_v(0, H * 0.44f, W, H * 0.56f * 0.54f, 0, BLACK_A(0), BLACK_A(140)); gfx_rrect_v(0, H * 0.44f + H * 0.56f * 0.54f, W, H * 0.56f * 0.46f, 0, BLACK_A(140), BLACK_A(224)); gfx_opacity_pop(); }
  if (P.dimT.v > 0.001f) { gfx_opacity_push(P.dimT.v); gfx_rrect_v(0, 0, W, 240, 0, BLACK_A(140), BLACK_A(0)); gfx_opacity_pop(); }
  if (P.vig.v > 0.001f) gfx_alpha_tex(vigTex, 0, 0, W, H, BLACK_A((int)(255 * P.vig.v)));
  draw_subs();
  /* buffering: a quarter of the ring turning */
  if (P.bufA.v > 0.001f) { gfx_opacity_push(P.bufA.v); float cx = W / 2, cy = H / 2 - 62 + 40; gfx_ring(cx - 40, cy - 40, 80, 80, 40, 5, WHITE_A(36));
    float ph = clock_ph(0.9) * 6.2832f; for (int i = 0; i <= 20; i++) { float an = ph - 2.356f + 1.5708f * i / 20; gfx_circle(cx + cosf(an) * 37.5f, cy + sinf(an) * 37.5f, 2.5f, ACC2); }
    xline_draw(&stX[1], status, cx, cy + 40 + 22, 24, WHITE_A(204), 0, ALIGN_CENTER); gfx_opacity_pop(); }
  /* top left, as the film starts, for 6 s (under the peers chip while the controls show it): one glass pill like the peers chip, the age in a small
     accent badge, then what for, each with three small dots for how much; the reasons come in one after the other */
  if (P.pgA.v > 0.001f) { float x = 72 + P.pgY.v, y = 52 + (46 + 14) * P.p2pA.v, h = 52, t = (float)(now_() - P.pg.at); char ag[40]; if (P.pg.age > 0) snprintf(ag, sizeof ag, "%d+", P.pg.age); else snprintf(ag, sizeof ag, "%s", T("All ages"));
    static float nw[PG_CATS]; static unsigned lg; if (lg != i18n_gen() || !nw[0]) { lg = i18n_gen(); for (int c = 0; c < PG_CATS; c++) nw[c] = text_width(parental_cat_name(c), 22, 600, 0); }
    float bw = text_width(ag, 25, 800, 0) + 22, /* (the age a step above the rest) */ w = 9 + bw + 16; for (int i = 0; i < P.pg.g.n; i++) w += nw[P.pg.g.cat[i]] + 9 + 2 * 10 + 7 + (i + 1 < P.pg.g.n ? 20 : 18); if (!P.pg.g.n) w = 9 + bw + 9; /* (the age alone) */
    gfx_opacity_push(P.pgA.v);
    lglass_shell(x, y, w, h, h / 2, RGBA(10, 12, 22, 150), 1);
    float bx = x + 7, by = y + 7; gfx_rrect(bx, by, bw, h - 14, (h - 14) / 2, acc_a(0.35f)); text_draw(ag, bx + bw / 2, by + (h - 14 - text_line_height(25)) / 2, 25, 800, WHITE_A(255), 0, 0, ALIGN_CENTER);
    float rx = bx + bw + 16, ry = y + (h - text_line_height(22)) / 2, cy = y + h / 2;
    for (int i = 0; i < P.pg.g.n; i++) { float k = (t - 0.25f - i * 0.12f) / 0.35f; k = k < 0 ? 0 : k > 1 ? 1 : k; k = 1 - (1 - k) * (1 - k); int c = P.pg.g.cat[i], lv = P.pg.g.lvl[i];
      if (k > 0.004f) { gfx_opacity_push(k); text_draw(parental_cat_name(c), rx, ry, 22, 600, WHITE_A(225), 0, 0, ALIGN_LEFT);
        for (int d = 0; d < 3; d++) gfx_circle(rx + nw[c] + 9 + 3.5f + d * 10, cy, 3.5f, d < lv ? (lv == PG_SEVERE ? RGB(0xf0, 0xb9, 0x4a) : WHITE_A(220)) : WHITE_A(55));
        gfx_opacity_pop(); }
      rx += nw[c] + 9 + 2 * 10 + 7 + 20; }
    gfx_opacity_pop(); }
  /* top left, with the controls: a torrent's peers and speed */
  if (P.p2pA.v > 0.001f) { gfx_opacity_push(P.p2pA.v); static TMemo p2W; float tw = tm_width(&p2W, P.p2pLine, 24, 600, 0), w = 16 + 10 + 12 + tw + 20 + 4;
    lglass_shell(72, 52, w, 46, 23, RGBA(10, 12, 22, 150), 1); gfx_circle(72 + 2 + 16 + 5, 52 + 23, 5, RGB(0x4a, 0xde, 0x80));
    xline_draw(&stX[2], P.p2pLine, 72 + 2 + 16 + 10 + 12, 52 + (46 - text_line_height(24)) / 2, 24, WHITE_A(255), 0, ALIGN_LEFT); gfx_opacity_pop(); }
  /* top right: the clock, the pages' own (a new minute: only its changed digits crossfade), and when it ends */
  if (P.topR.v > 0.001f) { gfx_opacity_push(P.topR.v); gfx_offset_push(0, P.topY.v);
    app_clock_draw(W - 64, 46, 1);
#ifdef SB_LINUX
    app_fsbtn_draw(W - 64, 46);
#endif
    double d = dur(); if (d > 0) { time_t t = time(NULL) + (time_t)(d - video_time());
      static char s[64]; static long sMin = -1; static unsigned sLg; /* made again when its minute or the language changes */
      if ((long)(t / 60) != sMin || sLg != i18n_gen()) { sMin = (long)(t / 60); sLg = i18n_gen(); struct tm tm; localtime_r(&t, &tm); char e[16]; snprintf(e, sizeof e, "%02d:%02d", tm.tm_hour, tm.tm_min); snprintf(s, sizeof s, "%s", T1("Ends at {t}", "t", e)); text_upper(s, sizeof s); }
      txt_sh(s, W - 64, 46 + text_line_height(38) / 2 + 38 + 14, 20, 600, WHITE_A(178), 1.6f, 0, ALIGN_RIGHT, 128); } /* a quiet line just under the clock's glass, in line with the time */
    gfx_offset_pop(); gfx_opacity_pop(); }
  /* bottom left: the title's art, S:E and its name, the info chips, year · genre · via addon (laid out from the bar up) */
  if (P.ttlA.v > 0.001f) { gfx_opacity_push(P.ttlA.v); gfx_offset_push(P.ttlX.v, P.ttlY.v);
    static char viaSrc[96], mt2[96]; static unsigned viaLg; /* "via addon": made when the addon or the language changes */
    if (viaLg != i18n_gen() || strcmp(viaSrc, P.saddon)) { viaLg = i18n_gen(); snprintf(viaSrc, sizeof viaSrc, "%s", P.saddon); char via[120]; snprintf(via, sizeof via, "%s", P.saddon); char *v = via; while (*v && !isalnum((unsigned char)*v)) v++; snprintf(mt2, sizeof mt2, "%s", *v ? T1("via {a}", "a", v) : ""); }
    const char *mt[3] = { P.isTrailer ? "" : P.year, P.isTrailer || !P.genre[0] ? "" : T(P.genre), P.isTrailer || !mt2[0] ? "" : mt2 };
    int hasMt = mt[0][0] || mt[1][0] || mt[2][0], hasSe = P.isTrailer || P.episode > 0; /* (an anime episode: "E5", no season) */ float L30 = text_line_height(30);
    float y = ttlBot, mtTop = 0, infTop = 0, seTop = 0;
    if (hasMt) { mtTop = y - L25; y = mtTop - 14; }
    if (P.nchip) { infTop = y - 46; y = infTop - 18; }
    if (hasSe) { seTop = y - 36; y = seTop - 18; }
    float lw, lh; GLuint t = logo_fit(560, 170, &lw, &lh); int nm = name_shown(t), nl = 0; static TMemo nmW; if (nm) { nl = tm_wrap(&nmW, P.name, 1100, 3, 60, 800, -1); lh = nl * 60 * 1.05f; }
    float lgTop = y - lh;
    if (P.pzA.v > 0.001f) { static UMemo pzU; const char *pz = um_upper(&pzU, T("Paused")); text_draw(pz, bx, lgTop - 40, 24, 700, WHITE_A((int)(199 * P.pzA.v)), 6, 0, ALIGN_LEFT); }

    if (t) { char sk[80]; snprintf(sk, sizeof sk, "logo:%s", P.id); sx_image(sk, t, bx, lgTop, lw, lh, 0, WHITE_A(255), 0, 0, 1, 1); } else if (nm) text_draw_wrapped(P.name, bx, lgTop, 1100, 3, 60, 800, WHITE_A((int)(255 * kf(P.nameAt, .45f, C_LGIN, 0))), -1, ALIGN_LEFT, 60 * 1.05f);
    if (hasSe) { float ty = seTop + (36 - L30) / 2;
      if (P.isTrailer) { static UMemo tgU; const char *tg = um_upper(&tgU, trailer_name()); text_draw(tg, bx, seTop + (36 - text_line_height(22)) / 2, 22, 700, ACC2, 6, 0, ALIGN_LEFT); }
      else { char se[16]; if (P.season >= 0) snprintf(se, sizeof se, "S%d:E%d", P.season, P.episode); else snprintf(se, sizeof se, "E%d", P.episode); float w = txt_sh(se, bx, ty, 30, 800, WHITE_A(255), 0.5f, 0, ALIGN_LEFT, 153);
        if (P.epTitle[0]) { gfx_circle(bx + w + 16 + 3.5f, seTop + 18, 3.5f, ACC2); txt_sh(P.epTitle, bx + w + 16 + 7 + 16, ty, 30, 700, WHITE_A(255), 0, 1000, ALIGN_LEFT, 153); } } }
    if (P.nchip) { float cx = bx, L24 = text_line_height(24);
      for (int i = 0; i < P.nchip; i++) { float k = kf(P.chip[i].at, .42f, C_OUT, P.chip[i].delay); if (P.chip[i].at && now_() - P.chip[i].at < P.chip[i].delay) k = 0; /* the entrance keeps its first frame through the delay */
        if (P.chip[i].sep) { cx += 4; gfx_rect(cx, infTop + 6 + 8 * (1 - k), 2, 34, WHITE_A((int)(115 * k))); cx += 2 + 16; continue; }
        static TMemo chW[8]; int b = P.chip[i].bold; float tw = tm_width(&chW[i], P.chip[i].t, 24, b ? 800 : 600, b ? 0.5f : 0), cy = infTop + 8 * (1 - k);
        float xk = P.chip[i].xAt ? clampf((float)((now_() - P.chip[i].xAt) / 0.26), 0, 1) : 1; xk = 1 - (1 - xk) * (1 - xk) * (1 - xk); if (xk >= 1) P.chip[i].xAt = 0; /* its words changing */
        if (xk < 1) tw = P.chip[i].oldW + (tw - P.chip[i].oldW) * xk;
        float w = 40 + tw + (P.chip[i].icon ? 36 : 0);
        if (k > 0.001f) { gfx_opacity_push(k); gfx_rrect(cx, cy, w, 46, 23, RGBA(10, 12, 22, 140)); gfx_ring(cx, cy, w, 46, 23, 2, WHITE_A(56)); float tx = cx + 20, ty = cy + (46 - L24) / 2;
          if (P.chip[i].icon) { icon_draw(P.chip[i].icon, tx, cy + 10, 26, WHITE_A(230)); tx += 36; }
          if (xk < 1) { gfx_clip_push(cx, cy, w, 46); text_draw(P.chip[i].old, tx, ty - 6 * xk, 24, b ? 800 : 600, WHITE_A((int)(255 * (1 - xk))), b ? 0.5f : 0, 0, ALIGN_LEFT); text_draw(P.chip[i].t, tx, ty + 6 * (1 - xk), 24, b ? 800 : 600, WHITE_A((int)(255 * xk)), b ? 0.5f : 0, 0, ALIGN_LEFT); gfx_clip_pop(); }
          else text_draw(P.chip[i].t, tx, ty, 24, b ? 800 : 600, WHITE_A(255), b ? 0.5f : 0, 0, ALIGN_LEFT); gfx_opacity_pop(); }
        cx += w + 12; } }
    if (hasMt) { float x = bx; int any = 0; static TMemo dotW; float bw = tm_width(&dotW, "•", 25, 400, 0);
      for (int i = 0; i < 3; i++) { if (!mt[i][0]) continue; if (any) { txt_sh("•", x + 14, mtTop, 25, 400, WHITE_A(128), 0, 0, ALIGN_LEFT, 128); x += 14 + bw + 14; } x += txt_sh(mt[i], x, mtTop, 25, 500, WHITE_A(191), 0, 0, ALIGN_LEFT, 128); any = 1; } }
    gfx_offset_pop(); gfx_opacity_pop(); }
  /* the bar: time, buffered, the intro / recap / credits marks, the progress, the knob, the pointer's time and the seek pill */
  if (P.barA.v > 0.001f) { gfx_opacity_push(P.barA.v); gfx_offset_push(0, P.barY.v); double d = dur(); double t = P.seekT >= 0 ? P.seekT : video_time(); float p = d > 0 ? (float)(t / d) : 0; if (p < 0) p = 0; if (p > 1) p = 1;
    char c1[16], c2[18]; fmt_time(t, c1, sizeof c1); char r[16]; fmt_time(d - t, r, sizeof r); snprintf(c2, sizeof c2, "-%s", r); int f = P.mode == 1 && P.row == 1, sk = P.skOn; float th = f || sk ? 10 : 6;
    float tx = bx + 132 + 20, tw = W - 2 * bx - 2 * (132 + 20), ty = trkC - th / 2;
    text_draw(c1, bx, barTop, 25, 600, WHITE_A(255), 0, 0, ALIGN_LEFT); text_draw(c2, W - bx, barTop, 25, 600, WHITE_A(184), 0, 0, ALIGN_RIGHT);
    gfx_rrect(tx, ty, tw, th, 3, WHITE_A(51)); float be = d > 0 ? (float)(video_buffered_end() / d) : 0; if (be > p) gfx_rrect(tx, ty, tw * (be > 1 ? 1 : be), th, 3, WHITE_A(66));
    if (P.skipsOk && d > 0) { struct { int has; double s, e; } segs[3] = { { P.intro.has, P.intro.s, P.intro.e }, { P.recap.has, P.recap.s, P.recap.e }, { P.outro.has, P.outro.s, P.outro.e } }; for (int i = 0; i < 3; i++) if (segs[i].has) { float a = (float)(segs[i].s / d), w = (float)((segs[i].e - segs[i].s) / d); if (w < 0.004f) w = 0.004f; gfx_rrect(tx + tw * a, ty, tw * w, th, 3, WHITE_A(128)); } }
    gfx_rrect_h(tx, ty, tw * p, th, 3, ACC, ACC2);
    float kx = tx + roundf(tw * p);
    if (P.knA.v > 0.001f) { gfx_opacity_push(P.knA.v); if (f || sk) { gfx_circle(kx, trkC, 22, acc_a(0.5f)); gfx_circle(kx, trkC, 15, WHITE_A(255)); } else gfx_circle(kx, trkC, 9, WHITE_A(255)); gfx_opacity_pop(); }
    if (P.hvA.v > 0.001f && d > 0) { char ht[16]; fmt_time(P.hvP * d, ht, sizeof ht); float bw = text_width(ht, 24, 700, 0) + 28, bh = 14 + text_line_height(24), bx2 = tx + roundf(tw * P.hvP) - bw / 2, by2 = ty + th - 26 - bh;
      gfx_opacity_push(P.hvA.v); gfx_rrect(bx2, by2, bw, bh, 12, RGBA(14, 12, 22, 219)); gfx_ring(bx2, by2, bw, bh, 12, 1, WHITE_A(36)); text_draw(ht, bx2 + 14, by2 + 7, 24, 700, WHITE_A(255), 0, 0, ALIGN_LEFT); gfx_opacity_pop(); }
    if (P.skA.v > 0.001f && P.seekTxt[0]) { /* the pill rises and grows from the knob; the number rolls the way it moved */
      float pw = roll_layout(P.seekTxt, P.seekDir) + 32, ph = 12 + L25, pb = ty + th - 34, s = .88f + .12f * P.skS.v;
      gfx_opacity_push(P.skA.v); gfx_scale_push(s, s, kx, pb); gfx_offset_push(0, 10 * (1 - P.skS.v));
      gfx_rrect(kx - pw / 2, pb - ph, pw, ph, 20, RGBA(18, 15, 26, 199)); gfx_ring(kx - pw / 2, pb - ph, pw, ph, 20, 1, WHITE_A(31));
      gfx_clip_push(kx - pw / 2, pb - ph, pw, ph); roll_draw(kx, pb - ph + 6, L25, WHITE_A(240)); gfx_clip_pop();
      gfx_offset_pop(); gfx_scale_pop(); gfx_opacity_pop(); }
    gfx_offset_pop(); gfx_opacity_pop(); }
  /* the buttons: one accent pill slides under the focused one and its name fades in */
  if (P.btnA.v > 0.001f) { gfx_opacity_push(P.btnA.v); gfx_offset_push(nudge_x(&P.nudge) * (P.mode == 1 && P.row == 0), P.btnY.v);
    int fi = P.mode == 1 && P.row == 0 ? P.bIdx : -1; if (P.mode == 2 && P.tray.kind) { int ak = P.tray.kind < 0 ? -P.tray.kind : P.tray.kind; fi = btn_index(ak == 1 || ak == 5 ? "eps" : ak == 2 ? "src" : ak == 3 ? "aud" : "subs"); }
    float fa = P.focA.v * fminf(1, kf(P.focFadeAt, .24f, C_EASE, 0));
    { static float sv, lx; lglass_stretch(&sv, &lx, P.focX.v, app_dt()); /* the lit button: accent glass that stretches as it glides and settles round */
      if (fa > 0.001f) { float w0 = P.focW.v, h0 = P.focH.v, sw = w0 * (1 + 0.16f * sv), sh = h0 * (1 - 0.08f * sv), px = P.focX.v + w0 / 2 - sw / 2, py = P.focY.v + h0 / 2 - sh / 2; lglass_accent(px, py, sw, sh, sh / 2, fa); } }
    if (P.tipA.v > 0.001f && P.tip[0]) { float ta = P.tipA.v * (P.tipAt && now_() - P.tipAt < .08 ? 0 : fminf(1, kf(P.tipAt, .3f, C_EASE, .08f)));
      int cyc = fi >= 0 && !strcmp(P.btns[fi].id, "asp") && !strcmp(P.tip, P.btns[fi].lb); /* a button that cycles: one dot per choice under its name, the lit one gliding on each press */
      if (cyc) { /* the name and its dots centred together in the pill's open part, whatever the name's length */
        float cx = P.focX.v + 70 + (P.focW.v - 70 - 24) / 2; int ai = !strcmp(P.aspect, "fill") ? 1 : !strcmp(P.aspect, "zoom") ? 2 : 0;
        text_draw(P.tip, cx, P.focY.v + 38 - text_line_height(24) / 2.0f - 8, 24, 700, col_alpha(ACC_FG, ta), 0, 0, ALIGN_CENTER);
        cycle_dots(0, 3, ai, cx, P.focY.v + 38 + text_line_height(24) / 2.0f + 2, 11, 2.5f, col_alpha(ACC_FG, .3f * ta), col_alpha(ACC_FG, ta), 0); }
      else text_draw(P.tip, P.focX.v + 70, P.focY.v + 38 - text_line_height(24) / 2.0f, 24, 700, col_alpha(ACC_FG, ta), 0, 0, ALIGN_LEFT); }
    for (int i = 0; i < P.nbtn; i++) { float sz = P.btns[i].w, x = P.btns[i].x, y = P.btns[i].y; int f = i == fi;
      if (!f) lglass_shell(x, y, sz, sz, sz / 2, RGBA(16, 16, 24, 90), 0.85f); /* glass at rest */
      float isz = P.btns[i].big ? 48 : 34, ix = x + sz / 2 - isz / 2, iy = y + sz / 2 - isz / 2; uint32_t ic = f ? ACC_FG : WHITE_A(255);
      if (!strcmp(P.btns[i].id, "cc")) { cc_icon(ix, iy, isz, ic, f ? ACC : RGBA(30, 30, 34, 235)); continue; }
      icon_draw(!strcmp(P.btns[i].id, "pp") ? (paused() ? "p_play" : "p_pause") : P.btns[i].icon, ix, iy, isz, ic); }
    gfx_offset_pop(); gfx_opacity_pop(); }
  /* the prompts: the skip pill, the credits buttons, the next-up card */
  PG.prompt = 0; const PPrompt *pr = P.prompt.k ? &P.prompt : &P.shown;
  if (P.prA.v > 0.001f && pr->k) { float a = P.prA.v; gfx_opacity_push(a); gfx_offset_push(nudge_x(&P.nudge) * (P.mode == 3), P.prY.v); float pb = P.prB.v, rm = 72 + 40 * fminf(1, fmaxf(0, (440 - pb) / 336)); /* off the edges: further in alone, in line with the buttons when they show */ PG.prompt = a > 0.5f && P.prompt.k; PG.card = 0; PG.npr = 0;
    if (pr->k == 3 && P.next.has) { float cw = 540, cx = W - rm - cw; float artH = 286, lh2 = text_line_height(21); static TMemo ovW; float ovn = P.next.ov[0] ? tm_wrap(&ovW, P.next.ov, cw - 44, 2, 24, 400, 0) : 0; float ch = 16 + artH + 18 + lh2 + 6 + text_line_height(30) + (ovn ? 6 + ovn * 33 : 0) + 18 + 36 + 18; float cy = H - pb - ch;
      PG.card = 1; PG.prX[0] = cx; PG.prY[0] = cy; PG.prW[0] = cw; PG.prH[0] = ch;
      lglass_shell(cx, cy, cw, ch, 28, RGBA(14, 12, 22, 220), 1);
      float ax = cx + 16, ay = cy + 16; const char *th = P.next.thumb[0] ? P.next.thumb : P.bg; GLuint t = th[0] ? img_get_shaped(th, (int)(cw - 32), (int)artH, 18, 1) : 0; ph_picture(t, ax, ay, cw - 32, artH, 18, WHITE_A(255));
      if (pr->autoN) { float k = fill_ease((now_() - pr->t0) / 10); gfx_clip_push(ax, ay + artH - 6, (cw - 32) * k, 6); gfx_rrect(ax, ay + artH - 24, cw - 32, 24, 18, ACC2); gfx_clip_pop(); }
      float y = ay + artH + 18; static UMemo upU; const char *up = um_upper(&upU, T("Next episode")); float w = text_draw(up, ax + 6, y, 21, 800, ACC2, 2.0f, 0, ALIGN_LEFT);
      if (pr->autoN) { int left = (int)(10 - (now_() - pr->t0) + 0.999); if (left < 0) left = 0; char n[8]; snprintf(n, sizeof n, "%d", left); char s[64]; snprintf(s, sizeof s, " · %s", T1("Starts in {n} s", "n", n)); text_draw(s, ax + 6 + w, y, 21, 600, WHITE_A(204), 0.4f, 0, ALIGN_LEFT); } y += lh2 + 6;
      { float sw = text_draw(P.next.se, ax + 6, y, 30, 800, WHITE_A(255), -0.3f, 0, ALIGN_LEFT); if (P.next.title[0]) { static TMemo mdW; float dw = tm_width(&mdW, "·", 30, 800, 0); text_draw("·", ax + 6 + sw + 12, y, 30, 800, WHITE_A(102), 0, 0, ALIGN_LEFT); text_draw(P.next.title, ax + 6 + sw + 12 + dw + 12, y, 30, 800, WHITE_A(255), -0.3f, cw - 44 - sw - 24 - dw, ALIGN_LEFT); } y += text_line_height(30); }
      if (ovn) { text_draw_wrapped(P.next.ov, ax + 6, y + 6 + (33 - text_line_height(24)) / 2, cw - 44, 2, 24, 400, WHITE_A(199), 0, ALIGN_LEFT, 33); y += 6 + ovn * 33; }
      y += 18; { float hx = ax + 6, cy2 = y + 18; hx += key_hint(hx, cy2, K_OK, T("Next episode"), WHITE_A(230), 0) + 28; key_hint(hx, cy2, K_BACK, T("Watch credits"), WHITE_A(230), 0); } } /* the remote's keys, as everywhere */

    else { float x = W - rm; int focus = P.mode == 3 || (P.mode == 0 && !pr->big); for (int i = pr->nbtn - 1; i >= 0; i--) { const char *ic = !strcmp(pr->b[i].id, "next") ? "p_next" : !strcmp(pr->b[i].id, "skip") ? "p_skip" : NULL; static TMemo prW[2]; float tw2 = tm_width(&prW[i & 1], pr->b[i].lb, 26, 700, 0), w = 2 + 64 + tw2 + (ic ? 42 : 0), h = 74; x -= w; float y = H - pb - h; int f = focus && i == pr->idx;
        int cnt = (pr->k == 1 || pr->k == 2) && pr->autoN; /* skip intro / recap counting down: the accent fills it from the left behind the words, then it skips */
        if (f && !cnt) lglass_accent(x, y, w, h, 37, 1); else lglass_shell(x, y, w, h, 37, RGBA(18, 16, 28, 200), 1); /* glass: lit in the accent when focused */
        if (cnt) { float k = fill_ease((now_() - pr->t0) / pr->adur); if (k > 0) { float fw = h + (w - h) * k; gfx_opacity_push(fminf(1, k * 6)); gfx_clip_push(x, y, fw, h); lglass_accent(x, y, w, h, 37, 1); gfx_clip_pop(); gfx_opacity_pop(); } } /* a pill growing out of its left end, eased */
        if (f) gfx_ring_out(x, y, w, h, 37, 4, acc_a(0.38f));
        if (pr->autoN && !strcmp(pr->b[i].id, "next")) { float k = fill_ease((now_() - pr->t0) / 10); if (k > 0) { gfx_opacity_push(fminf(1, k * 6)); gfx_rrect(x, y, h + (w - h) * k, h, 37, WHITE_A(71)); gfx_opacity_pop(); } }
        if (i < 2) { PG.prX[i] = x; PG.prY[i] = y; PG.prW[i] = w; PG.prH[i] = h; if (PG.npr < i + 1) PG.npr = i + 1; }
        float tx2 = x + 33; if (ic) { icon_draw(ic, tx2, y + 22, 30, f && !cnt ? ACC_FG : WHITE_A(255)); tx2 += 42; } text_draw(pr->b[i].lb, tx2, y + (h - text_line_height(26)) / 2, 26, 700, f && !cnt ? ACC_FG : WHITE_A(255), 0, 0, ALIGN_LEFT); x -= 16; } }
    gfx_offset_pop(); gfx_opacity_pop(); }
  dly_draw(); lglass_light(0);
}
/* a countdown's fill: eases in and out (sine), so it starts and lands softly */
static float fill_ease(double t) { if (t <= 0) return 0; if (t >= 1) return 1; return (float)(0.5 - 0.5 * cos(t * 3.14159265358979)); }
/* ---- the tray: sized by what it shows (never more than 60 % of the width), glass, over the buttons' side ---- */
static float item_w(TItem *it, int ak) {
  if (it->ww > 0) return it->ww; /* measured on the first frame it shows */
  if (it->k == 0) return it->ww = 20 + text_width(it->lb, 24, 400, 0) + (it->spin ? 34 : 0);
  if (it->k == 2) return 336; if (it->k == 3) return 400;
  float tw = text_width(it->lb, 27, 700, 0), sw = it->sub[0] ? text_width(it->sub, 23, 600, 0) : 0; /* (a chip's two lines: the wider one) */
  float w = 20 + (it->tool ? 0 : 36) + (tw > sw ? tw : sw) + (it->vl[0] ? 16 + text_width(it->vl, 22, 700, 0) : 0) + (it->sw ? 14 + 22 : 0) + 28; (void)ak;
  return it->ww = w < 240 ? 240 : w;
}
/* the "Playing" tag, upper-cased and measured once per language */
static const char *playing_tag(float *nw) { static char now[32]; static float w; static unsigned lg; if (lg != i18n_gen()) { lg = i18n_gen(); snprintf(now, sizeof now, "%s", T("Playing")); text_upper(now, sizeof now); w = 26 + text_width(now, 20, 800, 1); } *nw = w; return now; }
static float item_h(TItem *it) {
  if (it->k == 0) return 36 + text_line_height(24);
  if (it->k == 3) return 196;
  if (it->k == 2) { if (!it->hh) { int n = it->ov[0] ? text_wrap_count(it->ov, 300, 2, 22, 400, 0) : 0; it->hh = 189 + 14 + 30 + (it->sub[0] ? 4 + text_line_height(22) : 0) + (n ? 8 + n * 30 : 0) + 16; } return it->hh; }
  return 22 + text_line_height(27) + 2 + text_line_height(23) + 20; /* a chip: every one as tall as one with two lines */
}
static void draw_card(TItem *it, float x, float y, float w, float h, int f) {
  int on = it->on; uint32_t base = f ? ((it->k == 2 || (it->k == 3 && on) || it->k == 4) ? WHITE_A(18) : 0) : WHITE_A(11); /* rows: .045; focused: the plate shows through (.07 on some kinds) */
  if (it->k == 0) { float tx = x + 10; if (it->spin) { float cx = x + 10 + 11, cy = y + 18 + 13; gfx_ring(cx - 11, cy - 11, 22, 22, 11, 3, WHITE_A(51)); float ph = clock_ph(0.9) * 6.2832f; for (int i = 0; i <= 8; i++) { float an = ph - 2.356f + 1.5708f * i / 8; gfx_circle(cx + cosf(an) * 9.5f, cy + sinf(an) * 9.5f, 1.5f, ACC2); } tx += 34; } text_draw(it->lb, tx, y + 18, 24, 400, WHITE_A(184), 0, 0, ALIGN_LEFT); return; }
  if (base) gfx_rrect(x, y, w, h, 20, base); if (it->k == 3 && on && !f) gfx_ring(x, y, w, h, 20, 2, acc_a(0.7f));
  if (it->k == 2) { float th = 189; GLuint t = it->th[0] ? img_get_shaped(it->th, (int)w, (int)th + 20, 20, 1) : 0; if (!t && (!it->th[0] || img_shaped_failed(it->th, (int)w, (int)th + 20, 20, 1)) && P.bg[0]) t = img_get_shaped(P.bg, (int)w, (int)th + 20, 20, 1);
    gfx_clip_push(x, y, w, th); ph_picture(t, x, y, w, th + 20, 20, WHITE_A(255)); gfx_clip_pop(); /* rounded at the top only: the lower corners are cut square */
    if (on) { float nw; const char *now = playing_tag(&nw); gfx_rrect(x + 12, y + 12, nw, 8 + text_line_height(20), 11, ACC); text_draw(now, x + 12 + 13, y + 12 + 4, 20, 800, ACC_FG, 1, 0, ALIGN_LEFT); }
    float ty = y + th + 14; text_draw(it->lb, x + 18, ty + (30 - text_line_height(24)) / 2, 24, 700, WHITE_A(255), 0, w - 36, ALIGN_LEFT); ty += 30; if (it->sub[0]) { text_draw(it->sub, x + 18, ty + 4, 22, 600, WHITE_A(184), 0, w - 36, ALIGN_LEFT); ty += 4 + text_line_height(22); }
    if (it->ov[0]) text_draw_wrapped(it->ov, x + 18, ty + 8 + (30 - text_line_height(22)) / 2, w - 36, 2, 22, 400, WHITE_A(191), 0, ALIGN_LEFT, 30); return; }
  if (it->k == 3) { gfx_clip_push(x, y, w, h); int n = text_draw_wrapped(it->lb, x + 22, y + 20, w - 44, 2, 24, 700, WHITE_A(255), 0, ALIGN_LEFT, 24 * 1.25f); if (it->ov[0]) text_draw_wrapped(it->ov, x + 22, y + 20 + n * 24 * 1.25f + 8, w - 44, 3, 22, 400, WHITE_A(191), 0, ALIGN_LEFT, 30);
    if (on) { float nw; const char *now = playing_tag(&nw); gfx_rrect(x + w - 16 - nw, y + h - 14 - 8 - text_line_height(20), nw, 8 + text_line_height(20), 11, ACC); text_draw(now, x + w - 16 - nw + 13, y + h - 14 - 4 - text_line_height(20), 20, 800, ACC_FG, 1, 0, ALIGN_LEFT); } gfx_clip_pop(); return; }
  /* a chip (audio): tick, name, its language, value */
  float tx = x + 20; if (!it->tool) { if (on) icon_draw("check", tx + 1, y + h / 2 - 11, 22, col_alpha(ACC2, TM.rowChk.v)); tx += 24 + 12; }
  float right = x + w - 26; if (it->vl[0]) { if (it->vw <= 0) it->vw = text_width(it->vl, 22, 700, 0); float vw = it->vw; text_draw(it->vl, right, y + h / 2 - text_line_height(22) / 2.0f, 22, 700, ACC3, 0, 0, ALIGN_RIGHT); right -= vw + 16; }
  if (it->sw) { gfx_circle(right - 11, y + h / 2, 11, it->sw); gfx_ring(right - 22, y + h / 2 - 11, 22, 22, 11, 2, BLACK_A(77)); right -= 22 + 14; }
  float L1 = text_line_height(27), L2 = text_line_height(23);
  if (it->sub[0]) { float ty = y + (h - L1 - 2 - L2) / 2; text_draw(it->lb, tx, ty, 27, 700, WHITE_A(255), 0, right - tx - 4, ALIGN_LEFT); text_draw(it->sub, tx, ty + L1 + 2, 23, 600, WHITE_A(173), 0, right - tx - 4, ALIGN_LEFT); }
  else text_draw(it->lb, tx, y + h / 2 - L1 / 2, 27, 700, WHITE_A(255), 0, right - tx - 4, ALIGN_LEFT);
}
/* the sources: an addon's header (its name, how many, or the spinner while it is asked; plus / minus folds it), its rows
   as tall as their words need (up to 3 lines of name, 8 of words, then an ellipsis), the lit plate gliding under the
   focus; an addon whose header comes into view is asked then */
static void src_spin(float cx, float cy) { gfx_ring(cx - 11, cy - 11, 22, 22, 11, 3, WHITE_A(51)); float ph = clock_ph(0.9) * 6.2832f; for (int i = 0; i <= 8; i++) { float an = ph - 2.356f + 1.5708f * i / 8; gfx_circle(cx + cosf(an) * 9.5f, cy + sinf(an) * 9.5f, 1.5f, ACC2); } }
static void draw_srcs(float ix, float top, float W) {
  (void)W; float RW = SRC_RW, vx = ix + 30, vy = top + 8, vh = SRC_VH; int fi = !P.tray.onBtn && P.tray.kind > 0 && src_focusable(SR.idx) ? SR.idx : -1;
  float want = 0; if (src_focusable(SR.idx)) { want = SR.r[SR.idx].y - 120; float mx = SR.tot - 10 - vh; if (want > mx) want = mx; if (want < 0) want = 0; } mo_to(&SR.y, want, .28f, C_OUT, 0);
  PG.src = 1; PG.srcX = vx; PG.srcTop = vy;
  if (fi >= 0) { SRow *r = &SR.r[fi]; if (SR.plA.v <= 0.001f && !SR.plA.on) { mo_set(&SR.plY, r->y); mo_set(&SR.plH, r->h); } else { mo_to(&SR.plY, r->y, .26f, C_OUT, 0); mo_to(&SR.plH, r->h, .26f, C_OUT, 0); } mo_to(&SR.plA, 1, .18f, C_EASE, 0); }
  else mo_to(&SR.plA, 0, .18f, C_EASE, 0);
  gfx_clip_push(ix, vy, RW + 60, vh); gfx_offset_push(vx, vy - SR.y.v);
  if (SR.plA.v > 0.001f) { float py = SR.plY.v, ph = SR.plH.v; gfx_opacity_push(SR.plA.v); gfx_rrect(0, py, RW, ph, 20, WHITE_A(38)); gfx_clip_push(0, py, 4, ph); gfx_rrect(0, py, RW, ph, 20, ACC2); gfx_clip_pop(); gfx_opacity_pop(); }
  int ask[16], na = 0; float LN = text_line_height(SN_PX), LD = text_line_height(SD_PX);
  /* laid out as the lists open and fold now: a list takes the height it has reached, its rows slide and fade with it */
  float ly = 0, bTop = 0, bT0 = 0, al = 1, sl = 0; int clip = 0;
  for (int i = 0; i < SR.n; i++) { SRow *r = &SR.r[i]; float y, h = r->h; int f = i == fi;
    if (r->k == 5 || r->g < 0) { if (r->k == 5 && i) ly += 14; y = ly; ly += h + 10;
      if (r->k == 5) { __typeof__(P.srcs.g[0]) *g = &P.srcs.g[r->g]; bTop = ly; bT0 = r->y + h + 10; ly += g->bh.v; al = g->fa.v * g->ia.v; sl = -18 * (1 - g->fa.v); clip = g->bh.on || g->fa.on || g->leaving; } }
    else y = bTop + (r->y - bT0) + sl;
    if (y - SR.y.v > vh || y + h - SR.y.v < 0) continue;
    int body = r->k != 5 && r->g >= 0;
    if (body && al <= 0.004f) continue;
    if (body) { gfx_opacity_push(al); if (clip) gfx_clip_push(0, bTop, RW, P.srcs.g[r->g].bh.v); }
    #define SRC_NEXT { if (body) { if (clip) gfx_clip_pop(); gfx_opacity_pop(); } continue; }
    if (r->k == 5) { __typeof__(P.srcs.g[0]) *g = &P.srcs.g[r->g]; if (g->open && !g->st && na < 16) ask[na++] = r->g;
      char up[120]; snprintf(up, sizeof up, "%s", g->name); text_upper(up, sizeof up); float ty = y + (h - text_line_height(22)) / 2;
      text_draw(up, 22, ty, 22, 800, f ? WHITE_A(255) : WHITE_A(199), 1.4f, RW - 200, ALIGN_LEFT);
      icon_draw(g->open ? "minus" : "plus", RW - 22 - 24, y + (h - 24) / 2, 24, f ? WHITE_A(255) : WHITE_A(176));
      if (g->st == 1) src_spin(RW - 22 - 24 - 20 - 11, y + h / 2);
      else if (g->st == 2) { char n[12]; snprintf(n, sizeof n, "%d", g->n); text_draw(n, RW - 22 - 24 - 16, ty, 22, 700, WHITE_A(176), 0, 0, ALIGN_RIGHT); }
      continue; }
    if (r->k == 1) { gfx_rrect(0, y, RW, h, 20, WHITE_A(9)); ph_text(22, y + 18, (RW - 44) * (r->i ? 0.34f : 0.46f), 1, SN_PX, SN_LH, ALIGN_LEFT); ph_text(22, y + 18 + SN_LH + 8, (RW - 44) * (r->i ? 0.62f : 0.78f), 1, SD_PX, SD_LH, ALIGN_LEFT); SRC_NEXT }
    if (r->k == 0) { text_draw(T("No streams found."), 22, y + (h - text_line_height(25)) / 2, 25, 400, WHITE_A(191), 0, RW - 44, ALIGN_LEFT); SRC_NEXT }
    const Stream *st = &P.srcs.g[r->g].items[r->i]; int on = src_on(st);
    if (!f) gfx_rrect(0, y, RW, h, 20, WHITE_A(11)); if (on && !f) gfx_ring(0, y, RW, h, 20, 2, acc_a(0.7f));
    int nl = text_draw_wrapped(st->name[0] ? st->name : st->addon, 22, y + 18 + (SN_LH - LN) / 2, RW - 44 - 150, 3, SN_PX, 700, WHITE_A(255), 0, ALIGN_LEFT, SN_LH); if (nl < 1) nl = 1;
    if (st->title[0]) text_draw_wrapped(st->title, 22, y + 18 + nl * SN_LH + 8 + (SD_LH - LD) / 2, RW - 44, SD_MAX, SD_PX, 400, WHITE_A(199), 0, ALIGN_LEFT, SD_LH);
    if (on) { float nw; const char *now = playing_tag(&nw); gfx_rrect(RW - 18 - nw, y + 18, nw, 8 + text_line_height(20), 11, ACC); text_draw(now, RW - 18 - nw + 13, y + 18 + 4, 20, 800, ACC_FG, 1, 0, ALIGN_LEFT); }
    SRC_NEXT }
  #undef SRC_NEXT
  gfx_offset_pop(); gfx_clip_pop();
  /* more above or below: the list's edges fade */
  if (SR.y.v > 2) gfx_rrect_v(ix, vy, RW + 60, 36, 0, RGBA(18, 16, 28, 200), RGBA(18, 16, 28, 0));
  if (ly - 10 - SR.y.v > vh + 2) gfx_rrect_v(ix, vy + vh - 36, RW + 60, 36, 0, RGBA(18, 16, 28, 0), RGBA(18, 16, 28, 200));
  for (int j = 0; j < na; j++) src_ask(ask[j]); /* (after the drawing: an answer can come at once and lay the list out again) */
}
static float col_item_h(TItem *it) { if (it->tool && it->sub[0]) { if (it->hh <= 0) it->hh = 24 + 23 * 1.25f + 2 + 26 * text_wrap_count(it->sub, 6 + 232 - 18 - 24, 2, 21, 600, 0); return it->hh; }
  return it->k == 0 ? 24 + text_line_height(23) : it->k == 1 ? 58 : it->tool ? 30 + 23 * 1.25f : it->sub[0] ? 24 + 24 * 1.25f + 2 + text_line_height(21) : 30 + 24 * 1.25f; }
static void draw_tray(void) {
  int ak = P.tray.kind < 0 ? -P.tray.kind : P.tray.kind; PG.tray = 0; PG.ntab = 0; PG.nrow = 0; PG.src = 0; if (!ak || P.tray.openA.v <= 0.001f) return;
  float L24 = text_line_height(24), L30 = text_line_height(30), L18 = text_line_height(21); /* (the columns' headings) */
  /* the header: tabs (an episode's sources: its name above them), a title, or nothing */
  float hdH = 0, hdW = 0, tabW[40], tabsW = 0;
  static TMemo tabM[40], ttlM;
  if (P.tray.ntabs) { for (int i = 0; i < P.tray.ntabs; i++) { tabW[i] = 48 + tm_width(&tabM[i], P.tray.tabs[i], 24, 700, 0); tabsW += tabW[i] + 10; } hdW = tabsW; hdH = 20 + L24 + 18; if (ak == 5) { hdH += L24 + 14; hdW = fmaxf(hdW, tm_width(&ttlM, P.tray.title, 24, 800, 0)); } }
  else if (P.tray.title[0]) { hdH = L30 + 18; hdW = tm_width(&ttlM, P.tray.title, 30, 800, 0); }
  float xs[MAXROW], ws[MAXROW], hs[MAXROW], rowH = 0, rowW = 0;
  if (ak != 4 && ak != 2 && ak != 5) for (int i = 0; i < P.tray.n; i++) { ws[i] = item_w(&P.tray.row[i], ak); hs[i] = item_h(&P.tray.row[i]); xs[i] = rowW; rowW += ws[i] + (P.tray.row[i].k == 0 ? 0 : 18); /* notes have no margin */ if (hs[i] > rowH) rowH = hs[i]; }
  int srcs = ak == 2 || ak == 5;
  float colH = 6 + (6 + L18 + 14) + 294 + 4, listH = ak == 4 ? colH : srcs ? SRC_VH : rowH;
  float W = ak == 4 ? 56 + (P.tray.ncols ? P.tray.ncols : 4) * 282 - 10 /* the last column as far from the edge as the first */ : srcs ? SRC_W : fminf(SB_W * 0.6f, 2 + 60 + fmaxf(rowW, hdW)), H = 2 + 22 + hdH + 8 + listH + 8 + 20, x = SB_W - 72 - W, y;
  /* every tray takes the size of what is in it, and glides there when that changes (a group folded, a tab switched) */
  { static Mo tH, tW; static int tk; if (tk != ak || P.tray.openA.v < 0.02f) { mo_set(&tH, H); mo_set(&tW, W); tk = ak; } else { if (fabsf(tH.to - H) > 0.5f) mo_to(&tH, H, .26f, C_OUT, 0); if (fabsf(tW.to - W) > 0.5f) mo_to(&tW, W, .26f, C_OUT, 0); } static double lt; double nw = now_(); float fdt = lt ? (float)(nw - lt) : 0; lt = nw; if (fdt > 0.05f) fdt = 0.05f; mo_tick(&tH, fdt); mo_tick(&tW, fdt); H = tH.v; W = tW.v; }
  x = SB_W - 72 - W; y = SB_H - 178 - H;
  PG.tray = P.tray.kind > 0 && P.tray.openA.v > 0.5f; PG.x = x; PG.y = y; PG.w = W; PG.h = H;
  float sc = P.tray.open.v; gfx_opacity_push(P.tray.openA.v); gfx_scale_push(sc, sc, P.tray.ox, P.tray.oy);
  gfx_rrect(x, y, W, H, 28, RGBA(18, 16, 28, 204)); gfx_rrect_v(x, y, W, H, 28, WHITE_A(20), WHITE_A(5)); gfx_ring(x, y, W, H, 28, 1, WHITE_A(31));
  gfx_offset_push(nudge_x(&P.nudge) * (P.mode == 2), nudge_y(&P.nudge) * (P.mode == 2));
  float ix = x + 1, cy = y + 1 + 22;
  if (P.tray.ntabs) { if (ak == 5) { text_draw(P.tray.title, ix + 30, cy, 24, 800, WHITE_A(209), 0, W - 62, ALIGN_LEFT); cy += L24 + 14; }
    float vw = W - 2 - 60, xt[40], tx = 0; for (int i = 0; i < P.tray.ntabs; i++) { xt[i] = tx; tx += tabW[i] + 10; }
    float want = 0; if (P.tray.tab < P.tray.ntabs) { want = xt[P.tray.tab] + tabW[P.tray.tab] / 2 - vw / 2; if (want > tabsW - vw) want = tabsW - vw; if (want < 0) want = 0; } mo_to(&P.tray.tabsX, want, .25f, C_EASE, 0); /* the chosen tab stays in view */
    float bh = 20 + L24; gfx_clip_push(ix + 30, cy, vw, bh);
    /* one pill under the chosen tab, gliding from tab to tab; lit with the accent while the tabs have the focus */
    if (P.tray.tab < P.tray.ntabs) { int tf = P.tray.zone == 1 && !P.tray.onBtn && P.tray.kind > 0;
      if (!TM.tabSeen) { mo_set(&TM.tabX, xt[P.tray.tab]); mo_set(&TM.tabW, tabW[P.tray.tab]); mo_set(&TM.tabF, tf); TM.tabSeen = 1; }
      else { mo_to(&TM.tabX, xt[P.tray.tab], GLIDE_DUR, C_GLIDE, 0); mo_to(&TM.tabW, tabW[P.tray.tab], GLIDE_DUR, C_GLIDE, 0); mo_to(&TM.tabF, tf, .2f, C_EASE, 0); }
      float px = ix + 30 + TM.tabX.v - P.tray.tabsX.v, pw = TM.tabW.v, k = TM.tabF.v;
      if (k < 0.999f) gfx_rrect(px, cy, pw, bh, 24, WHITE_A((int)(26 * (1 - k))));
      if (k > 0.001f) { gfx_opacity_push(k); gfx_rrect(px, cy, pw, bh, 24, ACC); gfx_rrect_h(px, cy, pw, bh, 24, WHITE_A(0), WHITE_A(56)); gfx_ring_out(px, cy, pw, bh, 24, 4, acc_a(0.38f)); gfx_opacity_pop(); } }
    float pc = ix + 30 + TM.tabX.v - P.tray.tabsX.v + TM.tabW.v / 2;
    for (int i = 0; i < P.tray.ntabs; i++) { float bx = ix + 30 + xt[i] - P.tray.tabsX.v;
      PG.tabX[i] = bx; PG.tabW[i] = tabW[i]; PG.tabY = cy; PG.tabH = bh; PG.ntab = i + 1;
      float u = 1 - fabsf(pc - (bx + tabW[i] / 2)) / tabW[i]; if (u < 0) u = 0; /* the words under the pill take its colours as it passes */
      text_draw(P.tray.tabs[i], bx + tabW[i] / 2, cy + 10, 24, 700, col_mix(WHITE_A(191), col_mix(WHITE_A(255), ACC_FG, TM.tabF.v), u), 0, 0, ALIGN_CENTER); }
    gfx_clip_pop(); cy += bh + 18; }
  else if (P.tray.title[0]) { text_draw(P.tray.title, ix + 30, cy, 30, 800, WHITE_A(255), 0, W - 62, ALIGN_LEFT); cy += L30 + 18; }
  float lstTop = cy;
  /* changing tabs: what was shown slides out the other way and fades, the new one comes in from the side of the move */
  float swa = TM.swA.v, swx = TM.swPend ? -TM.swDir * 28 * (1 - swa) : TM.swDir * 28 * (1 - swa); int sw = swa < 0.999f;
  if (sw) { gfx_opacity_push(swa); gfx_offset_push(swx, 0); }
  if (ak == 4) { /* subtitles: columns side by side (the page's last one runs a little past the tray's edge) */
    gfx_clip_push(ix, lstTop, W - 2, listH + 16);
    /* the focused column's backdrop glides from column to column */
    { int on = P.tray.zone == 2 && P.tray.c < P.tray.ncols; float tx = ix + 22 + 5 + P.tray.c * 282;
      if (on && (!TM.colBSeen || TM.colBA.v <= 0.001f)) { mo_set(&TM.colBX, tx); TM.colBSeen = 1; } else if (on) mo_to(&TM.colBX, tx, GLIDE_DUR, C_GLIDE, 0);
      mo_to(&TM.colBA, on, .2f, C_EASE, 0);
      if (TM.colBA.v > 0.001f) gfx_rrect(TM.colBX.v, lstTop + 8, 272, colH, 22, WHITE_A((int)(10 * TM.colBA.v + .5f))); }
    for (int c = 0; c < P.tray.ncols; c++) { TCol *col = &P.tray.cols[c]; int cur = c == P.tray.c && P.tray.zone == 2; float colX = ix + 22 + 5 + c * 282, colTop = lstTop + 8;
      float near = 1 - fabsf(TM.colBX.v - colX) / 282; if (near < 0) near = 0; near *= TM.colBA.v;
      static UMemo colU[4][2]; const char *h2 = um_upper(&colU[c][0], col->title); text_draw(h2, colX + 18, colTop + 12, 21, 800, col_mix(WHITE_A(184), ACC2, near), 1.8f, 236, ALIGN_LEFT);
      if (col->vl[0]) { const char *v2 = um_upper(&colU[c][1], col->vl); float va = TM.vkC == c && !strcmp(TM.vkU, "dly") ? TM.vkA.v : 1; text_draw(v2, colX + 254, colTop + 12 + 6 * (1 - va), 21, 800, WHITE_A((int)(255 * va)), 0.5f, 0, ALIGN_RIGHT); }
      float clTop = colTop + 6 + 6 + L18 + 14, cinX = colX + 10 + 4, cinY = clTop + 2; int cn = col->n < 300 ? col->n : 300; float iy = 0, ys[300], hh[300];
      for (int i = 0; i < cn; i++) { ys[i] = iy; hh[i] = col_item_h(&col->items[i]); iy += hh[i] + (col->items[i].k == 0 ? 0 : 10); }
      int fi = P.tray.r[c]; if (fi >= cn) fi = -1; float want = 0; if (fi >= 0 && iy > 290) { want = ys[fi] - 145 + hh[fi] / 2; if (want > iy - 290) want = iy - 290; if (want < 0) want = 0; } mo_to(&P.tray.colY[c], -want, .25f, C_EASE, 0);
      PG.colX[c] = cinX + 6; PG.colW = 232; PG.colTop = clTop; PG.colBot = clTop + 294; PG.nit[c] = cn; for (int i = 0; i < cn; i++) { PG.itY[c][i] = cinY + P.tray.colY[c].v + ys[i]; PG.itH[c][i] = hh[i]; }
      gfx_clip_push(colX + 10, clTop, 252, 294); gfx_offset_push(cinX, cinY + P.tray.colY[c].v);
      int lfa = TM.colA[c].v < 0.999f; if (lfa) { gfx_opacity_push(TM.colA[c].v); gfx_offset_push(0, 10 * (1 - TM.colA[c].v)); } /* what it lists changed: it fades in */
      /* the column's plate glides to its focused item; moving across columns it fades out here and in there */
      int pon = cur && fi >= 0 && !P.tray.onBtn && col->items[fi].k != 0;
      if (pon) { if (P.tray.plA[c].v <= 0.001f && !P.tray.plA[c].on) mo_set(&P.tray.plY[c], ys[fi]); else mo_to(&P.tray.plY[c], ys[fi], .24f, C_OUT, 0); mo_to(&P.tray.plA[c], 1, .2f, C_EASE, 0); } else mo_to(&P.tray.plA[c], 0, .2f, C_EASE, 0);
      if (P.tray.plA[c].v > 0.001f && fi >= 0) { float ph = hh[fi]; gfx_opacity_push(P.tray.plA[c].v); gfx_rrect(6, P.tray.plY[c].v, 232, ph, 20, WHITE_A(41)); gfx_clip_push(6, P.tray.plY[c].v, 4, ph); gfx_rrect(6, P.tray.plY[c].v, 232, ph, 20, ACC2); gfx_clip_pop(); gfx_opacity_pop(); }
      for (int i = 0; i < cn; i++) { if (ys[i] + P.tray.colY[c].v > 294 || ys[i] + hh[i] + P.tray.colY[c].v < 0) continue; /* only what shows */ TItem *it = &col->items[i]; int f = cur && i == fi && !P.tray.onBtn; float h = hh[i], yy = ys[i];
        if (it->k == 0) { text_draw(it->lb, 14, yy + 12, 23, 400, WHITE_A(184), 0, 224, ALIGN_LEFT); continue; }
        if (it->k == 1) { ph_box(6, yy, 232, 58, 20); continue; } /* still being found: the waiting look */
        if (!f) gfx_rrect(6, yy, 232, h, 20, WHITE_A(18));
        float tx = 6 + (it->tool ? 18 : 14); if (!it->tool) { if (it->spin) { float cx = tx + 12, cy = yy + h / 2; gfx_ring(cx - 11, cy - 11, 22, 22, 11, 3, WHITE_A(51)); float ph = clock_ph(0.9) * 6.2832f; for (int q = 0; q <= 8; q++) { float an = ph - 2.356f + 1.5708f * q / 8; gfx_circle(cx + cosf(an) * 9.5f, cy + sinf(an) * 9.5f, 1.5f, ACC2); } } /* chosen, its lines still loading: the notes' spinner in place of the tick */
          else if (item_on(it)) icon_draw("check", tx + 1, yy + h / 2 - 11, 22, col_alpha(ACC2, i == TM.onIdx[c] ? TM.chkA[c].v : 1)); tx += 24 + 12; }
        /* a value that cycles: its dots under it, the lit one gliding along; a value just changed fades up */
        int dk = !it->tool ? 0 : !strcmp(it->u, "size") ? 1 : !strcmp(it->u, "color") ? 2 : !strcmp(it->u, "bg") ? 3 : 0; float va = TM.vkC == c && it->tool && !strcmp(TM.vkU, it->u) ? TM.vkA.v : 1, vy0 = dk ? -5 : 0;
        float right = 6 + 232 - 18; if (dk >= 2) { swatch_draw(dk - 2, dk == 2 ? (color_idx() < 0 ? 0 : color_idx()) : bg_idx(), right - 13, yy + h / 2, 13); right -= 26 + 10; }
        if (it->vl[0]) { if (it->vw <= 0) it->vw = text_width(it->vl, 22, 700, 0); float vw = it->vw; text_draw(it->vl, right, yy + h / 2 - text_line_height(22) / 2.0f + vy0 + 6 * (1 - va), 22, 700, col_alpha(ACC3, va), 0, 0, ALIGN_RIGHT);
          if (dk == 1) cycle_dots(dk, 7, size_idx(), right, yy + h / 2 + 14, 9, 2.5f, WHITE_A(64), ACC3, 1);
          right -= vw + 10; } if (it->sw) { gfx_circle(right - 9, yy + h / 2, 9, it->sw); gfx_ring(right - 18, yy + h / 2 - 9, 18, 18, 9, 2, BLACK_A(77)); right -= 18 + 10; }
        int px = it->tool ? 23 : 24;
        if (it->sub[0] && it->tool) { float ly = yy + 12; text_draw(it->lb, tx, ly, px, 700, WHITE_A(255), 0, right - tx - 4, ALIGN_LEFT); text_draw_wrapped(it->sub, tx, ly + px * 1.25f + 2, 6 + 232 - 18 - tx, 2, 21, 600, WHITE_A(184), 0, ALIGN_LEFT, 26); } /* Auto-sync: what it did, on up to two lines */
        else if (it->sub[0]) { float ly = yy + 12; text_draw(it->lb, tx, ly, px, 700, WHITE_A(255), 0, right - tx - 4, ALIGN_LEFT); text_draw(it->sub, tx, ly + px * 1.25f + 2, 21, 600, WHITE_A(184), 0, 6 + 232 - 18 - tx, ALIGN_LEFT); }
 /* an addon's subtitle: its addon under it */
        else text_draw(it->lb, tx, yy + h / 2 - text_line_height(px) / 2.0f, px, 700, WHITE_A(255), 0, right - tx - 4, ALIGN_LEFT); }
      gfx_offset_pop(); gfx_clip_pop(); if (lfa) { gfx_offset_pop(); gfx_opacity_pop(); } }
    gfx_clip_pop(); }
  else if (srcs) draw_srcs(ix, lstTop, W);
  else { /* a row: the focused card second from the left, a lit plate (and an accent line) behind it */
    gfx_clip_push(ix, lstTop, W - 2, listH + 16);
    float want = P.tray.idx > 0 && P.tray.idx < P.tray.n ? 30 + xs[P.tray.idx - 1] - 8 : 0, mx = 60 + rowW - (W - 2); if (want > mx) want = mx; if (want < 0) want = 0; mo_to(&P.tray.rowX, want, .3f, C_OUT, 0);
    float ox = ix - P.tray.rowX.v, oy = lstTop + 8;
    PG.nrow = P.tray.n; PG.rowY = oy; PG.rowH = rowH; for (int i = 0; i < P.tray.n; i++) { PG.rowX[i] = ox + 30 + xs[i]; PG.rowW[i] = ws[i]; }
    int fi = P.tray.zone == 0 && !P.tray.onBtn ? P.tray.idx : -1;
    if (fi >= 0 && fi < P.tray.n && P.tray.row[fi].k != 0) { if (!P.tray.ringW) { mo_set(&P.tray.ringX, 0); mo_set(&P.tray.ringY, 0); } mo_to(&P.tray.ringX, 30 + xs[fi], .2f, C_OUT, 0); P.tray.ringW = ws[fi]; P.tray.ringH = hs[fi]; mo_to(&P.tray.ringA, 1, .15f, C_EASE, 0); }
    else mo_to(&P.tray.ringA, 0, .15f, C_EASE, 0);
    gfx_offset_push(ox, oy);
    if (P.tray.ringA.v > 0.001f && P.tray.ringW) { float rx = P.tray.ringX.v, rw = P.tray.ringW, rh = P.tray.ringH; gfx_opacity_push(P.tray.ringA.v); gfx_rrect(rx, 0, rw, rh, 20, WHITE_A(33)); gfx_rrect(rx + rw * 0.22f, rh + 5, rw * 0.56f, 4, 2, ACC2); gfx_opacity_pop(); }
    int rf = TM.rowA.v < 0.999f; if (rf) { gfx_opacity_push(TM.rowA.v); gfx_offset_push(0, 10 * (1 - TM.rowA.v)); } /* what it lists changed: it fades in */
    for (int i = 0; i < P.tray.n; i++) { if (30 + xs[i] - P.tray.rowX.v > W || 30 + xs[i] + ws[i] - P.tray.rowX.v < 0) continue; draw_card(&P.tray.row[i], 30 + xs[i], 0, ws[i], hs[i], i == fi); }
    if (rf) { gfx_offset_pop(); gfx_opacity_pop(); }
    gfx_offset_pop(); gfx_clip_pop(); }
  if (sw) { gfx_offset_pop(); gfx_opacity_pop(); }
  gfx_offset_pop(); gfx_scale_pop(); gfx_opacity_pop();
}
void player_draw(void) {
  if (!P.open) return; float a = P.in.v; gfx_opacity_push(a);
  if (P.live && P.artA.v < 0.999f) { draw_osd(); draw_tray(); }
  else if (P.artA.v < 0.999f) hand_draw();
  draw_art();
  draw_vol();
  gfx_opacity_pop();
}
static void chapters_hook(const char *url, int p2p) { (void)url; (void)p2p; P.chapWant = 1; }
void player_init(void) { video_init(SB_APP_ID); app_play_hook = player_play; player_chapters_hook = chapters_hook; if (getenv("SB_PLAY_ALT")) player_fallback(getenv("SB_PLAY_ALT")); /* development: the fallback of the SB_PLAY stream */
  if (getenv("SB_PLAY_TRSUB")) { TrailerSub t; memset(&t, 0, sizeof t); snprintf(t.url, sizeof t.url, "%s", getenv("SB_PLAY_TRSUB")); snprintf(t.lang, sizeof t.lang, "eng"); snprintf(t.name, sizeof t.name, "English"); snprintf(t.kind, sizeof t.kind, "srt"); player_trailer_subs(&t, 1); } /* development: a caption track for an SB_PLAY trailer */ }

/* ---- the pointer in the player: moving brings the controls; pointing at a button, the bar, a prompt or a tray
   card highlights it; a click acts there as OK does: on the bar it seeks to that spot on release (a bubble shows the
   time under the pointer), on the picture it plays / pauses, beside an open tray it puts the tray away (on another
   button: that one acts) ---- */
static int in_box(float x, float y, float bx, float by, float bw, float bh) { return x >= bx && x <= bx + bw && y >= by && y <= by + bh; }
static void ptr_hv(int on, float x) {
  double d = dur(); on = on && d > 0; if (on) { float tx = 72 + 132 + 20, tw = SB_W - 2 * 72 - 2 * (132 + 20), p = (x - tx) / tw; P.hvP = p < 0 ? 0 : p > 1 ? 1 : p; }
  P.hvOn = on;
}
static void ptr_tray(float x, float y, int down) {
  for (int i = 0; i < PG.ntab && i < P.tray.ntabs; i++) if (in_box(x, y, PG.tabX[i], PG.tabY, PG.tabW[i], PG.tabH) && x >= PG.x && x <= PG.x + PG.w) {
    if (down && i != P.tray.tab) { tray_tab(i); P.tray.zone = 1; }
    return; }
  if (PG.src && src_tray()) { if (y < PG.srcTop || y > PG.srcTop + SRC_VH || x < PG.srcX || x > PG.srcX + SRC_RW) return; float ly = y - PG.srcTop + SR.y.v;
    for (int i = 0; i < SR.n; i++) if (ly >= SR.r[i].y && ly <= SR.r[i].y + SR.r[i].h) { if (!src_focusable(i)) return; if (SR.idx != i || P.tray.onBtn) { SR.idx = i; P.tray.onBtn = 0; } if (down) src_choose(i); return; }
    return; }
  if (P.tray.ncols) {
    for (int c = 0; c < P.tray.ncols; c++) if (x >= PG.colX[c] && x <= PG.colX[c] + PG.colW) {
      if (y < PG.colTop || y > PG.colBot) return;
      for (int i = 0; i < PG.nit[c] && i < P.tray.cols[c].n; i++) if (y >= PG.itY[c][i] && y <= PG.itY[c][i] + PG.itH[c][i]) {
        TItem *it = &P.tray.cols[c].items[i]; if (!focusable(it)) return;
        if (P.tray.c != c || P.tray.r[c] != i || P.tray.onBtn) { P.tray.c = c; P.tray.r[c] = i; P.tray.onBtn = 0; P.tray.zone = 2; }
        if (down) choose(it); return; }
      return; }
    return; }
  if (y < PG.rowY || y > PG.rowY + PG.rowH || x < PG.x || x > PG.x + PG.w) return;
  for (int i = 0; i < PG.nrow && i < P.tray.n; i++) if (x >= PG.rowX[i] && x <= PG.rowX[i] + PG.rowW[i]) {
    TItem *it = &P.tray.row[i]; if (!focusable(it)) return;
    if (P.tray.idx != i || P.tray.zone != 0 || P.tray.onBtn) { P.tray.idx = i; P.tray.zone = 0; P.tray.onBtn = 0; }
    if (down) choose(it); return; }
}
/* the volume, set with the mouse wheel while a film plays (persisted across starts) */
static void vol_ensure(void) { if (playVol < 0) { playVol = atoi(store_get("playVol", "100")); if (playVol < 0) playVol = 0; if (playVol > 100) playVol = 100; } }
void player_wheel(float dy) {
  if (!P.open || P.closing || !P.live) return;
  vol_ensure();
  int v = playVol + (dy > 0 ? 5 : -5); if (v < 0) v = 0; if (v > 100) v = 100;
  if (v == playVol) return; playVol = v;
  char b[16]; snprintf(b, sizeof b, "%d", v); store_set("playVol", b);
  video_set_volume(v, 0); P.volAt = now_(); mo_to(&P.volA, 1, .15f, C_EASE, 0);
}
static void draw_vol(void) { /* the volume pill, bottom centre over everything, only while changing */
  float a = P.volA.v; if (a <= 0.001f) return;
  float w = 440, h = 84, x = SB_W / 2 - w / 2, y = SB_H - 320;
  gfx_opacity_push(a);
  lglass_shell(x, y, w, h, 42, RGBA(10, 12, 22, 150), 1); gfx_ring(x, y, w, h, 42, 1, WHITE_A(60));
  icon_draw("p_spk", x + 26, y + 22, 40, WHITE_A(230));
  float bx = x + 82, bw = 196, bh = 8, by = y + h / 2 - 4;
  gfx_rrect(bx, by, bw, bh, 4, WHITE_A(40));
  if (playVol > 0) gfx_rrect(bx, by, bw * playVol / 100.0f, bh, 4, ACC2);
  char t[16]; snprintf(t, sizeof t, "%d", playVol < 0 ? 100 : playVol);
  text_draw(t, bx + bw + 16, y + (h - text_line_height(28)) / 2, 28, 700, WHITE_A(255), 0, 0, ALIGN_LEFT);
  gfx_opacity_pop();
}
/* the focused button opens into a pill with its name (foc_update): clicks land on the
   drawn pill, not just the 78px icon square, or the label half pauses instead of acting */
static int btn_foc(void) { int fi = P.bIdx; if (P.mode == 2 && P.tray.kind) { int ak = P.tray.kind < 0 ? -P.tray.kind : P.tray.kind; int b = btn_index(ak == 1 || ak == 5 ? "eps" : ak == 2 ? "src" : ak == 3 ? "aud" : "subs"); if (b >= 0) fi = b; } return fi; }
static int btn_hit(int i, float x) {
  if (x >= P.btns[i].x && x <= P.btns[i].x + P.btns[i].w) return 1;
  if (i == btn_foc() && P.focA.v > 0.02f && x >= P.focX.v && x <= P.focX.v + P.focW.v) return 1;
  return 0;
}
void player_ptr(int type, float x, float y) {  if (!P.open || P.closing || !P.live) return; /* the art is up (resolving): nothing to point at */
  float L25 = text_line_height(25), barTop = SB_H - 54 - 94 - 30 - L25; /* the bar's row, its times included (the track alone is a few pixels high) */
  int onBar = y >= barTop && y <= barTop + L25, onBtns = y >= SB_H - 54 - 94 && y <= SB_H - 54;
  if (type == 2) { if (P.barDown && onBar) { ptr_hv(1, x); double d = dur(); if (d > 0) { P.seekT = P.hvP * d; commit_seek(); } } P.barDown = 0; return; } /* the bar seeks on release, where the pointer is then */
  int down = type == 1;
  if (P.mode == 4) { /* the delay card: its buttons; elsewhere the picture stays as it is */
    if (DG.h > 0 && y >= DG.y && y <= DG.y + DG.h) for (int i = 0; i < 4; i++) if (x >= DG.x[i] && x <= DG.x[i] + DG.w[i]) { if (P.dlyF != i) { P.dlyF = i; mo_to(&P.dlyR, (float)i, .18f, C_OUT, 0); } if (down) dly_act(i); return; }
    return; }
  if (P.mode == 2) {
    if (PG.tray && in_box(x, y, PG.x, PG.y, PG.w, PG.h)) { ptr_tray(x, y, down); return; }
    if (down) { int ak = P.tray.kind < 0 ? -P.tray.kind : P.tray.kind, ob = btn_index(ak == 1 || ak == 5 ? "eps" : ak == 2 ? "src" : ak == 3 ? "aud" : "subs"); close_tray();
      if (onBtns) for (int i = 0; i < P.nbtn; i++) if (i != ob && btn_hit(i, x)) { P.row = 0; P.bIdx = i; act(P.btns[i].id); break; } }
    return; }
  if (P.prompt.k && PG.prompt) { /* skip intro / recap, the next-up card (all of it) */
    int pi = -1; if (PG.card) { if (in_box(x, y, PG.prX[0], PG.prY[0], PG.prW[0], PG.prH[0])) pi = P.prompt.nbtn - 1; }
    else for (int i = 0; i < PG.npr && i < P.prompt.nbtn; i++) if (in_box(x, y, PG.prX[i], PG.prY[i], PG.prW[i], PG.prH[i])) pi = i;
    if (pi >= 0) { if (P.mode != 3 || P.prompt.idx != pi) { if (P.mode == 0) show_osd(1); P.mode = 3; P.prompt.idx = pi; } ptr_hv(0, x); if (down) prompt_go(pi); return; } }
  if (P.mode == 0 || P.mode == 3) { if (P.mode == 3) P.mode = 0; show_osd(1); } else arm_hide(); /* moving brings the controls */
#ifdef SB_LINUX
  if (down && P.topR.v > 0.5f && app_fsbtn_hit(SB_W - 64, 46 + P.topY.v, x, y)) { app_toggle_fullscreen(); return; } /* the fullscreen button by the clock */
#endif
  if (onBtns) for (int i = 0; i < P.nbtn; i++) if (btn_hit(i, x)) { if (P.row != 0 || P.bIdx != i) { P.row = 0; P.bIdx = i; } ptr_hv(0, x); if (down) act(P.btns[i].id); return; }
  if (onBar) { if (P.row != 1) P.row = 1; ptr_hv(1, x); if (down) P.barDown = 1; return; } /* the bar, its times included */
  ptr_hv(0, x);
  if (down) { if (P.seekT >= 0) commit_seek(); else toggle(); show_osd(1); } /* the picture: play / pause */
}

/* Stats for nerds: what plays and how, a few short lines (n of them filled) */
int player_nerd_lines(char (*out)[120], int max) {
  if (!P.open || max < 1) return 0; int n = 0; double t = video_time(), d = dur(), buf = video_buffered_end() - t;
  const char *st = P.closing ? "closing" : !P.started ? "starting" : paused() ? "paused" : video_state() == VID_PLAYING ? "playing" : "waiting";
  snprintf(out[n++], 120, "player  %s  %d:%02d / %d:%02d", st, (int)t / 60, (int)t % 60, (int)d / 60, (int)d % 60);
  if (n < max) { char hdr[32] = "", fp[24] = ""; video_hdr(hdr, sizeof hdr); double fps = video_fps(); if (fps > 0) snprintf(fp, sizeof fp, " @ %.3g fps", fps);
    snprintf(out[n++], 120, "video  %dx%d%s%s%s", video_width(), video_height(), fp, hdr[0] && strcmp(hdr, "none") ? " · " : "", hdr[0] && strcmp(hdr, "none") ? hdr : ""); }
  if (n < max) { VidTrack a[16]; int na = video_audio_tracks(a, 16); const VidTrack *c = P.curAud >= 0 && P.curAud < na ? &a[P.curAud] : NULL; char ch[16] = "";
    if (c && c->channels > 0) snprintf(ch, sizeof ch, " · %d ch", c->channels);
    snprintf(out[n++], 120, "audio  %s%s%s", c && c->codec[0] ? c->codec : "not reported yet", ch, P.dts.conv ? " · converted to AC3 here" : ""); }
  if (n < max) snprintf(out[n++], 120, "buffer  %.0f s ahead  ·  %s", buf > 0 ? buf : 0, P.isP2p ? "torrent" : P.isHls ? "HLS" : "direct");
  if (n < max && P.isP2p) { char pl[96]; if (p2p_peer_line(pl, sizeof pl)) snprintf(out[n++], 120, "p2p  %s", pl); }
  if (n < max) { double am = asssubs_render_ms(); if (asssubs_on()) snprintf(out[n++], 120, "subtitles  styled, %.0f ms a render", am); else if (P.curSub[0]) snprintf(out[n++], 120, "subtitles  plain"); }
  return n;
}

static void dly_draw(void) {
  float a = P.dlyA.v; if (a <= 0.004f) { DG.h = 0; return; }
  /* [ - ]  +0.25 s  [ + ]  |  [ play ]  [ Done ]: one row, the value large between its two keys */
  const float B = 76, G = 18, VW = 230, h = 120; static TMemo dW; const char *dn = T("Done"); float dw = tm_width(&dW, dn, 26, 700, 0) + 64;
  float w = 28 + B + G + VW + G + B + 28 + 2 + 28 + B + G + dw + 28, x = SB_W / 2 - w / 2, y = 44 - 24 * (1 - a);
  gfx_opacity_push(a); gfx_offset_push(nudge_x(&P.nudge), 0);
  gfx_shadow(x, y + 14, w, h, 30, 40, BLACK_A(110)); gfx_rrect(x, y, w, h, 30, RGBA(18, 16, 28, 222)); gfx_rrect_v(x, y, w, h, 30, WHITE_A(18), WHITE_A(4)); gfx_ring(x, y, w, h, 30, 1, WHITE_A(36));
  float cy = y + h / 2, bx[4], bw[4]; bx[0] = x + 28; bw[0] = B; float vx = bx[0] + B + G; bx[1] = vx + VW + G; bw[1] = B;
  float sep = bx[1] + B + 28; bx[2] = sep + 2 + 28; bw[2] = B; bx[3] = bx[2] + B + G; bw[3] = dw;
  gfx_rect(sep, cy - 30, 1.5f, 60, WHITE_A(40)); /* the timing apart from the playing */
  for (int i = 0; i < 4; i++) { DG.x[i] = bx[i]; DG.w[i] = bw[i]; } DG.y = cy - B / 2; DG.h = B;
  /* the focus: one accent shape gliding between the buttons (a circle, stretching into the Done pill) */
  { float k = P.dlyR.v; int i0 = (int)floorf(k); if (i0 < 0) i0 = 0; if (i0 > 2) i0 = 2; float f = k - i0; if (f < 0) f = 0; if (f > 1) f = 1;
    float fx = bx[i0] + (bx[i0 + 1] - bx[i0]) * f, fw = bw[i0] + (bw[i0 + 1] - bw[i0]) * f; if (k >= 3) { fx = bx[3]; fw = bw[3]; }
    gfx_rrect(fx, cy - B / 2, fw, B, B / 2, ACC); gfx_ring_out(fx, cy - B / 2, fw, B, B / 2, 4, acc_a(0.35f)); }
  for (int i = 0; i < 4; i++) { int f = (int)floorf(P.dlyR.v + 0.5f) == i; uint32_t c = f ? ACC_FG : WHITE_A(240);
    if (!f) gfx_rrect(bx[i], cy - B / 2, bw[i], B, B / 2, WHITE_A(22));
    if (i < 2) { float s = 26, t = 4; gfx_rrect(bx[i] + B / 2 - s / 2, cy - t / 2, s, t, 2, c); if (i == 1) gfx_rrect(bx[i] + B / 2 - t / 2, cy - s / 2, t, s, 2, c); } /* - and + drawn: even strokes, centred */
    else if (i == 2) icon_draw(paused() ? "p_play" : "p_pause", bx[i] + B / 2 - 17, cy - 17, 34, c);
    else text_draw(dn, bx[i] + bw[i] / 2, cy - text_line_height(26) / 2.0f, 26, 700, c, 0, 0, ALIGN_CENTER); }
  /* the delay: large, a small lift when it changes; under it what it means */
  { char v[32]; if (P.subDelay == 0) snprintf(v, sizeof v, "0.00 s"); else snprintf(v, sizeof v, "%s%.2f s", P.subDelay > 0 ? "+" : "\xe2\x88\x92", fabs(P.subDelay));
    float e = (float)((now_() - P.dlyAt) / 0.22); e = e < 0 ? 0 : e > 1 ? 1 : e; float lift = 6 * (1 - ease_apply(EASE_OUT, e));
    text_draw(v, vx + VW / 2, cy - 30 - lift, 40, 800, WHITE_A(255), -0.4f, 0, ALIGN_CENTER);
    const char *m = P.subDelay > 0 ? T("Subtitles later") : P.subDelay < 0 ? T("Subtitles earlier") : T("Subtitle delay");
    text_draw(m, vx + VW / 2, cy + 18, 20, 600, WHITE_A(160), 0.3f, VW, ALIGN_CENTER); }
  gfx_offset_pop(); gfx_opacity_pop();
}
