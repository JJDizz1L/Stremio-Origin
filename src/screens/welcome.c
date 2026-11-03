/* The guide, over a frosted Home.
   - The first start ever: a tour of seven pages. Each has its words on the start side (a step, a title, one short
     line, then its points) and, beside them, the app itself showing what the words say: the app's own screens at
     their own size and layout (1920 x 1080), with its real cards and the posters Home has just shown, drawn into a
     picture and shown in a rounded frame. The points are steps: the one in focus is lit and the picture plays it;
     OK goes down to the next point and, after the last, to the next page. The page on streams ends on the switch for
     the best stream: OK turns it on, the line is measured there and ticked off.
   - The first start of a new version: one page, the version's news (assets/whatsnew.json) on the start side, a
     message and the code to leave a tip on the other.
   Right goes to the next page, Left back to the page's first point (or the page before), Up / Down between points,
   Back closes it (Skip). The glows in the pictures are the app's own (ui/orbs.c) and feel what
   happens there, as the page's do. Seen once: guideSeen for the tour, whatsNewSeen for each version.
   Development: SB_GUIDE=1 shows the tour, SB_WHATSNEW=1 the version's page, at every start. */
#include "screens/welcome.h"
#include "screens/login.h"
#include "screens/board.h"
#include "ui/qr.h"
#include "ui/gfx.h"
#include "ui/text.h"
#include "ui/glass.h"
#include "ui/icon.h"
#include "ui/img.h"
#include "ui/orbs.h"
#include "ui/sound.h"
#include "ui/support.h"
#include "ui/widgets.h"
#include "ui/qrcard.h"
#include "screens/revpanel.h"
#include "core/app.h"
#include "core/tmdb.h"
#include "core/trakt.h"
#include "core/best.h"
#include "core/hit.h"
#include "core/httpd.h"
#include "core/i18n.h"
#include "core/keys.h"
#include "core/store.h"
#include "core/update.h"
#include "core/json.h"
#include "platform/plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* the page: the words' column on the start side, the picture's frame on the other, the dots and keys at the foot */
#define MX 88.0f
#define COLW 720.0f
#define FW 960.0f
#define FH 540.0f      /* 16:9: the app's own screen */
#define FR 30.0f
#define FY 196.0f
#define BOTY 954.0f
#define SLIDE 64.0f
#define MAXP 10
#define MAXB 5
#define MAXN 8
#define NS 10
/* the words: big and plain, to be read at a glance from the sofa */
#define STEP_PX 26
#define TI_PX 70
#define TI_LH 78.0f
#define LE_PX 34
#define LE_LH 48.0f
#define PO_PX 32
#define PO_LH 42.0f
#define CHIP 60.0f
/* the version's items: the type a little smaller and closer when there are many (six fit above the keys) */
#define NBIG (W.nnews <= 4)
#define NT_PX (NBIG ? 34 : 30)
#define NT_LH (NBIG ? 42.0f : 37.0f)
#define NX_PX (NBIG ? 27 : 25)
#define NX_LH (NBIG ? 38.0f : 33.0f)
#define NGAP (NBIG ? 34.0f : 20.0f)
#define PO_GAP 16.0f
#define CTL_H 120.0f

enum { PV_KEYS, PV_HOLD, PV_STREAMS, PV_SUBS, PV_TORRENTS, PV_PHONE, PV_YOURKEYS, PV_THANKS, PV_NEWS };
typedef struct { char title[160], lead[300], b[MAXB][200]; const char *ic[MAXB]; int nb, pv, ctl; int nt, nl, nbl[MAXB]; } Page;
/* a point's chip: an icon, or "@red", "@green", "@ok", "@back": the remote's key itself */
static const struct { const char *t, *l, *b[MAXB], *ic[MAXB]; int pv, ctl; } TOUR[] = {
  { "Your remote", "Four keys take you everywhere.",
    { "Red opens Settings", "Green opens Search", "The arrows move, OK opens", "Back goes to the tabs, then Home" },
    { "@red", "@green", "g_dpad", "@back" }, PV_KEYS, 0 },
  { "Hold OK", "Hold OK on any poster or episode for all you can do with it.",
    { "Hold to open its menu", "Add it to your Library or mark it as watched", "Its trailer, start over, or something similar" },
    { "@ok", "plus", "similar" }, PV_HOLD, 0 },
  { "The best stream", "Play picks the stream that plays best here, from:",
    { "Your internet speed", "What your TV plays: resolution, codecs, HDR", "For torrents, how many peers share it", "Hold Play to show all streams", "Play the best stream automatically" },
    { "g_speed", "p_asp", "g_stack", "@ok", "g_bolt" }, PV_STREAMS, 1 },
  { "Subtitles", "In your language, with nothing to install.",
    { "OpenSubtitles is built in", "Your subtitle addons have their own tab", "Out of sync? It is lined up by itself", "Styled subtitles, as the film made them" },
    { "p_subs", "set_addon", "g_sync", "g_type" }, PV_SUBS, 0 },
  { "Torrents", "Made for TVs with as little as 512 MB.",
    { "They start in seconds: only a small part is kept in memory", "Short on memory? Plug in a USB drive, then turn it on in Settings" },
    { "g_chip", "g_usb" }, PV_TORRENTS, 0 },
  { "Your phone as a keyboard", "Typing with a remote is slow. Use your phone instead.",
    { "Open Search: the code is under the keyboard", "Scan it with your phone\xe2\x80\x99s camera", "Type there and it lands on the TV" },
    { "nav_search", "g_phone", "g_wifi" }, PV_PHONE, 0 },
  { "Your keys", "Two keys bring more. OK shows the code for one; you can always add them later in Settings \xe2\x80\xba Keys.",
    { "TMDB: titles and descriptions in your language, and its reviews", "Trakt: the rating, how people spread theirs, and their comments" },
    { "set_lang", "reviews" }, PV_YOURKEYS, 0 },
  { "Thank you", "Made by one person in free time. No ads and no tracking.",
    { "Something wrong? Settings \xe2\x80\xba About \xe2\x80\xba Share logs", "Enjoying it? A coffee, or a star on GitHub" },
    { "g_bug", "g_heart" }, PV_THANKS, 0 },
};
#define NTOUR (int)(sizeof TOUR / sizeof TOUR[0])
static int ykOff; /* the keys page's first step, when TMDB is built in: Trakt's */
typedef struct { char t[120], x[300]; const char *ic; char icn[24]; int nt, nx; } News;
static struct {
  int open, closing, checked, tour, n, cur, step, from, fromStep, dir, pageSw, fromSignIn; Page p[MAXP];
  char ver[32], short_ver[16], stitle[96], smsg[400], url[400], urlShown[400], gh[300], ghShown[300];
  News news[MAXN]; int nnews;
  double pageAt, stepAt, fromAt; Timer1 in, out, sw; Tween dot, focus, knob; Nudge nudge; unsigned lg;
  char speed[200]; double doneAt; int speedFail;
  int tipWant; Tween tipK; /* the thank-you's codes: which shows (0 the coffee, 1 GitHub); OK turns it over */
} W;
static Screen card; static SGlass glass; /* Home behind, frosted */
static GfxTarget pic[2]; static int picOk = -1; /* the pictures: the one shown, the one leaving */
static Orbs orbs[2]; static float evPrev[2], evNow[2]; static int evSlot; /* the pictures' glows, and the moments they feel */
static Item SI[NS]; static int nsi; static char rowName[96], qrUrl[300];

static float cl01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
static float sstep(float a, float b, float t) { float x = cl01((t - a) / (b - a)); return x * x * (3 - 2 * x); }
static float eo(float a, float b, float t) { return ease_apply(EASE_OUT, (t - a) / (b - a)); }
static float glide(float at, float t) { return ease_apply(GLIDE_EASE, (t - at) / GLIDE_DUR); } /* one step of a highlight, as the app glides them */
static float vis(float t, float a, float b, float f) { return sstep(a, a + f, t) * (1 - sstep(b - f, b, t)); } /* in from a, gone by b */
static float bez(float a, float b, float c, float e) { return (1 - e) * (1 - e) * a + 2 * (1 - e) * e * b + e * e * c; }
static float rnd(int i) { unsigned x = (unsigned)i * 2654435761u; x ^= x >> 13; x *= 0x5bd1e995u; x ^= x >> 15; return (x & 0xffff) / 65535.0f; }
static void glow(float cx, float cy, float w, float h, uint32_t c, float a) { if (a > 0.004f) gfx_alpha_tex(gfx_radial_tex(), cx - w / 2, cy - h / 2, w, h, col_alpha(c | 0xff000000u, a)); }
/* the picture's clock passed `when` this frame (on a loop of `len` seconds): the glows are pushed then */
static int ev(float when, float len) { float a = fmodf(evPrev[evSlot], len), b = fmodf(evNow[evSlot], len); if (evNow[evSlot] <= evPrev[evSlot]) return 0; return b >= a ? when > a && when <= b : when > a || when <= b; }
static void push(float when, float len, int kind, float s) { if (ev(when, len)) orbs_push(&orbs[evSlot], kind, s); }
static void porbs(int layout, float a) { orbs_step(&orbs[evSlot], app_dt(), layout, 1, 2.6f); orbs_draw(&orbs[evSlot], a); }

/* ==== the pictures: the app's own screens, in layout units (1920 x 1080) ==== */
static const Item *si(int i) { return nsi ? &SI[(i % nsi + nsi) % nsi] : NULL; }
static void poster(int i, float x, float y, float w, float h) { const Item *it = si(i); if (it) card_draw_wh(it, x, y, w, h, 0, 0, 1); else ph_surface(x, y, w, h, CARD_R); }
/* a press seen in the picture: the remote's key on screen (press()) lights and sinks what it presses, the frame after
   (wPress 0..1 as the key goes down and up; wKey which key) */
static float wPress, pressNow, pressK[2]; static int wKey, keyNow, keyK[2];
static float ok_press(void) { return wKey == K_OK ? wPress : 0; }
/* a lit row or option, pressed: a touch smaller and lighter while the key is down */
static void lit_box(float x, float y, float w, float h, float r, uint32_t c, float pk) {
  float d = 6 * pk; gfx_rrect(x + d, y + d * 0.6f, w - 2 * d, h - 1.2f * d, r, c); if (pk > 0.01f) gfx_rrect(x + d, y + d * 0.6f, w - 2 * d, h - 1.2f * d, r, WHITE_A((int)(46 * pk))); }
/* a card with the app's own focus (its lift, shadow and glare), held for its menu while hold > 0 */
static void card_at(int key, int i, float x, float y, float w, float h, int focused, float hold) {
  unsigned long long k = lift_key(&W, key + 100 * (W.pageSw ? evSlot : 0)); /* (two points of one page share their cards' lift: the handover moves only what changes) */ float ps = focused ? 1 - 0.05f * ok_press() : 1; if (ps < 1) gfx_scale_push(ps, ps, x + w / 2, y + h / 2); /* OK pressed on it: it sinks a little */
  lift_begin(k, focused, hold, x, y, w, h, CARD_R); poster(i, x, y, w, h);
  if (hold > 0) hold_trace_draw(x, y, w, h, CARD_R, ease_apply(EASE_IN_OUT, hold));
  lift_end(k, x, y, w, h, CARD_R); if (ps < 1) gfx_scale_pop();
}
static GLuint art(int i, int logo, float *asp) { const Item *it = si(i); char b[512]; *asp = 0; if (!it) return 0;
  if (logo ? !sb_logo_of(it, b, sizeof b)[0] : !sb_bg_mid(it, b, sizeof b)[0]) return 0; return img_get(b, logo ? 600 : 1280, logo ? 240 : 720, asp); }
/* a title's backdrop under the pages' shading (dark from the start side and from the foot) */
static GfxTarget orbT[2]; static int orbTOk = -1; static GLuint sgOrb; /* each picture's glows, small (made before its picture: no target in a target) */
static GLuint sgT, sgT2; static float sgShade, sgPage, sgMix, sgDim, wSoft = 1; /* (wSoft: the next wglass bends less) */ /* (sgT2: a second backdrop coming in over the first, by sgMix) */ /* the scene's backdrop and its shading: what its glass bends; how far Settings' or Search's page is over it */
static void backdrop(int i, float shade) {
  gfx_rect(0, 0, SB_W, SB_H, PAGE); float asp; GLuint t = art(i, 0, &asp); sgT = t; sgShade = shade;
  if (t) gfx_image(t, 0, 0, SB_W, SB_H, 0, WHITE_A(255), 0, 0, 1, 1);
  if (shade > 0) { gfx_rrect_h(0, 0, 1400, SB_H, 0, col_alpha(PAGE, 0.94f * shade), col_alpha(PAGE, 0)); gfx_rrect_v(0, 360, SB_W, SB_H - 360, 0, col_alpha(PAGE, 0), col_alpha(PAGE, 0.97f * shade)); }
}
static void logo(int i, float x, float yb, float mw, float mh) { /* the title's logo, its foot at yb (its name when it has none) */
  float asp; GLuint t = art(i, 1, &asp); const Item *it = si(i);
  if (t && asp > 0) { float w = fminf(mw, mh * asp), h = w / asp; gfx_image(t, x, yb - h, w, h, 0, WHITE_A(255), 0, 0, 1, 1); }
  else if (it) text_draw_wrapped(it->name, x, yb - 140, mw, 2, 60, 800, WHITE_A(255), -1.2f, ALIGN_LEFT, 66);
}
/* the title, its line and its words, where Home's hero has them */
static void hero(int i, float x, float y) {
  const Item *it = si(i); if (!it) return;
  logo(i, x, y + 170, 560, 170); y += 196; meta_draw(it->metaLine, "", it->name, x, y, 25, WHITE_A(220), ALIGN_LEFT); y += 46;
  text_draw_wrapped(it->description, x, y, 960, 3, 27, 400, WHITE_A(205), 0, ALIGN_LEFT, 40);
}
/* the top bar as the app draws it: Settings and Search at the corners with their key's dot, the tabs in the middle
   (cur: the page you are on, -1 none; pill: the focused tab, -1 none), the clock. setF / searchF: the corner in focus */
static float tabX[3], tabW[3];
/* the bar's liquid glass over the scene's backdrop (its shading there laid over it, as the app's baked layers have it) */
static void wglass(float x, float y, float w, float h, float r, float k, int seed) {
  float cx = x + w / 2, cy = y + h / 2, dh = 0.94f * cl01(1 - cx / 1400), dv = cy > 360 ? 0.97f * cl01((cy - 360) / (SB_H - 360)) : 0;
  float dk = 1 - (1 - sgShade * (1 - (1 - dh) * (1 - dv))) * (1 - sgDim); /* the picture's shading there, and a scene's own dark over it */
  if (sgPage < 0.999f || !sgOrb) { lglass_soft(wSoft); lglass_liquid(sgT, x, y, w, h, r, x / SB_W, y / SB_H, (x + w) / SB_W, (y + h) / SB_H, k, dk, seed, w < 300); }
  if (sgMix > 0.001f && sgT2) { gfx_opacity_push(sgMix); lglass_soft(wSoft); lglass_liquid(sgT2, x, y, w, h, r, x / SB_W, y / SB_H, (x + w) / SB_W, (y + h) / SB_H, k, dk, seed, w < 300); gfx_opacity_pop(); }
  if (sgPage > 0.001f && sgOrb) { gfx_opacity_push(sgPage); lglass_soft(wSoft); lglass_liquid(sgOrb, x, y, w, h, r, x / SB_W, 1 - y / SB_H, (x + w) / SB_W, 1 - (y + h) / SB_H, k, 0, seed, w < 300); gfx_opacity_pop(); } /* Settings' or Search's page over it: its glows */
  wSoft = 1;
}
static void corner(float x, float f, const char *icon, uint32_t dot, float rot, int seed) {
  float y = 50 - 38, pk = (wKey == K_RED && seed == 1) || (wKey == K_GREEN && seed == 2) ? wPress : 0, ps = 1 - 0.12f * pk; /* its key pressed: it sinks and lights */
  if (ps < 1) gfx_scale_push(ps, ps, x + 38, y + 38);
  wglass(x, y, 76, 76, 38, 0.85f + 0.3f * f + 0.5f * pk, seed);
  if (f > 0.001f) { float s = 0.78f + 0.22f * f; gfx_opacity_push(f); gfx_scale_push(s, s, x + 38, y + 38);
    gfx_rrect_v(x, y, 76, 76, 38, col_alpha(ACC2 | 0xff000000u, 0.42f), col_alpha(ACC | 0xff000000u, 0.30f)); gfx_scale_pop(); gfx_opacity_pop(); } /* the accent */
  icon_draw_rot(icon, x + 18, y + 18, 40, col_mix(WHITE_A(184), WHITE_A(255), f), rot);
  float d = 1 - f; if (d > 0.001f) { uint32_t c = dot | 0xff000000u; /* the remote's colour key: a short arc along the top, just inside the rim */
    gfx_clip_push(x, y, 76, 13); gfx_ring(x + 3.5f, y + 3.5f, 69, 69, 34.5f, 1.75f, col_alpha(c, 0.32f * d)); gfx_clip_pop();
    gfx_clip_push(x, y, 76, 9); gfx_ring(x + 3.5f, y + 3.5f, 69, 69, 34.5f, 1.75f, col_alpha(c, 0.5f * d)); gfx_clip_pop(); }
  if (pk > 0.01f) gfx_ring_out(x, y, 76, 76, 38, 3 + 6 * pk, col_alpha(dot | 0xff000000u, 0.55f * pk)); /* the key's colour round it as it goes down */
  if (ps < 1) gfx_scale_pop();
}
static void topbar(int cur, float pill, float setF, float searchF, float libPing) {
  static const char *NM[3] = { "Discover", "Home", "Library" }; float tot = 0;
  for (int i = 0; i < 3; i++) { tabW[i] = text_width(T(NM[i]), 32, i == cur ? 700 : 400, 0) + 80; tot += tabW[i] + 18; }
  float x = SB_W / 2 - (tot - 18) / 2; for (int i = 0; i < 3; i++) { tabX[i] = x; x += tabW[i] + 18; }
  wglass(tabX[0] - 8, 50 - 39, tabX[2] + tabW[2] + 8 - (tabX[0] - 8), 78, 39, pill >= 0 ? 1.15f : 0.85f, 0); /* the tabs' capsule */
  if (pill >= 0) { int a = (int)pill; float f = pill - a; int b = a < 2 ? a + 1 : 2; float px = tabX[a] + (tabX[b] - tabX[a]) * f, pw = tabW[a] + (tabW[b] - tabW[a]) * f;
    gfx_rrect_v(px, 50 - 31, pw, 62, 31, col_alpha(ACC2 | 0xff000000u, 0.46f), col_alpha(ACC | 0xff000000u, 0.36f)); gfx_rrect_v(px, 50 - 31, pw, 62, 31, WHITE_A(40), WHITE_A(0));
    gfx_ring(px, 50 - 31, pw, 62, 31, 1.2f, WHITE_A(70)); } /* the focus: accent glass */
  for (int i = 0; i < 3; i++) { int on = i == cur || (pill >= 0 && fabsf(pill - i) < 0.5f);
    if (i == cur && pill < 0) { gfx_rrect(tabX[i], 50 - 30, tabW[i], 60, 30, WHITE_A(20)); gfx_ring(tabX[i], 50 - 30, tabW[i], 60, 30, 1, WHITE_A(46)); } /* the page shown: a softer piece of glass */
    text_draw(T(NM[i]), tabX[i] + tabW[i] / 2, 50 - text_line_height(32) / 2.0f, 32, on ? 700 : 400, on ? WHITE_A(255) : WHITE_A(184), 0, 0, ALIGN_CENTER);
    if (i == 2 && libPing > 0) { float k = libPing; gfx_circle(tabX[i] + tabW[i] - 26, 34, 6 * fminf(1, 2 * k), ACC2); gfx_ring_out(tabX[i] + tabW[i] - 26 - 6 - 14 * k, 34 - 6 - 14 * k, 12 + 28 * k, 12 + 28 * k, 6 + 14 * k, 2, col_alpha(ACC2, 1 - k)); } }
  corner(128, searchF, "nav_search", RGB(0x2f, 0xd6, 0x6b), 0, 2);
  corner(40, setF, "nav_set", RGB(0xef, 0x3b, 0x3b), 90 * ease_apply(EASE_OUT, setF), 1);
  lglass_light(0); app_clock_draw(SB_W - 56, 50, 1);
#ifdef SB_LINUX
  app_fsbtn_draw(SB_W - 56, 50);
#endif
}
/* Home: a title's backdrop and words, its row (focus: the card with the focus, -1 none; hold 0..1 held on it) */
static float rowY(void) { return 478 + 18 + text_line_height(31) + 26; }
static void home(int bgi, int focus, float hold, float off) {
  int b = bgi >= 0 ? bgi : focus < 0 ? 0 : focus; backdrop(b, 1); hero(b, 64, 110);
  text_draw(rowName[0] ? rowName : T("Popular"), 52, 478 + 18, 31, 700, RGB(0xe9, 0xe9, 0xee), -0.3f, SB_W - 104, ALIGN_LEFT);
  float cy = rowY();
  for (int i = 0; i < 7; i++) if (i != focus) card_at(i, i, 48 + i * 328 - off, cy, 300, 450, 0, 0);
  if (focus >= 0) card_at(focus, focus, 48 + focus * 328 - off, cy, 300, 450, 1, hold);
}
/* a key of the remote, large, as it is pressed: the key and what it does on a glass pill (k 0..1 shown, pr 0..1 the press) */
static void press(int key, const char *label, float k, float pr, float x, float y) {
  if (pr > 0 && pr < 1) { float b = sinf(3.14159f * pr); if (b > pressNow) { pressNow = b; keyNow = key; } } /* (what it presses feels it, the next frame) */
  if (k <= 0.004f) return; float w = 2 * (42 + 10 + text_width(label, 22, 600, 0)) + 64, h = 108, s = 1 - 0.07f * sinf(3.14159f * cl01(pr));
  gfx_opacity_push(k); gfx_offset_push(0, 16 * (1 - k)); gfx_scale_push(s, s, x, y);
  gfx_shadow(x - w / 2, y - h / 2 + 12, w, h, h / 2, 40, BLACK_A(120)); gfx_rrect(x - w / 2, y - h / 2, w, h, h / 2, RGBA(22, 20, 30, 230)); gfx_rrect_v(x - w / 2, y - h / 2, w, h, h / 2, WHITE_A(22), WHITE_A(0)); gfx_ring(x - w / 2, y - h / 2, w, h, h / 2, 1, WHITE_A(40));
  if (pr > 0 && pr < 1) gfx_ring_out(x - w / 2, y - h / 2, w, h, h / 2, 3 + 10 * pr, col_alpha(key == K_RED ? RGB(0xef, 0x3b, 0x3b) : key == K_GREEN ? RGB(0x2f, 0xd6, 0x6b) : ACC2 | 0xff000000u, 0.6f * (1 - pr)));
  if (gfx_mirrored()) key = key == K_LEFT ? K_RIGHT : key == K_RIGHT ? K_LEFT : key; /* (mirrored: the arrow the viewer really presses) */
  gfx_scale_push(2, 2, x - w / 2 + 32, y); key_hint(x - w / 2 + 32, y, key, label, WHITE_A(250), 0); gfx_scale_pop();
  gfx_scale_pop(); gfx_offset_pop(); gfx_opacity_pop();
}
#define PRESS_Y (SB_H - 120)

