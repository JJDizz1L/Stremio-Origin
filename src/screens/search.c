/* Search: our own keyboard on the left (no system keyboard over the app), suggestions under
   it, a small results grid on the right. Results follow the typing: IMDb's suggestions first
   (its first answer is nearly always the one meant), then every installed catalog that searches (Cinemeta's films and
   series when none does), each answering on its own time. */
#include "screens/search.h"
#include "core/keys.h"
#include "core/i18n.h"
#include "platform/plat.h"
#include "ui/gfx.h"
#include "ui/text.h"
#include "ui/img.h"
#include "ui/icon.h"
#include "ui/widgets.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/hit.h"
#include "core/httpd.h"
#include "core/tmdb.h"
#include "ui/qr.h"
#include <SDL2/SDL.h>

#define SKEYS "abcdefghijklmnopqrstuvwxyz1234567890"
#define NK 36
#define SCOLS 6
#define SMAX 24
#define NCAT 16
#define SGC 4
#define SROWH 456.0f   /* the scroll step per row (the web's; its rows lie 420 apart) */
#define SPITCH 420.0f  /* a poster (354) and the space under it (66) */
#define LEFT_X 72.0f
#define TOP_Y 132.0f
#define RIGHT_X (72.0f + 620)
#define GRID_W (SB_W - 72 - RIGHT_X)
static struct {
  int zone; /* 0 kb, 1 sug, 2 res */ int kr, kc, si, ri; char q[48]; unsigned tok; int built;
  ItemList items; char sug[6][128]; int nsug; int loading, final;
  ItemList top, cat[NCAT]; int topDone, ncat, catDone[NCAT], topFail, catFail[NCAT], allFailed; double liveAt, catAt; int livePending, catPending;
  Tween kfx, kfy, kfw, caretX; double caretAt; int caretSeen; /* (the text cursor's glide and breath) */
  Tween sugX; Ring ring; Tween gridY; Timer1 in; Timer1 gridIn; Nudge nudge; int kfShown;
  int drawn;                                  /* a search ran once: the hint line speaks from then on */
  char qrUrl[300]; int qrOk; double qrAt, typedAt; int seq, seqKnown, phone; /* typing on the phone */
  char pu[SMAX][512];                         /* each result's poster address, resolved when the results change */
} S;
static int rows_of(void) { return (NK + SCOLS - 1) / SCOLS; }
static int key_at(int r, int c) { if (r < rows_of()) return r * SCOLS + c; return NK + (c / 2 < 2 ? c / 2 : 2); } /* the last row: Space, backspace, (clear) */
static void live(void);
static void set_q(const char *q) { snprintf(S.q, sizeof S.q, "%.40s", q); S.liveAt = app_now() + 0.26; S.livePending = 1; }
static void type_key(int ki) {
  char q[64]; snprintf(q, sizeof q, "%s", S.q); size_t n = strlen(q);
  if (ki < NK) { if (n < 40) { q[n] = SKEYS[ki]; q[n + 1] = 0; } }
  else if (ki == NK) { if (n && q[n - 1] != ' ') { q[n] = ' '; q[n + 1] = 0; } }  /* space */
  else if (ki == NK + 1) { if (n) { n--; while (n && ((unsigned char)q[n] & 0xc0) == 0x80) n--; q[n] = 0; } } /* backspace */
  else if (ki == NK + 2) q[0] = 0;                                                  /* clear */
  set_q(q);
}
static void results_draw_state(void) {
  if (S.ri >= S.items.n) S.ri = S.items.n ? S.items.n - 1 : 0; if (S.si >= S.nsug) S.si = S.nsug ? S.nsug - 1 : 0;
  if (S.zone == 2 && !S.items.n) S.zone = 0; if (S.zone == 1 && !S.nsug) S.zone = 0;
}
/* physical backspace (desktop only: the remote's Back keeps navigating): deletes a letter while the query has one */
static int backspace_q(void) { if (!plat_text_input() || !S.q[0]) return 0; char q[64]; snprintf(q, sizeof q, "%s", S.q); size_t n = strlen(q); n--; while (n && ((unsigned char)q[n] & 0xc0) == 0x80) n--; q[n] = 0; set_q(q); return 1; }
/* the same title from two sources (an IMDb id and another addon's id): the same kind, name and year */
static int same_title(const Item *a, const Item *b) { return !strcmp(a->id, b->id) || (!strcmp(a->type, b->type) && !strcasecmp(a->name, b->name) && !strncmp(a->metaLine, b->metaLine, 4)); }
/* a notice an addon sends as a result (its search failing, a missing key): not a title. Its id says so, or a name
   saying so on an id that is no title's */
