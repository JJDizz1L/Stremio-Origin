#include "core/app.h"
#include "platform/plat.h"
#include "core/bgtrailer.h"
#include "core/best.h"
#include "core/keys.h"
#include "core/store.h"
#include "core/i18n.h"
#include "ui/shared.h"
#include "ui/orbs.h"
#include "ui/widgets.h"
#include "ui/sound.h"
#include "screens/revpanel.h"
#include "ui/glass.h"
#include "screens/welcome.h"
#include "ui/gfx.h"
#include "ui/text.h"
#include "ui/img.h"
#include "ui/icon.h"
#include "ui/palette.h"
#include "screens/player.h"
#include "screens/saver.h"
#include "core/hit.h"
#include "ui/qrcard.h"
#include "core/sync.h"
#include "core/update.h"
#include "screens/btrailer.h"
#include "screens/board.h"
#include "screens/login.h"
#include "screens/splash.h"
#include "core/castlink.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

Scr SCR = SCR_HOME;
Screen *screens[SCR_N];
Screen *app_overlay;
static Screen *seenOv[8]; static int nseenOv; /* overlays that have shown: drawn and ticked while they close */
void app_overlay_keep(Screen *s) { for (int k = 0; k < nseenOv; k++) if (seenOv[k] == s) return; if (nseenOv < 8) seenOv[nseenOv++] = s; } /* drawn and ticked from now on, shown or not */
int app_splash_done;
int (*app_resume_hook)(const Item *it);
void (*app_card_menu_hook)(const Item *it);
void (*app_play_hook)(const Stream *s, const char *type, const char *id, const char *vid, int fromStart);
void app_play(const Stream *s, const char *type, const char *id, const char *vid, int fromStart) { if (!s->url[0]) { app_open_external(s->ext, s->name); return; } if (btr_under()) btr_handover(); else btr_release(); /* a trailer on: the player's art comes over it, then it goes */ /* one decoder: the background trailer lets go first */ if (app_play_hook) app_play_hook(s, type, id, vid, fromStart); else toast(T("Could not play"), 2000, NULL); }
static char fromStartOnce[160];
/* "Watch from the beginning": the next play of this title/episode ignores the saved position */
void app_play_from_start(const Item *it) {
  CwInfo ci; const char *vid = it->videoId[0] ? it->videoId : (lib_cw_info(it, &ci) ? ci.vid : it->id);
  snprintf(fromStartOnce, sizeof fromStartOnce, "%s|%s", it->id, vid);
  Stream st; if (sb_resume_stream(it->id, vid, &st)) { app_play(&st, it->type, it->id, vid, 1); return; }
  if (it->ep) { extern void detail_open_streams(const char *vid); detail_open_streams(vid); return; }
  DetailParams p; memset(&p, 0, sizeof p); p.it = *it; if (strcmp(vid, it->id)) snprintf(p.it.videoId, sizeof p.it.videoId, "%s", vid); p.openStreams = 1; app_go(SCR_DETAIL, &p, 0);
}
static Scr backStack[8]; static float backX[8], backY[8]; static int nback;  /* with where each screen was left from */
static Timer1 xin;                 /* the entrance fade of the screen just entered */
/* where the screen coming in comes from: a tab slides in from the side you moved to, a title page grows out of the
   poster you pressed (Similar, a person's films: out of the button or the face), Search and Settings out of their
   icons, and Back shrinks the page toward that same spot; one short transform of the incoming screen, nothing more */
enum { NAV_NONE, NAV_ZOOM, NAV_BACKZ, NAV_SLIDE };
static struct { int kind; float x, y, dx; } navf, xmv; static Timer1 xmvT;
static Scr xoutScr; static Timer1 xoutT; /* the page left by Back, leaving over the next one (its own exit) */
#define XOUT_DUR 0.2f
static float DT; static double NOW; static unsigned FRAME; static double lastIn;
static int quitting;

float app_dt(void) { return DT; }
double app_now(void) { return NOW; }
unsigned app_frame(void) { return FRAME; }
float app_idle(void) { return (float)(NOW - lastIn); }
static int rtlHeld; /* a picture drawn left to right and mirrored as a whole (the guide's) */
int app_rtl(void) { return rtlHeld ? 0 : i18n_rtl(); }
void app_rtl_hold(int on) { rtlHeld = on; }
int app_mirror_key(int k) { if (!i18n_rtl()) return k; return k == K_LEFT ? K_RIGHT : k == K_RIGHT ? K_LEFT : k; }
int app_is_board(Scr s) { return s == SCR_HOME || s == SCR_DISC || s == SCR_LIB; }
static int xinLib; /* the Library's entrance runs 0.42 s on the page's main curve; the others 0.3 s ease-out */
float app_screen_alpha(void) { return xin.on ? ease_apply(EASE_CSS_EASEOUT, timer_x(&xin)) : 1; } /* an even ease-out: the front-loaded curves read as a pop */
static int scr_origin(float *x, float *y) { if (screens[SCR] && screens[SCR]->origin && screens[SCR]->origin(x, y)) return 1; *x = SB_W / 2; *y = SB_H / 2; return 0; }
static void nav_slide(int dir) { navf.kind = NAV_SLIDE; navf.dx = dir * (i18n_rtl() ? -1 : 1); } /* the bar is mirrored in RTL: so is the side */
void app_quit(void) { if (!quitting) fprintf(stderr, "[exit] app_quit\n"); quitting = 1; }
int app_quitting(void) { return quitting; }
/* first launch: everything stays hidden behind the splash, then rises in, rows staggered */
static double revealAt;
void app_reveal_start(void) { revealAt = NOW; }
float app_reveal_x(float delay, float dur) { if (!revealAt) return 1; float t = (float)(NOW - revealAt) - delay; if (t <= 0) return 0; if (t >= dur) return 1; return t / dur; }
int app_revealing(void) { return revealAt && NOW - revealAt < 1.2; }
void app_register(Scr s, Screen *sc) { screens[s] = sc; }

/* ---- nudge ---- */
void nudge_start(Nudge *n, int k) {
  k = app_mirror_key(k);
  float dx = k == K_LEFT ? -1 : k == K_RIGHT ? 1 : 0, dy = k == K_UP ? -1 : k == K_DOWN ? 1 : 0;
  if (!dx && !dy) return;
  if (n->t.on) return;                /* a held key lets the push finish instead of restarting it */
  n->dx = dx * 14; n->dy = dy * 10; timer_start(&n->t, 0.36f);
}
void nudge_tick(Nudge *n, float dt) { timer_tick(&n->t, dt); }
static float nudge_k(const Nudge *n) {
  if (!n->t.on) return 0;
  float x = timer_x(&n->t); x = 1 - powf(1 - x, 2); /* ease-out over the keyframes */
  if (x < 0.32f) return x / 0.32f;
  if (x < 0.7f) return 1 - (x - 0.32f) / 0.38f * 1.16f;
  return -0.16f * (1 - (x - 0.7f) / 0.3f);
}
float nudge_x(const Nudge *n) { return n->dx * nudge_k(n); }
float nudge_y(const Nudge *n) { return n->dy * nudge_k(n); }

/* ---- accent ---- */
uint32_t ACC, ACC2, ACC3, ACC_D, ACC_FG;
const char *ACCENT_KEYS[18] = { "purple", "violet", "fuchsia", "pink", "rose", "red", "orange", "amber", "gold", "lime", "green", "emerald", "teal", "cyan", "sky", "blue", "indigo", "graphite" };
static const uint32_t ACCENTS[18][4] = {
  { 0x8c5cff, 0xb794ff, 0xc9b8ff, 0x5b34e6 },
  { 0xa855f7, 0xd8b4fe, 0xe9d5ff, 0x7e22ce },
  { 0xd946ef, 0xf0abfc, 0xf5d0fe, 0xa21caf },
  { 0xec4899, 0xf9a8d4, 0xfbcfe8, 0xbe185d },
  { 0xf43f5e, 0xfda4af, 0xfecdd3, 0xbe123c },
  { 0xef4444, 0xfca5a5, 0xfecaca, 0xb91c1c },
  { 0xf97316, 0xfdba74, 0xfed7aa, 0xc2410c },
  { 0xf59e0b, 0xfcd34d, 0xfde68a, 0xb45309 },
  { 0xeab308, 0xfde047, 0xfef08a, 0xa16207 },
  { 0x84cc16, 0xbef264, 0xd9f99d, 0x4d7c0f },
  { 0x22c55e, 0x86efac, 0xbbf7d0, 0x15803d },
  { 0x10b981, 0x6ee7b7, 0xa7f3d0, 0x047857 },
  { 0x14b8a6, 0x5eead4, 0xb5f5ea, 0x0f766e },
  { 0x06b6d4, 0x67e8f9, 0xcffafe, 0x0e7490 },
  { 0x0ea5e9, 0x7dd3fc, 0xbae6fd, 0x0369a1 },
  { 0x3b82f6, 0x93b4ff, 0xc4d6ff, 0x1d4ed8 },
  { 0x6366f1, 0xa5b4fc, 0xc7d2fe, 0x4338ca },
  { 0x9aa3b5, 0xd3d8e2, 0xeef0f4, 0x525a6c } }; /* (round the wheel, then graphite: a cool glassy grey; purple, the first, is the dynamic modes' default) */