/* Settings, as the page draws it: the categories, then Playback's rows, or About's (focus: a row lit, -1 none: the
   category; usbOn 0..1) */
static void settings_page(float a, int focus, float usbOn, int about) {
  if (a <= 0.004f) return; gfx_opacity_push(a); sgPage = a;
  gfx_rect(0, 0, SB_W, SB_H, PAGE); porbs(0, 1);
  static const char *IC[7] = { "set_play", "set_subs", "set_aud", "set_look", "set_lang", "set_addon", "info" }, *LB[7] = { "Playback", "Subtitles", "Audio", "Appearance", "Language", "Addons", "About" };
  float cx = 160, y = 150;
  text_draw(T("Watch"), cx + 20, y, 22, 600, WHITE_A(158), 0, 460, ALIGN_LEFT); y += 44;
  for (int i = 0; i < 7; i++) { if (i == 3) { text_draw(T("Screens"), cx + 20, y + 14, 22, 600, WHITE_A(158), 0, 460, ALIGN_LEFT); y += 58; } if (i == 5) { text_draw(T("Account and system"), cx + 20, y + 14, 22, 600, WHITE_A(158), 0, 460, ALIGN_LEFT); y += 58; }
    int f = i == (about ? 6 : 0) && focus < 0, cur = i == (about ? 6 : 0); if (f) gfx_rrect(cx, y, 500, 64, 18, ACC | 0xff000000u);
    icon_draw(IC[i], cx + 20, y + 17, 30, f ? ACC_FG : cur ? ACC3 : WHITE_A(140)); text_draw(T(LB[i]), cx + 70, y + 32 - text_line_height(27) / 2.0f, 27, 600, f ? ACC_FG : cur ? WHITE_A(255) : WHITE_A(204), -0.2f, 410, ALIGN_LEFT); y += 64; }
  float px = 720, py = 150, pw = 1040, ph = 810;
  gfx_rrect(px, py, pw, ph, 24, RGBA(21, 19, 28, 150)); gfx_rrect_v(px, py, pw, ph, 24, WHITE_A(10), WHITE_A(0)); gfx_ring(px, py, pw, ph, 24, 1, WHITE_A(22));
  text_draw(T(about ? "About" : "Playback"), px + 54, py + 40, 38, 700, WHITE_A(255), -0.5f, 900, ALIGN_LEFT);
  if (about) { static const char *AL[3] = { "Check for updates", "Share logs", "Support the project" }, *AD[3] = { "You have the latest version", "Share the code and describe the issue", "A coffee, or a star on GitHub" };
    float ry = py + 120; for (int i = 0; i < 3; i++) { int f = i == focus;
      if (f) { float pk = ok_press(), d = 6 * pk; lit_box(px + 30, ry - 4, pw - 60, 112, 20, ACC | 0xff000000u, pk); gfx_ring_out(px + 30 + d, ry - 4 + d * 0.6f, pw - 60 - 2 * d, 112 - 1.2f * d, 20, 4, acc_a(0.3f)); }
      text_draw(T(AL[i]), px + 54, ry + 16, 28, 600, f ? ACC_FG : ACC3 | 0xff000000u, -0.2f, 900, ALIGN_LEFT); text_draw(T(AD[i]), px + 54, ry + 54, 22, 400, f ? col_alpha(ACC_FG, 0.85f) : WHITE_A(199), 0, 900, ALIGN_LEFT); ry += 118; }
    gfx_opacity_pop(); return; }
  static const struct { const char *l, *d, *v; int sw; } R[6] = {
    { "Play the best stream automatically", "Play starts the best stream for this TV and your internet speed", NULL, 0 },
    { "Seek step", "How far Left / Right jumps in the player", "10s", 0 }, { "Auto-play next episode", "When an episode ends, the next one starts", NULL, 1 },
    { "Picture size", "Original, fill the screen or zoom", "Original", 0 }, { "Background trailers", "A trailer plays behind Home after a few seconds", NULL, 1 },
    { "Cache torrents on USB", "USB drive \xc2\xb7 28 GB free. Faster seeks and less memory used", NULL, 2 } };
  float ry = py + 120;
  for (int i = 0; i < 6; i++) { float L = px + 54, Rx = px + pw - 54; int f = i == focus;
    if (f) { float pk = ok_press(), d = 6 * pk; lit_box(px + 30, ry - 4, pw - 60, 112, 20, ACC | 0xff000000u, pk); gfx_ring_out(px + 30 + d, ry - 4 + d * 0.6f, pw - 60 - 2 * d, 112 - 1.2f * d, 20, 4, acc_a(0.3f)); }
    text_draw(T(R[i].l), L, ry + 16, 28, 600, f ? ACC_FG : WHITE_A(255), -0.2f, 700, ALIGN_LEFT); text_draw(T(R[i].d), L, ry + 16 + 38, 22, 400, f ? col_alpha(ACC_FG, 0.85f) : WHITE_A(199), 0, 760, ALIGN_LEFT);
    if (R[i].v) { text_draw(" \xe2\x80\xba", Rx, ry + 36, 26, 600, WHITE_A(115), 0, 0, ALIGN_RIGHT); text_draw(T(R[i].v), Rx - 22, ry + 36, 26, 600, ACC3 | 0xff000000u, 0, 0, ALIGN_RIGHT); }
    else { float on = R[i].sw == 1 ? 1 : R[i].sw == 2 ? usbOn : best_on() ? 1 : 0; uint32_t tr = f ? col_mix(WHITE_A(82), WHITE_A(255), on) : col_mix(WHITE_A(51), ACC | 0xff000000u, on);
      gfx_rrect(Rx - 64, ry + 30, 64, 36, 18, tr); gfx_circle(Rx - 64 + 18 + 28 * on, ry + 48, 14, f ? col_mix(WHITE_A(255), ACC | 0xff000000u, on) : WHITE_A(255)); }
    ry += 118; }
  gfx_opacity_pop();
}
/* Search, as the page draws it: the box (typed: letters of the word), the keyboard, the phone's code, the results */
static void search_page(float a, int typed, float res, float qrRing) {
  if (a <= 0.004f) return; gfx_opacity_push(a); sgPage = a;
  gfx_rect(0, 0, SB_W, SB_H, PAGE); porbs(1, 1);
  float qx = 96, qy = 160; static const char *WORD = "moon";
  gfx_rrect(qx, qy, 552, 76, 20, WHITE_A(18)); gfx_ring(qx, qy, 552, 76, 20, 1, WHITE_A(26)); icon_draw("search_box", qx + 24, qy + 22, 32, WHITE_A(153));
  char q[8]; snprintf(q, sizeof q, "%.*s", typed, WORD); float tx = qx + 72, cw = 0;
  if (typed) cw = text_draw(q, tx, qy + (76 - text_line_height(32)) / 2, 32, 600, WHITE_A(255), 0, 420, ALIGN_LEFT);
  else text_draw(T("Search films and series"), tx + 14, qy + (76 - text_line_height(32)) / 2, 32, 500, WHITE_A(133), 0, 420, ALIGN_LEFT);
  gfx_rrect(tx + (typed ? cw + 4 : 0), qy + 20, 3, 36, 1.5f, col_alpha(ACC2 | 0xff000000u, 0.6f + 0.4f * cosf((float)app_now() * 5)));
  static const char *K = "abcdefghijklmnopqrstuvwxyz1234567890"; int hot = typed > 0 && typed <= 4 ? (int)(strchr(K, WORD[typed - 1]) - K) : -1;
  for (int i = 0; i < 36; i++) { float x = qx + (i % 6) * 93, y = qy + 94 + (i / 6) * 74; int f = i == hot;
    gfx_rrect(x, y, 84, 66, 14, f ? ACC | 0xff000000u : WHITE_A(15)); char s[2] = { (char)(K[i] >= 'a' ? K[i] - 32 : K[i]), 0 };
    text_draw(s, x + 42, y + (66 - text_line_height(28)) / 2, 28, 700, f ? ACC_FG : WHITE_A(219), 0, 0, ALIGN_CENTER); }
  float qr = qy + 94 + 6 * 74 + 30;
  if (qrRing > 0) gfx_ring_out(qx - 8, qr - 8, 166, 166, 20, 4 + 3 * sinf((float)app_now() * 6), col_alpha(ACC2, qrRing));
  if (!qr_draw(qrUrl, qx, qr, 150, 14, 8)) gfx_rrect(qx, qr, 150, 150, 14, WHITE_A(20));
  text_draw(T("Type on your phone"), qx + 172, qr + 40, 24, 700, WHITE_A(255), 0, 380, ALIGN_LEFT);
  text_draw_wrapped(T("Scan with your phone's camera, then type there"), qx + 172, qr + 76, 380, 2, 23, 400, WHITE_A(191), 0, ALIGN_LEFT, 31);
  float rx = 760; char h[96]; snprintf(h, sizeof h, "%s", typed ? T1("Results for \xe2\x80\x9c{q}\xe2\x80\x9d", "q", q) : T("Type to search films and series"));
  text_draw(h, rx, qy, 24, 600, WHITE_A(191), 0, 1080, ALIGN_LEFT);
  for (int i = 0; i < 8; i++) { float r = cl01(res * 1.6f - i * 0.08f); if (r <= 0.004f) continue; gfx_opacity_push(r);
    poster(i + 1, rx + (i % 4) * 268, qy + 60 + (i / 4) * 384 + 16 * (1 - r), 244, 366); gfx_opacity_pop(); }
  gfx_opacity_pop();
}

/* -- 1. the remote -- */
/* the camera: the whole screen first, then in on what matters (fx, fy) by s, easing in from a and back out by b; the
   picture pops it after the scene */
static int camOn;
static void cam(float t, float a, float b, float fx, float fy, float s) {
  float k = ease_apply(EASE_IN_OUT, cl01((t - a) / 0.8f)) * (1 - ease_apply(EASE_IN_OUT, cl01((t - (b - 0.8f)) / 0.8f))); if (k <= 0.001f || camOn) return;
  float sc = 1 + (s - 1) * k, ox = (SB_W / 2 - fx) * 0.55f * k, oy = (SB_H / 2 - fy) * 0.55f * k;
  /* never past the picture's own edges (the dark under it, a hard corner): the move is held inside them */
  float lo = (SB_W - fx) * (1 - sc), hi = fx * (sc - 1); ox = ox < lo ? lo : ox > hi ? hi : ox;
  lo = (SB_H - fy) * (1 - sc); hi = fy * (sc - 1); oy = oy < lo ? lo : oy > hi ? hi : oy;
  camOn = 1; gfx_offset_push(ox, oy); gfx_scale_push(sc, sc, fx, fy);
}
static void sc_keys(int st, float t) {
  if (st == 0 || st == 1) { /* a colour key: its page comes out of its corner, then Back */
    float L = 7.0f; t = fmodf(t, L); int red = st == 0; cam(t, 0.4f, 2.2f, red ? 78 : 166, 50, 1.9f); /* in on its key as it is pressed, out as its page fills the screen */
    float pa = vis(t, 1.4f, 5.4f, 0.3f), s = 0.9f + 0.1f * ease_apply(EASE_OUT, pa);
    home(-1, 1, 0, 0);
    push(1.4f, L, red ? ORB_RED : ORB_GREEN, 1); push(5.1f, L, ORB_BACK, 0.7f);
    if (pa > 0.004f) { gfx_scale_push(s, s, red ? 78 : 166, 50);
      if (red) settings_page(pa, -1, 0, 0); else { int ty = t < 2.3f ? 0 : t < 2.6f ? 1 : t < 2.9f ? 2 : t < 3.2f ? 3 : 4; for (int k = 0; k < 4; k++) push(2.3f + 0.3f * k, L, ORB_OK, 0.25f); search_page(pa, ty, cl01((t - 3.3f) / 0.8f), 0); }
      gfx_scale_pop(); }
    topbar(pa > 0.5f ? -1 : 1, -1, red ? vis(t, 1.2f, 5.4f, 0.25f) : 0, red ? 0 : vis(t, 1.2f, 5.4f, 0.25f), 0);
    press(red ? K_RED : K_GREEN, T(red ? "Settings" : "Search"), vis(t, 0.6f, 1.8f, 0.2f), (t - 1.0f) / 0.35f, SB_W / 2, PRESS_Y);
    press(K_BACK, T("Back"), vis(t, 4.7f, 5.8f, 0.2f), (t - 5.0f) / 0.35f, SB_W / 2, PRESS_Y);
  } else if (st == 2) { /* the arrows along the row, then OK opens the title */
    float L = 7.0f; t = fmodf(t, L); int f = t < 0.8f ? 0 : t < 1.6f ? 1 : t < 2.4f ? 2 : 3; float op = vis(t, 3.4f, 6.4f, 0.35f);
    home(-1, f, 0, 0); topbar(1, -1, 0, 0, 0);
    if (op > 0.004f) { gfx_opacity_push(op); gfx_scale_push(0.96f + 0.04f * op, 0.96f + 0.04f * op, SB_W / 2, SB_H / 2); backdrop(3, 0.7f); logo(3, 96, 520, 760, 230); const Item *it = si(3);
      if (it) { meta_draw(it->metaLine, "", it->name, 96, 560, 28, WHITE_A(225), ALIGN_LEFT); text_draw_wrapped(it->description, 96, 620, 1000, 3, 29, 400, WHITE_A(210), 0, ALIGN_LEFT, 42); }
      gfx_rrect(96, 800, 260, 84, 42, WHITE_A(255)); icon_draw("play", 130, 824, 36, RGB(17, 17, 17)); text_draw(T("Play"), 180, 842 - text_line_height(30) / 2.0f, 30, 700, RGB(17, 17, 17), 0, 0, ALIGN_LEFT);
      gfx_scale_pop(); gfx_opacity_pop(); }
    for (int k = 0; k < 3; k++) push(0.8f + 0.8f * k, L, ORB_RIGHT, 0.4f); push(3.4f, L, ORB_OK, 0.7f);
    press(K_RIGHT, T("Move"), vis(t, 0.5f, 2.8f, 0.2f), fmodf(t - 0.6f, 0.8f) / 0.35f, SB_W / 2, PRESS_Y);
    press(K_OK, T("Open"), vis(t, 3.0f, 4.0f, 0.2f), (t - 3.3f) / 0.35f, SB_W / 2, PRESS_Y);
  } else { /* Back: from a page's row to its tabs, then to Home */
    float L = 6.5f; t = fmodf(t, L); float up = sstep(1.2f, 1.5f, t), home2 = sstep(3.3f, 3.8f, t), pill = up > 0.5f ? 0 + glide(3.3f, t) : -1;
    gfx_offset_push(-90 * home2, 0); gfx_opacity_push(1 - home2); backdrop(5, 1); hero(5, 64, 110);
    text_draw(T("Popular"), 52, 496, 31, 700, RGB(0xe9, 0xe9, 0xee), -0.3f, SB_W - 104, ALIGN_LEFT);
    for (int i = 0; i < 7; i++) card_at(20 + i, i + 5, 48 + i * 328, rowY(), 300, 450, i == 2 && up < 0.5f, 0); gfx_opacity_pop(); gfx_offset_pop();
    if (home2 > 0.004f) { gfx_offset_push(90 * (1 - home2), 0); gfx_opacity_push(home2); home(-1, -1, 0, 0); gfx_opacity_pop(); gfx_offset_pop(); }
    topbar(home2 > 0.5f ? 1 : 0, pill, 0, 0, 0);
    push(1.2f, L, ORB_BACK, 0.6f); push(3.3f, L, ORB_TAB_R, 0.8f);
    press(K_BACK, T("Back"), vis(t, 0.7f, 4.4f, 0.2f), t < 2.5f ? (t - 1.0f) / 0.35f : (t - 3.1f) / 0.35f, SB_W / 2, PRESS_Y);
  }
}