static int has_word(const char *s, const char *const *w, int n) { /* w lower-case */
  for (int i = 0; i < n; i++) { size_t L = strlen(w[i]); for (const char *p = s; *p; p++) if (!strncasecmp(p, w[i], L)) return 1; } return 0; }
static int notice_item(const Item *it) {
  static const char *const ID[] = { "error", "unavailable", "notice", "placeholder", "failed" }, *NM[] = { "unavailable", "error", "failed", "api key", "not configured", "try again" };
  static const char *const TP[] = { "tt", "tmdb:", "tvdb:", "kitsu:", "mal:", "anilist:", "anidb:", "imdb" };
  if (has_word(it->id, ID, 5)) return 1;
  for (int i = 0; i < 8; i++) if (!strncmp(it->id, TP[i], strlen(TP[i])) && (i || (it->id[2] >= '0' && it->id[2] <= '9'))) return 0;
  return has_word(it->name, NM, 6);
}
static void merge(void) {
  Item *out = calloc(SMAX, sizeof(Item)); int n = 0;
  #define ADD(it) do { int dup = notice_item(it); for (int d = 0; d < n; d++) if (same_title(&out[d], (it))) dup = 1; if (!dup && (it)->id[0] && n < SMAX) out[n++] = *(it); } while (0)
  for (int i = 0; i < S.top.n && n < SMAX; i++) ADD(&S.top.items[i]);
  for (int j = 0; j < 40 && n < SMAX; j++) for (int c = 0; c < S.ncat && n < SMAX; c++) if (j < S.cat[c].n) ADD(&S.cat[c].items[j]); /* the catalogs take turns */
  #undef ADD
  int changed = n != S.items.n; for (int i = 0; !changed && i < n; i++) if (strcmp(out[i].id, S.items.items[i].id)) changed = 1;
  itemlist_clear(&S.items); S.items.items = out; S.items.n = n;
  for (int i = 0; i < n; i++) sb_poster_of(&out[i], S.pu[i], sizeof S.pu[i]);
  if (changed && n) timer_start(&S.gridIn, 0.3f); /* new results fade in */
  int all = S.topDone && !S.catPending; for (int c = 0; c < S.ncat; c++) all = all && S.catDone[c]; S.final = all; S.loading = !S.final;
  int failed = all && S.topFail; for (int c = 0; c < S.ncat; c++) failed = failed && S.catFail[c]; S.allFailed = failed && !n; /* every source failed: said in the hint line, never as a card */
  if (S.phone && n && SCR == SCR_SEARCH) { S.phone = 0; S.zone = 2; S.ri = 0; topNav.active = 0; } /* typed on the phone: the results take the focus */
  results_draw_state();
}
static void on_sug(ItemList *l, char names[6][128], int nn, void *ud) {
  if ((unsigned)(uintptr_t)ud != S.tok) { if (l) itemlist_free(l); return; }
  itemlist_clear(&S.top); S.topFail = !l || l->total < 0; if (l) { S.top = *l; free(l); }
  S.nsug = 0; for (int i = 0; i < nn && S.nsug < 6; i++) { int dup = 0; for (int k = 0; k < S.nsug; k++) if (!strcasecmp(S.sug[k], names[i])) dup = 1; if (!dup && names[i][0]) snprintf(S.sug[S.nsug++], 128, "%s", names[i]); }
  S.topDone = 1; merge();
}
/* a catalog's answer: the query's token above, the catalog's place in the low 5 bits */
static void on_cat(ItemList *l, int cached, void *ud) { (void)cached; uintptr_t t = (uintptr_t)ud; int c = (int)(t & 31);
  if ((unsigned)(t >> 5) != (S.tok & 0x7ffffff) || c >= S.ncat) { itemlist_free(l); return; } itemlist_clear(&S.cat[c]); S.catFail[c] = l->total < 0; if (!S.catFail[c]) { int k = 0; for (int i = 0; i < l->n; i++) if (!notice_item(&l->items[i])) k++; else fprintf(stderr, "[search] catalog %d sent a notice, left out: %s (%s)\n", c, l->items[i].name, l->items[i].id); S.catFail[c] = l->n > 0 && !k; } /* only notices: failed too */
  S.cat[c] = *l; free(l); S.catDone[c] = 1; merge(); }