#define ACC_DEFAULT ACCENTS[0]
const uint32_t *settings_accent_rgb(int i) { return ACCENTS[i < 0 ? 0 : i > 17 ? 17 : i]; }
static const uint32_t TINT_GREY[4] = { 0x9aa6bd, 0xc1c9d8, 0xe1e5ee, 0x5d6880 };
static uint32_t accFrom[5], accTo[5]; static Timer1 accFade; static int accSet;
static float lum_of(uint32_t c) {
  float v[3] = { ((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f };
  for (int i = 0; i < 3; i++) v[i] = v[i] <= 0.03928f ? v[i] / 12.92f : powf((v[i] + 0.055f) / 1.055f, 2.4f);
  return 0.2126f * v[0] + 0.7152f * v[1] + 0.0722f * v[2];
}
static uint32_t fg_on(uint32_t a, uint32_t b) { float L = (lum_of(a) + lum_of(b)) / 2; return (1.05f / (L + 0.05f)) >= ((L + 0.05f) / 0.05f) ? 0xffffffffu : 0xff14111cu; }
static void set_accent(const uint32_t c[4]) {
  uint32_t to[5] = { 0xff000000u | c[0], 0xff000000u | c[1], 0xff000000u | c[2], 0xff000000u | c[3], fg_on(c[0], c[1]) };
  if (accSet && to[0] == accTo[0]) return;
  if (accSet) { for (int i = 0; i < 5; i++) accFrom[i] = col_mix(accFrom[i], accTo[i], accFade.on ? timer_x(&accFade) : 1); timer_start(&accFade, 0.75f); } /* a new colour fades in, never snaps */
  else for (int i = 0; i < 5; i++) accFrom[i] = to[i];
  memcpy(accTo, to, sizeof to); accSet = 1;
}
static void accent_tick(float dt) {
  float k = accFade.on ? timer_x(&accFade) : 1; timer_tick(&accFade, dt); k = 1 - powf(1 - k, 2);
  ACC = col_mix(accFrom[0], accTo[0], k); ACC2 = col_mix(accFrom[1], accTo[1], k); ACC3 = col_mix(accFrom[2], accTo[2], k); ACC_D = col_mix(accFrom[3], accTo[3], k); ACC_FG = col_mix(accFrom[4], accTo[4], k);
}
uint32_t acc_a(float a) { return col_alpha(ACC, a); }
static const char *accent_mode(void) { const char *a = store_get("accent", "logo"); if (!strcmp(a, "duo")) a = "adaptive"; return a; }
int accent_dynamic(void) { const char *a = accent_mode(); return !strcmp(a, "adaptive") || !strcmp(a, "logo"); }
static const uint32_t *fixed_accent(void) { const char *a = accent_mode(); for (int i = 0; i < 18; i++) if (!strcmp(a, ACCENT_KEYS[i])) return ACCENTS[i]; return ACCENTS[0]; }
/* a swatch made usable as an accent: its own hue and character, lightness/saturation brought into range */
static void accent_of(const Swatch *w, const Swatch *w2, uint32_t out[4]) {
  float s = clampf(w->s * 1.1f, .38f, .92f), h = w->h, l = clampf(w->l, .48f, .64f);
  out[0] = hsl2rgb(h, s, l) & 0xffffff;
  if (w2 && w2->ok && fabsf(fmodf(w2->h - h + 540, 360) - 180) > 20) out[1] = hsl2rgb(w2->h, clampf(w2->s * 1.1f, .38f, .92f), clampf(w2->l, .55f, .72f)) & 0xffffff;
  else out[1] = hsl2rgb(h, s, fminf(.78f, l + .15f)) & 0xffffff;
  out[2] = hsl2rgb(h, fminf(.9f, s), .86f) & 0xffffff; out[3] = hsl2rgb(h, s, .38f) & 0xffffff;
}
static const Swatch *first_ok(const Palette *p, int withMuted) {
  if (p->vibrant.ok) return &p->vibrant; if (p->light.ok) return &p->light; if (p->dark.ok) return &p->dark;
  if (withMuted) { if (p->muted.ok) return &p->muted; if (p->darkMuted.ok) return &p->darkMuted; }
  return NULL;
}
/* the colours found for a title, kept across runs (store node "tintMem2": id -> {cover:[..], logo:[..]}) */
typedef struct { char id[64], poster[512], logo[512]; int artDone, logoDone; Palette art, logo_; double at; } TintJob;
static TintJob tintJobs[8];
static cJSON *tint_mem(void) { cJSON *m = store_node("tintMem2"); if (!m) { m = cJSON_CreateObject(); store_set_node("tintMem2", m); } return m; }
static int tint_for(const char *id, uint32_t out[4]) {
  cJSON *m = cJSON_GetObjectItem(tint_mem(), id); if (!m) return 0;
  cJSON *a = !strcmp(accent_mode(), "logo") ? cJSON_GetObjectItem(m, "logo") : NULL; if (!a) a = cJSON_GetObjectItem(m, "cover"); if (!a || cJSON_GetArraySize(a) < 4) return 0;
  for (int i = 0; i < 4; i++) out[i] = (uint32_t)cJSON_GetArrayItem(a, i)->valuedouble;
  return 1;
}
static char scopeId[64], scopePoster[512], scopeLogo[512];
static void tint_settle(TintJob *j) {
  const Swatch *ca = first_ok(&j->art, 1), *cl = first_ok(&j->logo_, 0);
  cJSON *e = cJSON_CreateObject(); uint32_t c[4];
  if (ca) accent_of(ca, NULL, c); else memcpy(c, TINT_GREY, sizeof c);
  cJSON *arr = cJSON_CreateArray(); for (int i = 0; i < 4; i++) cJSON_AddItemToArray(arr, cJSON_CreateNumber(c[i])); cJSON_AddItemToObject(e, "cover", arr);
  if (cl && cl->s >= .3f) { accent_of(cl, NULL, c); arr = cJSON_CreateArray(); for (int i = 0; i < 4; i++) cJSON_AddItemToArray(arr, cJSON_CreateNumber(c[i])); cJSON_AddItemToObject(e, "logo", arr); } /* a logo in white or grey says nothing: the artwork then */
  cJSON *m = tint_mem(); cJSON_DeleteItemFromObject(m, j->id); cJSON_AddItemToObject(m, j->id, e);
  while (cJSON_GetArraySize(m) > 200) cJSON_DeleteItemFromArray(m, 0); /* oldest out */
  store_set_node("tintMem2", cJSON_Duplicate(m, 1));
  char id[64]; snprintf(id, sizeof id, "%s", j->id); j->id[0] = 0;
  if (scopeId[0] && !strcmp(scopeId, id)) accent_scope(scopeId, scopePoster, scopeLogo); /* the page waiting for it */
}
void accent_tint_fetch(const char *id, const char *poster, const char *logo) {
  if (!accent_dynamic() || !id || !*id || !poster || !*poster) return;
  if (cJSON_GetObjectItem(tint_mem(), id)) return;
  for (int i = 0; i < 8; i++) if (!strcmp(tintJobs[i].id, id)) { if (logo && *logo && !tintJobs[i].logo[0]) snprintf(tintJobs[i].logo, sizeof tintJobs[i].logo, "%s", logo); return; }
  int slot = -1; for (int i = 0; i < 8; i++) if (!tintJobs[i].id[0]) { slot = i; break; }
  if (slot < 0) { slot = 0; for (int i = 1; i < 8; i++) if (tintJobs[i].at < tintJobs[slot].at) slot = i; }
  TintJob *j = &tintJobs[slot]; memset(j, 0, sizeof *j); snprintf(j->id, sizeof j->id, "%s", id); snprintf(j->poster, sizeof j->poster, "%s", poster);
  if (logo && *logo) snprintf(j->logo, sizeof j->logo, "%s", logo); else j->logoDone = 1;
  if (!sb_is_imdb(id)) j->logoDone = 1; j->at = NOW;
}
static void tint_tick(void) {
  for (int i = 0; i < 8; i++) { TintJob *j = &tintJobs[i]; if (!j->id[0]) continue;
    if (!j->artDone) { int r = img_palette(j->poster, 264, 396, 0, &j->art); if (r) j->artDone = 1; if (r < 0) memset(&j->art, 0, sizeof j->art); }
    if (!j->logoDone) { int r = img_palette(j->logo, 520, 175, 1, &j->logo_); if (r) j->logoDone = 1; if (r < 0) memset(&j->logo_, 0, sizeof j->logo_); }
    if (j->artDone && j->logoDone) {
      if (!first_ok(&j->art, 1) && !first_ok(&j->logo_, 0)) { j->id[0] = 0; continue; }
      tint_settle(j); }
  }
}
/* a title's own accent (Settings' preview of the dynamic modes): 1 with it in out, 0 while it is worked out */
int accent_tint_of(const char *id, const char *poster, const char *logo, uint32_t out[4]) { if (tint_for(id, out)) return 1; accent_tint_fetch(id, poster, logo); return 0; }
static int tint_busy(const char *id) { for (int i = 0; i < 8; i++) if (!strcmp(tintJobs[i].id, id)) return 1; return 0; }
void accent_apply(void) { if (accent_dynamic()) accent_scope(scopeId[0] ? scopeId : NULL, scopePoster, scopeLogo); else set_accent(fixed_accent()); }
/* Dynamic accents ("From the logo", "From the artwork") belong to ONE title: they show on that title's
   page and in its player; everywhere else the app keeps the default (purple). */
void accent_scope(const char *id, const char *poster, const char *logo) {
  if (id != scopeId) { if (id && *id) { snprintf(scopeId, sizeof scopeId, "%s", id); snprintf(scopePoster, sizeof scopePoster, "%s", poster ? poster : ""); snprintf(scopeLogo, sizeof scopeLogo, "%s", logo ? logo : ""); } else scopeId[0] = 0; }
  if (!accent_dynamic()) { set_accent(fixed_accent()); return; }
  uint32_t c[4];
  if (scopeId[0] && tint_for(scopeId, c)) { set_accent(c); return; }
  if (scopeId[0]) { accent_tint_fetch(scopeId, scopePoster, scopeLogo); if (tint_busy(scopeId)) return; } /* coming in a moment: keep what is shown, no purple flash */
  set_accent(ACC_DEFAULT);
}

/* ---- toast ---- */
static char toastMsg[256], toastKey[32]; static double toastUntil; static Tween toastA; static int toastOn, toastKind; static Timer1 toastNew;
/* the icon a notice carries: working on it (ends in an ellipsis), something went wrong, or done. Read from the
   English text (screens pass it translated: looked up back) */
enum { TK_NONE, TK_WORK, TK_ERR, TK_OK };
static int has_ci(const char *s, const char *w) { size_t n = strlen(w); for (; *s; s++) if (!strncasecmp(s, w, n)) return 1; return 0; }
static int toast_kind_of(const char *m) {
  size_t n = strlen(m); if ((n >= 3 && !strcmp(m + n - 3, "\xe2\x80\xa6")) || (n >= 3 && !strcmp(m + n - 3, "..."))) return TK_WORK;
  const char *en = i18n_key_of(m); if (en) m = en;
  static const char *er[] = { "fail", "could not", "couldn", "unavailable", "no trailer", "no embedded", "needs a", "error", "nothing to" };
  static const char *ok[] = { "updated", "signed in", "in english", "in your language", "latest version", "synced", "saved" };
  for (int i = 0; i < 9; i++) if (has_ci(m, er[i])) return TK_ERR;
  for (int i = 0; i < 7; i++) if (has_ci(m, ok[i])) return TK_OK;
  return TK_NONE;
}
void toast(const char *msg, int ms, const char *key) {
  fprintf(stderr, "[toast] %s\n", msg); /* the kept log shows what the screen said */
  if (toastOn && strcmp(toastMsg, msg)) timer_start(&toastNew, 0.25f); /* a new message over an old one: the words fade in, the card stays */
  snprintf(toastMsg, sizeof toastMsg, "%s", msg); snprintf(toastKey, sizeof toastKey, "%s", key ? key : ""); toastKind = key && *key ? TK_NONE : toast_kind_of(msg);
  toastUntil = NOW + (ms > 0 ? ms : 2600) / 1000.0; if (!toastOn) { toastOn = 1; tween_to(&toastA, 1, 0.4f, EASE_CSS_EASE); }
}
/* notices (one style everywhere): an opaque card, bottom centre; it comes up from below and goes back down (a calm
   'ease': a notice is read, not chased); a leading mark says what it is (working, done, failed); a key badge in the
   keycap style of the hints */
static void toast_tick(float dt) { tween_tick(&toastA, dt); timer_tick(&toastNew, dt); if (toastOn && NOW >= toastUntil) { toastOn = 0; tween_to(&toastA, 0, 0.3f, EASE_CSS_EASE); } }
static void toast_draw(void) {
  float a = toastA.v; if (a <= 0.001f) return;
  int rtl = app_rtl(); float tw = text_width(toastMsg, 26, 600, -0.1f), keyW = 0, kindW = toastKind ? 26 + 16 - 6 : 0;
  int keyG = !strcmp(toastKey, "\001back"); /* the remote's Back key, drawn */
  if (toastKey[0]) keyW = keyG ? 38 + 14 - 6 : 24 + text_width(toastKey, 21, 600, 0) + 14 - 6;
  float w = 56 + kindW + keyW + tw, h = 72, x = SB_W / 2 - w / 2, y = SB_H - 64 - h + 36 * (1 - a);
  gfx_opacity_push(a);
  gfx_shadow(x, y + 16, w, h, 18, 40, BLACK_A(115));
  gfx_rrect(x, y, w, h, 18, RGB(0x1f, 0x1c, 0x28)); gfx_ring(x, y, w, h, 18, 1, WHITE_A(15));
  float cx = rtl ? x + w - 28 : x + 28; /* the pen: left to right, or right to left */
  if (toastKind) { float ix = rtl ? cx + 6 - 26 : cx - 6, iy = y + h / 2 - 13;
    if (toastKind == TK_WORK) { /* a faint ring whose top quarter turns */
      gfx_ring(ix, iy, 26, 26, 13, 3, WHITE_A(41)); float a0 = fmodf((float)NOW / 0.8f, 1) * 360;
      for (int k = 0; k <= 12; k++) { float an = (a0 + k * 7.5f - 45) * (float)M_PI / 180; gfx_circle(ix + 13 + 11.5f * sinf(an), iy + 13 - 11.5f * cosf(an), 1.5f, ACC3); } }
    else if (toastKind == TK_ERR) { gfx_circle(ix + 13, iy + 13, 13, RGBA(239, 143, 138, 46)); gfx_rrect(ix + 11, iy + 6, 4, 9, 2, RGB(0xef, 0x8f, 0x8a)); gfx_rrect(ix + 11, iy + 17, 4, 4, 2, RGB(0xef, 0x8f, 0x8a)); }
    else { gfx_circle(ix + 13, iy + 13, 13, ACC); icon_draw("check", ix + 4, iy + 4, 18, ACC_FG); }
    cx += rtl ? -kindW : kindW; }
  if (keyG) { key_hint(rtl ? cx + 6 : cx - 6, y + h / 2, K_BACK, NULL, WHITE_A(235), rtl); cx += rtl ? -keyW : keyW; }
  else if (toastKey[0]) { float kw = 24 + text_width(toastKey, 21, 600, 0), kh = 8 + text_line_height(21), kx = rtl ? cx + 6 - kw : cx - 6;
    gfx_rrect(kx, y + h / 2 - kh / 2, kw, kh, 9, WHITE_A(31)); text_draw(toastKey, kx + kw / 2, y + h / 2 - text_line_height(21) / 2.0f, 21, 600, WHITE_A(235), 0, 0, ALIGN_CENTER); cx += rtl ? -keyW : keyW; }
  float na = toastNew.on ? ease_apply(EASE_CSS_EASE, timer_x(&toastNew)) : 1; if (na < 1) gfx_opacity_push(na); /* a new message over an old one: the words fade in, the card stays */
  text_draw(toastMsg, cx, y + h / 2 - text_line_height(26) / 2.0f, 26, 600,
 WHITE_A(240), -0.1f, 0, rtl ? ALIGN_RIGHT : ALIGN_LEFT);
  if (na < 1) gfx_opacity_pop();
  gfx_opacity_pop();
}
static double exitAt = -10;
void exit_ask(void) { if (NOW - exitAt < 2.5) { app_quit(); return; } exitAt = NOW; toast(T("Press again to exit"), 2500, "\001back"); }

/* ---- the clock ---- */
static char clockTxt[8], clockOld[8]; static Timer1 clockAnim; static double clockAt;
static void clock_tick(void) {
  timer_tick(&clockAnim, DT);
  if (NOW - clockAt < 1) return; clockAt = NOW;
  time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm); char s[8]; snprintf(s, sizeof s, "%02d:%02d", tm.tm_hour, tm.tm_min);
  if (strcmp(s, clockTxt)) { int first = !clockTxt[0]; snprintf(clockOld, sizeof clockOld, "%s", clockTxt); snprintf(clockTxt, sizeof clockTxt, "%s", s); if (!first) timer_start(&clockAnim, 0.6f); }
}
/* a new minute: only the digits that changed crossfade, drifting a few px (the rest of the time stays still); the
   time sits from a fixed left edge so nothing beside the changing digits moves */
static int clockLiquid; /* the bar's own clock: its glass bends the backdrop (the player's and the guide's: the glass alone) */
static void lg_glass(float x, float y, float w, float h, float r, uint32_t tint, float k);
static int lgSeed; /* the next piece's own timing (0 capsule, 1-2 the circles, 3 the clock), then 0 again */
static float lgPushX, lgPushY, lgBase[4]; /* the next piece is taking up an edge push (its units; its rest shape), then none again */
static float lg_drift_at(float ph);
void app_clock_draw(float right, float y, float alpha) {
  static float W; if (W <= 0) W = text_width("00:00", 38, 600, -0.6f); float x0 = right - W; if (alpha < 1) gfx_opacity_push(alpha);
  { float ph = 76, pw = W + 52, px = x0 - 26, py = y + text_line_height(38) / 2 - ph / 2; /* a glass pill behind it, the bar's circles' sister */
    if (clockLiquid) { lgSeed = 3; lg_glass(px, py, pw, ph, ph / 2, ACC, 0.85f); } else { lglass_light(lg_drift_at(113.0f)); lglass_shell(px, py, pw, ph, ph / 2, RGBA(14, 14, 22, 120), 0.85f); } lglass_light(0); }
  { static float cw = -1, cwTo = -1; float tw = text_width(clockTxt, 38, 600, -0.6f); if (cw < 0) cw = tw; cwTo = tw; cw += (cwTo - cw) * fminf(1, DT * 8); x0 += (W - cw) / 2; } /* the time centred in its pill (gliding when its width changes) */
  float k = clockAnim.on ? ease_apply(EASE_IN_OUT, timer_x(&clockAnim)) : 1;
  size_t n = 0; if (k < 1) while (clockTxt[n] && clockTxt[n] == clockOld[n]) n++; else n = strlen(clockTxt);
  char pre[8]; snprintf(pre, sizeof pre, "%.*s", (int)n, clockTxt); float pw = n ? text_width(pre, 38, 600, -0.6f) : 0;
  if (n) text_draw(pre, x0, y, 38, 600, WHITE_A(230), -0.6f, 0, ALIGN_LEFT);
  if (k < 1) {
    gfx_opacity_push(1 - k); text_draw(clockOld + n, x0 + pw, y - 5 * k, 38, 600, WHITE_A(230), -0.6f, 0, ALIGN_LEFT); gfx_opacity_pop();
    gfx_opacity_push(k); text_draw(clockTxt + n, x0 + pw, y + 5 * (1 - k), 38, 600, WHITE_A(230), -0.6f, 0, ALIGN_LEFT); gfx_opacity_pop(); }
  if (alpha < 1) gfx_opacity_pop();
}
#ifdef SB_LINUX
void app_fsbtn_draw(float right, float y);
#endif
static void clock_draw(void) { float W = text_width("00:00", 38, 600, -0.6f); clockLiquid = 1; float right = app_rtl() ? 48 + W : SB_W - 48; app_clock_draw(right, 50 - 22, 1); clockLiquid = 0;
#ifdef SB_LINUX
  app_fsbtn_draw(right, 50 - 22);
#endif
}
#ifdef SB_LINUX
/* the fullscreen button (desktop): a glass circle on the clock's centre side, expand / compress icon
   for the state. Drawn everywhere the clock is; the player handles its clicks itself (player_ptr). */
void app_fsbtn_geom(float right, float y, float *x, float *yy) {
  static float W; if (W <= 0) W = text_width("00:00", 38, 600, -0.6f);
  float px = right - W - 26, pw = W + 52; /* the clock pill, as in app_clock_draw */
  float bx = px + pw / 2 >= SB_W / 2 ? px - 16 - 76 : px + pw + 16; /* its centre side, never off-screen */
  if (x) *x = bx; if (yy) *yy = y + text_line_height(38) / 2 - 38;
}
static void fs_act(int a, int b) { (void)a; (void)b; app_toggle_fullscreen(); }
void app_toggle_fullscreen(void) { plat_set_fullscreen(!plat_fullscreen()); }
int app_fsbtn_hit(float right, float y, float x, float yy) { float bx, by; app_fsbtn_geom(right, y, &bx, &by); return x >= bx && x < bx + 76 && yy >= by && yy < by + 76; }
void app_fsbtn_draw(float right, float y) {
  float bx, by; app_fsbtn_geom(right, y, &bx, &by);
  lglass_light(lg_drift_at(171.0f)); lglass_shell(bx, by, 76, 76, 38, RGBA(14, 14, 22, 120), 0.85f); lglass_light(0);
  gfx_ring(bx, by, 76, 76, 38, 1, WHITE_A(46));
  icon_draw(plat_fullscreen() ? "desk_restore" : "desk_full", bx + 18, by + 18, 40, WHITE_A(210));
  hit_act(bx, by, 76, 76, fs_act, 0, 0);
}
#endif

/* ---- the top bar: centred tabs, the corner icons, one pill that glides to the focused tab ---- */
TopNav topNav = { .idx = NAV_HOME };
static const char *chipLabel(int i) { return i == NAV_DISC ? T("Discover") : i == NAV_HOME ? T("Home") : T("Library"); }
static int chipCur(int i) {
  Scr t = i == NAV_SEARCH ? SCR_SEARCH : i == NAV_SET ? SCR_SET : i == NAV_DISC ? SCR_DISC : i == NAV_HOME ? SCR_HOME : SCR_LIB;
  return t == SCR || (t == SCR_HOME && (SCR == SCR_DETAIL || SCR == SCR_SIM));
}
static int chipFocused(int i) { return topNav.active && topNav.idx == i && !update_pill_focused(); } /* the update notice holds the highlight */
static void topnav_layout(void) {
  float w[3], tot = 0;
  for (int i = 0; i < 3; i++) { int f = chipFocused(NAV_DISC + i) || chipCur(NAV_DISC + i); w[i] = text_width(chipLabel(NAV_DISC + i), 32, f ? 700 : 400, 0) + 80; tot += w[i] + 18; }
  float x = SB_W / 2 - (tot - 18) / 2;
  for (int i = 0; i < 3; i++) { int ci = app_rtl() ? NAV_DISC + 2 - i : NAV_DISC + i; topNav.chipX[ci] = x; topNav.chipW[ci] = w[ci - NAV_DISC]; x += w[ci - NAV_DISC] + 18; }
  topNav.chipX[NAV_SET] = app_rtl() ? SB_W - 40 - 76 : 40; topNav.chipW[NAV_SET] = 76;
  topNav.chipX[NAV_SEARCH] = app_rtl() ? SB_W - 128 - 76 : 128; topNav.chipW[NAV_SEARCH] = 76;
  topNav.laidOut = 1;
}
static void topnav_paint(void) {
  topnav_layout();
  int fc = topNav.active && topNav.idx >= NAV_DISC ? topNav.idx : -1;
  if (fc >= 0) {
    float x = topNav.chipX[fc], w = topNav.chipW[fc];
    if (topNav.pillShown < 0.5f && !topNav.pill.on) { tween_set(&topNav.px, x); tween_set(&topNav.pw, w); } /* appears in place */
    else { tween_to(&topNav.px, x, GLIDE_DUR, GLIDE_EASE); tween_to(&topNav.pw, w, GLIDE_DUR, GLIDE_EASE); }
    tween_to(&topNav.pill, 1, 0.2f, EASE_CSS_EASE); topNav.pillShown = 1;
  } else { tween_to(&topNav.pill, 0, 0.2f, EASE_CSS_EASE); topNav.pillShown = 0; }
  /* a corner's glass circle fades in (.2 s) and grows from .78 (.3 s); its coloured dot shrinks away (.24 s) */
  for (int i = NAV_SET; i <= NAV_SEARCH; i++) { int f = chipFocused(i);
    tween_to(&topNav.cornerF[i], f, 0.2f, EASE_CSS_EASE); tween_to(&topNav.cornerS[i], f, 0.3f, EASE_OUT); tween_to(&topNav.dotK[i], f, 0.24f, EASE_STD); }
  tween_to(&topNav.setRot, chipFocused(NAV_SET) ? 90 : 0, 0.5f, EASE_OUT);
}
static Scr chipScr(int i) { return i == NAV_SEARCH ? SCR_SEARCH : i == NAV_SET ? SCR_SET : i == NAV_DISC ? SCR_DISC : i == NAV_HOME ? SCR_HOME : SCR_LIB; }
void topnav_focus(void) { topNav.active = 1; topnav_paint(); }
void topnav_switch(int i) {
  i = i < 0 ? 0 : i > 4 ? 4 : i;
  if (i == topNav.idx && (app_is_board(SCR) || SCR == SCR_SEARCH || SCR == SCR_SET)) return;
  { Scr c0 = (SCR == SCR_DETAIL || SCR == SCR_SIM) ? SCR_HOME : SCR; int cs = -1; for (int j = 0; j < 5; j++) if (chipScr(j) == c0) cs = j;
    if (i == NAV_SEARCH || i == NAV_SET) { navf.kind = NAV_ZOOM; navf.x = topNav.chipX[i] + topNav.chipW[i] / 2; navf.y = 50; } else nav_slide(i >= cs ? 1 : -1); }
  topNav.idx = i; nback = 0; topNav.active = 1;
  app_go(chipScr(i), NULL, 1); topNav.active = 1; topnav_paint();
}
static void topnav_flush(int drop) { if (topNav.want < 0) return; int w = topNav.want; topNav.want = -1; if (drop) return; topNav.idx = -1; topnav_switch(w); }
static void topnav_enter_content(void) {
  topNav.active = 0; Scr t = chipScr(topNav.idx);
  extern void board_from_bar(void), lib_from_bar(void);
  if (SCR == SCR_HOME || SCR == SCR_DISC) board_from_bar();   /* a board: its first row (Discover: its spotlight) */
  else if (SCR == SCR_LIB) lib_from_bar();                    /* the Library: its categories */
  else if (t != SCR_SEARCH && t != SCR_SET && screens[SCR] && screens[SCR]->key) screens[SCR]->key(K_DOWN); /* Search and Settings keep their own top highlight */
  topnav_paint();
}
static void topnav_nudge(void) { nudge_start(&topNav.nudge, K_UP); }
/* the pill leaves on the key press; the screen follows once the remote rests (150 ms): building a screen in
   the same moment starves the slide, and passing through a tab on the way to another one no longer builds it */
static int topnav_key(int k) {
  if (update_pill_key(k, topNav.idx == NAV_SET)) { topnav_paint(); return 1; }
  topnav_paint();
  if (k == K_LEFT || k == K_RIGHT) {
    int i = topNav.idx + (k == K_RIGHT ? 1 : -1); if (i < 0) i = 0; if (i > 4) i = 4;
    if (i == topNav.idx) { nudge_start(&topNav.nudge, k); return 1; } /* the end of the bar */
    Scr t = chipScr(i);
    if ((t == SCR_SEARCH || t == SCR_SET) && SCR != t) { topnav_flush(1); topNav.idx = i; topnav_paint(); return 1; }
    topNav.idx = i; topnav_paint(); topNav.want = i; topNav.swAt = NOW + 0.15;
  } else if (k == K_DOWN || k == K_OK) {
    topnav_flush(0); Scr tg = chipScr(topNav.idx);
    if ((tg == SCR_SEARCH || tg == SCR_SET) && SCR != tg) {
      if (k == K_OK) { topNav.active = 1; app_go(tg, NULL, 1); topnav_enter_content(); return 1; }
      /* Down from a corner icon: into the page you are on */
      Scr cur = (SCR == SCR_DETAIL || SCR == SCR_SIM) ? SCR_HOME : SCR; for (int j = 2; j < 5; j++) if (chipScr(j) == cur) topNav.idx = j; }
    topnav_enter_content();
  } else if (k == K_UP) topnav_nudge();
  return 1;
}
void topnav_tick(float dt) {
  if (topNav.want >= 0 && NOW >= topNav.swAt) topnav_flush(0);
  tween_tick(&topNav.px, dt); tween_tick(&topNav.pw, dt); tween_tick(&topNav.pill, dt); for (int i = 0; i < 2; i++) { tween_tick(&topNav.cornerF[i], dt); tween_tick(&topNav.cornerS[i], dt); tween_tick(&topNav.dotK[i], dt); } tween_tick(&topNav.setRot, dt);
  nudge_tick(&topNav.nudge, dt);
  if (!topNav.laidOut) topnav_paint();
}
/* Search steps aside for a notice out of the Settings icon; when a long one would push it onto the tabs it fades away */
static float search_fade(float sh) {
  if (sh == 0) return 1; float sx = topNav.chipX[NAV_SEARCH] + sh, room;
  if (!app_rtl()) { float l = SB_W; for (int j = NAV_DISC; j <= NAV_LIB; j++) if (topNav.chipX[j] < l) l = topNav.chipX[j]; room = l - 16 - (sx + 76); }
  else { float r = 0; for (int j = NAV_DISC; j <= NAV_LIB; j++) if (topNav.chipX[j] + topNav.chipW[j] > r) r = topNav.chipX[j] + topNav.chipW[j]; room = sx - 16 - r; }
  return room >= 0 ? 1 : clampf(1 + room / 40, 0, 1);
}
/* liquid glass, drawn (no blur read back: the bar sits over moving backdrops and trailers): a tinted see-through
   body, a sheen from the top, the rim lit at the top-left and bottom-right as by a light at 135 degrees, and a soft
   shadow; k scales it all (0 none, 1 at rest, more when lit), tint is the accent */
#define LG_H 112.0f /* the top bar glass strip's height (layout units) */
static GLuint lg_tex(float x, float y, float w, float h, float *u0, float *v0, float *u1, float *v1);
static void lg_tick(void);
/* the light on the glass: a slow wander of its own (a few percent along the top, never still) */
static float lg_drift_at(float ph) { float t = (float)NOW + ph; return 0.018f * sinf(t * 0.157f) + 0.012f * sinf(t * 0.263f + 1.3f) + 0.008f * sinf(t * 0.411f + 4.1f); } /* (three slow waves that never line up: a few percent, 15 to 40 s; ph sets each piece of glass apart) */
static float lgSpotLean; /* the next piece's spot leaning along (the capsule's towards the focused tab), then none again */
static void lg_glass(float x, float y, float w, float h, float r, uint32_t tint, float k) {
  if (k <= 0.004f) { lgSpotLean = 0; lgSeed = 0; lgPushX = lgPushY = 0; return; }
  float pxs = lgPushX, pys = lgPushY, pk = fminf(1, sqrtf(pxs * pxs + pys * pys) / 12); lgPushX = lgPushY = 0; /* how hard it is being pushed, 0..1 */
  int small = w < 300; /* (the circles and the clock: a softer touch than the capsule) */
  int sd = lgSeed < 0 ? 0 : lgSeed > 3 ? 3 : lgSeed; float spotK = 0, su = 0.5f, sv = 0.32f;
  { float n = (float)lgSeed, t = (float)NOW * (1 + 0.13f * n) + 17.0f * n, p = 2.39f * n; /* each piece its own pace and phase */
    su = 0.5f + 0.4f * (0.55f * sinf(t * 0.27f + p) + 0.3f * sinf(t * 0.43f + 2.0f + 1.7f * p) + 0.15f * sinf(t * 0.71f + 4.4f + 2.3f * p)) + lgSpotLean; sv = 0.32f + 0.1f * sinf(t * 0.33f + 1.0f + p);
    spotK = (small ? 0.6f : 1.0f) * (1 + 0.7f * pk); lglass_light(su - 0.12f); lgSpotLean = 0; lgSeed = 0; } /* every piece of the bar's glass with its own wandering spot, on its own timing, its glint riding with it */
  gfx_shadow(x, y + 12, w, h, r, 40, BLACK_A((int)(64 * fminf(k, 1))));
  { float u0, v0, u1, v1; GLuint t = pk > 0 ? lg_tex(lgBase[0], lgBase[1], lgBase[2], lgBase[3], &u0, &v0, &u1, &v1) : lg_tex(x, y, w, h, &u0, &v0, &u1, &v1); /* the blurred page behind, drawn in at the edges as by a lens (pushed: the picture at rest, stretched with the glass) */
    /* the picture behind changing (signing in, the start screen, the guide, a page of glows): the glass crossfades from the one it had, never a jump */
    static GLuint curT[4], oldT[4]; static float oldUv[4][4], curUv[4][4], fadeK[4]; static unsigned long long seenF[4];
    if (seenF[sd] + 2 < FRAME) { curT[sd] = t; fadeK[sd] = 0; } /* (not drawn a while: no fade from what it had then) */
    else if (t != curT[sd]) { oldT[sd] = curT[sd]; memcpy(oldUv[sd], curUv[sd], sizeof oldUv[sd]); curT[sd] = t; fadeK[sd] = 1; } /* (from nothing too: out of the plain glass) */
    seenF[sd] = FRAME; curUv[sd][0] = u0; curUv[sd][1] = v0; curUv[sd][2] = u1; curUv[sd][3] = v1; if (fadeK[sd] > 0) fadeK[sd] -= DT / 0.5f;
    float pull = fminf(h * 0.5f, 30) * (1 + 0.5f * pk); /* (pushed harder, the lens bends more) */
    float ok = fadeK[sd] > 0 ? fadeK[sd] * fadeK[sd] * (3 - 2 * fadeK[sd]) : 0;
    if (t) { float na = oldT[sd] ? 1 : 1 - ok; if (na < 0.999f) { gfx_rrect(x, y, w, h, r, RGBA(16, 16, 24, (int)(fminf(110 * k, 200) * ok))); gfx_opacity_push(na); }
      gfx_liquid_spot(su, sv, spotK); gfx_liquid(t, x, y, w, h, r, pull, fminf(k, 1), u0, v0, u1, v1); if (na < 0.999f) gfx_opacity_pop(); }
    else gfx_rrect(x, y, w, h, r, RGBA(16, 16, 24, (int)(fminf(110 * k, 200) * (oldT[sd] ? 1 - ok : 1))));
    if (ok > 0.004f && oldT[sd]) { gfx_opacity_push(ok); gfx_liquid_spot(su, sv, spotK); gfx_liquid(oldT[sd], x, y, w, h, r, pull, fminf(k, 1), oldUv[sd][0], oldUv[sd][1], oldUv[sd][2], oldUv[sd][3]); gfx_opacity_pop(); }
    if (t || ok > 0.004f) gfx_rrect(x, y, w, h, r, RGBA(10, 10, 16, (int)(fminf(18 * k, 50)))); }
  gfx_rrect(x, y, w, h, r, col_alpha(tint | 0xff000000u, 0.08f * k));
  gfx_rrect_v(x, y, w, h, r, WHITE_A((int)(fminf(28 * k, 70))), WHITE_A(0));
  gfx_ring(x, y, w, h, r, 1, WHITE_A((int)(fminf(34 * k, 90))));
  lglass_rim(x, y, w, h, r, 0.10f * k * (1 + 0.4f * pk), (small ? 0.17f : 0.28f) * fminf(k, 1.4f) * (1 + 0.25f * pk), 1); lglass_rim(x, y, w, h, r, 0.05f * k, 0.30f * fminf(k, 1.4f), 0); /* (a soft light: the glint at half) */
}
/* the edge push (nowhere to go) taken up by the glass holding the focus: it bulges out the way it was pushed, a little
   thinner across, and springs back with the push's rebound, so the focused pill never pokes out of it */
static void lg_squish(float *x, float *y, float *w, float *h, float nx, float ny) {
  lgBase[0] = *x; lgBase[1] = *y; lgBase[2] = *w; lgBase[3] = *h; lgPushX = nx; lgPushY = ny; /* the picture in it stretches with it (lg_glass) */
  float gx = fabsf(nx) * 0.85f, gy = fabsf(ny) * 0.85f;
  if (nx > 0) *w += gx; else if (nx < 0) { *x -= gx; *w += gx; }
  if (ny > 0) *h += gy; else if (ny < 0) { *y -= gy; *h += gy; }
  float qx = fabsf(nx) * 0.3f, qy = fabsf(ny) * 0.4f; *y += qx; *h -= 2 * qx; *x += qy; *w -= 2 * qy;
}
static void corner_draw(int i, const char *icon, uint32_t dot, float rot) {
  float sh = i == NAV_SEARCH ? update_search_shift(topNav.chipX[NAV_SEARCH], topNav.chipX[NAV_SET]) : 0, sf = i == NAV_SEARCH ? search_fade(sh) : 1;
  if (sf <= 0.001f) return; if (sf < 0.999f) gfx_opacity_push(sf);
  float f = topNav.cornerF[i].v, x = topNav.chipX[i] + sh, y = 50 - 38;
  int nd = chipFocused(i); if (nd) gfx_offset_push(nudge_x(&topNav.nudge), nudge_y(&topNav.nudge)); /* the edge push moves the focused icon too */
  lgSeed = 1 + i; lg_glass(x, y, 76, 76, 38, ACC, 0.85f + 0.3f * f); /* the bar's glass, as the capsule's (its own wandering spot), a little brighter lit */
  if (f > 0.001f) { float s = 0.78f + 0.22f * topNav.cornerS[i].v; gfx_opacity_push(f); gfx_scale_push(s, s, x + 38, y + 38);
    gfx_rrect_v(x, y, 76, 76, 38, col_alpha(ACC2 | 0xff000000u, 0.42f), col_alpha(ACC | 0xff000000u, 0.30f)); /* the accent */
    gfx_scale_pop(); gfx_opacity_pop(); }
  static Tween lit[2]; tween_to(&lit[i], chipFocused(i) || chipCur(i) ? 1 : 0, 0.22f, EASE_CSS_EASE); tween_tick(&lit[i], DT); /* lit and back crossfaded, as the tabs' words are */
  uint32_t col = col_mix(WHITE_A(184), WHITE_A(255), lit[i].v);
  icon_draw_rot(icon, x + 18, y + 18, 40, col, rot); /* outlined, 40 px */
  if (i == NAV_SET && update_avail()) { float bx = app_rtl() ? x + 9 + 4.5f : x + 76 - 9 - 4.5f, by = y + 76 - 9 - 4.5f; gfx_circle(bx, by, 7.5f, PAGE); gfx_circle(bx, by, 4.5f, ACC2); } /* an update is waiting */
  float dk = topNav.dotK[i].v, d = (1 - dk) * (i == NAV_SET ? 1 - update_dot_hide() : 1); if (d > 0.001f) { /* the remote's colour key: a short arc along the top, just inside the rim */
    uint32_t c = dot | 0xff000000u; gfx_clip_push(x, y, 76, 4 + 9); gfx_ring(x + 3.5f, y + 3.5f, 69, 69, 34.5f, 1.75f, col_alpha(c, 0.32f * d)); gfx_clip_pop(); /* soft ends */
    gfx_clip_push(x, y, 76, 4 + 5); gfx_ring(x + 3.5f, y + 3.5f, 69, 69, 34.5f, 1.75f, col_alpha(c, 0.5f * d)); gfx_clip_pop(); } /* it steps away when the icon is focused */
  if (nd) gfx_offset_pop();
  if (sf < 0.999f) gfx_opacity_pop();
}
static void chip_focus(int i, int b) { (void)b; if (!topNav.active || topNav.idx != i) { topnav_flush(1); if (!topNav.active) { topNav.active = 1; } topNav.idx = i; topnav_paint(); } }
static void chip_act(int i, int b) { (void)b; Scr tg = chipScr(i); Scr cur = (SCR == SCR_DETAIL || SCR == SCR_SIM) ? SCR_HOME : SCR; int ci = -1; for (int j = 2; j < 5; j++) if (chipScr(j) == cur) ci = j;
  if (app_overlay) { app_overlay->key(K_BACK); app_overlay = NULL; }
  topnav_flush(1); topNav.active = 1; topNav.idx = i;
  if (tg == SCR_SEARCH || tg == SCR_SET) topnav_key(K_OK); /* opens it, the highlight inside */
  else if (i != ci || !app_is_board(SCR)) { topNav.idx = -1; topnav_switch(i); }
  else topnav_enter_content(); }
void topnav_draw(void) {
  if (!topNav.laidOut) topnav_paint();
  int signin = SCR == SCR_LOGIN; float pg = revpanel_page_k(); /* the reviews risen into the title page: the bar goes up and away for its buttons */
  if (pg >= 0.999f) return;
  if (!signin && pg < 0.3f) for (int i = 0; i < 5; i++) { float h = i < 2 ? 76 : 66; hit_add_nav(topNav.chipX[i] + (i == NAV_SEARCH ? update_search_shift(topNav.chipX[NAV_SEARCH], topNav.chipX[NAV_SET]) : 0), 50 - h / 2, topNav.chipW[i], h, chip_focus, chip_act, i, 0); }
  static float barOut, barV; { float want = btr_melt() > 0.04f ? 1.0f : 0.0f, w = 11.0f, dt = DT > 0.05f ? 0.05f : DT; barV += (w * w * (want - barOut) - 2 * w * barV) * dt; barOut += barV * dt; if (fabsf(want - barOut) < 0.002f && fabsf(barV) < 0.01f) { barOut = want; barV = 0; } } /* its own motion: a spring that starts softly, travels and settles (half a second), turning back smoothly from wherever it is */
  float mt = barOut < 0 ? 0 : barOut > 1 ? 1 : barOut, ma = mt < 0.15f ? 0 : mt > 0.75f ? 1 : (mt - 0.15f) / 0.6f, rv = ease_apply(EASE_CSS_EASE, app_reveal_x(0, 0.4f)) * (1 - ma * ma * (3 - 2 * ma)); rv *= 1 - pg; if (rv < 1) gfx_opacity_push(rv); /* the background trailer: the bar rises out of the screen, fading across the middle of its way, and comes back as it goes */ float lift = -70 * pg - 140 * mt; if (lift < 0) gfx_offset_push(0, lift);
  float nx = nudge_x(&topNav.nudge), ny = nudge_y(&topNav.nudge);
  if (!signin) {
    /* the tabs' capsule: one piece of glass behind the three, brighter while the bar has the focus */
    { static Tween lit; tween_to(&lit, topNav.active ? 1 : 0, 0.3f, EASE_CSS_EASE); tween_tick(&lit, DT);
      float cx0 = topNav.chipX[NAV_DISC] - 8, cx1 = topNav.chipX[NAV_LIB] + topNav.chipW[NAV_LIB] + 8;
      { static float lean; float want = topNav.active && topNav.idx >= NAV_DISC ? ((topNav.px.v + topNav.pw.v / 2 - cx0) / (cx1 - cx0) - 0.5f) * 0.12f : 0; lean += (want - lean) * fminf(1, DT * 0.8f); /* the light leans a little towards the focused tab, slowly */
        lgSpotLean = lean; }
      { float gx = cx0, gy = 50 - 39, gw = cx1 - cx0, gh = 78; if (topNav.active && topNav.idx >= NAV_DISC) lg_squish(&gx, &gy, &gw, &gh, nx, ny); /* the focus pushed against the end: the capsule bulges with it */
        lg_glass(gx, gy, gw, gh, gh / 2, ACC, 0.85f + 0.3f * lit.v); }
      /* the current tab, when the focus is elsewhere: a softer piece of glass inside it */
      static Tween mk[3]; /* (only while the focus is out of the bar: moving to Search or Settings, nothing is left lit on the tabs) */
      for (int i = NAV_DISC; i <= NAV_LIB; i++) { tween_to(&mk[i - NAV_DISC], chipCur(i) && !topNav.active ? 1 : 0, 0.2f, EASE_CSS_EASE); tween_tick(&mk[i - NAV_DISC], DT); }
      for (int i = NAV_DISC; i <= NAV_LIB; i++) { float a = mk[i - NAV_DISC].v; if (a > 0.01f) { gfx_opacity_push(a); gfx_rrect(topNav.chipX[i], 50 - 30, topNav.chipW[i], 60, 30, WHITE_A(20)); gfx_ring(topNav.chipX[i], 50 - 30, topNav.chipW[i], 60, 30, 1, WHITE_A(46)); gfx_opacity_pop(); } } }
    /* the focus: liquid accent glass that stretches as it glides and settles back round (its speed, smoothed) */
    float pa = topNav.pill.v; if (pa > 0.001f) { static float lastX, sv, dir; float x = topNav.px.v, w = topNav.pw.v, h = 62, r = 31; float x0 = lastX; lglass_stretch(&sv, &lastX, x, DT); if (fabsf(x - x0) > 0.01f) dir = x > x0 ? 1 : -1; /* 0..1, and which way */
      float sw = w * (1 + 0.22f * sv), sh = h * (1 - 0.10f * sv), px = x + w / 2 - sw / 2, py = 50 - sh / 2;
      gfx_opacity_push(pa); gfx_offset_push(nx, ny);
      { float u0, v0, u1, v1; GLuint t = lg_tex(px, py, sw, sh, &u0, &v0, &u1, &v1); if (t) { float lag = (-dir * sv * 7.0f + 1.2f * sinf((float)NOW * 0.9f)) / SB_W, wob = 0.8f * sinf((float)NOW * 0.7f + 1) / LG_H; /* the picture in it lags a few units the way it moves, a tiny wobble at rest */
        gfx_liquid(t, px, py, sw, sh, sh / 2, fminf(sh * 0.6f, 36) * (1 + 0.25f * sv), 1, u0 + lag, v0 - wob, u1 + lag, v1 - wob); } } /* (a stronger lens: it is the drop of glass that moves, stronger still while it moves) */
      gfx_rrect_v(px, py, sw, sh, sh / 2, col_alpha(ACC2 | 0xff000000u, 0.46f), col_alpha(ACC | 0xff000000u, 0.36f));
      gfx_rrect_v(px, py, sw, sh, sh / 2, WHITE_A(40), WHITE_A(0)); gfx_clip_push(px, py, sw, sh); gfx_alpha_tex(gfx_radial_tex(), px + sw * 0.15f, py - sh * 0.6f, sw * 0.7f, sh, WHITE_A(55)); gfx_clip_pop();
      gfx_ring(px, py, sw, sh, sh / 2, 1.2f, WHITE_A(70)); (void)r; /* a fine bright edge: no glint sliding across a moving pill */
      gfx_offset_pop(); gfx_opacity_pop(); }
    for (int i = NAV_DISC; i <= NAV_LIB; i++) {
      int cur = chipCur(i), f = chipFocused(i); float x = topNav.chipX[i], w = topNav.chipW[i];
      if (f) gfx_offset_push(nx, ny);
      { static Tween em[3]; Tween *e = &em[i - NAV_DISC]; tween_to(e, f || (cur && !topNav.active) ? 1 : 0, 0.22f, EASE_CSS_EASE); tween_tick(e, DT); /* bold while focused, or the page shown with the focus elsewhere: crossfaded both ways */
        float ty = 50 - text_line_height(32) / 2.0f, k = e->v;
        if (k < 0.999f) text_draw(chipLabel(i), x + w / 2, ty, 32, 400, WHITE_A((int)(184 * (1 - k))), 0, 0, ALIGN_CENTER);
        if (k > 0.001f) text_draw(chipLabel(i), x + w / 2, ty, 32, 700, WHITE_A((int)(255 * k)), 0, 0, ALIGN_CENTER); }
      if (f) gfx_offset_pop();
    }
    update_pill_draw(topNav.chipX[NAV_SET]); /* under the Settings icon, which is its rounded end */
    corner_draw(NAV_SEARCH, "nav_search", RGB(0x2f, 0xd6, 0x6b), 0); /* green key: Search */
    corner_draw(NAV_SET, "nav_set", RGB(0xef, 0x3b, 0x3b), topNav.setRot.v); /* red key: Settings */
  }
  if (lift < 0) gfx_offset_pop();
  if (rv < 1) gfx_opacity_pop();
  lglass_light(0); if (!welcome_is_open()) { float ck = (1 - ma * ma * (3 - 2 * ma)) * (1 - pg); if (ck < 1) gfx_opacity_push(ck); if (lift < 0) gfx_offset_push(0, lift); clock_draw(); if (lift < 0) gfx_offset_pop(); if (ck < 1) gfx_opacity_pop(); } /* the clock: never faded in with the page (it stays where it is through signing in, the start screen and the guide); away only with the bar for a trailer or the reviews */
}
void app_clock_top(void) { if (splash_signed_in() || welcome_is_open()) { lglass_light(0); clock_draw(); } } /* over the start screen and the guide: the same clock, in its place

/* ---- the backdrop: two layers, the new one fades in over the old (never a dip to black) ---- */
#define BGW 1280
#define BGH 720
typedef struct { char url[512]; GLuint tex; float aspect; GfxTarget baked; char bakedUrl[512]; int mode, bakedMode, bakedRtl, bakedOk; } BgLayer; /* baked = the picture with its shading, made once per change */
static BgLayer bgL[2]; static int bgTop;        /* bgL[bgTop] is above */
static Timer1 bgFade; static int bgFadingIn, bgFadingOut;
static char bgCur[512], bgWant[512], bgAlt[512]; static double bgNotBefore; static int bgWaiting;
static int bgMode;                              /* 0 = the main pages' shading, 1 = a title page's */
const char *bg_current(void) { return bgCur; }
/* a change of shading is a crossfade of the same picture into a layer baked the new way (a mix of two shaded
   pictures is the mix of the shadings): no layer is baked again while the change runs (that was a full bake every
   frame for 0.3 s, the source picture reloaded for it) */
void bg_set_mode(int detail) {
  int m = detail ? 1 : 0; if (m == bgMode) return; bgMode = m;
  if (bgWaiting || !bgL[bgTop].url[0]) return; /* the next picture comes with the new shading anyway */
  snprintf(bgWant, sizeof bgWant, "%s", bgL[bgTop].url); bgAlt[0] = 0; bgNotBefore = NOW; bgWaiting = 1;
}
static void bg_fade_away(void) { if (!bgL[bgTop].url[0] && !bgL[!bgTop].url[0]) return;
  if (bgFade.on && bgFadingIn) { bgL[!bgTop].url[0] = 0; bgL[!bgTop].tex = 0; } /* the one still arriving is dropped */
  bgFadingIn = 0; bgFadingOut = 1; timer_start(&bgFade, 0.3f); }
void bg_clear(void) { bgCur[0] = 0; bgWaiting = 0; bg_fade_away(); }
void bg_set(const char *u, int delayMs, const char *alt) {
  if (!u || !*u) { if (bgCur[0] || bgWaiting) bg_clear(); return; }
  if (!strcmp(u, bgCur)) { if (bgWaiting && strcmp(bgWant, bgCur)) bgWaiting = 0; return; } /* back on the one shown (A, B, A quickly): the B still on its way must not land after (a change of shading of A itself stays) */
  if (bgWaiting && !strcmp(u, bgWant)) return;
  snprintf(bgWant, sizeof bgWant, "%s", u); snprintf(bgAlt, sizeof bgAlt, "%s", alt ? alt : ""); bgNotBefore = NOW + delayMs / 1000.0; bgWaiting = 1;
}
static unsigned bgGen; /* bumps each time a new backdrop takes over */
static void bg_show(const char *u, GLuint tex, float aspect) {
  if (bgFade.on) { /* a fade still running ends now */
    if (bgFadingOut) { bgL[bgTop].url[0] = 0; bgL[bgTop].tex = 0; bgL[!bgTop].url[0] = 0; bgL[!bgTop].tex = 0; }
    bgFade.on = 0; }
  int nx = !bgTop; /* the hidden layer takes the picture and fades in above */
  if (!bgL[bgTop].url[0] && bgL[nx].url[0]) { nx = bgTop; }
  snprintf(bgL[nx].url, sizeof bgL[nx].url, "%s", u); bgL[nx].tex = tex; bgL[nx].aspect = aspect; bgL[nx].mode = bgMode;
  bgTop = nx; bgFadingIn = 1; bgFadingOut = 0; timer_start(&bgFade, 0.3f); bgGen++;
  snprintf(bgCur, sizeof bgCur, "%s", u);
}
static int bgBakeBroken;                                  /* the driver cannot draw offscreen: draw it all every frame */
static int bg_baked_current(const BgLayer *l) { return !bgBakeBroken && l->bakedOk && !strcmp(l->bakedUrl, l->url) && l->bakedMode == l->mode && l->bakedRtl == app_rtl(); }
void bg_tick(float dt) {
  if (timer_tick(&bgFade, dt) >= 1 && !bgFade.on && (bgFadingIn || bgFadingOut)) {
    if (bgFadingOut) { bgL[0].url[0] = bgL[1].url[0] = 0; bgL[0].tex = bgL[1].tex = 0; }
    else { bgL[!bgTop].url[0] = 0; bgL[!bgTop].tex = 0; } /* the old one is dropped once the fade really ended */
    bgFadingIn = bgFadingOut = 0; }
  if (bgWaiting) {
    float aspect; img_urgent(bgWant, BGW, BGH); GLuint tex = img_get(bgWant, BGW, BGH, &aspect); /* decoded before anything asked after it */
    /* a crossfade running plays out first (cut short, its picture jumped in whole): the newest wanted follows it */
    if (tex) { if (NOW >= bgNotBefore && !(bgFade.on && bgFadingIn)) { bgWaiting = 0; bg_show(bgWant, tex, aspect); } }
    else if (img_failed(bgWant)) { if (bgAlt[0] && strcmp(bgAlt, bgWant)) { snprintf(bgWant, sizeof bgWant, "%s", bgAlt); bgAlt[0] = 0; } else { bgWaiting = 0; bgCur[0] = 0; bg_fade_away(); } }
  }
  /* the shown pictures stay asked for (kept from eviction): the player opens on the same picture at the same size, and
     a change of shading bakes from it again */
  for (int i = 0; i < 2; i++) if (bgL[i].url[0]) { GLuint t = img_get(bgL[i].url, BGW, BGH, &bgL[i].aspect); bgL[i].tex = t; }
  if (!bgL[0].url[0] && !bgL[1].url[0]) for (int i = 0; i < 2; i++) if (bgL[i].baked.fbo) { gfx_target_free(&bgL[i].baked); bgL[i].bakedOk = 0; bgL[i].bakedUrl[0] = 0; } /* nothing shown: the memory goes back */
}
/* the shading as the page draws it: made ONCE per kind (and side) as a small alpha picture in the page colour
   and stretched over each backdrop. The main pages: art in the upper part, a soft fade into the rows (solid from
   84% down), a darker left side and a soft oval vignette around (62%, 22%); a title page: the cover shows, with
   enough shade for the text on the left and the rows below. Layers of one colour stack as 1-(1-a)(1-b) */
#define VIGW 640
#define VIGH 360
static float lin_at(const float (*st)[2], int n, float t) {
  if (t <= st[0][0]) return st[0][1];
  for (int i = 0; i + 1 < n; i++) if (t <= st[i + 1][0]) { float d = st[i + 1][0] - st[i][0]; return d > 0 ? st[i][1] + (st[i + 1][1] - st[i][1]) * (t - st[i][0]) / d : st[i + 1][1]; }
  return st[n - 1][1];
}
#define VTX 16
#define VTY 9
static unsigned char vigUsed[4][VTY][VTX]; /* per tile of the shading: 1 when any texel there (or one beside it) is not clear */
/* the shading's darkness at (u, v) of the screen: the page's gradients down and across, and (on the boards) its oval */
static const float mV[][2] = { { 0, .26f }, { .10f, 0 }, { .32f, 0 }, { .46f, .16f }, { .58f, .46f }, { .68f, .74f }, { .77f, .92f }, { .84f, 1 }, { 1, 1 } };
static const float mH[][2] = { { 0, .84f }, { .32f, .5f }, { .62f, .12f }, { 1, 0 } };
static const float dV[][2] = { { 0, .18f }, { .12f, 0 }, { .5f, 0 }, { .7f, .32f }, { .86f, .7f }, { 1, .92f } };
static const float dH[][2] = { { 0, .78f }, { .34f, .42f }, { .6f, .06f }, { 1, 0 } };
static float vig_at(int detail, int rtl, float u, float v) {
  float hx = rtl ? 1 - u : u, a1, a2, a3 = 0;
  if (detail) { a1 = lin_at(dV, 6, v); a2 = lin_at(dH, 4, hx); }
  else { a1 = lin_at(mV, 9, v); a2 = lin_at(mH, 4, hx);
    float ex = (u - (rtl ? .38f : .62f)) / .85f, ey = (v - .22f) / .7f, d = sqrtf(ex * ex + ey * ey); /* the oval: 85% x 70% of the screen */
    a3 = d <= .55f ? 0 : d >= 1 ? .36f : .36f * (d - .55f) / .45f; }
  return 1 - (1 - a1) * (1 - a2) * (1 - a3);
}
static GLuint vig_tex(int detail) {
  static GLuint tex[4]; int k = detail * 2 + (app_rtl() ? 1 : 0); if (tex[k]) return tex[k];
  unsigned char *px = malloc((size_t)VIGW * VIGH); int rtl = k & 1;
  for (int y = 0; y < VIGH; y++) for (int x = 0; x < VIGW; x++) px[y * VIGW + x] = (unsigned char)(vig_at(detail, rtl, (x + .5f) / VIGW, (y + .5f) / VIGH) * 255 + .5f);
  for (int ty = 0; ty < VTY; ty++) for (int tx = 0; tx < VTX; tx++) { int any = 0; /* (a texel beside the tile counts: the filter reads across its edge) */
    for (int y = ty * VIGH / VTY - 1; y <= (ty + 1) * VIGH / VTY && !any; y++) for (int x = tx * VIGW / VTX - 1; x <= (tx + 1) * VIGW / VTX && !any; x++)
      if (y >= 0 && y < VIGH && x >= 0 && x < VIGW && px[y * VIGW + x]) any = 1;
    vigUsed[k][ty][tx] = (unsigned char)any; }
  tex[k] = gfx_tex_create(px, VIGW, VIGH, 1, 1); free(px); return tex[k];
}
/* the shading over the whole screen down to yMax: only its tiles that darken anything (the clear ones cost the GPU a
   full pass over the video for nothing), each run of them one quad; exactly what the one full quad drew */
static void vig_draw(int mode, uint32_t c, float yMax) {
  GLuint t = vig_tex(mode); int k = mode * 2 + (app_rtl() ? 1 : 0);
  for (int ty = 0; ty < VTY; ty++) { float y0 = (float)ty * SB_H / VTY, y1 = (float)(ty + 1) * SB_H / VTY; if (y0 >= yMax) break; if (y1 > yMax) y1 = yMax;
    for (int tx = 0; tx < VTX;) { if (!vigUsed[k][ty][tx]) { tx++; continue; } int e = tx; while (e < VTX && vigUsed[k][ty][e]) e++;
      float x0 = (float)tx * SB_W / VTX, x1 = (float)e * SB_W / VTX;
      gfx_alpha_tex_uv(t, x0, y0, x1 - x0, y1 - y0, c, x0 / SB_W, y0 / SB_H, x1 / SB_W, y1 / SB_H); tx = e; } }
}
static void bg_picture(const BgLayer *l, float alpha) {
  float u0, v0, u1, v1; img_cover_uv(l->aspect, SB_W / SB_H, 1, &u0, &v0, &u1, &v1);
  gfx_image(l->tex, 0, 0, SB_W, SB_H, 0, WHITE_A((int)(255 * 0.86f * alpha + 0.5f)), u0, v0, u1, v1);
}
static void bg_shading(int mode, float alpha) { vig_draw(mode, RGBA(12, 11, 17, (int)(255 * alpha + .5f)), SB_H); }
/* the layer as one opaque picture: shading a mix of two pictures is the mix of the two shaded
   pictures, so each layer carries its own shading and the crossfade stays exact */
static int bg_bake(BgLayer *l) {
  if (bgBakeBroken) return 0;
  if (bg_baked_current(l)) return 1;
  if (!l->tex) return l->bakedOk && !strcmp(l->bakedUrl, l->url); /* the picture is reloading: the last bake of it stays */
  if (!l->baked.fbo && !gfx_target_create(&l->baked, BGW, BGH)) { bgBakeBroken = 1; return 0; }
  gfx_target_begin(&l->baked, PAGE);
  bg_picture(l, 1); bg_shading(l->mode, 1);
  gfx_target_end();
  snprintf(l->bakedUrl, sizeof l->bakedUrl, "%s", l->url); l->bakedMode = l->mode; l->bakedRtl = app_rtl(); l->bakedOk = 1;
  return 1;
}
/* an opaque panel that holds still (the card menu, the streams sheet) says where it is, for the next frame: the
   backdrop is not drawn under it (hidden there; a third of a screen this TV's GPU then has to spare). Set inside
   its edges, so the first frame of its leaving never uncovers a gap */
static float occX, occY, occW, occH, occNX, occNY, occNW, occNH; static int occOn, occNOn;
void app_occluder(float x, float y, float w, float h) { occNX = x; occNY = y; occNW = w; occNH = h; occNOn = 1; }
void app_occluder_drop(void) { occOn = 0; occNOn = 0; } /* a panel starting to move: the backdrop under it is drawn again from this frame */
static void bg_baked_draw(GLuint tex, float alpha) {
  uint32_t c = WHITE_A((int)(255 * alpha + 0.5f));
  if (!occOn) { gfx_image(tex, 0, 0, SB_W, SB_H, 0, c, 0, 1, 1, 0); return; }
  float x0 = occX, y0 = occY, x1 = occX + occW, y1 = occY + occH; /* the four bands around it, the picture's own place in each */
  float r[4][4] = { { 0, 0, SB_W, y0 }, { 0, y1, SB_W, SB_H }, { 0, y0, x0, y1 }, { x1, y0, SB_W, y1 } };
  for (int i = 0; i < 4; i++) { float a = r[i][0], b = r[i][1], e = r[i][2], f = r[i][3]; if (e - a <= 0 || f - b <= 0) continue;
    gfx_image(tex, a, b, e - a, f - b, 0, c, a / SB_W, 1 - b / SB_H, e / SB_W, 1 - f / SB_H); }
}
static void bg_layer_draw(BgLayer *l, float alpha) {
  if (!l->url[0]) return;
  if (bg_bake(l)) { bg_baked_draw(l->baked.tex, alpha); return; }
  if (l->tex) { bg_picture(l, alpha); bg_shading(l->mode, alpha); } /* no offscreen drawing: shaded as drawn */
}
/* the picture under the sheets' glass: the shown layer as it is drawn (baked with its shading), upside down */
unsigned bg_glass_tex(float *alpha) {
  BgLayer *l = &bgL[bgTop]; if (!l->url[0]) return 0;
  float k = bgFade.on ? 1 - powf(1 - timer_x(&bgFade), 2) : 1; if (alpha) *alpha = bgFadingOut ? 1 - k : 1;
  return bg_baked_current(l) || (l->bakedOk && !strcmp(l->bakedUrl, l->url)) ? l->baked.tex : 0;
}
unsigned app_bg_gen(void) { return bgGen; }
int app_bg_pending(void) { return bgWaiting; } /* a new backdrop is on its way (asked, not shown yet) */
void bg_draw(void) {
  float k = bgFade.on ? ease_apply(EASE_CSS_EASEOUT, timer_x(&bgFade)) : 1;
  /* first launch: the backdrop fades up after the start screen too (0.7 s, from 50 ms in) */
  float rv = ease_apply(EASE_CSS_EASE, app_reveal_x(0.05f, 0.7f));
  /* the background trailer plays on the video plane under the window: its shading is drawn over it, the page colour
     below its box, and the picture over both fades off it while the whole background pushes in 2.5% */
  int under = btr_under(); float cov = under ? btr_cover() : 1, z = btr_zoom();
  if (under) { gfx_rect(0, 809, SB_W, SB_H - 809, PAGE); vig_draw(0, PAGE, 809); } /* (the shading in the same colour over the opaque foot adds nothing: it stops there) */
  if (under && !app_is_board(SCR)) gfx_rect(0, 0, SB_W, SB_H, col_alpha(PAGE | 0xff000000u, cov)); /* on a page with no backdrop (Settings, Search): its own colour comes over the trailer as it gives way */
  float a = rv * cov; if (a <= 0.001f) return;
  if (z != 1) gfx_scale_push(z, z, SB_W / 2, SB_H * 0.37f);
  if (under && btr_hold_gen() && bgGen == btr_hold_gen()) a = 0; /* a key stopped the trailer: its own backdrop never comes back, its last frame stays until the next title's */
  if (a <= 0.001f) { if (z != 1) gfx_scale_pop(); return; }
  if (under && btr_hold_gen()) bg_layer_draw(&bgL[bgTop], a); /* over the trailer's frame: the new one alone, by the cover's fade */
  else if (bgFadingIn && bgFade.on) { bg_layer_draw(&bgL[!bgTop], a); bg_layer_draw(&bgL[bgTop], k * a); }
  else if (bgFadingOut && bgFade.on) { bg_layer_draw(&bgL[!bgTop], (1 - k) * a); bg_layer_draw(&bgL[bgTop], (1 - k) * a); }
  else bg_layer_draw(&bgL[bgTop], a);
  if (z != 1) gfx_scale_pop();
}
/* the window is cleared transparent while the background trailer shows through it */
int app_bg_video(void) { return btr_under() && (!player_open() || btr_handing()); } /* (and while the player's art comes over it) */

/* ---- screens, keys ---- */
static double lastNav; static int curKey; /* the trailer's own idleness: OK (a tap, a hold) neither counts nor stops it */
static void idle_touch(void) { lastIn = NOW; if (curKey == K_OK) return; lastNav = NOW; btr_key(curKey >= K_LEFT && curKey <= K_DOWN); } /* any other key stops the background trailer at once */
float app_trailer_idle(void) { return (float)(NOW - lastNav); }
static void idle_touch_by(const char *why) { if (bgt_state() == BGT_LOAD || bgt_state() == BGT_SHOW) fprintf(stderr, "[btr] input: %s\n", why); idle_touch(); }
void app_touch(void) { idle_touch(); }
/* the page Settings or Search was opened from: its colour key pressed again there goes back to it */
static Scr colorFrom; static int colorFromSet;
void app_go(Scr s, const void *params, int noStack) {
  idle_touch();
  if (!noStack && SCR != s) { float ox, oy; int has = 1; if (navf.kind == NAV_ZOOM) { ox = navf.x; oy = navf.y; } else has = scr_origin(&ox, &oy); /* nothing focused: a plain fade in, Back toward the middle */
    if (nback == 8) { memmove(backStack, backStack + 1, 7 * sizeof *backStack); memmove(backX, backX + 1, 7 * sizeof *backX); memmove(backY, backY + 1, 7 * sizeof *backY); nback = 7; }
    backX[nback] = ox; backY[nback] = oy; backStack[nback++] = SCR;
    if (navf.kind == NAV_NONE && has && (s == SCR_DETAIL || s == SCR_SIM)) { navf.kind = NAV_ZOOM; navf.x = ox; navf.y = oy; } }
  topNav.active = 0;
  Scr old = SCR;
  if ((s == SCR_SET || s == SCR_SEARCH) && old != SCR_SET && old != SCR_SEARCH && old != SCR_LOGIN) { colorFrom = old; colorFromSet = 1; } /* where Settings / Search were opened from */
  xoutT.on = 0; if (old != s && app_splash_done && navf.kind == NAV_BACKZ && screens[old] && screens[old]->out) { xoutScr = old; timer_start(&xoutT, XOUT_DUR); }
  int shared = (old == SCR_HOME || old == SCR_DISC) && (s == SCR_HOME || s == SCR_DISC);
  /* every switch is a hand-over (no screen pops in), signing out to the sign-in page too; Home and Discover share one
     layer and hand over on their own; signing in restarts behind the start screen */
  int xf = app_splash_done && old != s && old != SCR_LOGIN && !shared;
  { int tab = (old == SCR_HOME || old == SCR_DISC || old == SCR_LIB) && (s == SCR_HOME || s == SCR_DISC || s == SCR_LIB);
    if (old != s && app_splash_done && !tab) sx_begin(0); } /* page to page, the same logo, poster or words move across (never between the tabs) */
  if (screens[old] && screens[old]->leave && old != s) screens[old]->leave();
  SCR = s;
  if (s == SCR_HOME || s == SCR_LIB) sb_sync_soon(0);
  if (s != SCR_DETAIL) accent_scope(NULL, NULL, NULL); /* the title page sets its own cover colour */
  bg_set_mode(s == SCR_DETAIL);
  if (s == SCR_LOGIN || s == SCR_SEARCH || s == SCR_SET) bg_clear(); /* a title's cover never stays behind them */
  if (getenv("SB_NAVLOG")) fprintf(stderr, "[nav] %d -> %d xf %d shared %d splash %d at %.3f\n", old, s, xf, shared, app_splash_done, NOW);
  if (xf) { xinLib = s == SCR_LIB; timer_start(&xin, s == SCR_LIB ? 0.45f : 0.38f); xmv = navf; if (xmv.kind) timer_start(&xmvT, xmv.kind == NAV_SLIDE ? 0.45f : 0.4f); else xmvT.on = 0; }
  navf.kind = NAV_NONE;
  if (screens[s] && screens[s]->enter) screens[s]->enter(params);
  text_gc();
  topnav_paint();
}
void app_back(void) {
  if (topNav.want >= 0) { topnav_flush(1); Scr cur = (SCR == SCR_DETAIL || SCR == SCR_SIM) ? SCR_HOME : SCR; for (int j = 0; j < 5; j++) if (chipScr(j) == cur) topNav.idx = j; topnav_paint(); return; } /* Back while the pill travels: it returns */
  if (app_overlay) { app_overlay->key(K_BACK); return; }
  if (SCR == SCR_LOGIN) { if (screens[SCR] && screens[SCR]->key) screens[SCR]->key(K_BACK); return; }
  if (screens[SCR] && screens[SCR]->key && screens[SCR]->key(K_BACK)) return; /* the screen stepped back inside itself */
  int ci = -1; for (int j = 0; j < 5; j++) if (chipScr(j) == SCR) ci = j;
  if (topNav.active) { if (SCR != SCR_HOME) { nav_slide(NAV_HOME >= ci ? 1 : -1); topNav.idx = NAV_HOME; app_go(SCR_HOME, NULL, 1); topNav.active = 0; topnav_paint(); } else exit_ask(); return; }
  if (nback) { Scr prev = backStack[--nback]; navf.kind = NAV_BACKZ; navf.x = backX[nback]; navf.y = backY[nback]; app_go(prev, NULL, 1); return; }
  if (ci >= 0) { topnav_focus(); return; } /* inside a tab (Home, Discover, Library, Search, Settings): Back goes up to the tab bar first; from there, Home, then the exit question */
  if (SCR != SCR_HOME) { navf.kind = NAV_BACKZ; navf.x = SB_W / 2; navf.y = SB_H / 2; topNav.idx = NAV_HOME; app_go(SCR_HOME, NULL, 1); return; }
  exit_ask();
}
/* an addon's link to somewhere else (a stream's externalUrl, a meta link): the app's own pages for stremio:///detail
   links, a code to open it on the phone for everything else (the TV has no browser of ours) */
void app_open_external(const char *url, const char *name) {
  if (!url || !*url) return;
  if (!strncmp(url, "stremio:///detail/", 18)) { char t[300]; snprintf(t, sizeof t, "%s", url + 18); char *ty = t, *id = strchr(t, '/'); if (id) { *id++ = 0; char *vid = strchr(id, '/'); if (vid) *vid++ = 0;
      DetailParams p; memset(&p, 0, sizeof p); snprintf(p.it.type, sizeof p.it.type, "%s", ty); snprintf(p.it.id, sizeof p.it.id, "%s", id); if (vid && *vid) { snprintf(p.it.videoId, sizeof p.it.videoId, "%s", vid); p.openStreams = 1; }
      if (*ty && *id) { app_go(SCR_DETAIL, &p, 0); return; } } }
  char nm[120]; snprintf(nm, sizeof nm, "%.*s", (int)strcspn(name ? name : "", "\n"), name ? name : "");
  QrCardOpts o; memset(&o, 0, sizeof o); o.title = T("Open on another device"); o.sub = nm[0] ? nm : NULL; o.url = url; qrcard_show(&o);
}
void app_play(const Stream *s, const char *type, const char *id, const char *vid, int fromStart);
void app_open_item(const Item *it) {
  if (it->cw) { Stream st; if (sb_resume_stream(it->id, it->videoId, &st)) { app_play(&st, it->type, it->id, it->videoId, 0); return; } }
  DetailParams p; memset(&p, 0, sizeof p); p.it = *it; app_go(SCR_DETAIL, &p, 0);
}
/* long-press OK on any card opens its menu; a short tap does its normal action */
#define HOLD_MS 550
static struct { int down, menu; Item item; double at; int has; } okHold; static int menuGuard;
float app_hold_progress(void) { if (!okHold.down || okHold.menu) return 0; float x = (float)((NOW - okHold.at) * 1000 - 220) / (HOLD_MS - 220); return x < 0 ? 0 : x > 1 ? 1 : x; }
static void ok_up(void) {
  menuGuard = 0; int wasDown = okHold.down, menu = okHold.menu, has = okHold.has; Item it = okHold.item;
  memset(&okHold, 0, sizeof okHold);
  if (wasDown && !menu && has) { /* short press = the card's normal action */
    if (screens[SCR] && screens[SCR]->open) screens[SCR]->open(&it); else app_open_item(&it); }
}
static void key_core(int k, int down, int repeat);
/* ---- the Magic Remote pointer. How it behaves (timings as measured on LG TVs):
   pointing moves the same highlight the arrows move and the screen follows as for the arrows; the brake: while
   the thing under the pointer is still moving the pointer must stand still 150 ms first. A click is OK pressed
   and released (held on a card: its menu); a few targets act on release (a tab, a chip, the seek bar, beside a
   sheet = close it); a sheet's body absorbs a click. A click and an OK key within 150 ms are one press. The
   wheel is Up/Down, one step per notch (70 ms apart at most). An arrow key puts the pointer away; it comes
   back only with a real movement: nothing in the first 350 ms, then 32 px from where the hand started again. */
#define PTR_SETTLE 0.15
#define PTR_DEDUPE 0.15
#define PTR_WHEEL 0.07
#define PTR_ARROW_WIN 0.35
#define PTR_ARROW_DIST 32
#define PTR_ARROW_PAUSE 0.3
static struct { int on; float x, y; Hit hv; int hasHv; float hx, hy; double movedAt; int down, downAct; Hit act; double okKeyAt, clickAt, wheelAt; int eatUp, eatOk, arrowHid; double arrowAt; int anchor; float ax, ay; double tremAt; int dirty; } PTR;
static void ptr_show(void) { PTR.arrowHid = 0; if (PTR.on) return; PTR.on = 1; PTR.hasHv = 0; }
static void ptr_hide(void) { if (!PTR.on) return; PTR.on = 0; PTR.hasHv = 0; }
static int ptr_tremor(double now) { if (!PTR.arrowHid) return 0; if (now - PTR.arrowAt < PTR_ARROW_WIN) return 1;
  if (!PTR.anchor || now - PTR.tremAt > PTR_ARROW_PAUSE) { PTR.anchor = 1; PTR.ax = PTR.x; PTR.ay = PTR.y; } PTR.tremAt = now; float dx = PTR.x - PTR.ax, dy = PTR.y - PTR.ay; return dx * dx + dy * dy < PTR_ARROW_DIST * PTR_ARROW_DIST; }
static void ptr_nav_back(void) { if (topNav.want >= 0) return; Scr cur = (SCR == SCR_DETAIL || SCR == SCR_SIM) ? SCR_HOME : SCR; for (int j = 2; j < 5; j++) if (chipScr(j) == cur && topNav.idx != j) { topNav.idx = j; topnav_paint(); } }
static void ptr_keep(const Hit *h) { PTR.hv = *h; PTR.hasHv = 1; PTR.hx = h->x + h->w / 2; PTR.hy = h->y + h->h / 2; }
/* the brake: the element the pointer last focused moved (a slide, a scroll) or went away -> note when */
static void ptr_settle(double now) { if (!PTR.hasHv) return; const Hit *h = hit_at(PTR.hx, PTR.hy); int found = 0; float cx = PTR.hx, cy = PTR.hy;
  /* the element is looked up by identity among this frame's rectangles */
  extern const Hit *hit_find(const Hit *like); const Hit *e = hit_find(&PTR.hv); (void)h; if (e) { found = 1; cx = e->x + e->w / 2; cy = e->y + e->h / 2; }
  if (!found) { PTR.movedAt = now; PTR.hasHv = 0; return; } if (fabsf(cx - PTR.hx) > 3 || fabsf(cy - PTR.hy) > 3) PTR.movedAt = now; PTR.hx = cx; PTR.hy = cy; }
static void ptr_frame(void) { if (!PTR.on || !PTR.dirty) return; PTR.dirty = 0; double now = NOW; if (player_open()) { extern void player_ptr(int type, float x, float y); player_ptr(0, PTR.x, PTR.y); return; }
  if (PTR.down) return; /* pressed: a hand's tremble must not move the press onto the next card */
  ptr_settle(now); const Hit *d = hit_at(PTR.x, PTR.y);
  if (topNav.active && !(d && d->nav)) ptr_nav_back();
  if (!d || !d->focus || (PTR.hasHv && hit_same(d, &PTR.hv))) return;
  if (PTR.movedAt && now - PTR.movedAt < PTR_SETTLE) { PTR.dirty = 1; return; } /* the page is still moving under the pointer */
  d->focus(d->a, d->b); ptr_keep(d); }
void app_pointer_move(float x, float y) { if (x == PTR.x && y == PTR.y) return; PTR.x = x; PTR.y = y; if (ptr_tremor(NOW)) return; ptr_show(); idle_touch_by("pointer moved"); PTR.dirty = 1; }
void app_pointer_button(int down, float x, float y) {
  double now = NOW; PTR.x = x; PTR.y = y;
  if (saver_on()) return; /* the screensaver covers the page: nothing under it is pressed */
  if (down) { ptr_show(); idle_touch();
    if (PTR.okKeyAt && now - PTR.okKeyAt < PTR_DEDUPE) { PTR.eatUp = 1; return; } /* the OK key of this same press came first */
    PTR.clickAt = now;
    if (player_open()) { extern void player_ptr(int type, float x, float y); PTR.down = 2; player_ptr(1, x, y); return; }
    const Hit *d = hit_at(x, y); if (!d) return;
    if (d->act) { PTR.down = 3; PTR.act = *d; return; }
    if (d->shield || !d->focus) return;
    d->focus(d->a, d->b); ptr_keep(d); PTR.down = 1; key_core(K_OK, 1, 0); /* OK pressed (held: the card's menu) */
    return; }
  if (PTR.eatUp) { PTR.eatUp = 0; return; }
  int dn = PTR.down; PTR.down = 0; if (!dn) return;
  if (dn == 2) { extern void player_ptr(int type, float x, float y); player_ptr(2, x, y); return; }
  if (dn == 1) { key_core(K_OK, 0, 0); return; }
  const Hit *d = hit_at(x, y); if (d && hit_same(d, &PTR.act)) PTR.act.act(PTR.act.a, PTR.act.b); /* released on it: its own action */
}
/* the wheel = the Up/Down arrows (Left/Right for a sideways wheel), one per notch; it leaves the pointer out.
   In the player it is the volume instead (shown only while changing): Up louder, Down quieter */
void app_pointer_wheel(float dx, float dy) { double now = NOW; if (saver_on()) return; if (now - PTR.wheelAt < PTR_WHEEL || (!dx && !dy)) return; PTR.wheelAt = now; idle_touch();
  if (player_open() && dy) { extern void player_wheel(float dy); player_wheel(dy); return; }
  int k = dy > 0 ? K_UP : dy < 0 ? K_DOWN : dx > 0 ? K_RIGHT : K_LEFT; key_core(app_mirror_key(k), 1, 0); key_core(k, 0, 0); }
void app_pointer_system(int show) { if (!show) { ptr_hide(); return; } if (PTR.arrowHid) return; ptr_show(); }
void app_key(int k, int down, int repeat) {
  if (k == K_CURSOR_SHOW || k == K_CURSOR_HIDE) { if (down) app_pointer_system(k == K_CURSOR_SHOW); return; } /* a notice from the TV, not a key */
  if (down && k == K_OK && !repeat) { if (PTR.eatOk || PTR.down || (PTR.clickAt && NOW - PTR.clickAt < PTR_DEDUPE)) { PTR.eatOk = 1; return; } PTR.okKeyAt = NOW; } /* the click of this same press came first */
  if (!down && k == K_OK && PTR.eatOk) { PTR.eatOk = 0; return; }
  if (down && saver_on() && k != K_CURSOR_SHOW && k != K_CURSOR_HIDE) { saver_stop(); idle_touch(); if (k == K_OK) PTR.eatOk = 1; return; } /* the key that wakes the screensaver does nothing else */
  if (down && k >= K_LEFT && k <= K_DOWN) { ptr_hide(); PTR.arrowHid = 1; PTR.arrowAt = NOW; PTR.anchor = 0; } /* every arrow re-arms it, hidden or not */
  key_core(k, down, repeat);
}
/* where the focus is, as far as can be told from outside: the screen, the open sheet, the top bar, the centre of
   the focused element (the same point the page transitions grow from) */
typedef struct { int scr, nav, navIdx, known; const void *ov; float x, y; } FocusSig;
static void focus_sig(FocusSig *f) {
  memset(f, 0, sizeof *f); f->scr = SCR; f->ov = app_overlay; f->nav = topNav.active; f->navIdx = topNav.idx;
  Screen *s = app_overlay ? app_overlay : screens[SCR];
  if (f->nav) f->known = 1; else if (s && s->origin) f->known = s->origin(&f->x, &f->y) || !app_overlay;
}
static void key_dispatch(int k, int repeat);
static int keyHeld; /* the key being handled is a repeat of a held one */
int app_key_held(void) { return keyHeld; }
static int okDown; /* OK is down (a key or the pointer's button) */
int app_ok_down(void) { return okDown; }
float app_hold_k(double at) { float x = (float)((NOW - at) * 1000 - 220) / (HOLD_MS - 220); return x < 0 ? 0 : x > 1 ? 1 : x; }
static void key_core(int k, int down, int repeat) {
  if (down) keyHeld = repeat != 0;
  if (k == K_OK) okDown = down;
  if (player_open()) { if (!(k == K_OK && repeat && down)) player_key(k, down); return; }
  if (!down) { if (k == K_OK) ok_up(); return; }
  /* the pointer appearing or hiding is the TV's own doing (it hides by itself, and shows when the remote is
     picked up): not input that stops the background trailer */
  if (k != K_CURSOR_SHOW && k != K_CURSOR_HIDE) { char why[32]; snprintf(why, sizeof why, "key %d", k); curKey = k; idle_touch_by(why); }
  k = app_mirror_key(k); /* mirrored layout: Left moves forward (every screen sees the logical direction) */
  if (k >= K_LEFT && k <= K_DOWN) { /* an arrow: a tap when the focus moved (a screen's own sound, e.g. a filter, wins) */
    FocusSig f0, f1; focus_sig(&f0); double t0 = NOW; key_dispatch(k, repeat); focus_sig(&f1);
    /* the glows lean the way it went: a tab sweeps them across, a step only leans them (held keys, less) */
    if (f1.nav && f1.navIdx != f0.navIdx) orbs_pulse(f1.navIdx > f0.navIdx ? ORB_TAB_R : ORB_TAB_L, 0.8f);
    else orbs_pulse(k == K_LEFT ? ORB_LEFT : k == K_RIGHT ? ORB_RIGHT : k == K_UP ? ORB_UP : ORB_DOWN, repeat ? 0.15f : memcmp(&f0, &f1, sizeof f0) ? 0.35f : 0.2f);
    if (sound_last() < t0 - 1e-6 && memcmp(&f0, &f1, sizeof f0)) sound_play(f1.nav && f1.navIdx != f0.navIdx ? SND_TAB : SND_FOCUS);
    else if (sound_last() < t0 - 1e-6 && !f1.known) sound_play(SND_FOCUS); /* a sheet that cannot say where its focus is: every move */
    return; }
  key_dispatch(k, repeat);
}
static void key_dispatch(int k, int repeat) {
  if (k == K_OK && repeat) { return; } /* repeats of a held OK: the hold timer decides */
  if (k == K_OK || k == K_BACK) sound_play(SND_FOCUS); /* a press: the same tap as a move */
  if (k == K_OK) orbs_pulse(ORB_OK, 0.6f); else if (k == K_BACK) orbs_pulse(ORB_BACK, 0.6f);
  else if (k == K_RED || k == K_GREEN) { int back = SCR == (k == K_RED ? SCR_SET : SCR_SEARCH) && colorFromSet; orbs_pulse(back ? ORB_BACK : k == K_RED ? ORB_RED : ORB_GREEN, back ? 0.8f : 1); } /* (its own page: they breathe out as it leaves) */
  if (app_overlay) { if (k == K_OK && menuGuard) return; app_overlay->key(k); return; }
  if (k == K_BACK) { app_back(); return; }
  if (k == K_EXIT) { app_quit(); return; }
  if ((k == K_RED || k == K_GREEN) && SCR != SCR_LOGIN) { if (k == K_RED && update_notice_key()) { sound_play(SND_TAB); return; } /* a notice out: red takes you to what it is about */
    int t = k == K_RED ? NAV_SET : NAV_SEARCH; sound_play(SND_TAB);
    if (SCR == chipScr(t) && colorFromSet) { Scr p = colorFrom; colorFromSet = 0; int ci = -1; for (int j = 0; j < 5; j++) if (chipScr(j) == p) ci = j; /* pressed on its own page: back where it was opened from */
      topnav_flush(1); navf.kind = NAV_BACKZ; navf.x = topNav.chipX[t] + topNav.chipW[t] / 2; navf.y = 50; topNav.idx = ci >= 0 ? ci : NAV_HOME;
      app_go(p, NULL, 1); topNav.active = 0; topnav_paint(); return; } topnav_flush(1); if (chipScr(t) != SCR) { topNav.idx = -1; topnav_switch(t); } topNav.idx = t; topnav_enter_content(); return; } /* the remote's colour keys: red Settings, green Search */
  if (topNav.active) { topnav_key(k); return; }
  if (k == K_OK && screens[SCR] && screens[SCR]->focused) { const Item *fi = screens[SCR]->focused();
    if (fi) { if (!okHold.down) { okHold.down = 1; okHold.menu = 0; okHold.item = *fi; okHold.has = 1; okHold.at = NOW; } return; } }
  if (screens[SCR] && screens[SCR]->key) screens[SCR]->key(k);
}
void app_text(const char *utf8) { idle_touch(); if (app_overlay && app_overlay->text) app_overlay->text(utf8); else if (screens[SCR] && screens[SCR]->text) screens[SCR]->text(utf8); }
void app_refresh_boards(void) { extern void board_invalidate(void); extern void board_refresh_quiet(void); board_invalidate(); if (app_is_board(SCR)) board_refresh_quiet(); } /* in place: no row replays its entrance */
void app_init(void) {
  vig_tex(0); vig_tex(1); /* both shadings made at start: the first title page would make its own mid-zoom */
  topNav.want = -1; NOW = SDL_GetTicks() / 1000.0; lastIn = NOW; lastNav = NOW; bgMode = 0; tween_set(&toastA, 0);
  accent_apply(); accent_tick(0); sync_init(); btr_init(); castlink_start();
}
static void amb_bake(void);
void app_tick(float dt) {
  DT = dt; NOW = SDL_GetTicks() / 1000.0; FRAME++;
  ptr_frame();
  { static int swelled; if (app_hold_progress() > 0 && !swelled) { swelled = 1; sound_play(SND_HOLD); } else if (!okHold.down) swelled = 0; } /* holding OK: a low swell as the line starts round */
  if (okHold.down && !okHold.menu && (NOW - okHold.at) * 1000 >= HOLD_MS) { okHold.menu = 1; menuGuard = 1; orbs_pulse(ORB_HOLD, 1); if (app_card_menu_hook) app_card_menu_hook(&okHold.item); }
  timer_tick(&xin, dt); timer_tick(&xmvT, dt); timer_tick(&xoutT, dt);
  saver_tick(dt);
  accent_tick(dt); tint_tick(); toast_tick(dt); clock_tick(); if (SCR == SCR_LOGIN || splash_signed_in()) login_glow_bake(); /* (the sign-in's glass: the glows under it) */ amb_bake(); topnav_tick(dt); bg_tick(dt); lg_tick(); sync_tick(); update_tick(dt); best_tick();
  curKey = 0; /* (a page switch later in the frame is not a key's) */
  btr_tick(dt); welcome_tick(); lift_tick(dt); tabs_warm_tick();
  { static int greeted; if (!greeted && app_splash_done) { greeted = 1; sound_play(SND_START); } } /* start-up: one very faint sound */
  { static int wasP; int po = player_open(); if (po != wasP) { wasP = po; if (po) sound_play(SND_PLAY); sound_quiet(po); } } /* into the player: one faint sound, then the film has the audio alone */
  castlink_tick(dt); /* (a phone drives the film too: before the app rests under it) */
  player_tick(dt); if (player_open() && player_cover() >= 0.999f) return; /* the app rests while the film plays (not while the player fades over it or away from it) */
  if (screens[SCR] && screens[SCR]->tick) screens[SCR]->tick(dt);
  /* every overlay that has shown keeps ticking after it lets go (its closing motion plays out, and its state settles:
     frozen half-closed, the next opening was taken for a reopening mid-close and skipped its entrance) */
  if (app_overlay) { int k = 0; for (; k < nseenOv; k++) if (seenOv[k] == app_overlay) break; if (k == nseenOv && nseenOv < 8) seenOv[nseenOv++] = app_overlay; }
  for (int k = 0; k < nseenOv; k++) if (seenOv[k] != app_overlay && seenOv[k]->tick) seenOv[k]->tick(dt);
  if (app_overlay && app_overlay->tick) app_overlay->tick(dt);
}
/* the purple glass behind the sign-in page and Search (the page's faded mark lies under its glass there: never seen, not drawn) */
static Tween glassA; static char glassUrl[1100];
static void glass_draw(void) {
  static int noGlass = -1; if (noGlass < 0) noGlass = getenv("SB_NOGLASS") != NULL; /* read once */
  if (noGlass) return; /* an experiment: the page without its full-screen layers */
  int wantGlass = SCR == SCR_LOGIN; /* (Search shares Settings' plain base and its two glows) */
  tween_to(&glassA, wantGlass ? 1 : 0, 0.4f, EASE_OUT); tween_tick(&glassA, DT);
  if (!glassUrl[0]) { extern const char *plat_asset_dir(void); snprintf(glassUrl, sizeof glassUrl, "file://%s/splashbg.jpg", plat_asset_dir()); }
  if (glassA.v > 0.001f) { float asp; GLuint t = img_get(glassUrl, 1920, 1080, &asp); if (t) { float u0, v0, u1, v1; img_cover_uv(asp, SB_W / SB_H, 0, &u0, &v0, &u1, &v1); gfx_image(t, 0, 0, SB_W, SB_H, 0, WHITE_A((int)(255 * glassA.v)), u0, v0, u1, v1); } }
}
/* Settings' and Search's background, and the guide's: the two glows (ui/orbs.c), on their springs */
/* the guide over Home (welcome.c) takes the glows for its page (as Settings has them) and draws them over its own
   base: 1 while it is up, 2 while it fades out (they go with it) */
static int ambGuide; static Tween ambA; static int ambIn;
void app_ambient_guide(int mode) { ambGuide = mode; }
/* the phone's words landed: the glows surge in towards the middle and spring back */
void app_glow_kick(void) { orbs_pulse(ORB_PHONE, 1); }
static void ambient_draw(void) {
  int srch = SCR == SCR_SEARCH && !ambGuide, want = SCR == SCR_SET || srch || ambGuide == 1; Orbs *o = orbs_app();
  if (want && !ambIn) { ambIn = 1; if (ambA.v < 0.02f) orbs_place(o, srch, 4); } /* arriving: out of the middle, wide, to their corners */
  if (!want) ambIn = 0;
  tween_to(&ambA, want ? 1 : 0, want ? 0.45f : 0.42f, EASE_CSS_EASE); tween_tick(&ambA, DT);
  if (ambA.v <= 0.001f) return;
  /* coming in or going: quick and plain to see; between the two pages: a slow glide; going, they rush in towards the
     middle and spread as they fade, melting into the next page's colours */
  orbs_step(o, (float)DT, srch, want ? 1 : 5, want ? (ambA.v < 0.98f ? 7.5f : 2.6f) : 8.0f);
  orbs_draw(o, ambA.v);
}
/* the glows, small, for the glass over the pages that have them (Settings, Search, the guide): made in the tick, they move */
static GfxTarget ambT; static int ambTOk = -1, ambTFresh;
static int amb_glass_on(void) { return welcome_is_open() || ((SCR == SCR_SET || SCR == SCR_SEARCH) && ambA.v > 0.001f); }
static void amb_bake(void) {
  { static int no = -1; if (no < 0) no = getenv("SB_NOAMB") != NULL; if (no) { ambTFresh = 0; return; } } /* (development: measuring) */
  if (!amb_glass_on()) { ambTFresh = 0; return; }
  if (ambTOk < 0) ambTOk = gfx_target_create(&ambT, 384, 216);
  if (ambTFresh && FRAME % 4) return; /* the glows drift slowly: made again every fourth frame (each bake is a switch of the GPU's target, costly on these chips) */
  if (!ambTOk) return; gfx_target_begin(&ambT, PAGE | 0xff000000u); if (welcome_is_open()) login_backdrop_draw(); else orbs_draw(orbs_app(), ambA.v); gfx_target_end(); /* (the guide has the sign-in's glows) */ ambTFresh = 1;
}
unsigned int app_ambient_glass_tex(void) { return ambTOk > 0 && ambTFresh ? ambT.tex : 0; }
void app_ambient_draw(void) { if (ambGuide) ambient_draw(); } /* (the guide's, once a frame) */
/* the top bar's glass: what lies under the bar (the backdrop) drawn at an
   eighth of its size into a strip and lightly blurred, made again when the backdrop changes and each frame while it fades, never else */
static GfxTarget lgS[3]; static int lgOk; static unsigned lgGen = ~0u; static int lgScr = -1; static double lgAt;
/* a backdrop layer's top strip, at a: its own picture unshaded (else its baked one, upside down); never a bake here (a
   second offscreen picture inside this one) */
static void lg_layer(BgLayer *l, float a) {
  if (a <= 0.002f || !l->url[0]) return;
  if (l->bakedOk && !strcmp(l->bakedUrl, l->url)) { gfx_image(l->baked.tex, 0, 0, SB_W, LG_H, 0, WHITE_A((int)(255 * a + 0.5f)), 0, 1, 1, 1 - LG_H / SB_H); return; } /* the picture as it shows, shading and all: the glass is as dark as what is behind it */
  if (l->tex && l->aspect > 0) { float u0, v0, u1, v1; img_cover_uv(l->aspect, SB_W / SB_H, 0, &u0, &v0, &u1, &v1); /* the picture itself, unshaded: the glass takes the backdrop's own colours (the page darkens its top for the words) */
    gfx_image(l->tex, 0, 0, SB_W, LG_H, 0, WHITE_A((int)(255 * a + 0.5f)), u0, v0, u1, v0 + (v1 - v0) * (LG_H / SB_H)); return; }
  if (!l->bakedOk || strcmp(l->bakedUrl, l->url)) return;
  gfx_image(l->baked.tex, 0, 0, SB_W, LG_H, 0, WHITE_A((int)(255 * a + 0.5f)), 0, 1, 1, 1 - LG_H / SB_H); }
static void lg_bake(void) {
  int W = 960, H = 56, bw = 480, bh = 28; /* half size, lightly blurred: the glass shows the picture, softened, so its lens has something to bend */
  int sw[3] = { W, bw, bw }, sh[3] = { H, bh, bh };
  for (int i = 0; i < 3; i++) if (!lgS[i].fbo && !gfx_target_create(&lgS[i], sw[i], sh[i])) { lgOk = 0; return; }
  float k = bgFade.on ? ease_apply(EASE_CSS_EASEOUT, timer_x(&bgFade)) : 1, rv = ease_apply(EASE_CSS_EASE, app_reveal_x(0.05f, 0.7f));
  gfx_target_begin(&lgS[0], PAGE); /* the backdrop's two layers as bg_draw blends them */
  if (bgFadingIn && bgFade.on) { lg_layer(&bgL[!bgTop], rv); lg_layer(&bgL[bgTop], k * rv); }
  else if (bgFadingOut && bgFade.on) { lg_layer(&bgL[!bgTop], (1 - k) * rv); lg_layer(&bgL[bgTop], (1 - k) * rv); }
  else lg_layer(&bgL[bgTop], rv);
  gfx_target_end();
  gfx_target_begin(&lgS[1], PAGE); gfx_image(lgS[0].tex, 0, 0, SB_W, LG_H, 0, WHITE_A(255), 0, 1, 1, 0); gfx_target_end();
  gfx_blur_pass(&lgS[2], lgS[1].tex, bw, bh, 0, 1.0f); gfx_blur_pass(&lgS[1], lgS[2].tex, bw, bh, 1, 1.0f);
  lgOk = 1; lgAt = NOW;
}
static void lg_tick(void) {
  if (SCR == SCR_LOGIN || player_open()) return;
  int want = !lgOk || lgGen != bgGen || lgScr != (int)SCR || bgFade.on || app_reveal_x(0.05f, 0.7f) < 1; /* (each frame while the backdrop fades: the glass fades with it) */
  if (want) { lgGen = bgGen; lgScr = (int)SCR; lg_bake(); }
}
/* the strip's part under a box: its texture and uv (0 when there is none yet) */
static GLuint lg_tex(float x, float y, float w, float h, float *u0, float *v0, float *u1, float *v1) {
  if (SCR == SCR_LOGIN || splash_signed_in()) { GLuint g = login_glow_tex(); *u0 = x / SB_W; *u1 = (x + w) / SB_W; *v0 = 1 - y / SB_H; *v1 = 1 - (y + h) / SB_H; return g; } /* signing in: the glows */
  if (welcome_is_open() || ((SCR == SCR_SET || SCR == SCR_SEARCH) && !app_overlay)) { GLuint g = app_ambient_glass_tex(); if (g) { *u0 = x / SB_W; *u1 = (x + w) / SB_W; *v0 = 1 - y / SB_H; *v1 = 1 - (y + h) / SB_H; return g; } } /* the glows' pages: the glows */
  if (!lgOk) return 0; *u0 = x / SB_W; *u1 = (x + w) / SB_W; *v0 = 1 - y / LG_H; *v1 = 1 - (y + h) / LG_H; return lgS[1].tex; }
static void page_draw(void) {
  if (welcome_covers() && app_overlay && app_overlay->draw) { app_overlay->draw(); return; } /* the guide's page hides all of it */
  static const char *skip = (const char *)1; if (skip == (const char *)1) skip = getenv("SB_SKIP"); /* development: SB_SKIP=bg,bar,screen,amb leaves parts out, to time what each costs */
  if (!skip || !strstr(skip, "bg")) bg_draw(); glass_draw(); if (!ambGuide && (!skip || !strstr(skip, "amb"))) ambient_draw();
  if (screens[SCR] && screens[SCR]->draw && (!skip || !strstr(skip, "screen"))) { float a = app_screen_alpha(); if (xoutT.on) a *= ease_apply(EASE_CSS_EASEIN, timer_x(&xoutT)); /* under a page leaving by Back: it comes up as that one goes, never both at once */ if (a < 1) gfx_opacity_push(a);
    int mv = xmvT.on && xmv.kind && !sx_flying(); /* the hand-over's move: the incoming screen as a whole, one transform (not while a logo flies across: its landing spot must hold still, or it dips and comes back) */
    if (mv) { float k = ease_apply(EASE_CSS_EASEOUT, timer_x(&xmvT));
      if (xmv.kind == NAV_SLIDE) gfx_offset_push(90 * xmv.dx * (1 - k), 0);
      else { float s0 = xmv.kind == NAV_ZOOM ? 0.9f : 1.07f, sc = s0 + (1 - s0) * k; gfx_scale_push(sc, sc, xmv.x, xmv.y); } }
    screens[SCR]->draw();
    if (mv) { if (xmv.kind == NAV_SLIDE) gfx_offset_pop(); else gfx_scale_pop(); }
    if (a < 1) gfx_opacity_pop(); }
  if (xoutT.on && screens[xoutScr] && screens[xoutScr]->out) { int hn = hit_count(); sx_ghost(1); screens[xoutScr]->out(timer_x(&xoutT)); sx_ghost(0); hit_trim(hn); } /* seen leaving, never pointed at */
  if (!skip || !strstr(skip, "bar")) topnav_draw();
  for (int k = 0; k < nseenOv; k++) if (seenOv[k] != app_overlay && seenOv[k]->draw) seenOv[k]->draw(); /* closing ones (they draw nothing once gone) */
  if (app_overlay && app_overlay->draw) app_overlay->draw();
}
/* the page as it shows under the sheets and cards (its backdrop, the screen, the top bar), for their glass: drawn once
   into the glass's own small picture; what it adds for the pointer is dropped */
void app_draw_under(void) {
  int hn = hit_count(); sx_ghost(1); int occWas = occOn; occOn = 0; /* (the glass sees the whole page) */
  bg_draw(); glass_draw(); if (!ambGuide) ambient_draw();
  if (screens[SCR] && screens[SCR]->draw) screens[SCR]->draw();
  topnav_draw(); sx_ghost(0); hit_trim(hn); occOn = occWas;
}
void app_draw(void) {
  occOn = occNOn && !btr_under(); occX = occNX; occY = occNY; occW = occNW; occH = occNH; occNOn = 0; /* (last frame's still panel; never over the trailer's zoom) */
  hit_begin(); sx_frame_begin(); sx_layer(0);
  /* the player fades in over the page and, leaving, fades off it onto the page: the page is drawn under it only
     while it does not cover it all (the video plane itself never fades: the art and the controls above it do) */
  if (player_open() && saver_covers()) { saver_draw(); return; } /* (over a paused film: the screensaver alone) */
  if (player_open()) { if (player_page_under()) page_draw(); sx_layer(1); player_draw(); sx_layer(0); sx_flush(); toast_draw(); saver_draw(); return; }
  if (saver_covers()) { saver_draw(); return; } /* nothing under it is seen: not drawn */
  page_draw();
  sx_flush(); /* a logo or poster on its way from the last page */
  toast_draw();
  saver_draw();
}