/* -- 2. hold OK -- */
static void menu(float m, float sel, float ck) { /* the card menu's own layout, slid in by m; sel: the option lit; ck: Add to Library ticked */
  if (m <= 0.004f) return; const Item *it = si(1); float W0 = 640, P = 36, IH = 64;
  static const char *IC[6] = { "play", "info", "eye", "trailer", "similar", "plus" }, *LB[6] = { "Play", "Details", "Mark as watched", "Trailer", "Watch something similar", "Add to Library" };
  float tl = it ? (float)text_wrap_count(it->name, W0 - 2 * P, 2, 38, 700, -0.5f) : 1; if (tl < 1) tl = 1;
  float h = 40 + 36 + tl * 46 + 26 + 6 * (IH + 4) + 30, x = SB_W - 96 - W0, y = SB_H / 2 - h / 2, sc = 0.96f + 0.04f * m;
  gfx_opacity_push(m); gfx_offset_push(40 * (1 - m), 0); gfx_scale_push(sc, sc, x + W0 / 2, SB_H / 2);
  gfx_shadow(x, y + 30, W0, h, 28, 80, BLACK_A(140)); gfx_rrect(x, y, W0, h, 28, RGBA(30, 27, 40, 238)); gfx_rrect_v(x, y, W0, h, 28, WHITE_A(18), WHITE_A(0)); gfx_ring(x, y, W0, h, 28, 1, WHITE_A(18));
  float yy = y + 40; text_draw(T(it && !strcmp(it->type, "series") ? "Series" : "Movie"), x + P, yy + (30 - text_line_height(24)) / 2, 24, 600, WHITE_A(184), 0, W0 - 2 * P, ALIGN_LEFT); yy += 36;
  text_draw_wrapped(it ? it->name : "", x + P, yy + (46 - text_line_height(38)) / 2, W0 - 2 * P, 2, 38, 700, WHITE_A(255), -0.5f, ALIGN_LEFT, 46); yy += tl * 46 + 26;
  float py = yy + sel * (IH + 4); glow(x + W0 / 2, py + IH / 2, W0 + 40, IH + 70, ACC, 0.34f); lit_box(x + P, py, W0 - 2 * P, IH, 16, ACC | 0xff000000u, ok_press());
  for (int i = 0; i < 6; i++) { float k = 1 - fabsf(i - sel); k = k < 0 ? 0 : k; uint32_t c = col_mix(WHITE_A(217), ACC_FG | 0xff000000u, k), ic = col_mix(WHITE_A(176), ACC_FG | 0xff000000u, k);
    float iy = yy + i * (IH + 4), ix = x + P + 20;
    if (i == 5 && ck > 0) { icon_draw("plus", ix, iy + (IH - 30) / 2, 30, col_alpha(ic, 1 - ck)); icon_draw("check", ix, iy + (IH - 30) / 2, 30, col_alpha(ic, ck)); } else icon_draw(IC[i], ix, iy + (IH - 30) / 2, 30, ic);
    text_draw(i == 5 && ck > 0.5f ? T("Remove from Library") : T(LB[i]), ix + 50, iy + (IH - text_line_height(27)) / 2, 27, 600, c, 0, W0 - 2 * P - 90, ALIGN_LEFT); }
  gfx_scale_pop(); gfx_offset_pop(); gfx_opacity_pop();
}
static void sc_hold(int st, float t) {
  float cx = 48 + 328, cy = rowY();
  if (st == 0) { float L = 5.0f; t = fmodf(t, L); float hp = cl01((t - 0.5f) / 1.2f), held = t > 0.4f && t < 1.8f ? hp : 0, m = eo(1.7f, 2.0f, t) * (1 - eo(4.4f, 4.7f, t));
    home(1, 1, held, 0); topbar(1, -1, 0, 0, 0); menu(m, 0, 0); push(1.7f, L, ORB_HOLD, 1);
    press(K_OK, T("Hold"), vis(t, 0.2f, 2.0f, 0.2f), 0, 560, PRESS_Y); return; }
  if (st == 1) { float L = 7.0f; t = fmodf(t, L); float m = eo(0, 0.2f, t) * (1 - eo(3.0f, 3.3f, t)), sel = 0; for (int k = 0; k < 5; k++) sel += glide(0.6f + 0.25f * k, t);
    float ck = eo(2.5f, 2.7f, t), c = (t - 3.05f) / 0.85f;
    home(1, 1, 0, 0);
    float il = vis(t, 3.9f, 6.8f, 0.3f); if (il > 0) { const char *s = T("In Library"); float w = text_width(s, 22, 700, 0) + 30; gfx_opacity_push(il);
      gfx_rrect(cx + 300 - 14 - w, cy + 14, w, 40, 20, RGBA(12, 11, 17, 200)); gfx_ring(cx + 300 - 14 - w, cy + 14, w, 40, 20, 1, col_alpha(ACC2, 0.8f)); text_draw(s, cx + 300 - 14 - w / 2, cy + 34 - text_line_height(22) / 2.0f, 22, 700, ACC2, 0, 0, ALIGN_CENTER); gfx_opacity_pop(); }
    topbar(1, -1, 0, 0, vis(t, 3.85f, 5.0f, 0.1f) > 0 ? cl01((t - 3.85f) / 1.0f) : 0); menu(m, sel, ck);
    if (c > 0 && c < 1) { float e = ease_apply(EASE_IN_OUT, c), sx = cx + 150, sy = cy + 225, ex = tabX[2] + tabW[2] / 2, ey = 50, px = bez(sx, (sx + ex) / 2, ex, e), py = bez(sy, fminf(sy, ey) - 120, ey, e), s = 1 - 0.55f * e;
      const char *l = T("Added to Library"); float w = text_width(l, 26, 600, 0) + 96;
      gfx_opacity_push(1 - sstep(0.85f, 1, c)); gfx_scale_push(s, s, px, py); gfx_shadow(px - w / 2, py - 30 + 8, w, 60, 30, 24, BLACK_A(120));
      gfx_rrect(px - w / 2, py - 30, w, 60, 30, RGBA(30, 27, 40, 240)); gfx_ring(px - w / 2, py - 30, w, 60, 30, 1, WHITE_A(40)); gfx_circle(px - w / 2 + 34, py, 17, ACC2); icon_draw("check", px - w / 2 + 23, py - 11, 22, WHITE_A(255));
      text_draw(l, px - w / 2 + 64, py - text_line_height(26) / 2.0f, 26, 600, WHITE_A(240), 0, 0, ALIGN_LEFT); gfx_scale_pop(); gfx_opacity_pop(); }
    for (int k = 0; k < 5; k++) push(0.6f + 0.25f * k, L, ORB_DOWN, 0.3f);
    push(2.5f, L, ORB_OK, 0.6f); push(3.85f, L, ORB_DONE, 0.9f);
    press(K_OK, T("OK"), vis(t, 2.2f, 3.1f, 0.2f), (t - 2.45f) / 0.35f, 560, PRESS_Y); return; }
  { float L = 6.0f; t = fmodf(t, L); float sel = 3 + glide(1.2f, t) - ease_apply(GLIDE_EASE, (t - 3.2f) / GLIDE_DUR) * 2 + glide(4.4f, t) * 2;
    home(1, 1, 0, 0); topbar(1, -1, 0, 0, 0); menu(1, sel, 0); push(1.2f, L, ORB_DOWN, 0.3f); push(3.2f, L, ORB_UP, 0.4f); push(4.4f, L, ORB_DOWN, 0.4f); }
}

/* -- 3. the best stream: the title's page with the streams sheet as the app draws it (its header, its addon tabs, the
      best row first, then the addon's rows); each point rules out the streams it would not play well, in turn, until
      the best one is left; the last point measures the line and switches it on -- */
typedef struct { const char *nm, *ds, *why; int by; } SRow; /* by: the point that rules it out (-1 none) */
static void title_page(void); static int sheetOnly;
static void sheet(float sel, float hp, int upTo, float cur, float bestK) {
  const Item *it = si(5); float SWS = 960, x0 = SB_W - SWS + 52, LWS = SWS - 104;
  if (!sheetOnly) title_page();
  gfx_shadow(SB_W - SWS - 24, 0, SWS + 40, SB_H, 32, 70, BLACK_A(128)); gfx_rrect(SB_W - SWS, 0, SWS + 40, SB_H, 32, RGBA(22, 20, 30, 236)); gfx_rrect_v(SB_W - SWS, 0, SWS + 40, SB_H, 32, WHITE_A(14), WHITE_A(0));
  float y = 52; text_draw(T("Streams"), x0, y + (30 - text_line_height(24)) / 2, 24, 600, WHITE_A(184), 0, 0, ALIGN_LEFT); y += 36;
  text_draw(it ? it->name : "", x0, y + (44 - text_line_height(36)) / 2, 36, 700, WHITE_A(255), -0.4f, LWS, ALIGN_LEFT);
  /* the addon tabs, All underlined */
  static const char *TB[3] = { "All", "Addon 1", "Addon 2" }, *TN[3] = { "6", "4", "2" }; float cx = x0;
  for (int i = 0; i < 3; i++) { const char *l = i ? TB[i] : T(TB[i]); float w = 44 + text_width(l, 24, 600, 0) + 10 + text_width(TN[i], 24, 600, 0), ty = 200 + (56 - text_line_height(24)) / 2;
    float lw = text_draw(l, cx + 22, ty, 24, 600, i ? WHITE_A(191) : WHITE_A(255), 0, 0, ALIGN_LEFT); text_draw(TN[i], cx + 22 + lw + 10, ty, 24, 600, col_alpha(i ? WHITE_A(191) : WHITE_A(255), 0.66f), 0, 0, ALIGN_LEFT);
    if (!i) gfx_rrect(cx + 20, 200 + 61, w - 40, 4, 2, acc_a(0.9f)); cx += w + 6; }
  char tvl[64]; const TvCaps *c = best_caps(); snprintf(tvl, sizeof tvl, "%dp", c && c->maxRes ? (c->maxRes >= 2160 ? 2160 : 1080) : 1080);
  static char d0[200]; snprintf(d0, sizeof d0, "1080p  \xc2\xb7  HDR10  \xc2\xb7  EAC3 5.1  \xc2\xb7  4.1 GB  \xc2\xb7  %s", T1("{n} seeds", "n", "1412"));
  static char w2[96]; snprintf(w2, sizeof w2, "%s", T1("Above your TV\xe2\x80\x99s {r}", "r", tvl));
  SRow R[6] = { { T("Best fit for this TV"), d0, NULL, -1 },
    { "4K  \xc2\xb7  DV", "Film.2024.2160p.BluRay.REMUX.DV.TrueHD.7.1\n58.2 GB  \xc2\xb7  212 seeds", T("Too heavy for your internet speed"), 0 },
    { "4K  \xc2\xb7  HDR10", "Film.2024.2160p.WEB-DL.HDR10.HEVC\n18.4 GB  \xc2\xb7  640 seeds", w2, 1 },
    { "1080p", "Film.2024.1080p.BluRay.x264.DTS-HD\n9.8 GB  \xc2\xb7  880 seeds", T("DTS sound: this TV can\xe2\x80\x99t play it"), 1 },
    { "1080p", "Film.2024.1080p.WEB-DL.HEVC.EAC3.5.1\n4.1 GB  \xc2\xb7  1412 seeds", NULL, -1 },
    { "720p", "Film.2024.720p.WEB.x264.AAC\n1.1 GB  \xc2\xb7  3 seeds", T("Only 3 seeds: slow to start"), 2 } };
  y = 294; float ys[6], hs[6];
  for (int i = 0; i < 6; i++) { if (i == 1) { text_draw("Addon 1", x0 + 22, y + 26, 24, 600, WHITE_A(184), 0, LWS - 44, ALIGN_LEFT); y += 26 + text_line_height(24) + 10; }
    int nd = R[i].ds[0] ? text_wrap_count(R[i].ds, LWS - 48, 6, 26, 400, 0) : 0, out = R[i].by >= 0 && R[i].by <= upTo; float h = 18 + 35 + 8 + nd * 37 + (out ? 37 : 0) + 20; ys[i] = y; hs[i] = h; y += h + 8; }
  for (int i = 0; i < 6; i++) gfx_rrect(x0, ys[i], LWS, hs[i], 18, WHITE_A(14));
  if (sel >= 0) { int a = (int)sel; float f = sel - a; int b = a < 5 ? a + 1 : 5; float py = ys[a] + (ys[b] - ys[a]) * f, ph = hs[a] + (hs[b] - hs[a]) * f;
    glow(x0 + LWS / 2, py + ph / 2, LWS + 60, ph + 80, ACC, 0.34f + 0.2f * bestK); gfx_rrect(x0, py, LWS, ph, 18, ACC | 0xff000000u);
    if (hp > 0) hold_trace_draw(x0, py, LWS, ph, 18, ease_apply(EASE_IN_OUT, hp)); }
  for (int i = 0; i < 6; i++) { int out = R[i].by >= 0 && R[i].by <= upTo, now = out && R[i].by == (int)(cur + 0.5f); float k = sel >= 0 ? 1 - fabsf(i - sel) : 0; k = k < 0 ? 0 : k;
    float oa = out ? (now ? 0.55f + 0.45f * (1 - cl01(cur - R[i].by)) : 0.4f) : 1; gfx_opacity_push(oa);
    float tx = x0 + 24, ty = ys[i] + 18; uint32_t nc = col_mix(ACC3 | 0xff000000u, ACC_FG | 0xff000000u, k);
    if (i == 0) { icon_draw("play", tx, ty + 5, 26, nc); tx += 36; }
    text_draw(R[i].nm, tx, ty + (35 - text_line_height(27)) / 2, 27, 600, nc, 0.2f, LWS - 60, ALIGN_LEFT); ty += 35 + 8;
    ty += text_draw_wrapped(R[i].ds, x0 + 24, ty + (37 - text_line_height(26)) / 2, LWS - 48, 6, 26, 400, col_mix(WHITE_A(204), ACC_FG | 0xff000000u, k), 0, ALIGN_LEFT, 37) * 37.0f;
    gfx_opacity_pop();
    if (out) { gfx_opacity_push(now ? 1 : 0.6f); icon_draw("minus", x0 + 24, ty + 6, 26, RGB(0xff, 0x9a, 0x7a)); text_draw(R[i].why, x0 + 60, ty + (37 - text_line_height(25)) / 2, 25, 700, RGB(0xff, 0x9a, 0x7a), 0, LWS - 90, ALIGN_LEFT); gfx_opacity_pop(); } }
  /* what it reads, on the title's page: the points so far, the current one lit */
  char sp[80]; const char *m = store_get("netMbps", ""); snprintf(sp, sizeof sp, "%s", m[0] && strcmp(m, "0") ? T1("{n} Mbps", "n", m) : T("Measured once"));
  char tv[96]; snprintf(tv, sizeof tv, "%s  \xc2\xb7  HEVC  \xc2\xb7  %s", tvl, c && !c->hdr ? "SDR" : "HDR10");
  const char *FT[3] = { T("Your internet"), T("Your TV"), T("Peers") }, *FV[3] = { sp, tv, T("More seeds, faster start") }, *FI[3] = { "g_speed", "p_asp", "g_stack" };
  for (int i = 0; i <= upTo && i < 3; i++) { float a = i < upTo ? 1 : eo(0, 0.35f, cur - i + 0.35f), fy = 560 + i * 128, on = 1 - cl01(fabsf(cur - i));
    gfx_opacity_push(a); gfx_offset_push(-20 * (1 - a), 0); gfx_rrect(64, fy, 760, 108, 26, col_mix(RGBA(26, 23, 36, 230), RGBA(40, 34, 60, 245), on)); gfx_ring(64, fy, 760, 108, 26, on > 0.5f ? 2 : 1, on > 0.5f ? col_alpha(ACC2, 0.8f) : WHITE_A(36));
    icon_draw(FI[i], 96, fy + 36, 36, ACC2); text_draw(FT[i], 152, fy + 22, 24, 700, WHITE_A(170), 0.6f, 0, ALIGN_LEFT); text_draw(FV[i], 152, fy + 56, 30, 700, WHITE_A(250), -0.2f, 640, ALIGN_LEFT);
    gfx_offset_pop(); gfx_opacity_pop(); }
}
/* the switch, in the app's own way: a small glass pill at the foot carries the app's loading line while the line is
   measured, then the speed with a tick; the best row lights, a few purple sparks rise */
/* the speed test, over the whole picture while it runs and a moment after: its title, the speed so far in big
   figures, a line that fills as the test goes, why it is done; then the result with a tick, and back to the page */