static void urlenc(const char *s, char *out, size_t cap) { size_t o = 0; for (; *s && o + 4 < cap; s++) { unsigned char c = *s; if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') out[o++] = c; else o += snprintf(out + o, cap - o, "%%%02X", c); } out[o] = 0; }
static void live(void) {
  char q[48]; snprintf(q, sizeof q, "%s", S.q); char *e = q + strlen(q); while (e > q && e[-1] == ' ') *--e = 0; const char *p = q; while (*p == ' ') p++;
  S.tok++; S.topDone = 0; S.topFail = 0; S.allFailed = 0; for (int c = 0; c < S.ncat; c++) { S.catDone[c] = 0; S.catFail[c] = 0; itemlist_clear(&S.cat[c]); } S.ncat = 0; S.drawn = 1;
  if (!*p) { itemlist_clear(&S.items); S.nsug = 0; S.loading = 0; S.final = 1; results_draw_state(); return; }
  S.loading = 1; S.final = 0;
  sb_suggest(p, on_sug, (void *)(uintptr_t)S.tok);
  S.catAt = app_now() + 0.5; S.catPending = 1;
}
static void cat_go(void) {
  char q[48]; snprintf(q, sizeof q, "%s", S.q); char *e = q + strlen(q); while (e > q && e[-1] == ' ') *--e = 0; const char *p = q; while (*p == ' ') p++;
  CatalogDef defs[NCAT]; int n = sb_search_defs(p, defs, NCAT);
  if (!n) { char enc[200]; urlenc(p, enc, sizeof enc); const char *ty[2] = { "movie", "series" }; /* no installed catalog searches: Cinemeta's */
    for (int i = 0; i < 2; i++) { memset(&defs[n], 0, sizeof defs[n]); snprintf(defs[n].type, sizeof defs[n].type, "%s", ty[i]); snprintf(defs[n].url, sizeof defs[n].url, CINEMETA "/catalog/%s/top/search=%s.json", ty[i], enc); n++; } }
  for (int c = 0; c < S.ncat; c++) itemlist_clear(&S.cat[c]); S.ncat = n; for (int c = 0; c < n; c++) S.catDone[c] = S.catFail[c] = 0;
  fprintf(stderr, "[search] \"%s\" in %d catalog(s)\n", p, n);
  for (int c = 0; c < n; c++) sb_catalog_url(defs[c].url, defs[c].type, 20, on_cat, (void *)(uintptr_t)(((uintptr_t)(S.tok & 0x7ffffff) << 5) | (uintptr_t)c));
}
/* the QR for typing on the phone, and what arrives from it (asked only while Search is up) */
static void phone_ask(void) { if (S.qrOk) { httpd_start(); return; } S.qrAt = app_now(); S.qrOk = httpd_url("", S.qrUrl, sizeof S.qrUrl); }
static void phone_tick(void) {
  double now = app_now();
  if (!S.qrOk && now - S.qrAt >= 2.5) phone_ask(); /* no network address yet: again in a moment */
  if (!S.qrOk || now - S.typedAt < 2) return; S.typedAt = now;
  char t[96]; int sq = httpd_typed(S.seqKnown ? S.seq : 0, t, sizeof t);
  if (!S.seqKnown) { S.seqKnown = 1; S.seq = sq; return; } /* what was typed before this visit stays out */
  if (sq > S.seq) { S.seq = sq; S.phone = 1; app_glow_kick(); snprintf(S.q, sizeof S.q, "%.40s", t); S.livePending = 0; live(); }
}
static void enter(const void *p) { (void)p; S.zone = (S.zone == 2 && S.items.n) ? 2 : 0; timer_start(&S.in, 0.42f); results_draw_state(); S.typedAt = app_now() - 0.8; phone_ask(); if (plat_text_input()) SDL_StartTextInput(); } /* desktop: the physical keyboard types straight into the query */
static void leave(void) { SDL_StopTextInput(); } /* Search keeps its query and results for when you come back */
static void text(const char *s) { /* typed text from the physical keyboard (SDL_TEXTINPUT): appended to the query */
  char q[96]; size_t n = 0; q[0] = 0;
  for (const char *c = s; *c && n + 4 < sizeof q;) { unsigned char b = *c; if (b < 0x20) { c++; continue; } /* no newlines/tabs from a paste */
    int len = b < 0x80 ? 1 : b >= 0xf0 ? 4 : b >= 0xe0 ? 3 : b >= 0xc0 ? 2 : 1; int k; for (k = 0; k < len && c[k]; k++) q[n++] = c[k]; q[n] = 0; c += k ? k : 1; }
  if (!q[0]) return; char full[96]; snprintf(full, sizeof full, "%s%s", S.q, q); set_q(full); }
static const Item *focused(void) { return S.zone == 2 && S.ri < S.items.n ? &S.items.items[S.ri] : NULL; }
static int key(int k) {
  int rows = rows_of(), C = SGC; int z0 = S.zone, kr0 = S.kr, kc0 = S.kc, si0 = S.si, ri0 = S.ri;
  if (S.zone == 0) {
    if (k == K_LEFT) { if (S.kr == rows) S.kc = S.kc >= 2 ? S.kc - 2 : 0; else S.kc = S.kc > 0 ? S.kc - 1 : 0; }
    else if (k == K_RIGHT) { int edge = S.kr == rows ? S.kc >= 4 : S.kc >= SCOLS - 1; if (edge) { if (S.nsug) { S.zone = 1; S.si = 0; } else if (S.items.n) { S.zone = 2; S.ri = 0; } } else S.kc = S.kc + (S.kr == rows ? 2 : 1) < SCOLS ? S.kc + (S.kr == rows ? 2 : 1) : SCOLS - 1; }
    else if (k == K_UP) { if (S.kr > 0) S.kr--; else { topnav_focus(); return 1; } }
    else if (k == K_DOWN) { if (S.kr < rows) S.kr++; }
    else if (k == K_OK) type_key(key_at(S.kr, S.kc));
    else if (k == K_BACK) { if (backspace_q()) return 1; return 0; }
    if (k != K_OK && z0 == S.zone && kr0 == S.kr && kc0 == S.kc) nudge_start(&S.nudge, k); /* the key with its ring */
  } else if (S.zone == 1) {
    if (k == K_LEFT) { if (S.si > 0) S.si--; else S.zone = 0; }
    else if (k == K_RIGHT) { if (S.si < S.nsug - 1) S.si++; }
    else if (k == K_DOWN) { if (S.items.n) { S.zone = 2; S.ri = 0; } }
    else if (k == K_UP) { topnav_focus(); return 1; }
    else if (k == K_OK) { snprintf(S.q, sizeof S.q, "%.40s", S.sug[S.si]); S.livePending = 0; live(); }
    else if (k == K_BACK) { if (backspace_q()) return 1; S.zone = 0; return 1; }
    if (k != K_OK && z0 == S.zone && si0 == S.si) nudge_start(&S.nudge, k);
  } else {
    int r = S.ri / C, c = S.ri % C;
    if (k == K_LEFT) { if (c > 0) S.ri--; else S.zone = 0; }
    else if (k == K_RIGHT) { if (c < C - 1 && S.ri < S.items.n - 1) S.ri++; }
    else if (k == K_UP) { if (r > 0) S.ri -= C; else if (S.nsug) S.zone = 1; }
    else if (k == K_DOWN) { if (S.ri + C < S.items.n) S.ri += C; else if (r < (S.items.n - 1) / C) S.ri = S.items.n - 1; }
    else if (k == K_OK) { app_open_item(&S.items.items[S.ri]); return 1; }
    else if (k == K_BACK) { if (backspace_q()) return 1; S.zone = 0; return 1; }
    if (z0 == S.zone && ri0 == S.ri) nudge_start(&S.nudge, k);
  }
  return 1;
}
static void key_rect(int ki, float *x, float *y, float *w) {
  float kx = LEFT_X, ky = TOP_Y + 76 + 18;
  if (ki < NK) { *x = kx + (ki % SCOLS) * 92; *y = ky + (ki / SCOLS) * 74; *w = 84; }
  else { *x = kx + (ki - NK) * 184; *y = ky + rows_of() * 74; *w = 176; }
  if (app_rtl()) *x = SB_W - *x - *w;
}
/* the text art of the results around the focus, for their pages (drawn there at 520x175): ready when one opens */
static void logos_ahead(void) { static double at0; double now = app_now(); if (now - at0 < 0.5 || S.zone != 2) return; at0 = now;
  for (int i = S.ri - SGC; i <= S.ri + SGC; i++) { if (i < 0 || i >= S.items.n) continue; char lu[512]; sb_logo_of(&S.items.items[i], lu, sizeof lu); if (lu[0]) img_prefetch_pri(lu, 520, 175); } }
static void tick(float dt) {
  { static unsigned seen; unsigned h = httpd_hits(); if (h != seen) { if (seen && SCR == SCR_SEARCH) app_glow_kick(); seen = h; } } /* the phone opened the page: the glows feel it */
  logos_ahead();
  timer_tick(&S.in, dt); timer_tick(&S.gridIn, dt); nudge_tick(&S.nudge, dt); tween_tick(&S.kfx, dt); tween_tick(&S.caretX, dt); tween_tick(&S.kfy, dt); tween_tick(&S.kfw, dt); tween_tick(&S.sugX, dt); ring_tick(&S.ring, dt); tween_tick(&S.gridY, dt);
  if (S.livePending && app_now() >= S.liveAt) { S.livePending = 0; live(); }
  if (S.catPending && app_now() >= S.catAt) { S.catPending = 0; cat_go(); }
  phone_tick();
  int act = !topNav.active;
  float x, y, w; key_rect(key_at(S.kr, S.kc), &x, &y, &w);
  if (!S.kfShown) { tween_set(&S.kfx, x); tween_set(&S.kfy, y); tween_set(&S.kfw, w); S.kfShown = 1; } else { tween_to(&S.kfx, x, 0.14f, EASE_OUT); tween_to(&S.kfy, y, 0.14f, EASE_OUT); tween_to(&S.kfw, w, 0.14f, EASE_OUT); }
  int row = S.ri / SGC; tween_to(&S.gridY, -(row > 1 ? (row - 1) * SROWH : 0), 0.3f, EASE_OUT);
  if (act && S.zone == 2 && S.ri < S.items.n) { float gx = (app_rtl() ? SB_W - RIGHT_X - 236 - (S.ri % SGC) * 272 : RIGHT_X + (S.ri % SGC) * 272), gy = row * SPITCH; ring_to(&S.ring, gx, gy, 0); } else ring_hide(&S.ring);
}
static void ptr_off(void) { if (topNav.active) topNav.active = 0; }
static void key_focus(int ki, int b) { (void)b; int kr = ki < NK ? ki / SCOLS : rows_of(), kc = ki < NK ? ki % SCOLS : (ki - NK) * 2; ptr_off(); if (S.zone != 0 || S.kr != kr || S.kc != kc) { S.zone = 0; S.kr = kr; S.kc = kc; } }
static void sug_focus(int i, int b) { (void)b; ptr_off(); if (S.zone != 1 || S.si != i) { S.zone = 1; S.si = i; } }
static void res_focus(int i, int b) { (void)b; ptr_off(); if (S.zone != 2 || S.ri != i) { S.zone = 2; S.ri = i; } }
static void draw(void) {
  int rtl = app_rtl(); float ia = ease_apply(EASE_CSS_EASE, timer_x(&S.in)), iy = 22 * (1 - ia); int act = !topNav.active;
  gfx_opacity_push(S.in.on ? ia : 1); gfx_offset_push(0, S.in.on ? iy : 0); /* the pieces rise in; the grid only fades (its own scroll stays) */
  /* the query box */
  float qx = rtl ? SB_W - LEFT_X - 552 : LEFT_X, qy = TOP_Y;
  gfx_rrect(qx, qy, 552, 76, 20, WHITE_A(18)); gfx_ring(qx, qy, 552, 76, 20, 1, WHITE_A(26));
  icon_draw("search_box", rtl ? qx + 552 - 24 - 32 : qx + 24, qy + 22, 32, WHITE_A(153));
  float tx = rtl ? qx + 552 - 24 - 32 - 16 : qx + 24 + 32 + 16, th = text_line_height(32);
  /* the text cursor: rounded, gliding to where the next letter goes, solid while typing, else a soft slow breath */
  float cw = 0;
  if (S.q[0]) cw = text_draw(S.q, tx, qy + (76 - th) / 2, 32, 600, WHITE_A(255), 0, 552 - 120, rtl ? ALIGN_RIGHT : ALIGN_LEFT);
  else text_draw(T("Search films and series"), rtl ? tx - 14 : tx + 14, qy + (76 - th) / 2, 32, 500, WHITE_A(133), 0, 552 - 120, rtl ? ALIGN_RIGHT : ALIGN_LEFT);
  { float want = S.q[0] ? cw + 4 : 0; if (S.caretX.to != want) { if (!S.caretSeen) tween_set(&S.caretX, want); else tween_to(&S.caretX, want, 0.12f, EASE_OUT); S.caretAt = app_now(); } S.caretSeen = 1;
    double since = app_now() - S.caretAt; float ca = since < 0.6 ? 1 : 0.35f + 0.65f * (0.5f + 0.5f * cosf((float)((since - 0.6) * 6.2832 / 1.1)));
    float cx = rtl ? tx - S.caretX.v - 3 : tx + S.caretX.v; gfx_rrect(cx, qy + 20, 3, 36, 1.5f, col_alpha(ACC2 | 0xff000000u, ca)); }
  /* the keyboard: the accent block slides under the focused key */
  for (int i = 0; i < NK + 3; i++) { float x, y, w; key_rect(i, &x, &y, &w); hit_add(x, y, w, 66, key_focus, NULL, i, 0); gfx_rrect(x, y, w, 66, 14, WHITE_A(15)); } /* every key's plate, under the accent too: no hole where it glides to */
  if (act && S.zone == 0) { gfx_offset_push(nudge_x(&S.nudge), nudge_y(&S.nudge)); gfx_rrect(S.kfx.v, S.kfy.v, S.kfw.v, 66, 14, ACC); gfx_offset_pop(); }
  for (int i = 0; i < NK + 3; i++) { float x, y, w; key_rect(i, &x, &y, &w); int f = act && S.zone == 0 && key_at(S.kr, S.kc) == i;
    float nx = f ? nudge_x(&S.nudge) : 0, ny = f ? nudge_y(&S.nudge) : 0;
    uint32_t fg = f ? ACC_FG : WHITE_A(219);
    if (i < NK) { char s[2] = { (char)(SKEYS[i] >= 'a' ? SKEYS[i] - 32 : SKEYS[i]), 0 }; text_draw(s, x + w / 2 + nx, y + (66 - text_line_height(28)) / 2 + ny, 28, 700, fg, 0, 0, ALIGN_CENTER); }
    else if (i == NK + 2) text_draw(T("Clear"), x + w / 2 + nx, y + (66 - text_line_height(24)) / 2 + ny, 24, 700, fg, 0.4f, w - 16, ALIGN_CENTER);
    else if (i == NK) text_draw(T("Space"), x + w / 2 + nx, y + (66 - text_line_height(24)) / 2 + ny, 24, 700, fg, 0.4f, w - 16, ALIGN_CENTER);
    else icon_draw("backspace", x + w / 2 - 15 + nx, y + 18 + ny, 30, fg); }
  /* the QR for typing on the phone, under the keyboard (the code only where the TV has a network address) */
  if (S.qrOk) { float qy = TOP_Y + 76 + 18 + (rows_of() + 1) * 74 + 30, qx = rtl ? SB_W - LEFT_X - 150 : LEFT_X;
    if (!qr_draw(S.qrUrl, qx, qy, 150, 14, 8)) gfx_rrect(qx, qy, 150, 150, 14, WHITE_A(20));
    const char *t1 = T("Type on your phone"), *t2 = T("Scan with your phone's camera, then type there"); float tw = 552 - 150 - 22, lh2 = 31;
    static int n2; static unsigned n2Lg; if (n2Lg != i18n_gen()) { n2Lg = i18n_gen(); n2 = text_wrap_count(t2, tw, 3, 23, 400, 0); } /* once per language */
    float th = text_line_height(24) + 6 + n2 * lh2, ty = qy + 75 - th / 2, tx = rtl ? qx - 22 : qx + 150 + 22;
    text_draw(t1, tx, ty, 24, 700, WHITE_A(255), 0, tw, rtl ? ALIGN_RIGHT : ALIGN_LEFT);
    text_draw_wrapped(t2, tx, ty + text_line_height(24) + 6 + (lh2 - text_line_height(23)) / 2, tw, 3, 23, 400, WHITE_A(191), 0, rtl ? ALIGN_RIGHT : ALIGN_LEFT, lh2); }
  /* the right side: hint, suggestions, the grid */
  float rx = rtl ? SB_W - RIGHT_X : RIGHT_X, ry = TOP_Y; int al = rtl ? ALIGN_RIGHT : ALIGN_LEFT;
  if (S.q[0] && !S.items.n && !S.final) { /* the loading line */
    float lx = rtl ? rx - 220 : rx, ly = ry + 14; gfx_rrect(lx, ly, 220, 3, 2, WHITE_A(31)); gfx_clip_push(lx, ly, 220, 3);
    float ph = fmodf((float)app_now() / 1.1f, 1), p = ease_apply(EASE_IN_OUT, ph), sx = lx + 220 * 0.4f * (-1 + 3.5f * p);
    gfx_rrect_h(sx, ly, 44, 3, 2, col_alpha(ACC2, 0), ACC2); gfx_rrect_h(sx + 44, ly, 44, 3, 2, ACC2, col_alpha(ACC2, 0)); gfx_clip_pop(); }
  else if (S.drawn) { static char h[160], hq[48]; static int hn = -1; static unsigned hLg; /* the hint line: made when the query, the results or the language change */
    int hs = S.items.n ? 1 : S.allFailed ? 2 : 0;
    if (hLg != i18n_gen() || hn != hs || strcmp(hq, S.q)) { hLg = i18n_gen(); hn = hs; snprintf(hq, sizeof hq, "%s", S.q);
      if (!S.q[0]) snprintf(h, sizeof h, "%s", T("Type to search films and series")); else if (hs == 2) snprintf(h, sizeof h, "%s", T("Search is unavailable right now. Try again in a moment"));
      else snprintf(h, sizeof h, "%s", T1(S.items.n ? "Results for “{q}”" : "No results for “{q}”", "q", S.q)); }
    text_draw(h, rx, ry, 24, 600, WHITE_A(191), 0, GRID_W, al); }
  ry += text_line_height(24) + 18;
  if (S.nsug) { float cx = 0, xs[6], ws[6]; static char sw[6][128]; static float swW[6]; /* each suggestion measured once */
    for (int i = 0; i < S.nsug; i++) { if (strcmp(sw[i], S.sug[i])) { snprintf(sw[i], sizeof sw[i], "%s", S.sug[i]); swW[i] = text_width(S.sug[i], 24, 600, 0); } ws[i] = swW[i] + 40; if (ws[i] > 340) ws[i] = 340; xs[i] = cx; cx += ws[i] + 12; }
    float want = S.sugX.v; if (S.zone == 1) { float L = xs[S.si] + want; if (L < 0) want -= L; else if (L + ws[S.si] > GRID_W) want -= L + ws[S.si] - GRID_W; } tween_to(&S.sugX, want, 0.25f, EASE_OUT); /* the focused one whole */
    gfx_clip_push(rtl ? rx - GRID_W : rx, ry, GRID_W, 60);
    for (int i = 0; i < S.nsug; i++) { int f = act && S.zone == 1 && i == S.si; float x = rtl ? rx - xs[i] - S.sugX.v - ws[i] : rx + xs[i] + S.sugX.v, h = 20 + text_line_height(24);
      float nx = f ? nudge_x(&S.nudge) : 0; hit_add(x, ry, ws[i], h, sug_focus, NULL, i, 0); gfx_rrect(x + nx, ry, ws[i], h, 24, f ? WHITE_A(255) : WHITE_A(18)); text_draw(S.sug[i], x + 20 + nx, ry + 10, 24, 600, f ? RGB(17, 17, 17) : WHITE_A(209), 0, ws[i] - 40, ALIGN_LEFT); }
    gfx_clip_pop(); ry += 60; }
  gfx_offset_pop(); gfx_offset_push(0, 0); /* the grid: no rise, it only fades in with the screen */
  float gy = ry + 10; gfx_clip_push(rtl ? rx - GRID_W - 22 : rx - 22, gy - 8, GRID_W + 44, 828); /* (room for the focused poster to grow) */
  float ga = S.gridIn.on ? 1 - powf(1 - timer_x(&S.gridIn), 2) : 1; gfx_opacity_push(ga); gfx_offset_push(nudge_x(&S.nudge) * (S.zone == 2), nudge_y(&S.nudge) * (S.zone == 2) + gy + 12 + S.gridY.v);
  int fi = act && S.zone == 2 && S.ri < S.items.n ? S.ri : -1; float hp = app_hold_progress();
  for (int pass = 0; pass < 2; pass++) for (int i = 0; i < S.items.n; i++) { if ((i == fi) != pass) continue; /* the focused poster last, over its neighbours as it grows */
    int row = i / SGC; float x = rtl ? SB_W - RIGHT_X - 236 - (i % SGC) * 272 : RIGHT_X + (i % SGC) * 272, y = row * SPITCH; const Item *it = &S.items.items[i];
    const TmdbLoc *L = tmdb_loc(it->type, it->id, 1); /* translated poster and name, in place when they land */
    const char *src = L && L->poster && L->poster[0] ? L->poster : i < SMAX ? S.pu[i] : "";
    unsigned long long lk = lift_key(&S, i); lift_begin(lk, pass, pass ? hp : 0, x, y, 236, 354, 16);
    GLuint t = src && *src ? img_get_shaped(src, 236, 354, 16, 0) : 0; if (!t && (!src || !*src || img_shaped_failed(src, 236, 354, 16, 0))) ph_surface(x, y, 236, 354, 16); else ph_picture(t, x, y, 236, 354, 16, WHITE_A(255));
    if (pass && hp > 0) hold_trace_draw(x, y, 236, 354, 16, ease_apply(EASE_IN_OUT, hp));
    lift_end(lk, x, y, 236, 354, 16);
    hit_add(x, y, 236, 354 + 12 + 32, res_focus, NULL, i, 0); text_draw(L && L->name && L->name[0] ? L->name : it->name, rtl ? x + 236 - 2 : x + 2, y + 354 + 12, 25, 600, pass ? WHITE_A(255) : WHITE_A(217), 0, 232, al); }

  gfx_offset_pop(); gfx_opacity_pop(); gfx_clip_pop();
  gfx_offset_pop(); gfx_opacity_pop();
}
Screen searchScreen = { enter, leave, key, tick, draw, focused, NULL, text };