static void speed_card(void) {
  int busy = best_speed_busy(), on = best_on(); float since = W.doneAt > 0 ? (float)(app_now() - W.doneAt) : 99;
  static Tween ca; int want = busy || (on && since < 2.6f); tween_to(&ca, want ? 1 : 0, want ? 0.3f : 0.4f, EASE_IN_OUT); tween_tick(&ca, app_dt()); float a = ca.v; if (a <= 0.002f) return;
  gfx_opacity_push(a); gfx_rect(0, 0, SB_W, SB_H, BLACK_A(150)); gfx_scale_push(1.3f, 1.3f, SB_W / 2, SB_H / 2); /* (big in the picture: it reads at its size) */
  float w = 820, h = 430, x = (SB_W - w) / 2, y = (SB_H - h) / 2 + 16 * (1 - a);
  gfx_shadow(x, y + 24, w, h, 36, 70, BLACK_A(170)); gfx_rrect(x, y, w, h, 36, RGBA(24, 21, 34, 250)); gfx_rrect_v(x, y, w, h * 0.5f, 36, WHITE_A(16), WHITE_A(0)); gfx_ring(x, y, w, h, 36, 1, busy ? WHITE_A(40) : col_alpha(ACC2, 0.7f));
  glow(x + w / 2, y + 190, 560, 300, ACC, busy ? 0.18f + 0.08f * sinf((float)app_now() * 2.4f) : 0.3f);
  text_draw(busy ? T("Testing your internet speed") : T("Your internet speed"), x + w / 2, y + 44, 30, 700, WHITE_A(240), -0.2f, 0, ALIGN_CENTER);
  double mb = busy ? best_speed_live() : best_net_mbps(); char n[24]; if (mb > 0) snprintf(n, sizeof n, "%.0f", mb); else snprintf(n, sizeof n, "\xe2\x80\x94");
  float nw = text_width(n, 120, 800, -3.0f), uw = text_width("Mbps", 34, 700, 0), nx = x + w / 2 - (nw + 16 + uw) / 2, ny = y + 104;
  text_draw(n, nx, ny, 120, 800, WHITE_A(255), -3.0f, 0, ALIGN_LEFT); text_draw("Mbps", nx + nw + 16, ny + text_line_height(120) - text_line_height(34) - 18, 34, 700, ACC2 | 0xff000000u, 0, 0, ALIGN_LEFT);
  float bx = x + 90, bw = w - 180, by = y + 284;
  gfx_rrect(bx, by, bw, 8, 4, WHITE_A(30));
  if (busy) { double sc = best_speed_since(); float p = sc > 0 ? (float)(1 - exp(-sc / 4.0)) : 0; gfx_rrect_h(bx, by, bw * p, 8, 4, ACC | 0xff000000u, ACC2 | 0xff000000u); /* (it eases towards the end as it goes) */
    float ph = fmodf((float)app_now() / 1.2f, 1), sx = bx + bw * p * ph; gfx_clip_push(bx, by, bw * p, 8); gfx_rrect_h(sx - 40, by, 40, 8, 0, WHITE_A(0), WHITE_A(90)); gfx_clip_pop(); }
  else { gfx_rrect_h(bx, by, bw, 8, 4, ACC | 0xff000000u, ACC2 | 0xff000000u); float s2 = 0.6f + 0.4f * ease_apply(EASE_OUT_BACK, eo(0, 0.35f, since)), cx = x + w - 70, cy = y + 64; gfx_scale_push(s2, s2, cx, cy); gfx_circle(cx, cy, 24, ACC | 0xff000000u); icon_draw("check", cx - 14, cy - 14, 28, ACC_FG); gfx_scale_pop(); }
  text_draw_wrapped(busy ? T("So Play can pick a stream that never stutters") : T("Play now starts the best stream"), x + w / 2, by + 36, w - 140, 2, 26, 500, WHITE_A(190), 0, ALIGN_CENTER, 34);
  gfx_scale_pop(); gfx_opacity_pop();
}
static void speed_pill(void) {
  int busy = best_speed_busy(), on = best_on(); float since = W.doneAt > 0 ? (float)(app_now() - W.doneAt) : 99;
  char sp[120]; snprintf(sp, sizeof sp, "%s", T1("{n} Mbps \xc2\xb7 Play starts the best stream", "n", store_get("netMbps", "0")));
  const char *l = busy ? T("Measuring your connection\xe2\x80\xa6") : on ? sp : W.speedFail ? W.speed : T("Off: press OK to turn it on");
  float w = text_width(l, 30, 700, 0) + (on || busy ? 120 : 72), h = 84, x = 64 + 380 - w / 2, y = SB_H - 150, k = on ? eo(0, 0.35f, since) : 1;
  gfx_scale_push(1.25f, 1.25f, x + w / 2, y + h / 2); /* (bigger in the picture) */
  if (x < 40) x = 40;
  gfx_shadow(x, y + 16, w, h, h / 2, 40, BLACK_A(150)); gfx_rrect(x, y, w, h, h / 2, RGBA(26, 23, 36, 245)); gfx_rrect_v(x, y, w, h, h / 2, WHITE_A(18), WHITE_A(0)); gfx_ring(x, y, w, h, h / 2, 1, on ? col_alpha(ACC2, 0.6f) : WHITE_A(36));
  float tx = x + (on || busy ? 96 : 36);
  if (busy) { float lx = x + 30, ly = y + h / 2 - 1.5f, ph = fmodf((float)app_now() / 1.1f, 1), p = ease_apply(EASE_IN_OUT, ph), sx = lx + 46 * 0.4f * (-1 + 3.5f * p); /* the app's loading line, small */
    gfx_rrect(lx, ly, 46, 3, 1.5f, WHITE_A(40)); gfx_clip_push(lx, ly, 46, 3); gfx_rrect_h(sx, ly, 20, 3, 1.5f, col_alpha(ACC2, 0), ACC2); gfx_rrect_h(sx + 20, ly, 20, 3, 1.5f, ACC2, col_alpha(ACC2, 0)); gfx_clip_pop(); }
  if (on) { float cx = x + 52, cy = y + h / 2, s = 0.6f + 0.4f * ease_apply(EASE_OUT_BACK, k); gfx_scale_push(s, s, cx, cy); gfx_circle(cx, cy, 24, ACC | 0xff000000u); icon_draw("check", cx - 14, cy - 14, 28, ACC_FG); gfx_scale_pop(); }
  gfx_opacity_push(on ? k : 1); text_draw(l, tx, y + h / 2 - text_line_height(30) / 2.0f, 30, 700, WHITE_A(250), -0.2f, 0, ALIGN_LEFT); gfx_opacity_pop();
  if (on && since < 1.8f) for (int i = 0; i < 16; i++) { float an = -3.14159f * (0.15f + 0.7f * rnd(i)), v = 160 + 160 * rnd(i + 40), tt = since, px = x + w / 2 + (rnd(i + 9) - 0.5f) * w * 0.8f + cosf(an) * v * tt, py = y + sinf(an) * v * tt + 60 * tt * tt;
    gfx_opacity_push(0.7f * (1 - tt / 1.8f)); gfx_circle(px, py, 3 + 3 * rnd(i + 5), i % 2 ? ACC2 : ACC); gfx_opacity_pop(); }
  gfx_scale_pop();
}
/* the title page's buttons exactly as it draws them (76 round, 16 apart), Play focused and held k of the way: the
   words turn to Hold for streams at once and the pill's own lighter shape fills it; the hint after the row at rest */
static void play_row(float x, float y, float k) {
  const float BDw = 76, BGw = 16; float lk = cl01(k * 6), wp = text_width(T("Play"), 26, 600, 0), wa = text_width(T("Hold for streams"), 26, 600, 0), w = BDw + wp + (wa - wp) * lk + 26;
  gfx_rrect(x, y, w, BDw, BDw / 2, ACC | 0xff000000u);
  if (k > 0) { gfx_clip_push(x, y, w * k, BDw); gfx_rrect(x, y, w, BDw, BDw / 2, WHITE_A(70)); gfx_clip_pop(); }
  icon_draw("play", x + 21, y + 21, 34, ACC_FG);
  gfx_opacity_push(1 - lk); text_draw(T("Play"), x + 66, y + (BDw - text_line_height(26)) / 2, 26, 600, ACC_FG, 0, 0, ALIGN_LEFT); gfx_opacity_pop();
  if (lk > 0) { gfx_opacity_push(lk); text_draw(T("Hold for streams"), x + 66, y + (BDw - text_line_height(26)) / 2, 26, 600, ACC_FG, 0, 0, ALIGN_LEFT); gfx_opacity_pop(); }
  static const char *IC[5] = { "trailer", "similar", "plus", "reviews", "eye" }; float bx = x + w + BGw;
  for (int i = 0; i < 5; i++) { gfx_circle(bx + BDw / 2, y + BDw / 2, BDw / 2, WHITE_A(46)); icon_draw(IC[i], bx + 21, y + 21, 34, WHITE_A(255)); bx += BDw + BGw; }
  if (k <= 0) key_hint(bx + 12, y + BDw / 2, K_OK, T("Hold for streams"), WHITE_A(170), 0);
}
/* the title page as the streams' sheet opens over it (its left side): the backdrop, the logo, the line, the words */
static void title_page(void) { const Item *it = si(5); backdrop(5, 1); logo(5, 64, 300, 520, 170); if (it) { meta_draw(it->metaLine, "", it->name, 64, 330, 25, WHITE_A(220), ALIGN_LEFT); text_draw_wrapped(it->description, 64, 384, 760, 3, 27, 400, WHITE_A(200), 0, ALIGN_LEFT, 40); } }
/* (sheetOnly: the sheet alone, over a page already drawn) */
static void sc_streams(int st, float t) {
  if (st == 3) { float L = 5.6f; t = fmodf(t, L); float k = ease_apply(EASE_IN_OUT, cl01((t - 1.0f) / 1.0f)), e = ease_apply(EASE_OUT, cl01((t - 2.05f) / 0.45f)) * (1 - ease_apply(EASE_IN_OUT, cl01((t - 4.9f) / 0.45f)));
    title_page(); play_row(64, 540, t < 2.05f ? k : 0); push(1.0f, L, ORB_HOLD, 0.6f);
    if (e > 0.001f) { sheetOnly = 1; gfx_offset_push(960 * (1 - e), 0); sheet(0, 0, -1, -1, 0); gfx_offset_pop(); sheetOnly = 0; } /* full: the streams slide in over the page, every one */
    return; }
  if (st < 3) { float L = 5.0f; t = fmodf(t, L); float cur = st - 1 + eo(0.5f, 1.2f, t); /* this point's streams fall out as it comes in */
    sheet(0, 0, st, cur, 0); push(0.9f, L, ORB_DONE, 0.4f); return; }
  float L = 6.0f; t = fmodf(t, L); int on = best_on();
  sheet(0, 0, 2, 2, on ? 0.5f + 0.5f * sinf(t * 2.4f) : 0); if (!best_speed_busy()) speed_pill(); speed_card(); /* (measuring: the card says so, big) */
}

/* -- 4. subtitles, in the player -- */
/* the player's controls as it draws them: the shades, the title, its chips, the bar, the buttons (fb: the focused
   button's id, NULL none), the peers chip at the top (NULL none; usbLit: its USB lit) */
static void osd(float a, const char *fb, const char *peers, float usbLit) {
  if (a <= 0.004f) return; gfx_opacity_push(a);
  float W0 = SB_W, H = SB_H, bx = 72, rowTop = H - 54 - 94, L25 = text_line_height(25), barTop = rowTop - 30 - L25, trkC = barTop + L25 / 2, ttlBot = barTop - 32;
  gfx_rrect_v(0, H * 0.44f, W0, H * 0.3f, 0, BLACK_A(0), BLACK_A(140)); gfx_rrect_v(0, H * 0.74f, W0, H * 0.26f, 0, BLACK_A(140), BLACK_A(224)); gfx_rrect_v(0, 0, W0, 240, 0, BLACK_A(140), BLACK_A(0));
  app_clock_draw(W0 - 64, 46, 1);
#ifdef SB_LINUX
  app_fsbtn_draw(W0 - 64, 46);
#endif
  if (peers) { float tw = text_width(peers, 24, 600, 0), w = 16 + 10 + 12 + tw + 24;
    lglass_shell(72, 52, w, 46, 23, RGBA(10, 12, 22, 150), 1); gfx_circle(72 + 2 + 16 + 5, 52 + 23, 5, RGB(0x4a, 0xde, 0x80));
    text_draw(peers, 72 + 40, 52 + (46 - text_line_height(24)) / 2, 24, 600, WHITE_A(255), 0, 0, ALIGN_LEFT);
    if (usbLit > 0) { float uw = text_width("USB", 24, 600, 0), ux = 72 + 40 + tw - uw; gfx_rrect(ux - 8, 58, uw + 16, 34, 10, col_alpha(ACC2, 0.3f * usbLit)); gfx_ring_out(ux - 8, 58, uw + 16, 34, 10, 2, col_alpha(ACC2, usbLit)); } }
  /* the title, its chips and its line, from the bar up */
  float mtTop = ttlBot - L25, infTop = mtTop - 14 - 46; logo(4, bx, infTop - 18, 560, 170);
  static const char *CH[3] = { "4K", "HDR10", "EAC3 5.1" }; float cx = bx;
  for (int i = 0; i < 3; i++) { float w = 40 + text_width(CH[i], 24, i ? 600 : 800, i ? 0 : 0.5f); gfx_rrect(cx, infTop, w, 46, 23, RGBA(10, 12, 22, 140)); gfx_ring(cx, infTop, w, 46, 23, 2, WHITE_A(56));
    text_draw(CH[i], cx + 20, infTop + (46 - text_line_height(24)) / 2, 24, i ? 600 : 800, WHITE_A(255), i ? 0 : 0.5f, 0, ALIGN_LEFT); cx += w + 12; }
  const Item *it = si(4); if (it) text_draw(it->metaLine, bx, mtTop, 25, 500, WHITE_A(191), 0, 1200, ALIGN_LEFT);
  /* the bar */
  float tx = bx + 152, tw = W0 - 2 * bx - 304, p = 0.38f + 0.002f * (float)fmod(app_now(), 10);
  text_draw("1:02:15", bx, barTop, 25, 600, WHITE_A(255), 0, 0, ALIGN_LEFT); text_draw("-38:41", W0 - bx, barTop, 25, 600, WHITE_A(184), 0, 0, ALIGN_RIGHT);
  gfx_rrect(tx, trkC - 3, tw, 6, 3, WHITE_A(51)); gfx_rrect(tx, trkC - 3, tw * (p + 0.12f), 6, 3, WHITE_A(66)); gfx_rrect_h(tx, trkC - 3, tw * p, 6, 3, ACC, ACC2); gfx_circle(tx + tw * p, trkC, 9, WHITE_A(255));
  /* the buttons: play and from the beginning on the left; sources, subtitles, audio, picture size on the right; the
     focused one's accent pill carries its name, the ones before it step aside */
  static const char *ID[6] = { "pp", "beg", "src", "subs", "aud", "asp" }, *ICN[6] = { "p_pause", "restart", "p_src", "p_subs", "p_aud", "p_asp" }, *NM[6] = { "", "Play from the beginning", "Sources", "Subtitles", "Audio", "Original" };
  float xs[6], ws[6]; int fi = -1; for (int i = 0; i < 6; i++) { ws[i] = i ? 78 : 94; if (fb && !strcmp(fb, ID[i])) fi = i; }
  float lw = fi > 0 ? text_width(T(NM[fi]), 24, 700, 0) + 22 : 0;
  xs[0] = 72; xs[1] = 72 + 94 + 20 + 8;
  float x = W0 - 72; for (int i = 5; i >= 2; i--) { x -= ws[i]; if (i == fi) x -= lw; xs[i] = x; x -= 18; }
  for (int i = 0; i < 6; i++) { float sz = ws[i], y = i ? rowTop + 8 : rowTop; int f = i == fi;
    if (f) { gfx_rrect(xs[i], y, sz + lw, sz, sz / 2, ACC | 0xff000000u); text_draw(T(NM[i]), xs[i] + 70, y + sz / 2 - text_line_height(24) / 2.0f, 24, 700, ACC_FG, 0, 0, ALIGN_LEFT); }
    else { gfx_circle(xs[i] + sz / 2, y + sz / 2, sz / 2, WHITE_A(26)); gfx_ring(xs[i], y, sz, sz, sz / 2, 1, WHITE_A(26)); }
    float is = i ? 34 : 48; icon_draw(ICN[i], xs[i] + sz / 2 - is / 2, y + sz / 2 - is / 2, is, f ? ACC_FG : WHITE_A(255)); }
  gfx_opacity_pop();
}
/* the Subtitles tray as the player draws it: its tabs (Subtitles, Addons; tab 0..1 between them), four columns, the
   lit row of the focused column (col, row; col -1: the tabs have the focus) */
static void subs_tray(float open, float tab, int col, float row) {
  if (open <= 0.004f) return; float L24 = text_line_height(24), L18 = text_line_height(21), colH = 6 + (6 + L18 + 14) + 294 + 4;
  float W0 = 56 + 4 * 282 - 10, hdH = 20 + L24 + 18, H = 2 + 22 + hdH + 8 + colH + 8 + 20, x = SB_W - 72 - W0, y = SB_H - 178 - H, sc = 0.94f + 0.06f * open;
  gfx_opacity_push(open); gfx_scale_push(sc, sc, x + W0 - 200, SB_H - 100);
  gfx_rrect(x, y, W0, H, 28, RGBA(18, 16, 28, 214)); gfx_rrect_v(x, y, W0, H, 28, WHITE_A(20), WHITE_A(5)); gfx_ring(x, y, W0, H, 28, 1, WHITE_A(31));
  const char *TB[2] = { T("Subtitles"), T("Addons") }; float tbw[2], tbx[2], tx = x + 31; for (int i = 0; i < 2; i++) { tbw[i] = 48 + text_width(TB[i], 24, 700, 0); tbx[i] = tx; tx += tbw[i] + 10; }
  float cy = y + 23, bh = 20 + L24, px = tbx[0] + (tbx[1] - tbx[0]) * tab, pw = tbw[0] + (tbw[1] - tbw[0]) * tab, tf = col < 0 ? 1 : 0;
  lit_box(px, cy, pw, bh, 24, col_mix(WHITE_A(26), ACC | 0xff000000u, tf), tf > 0.5f ? ok_press() : 0); if (tf) gfx_ring_out(px, cy, pw, bh, 24, 4, acc_a(0.38f));
  for (int i = 0; i < 2; i++) { float u = 1 - fabsf(i - tab); u = u < 0 ? 0 : u; text_draw(TB[i], tbx[i] + tbw[i] / 2, cy + 10, 24, 700, col_mix(WHITE_A(191), tf ? ACC_FG : WHITE_A(255), u), 0, 0, ALIGN_CENTER); }
  cy += bh + 18;
  const char *CT[4] = { tab < 0.5f ? T("In the file") : T("Language"), tab < 0.5f ? "OpenSubtitles" : T("English"), T("Style"), T("Delay") };
  static const char *A0[3] = { "Off", "English", "English (SDH)" }, *A1[4] = { "English", "French", "Spanish", "Arabic" };
  static const char *B0[4] = { "English \xc2\xb7 WEB-DL", "English \xc2\xb7 BluRay", "English \xc2\xb7 SDH", "English \xc2\xb7 HDTV" }, *B1[3] = { "Addon 1 \xc2\xb7 WEB-DL", "Addon 2 \xc2\xb7 BluRay", "Addon 1 \xc2\xb7 SDH" };
  static const char *C0[3] = { "Size \xc2\xb7 100%", "Colour", "Background" }, *D0[4] = { "Auto-sync", "\xe2\x88\x92\x30.5 s", "+0.5 s", "Reset" };
  for (int c = 0; c < 4; c++) { float colX = x + 1 + 22 + 5 + c * 282, colTop = cy + 8; int cur = c == col;
    if (cur) gfx_rrect(colX, colTop, 272, colH, 22, WHITE_A(10));
    char h2[64]; snprintf(h2, sizeof h2, "%s", CT[c]); for (char *q = h2; *q; q++) if (*q >= 'a' && *q <= 'z') *q -= 32;
    text_draw(h2, colX + 18, colTop + 12, 21, 800, cur ? ACC2 : WHITE_A(184), 1.8f, 236, ALIGN_LEFT);
    int n = c == 0 ? (tab < 0.5f ? 3 : 4) : c == 1 ? (tab < 0.5f ? 4 : 3) : c == 2 ? 3 : 4; float iy = colTop + 6 + 6 + L18 + 14 + 2;
    for (int i = 0; i < n; i++) { const char *l = c == 0 ? (tab < 0.5f ? T(A0[i]) : T(A1[i])) : c == 1 ? (tab < 0.5f ? B0[i] : B1[i]) : c == 2 ? T(C0[i]) : D0[i];
      float k = cur ? 1 - fabsf(i - row) : 0; k = k < 0 ? 0 : k; int on = (c == 0 && i == (tab < 0.5f ? 1 : 0)) || (c == 1 && i == 0 && !cur);
      if (k > 0.01f) gfx_rrect(colX + 10, iy, 252, 56, 16, col_alpha(ACC | 0xff000000u, k));
      text_draw(l, colX + 28, iy + 28 - text_line_height(23) / 2.0f, 23, 600, col_mix(on ? WHITE_A(255) : WHITE_A(205), ACC_FG | 0xff000000u, k), 0, 190, ALIGN_LEFT);
      if (on) icon_draw("check", colX + 226, iy + 16, 24, col_mix(ACC2 | 0xff000000u, ACC_FG | 0xff000000u, k));
      iy += 66; } }
  gfx_scale_pop(); gfx_opacity_pop();
}
static void sub_line(const char *s, float y, float a) { /* the app's own subtitle: white, a soft dark edge */
  if (a <= 0.004f) return; gfx_opacity_push(a);
  for (int dx = -2; dx <= 2; dx += 2) for (int dy = -2; dy <= 2; dy += 2) if (dx || dy) text_draw(s, SB_W / 2 + dx, y + dy, 50, 600, BLACK_A(230), 0, SB_W - 300, ALIGN_CENTER);
  text_draw(s, SB_W / 2, y, 50, 600, WHITE_A(250), 0, SB_W - 300, ALIGN_CENTER); gfx_opacity_pop();
}
static void film(int i) { backdrop(i, 0); }
static void chip_tr(const char *s, const char *icon, float a, float spin) { /* a notice chip at the player's top right */
  if (a <= 0.004f) return; float w = text_width(s, 26, 700, 0) + 92, h = 60, x = SB_W - 64 - w, y = 120; gfx_opacity_push(a); gfx_offset_push(0, -12 * (1 - a));
  lglass_shell(x, y, w, h, h / 2, RGBA(10, 12, 22, 170), 1);
  if (spin >= 0) { for (int k = 0; k < 10; k++) { float an = spin * 6.2832f + k * 0.55f; gfx_circle(x + 36 + cosf(an) * 11, y + h / 2 + sinf(an) * 11, 1.5f + 0.15f * k, col_alpha(ACC2, 0.2f + 0.08f * k)); } }
  else icon_draw(icon, x + 22, y + h / 2 - 14, 28, ACC2);
  text_draw(s, x + 64, y + h / 2 - text_line_height(26) / 2.0f, 26, 700, WHITE_A(245), 0, 0, ALIGN_LEFT); gfx_offset_pop(); gfx_opacity_pop();
}
static void outlined(const char *s, float x, float y, int px, int wt, uint32_t fill, uint32_t edge, float r, float sp, int al) { /* a line with a thick round edge, as styled subtitles draw them */
  for (int k = 0; k < 16; k++) { float an = k * 0.3927f; text_draw(s, x + cosf(an) * r, y + sinf(an) * r, px, wt, edge, sp, 0, al); }
  text_draw(s, x, y, px, wt, fill, sp, 0, al);
}
static void sc_subs(int st, float t) {
  if (st == 0) { float L = 8.0f; t = fmodf(t, L); film(4);
    float oa = 1 - sstep(5.4f, 5.8f, t), tr = eo(1.9f, 2.25f, t) * (1 - eo(5.0f, 5.35f, t)), row = glide(3.0f, t) + glide(3.5f, t);
    const char *fb = t < 0.8f ? NULL : t < 1.2f ? "src" : "subs";
    osd(oa, tr > 0.01f ? "subs" : fb, NULL, 0); subs_tray(tr, 0, t < 2.6f ? -1 : 1, row);
    sub_line(T("I came back to tell you something."), SB_H - 260, sstep(5.6f, 5.9f, t) * (1 - sstep(7.6f, 7.9f, t)));
    push(0.8f, L, ORB_RIGHT, 0.3f); push(1.2f, L, ORB_RIGHT, 0.3f); push(1.9f, L, ORB_OK, 0.6f); push(4.9f, L, ORB_OK, 0.6f);
    press(K_OK, T("OK"), vis(t, 4.5f, 5.4f, 0.2f), (t - 4.8f) / 0.35f, 520, 200); return; }
  if (st == 1) { float L = 5.0f; t = fmodf(t, L); film(4); osd(1, "subs", NULL, 0);
    float tab = glide(1.0f, t) - ease_apply(GLIDE_EASE, (t - 4.2f) / GLIDE_DUR); subs_tray(1, tab, -1, 0); push(1.0f, L, ORB_TAB_R, 0.7f);
    press(K_RIGHT, T("Addons"), vis(t, 0.6f, 1.8f, 0.2f), (t - 0.9f) / 0.35f, 520, 200); return; }
  if (st == 2) { float L = 7.0f; t = fmodf(t, L); film(4);
    /* the speech and the subtitle's lines as two tracks: the lines run early, then slide onto the speech */
    float fix = eo(2.6f, 3.6f, t), off = 120 * (1 - fix);
    float bx = 260, by = 640, bw = SB_W - 520; gfx_rrect(bx - 30, by - 50, bw + 60, 270, 28, RGBA(14, 12, 22, 210)); gfx_ring(bx - 30, by - 50, bw + 60, 270, 28, 1, WHITE_A(36));
    text_draw(T("Speech"), bx, by - 28, 24, 700, WHITE_A(180), 1.2f, 0, ALIGN_LEFT); text_draw(T("Subtitles"), bx, by + 84, 24, 700, WHITE_A(180), 1.2f, 0, ALIGN_LEFT);
    static const float SEG[5][2] = { { 0.05f, 0.16f }, { 0.22f, 0.37f }, { 0.44f, 0.52f }, { 0.6f, 0.78f }, { 0.84f, 0.95f } };
    for (int i = 0; i < 5; i++) { float a = bx + SEG[i][0] * bw, w = (SEG[i][1] - SEG[i][0]) * bw;
      for (int k = 0; k < (int)(w / 9); k++) { float hh = 10 + 34 * fabsf(sinf(k * 0.9f + i)); gfx_rrect(a + k * 9, by + 34 - hh / 2, 5, hh, 2.5f, WHITE_A(150)); }
      gfx_rrect(a - off, by + 120, w, 40, 10, col_mix(RGB(0xff, 0x8a, 0x5a), ACC2 | 0xff000000u, fix)); }
    gfx_rect(bx + bw * (t / L), by - 30, 2, 230, WHITE_A(200));
    sub_line(t < 3.4f ? T("\xe2\x80\xa6you never even said goodbye.") : T("I came back to tell you something."), 330, 1);
    chip_tr(T("Auto-sync: lining it up\xe2\x80\xa6"), NULL, vis(t, 0.5f, 3.4f, 0.2f), fmodf(t, 1.0f));
    chip_tr(T("In sync \xc2\xb7 2.4 s earlier"), "check", vis(t, 3.4f, 6.6f, 0.2f), -1);
    push(3.4f, L, ORB_DONE, 0.6f); return; }
  /* styled (ASS) subtitles: a sign placed on the picture in its own font and colours, sliding in; a karaoke line
     filling word by word; a translator's note at the top in another colour */
  { float L = 8.0f; t = fmodf(t, L); film(6);
    float sg = eo(0.3f, 1.0f, t) * (1 - sstep(7.4f, 7.9f, t)), sx = 1400 + 70 * (1 - sg);
    gfx_opacity_push(sg);
    outlined(T("THE OLD LIGHTHOUSE"), sx, 250, 66, 800, RGB(0xff, 0xd4, 0x5a), RGB(0x3a, 0x12, 0x5c), 6, 3, ALIGN_CENTER);
    outlined(T("Open since 1904"), sx, 346, 40, 600, WHITE_A(250), RGB(0x3a, 0x12, 0x5c), 3, 1, ALIGN_CENTER); gfx_opacity_pop();
    float na = vis(t, 1.4f, 7.6f, 0.3f); if (na > 0) { gfx_opacity_push(na); outlined(T("The town\xe2\x80\x99s name means \xe2\x80\x9cquiet harbour\xe2\x80\x9d"), SB_W / 2, 84, 36, 600, RGB(0xc8, 0xe6, 0xff), RGB(0x06, 0x1a, 0x30), 3, 0, ALIGN_CENTER); gfx_opacity_pop(); }
    const char *k = T("Sing it with me tonight"); float kw = text_width(k, 60, 800, 0), kx = SB_W / 2 - kw / 2, ky = SB_H - 200, p = cl01((t - 2.0f) / 3.6f), ka = vis(t, 1.6f, 7.6f, 0.3f);
    gfx_opacity_push(ka); outlined(k, kx, ky, 60, 800, WHITE_A(250), RGB(0x10, 0x0c, 0x22), 4, 0, ALIGN_LEFT);
    gfx_clip_push(kx - 10, ky - 20, kw * p + 10, 120); outlined(k, kx, ky, 60, 800, RGB(0xff, 0x6b, 0xc8), RGB(0x10, 0x0c, 0x22), 4, 0, ALIGN_LEFT); gfx_clip_pop();
    if (p > 0 && p < 1) glow(kx + kw * p, ky + 30, 200, 160, RGB(0xff, 0x6b, 0xc8), 0.5f);
    gfx_opacity_pop();
    chip_tr(T("Styled subtitles"), "g_type", vis(t, 0.2f, 7.6f, 0.3f), -1);
    for (int w = 0; w < 5; w++) push(2.0f + 0.72f * w, L, ORB_UP, 0.2f); }
}

/* -- 5. torrents -- */
static void notice(float a, const char *s) { /* the app's notice capsule out of the Settings icon, with the red key */
  if (a <= 0.004f) return; float w = text_width(s, 26, 700, 0) + 46 + 30 + 40, x = 40, y = 12; gfx_opacity_push(a);
  lglass_shell(x, y, 76 + w * a, 76, 38, RGBA(14, 12, 22, 200), 1);
  icon_draw("nav_set", x + 18, y + 18, 40, WHITE_A(255)); text_draw(s, x + 96, y + 38 - text_line_height(26) / 2.0f, 26, 700, WHITE_A(250), 0, 0, ALIGN_LEFT);
  key_hint(x + 96 + text_width(s, 26, 700, 0) + 20, y + 38, K_RED, NULL, WHITE_A(250), 0); gfx_opacity_pop();
}
static void sc_torrents(int st, float t) {
  if (st == 0) { /* the film is big, the part in memory small and moving along it */
    float L = 7.0f; t = fmodf(t, L); film(4); osd(1, NULL, T1("{n} peers", "n", "48"), 0);
    float cx = SB_W / 2, y = 330, w = 1300, x = cx - w / 2, a = eo(0.2f, 0.8f, t);
    gfx_opacity_push(a); gfx_rrect(x - 40, y - 80, w + 80, 230, 30, RGBA(16, 14, 24, 228)); gfx_ring(x - 40, y - 80, w + 80, 230, 30, 1, WHITE_A(36));
    text_draw("22.7 GB", x, y - 54, 30, 700, WHITE_A(200), 0, 0, ALIGN_LEFT);
    for (int i = 0; i < 60; i++) gfx_rrect(x + i * (w / 60), y, w / 60 - 4, 50, 5, WHITE_A(28));
    float p = 0.12f + 0.7f * cl01((t - 1.0f) / 5.5f), ww = w * 0.06f; glow(x + w * p + ww / 2, y + 25, 300, 200, ACC, 0.5f); gfx_rrect(x + w * p, y - 6, ww, 62, 10, ACC | 0xff000000u);
    text_draw("64 MB", x + w * p + ww / 2, y + 74, 30, 800, ACC2, 0, 0, ALIGN_CENTER);
    gfx_opacity_pop(); return; }
  { /* the drive found (the notice out of Settings), its row in Playback, OK, on; the peers chip says USB */
    float L = 7.0f; t = fmodf(t, L); float set = sstep(1.6f, 2.0f, t) * (1 - sstep(5.0f, 5.4f, t)), on = eo(3.2f, 3.5f, t);
    cam(t, 2.2f, 4.9f, 1240, 912, 1.7f); /* the whole of Settings first, then in on the USB row while it is turned on */
    film(4); osd(1, NULL, T1("{n} peers", "n", "48"), 0); notice(vis(t, 0.3f, 1.8f, 0.3f), T("USB drive found"));
    if (set > 0.004f) { gfx_scale_push(0.92f + 0.08f * set, 0.92f + 0.08f * set, 78, 50); settings_page(set, 5, on, 0); topbar(-1, -1, 1, 0, 0); gfx_scale_pop(); }
    if (t > 5.2f) { char pl[96]; snprintf(pl, sizeof pl, "%s \xc2\xb7 USB", T1("{n} peers", "n", "48")); gfx_opacity_push(sstep(5.2f, 5.5f, t)); osd(1, NULL, pl, 1); gfx_opacity_pop(); }
    push(0.4f, L, ORB_RED, 0.6f); push(1.6f, L, ORB_OK, 0.5f); push(3.2f, L, ORB_DONE, 0.8f); push(5.2f, L, ORB_BACK, 0.5f);
    press(K_OK, T("Turn on"), vis(t, 2.6f, 3.9f, 0.2f), (t - 3.0f) / 0.35f, 400, PRESS_Y); }
}

/* -- 6. the phone: the Search page as it is; a phone of the same glass comes up beside it, its camera finds the
      code, the page opens, and the word typed there lands in the search box -- */
static void phone_card(float x, float y, int st, float t) {
  float pw = 400, ph = 720, sx = x + 18, sy = y + 18, sw = pw - 36, sh = ph - 36;
  gfx_shadow(x, y + 30, pw, ph, 56, 70, BLACK_A(170)); gfx_rrect(x, y, pw, ph, 56, RGBA(30, 27, 40, 250)); gfx_rrect_v(x, y, pw, ph, 56, WHITE_A(22), WHITE_A(0)); gfx_ring(x, y, pw, ph, 56, 1.5f, WHITE_A(46));
  gfx_rrect(sx, sy, sw, sh, 40, RGB(0x10, 0x0e, 0x16)); gfx_rrect(x + pw / 2 - 40, y + 30, 80, 10, 5, WHITE_A(40));
  if (st < 2) { /* the camera: the code in its frame, found, then the page to open */
    float cs = 230, cx = sx + sw / 2 - cs / 2, cy = sy + 150, found = eo(1.0f, 1.4f, t), fr = 1.15f - 0.15f * found;
    qr_draw(qrUrl, cx, cy, cs, 14, 12);
    gfx_scale_push(fr, fr, cx + cs / 2, cy + cs / 2); uint32_t fc = col_mix(WHITE_A(230), ACC2 | 0xff000000u, found);
    for (int c = 0; c < 4; c++) { float ex = c & 1 ? cx + cs + 16 - 44 : cx - 16, ey = c & 2 ? cy + cs + 16 - 44 : cy - 16;
      gfx_rrect(c & 1 ? ex + 38 : ex, ey, 6, 44, 3, fc); gfx_rrect(ex, c & 2 ? ey + 38 : ey, 44, 6, 3, fc); }
    gfx_scale_pop();
    if (t < 1.0f) gfx_rrect(cx, cy + cs * fmodf(t * 1.4f, 1), cs, 4, 2, col_alpha(ACC2, 0.85f));
    float op = eo(1.5f, 1.9f, t); if (op > 0) { gfx_opacity_push(op); gfx_offset_push(0, 20 * (1 - op)); gfx_rrect(sx + 24, sy + sh - 130, sw - 48, 80, 40, ACC | 0xff000000u);
      text_draw(T("Open the page"), sx + sw / 2, sy + sh - 90 - text_line_height(28) / 2.0f, 28, 700, ACC_FG, 0, sw - 80, ALIGN_CENTER); gfx_offset_pop(); gfx_opacity_pop(); }
    return; }
  /* the page: a field and a keyboard, the word typed key by key */
  int typed = t < 0.6f ? 0 : t < 0.9f ? 1 : t < 1.2f ? 2 : t < 1.5f ? 3 : 4;
  glow(sx + 60, sy + 100, 520, 400, ACC, 0.45f); text_draw(T("Type here"), sx + 30, sy + 80, 40, 800, WHITE_A(255), -0.8f, sw - 60, ALIGN_LEFT);
  gfx_rrect(sx + 24, sy + 160, sw - 48, 76, 22, WHITE_A(26)); gfx_ring(sx + 24, sy + 160, sw - 48, 76, 22, 1, WHITE_A(50));
  char q[8]; snprintf(q, sizeof q, "%.*s", typed, "moon"); float qw = text_draw(q, sx + 48, sy + 198 - text_line_height(34) / 2.0f, 34, 600, WHITE_A(255), 0, 0, ALIGN_LEFT); gfx_rrect(sx + 52 + qw, sy + 178, 3, 40, 1.5f, ACC2);
  static const char *ROWS[3] = { "qwertyuiop", "asdfghjkl", "zxcvbnm" };
  for (int r = 0; r < 3; r++) { int n = (int)strlen(ROWS[r]); float kw = (sw - 20) / 10.0f, x0 = sx + 10 + (10 - n) * kw / 2;
    for (int c = 0; c < n; c++) { char ch = ROWS[r][c]; int hot = typed >= 1 && typed <= 4 && ch == "moon"[typed - 1] && fmodf(t - 0.6f, 0.3f) < 0.18f;
      gfx_rrect(x0 + c * kw + 3, sy + sh - 270 + r * 78, kw - 6, 66, 12, hot ? ACC | 0xff000000u : WHITE_A(30)); char s2[2] = { (char)(ch - 32), 0 };
      text_draw(s2, x0 + c * kw + kw / 2, sy + sh - 270 + r * 78 + 33 - text_line_height(24) / 2.0f, 24, 600, hot ? ACC_FG : WHITE_A(225), 0, 0, ALIGN_CENTER); } }
}
static void sc_phone(int st, float t) {
  float L = st == 2 ? 6.0f : 5.0f; t = fmodf(t, L);
  int typed = st < 2 ? 0 : t < 0.9f ? 0 : t < 1.2f ? 1 : t < 1.5f ? 2 : t < 1.8f ? 3 : 4; /* (each letter lands 0.3 s after its key) */
  search_page(1, typed, st == 2 ? cl01((t - 2.0f) / 0.8f) : 0, st == 0 ? 0.6f + 0.4f * sinf(t * 4) : st == 1 ? 1 - eo(1.0f, 1.6f, t) * 0.7f : 0); topbar(-1, -1, 0, 1, 0);
  if (st == 0) { press(K_GREEN, T("Search"), vis(t, 0.3f, 1.6f, 0.2f), (t - 0.6f) / 0.35f, 1300, PRESS_Y); push(0.6f, L, ORB_GREEN, 0.8f); return; }
  float up = st == 1 ? eo(0, 0.6f, t) : 1, px = SB_W - 140 - 400, py = 250 + (1 - up) * 700 + 6 * sinf(t * 1.6f);
  if (st == 1) { /* the camera's line of sight, from the phone to the code */
    float qx = 96 + 75, qy = 160 + 94 + 6 * 74 + 30 + 75, ex = px + 200, ey = py + 18 + 150 + 115, a = vis(t, 0.5f, 1.6f, 0.2f);
    for (int k = 0; k < 18; k++) { float u = (k + fmodf(t * 3, 1)) / 18; gfx_circle(qx + (ex - qx) * u, qy + (ey - qy) * u, 3, col_alpha(ACC2, 0.7f * a)); } }
  phone_card(px, py, st, t);
  if (st == 2) { for (int i = 0; i < 4; i++) { float ti = 0.6f + 0.3f * i, c = cl01((t - ti) / 0.4f); if (c <= 0 || c >= 1) continue; float e = ease_apply(EASE_IN_OUT, c);
      float sx = px + 66 + i * 22, sy = py + 18 + 198, ex = 96 + 72 + i * 20, ey = 198, lx = bez(sx, (sx + ex) / 2, ex, e), ly = bez(sy, fminf(sy, ey) - 140, ey, e);
      char s2[2] = { "moon"[i], 0 }; text_draw(s2, lx, ly - text_line_height(36) / 2.0f, 36, 700, col_mix(ACC2 | 0xff000000u, WHITE_A(255), e), 0, 0, ALIGN_CENTER); push(ti + 0.4f, L, ORB_OK, 0.3f); }
    push(2.0f, L, ORB_PHONE, 1); }
  else push(1.0f, L, ORB_DONE, 0.6f);
}

/* -- 7. the keys: the film's still behind; TMDB: the title's card, its words sweeping from English into the language
      every few seconds, its review under it; Trakt: the score with its spread growing in, the other sources' tiles,
      people's comments rising in one after another. A key already there: a tick in the corner -- */
/* the app's own demo pictures (assets/guide), for the pictures of its screens */
static const char *demo_art(const char *file) { static char b[4][640]; static int k; k = (k + 1) & 3; snprintf(b[k], sizeof b[k], "file://%s/guide/%s", plat_asset_dir(), file); return b[k]; }
#define KEY_BG demo_art("demo3.bg.webp")
static void key_still(void) {
  float asp = 0, u0, v0, u1, v1; GLuint bt = img_get(KEY_BG, 1280, 720, &asp);
  if (bt && asp > 0) { img_cover_uv(asp, SB_W / SB_H, 0, &u0, &v0, &u1, &v1); gfx_image(bt, 0, 0, SB_W, SB_H, 0, WHITE_A(255), u0, v0, u1, v1); } else gfx_rect(0, 0, SB_W, SB_H, PAGE);
  gfx_rect(0, 0, SB_W, SB_H, col_alpha(PAGE, 0.8f)); porbs(0, 1);
}
static void key_added(int on) { if (!on) return; const char *s = T("Added"); float w = text_width(s, 26, 700, 0) + 84, x = SB_W - 96 - w, y = 72;
  gfx_rrect(x, y, w, 60, 30, RGBA(16, 14, 22, 212)); gfx_ring(x, y, w, 60, 30, 1, WHITE_A(46)); gfx_circle(x + 30, y + 30, 19, ACC); icon_draw("check", x + 17, y + 17, 26, WHITE_A(255)); text_draw(s, x + 60, y + 30 - text_line_height(26) / 2.0f, 26, 600, WHITE_A(245), 0, 0, ALIGN_LEFT); }
static void key_card(float x, float y, float w, float h, const char *tag, const char *ti, const char *ds, int lit) { /* the title's card as Settings shows it */
  float pw = 200, ph = 300, px = x + 32, tx = px + pw + 36, tw = w - pw - 100;
  gfx_shadow(x, y + 20, w, h, 28, 50, BLACK_A(120)); gfx_rrect(x, y, w, h, 28, lit ? RGBA(16, 14, 24, 220) : RGBA(16, 14, 24, 180)); gfx_ring(x, y, w, h, 28, lit ? 3 : 1.5f, lit ? col_alpha(ACC, 0.85f) : WHITE_A(36));
  GLuint pt = img_get_shaped(demo_art("demo3.poster.webp"), (int)pw, (int)ph, 16, 0); if (!ph_picture(pt, px, y + (h - ph) / 2, pw, ph, 16, WHITE_A(lit ? 255 : 150))) ph_box(px, y + (h - ph) / 2, pw, ph, 16);
  float ty = y + 40; text_draw(tag, tx, ty, 24, 700, lit ? ACC3 : WHITE_A(150), 1.0f, tw, ALIGN_LEFT); ty += text_line_height(24) + 14;
  int tn = text_draw_wrapped(ti, tx, ty, tw, 2, 44, 800, WHITE_A(lit ? 255 : 190), -0.6f, ALIGN_LEFT, 52); ty += tn * 52 + 6;
  { char m[64]; snprintf(m, sizeof m, "2001 \xc2\xb7 %s", lit ? T("Animation") : "Animation"); text_draw(m, tx, ty, 26, 600, WHITE_A(lit ? 170 : 120), 0, tw, ALIGN_LEFT); } ty += text_line_height(26) + 14;
  text_draw_wrapped(ds, tx, ty, tw, tn > 1 ? 2 : 3, 28, 400, WHITE_A(lit ? 215 : 140), 0, ALIGN_LEFT, 38);
}
#define KS 1.55f /* the pieces half as large again than the app draws them: read from the sofa in the small frame */
static void sc_yourkeys(int st, float t) {
  key_still(); gfx_scale_push(KS, KS, 0, 0);
  if (st == 0) { float L = 6.0f; t = fmodf(t, L); float a = vis(t, 0, L, 0.3f);
    const char *EN_T = "Spirited Away", *EN_D = "A girl wanders into a world of spirits and must work in a bathhouse to save her parents.";
    int en = !strcmp(i18n_lang(), "en"); const char *LT = en ? "Le Voyage de Chihiro" : T(EN_T), *LD = en ? "Une fillette s\xe2\x80\x99\xc3\xa9gare dans un monde d\xe2\x80\x99" "esprits et doit travailler dans des bains pour sauver ses parents." : T(EN_D);
    float x = 80, y = 60, w = SB_W / KS - 160, h = 340, k = t < 1.2f ? 0 : t < 2.0f ? ease_apply(EASE_IN_OUT, (t - 1.2f) / 0.8f) : 1; /* the sweep */
    gfx_opacity_push(a); gfx_offset_push(0, 20 * (1 - eo(0, 0.6f, t)));
    if (k < 1) { gfx_clip_push(x + w * k, y - 30, w * (1 - k) + 30, h + 60); key_card(x, y, w, h, T("Without a key"), EN_T, EN_D, 0); gfx_clip_pop(); }
    if (k > 0) { gfx_clip_push(x - 30, y - 30, w * k + 30, h + 60); key_card(x, y, w, h, T("With your key"), LT, LD, 1); gfx_clip_pop(); }
    if (k > 0.01f && k < 0.99f) { float sx = x + w * k; gfx_rrect(sx - 2, y + 16, 4, h - 32, 2, ACC); gfx_glow_round(sx - 60, y, 120, h, col_alpha(ACC, 0.5f), x, y, w, h, 28); }
    float r = eo(2.4f, 3.0f, t); if (r > 0) { gfx_opacity_push(r); gfx_offset_push(0, 30 * (1 - r)); text_draw(T("Its reviews, too"), x, y + h + 36, 26, 600, WHITE_A(170), 0, w, ALIGN_LEFT); revpanel_draw_comment(x, y + h + 36 + text_line_height(26) + 12, w, revpanel_sample(1), 0, revpanel_comment_lines(revpanel_sample(1), w, 3), r); gfx_offset_pop(); gfx_opacity_pop(); }
    gfx_offset_pop(); gfx_opacity_pop(); gfx_scale_pop(); push(1.2f, L, ORB_RIGHT, 0.6f); push(2.4f, L, ORB_DONE, 0.5f); key_added(tmdb_has_key()); return; }
  { float L = 7.0f; t = fmodf(t, L); float a = vis(t, 0, L, 0.3f), g = eo(0.2f, 1.3f, t); int d[10]; const int *sd = revpanel_sample_dist(); for (int i = 0; i < 10; i++) d[i] = (int)(sd[i] * (0.04f + 0.96f * g) + 0.5f);
    float x = 80, y = 60, lw = 600, rx = x + lw + 40, rw = SB_W / KS - 80 - rx;
    gfx_opacity_push(a); gfx_offset_push(0, 20 * (1 - eo(0, 0.6f, t)));
    text_draw(T("Ratings and reviews"), x, y, 24, 600, WHITE_A(170), 0, lw, ALIGN_LEFT); y += text_line_height(24) + 16;
    y += revpanel_draw_score(x, y, lw, 8.2f, "Trakt", 23140, d, 0) + 26;
    float tr = eo(1.4f, 2.0f, t); if (tr > 0) { gfx_opacity_push(tr); gfx_offset_push(0, 24 * (1 - tr)); text_draw(T("Ratings from other sources"), x, y, 24, 600, WHITE_A(170), 0, lw, ALIGN_LEFT); y += text_line_height(24) + 14;
      static const char *SRC[4] = { "IMDb", "Rotten Tomatoes", "Metacritic", "TMDB" }, *VAL[4] = { "8.6", "97%", "96", "8.4" }; float cw = lw / 4;
      for (int i = 0; i < 4; i++) { float ti = eo(1.5f + 0.12f * i, 2.3f + 0.12f * i, t); gfx_opacity_push(ti > 0.3f ? 1 : ti / 0.3f); revpanel_draw_ring(x + cw * (i + 0.5f), y, 66, SRC[i], VAL[i], ti); gfx_opacity_pop(); }
      gfx_offset_pop(); gfx_opacity_pop(); }
    float cy = 60; text_draw(T("Comments"), rx, cy, 24, 600, WHITE_A(170), 0, rw, ALIGN_LEFT); cy += text_line_height(24) + 16;
    for (int i = 0; i < 2; i++) { float ci = eo(0.8f + 0.5f * i, 1.4f + 0.5f * i, t); if (ci <= 0) continue; const RevComment *c = revpanel_sample(i ? 2 : 0);
      gfx_opacity_push(ci); gfx_offset_push(0, 40 * (1 - ci)); cy += revpanel_draw_comment(rx, cy, rw, c, 0, revpanel_comment_lines(c, rw, 3), ci) + 14; gfx_offset_pop(); gfx_opacity_pop(); }
    gfx_offset_pop(); gfx_opacity_pop(); gfx_scale_pop(); push(0.5f, L, ORB_OK, 0.5f); push(1.3f, L, ORB_DONE, 0.6f); push(1.9f, L, ORB_HOLD, 0.5f); key_added(trakt_has_key()); }
}

/* -- 8. the thank-you, and the version's page: the code for a tip on a soft white tile, hearts rising beside it;
      the second point: the Share logs card -- */
static void tip_card(float t, const char *head, const char *msg) {
  gfx_rrect_v(0, 0, SB_W, SB_H, 0, RGB(0x19, 0x15, 0x26), RGB(0x0b, 0x0a, 0x10)); porbs(0, 1);
  float S = msg ? 560 : 640, x = msg ? SB_W - 140 - S : SB_W / 2 - S / 2, y = SB_H / 2 - S / 2 - (msg ? 50 : 30) + 14 * (1 - eo(0, 0.7f, t)), br = 1 + 0.012f * sinf(t * 2.2f);
  gfx_scale_push(br, br, x + S / 2, y + S / 2); glow(x + S / 2, y + S / 2, S * 1.9f, S * 1.6f, ACC, 0.32f + 0.1f * sinf(t * 2.2f));
  gfx_shadow(x, y + 24, S, S, 52, 60, BLACK_A(140));
  if (W.url[0] && qr_draw(W.url, x, y, S, 52, 40)) gfx_rrect_v(x, y, S, S, 52, WHITE_A(0), RGBA(225, 220, 240, 30)); else gfx_rrect(x, y, S, S, 52, WHITE_A(20));
  gfx_scale_pop();
  text_draw(W.urlShown, x + S / 2, y + S + 34, 36, 700, WHITE_A(225), 0, S + 300, ALIGN_CENTER);
  for (int i = 0; i < 4; i++) { float p = fmodf(t * 0.3f + i * 0.25f, 1); float hx = x + S + 40 + 30 * sinf(p * 6 + i), hy = y + S - p * 460; gfx_opacity_push(sinf(3.14159f * p) * 0.85f); icon_draw("g_heart", hx, hy, 44 + 10 * (i % 3), ACC2); gfx_opacity_pop(); }
  if (msg) { float tx = 140, tw = SB_W - 140 - S - 140 - 120; icon_draw("g_heart", tx, y, 72, ACC2);
    text_draw_wrapped(head, tx, y + 110, tw, 2, 76, 800, WHITE_A(255), -1.4f, ALIGN_LEFT, 84);
    text_draw_wrapped(msg, tx, y + 290, tw, 6, 40, 500, WHITE_A(220), 0, ALIGN_LEFT, 56); }
}
/* the Share logs card as the app shows it, rising in */
static void logs_card(float t, float a) {
  if (a <= 0.004f) return; float CW = 1360, CH = 620, x = SB_W / 2 - CW / 2, y = SB_H / 2 - CH / 2 + 20 * (1 - a), P = 70, QS = 380, sc = 0.97f + 0.03f * a;
  gfx_opacity_push(a); gfx_scale_push(sc, sc, SB_W / 2, SB_H / 2);
  gfx_shadow(x, y + 30, CW, CH, 36, 80, BLACK_A(150)); gfx_rrect(x, y, CW, CH, 36, RGBA(30, 27, 40, 244)); gfx_rrect_v(x, y, CW, CH, 36, WHITE_A(18), WHITE_A(0)); gfx_ring(x, y, CW, CH, 36, 1, WHITE_A(26));
  text_draw(T("Share logs"), x + P, y + P, 56, 800, WHITE_A(255), -1, 0, ALIGN_LEFT);
  text_draw_wrapped(T("Scan the code and describe the issue."), x + P, y + P + 90, CW - 3 * P - QS, 4, 30, 500, WHITE_A(215), 0, ALIGN_LEFT, 42);
  text_draw(T("Log code"), x + P, y + CH - P - 160, 26, 700, WHITE_A(160), 0.6f, 0, ALIGN_LEFT);
  float k = eo(0.5f, 0.9f, t); gfx_opacity_push(k); text_draw("K7Q2", x + P, y + CH - P - 120 + 10 * (1 - k), 110, 800, WHITE_A(255), 22, 0, ALIGN_LEFT);
  qr_draw("https://github.com/issues/new?log=K7Q2", x + CW - P - QS, y + CH / 2 - QS / 2, QS, 32, 28); gfx_opacity_pop();
  gfx_scale_pop(); gfx_opacity_pop();
}
/* how to share logs, as the app does it: Settings, About, Share logs, OK, the card with its code (a loop of 8 s) */
static void logs_howto(float t) {
  float L = 8.0f; t = fmodf(t, L); float card = eo(2.4f, 2.8f, t) * (1 - eo(7.4f, 7.8f, t));
  cam(t, 0.9f, 2.5f, 1240, 440, 1.6f); /* About first, then in on Share logs as it is chosen, out as its card comes */
  settings_page(1, t < 0.8f ? -1 : t < 1.4f ? 0 : 1, 0, 1); topbar(-1, -1, 1, 0, 0);
  push(0.8f, L, ORB_RIGHT, 0.4f); push(1.4f, L, ORB_DOWN, 0.3f); push(2.4f, L, ORB_OK, 0.7f);
  logs_card(t - 2.4f, card);
  press(K_OK, T("OK"), vis(t, 1.9f, 2.9f, 0.2f), (t - 2.2f) / 0.35f, 400, PRESS_Y);
}
/* the thank-you's points: how to share logs (the app's own screens), then the codes to support it and to star it, big */
static void tip_switch(float t) { /* one code at a time, the pill saying which (OK turns it over) */
  gfx_rrect_v(0, 0, SB_W, SB_H, 0, RGB(0x19, 0x15, 0x26), RGB(0x0b, 0x0a, 0x10)); porbs(0, 1);
  float Q = 660, h = support_h(Q), x = SB_W / 2 - support_w(Q) / 2, y = SB_H / 2 - h / 2 + 14 * (1 - eo(0, 0.7f, t)), sel = W.tipK.v;
  glow(SB_W / 2, y + 78 + Q / 2, Q * 2, Q * 1.6f, col_mix(ACC | 0xff000000u, RGB(0xff, 0xd4, 0x5a), sel), 0.3f + 0.08f * sinf(t * 2.2f));
  support_glass(sgOrb); support_draw(x, y, Q, sel, 0, W.url, W.urlShown, W.gh, W.ghShown);
  for (int i = 0; i < 4; i++) { float p = fmodf(t * 0.3f + i * 0.25f, 1); float hx = x + Q + 70 + 30 * sinf(p * 6 + i), hy = y + h - 60 - p * 440; gfx_opacity_push(sinf(3.14159f * p) * 0.8f); icon_draw(sel < 0.5f ? "g_heart" : "g_star", hx, hy, 44 + 10 * (i % 3), sel < 0.5f ? ACC2 | 0xff000000u : RGB(0xff, 0xd4, 0x5a)); gfx_opacity_pop(); }
}
static void sc_thanks(int st, float t) { if (st == 0) logs_howto(t); else if (W.gh[0]) tip_switch(t); else tip_card(t, NULL, NULL); }
/* the version's page: its glass shown off over the brightest pictures (one flowing into the next under it), the bar's
   focus gliding along the tabs, and the two codes on a sheet of the same glass in the middle */
static void sc_news(float t) {
  static const int ART[3] = { 5, 1, 6 }; const float P = 6.0f; int n = (int)(t / P), a = ART[n % 3], b = ART[(n + 1) % 3]; float f = sstep(P - 1.2f, P, fmodf(t, P));
  backdrop(a, 0.35f); { float asp; GLuint t2 = art(b, 0, &asp); if (t2 && f > 0.001f) { gfx_image(t2, 0, 0, SB_W, SB_H, 0, WHITE_A((int)(255 * f)), 0, 0, 1, 1); sgT2 = t2; sgMix = f; } }
  gfx_rrect(0, 0, SB_W, SB_H, 0, BLACK_A(90)); sgDim = 90 / 255.0f; /* (a touch of the pages' dark: the words on the glass read) */
  float pt = fmodf(t, 12.0f), pill = pt < 8.0f ? 1 + glide(5.0f, pt) : 2 - glide(9.0f, pt); /* calm: on Home, now and then over to Library and back */
  topbar(1, pill, 0, 0, 0);
  if (!W.gh[0] || W.tour) return; /* (in the tour: the codes are the thanks page's, never twice) */
  float Q = 500, sw = support_w(Q), h = support_h(Q), pw = sw + 120, phh = h + 110, px = SB_W / 2 - pw / 2, py = 150 + (SB_H - 150) / 2 - phh / 2 + 14 * (1 - eo(0, 0.7f, t)), sel = W.tipK.v;
  wSoft = 0.4f; wglass(px, py, pw, phh, 56, 0.9f, 9); lglass_light(0); /* the sheet: the bar's glass, large, bending gently */
  support_glass_picture(f > 0.5f && sgT2 ? sgT2 : sgT); support_draw(px + 60, py + 55, Q, sel, 0, W.url, W.urlShown, W.gh, W.ghShown);
}
static void scene(int pv, int st, float t) {
  switch (pv) { case PV_KEYS: sc_keys(st, t); break; case PV_HOLD: sc_hold(st, t); break; case PV_STREAMS: sc_streams(st, t); break; case PV_SUBS: sc_subs(st, t); break;
    case PV_TORRENTS: sc_torrents(st, t); break; case PV_PHONE: sc_phone(st, t); break; case PV_YOURKEYS: sc_yourkeys(st + ykOff, t); break; case PV_THANKS: sc_thanks(st, t); break; default: sc_news(t); }
}

/* ==== the page ==== */
static float colX(void) { return app_rtl() ? SB_W - MX : MX; }   /* the words' start edge */
static float frameX(void) { return app_rtl() ? MX : SB_W - MX - FW; }
static void measure(void) {
  if (W.lg == i18n_gen()) return; W.lg = i18n_gen();
  for (int i = 0; i < W.n; i++) { Page *p = &W.p[i]; p->nt = text_wrap_count(T(p->title), COLW, 2, TI_PX, 800, -1.4f); p->nl = p->lead[0] ? text_wrap_count(T(p->lead), COLW, 3, LE_PX, 500, 0) : 0;
    for (int b = 0; b < p->nb; b++) p->nbl[b] = text_wrap_count(T(p->b[b]), COLW - CHIP - 24 - (p->ctl && b == p->nb - 1 ? 90 : 0), 3, PO_PX, 700, -0.2f); }
  for (int i = 0; i < W.nnews; i++) { W.news[i].nt = text_wrap_count(T(W.news[i].t), COLW - CHIP - 24, 2, NT_PX, 800, -0.5f); W.news[i].nx = W.news[i].x[0] ? text_wrap_count(T(W.news[i].x), COLW - CHIP - 24, 3, NX_PX, 500, 0) : 0; }
}
static void step_label(int i, char *o, size_t n) {
  if (W.p[i].pv == PV_NEWS) { snprintf(o, n, "%s", T1("Version {v}", "v", W.short_ver)); return; }
  char a[16], b[16]; snprintf(a, sizeof a, "%d", i + 1); snprintf(b, sizeof b, "%d", W.n); snprintf(o, n, "%s", T2("{n} of {total}", "n", a, "total", b));
}
static float point_h(const Page *p, int b) { float h = fmaxf(CHIP, p->nbl[b] * PO_LH); return p->ctl && b == p->nb - 1 ? fmaxf(h, CTL_H - 24) + 24 : h + 12; }
static float words_h(const Page *p) {
  if (p->pv == PV_NEWS) { float h = text_line_height(STEP_PX) + 18 + p->nt * TI_LH + 36; for (int i = 0; i < W.nnews; i++) h += fmaxf(CHIP, W.news[i].nt * NT_LH + (W.news[i].nx ? 6 + W.news[i].nx * NX_LH : 0)) + NGAP; return h; }
  float h = text_line_height(STEP_PX) + 18 + p->nt * TI_LH + (p->nl ? 20 + p->nl * LE_LH : 0) + 40;
  for (int b = 0; b < p->nb; b++) h += point_h(p, b) + PO_GAP; return h;
}
/* the switch for the best stream, inside its point: on, off, or the ring while the line is measured */
static void ctl_switch(float x, float y, float h, float k, int rtl) {
  float sx = rtl ? x + 8 : x - 80, sy = y + h / 2 - 20;
  if (best_speed_busy()) { float cx = sx + 36, cy = sy + 20, ph = (float)fmod(app_now() / 0.8, 1) * 6.2832f; gfx_ring(cx - 16, cy - 16, 32, 32, 16, 3, col_alpha(ACC_FG, 0.3f));
    for (int q = 0; q < 12; q++) { float an = ph + q * 0.14f; gfx_circle(cx + cosf(an) * 14.5f, cy + sinf(an) * 14.5f, 1.8f, k > 0.5f ? ACC_FG : ACC2); } return; }
  float kk = W.knob.v; lglass_switch(sx, sy, 72, 40, rtl ? 1 - kk : kk, kk, k > 0.5f); /* the app's glass switch */
}
static void words(int i, float a, float dx) {
  if (a <= 0.004f || i < 0 || i >= W.n) return;
  const Page *p = &W.p[i]; int rtl = app_rtl(), al = rtl ? ALIGN_RIGHT : ALIGN_LEFT; float x = colX(), y = FY + FH / 2 - words_h(p) / 2, s = rtl ? -1 : 1;
  if (y < 110) y = 110;
  gfx_opacity_push(a); gfx_offset_push(dx, 0);
  char lb[160]; step_label(i, lb, sizeof lb); text_draw(lb, x, y, STEP_PX, 800, ACC2 | 0xff000000u, 1.4f, COLW, al); y += text_line_height(STEP_PX) + 18;
  text_draw_wrapped(T(p->title), x, y, COLW, 2, TI_PX, 800, WHITE_A(255), -1.4f, al, TI_LH); y += p->nt * TI_LH;
  if (p->pv == PV_NEWS) { y += 36;
    for (int k = 0; k < W.nnews; k++) { float cx = rtl ? x - CHIP : x, h = fmaxf(CHIP, W.news[k].nt * NT_LH + (W.news[k].nx ? 6 + W.news[k].nx * NX_LH : 0));
      { float is = 40; icon_draw(W.news[k].ic, cx + (CHIP - is) / 2, y + 4 + NT_LH / 2 - is / 2, is, WHITE_A(228)); } /* the glyph alone, soft white, on its title's line (no tile, no ring) */
      float tx = x + s * (CHIP + 24); text_draw_wrapped(T(W.news[k].t), tx, y + 4, COLW - CHIP - 24, 2, NT_PX, 800, WHITE_A(255), -0.5f, al, NT_LH);
      if (W.news[k].nx) text_draw_wrapped(T(W.news[k].x), tx, y + 4 + W.news[k].nt * NT_LH + 6, COLW - CHIP - 24, 3, NX_PX, 500, WHITE_A(210), 0, al, NX_LH);
      y += h + NGAP; } }
  else {
    if (p->nl) { y += 20; text_draw_wrapped(T(p->lead), x, y, COLW, 3, LE_PX, 500, WHITE_A(225), 0, al, LE_LH); y += p->nl * LE_LH; }
    y += 40;
    float fo = i == W.cur ? W.focus.v : 0; /* the point in focus: lit, its chip in the accent, a bar at its side */
    for (int b = 0; b < p->nb; b++) { float h = point_h(p, b), k = 1 - fabsf(b - fo); k = k < 0 ? 0 : k; int ctl = p->ctl && b == p->nb - 1; float L0 = rtl ? x - COLW : x;
      if (ctl) { gfx_rrect(L0 - 16, y - 12, COLW + 32, h, 26, col_mix(WHITE_A(16), ACC | 0xff000000u, k)); if (k > 0.01f) gfx_ring_out(L0 - 16, y - 12, COLW + 32, h, 26, 4, acc_a(0.35f * k)); }
      else if (k > 0.01f) { gfx_rrect(L0 - 16, y - 6, COLW + 32, h, 22, WHITE_A((int)(18 * k))); gfx_rrect(rtl ? x + 8 : x - 22, y + 6, 6, h - 24, 3, col_alpha(ACC2, k)); }
      float ph = fmaxf(CHIP, p->nbl[b] * PO_LH), top = ctl ? y - 12 + (h - ph) / 2 : y, cy2 = top + (ph - CHIP) / 2, cx = rtl ? x - CHIP : x;
      uint32_t cbg = ctl ? col_mix(acc_a(0.22f), WHITE_A(60), k) : col_mix(acc_a(0.14f), ACC | 0xff000000u, k), cfg = ctl ? col_mix(ACC2 | 0xff000000u, ACC_FG | 0xff000000u, k) : col_mix(col_alpha(ACC2, 0.8f), ACC_FG | 0xff000000u, k);
      gfx_rrect(cx, cy2, CHIP, CHIP, 18, cbg);
      const char *ic = p->ic[b];
      if (ic[0] == '@') { int key = !strcmp(ic, "@red") ? K_RED : !strcmp(ic, "@green") ? K_GREEN : !strcmp(ic, "@ok") ? K_OK : K_BACK; float kw = key == K_RED || key == K_GREEN ? 42 : 38;
        key_hint(cx + CHIP / 2 - kw / 2, cy2 + CHIP / 2, key, NULL, key == K_RED || key == K_GREEN ? WHITE_A(255) : cfg, 0); }
      else icon_draw(ic, cx + 15, cy2 + 15, 30, cfg);
      float tw = COLW - CHIP - 24 - (ctl ? 90 : 0), ty = top + (ph - p->nbl[b] * PO_LH) / 2 + (PO_LH - text_line_height(PO_PX)) / 2;
      uint32_t tc = ctl ? col_mix(WHITE_A(240), ACC_FG | 0xff000000u, k) : col_mix(WHITE_A(150), WHITE_A(255), k);
      text_draw_wrapped(T(p->b[b]), x + s * (CHIP + 24), ty, tw, 3, PO_PX, 700, tc, -0.2f, al, PO_LH);
      if (ctl) ctl_switch(rtl ? x - COLW : x + COLW, y - 12, h, k, rtl);
      y += h + PO_GAP; }
  }
  gfx_offset_pop(); gfx_opacity_pop();
}
/* a page's picture (a point of it) into its target, at the moment t of its clock */
static void picture(int slot, int i, int st, float t) {
  if (picOk < 0) { int w = (int)(FW * gfx_scale() + 0.5f), h = (int)(FH * gfx_scale() + 0.5f); picOk = gfx_target_create(&pic[0], w, h) && gfx_target_create(&pic[1], w, h); }
  if (!picOk || i < 0 || i >= W.n) return;
  evSlot = slot; evPrev[slot] = evNow[slot]; evNow[slot] = t; if (evNow[slot] < evPrev[slot] || evNow[slot] - evPrev[slot] > 0.5f) evPrev[slot] = evNow[slot]; /* (a new point: nothing crossed) */
  if (orbTOk < 0) orbTOk = gfx_target_create(&orbT[0], 384, 216) && gfx_target_create(&orbT[1], 384, 216);
  if (orbTOk > 0) { gfx_target_begin(&orbT[slot], PAGE | 0xff000000u); gfx_rrect_v(0, 0, SB_W, SB_H, 0, RGB(0x19, 0x15, 0x26), RGB(0x0b, 0x0a, 0x10)); orbs_draw(&orbs[slot], 1); gfx_target_end(); sgOrb = orbT[slot].tex; } else sgOrb = 0;
  gfx_target_begin(&pic[slot], PAGE | 0xff000000u);
  { int m = i18n_rtl(); if (m) { app_rtl_hold(1); gfx_mirror_push(SB_W / 2); } /* right to left: the app's pages are mirrored, so their pictures are too */
    sgT = sgT2 = 0; sgShade = sgPage = sgMix = sgDim = 0; camOn = 0; wPress = pressK[slot]; wKey = keyK[slot]; pressNow = 0; keyNow = 0; scene(W.p[i].pv, st, t); pressK[slot] = pressNow; keyK[slot] = keyNow; if (camOn) { gfx_scale_pop(); gfx_offset_pop(); camOn = 0; } if (m) { gfx_mirror_pop(); app_rtl_hold(0); } }
  gfx_target_end();
}
static void frame_draw(int slot, float a, float dx) {
  if (a <= 0.004f || !picOk) return; float x = frameX() + dx * 0.6f;
  gfx_image(pic[slot].tex, x, FY, FW, FH, FR, WHITE_A((int)(255 * a)), 0, 1, 1, 0);
}
/* the page dots: the current page a short pill, the others dots, the pill flowing from one into the next */
static void page_dots(float cx, float y) {
  if (W.n < 2) return;
  float r = 6, d = 2 * r, L = 3.6f * d, sp = 26 - d, w[MAXP], tot = 0, cur = W.dot.v; int n = W.n;
  for (int i = 0; i < n; i++) { float k = 1 - fabsf(i - cur); k = k < 0 ? 0 : k; w[i] = d + (L - d) * k; tot += w[i] + (i ? sp : 0); }
  float xx = cx - tot / 2;
  for (int i = 0; i < n; i++) { float k = 1 - fabsf(i - cur); k = k < 0 ? 0 : k;
    gfx_rrect(app_rtl() ? 2 * cx - xx - w[i] : xx, y - r, w[i], d, r, k > 0.01f ? col_mix(WHITE_A(64), WHITE_A(240), k) : WHITE_A(64)); xx += w[i] + sp; }
}
static int at_codes(void);
static int at_keys(void);
static int at_ctl(void) { const Page *p = &W.p[W.cur]; return p->ctl && W.step == p->nb - 1; }
static void hints(float a) {
  int lastPage = W.cur == W.n - 1, last = lastPage && W.step >= W.p[W.cur].nb - 1, rtl = app_rtl(), ctl = at_ctl(), codes = at_codes(); uint32_t c = WHITE_A((int)(215 * a));
  const char *ok = codes ? (W.tipWant ? T("Buy me a coffee") : T("Star on GitHub")) : at_keys() ? T("Show the code") : ctl && !best_on() ? (best_speed_busy() ? T("Measuring\xe2\x80\xa6") : T("Turn on")) : last ? T("Close") : T("Next");
  const char *nx = T("Next page"), *bk = lastPage ? T("Close") : T("Skip"); int showNx = !lastPage, showBk = !last || codes;
  float g = 44, wo = 38 + 10 + text_width(ok, 22, 600, 0), wn = showNx ? 38 + 10 + text_width(nx, 22, 600, 0) + g : 0, wb = showBk ? 38 + 10 + text_width(bk, 22, 600, 0) + g : 0;
  gfx_scale_push(1.25f, 1.25f, rtl ? MX : SB_W - MX, BOTY);
  if (!rtl) { float x = SB_W - MX - (wo + wn + wb); key_hint(x, BOTY, K_OK, ok, c, 0); x += wo + g; if (showNx) { key_hint(x, BOTY, K_RIGHT, nx, c, 0); x += wn; } if (showBk) key_hint(x, BOTY, K_BACK, bk, c, 0); }
  else { float x = MX; if (showBk) { key_hint(x, BOTY, K_BACK, bk, c, 0); x += wb; } if (showNx) { key_hint(x, BOTY, K_LEFT, nx, c, 0); x += wn; } key_hint(x, BOTY, K_OK, ok, c, 0); }
  gfx_scale_pop();
}

static void finish(void) { W.open = W.closing = 0; W.fromSignIn = 0; app_ambient_guide(0); if (app_overlay == &card) app_overlay = NULL; sglass_free(&glass);
  for (int s = 0; s < 2; s++) gfx_target_free(&pic[s]); picOk = -1; }
/* it lets go of the keys at once (Home answers the next press) and fades out calmly over it */
static void close_guide(void) {
  if (!W.open || W.closing) return; W.closing = 1; timer_start(&W.out, 0.75f); app_ambient_guide(2);
  store_set("whatsNewSeen", W.ver); store_set("guideSeen", "1");
  if (app_overlay == &card) app_overlay = NULL;
}
/* each point's part of the page's picture, in seconds (the scene's own loop): one page plays its points one after
   the other, as one animation, round and round; the list follows it */
static float step_len(int pv, int st) {
  switch (pv) {
    case PV_KEYS: return st == 3 ? 6.5f : 7.0f;
    case PV_HOLD: return st == 0 ? 5.0f : st == 1 ? 7.0f : 6.0f;
    case PV_STREAMS: return st < 3 ? 5.0f : st == 3 ? 5.6f : 6.0f;
    case PV_SUBS: return st == 1 ? 5.0f : st == 2 ? 7.0f : 8.0f;
    case PV_TORRENTS: return 7.0f;
    case PV_PHONE: return st == 2 ? 6.0f : 5.0f;
    case PV_YOURKEYS: return st == 0 ? 6.0f : 7.0f;
    case PV_NEWS: return 18.0f;
    default: return 8.0f; }
}
static float seg_start(int page, int st) { float a = 0; for (int i = 0; i < st && i < W.p[page].nb; i++) a += step_len(W.p[page].pv, i); return a; }
static float page_len(int page) { int n = W.p[page].nb > 0 ? W.p[page].nb : 1; return seg_start(page, n); }
/* to a page and a point of it: the words slide when the page changes; the picture crosses over either way (the one
   leaving keeps its glows) */
static void go(int to, int step) {
  if (to < 0 || to >= W.n || (to == W.cur && step == W.step)) return;
  W.from = W.cur; W.fromStep = W.step; W.fromAt = W.stepAt; W.pageSw = to != W.cur; W.dir = to > W.cur || (to == W.cur && step > W.step) ? 1 : -1;
  if (W.pageSw) { tween_to(&W.dot, (float)to, GLIDE_DUR, GLIDE_EASE); tween_set(&W.focus, (float)step); }
  else tween_to(&W.focus, (float)step, GLIDE_DUR, GLIDE_EASE);
  W.cur = to; W.step = step; W.stepAt = app_now(); W.pageAt = W.stepAt - seg_start(to, step); /* the page's one animation, from this point of it */ orbs[1] = orbs[0]; evNow[1] = evNow[0]; evNow[0] = evPrev[0] = 0;
  timer_start(&W.sw, W.pageSw ? 0.5f : 0.4f); sound_focus();
}
static void on_speed(int ok, double mbps, void *ud) { (void)ud; (void)mbps;
  if (!ok) { W.speedFail = 1; snprintf(W.speed, sizeof W.speed, "%s", T("Could not check the connection. Try again in a moment")); return; }
  store_set("autoBest", "true"); W.doneAt = app_now(); W.speedFail = 0; orbs_pulse(ORB_DONE, 1); orbs_push(&orbs[0], ORB_DONE, 1.2f); sound_play(SND_TAB); }
static int at_codes(void) { const Page *p = &W.p[W.cur]; return W.gh[0] && ((p->pv == PV_NEWS && !W.tour) || (p->pv == PV_THANKS && W.step == p->nb - 1)); } /* (in the tour the codes are the thanks page's alone) */
static int at_keys(void) { return W.p[W.cur].pv == PV_YOURKEYS; }
static void next(void) {
  const Page *p = &W.p[W.cur];
  if (at_keys()) { if (W.step + ykOff == 0) qrcard_show_tmdb(NULL, NULL); else qrcard_show_trakt(NULL, NULL); return; } /* the key's code; Right or Down go on */
  if (at_codes()) { W.tipWant = !W.tipWant; tween_to(&W.tipK, (float)W.tipWant, 0.5f, EASE_IN_OUT); orbs_push(&orbs[0], W.tipWant ? ORB_RIGHT : ORB_LEFT, 0.7f); return; } /* the other code (Back closes) */
  if (at_ctl() && !best_on()) { if (!best_speed_busy()) { W.speedFail = 0; best_caps_probe(NULL, NULL); best_speed_run(on_speed, NULL); orbs_push(&orbs[0], ORB_HOLD, 1); } return; } /* OK turns it on (measured there); once on, it goes on */
  if (W.step < p->nb - 1) go(W.cur, W.step + 1); else if (W.cur < W.n - 1) go(W.cur + 1, 0); else close_guide();
}
static int key(int k) {
  if (W.closing) return 1;
  if (k == K_OK) next();
  else if (k == K_DOWN) { if (W.step < W.p[W.cur].nb - 1) go(W.cur, W.step + 1); else nudge_start(&W.nudge, k); }
  else if (k == K_UP) { if (W.step > 0) go(W.cur, W.step - 1); else nudge_start(&W.nudge, k); }
  else if (k == K_RIGHT) { if (W.cur < W.n - 1) go(W.cur + 1, 0); else nudge_start(&W.nudge, k); }
  else if (k == K_LEFT) { if (W.step > 0) go(W.cur, 0); else if (W.cur > 0) go(W.cur - 1, 0); else nudge_start(&W.nudge, k); }
  else if (k == K_BACK) close_guide(); /* Skip: the guide closes */
  return 1;
}
static void click(int a, int b) { (void)a; (void)b; key(K_OK); }
/* the page's animation goes on by itself from one point to the next (the list's highlight follows), never restarting */
static void follow(void) {
  if (!W.open || W.closing || W.n <= 0) return; const Page *p = &W.p[W.cur]; if (p->nb <= 1) return;
  if (p->ctl && W.step == p->nb - 1) return; /* (a switch to set: it waits there) */
  if (at_codes() || at_keys()) return; /* (a code to scan, a key to type: it waits there) */
  double now = app_now(); float tot = page_len(W.cur); if (tot <= 0) return;
  double pc = now - W.pageAt; if (pc >= tot) { W.pageAt += tot * floor(pc / tot); pc = now - W.pageAt; }
  int sg = 0; while (sg < p->nb - 1 && pc >= seg_start(W.cur, sg + 1)) sg++;
  if (sg == W.step) return;
  W.from = W.cur; W.fromStep = W.step; W.fromAt = W.stepAt; W.pageSw = 0; W.dir = 1;
  W.step = sg; W.stepAt = W.pageAt + seg_start(W.cur, sg); tween_to(&W.focus, (float)sg, GLIDE_DUR, GLIDE_EASE);
  orbs[1] = orbs[0]; evNow[1] = evNow[0]; evNow[0] = evPrev[0] = 0; timer_start(&W.sw, 0.4f);
}
static void tick(float dt) {
  follow();
  timer_tick(&W.in, dt); timer_tick(&W.sw, dt); tween_tick(&W.dot, dt); tween_tick(&W.focus, dt); tween_tick(&W.tipK, dt); nudge_tick(&W.nudge, dt); sglass_tick(&glass, dt);
  float want = best_on() ? 1 : 0; if (W.knob.to != want) tween_to(&W.knob, want, 0.24f, EASE_OUT); tween_tick(&W.knob, dt);
  if (W.closing && timer_tick(&W.out, dt) >= 1) finish();
}
static void draw(void) {
  if (!W.open) return;
  measure();
  double now = app_now(); int sw = W.sw.on;
  /* the pictures first, each into its own target (the page's state does not reach into them) */
  { static int fz = -1; static int fp, fs; static float ft; if (fz < 0) { const char *e = getenv("SB_GUIDE_AT"); fz = e && sscanf(e, "%d,%d,%f", &fp, &fs, &ft) == 3; } /* development: SB_GUIDE_AT=page,point,seconds holds the picture there */
    if (fz && fp >= 0 && fp < W.n) { W.cur = fp; W.step = fs; W.sw.on = 0; sw = 0; picture(0, fp, fs, ft); } else {
  picture(0, W.cur, W.step, (float)(now - W.stepAt)); if (sw) picture(1, W.from, W.fromStep, (float)(now - W.fromAt)); } }
  /* in: Home frosts over and the page's own colour and glows come up, then the words and the picture rise in; out: the
     words go first, the page clears after them, evenly */
  float it = W.in.on ? timer_x(&W.in) : 1, ot = W.closing ? timer_x(&W.out) : 0;
  float base = ease_apply(EASE_CSS_EASE, it / 0.5f), vt = cl01((ot - 0.25f) / 0.75f); base *= 1 - vt * vt * (3 - 2 * vt);
  float ck = W.in.on ? ease_apply(EASE_OUT, (it - 0.25f) / 0.75f) : 1, co = W.closing ? 1 - ease_apply(EASE_OUT, ot / 0.45f) : 1, rise = 18 * (1 - ck) - 10 * (1 - co);
  if (!W.closing) hit_act(0, 0, SB_W, SB_H, click, 0, 0);
  gfx_opacity_push(base); if (W.fromSignIn) login_backdrop_draw(); /* straight from signing in: its very background, carrying on */ else { sglass_draw(&glass, 0, 0, SB_W, SB_H, 0); gfx_rect(0, 0, SB_W, SB_H, col_alpha(PAGE, 0.88f)); login_glows_draw(); } gfx_opacity_pop(); /* the sign-in's glows, on their one clock: the same from signing in through every page here, never placed again */
  float a = ck * co; if (a <= 0.004f) return;
  gfx_opacity_push(a); gfx_offset_push(nudge_x(&W.nudge), rise + nudge_y(&W.nudge));
  float fx = frameX(); gfx_shadow(fx, FY + 24, FW, FH, FR, 60, BLACK_A(130)); gfx_rrect(fx, FY, FW, FH, FR, PAGE | 0xff000000u);
  float s = app_rtl() ? -1 : 1;
  if (sw) { float x = timer_x(&W.sw), e = ease_apply(EASE_SNAP, x), ao = 1 - ease_apply(EASE_OUT, x / 0.55f), ai = ease_apply(EASE_OUT, (x - 0.12f) / 0.7f);
    if (W.pageSw) { frame_draw(1, 1, 0); frame_draw(0, ease_apply(EASE_IN_OUT, x), 0); } /* the next page's picture dissolves in place over the last (the words slide): never the dark between, never past the frame's edges */
    else { frame_draw(1, 1, 0); frame_draw(0, ease_apply(EASE_IN_OUT, x), 0); } /* a point of the same page: the new one dissolves in over the last, in place (never through the dark under them) */
    if (W.pageSw) { words(W.from, ao, -W.dir * s * SLIDE * e); words(W.cur, ai, W.dir * s * SLIDE * (1 - e)); } else words(W.cur, 1, 0); }
  else { frame_draw(0, 1, 0); words(W.cur, 1, 0); }
  gfx_ring(fx, FY, FW, FH, FR, 1.5f, WHITE_A(34));
  gfx_offset_pop();
  page_dots(SB_W / 2, BOTY); hints(1);
  gfx_opacity_pop();
}
static Screen card = { NULL, NULL, key, tick, draw, NULL, NULL, NULL };
int welcome_is_open(void) { return W.open; }
int welcome_covers(void) { return W.open && !W.closing && !W.in.on && app_overlay == &card; }

/* whatsnew.json: { "version", "items": [ { "title", "text", "icon" } or "a line", ... ], "support": { "title", "message", "url" } } */
/* the guide's titles: bundled with the app (assets/guide: their words, posters, backdrops and logos), the same at
   every start, never waiting on the network or a cache */
static int guide_titles(Item *out, int max) {
  char p[1200]; snprintf(p, sizeof p, "%s/guide/titles.json", plat_asset_dir());
  FILE *f = fopen(p, "rb"); if (!f) return 0; fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  char *b = malloc((size_t)n + 1); size_t got = fread(b, 1, (size_t)n, f); b[got] = 0; fclose(f);
  cJSON *j = cJSON_Parse(b); free(b); int k = 0; const cJSON *m;
  cJSON_ArrayForEach(m, j) { if (k >= max) break; Item *it = &out[k]; sb_fill_item(it, m, "movie"); const char *d = plat_asset_dir();
    snprintf(it->poster, sizeof it->poster, "file://%s/guide/%s.poster.webp", d, it->id); snprintf(it->background, sizeof it->background, "file://%s/guide/%s.bg.webp", d, it->id);
    snprintf(it->logo, sizeof it->logo, "file://%s/guide/%s.logo.webp", d, it->id); k++; }
  cJSON_Delete(j); return k;
}
/* the guide's pictures, asked for every frame while a guide is coming or open (kept fresh: never dropped from the
   queue), ready before it opens: no picture ever waits on a placeholder */
static int siLoaded;
static void si_load(void) { if (siLoaded) return; siLoaded = 1; nsi = guide_titles(SI, NS); rowName[0] = 0; if (!nsi) nsi = board_sample(SI, NS, rowName, sizeof rowName); }
static int preload(void) {
  si_load(); int ready = 1; float a; char b[512];
  for (int i = 0; i < nsi; i++) { const Item *it = &SI[i];
    if (it->poster[0]) { img_get_shaped(it->poster, 300, 450, CARD_R, 0); ready &= img_shaped_loaded(it->poster, 300, 450, CARD_R, 0) || img_shaped_failed(it->poster, 300, 450, CARD_R, 0); }
    if (sb_bg_mid(it, b, sizeof b)[0]) { img_get(b, 1280, 720, &a); ready &= img_loaded(b) || img_failed(b); }
    if (sb_logo_of(it, b, sizeof b)[0]) { img_get(b, 600, 240, &a); ready &= img_loaded(b) || img_failed(b); } }
  return ready;
}
static int load(void) {
  char p[1200]; snprintf(p, sizeof p, "%s/whatsnew.json", plat_asset_dir());
  W.nnews = 0; W.url[0] = 0; W.smsg[0] = 0; snprintf(W.ver, sizeof W.ver, "%s", update_current());
  FILE *f = fopen(p, "rb"); if (!f) return 0; fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  char *b = malloc((size_t)n + 1); size_t got = fread(b, 1, (size_t)n, f); b[got] = 0; fclose(f);
  cJSON *j = cJSON_Parse(b); free(b); if (!j) return 0;
  snprintf(W.ver, sizeof W.ver, "%s", js_str(j, "version", update_current()));
  const cJSON *it; cJSON_ArrayForEach(it, js_arr(j, "items")) { if (W.nnews >= MAXN) break; News *nw = &W.news[W.nnews]; memset(nw, 0, sizeof *nw);
    if (cJSON_IsString(it)) snprintf(nw->t, sizeof nw->t, "%s", it->valuestring); /* the older form: a line alone */
    else if (cJSON_IsObject(it)) { snprintf(nw->t, sizeof nw->t, "%s", js_str(it, "title", "")); snprintf(nw->x, sizeof nw->x, "%s", js_str(it, "text", "")); snprintf(nw->icn, sizeof nw->icn, "%s", js_str(it, "icon", "g_spark")); }
    nw->ic = icon_exists(nw->icn) ? nw->icn : "g_spark";
    if (nw->t[0]) W.nnews++; }
  const cJSON *s = js_obj(j, "support"); snprintf(W.url, sizeof W.url, "%s", js_str(s, "url", "")); snprintf(W.gh, sizeof W.gh, "%s", js_str(s, "github", ""));
  { const char *g = W.gh; if (!strncmp(g, "https://", 8)) g += 8; snprintf(W.ghShown, sizeof W.ghShown, "%s", g); }
  snprintf(W.stitle, sizeof W.stitle, "%s", js_str(s, "title", "")); snprintf(W.smsg, sizeof W.smsg, "%s", js_str(s, "message", ""));
  cJSON_Delete(j);
  const char *u = W.url; if (!strncmp(u, "https://", 8)) u += 8; else if (!strncmp(u, "http://", 7)) u += 7; if (!strncmp(u, "www.", 4)) u += 4;
  snprintf(W.urlShown, sizeof W.urlShown, "%s", u);
  /* "1.0.0" reads as "1.0" */
  snprintf(W.short_ver, sizeof W.short_ver, "%s", W.ver); size_t L = strlen(W.short_ver);
  if (L > 2 && !strcmp(W.short_ver + L - 2, ".0") && strchr(W.short_ver, '.') != W.short_ver + L - 2) W.short_ver[L - 2] = 0;
  return W.nnews > 0;
}
/* the tour (tour 1) or the version's page, over whatever is on screen */
static void open_guide(int tour) {
  W.n = 0; W.tour = tour;
  if (tour) { for (int i = 0; i < NTOUR; i++) { Page *pg = &W.p[W.n++]; memset(pg, 0, sizeof *pg); snprintf(pg->title, sizeof pg->title, "%s", TOUR[i].t); snprintf(pg->lead, sizeof pg->lead, "%s", TOUR[i].l);
      for (int b = 0; b < MAXB && TOUR[i].b[b]; b++) { snprintf(pg->b[b], sizeof pg->b[b], "%s", TOUR[i].b[b]); pg->ic[b] = TOUR[i].ic[b]; pg->nb++; } pg->pv = TOUR[i].pv; pg->ctl = TOUR[i].ctl;
      if (pg->pv == PV_YOURKEYS) { int tm = !tmdb_builtin(), tk = !trakt_builtin(); ykOff = 0; /* (a key this build carries is not asked for: its step goes, and the page with both) */
        if (!tm && !tk) W.n--;
        else if (!tk) { pg->nb = 1; snprintf(pg->lead, sizeof pg->lead, "%s", "Ratings and comments come built in. A TMDB key brings more: OK shows the code. You can always add it later in Settings \xe2\x80\xba Keys."); }
        else if (!tm) { pg->nb = 1; ykOff = 1; snprintf(pg->b[0], sizeof pg->b[0], "%s", pg->b[1]); pg->ic[0] = pg->ic[1];
          snprintf(pg->lead, sizeof pg->lead, "%s", "Titles and descriptions in your language come built in. A Trakt id brings ratings and comments: OK shows the code. You can always add it later in Settings \xe2\x80\xba Keys."); } } } }
  else { Page *pg = &W.p[W.n++]; memset(pg, 0, sizeof *pg); pg->pv = PV_NEWS; snprintf(pg->title, sizeof pg->title, "%s", "What\xe2\x80\x99s new"); }
  if (tour && W.n < MAXP && load()) { /* the first start: the version's news after the tour, the thanks (support) page staying last */
    int at = W.n; if (at > 0 && W.p[at - 1].pv == PV_THANKS) at--;
    for (int i = W.n; i > at; i--) W.p[i] = W.p[i - 1]; W.n++;
    Page *pg = &W.p[at]; memset(pg, 0, sizeof *pg); pg->pv = PV_NEWS; snprintf(pg->title, sizeof pg->title, "%s", "What\xe2\x80\x99s new"); }
  si_load(); /* (the bundle missing: Home's own) */
  if (!httpd_url("", qrUrl, sizeof qrUrl)) snprintf(qrUrl, sizeof qrUrl, "http://tv.local:8099"); /* (no network address: a stand-in for the picture) */
  { char ts[400]; size_t k = 0; ts[0] = 0; for (int i = 0; i < W.n && k + 40 < sizeof ts; i++) k += (size_t)snprintf(ts + k, sizeof ts - k, "%s%.30s", i ? " | " : "", W.p[i].title); fprintf(stderr, "[guide] %s, %d pages: %s\n", tour ? "first start" : "what's new", W.n, ts); } /* (the log says what a user saw) */
  W.open = 1; W.closing = 0; W.cur = W.from = W.step = W.fromStep = 0; W.lg = 0; W.pageAt = W.stepAt = app_now(); W.sw.on = 0; tween_set(&W.dot, 0); tween_set(&W.focus, 0);
  tween_set(&W.knob, best_on() ? 1 : 0); memset(&W.nudge, 0, sizeof W.nudge); W.speed[0] = 0; W.doneAt = 0; memset(orbs, 0, sizeof orbs); W.tipWant = 0; tween_set(&W.tipK, 0);
  timer_start(&W.in, 0.9f); sglass_bake(&glass, 0, 0, SB_W, SB_H); app_ambient_guide(1); app_overlay = &card;
}
static int env_on(const char *k) { const char *v = getenv(k); return v && *v && strcmp(v, "0"); }
/* once Home is up and nothing else is open: the first start ever (the tour), or the first start of a new version */
/* a guide or the version's page is due (the first start, or a new version): its pictures are got ready from the start */
static int due = -1;
static int is_due(void) { if (due < 0) { int has = load(); const char *seen = store_get("whatsNewSeen", ""); due = getenv("SB_GUIDE") || getenv("SB_WHATSNEW") || (!store_get("guideSeen", NULL) && !seen[0]) || (has && strcmp(seen, W.ver)); } return due; }
int welcome_ready(void) { return !is_due() || W.checked || preload(); }
void welcome_tick(void) {
  if (W.open || (!W.checked && is_due())) { int r = preload(); static double since; if (!since) since = app_now(); if (!W.open && !r && app_now() - since < 4) return; } /* (kept fresh while it shows; it opens once they are in, 4 s at most) */
  if (W.checked || W.open || !app_splash_done || SCR != SCR_HOME || app_overlay || app_idle() < 1.2f) return;
  W.checked = 1; if (getenv("SB_STRESS")) return;
  int has = load(), forceG = env_on("SB_GUIDE"), forceW = env_on("SB_WHATSNEW");
  const char *seen = store_get("whatsNewSeen", ""); int first = !store_get("guideSeen", NULL) && !seen[0];
  if (forceG || (!forceW && first)) open_guide(1); else if (has && (forceW || strcmp(seen, W.ver))) open_guide(0);
}
void welcome_open_now(void) {
  if (W.checked || W.open || getenv("SB_STRESS")) return;
  W.checked = 1; int has = load(); const char *seen = store_get("whatsNewSeen", ""); int first = !store_get("guideSeen", NULL) && !seen[0];
  if (first) open_guide(1); else if (has && strcmp(seen, W.ver)) open_guide(0); else return;
  W.in.on = 0; W.fromSignIn = 1; /* already in: the loading fades off it, its glows carrying on */
}
void welcome_show_guide(void) { if (W.open && !W.closing) return; load(); W.checked = 1; open_guide(1); }
const char *welcome_support_url(void) { if (!W.url[0]) load(); return W.url; }
void welcome_show_whatsnew(void) { if ((W.open && !W.closing) || !load()) return; W.checked = 1; open_guide(0); } /* Settings > About: the version's page again */
const char *welcome_github_url(void) { if (!W.url[0]) load(); return W.gh; }

