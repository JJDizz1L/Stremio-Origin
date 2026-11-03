/* The best stream for this TV: Settings > Playback > "Play the best stream automatically". Play starts,
   without the list, the stream that suits this TV and this connection. What the TV can show and decode is
   asked once (its panel, HDR / Dolby Vision, the codecs its media engine takes), the connection is measured
   once when the setting is switched on. Each stream is read from its own text: resolution, HDR, audio,
   source, size (with the running time: its bitrate), seeds. Debrid files are taken at the best quality the
   TV and the line carry, torrents need live peers. The scoring is the web app's, number for number. */
#include "core/best.h"
#include "core/i18n.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifndef SB_BEST_PURE
#include "core/app.h"
#include "core/json.h"
#include "core/net.h"
#include "core/store.h"
#include "platform/plat.h"
#include <pthread.h>
#include <time.h>
#include <SDL2/SDL.h>
#endif

/* ---- reading the text ----------------------------------------------------------------------------- */
/* \b as the web reads it: letters, digits and _ are word characters, everything else (UTF-8 bytes included) is not */
static int wch(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }
static int is_dig(unsigned char c) { return c >= '0' && c <= '9'; }
static int word_at(const char *s, const char *p, size_t n, int before, int after) { return (!before || p == s || !wch((unsigned char)p[-1])) && (!after || !wch((unsigned char)p[n])); }
static int find_w(const char *s, const char *w, int before, int after) { size_t n = strlen(w); for (const char *p = s; (p = strstr(p, w)); p++) if (word_at(s, p, n, before, after)) return 1; return 0; }
static int has_word(const char *s, const char *w) { return find_w(s, w, 1, 1); }
static void lower(const char *s, char *out, size_t cap) { size_t i = 0; for (; s[i] && i + 1 < cap; i++) { unsigned char c = (unsigned char)s[i]; out[i] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c); } out[i] = 0; }
static void blank(char *s, const char *set) { for (; *s; s++) if (strchr(set, *s)) *s = ' '; }
/* after p: an optional space, then an optional dash (the web's " ?-?") */
static const char *opt_sp_dash(const char *p) { if (*p == ' ') p++; if (*p == '-') p++; return p; }

/* JavaScript's \s, in UTF-8: the length of the space at p, 0 when none */
static int js_space(const unsigned char *p) {
  if (*p == ' ' || (*p >= 9 && *p <= 13)) return 1;
  if (p[0] == 0xc2 && p[1] == 0xa0) return 2;
  if (p[0] == 0xe1 && p[1] == 0x9a && p[2] == 0x80) return 3;
  if (p[0] == 0xe2 && p[1] == 0x80 && ((p[2] >= 0x80 && p[2] <= 0x8a) || p[2] == 0xa8 || p[2] == 0xa9 || p[2] == 0xaf)) return 3;
  if (p[0] == 0xe2 && p[1] == 0x81 && p[2] == 0x9f) return 3;
  if (p[0] == 0xe3 && p[1] == 0x80 && p[2] == 0x80) return 3;
  if (p[0] == 0xef && p[1] == 0xbb && p[2] == 0xbf) return 3;
  return 0;
}
/* everything the addon says about a stream, as the web joins it: spaces folded, at most 600 UTF-16 units */
void best_stream_info(const Stream *s, char *out, size_t cap) {
  char raw[1200]; snprintf(raw, sizeof raw, "%s %s %s", s->name[0] ? s->name : s->addon, s->title, s->filename);
  size_t o = 0; int units = 0, sp = 0; const unsigned char *p = (const unsigned char *)raw;
  while (*p == ' ' || js_space(p)) p += js_space(p);
  while (*p && o + 5 < cap) {
    int k = js_space(p); if (k) { sp = 1; p += k; continue; }
    if (sp) { if (units >= 600) break; out[o++] = ' '; units++; sp = 0; }
    int len = *p < 0x80 ? 1 : *p >= 0xf0 ? 4 : *p >= 0xe0 ? 3 : *p >= 0xc0 ? 2 : 1, u = len == 4 ? 2 : 1;
    if (units + u > 600) break;
    for (int i = 0; i < len && p[i]; i++) out[o++] = (char)p[i];
    p += len; units += u;
  }
  out[o] = 0;
}

const char *best_res_of(const char *label) {
  char s[2048]; lower(label, s, sizeof s);
  if (strstr(s, "2160p") || has_word(s, "4k") || has_word(s, "uhd")) return "4K";
  if (strstr(s, "1440p") || has_word(s, "2k")) return "1440p";
  if (strstr(s, "1080p") || has_word(s, "fhd")) return "1080p";
  if (strstr(s, "720p")) return "720p";
  if (strstr(s, "576p") || strstr(s, "480p") || has_word(s, "sd")) return "SD";
  return "";
}
void best_hdr_of(const char *label, char *out, size_t cap) {
  char s[2048]; lower(label, s, sizeof s); blank(s, "._");
  int dv = has_word(s, "dv") || has_word(s, "dovi") || has_word(s, "dolby vision") || has_word(s, "dolbyvision");
  const char *h = (strstr(s, "hdr10+") || strstr(s, "hdr10 +") || strstr(s, "hdr10plus") || strstr(s, "hdr10 plus") || find_w(s, "hdr10p", 0, 1)) ? "HDR10+"
    : has_word(s, "hdr10") ? "HDR10" : (has_word(s, "hdr") && !dv) ? "HDR" : has_word(s, "hlg") ? "HLG" : "";
  snprintf(out, cap, "%s%s%s", dv ? "DV" : "", dv && *h ? " · " : "", h);
}
/* "dts", the web's " ?-?", then what must follow (and a word end after it when asked) */
static int dts_then(const char *s, const char *tail, int end) {
  size_t n = strlen(tail);
  for (const char *p = s; (p = strstr(p, "dts")); p++) { const char *q = opt_sp_dash(p + 3); if (!strncmp(q, tail, n) && (!end || !wch((unsigned char)q[n]))) return 1; }
  return 0;
}
static int dts_hd_ma(const char *s) {
  for (const char *p = s; (p = strstr(p, "dts")); p++) { const char *q = opt_sp_dash(p + 3); if (!strncmp(q, "hd", 2) && !strncmp(opt_sp_dash(q + 2), "ma", 2)) return 1; }
  return 0;
}
/* "dd" / "aac" then " ?[257] ?[01]" (5.1 written as 5 1, 51...), with the web's word ends when asked */
static int codec_ch(const char *s, const char *w, int bounds) {
  size_t n = strlen(w);
  for (const char *p = s; (p = strstr(p, w)); p++) {
    if (bounds && p > s && wch((unsigned char)p[-1])) continue;
    const char *q = p + n; if (*q == ' ') q++; if (*q != '2' && *q != '5' && *q != '7') continue; q++; if (*q == ' ') q++; if (*q != '0' && *q != '1') continue; q++;
    if (!bounds || !wch((unsigned char)*q)) return 1;
  }
  return 0;
}
void best_audio_of(const char *label, char *out, size_t cap) {
  char s[2048]; lower(label, s, sizeof s); blank(s, "._");
  const char *au = "";
  if (strstr(s, "truehd")) au = "TrueHD"; else if (dts_then(s, "x", 1)) au = "DTS:X"; else if (dts_hd_ma(s) || dts_then(s, "ma", 1)) au = "DTS-HD MA";
  else if (dts_then(s, "hd", 0)) au = "DTS-HD"; else if (has_word(s, "dts")) au = "DTS";
  else if (find_w(s, "ddp", 1, 0) || strstr(s, "dd+") || strstr(s, "eac3") || strstr(s, "e-ac3") || strstr(s, "eac-3") || strstr(s, "e-ac-3") || strstr(s, "dolby digital plus")) au = "EAC3";
  else if (has_word(s, "dd") || codec_ch(s, "dd", 1) || has_word(s, "ac3") || has_word(s, "ac-3") || strstr(s, "dolby digital")) au = "AC3";
  else if (has_word(s, "aac") || codec_ch(s, "aac", 0) || strstr(s, "aac2")) au = "AAC"; else if (has_word(s, "opus")) au = "Opus"; else if (has_word(s, "flac")) au = "FLAC"; else if (has_word(s, "mp3")) au = "MP3";
  int atmos = strstr(s, "atmos") != NULL; char a[24]; snprintf(a, sizeof a, "%s%s%s", au, *au && atmos ? " " : "", atmos ? "Atmos" : "");
  /* channels: the first "5 1" not glued to another number */
  char ch[8] = "";
  if (*a) for (const char *p = s; *p; p++) if (p[0] >= '1' && p[0] <= '9' && (p == s || (!is_dig((unsigned char)p[-1]) && p[-1] != '.')) && (p[1] == ' ' || p[1] == '.') && (p[2] == '0' || p[2] == '1') && !is_dig((unsigned char)p[3])) { snprintf(ch, sizeof ch, "%c.%c", p[0], p[2]); break; }
  snprintf(out, cap, "%s%s%s", a, ch[0] ? " " : "", ch);
}
/* the file size in MB from "29.26 GB", "700 MB", "1,4 GiB" */
static double size_mb(const char *t) {
  for (const char *p = t; *p; p++) {
    if (!is_dig((unsigned char)*p)) continue;
    const char *q = p; while (is_dig((unsigned char)*q)) q++;
    if ((*q == '.' || *q == ',') && is_dig((unsigned char)q[1])) { q++; while (is_dig((unsigned char)*q)) q++; }
    const char *e = q; while (*e == ' ') e++;
    char u[4] = { 0 }; for (int i = 0; i < 3 && e[i]; i++) u[i] = (char)(e[i] >= 'A' && e[i] <= 'Z' ? e[i] + 32 : e[i]);
    int ul = !strncmp(u, "gb", 2) ? 2 : !strncmp(u, "gib", 3) ? 3 : !strncmp(u, "mb", 2) ? 2 : !strncmp(u, "mib", 3) ? 3 : 0;
    if (!ul || wch((unsigned char)e[ul])) continue;
    char num[48]; size_t n = (size_t)(q - p) < sizeof num - 1 ? (size_t)(q - p) : sizeof num - 1; memcpy(num, p, n); num[n] = 0; char *c = strchr(num, ','); if (c) *c = '.';
    double v = atof(num); return u[0] == 'g' ? v * 1024 : v;
  }
  return 0;
}
static int ci_eq(const char *p, const char *w) { for (; *w; p++, w++) { char c = *p >= 'A' && *p <= 'Z' ? *p + 32 : *p; if (c != *w) return 0; } return 1; }
static int num_at(const char *p) { long v = 0; while (is_dig((unsigned char)*p) && v < 100000000) v = v * 10 + (*p++ - '0'); return (int)v; }
/* the seeds a torrent stream announces ("👤 484", "Seeders: 12", "12 seeds"), -1 when it does not say */
static int seeds_of(const char *t) {
  for (const char *p = t; (p = strstr(p, "\xf0\x9f\x91\xa4")); p++) { const char *q = p + 4; while (*q == ' ') q++; if (is_dig((unsigned char)*q)) return num_at(q); }
  for (const char *p = t; *p; p++) {
    if (!ci_eq(p, "seed") || (p > t && wch((unsigned char)p[-1]))) continue;
    const char *q = p + 4; if (ci_eq(q, "er")) q += 2; if (*q == 's' || *q == 'S') q++; while (*q == ' ') q++; if (*q == ':' || *q == '=') q++; while (*q == ' ') q++;
    if (is_dig((unsigned char)*q)) return num_at(q);
  }
  for (const char *p = t; *p; p++) { if (!is_dig((unsigned char)*p)) continue; const char *q = p; while (is_dig((unsigned char)*q)) q++; while (*q == ' ') q++; if (ci_eq(q, "seed")) return num_at(p); }
  return -1;
}
static int res_n(const char *r) { return !strcmp(r, "4K") ? 2160 : !strcmp(r, "1440p") ? 1440 : !strcmp(r, "1080p") ? 1080 : !strcmp(r, "720p") ? 720 : !strcmp(r, "SD") ? 480 : 0; }
static double by_res(int r, double r2160, double r1440, double r1080, double r720, double r480) { return r == 2160 ? r2160 : r == 1440 ? r1440 : r == 1080 ? r1080 : r == 720 ? r720 : r480; }
/* Number#toFixed(1): half-way cases round up on the exact decimal value, as the web shows sizes */
static void to_fixed1(double v, char *out, size_t cap) {
  char b[64]; snprintf(b, sizeof b, "%.20f", v); char *d = strchr(b, '.'); if (!d) { snprintf(out, cap, "%s.0", b); return; }
  int up = d[2] >= '5'; d[2] = 0;
  if (up) { char *p = d + 1; for (;;) { if (p == d) p--; if (p < b) { memmove(b + 1, b, strlen(b) + 1); b[0] = '1'; break; } if (*p == '9') { *p = '0'; p--; continue; } (*p)++; break; } }
  snprintf(out, cap, "%s", b);
}

/* another track named beside a DTS / TrueHD one ("DTS-HD MA 5.1 + AC3", "TrueHD Atmos / DD 5.1"): the file has a
   sound this TV plays */
static int other_audio(const char *l) {
  return find_w(l, "ddp", 1, 0) || strstr(l, "dd+") || strstr(l, "eac3") || strstr(l, "e ac3") || strstr(l, "dolby digital") || has_word(l, "dd") || codec_ch(l, "dd", 1)
    || has_word(l, "ac3") || has_word(l, "aac") || codec_ch(l, "aac", 0) || has_word(l, "mp3") || has_word(l, "opus") || has_word(l, "flac");
}

/* ---- the score ------------------------------------------------------------------------------------ */
/* One stream for this TV, as one sum: the picture it gives here (capped at the panel), its source, audio and
   bitrate, minus a little for whatever needs the TV's fallback (Dolby Vision on a panel without it plays its HDR
   / SDR base, HDR is tone-mapped, a DTS / TrueHD track leans on the file's other tracks), plus how surely it
   plays (a debrid file is there, a torrent has its seeds). Only what can't play at all is left out: cams, debrid
   files not cached yet, torrents without peers. */
/* the languages a stream's text names (its audio, as addons write it: words, short tags, flags), against the user's
   audio and subtitle languages from the settings: a stream in the user's language first; one naming only others (a
   Russian dub for a French viewer) last, played only when nothing else can be; none named, the original: as it was */
static char prefAudio[8], prefSubs[8];
void best_set_langs(const char *audio, const char *subs) { snprintf(prefAudio, sizeof prefAudio, "%s", audio ? audio : ""); snprintf(prefSubs, sizeof prefSubs, "%s", subs ? subs : ""); }
static const struct { const char *code, *w[9], *flag, *flag2; } LANG[] = {
  { "eng", { "english", "eng", NULL }, "\xf0\x9f\x87\xac\xf0\x9f\x87\xa7", "\xf0\x9f\x87\xba\xf0\x9f\x87\xb8" },
  { "fre", { "french", "fre", "fra", "vf", "vff", "vfq", "vfi", "truefrench", "francais" }, "\xf0\x9f\x87\xab\xf0\x9f\x87\xb7", NULL },
  { "spa", { "spanish", "spa", "esp", "castellano", "latino", "espanol", NULL }, "\xf0\x9f\x87\xaa\xf0\x9f\x87\xb8", "\xf0\x9f\x87\xb2\xf0\x9f\x87\xbd" },
  { "ger", { "german", "ger", "deu", "deutsch", NULL }, "\xf0\x9f\x87\xa9\xf0\x9f\x87\xaa", NULL },
  { "ita", { "italian", "ita", "italiano", NULL }, "\xf0\x9f\x87\xae\xf0\x9f\x87\xb9", NULL },
  { "por", { "portuguese", "por", "dublado", "portugues", NULL }, "\xf0\x9f\x87\xb5\xf0\x9f\x87\xb9", "\xf0\x9f\x87\xa7\xf0\x9f\x87\xb7" },
  { "rus", { "russian", "rus", NULL }, "\xf0\x9f\x87\xb7\xf0\x9f\x87\xba", NULL },
  { "ukr", { "ukrainian", "ukr", NULL }, "\xf0\x9f\x87\xba\xf0\x9f\x87\xa6", NULL },
  { "pol", { "polish", "pol", "lektor", NULL }, "\xf0\x9f\x87\xb5\xf0\x9f\x87\xb1", NULL },
  { "tur", { "turkish", "tur", NULL }, "\xf0\x9f\x87\xb9\xf0\x9f\x87\xb7", NULL },
  { "ara", { "arabic", "ara", NULL }, "\xf0\x9f\x87\xb8\xf0\x9f\x87\xa6", NULL },
  { "dut", { "dutch", "nld", NULL }, "\xf0\x9f\x87\xb3\xf0\x9f\x87\xb1", NULL },
  { "hin", { "hindi", "hin", NULL }, "\xf0\x9f\x87\xae\xf0\x9f\x87\xb3", NULL },
  { "jpn", { "japanese", "jpn", NULL }, "\xf0\x9f\x87\xaf\xf0\x9f\x87\xb5", NULL },
  { "kor", { "korean", "kor", NULL }, "\xf0\x9f\x87\xb0\xf0\x9f\x87\xb7", NULL },
  { "chi", { "chinese", "chi", "mandarin", NULL }, "\xf0\x9f\x87\xa8\xf0\x9f\x87\xb3", NULL },
  { "swe", { "swedish", "swe", NULL }, "\xf0\x9f\x87\xb8\xf0\x9f\x87\xaa", NULL },
  { "heb", { "hebrew", "heb", NULL }, "\xf0\x9f\x87\xae\xf0\x9f\x87\xb1", NULL } };
#define NLANG (int)(sizeof LANG / sizeof LANG[0])
static unsigned langs_named(const char *raw, const char *l) { unsigned m = 0; for (int i = 0; i < NLANG; i++) { int f = 0;
    for (int k = 0; k < 9 && LANG[i].w[k] && !f; k++) if (has_word(l, LANG[i].w[k])) f = 1;
    if (!f && LANG[i].flag && strstr(raw, LANG[i].flag)) f = 1; if (!f && LANG[i].flag2 && strstr(raw, LANG[i].flag2)) f = 1;
    if (f) m |= 1u << i; } return m; }
static int lang_index(const char *code) { for (int i = 0; i < NLANG; i++) if (code && !strcmp(LANG[i].code, code)) return i; return -1; }
static int lang_score(const char *raw, const char *l) { int ia = lang_index(prefAudio), is = lang_index(prefSubs), ie = lang_index("eng"); if (ia < 0) ia = ie; /* (no audio language set: English) */
  unsigned m = langs_named(raw, l); int multi = has_word(l, "multi") || has_word(l, "dual") || strstr(l, "multi audio") != NULL, sc = 0;
  if (ia >= 0 && m) { if (m & (1u << ia)) sc += 60; else if (ie >= 0 && (m & (1u << ie))) sc += 0; else if (multi) sc -= 20; else sc -= 160; } /* the user's audio named: first; else English (as one with none named, the original); only others named: last */
  if (is >= 0 && is != ia) { const char *sw[] = { "vost", "vostfr", "subbed", "subs", "sub", NULL }; int subWord = 0; for (int k = 0; sw[k]; k++) if (has_word(l, sw[k])) subWord = 1; if (subWord && (m & (1u << is))) sc += 12; }
  return sc; }
static int rate(const Stream *s, const TvCaps *c, double mbps, double dur, BestRate *o, int *resOut) {
  char t[2048], l[2048]; best_stream_info(s, t, sizeof t); lower(t, l, sizeof l); blank(l, "._-[]()");
  int p2p = s->p2p;
  static const char *CAM[] = { "cam", "camrip", "hdcam", "ts", "hdts", "telesync", "tc", "telecine", "scr", "screener", "dvdscr" };
  for (size_t i = 0; i < sizeof CAM / sizeof CAM[0]; i++) if (has_word(l, CAM[i])) return 0;
  if (!p2p) { char nt[1100], raw[1100]; snprintf(raw, sizeof raw, "%s %s", s->name, s->title); lower(raw, nt, sizeof nt); /* debrid, not in the cloud yet */
    if (strstr(nt, "download]") || strstr(nt, "uncached") || strstr(nt, "needs to download") || strstr(nt, "\xe2\x8f\xb3")) return 0; }
  int sd = p2p ? seeds_of(t) : -1; if (p2p && sd >= 0 && sd < 8) return 0; /* a torrent needs a healthy swarm: a thin one stalls */
  const char *rl = best_res_of(t); int r = res_n(rl), rr = r ? r : 720; char hd[32], au[32], a0[32]; best_hdr_of(t, hd, sizeof hd); best_audio_of(t, au, sizeof au);
  snprintf(a0, sizeof a0, "%s", au); { char *sp = strchr(a0, ' '); if (sp) *sp = 0; }
  if (r > (c->maxRes ? c->maxRes : 1080)) return 0; /* above the panel's resolution: the TV fails to play it */
  double sc = by_res(rr, 400, 340, 300, 200, 80);
  if (!r) sc -= 40;
  sc += strstr(l, "remux") ? 30 : (strstr(l, "bluray") || strstr(l, "blu ray") || strstr(l, "bdrip") || strstr(l, "brrip")) ? 24 : (strstr(l, "webdl") || strstr(l, "web dl") || strstr(l, "webrip") || has_word(l, "web")) ? 20 : strstr(l, "hdtv") ? 4 : 0;
  if (hd[0]) sc += c->hdr ? 12 : -10;
  int dv = has_word(l, "dv") || has_word(l, "dovi") || has_word(l, "dolby vision"); if (dv) sc += c->dv ? 20 : (strstr(l, "hdr") ? -5 : -30); /* the TV's own Dolby Vision first; without it the HDR base plays */
  int dm = best_dts_mode(), dts = dm != 2 && strstr(au, "DTS") != NULL, thd = strstr(au, "TrueHD") != NULL, cdts = dm == 1 ? 0 : c->dts; /* DTS: Off, neither favoured nor avoided; Convert, it will be; Auto, what was found out */
  int mute = ((dts && !cdts && dm != 1) || (thd && !c->truehd)) && !other_audio(l); /* nothing else named: the sound is the one this TV can't decode (DTS set to Convert: it will be) */
  if ((dts && cdts) || (thd && c->truehd)) sc += 16; /* the TV decodes the special track: the best sound it can give */
  else if (dts || thd) sc -= 45;                       /* the file's other tracks have to stand in */
  else if (!strcmp(a0, "EAC3")) sc += 14; else if (!strcmp(a0, "AC3")) sc += 12; else if (!strcmp(a0, "AAC")) sc += 8; else if (!strcmp(a0, "Opus") || !strcmp(a0, "FLAC")) sc -= 15;
  if (strstr(au, "Atmos")) sc += 6;
  if (has_word(l, "av1") && !c->av1) sc -= 200; /* nothing to fall back to: the picture won't decode */
  sc += lang_score(t, l); /* the user's languages (settings) */
  double mb = size_mb(t), need = mb ? mb * 8 / (dur * 60) : by_res(rr, 22, 12, 8, 4, 2);
  if (mb) sc += fmin(18, need / by_res(rr, 20, 10, 6, 3, 1.5) * 8); /* more bitrate for the same picture: better */
  double line = mbps ? mbps : (p2p ? 40 : 100), cap = p2p ? fmin(line, sd > 0 ? 2 + sd * 1.5 : 4) : line; /* a swarm gives roughly 1.5 Mbps a seed */
  sc += p2p ? (sd < 0 ? -80 : fmin(45, 7 * log(sd + 1) / M_LN2) - 35) : 40; /* seeds unknown: its health can't be told */
  if (!o) return 1;
  o->s = s; o->sc = sc; o->need = need; o->cap = cap; o->mute = mute; if (resOut) *resOut = r;
  /* the summary: "4K  ·  DV HDR10  ·  EAC3 Atmos 5.1  ·  29.3 GB  ·  484 seeds" */
  char parts[5][48]; int np = 0;
  if (*rl) snprintf(parts[np++], 48, "%s", rl);
  if (hd[0]) { char h2[32]; size_t k = 0; for (const char *p = hd; *p && k + 1 < sizeof h2; ) { if (!strncmp(p, " \xc2\xb7 ", 4)) { h2[k++] = ' '; p += 4; } else h2[k++] = *p++; } h2[k] = 0; snprintf(parts[np++], 48, "%s", h2); }
  if (au[0]) snprintf(parts[np++], 48, "%s", au);
  if (mb) { if (mb >= 1024) { char f[32]; to_fixed1(mb / 1024, f, sizeof f); snprintf(parts[np++], 48, "%s GB", f); } else snprintf(parts[np++], 48, "%.0f MB", floor(mb + 0.5)); }
  if (p2p) { if (sd >= 0) { char n[16]; snprintf(n, sizeof n, "%d", sd); snprintf(parts[np++], 48, "%s", T1("{n} seeds", "n", n)); } } else snprintf(parts[np++], 48, "%s", T("Direct"));
  size_t k = 0; o->sum[0] = 0; for (int i = 0; i < np; i++) k += snprintf(o->sum + k, k < sizeof o->sum ? sizeof o->sum - k : 0, "%s%s", i ? "  \xc2\xb7  " : "", parts[i]);
  return 1;
}
static const TvCaps FALLBACK = { 1080, 0, 0, 0, 0, 0 };
/* c NULL: the kept caps, else a 1080p panel (a caller asking only for the summary line); mbps < 0: the measured speed */
int best_rate(const Stream *s, const TvCaps *c, double mbps, double durMin, BestRate *out) {
  if (!c) c = best_caps(); if (!c) c = &FALLBACK; if (mbps < 0) mbps = best_net_mbps();
  return rate(s, c, mbps, durMin, out, NULL); }
/* The TV's own resolution first (never above it), lower pictures only after it, and only among streams the line
   carries without buffering: a file's average bitrate (its size over the running time) plus 30% headroom must
   fit the speed measured when the setting was switched on. The first resolution with such a stream wins (its
   best one); when nothing fits anywhere, the lightest stream at the TV's resolution. */
const Stream *best_pick(const Stream *const *list, int n, double durMin, const TvCaps *c, double mbps, BestRate *out) {
  if (!c) { c = best_caps(); if (!c) c = &FALLBACK; }
  if (mbps < 0) mbps = best_net_mbps();
  if (durMin <= 0) durMin = 100;
  int mx = c->maxRes ? c->maxRes : 1080;
  BestRate *rs = n > 0 ? malloc(n * sizeof *rs) : NULL; int *tk = n > 0 ? malloc(n * sizeof *tk) : NULL, m = 0;
  for (int i = 0; i < n; i++) { int res = 0; if (!list[i] || !list[i]->url[0] || !rate(list[i], c, mbps, durMin, &rs[m], &res)) continue; tk[m++] = res == mx ? 0 : 1 + (mx - res); }
  const BestRate *best = NULL;
  /* tiers in order; inside one, the first best score (the web's stable sort) */
  for (int done = 0, cur = -1; !best && !done; ) {
    int next = -1; for (int i = 0; i < m; i++) if (tk[i] > cur && (next < 0 || tk[i] < next)) next = tk[i];
    if (next < 0) { done = 1; break; } cur = next;
    for (int i = 0; i < m; i++) if (tk[i] == cur && rs[i].need * 1.3 <= rs[i].cap && (!best || rs[i].mute < best->mute || (rs[i].mute == best->mute && rs[i].sc > best->sc))) best = &rs[i];
  }
  if (!best && m) { int top = tk[0]; for (int i = 1; i < m; i++) if (tk[i] < top) top = tk[i];
    for (int i = 0; i < m; i++) if (tk[i] == top && (!best || rs[i].mute < best->mute || (rs[i].mute == best->mute && (rs[i].need < best->need || (rs[i].need == best->need && rs[i].sc > best->sc))))) best = &rs[i]; }
  const Stream *s = best ? best->s : NULL; if (best && out) *out = *best;
  free(rs); free(tk); return s;
}

#ifdef SB_BEST_PURE
int best_dts_mode(void) { return 0; } /* the scoring test: Auto, as the web app scores */
#endif
#ifndef SB_BEST_PURE
int best_on(void) { return !strcmp(store_get("autoBest", "false"), "true"); }
int best_dur(const cJSON *meta, const char *type) {
  const cJSON *rt = meta ? cJSON_GetObjectItemCaseSensitive(meta, "runtime") : NULL; char b[64] = "";
  if (cJSON_IsString(rt) && rt->valuestring) snprintf(b, sizeof b, "%s", rt->valuestring); else if (cJSON_IsNumber(rt)) snprintf(b, sizeof b, "%g", rt->valuedouble);
  int d = sb_runtime_min(b); return d ? d : (type && !strcmp(type, "series") ? 45 : 110);
}

/* ---- worker results, handed to the main thread ------------------------------------------------------ */
static pthread_mutex_t mailMu = PTHREAD_MUTEX_INITIALIZER;
static struct { int capsReady, capsSure, dtsCfg, speedReady, speedOk; TvCaps caps; double mbps; } mail;

/* ---- what the TV can show and decode ---------------------------------------------------------------- */
static TvCaps caps; static int capsKnown = -1, capsAsking;
typedef struct { void (*cb)(const TvCaps *, void *); void *ud; } CapsWait;
static CapsWait capsWaits[8]; static int nCapsWaits;
const TvCaps *best_caps(void) {
  if (capsKnown < 0) { const cJSON *n = store_node("tvCaps2"); capsKnown = 0;
    if (cJSON_IsObject(n) && cJSON_GetObjectItemCaseSensitive(n, "maxRes")) { caps.maxRes = (int)js_num(n, "maxRes", 1080); caps.hdr = js_bool(n, "hdr", 0); caps.dv = js_bool(n, "dv", 0); caps.av1 = js_bool(n, "av1", 0); caps.truehd = js_bool(n, "truehd", 0); capsKnown = 1;
      caps.dts = best_learned("dts") == 1 || (best_learned("dts") < 0 && js_bool(n, "dtsCfg", 0)); } } /* the old "dts" (a guess from the webOS release) no longer counts */ /* the old "dts" (a guess from the webOS release) no longer counts */
  return capsKnown > 0 ? &caps : NULL;
}
/* the learned flags sit beside the asked ones in tvCaps2 ("dtsLearned", "hlsHevcOk", "dv81Hls"): kept when the caps are asked again */
static const char *learned_key(const char *what) { return !strcmp(what, "dts") ? "dtsLearned" : what; }
/* Settings > Audio > DTS sound: "auto" (playback finds out), "convert" (always converted), "off" (the TV plays it) */
int best_dts_mode(void) { const char *m = store_get("dtsMode", "auto"); return !strcmp(m, "convert") ? 1 : !strcmp(m, "off") ? 2 : 0; }
int best_learned(const char *what) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(store_node("tvCaps2"), learned_key(what));
  return cJSON_IsBool(v) ? cJSON_IsTrue(v) : -1;
}
void best_learn(const char *what, int yes) {
  if (best_learned(what) == !!yes) return;
  const cJSON *o = store_node("tvCaps2"); cJSON *n = cJSON_IsObject(o) ? cJSON_Duplicate(o, 1) : cJSON_CreateObject(); if (!n) return;
  cJSON_DeleteItemFromObjectCaseSensitive(n, learned_key(what)); cJSON_AddBoolToObject(n, learned_key(what), yes != 0); store_set_node("tvCaps2", n);
  if (!strcmp(what, "dts") && capsKnown > 0) caps.dts = yes != 0;
  fprintf(stderr, "[best] learned: %s %s\n", what, yes ? "yes" : "no");
}
/* a luna answer read the web's way: anything set and not false counts */
static int truthy(const cJSON *v) { return cJSON_IsTrue(v) || (cJSON_IsNumber(v) && v->valuedouble != 0) || (cJSON_IsString(v) && v->valuestring && *v->valuestring) || cJSON_IsObject(v) || cJSON_IsArray(v); }
static int webos_major(void) { const char *d = plat_device_name(), *p = d ? strstr(d, "webOS ") : NULL; return p ? atoi(p + 6) : 0; }
static void *caps_worker(void *arg) {
  (void)arg; TvCaps c = FALLBACK; int sure = 0, dtsCfg = -1, v = 0;
#ifdef SB_LINUX
  /* desktop: no TV pipeline to ask — mpv decodes everything itself, so the caps describe the display (maxRes from
     its height) plus what mpv plays: DTS/TrueHD natively, HDR/DV shown tone-mapped, AV1 through dav1d (in hardware
     when the Decoding setting's backend offers it). SB_TVCAPS below still overrides per field. */
  { int lines = plat_panel_lines(); c.maxRes = lines >= 2160 ? 2160 : 1080; sure = lines > 0;
    c.hdr = 1; c.dv = 1; c.av1 = 1; c.dts = 1; c.truehd = 1; }
#else
  char *r = plat_luna_call("luna://com.webos.service.tv.systemproperty/getSystemInfo", "{\"keys\":[\"UHD\"]}", 3000);
  cJSON *j = r ? cJSON_Parse(r) : NULL; free(r); const cJSON *u = cJSON_GetObjectItemCaseSensitive(j, "UHD");
  if (cJSON_IsTrue(u) || (cJSON_IsString(u) && !strcmp(u->valuestring, "true"))) c.maxRes = 2160;
  sure = u != NULL; cJSON_Delete(j);
  if (!sure) { int lines = plat_panel_lines(); if (lines >= 2160) c.maxRes = 2160; /* the service refused or kept quiet: the panel as SDL sees it */
    fprintf(stderr, "[best] no answer on the panel from the TV: %d lines from SDL; asked again next start\n", lines); }
  r = plat_luna_call("luna://com.webos.service.config/getConfigs", "{\"configNames\":[\"tv.model.supportHDR\",\"tv.model.supportDolbyVisionHDR\"]}", 3000);
  j = r ? cJSON_Parse(r) : NULL; free(r); const cJSON *k = cJSON_GetObjectItemCaseSensitive(j, "configs");
  c.hdr = truthy(cJSON_GetObjectItemCaseSensitive(k, "tv.model.supportHDR")); c.dv = truthy(cJSON_GetObjectItemCaseSensitive(k, "tv.model.supportDolbyVisionHDR")); cJSON_Delete(j);
  /* The codecs: the TV's media engine has no query for them (the web asked its browser's canPlayType):
     - DTS: LG decoded it up to the 2019 sets, dropped it in 2020-2022, brought it back on some 2023/24 models only
       and dropped it again in 2025, so the release says nothing. A config key naming DTS is taken where the
       firmware has one (dtsCfg); what playback shows (best_learn) wins over it, and the player tries the TV's own
       decoder while nothing is known.
     - AV1: decoded from the 2020 sets on (webOS 5+), the 4K panels at least; earlier sets have no AV1 at all.
     - TrueHD: these TVs only pass it through to an eARC receiver and are silent without one: never counted.
     A wrong "no" only costs a little score; a wrong "yes" would pick a stream that plays without sound or
     picture, so every doubt is a no. */
  v = webos_major(); dtsCfg = -1;
  c.dts = 0; c.av1 = v >= 6 || (v == 5 && c.maxRes == 2160); c.truehd = 0;
  r = plat_luna_call("luna://com.webos.service.config/getConfigs", "{\"configNames\":[\"tv.model.*\"]}", 3000);
  j = r ? cJSON_Parse(r) : NULL; free(r); k = cJSON_GetObjectItemCaseSensitive(j, "configs");
  if (cJSON_IsObject(k)) { const cJSON *e; cJSON_ArrayForEach(e, k) { if (!e->string || !cJSON_IsBool(e)) continue; /* tv.model.supportDTS and the like, on firmware that has one */
      char lk[96]; size_t q = 0; for (; e->string[q] && q + 1 < sizeof lk; q++) lk[q] = (char)(e->string[q] >= 'A' && e->string[q] <= 'Z' ? e->string[q] + 32 : e->string[q]); lk[q] = 0;
      if (strstr(lk, "dts") && !strstr(lk, "pass") && !strstr(lk, "spdif")) { dtsCfg = cJSON_IsTrue(e); fprintf(stderr, "[best] the TV's config: %s %s\n", e->string, dtsCfg ? "true" : "false"); } } }
  cJSON_Delete(j);
  c.dts = dtsCfg == 1; /* what playback learned is applied on the main thread (caps_landed) */
#endif
  const char *ov = getenv("SB_TVCAPS"); /* testing: SB_TVCAPS="maxRes=2160,hdr=1,dv=1,av1=0,dts=1,truehd=0" */
  if (ov) { int x; const char *p;
    if ((p = strstr(ov, "maxRes=")) && sscanf(p + 7, "%d", &x) == 1) c.maxRes = x; if ((p = strstr(ov, "hdr=")) && sscanf(p + 4, "%d", &x) == 1) c.hdr = x; if ((p = strstr(ov, "dv=")) && sscanf(p + 3, "%d", &x) == 1) c.dv = x;
    if ((p = strstr(ov, "av1=")) && sscanf(p + 4, "%d", &x) == 1) c.av1 = x; if ((p = strstr(ov, "dts=")) && sscanf(p + 4, "%d", &x) == 1) c.dts = x; if ((p = strstr(ov, "truehd=")) && sscanf(p + 7, "%d", &x) == 1) c.truehd = x; }
#ifdef SB_LINUX
  fprintf(stderr, "[best] PC: %dp, HDR %d, Dolby Vision %d, AV1 %d, DTS %d, TrueHD %d\n", c.maxRes, c.hdr, c.dv, c.av1, c.dts, c.truehd);
#else
  fprintf(stderr, "[best] TV: %dp, HDR %d, Dolby Vision %d, AV1 %d, DTS %d, TrueHD %d (webOS %d)\n", c.maxRes, c.hdr, c.dv, c.av1, c.dts, c.truehd, v);
#endif
  pthread_mutex_lock(&mailMu); mail.caps = c; mail.capsSure = sure; mail.dtsCfg = dtsCfg; mail.capsReady = 1; pthread_mutex_unlock(&mailMu);
  return NULL;
}
void best_caps_probe(void (*cb)(const TvCaps *c, void *ud), void *ud) {
  if (best_caps()) { if (cb) cb(&caps, ud); return; }
  if (cb && nCapsWaits < 8) { capsWaits[nCapsWaits].cb = cb; capsWaits[nCapsWaits].ud = ud; nCapsWaits++; }
  if (capsAsking) return;
  capsAsking = 1; pthread_t t; if (pthread_create(&t, NULL, caps_worker, NULL)) { capsAsking = 0; caps_worker(NULL); return; } pthread_detach(t);
}
static void caps_landed(const TvCaps *c, int sure, int dtsCfg) {
  caps = *c; capsKnown = 1; capsAsking = 0;
  if (best_learned("dts") >= 0) caps.dts = best_learned("dts"); /* learned while the worker asked */
  if (sure) { /* kept only when the TV answered: a guess serves this run only */
  cJSON *n = cJSON_CreateObject(); cJSON_AddNumberToObject(n, "maxRes", c->maxRes); cJSON_AddBoolToObject(n, "hdr", c->hdr); cJSON_AddBoolToObject(n, "dv", c->dv);
  cJSON_AddBoolToObject(n, "av1", c->av1); cJSON_AddBoolToObject(n, "truehd", c->truehd); if (dtsCfg >= 0) cJSON_AddBoolToObject(n, "dtsCfg", dtsCfg);
  static const char *const KEEP[] = { "dtsLearned", "hlsHevcOk", "dv81Hls" }; /* what playback taught stays */
  for (int i = 0; i < 3; i++) { int v = best_learned(KEEP[i]); if (v >= 0) cJSON_AddBoolToObject(n, KEEP[i], v); }
  store_set_node("tvCaps2", n); }
  int nw = nCapsWaits; CapsWait w[8]; memcpy(w, capsWaits, sizeof w); nCapsWaits = 0;
  for (int i = 0; i < nw; i++) w[i].cb(&caps, w[i].ud);
}

/* ---- the line: three downloads for 7 s, the first second left out (connections still opening) ---------- */
#define SPEED_URL "https://speed.cloudflare.com/__down?bytes=50000000"
#define WARM_MS 1000
#define END_MS 7000
static struct { volatile int stop; Uint32 t0; long long bytes; pthread_mutex_t mu; } SP = { 0, 0, 0, PTHREAD_MUTEX_INITIALIZER };
static int speedBusy; static double speedAt; static void (*speedCb)(int, double, void *); static void *speedUd; static volatile int speedDropped;
static int speed_sink(const unsigned char *p, size_t n, void *ud) {
  (void)p; (void)ud; Uint32 el = SDL_GetTicks() - SP.t0;
  if (SP.stop || el >= END_MS) return 0;
  if (n && el >= WARM_MS) { pthread_mutex_lock(&SP.mu); SP.bytes += (long long)n; pthread_mutex_unlock(&SP.mu); }
  return 1;
}
static void *speed_one(void *arg) {
  (void)arg;
  for (;;) { Uint32 el = SDL_GetTicks() - SP.t0; if (SP.stop || el >= END_MS) break;
    int st = net_stream_sync(SPEED_URL, (int)(END_MS - el) + 300, speed_sink, NULL);
    el = SDL_GetTicks() - SP.t0; if (st <= 0 || SP.stop || el >= END_MS - 500) break; } /* a download that finished early: another one */
  return NULL;
}
static void *speed_boss(void *arg) {
  (void)arg; pthread_t w[3]; int nw = 0;
  for (int i = 0; i < 3; i++) if (!pthread_create(&w[nw], NULL, speed_one, NULL)) nw++;
  while (!SP.stop && SDL_GetTicks() - SP.t0 < END_MS) SDL_Delay(20);
  Uint32 now = SDL_GetTicks(); SP.stop = 1;
  for (int i = 0; i < nw; i++) pthread_join(w[i], NULL);
  double secs = fmax(0.5, ((double)((now - SP.t0) < END_MS ? (now - SP.t0) : END_MS) - WARM_MS) / 1000), mbps = floor(SP.bytes * 8 / secs / 1e5 + 0.5) / 10;
  fprintf(stderr, "[best] speed: %.1f Mbps (%lld bytes in %.1f s)%s\n", mbps, SP.bytes, secs, speedDropped ? ", cancelled" : "");
  pthread_mutex_lock(&mailMu); mail.speedOk = SP.bytes > 0 && mbps > 0; mail.mbps = mbps; mail.speedReady = 1; pthread_mutex_unlock(&mailMu);
  return NULL;
}
double best_speed_live(void) { /* the speed so far, once the first second has passed */
  if (!speedBusy) return 0; Uint32 el = SDL_GetTicks() - SP.t0; if (el < WARM_MS + 400) return 0; if (el > END_MS) el = END_MS;
  pthread_mutex_lock(&SP.mu); long long by = SP.bytes; pthread_mutex_unlock(&SP.mu); return by * 8.0 / ((el - WARM_MS) / 1000.0) / 1e6; }
double best_net_mbps(void) { return atof(store_get("netMbps", "0")); }
int best_speed_busy(void) { return speedBusy; }
double best_speed_since(void) { return speedBusy ? app_now() - speedAt : -1; }
void best_speed_run(void (*cb)(int, double, void *), void *ud) {
  if (speedBusy) return;
  speedBusy = 1; speedAt = app_now(); speedCb = cb; speedUd = ud; speedDropped = 0;
  SP.stop = 0; SP.bytes = 0; SP.t0 = SDL_GetTicks();
  pthread_t t; if (pthread_create(&t, NULL, speed_boss, NULL)) { speedBusy = 0; if (cb) cb(0, 0, ud); return; } pthread_detach(t);
}
void best_speed_cancel(void) { if (!speedBusy) return; speedDropped = 1; SP.stop = 1; }
static void speed_landed(int ok, double mbps) {
  speedBusy = 0; if (speedDropped) return;
  if (ok) { char b[32]; snprintf(b, sizeof b, "%.1f", mbps); size_t n = strlen(b); if (n > 2 && !strcmp(b + n - 2, ".0")) b[n - 2] = 0; store_set("netMbps", b);
    snprintf(b, sizeof b, "%.0f", (double)time(NULL) * 1000); store_set("netAt", b); }
  if (speedCb) speedCb(ok, mbps, speedUd);
}

/* ---- choosing for Play ------------------------------------------------------------------------------- */
/* The player's title art comes up at once and stays while the addons answer. All answered: chosen at once; some
   still answering: up to 12 s for them (most answer in 1-3 s, debrid aggregators give their own sources in
   7-15 s), then the best of what came, or, with nothing usable yet, up to 30 s. One quiet line says what it
   waits for. */
static unsigned long long skipped[24]; static int nSkipped;
static unsigned long long url_hash(const char *u) { unsigned long long h = 1469598103934665603ull; for (; *u; u++) { h ^= (unsigned char)*u; h *= 1099511628211ull; } return h; }
void best_skip(const char *url) { if (url && *url) skipped[nSkipped++ % 24] = url_hash(url); }
static int was_skipped(const Stream *s) { unsigned long long h = url_hash(s->url); for (int i = 0; i < nSkipped && i < 24; i++) if (skipped[i] == h) return 1; return 0; }
static struct { unsigned tok; int on, done, fin, wait, said3, t12, t30, dur; double t0; char type[16], vid[80]; Stream *all; int n, cap; BestStatusCb status; BestDoneCb cb; void *ud; } F;
static void flow_end(void) { F.on = 0; free(F.all); F.all = NULL; F.n = F.cap = 0; }
static void say(void) {
  if (!F.on || F.fin || !F.status) return; int slow = F.wait > 0 && app_now() - F.t0 > 2.8; char t[256], n[16]; /* only when it takes a while: some addons are slow */
  size_t k = snprintf(t, sizeof t, "%s", T("Choosing the best stream"));
  if (F.n) { snprintf(n, sizeof n, "%d", F.n); k += snprintf(t + k, sizeof t - k, "  \xc2\xb7  %s", T1("{n} found", "n", n)); }
  if (slow && k < sizeof t) { snprintf(n, sizeof n, "%d", F.wait); snprintf(t + k, sizeof t - k, "  \xc2\xb7  %s", F.wait == 1 ? T("waiting for 1 addon") : T1("waiting for {n} addons", "n", n)); }
  F.status(t, F.ud);
}
static void decide(int force) {
  if (!F.on || F.fin) return;
  double el = app_now() - F.t0;
  if (!F.done && !force && el < 12) return;
  const Stream **l = F.n ? malloc(F.n * sizeof *l) : NULL; int nl = 0; for (int i = 0; i < F.n; i++) if (!was_skipped(&F.all[i])) l[nl++] = &F.all[i]; /* never one that just failed */
  BestRate r; const Stream *b = best_pick(l, nl, F.dur, NULL, -1, &r); free(l);
  if (!b) { if (!F.done && el < 30) return; F.fin = 1; BestDoneCb cb = F.cb; void *ud = F.ud; fprintf(stderr, "[best] nothing suits this TV (%d streams)\n", F.n); flow_end(); if (cb) cb(NULL, NULL, ud); return; }
  F.fin = 1; char info[2048]; best_stream_info(b, info, sizeof info); if (strlen(info) > 160) info[160] = 0; fprintf(stderr, "[best] %s | %s\n", r.sum, info);
  if (F.status) F.status(r.sum, F.ud);
  Stream pick = *b; r.s = &pick; BestDoneCb cb = F.cb; void *ud = F.ud; flow_end(); if (cb) cb(&pick, &r, ud);
}
static void on_part(StreamGroup *g, int gi, void *ud) {
  (void)gi;
  if (F.on && !F.fin && (unsigned)(uintptr_t)ud == F.tok) {
    if (F.n + g->n > F.cap) { F.cap = (F.n + g->n) * 2; F.all = realloc(F.all, F.cap * sizeof *F.all); }
    memcpy(F.all + F.n, g->items, g->n * sizeof *g->items); F.n += g->n; say(); decide(0);
  }
  free(g->items); /* the part's streams belong to whoever receives them */
}
static void on_ans(int pending, int count, void *ud) { (void)count; if ((unsigned)(uintptr_t)ud != F.tok) return; F.wait = pending; say(); }
static void on_done(int total, void *ud) { (void)total; if ((unsigned)(uintptr_t)ud != F.tok || !F.on) return; F.done = 1; decide(1); }
static void on_caps(const TvCaps *c, void *ud) { (void)c; if (!F.on || (unsigned)(uintptr_t)ud != F.tok) return; sb_streams_ex(F.type, F.vid, on_part, on_done, on_ans, ud); }
unsigned best_choose(const char *type, const char *vid, int durMin, BestStatusCb status, BestDoneCb done, void *ud) {
  static unsigned tokN; flow_end(); memset(&F, 0, sizeof F);
  { const char *au = store_get("defaultAudioTrack", ""), *su = store_get("subsLang", ""); best_set_langs(au, su); } /* the user's audio and subtitle languages, for this choice */
  F.tok = ++tokN; F.on = 1; F.wait = -1; F.t0 = app_now(); F.dur = durMin > 0 ? durMin : 100; F.status = status; F.cb = done; F.ud = ud;
  snprintf(F.type, sizeof F.type, "%s", type); snprintf(F.vid, sizeof F.vid, "%s", vid);
  unsigned tok = F.tok; say();
  best_caps_probe(on_caps, (void *)(uintptr_t)tok);
  return tok;
}
void best_choose_cancel(unsigned tok) { if (F.on && F.tok == tok) flow_end(); }

void best_tick(void) {
  static unsigned frame; if (frame == app_frame() && frame) return; frame = app_frame(); /* safe to call from more than one place */
  int cr = 0, csure = 0, ccfg = -1, sr = 0, sok = 0; TvCaps c; double mb = 0;
  pthread_mutex_lock(&mailMu); if (mail.capsReady) { cr = 1; c = mail.caps; csure = mail.capsSure; ccfg = mail.dtsCfg; mail.capsReady = 0; } if (mail.speedReady) { sr = 1; sok = mail.speedOk; mb = mail.mbps; mail.speedReady = 0; } pthread_mutex_unlock(&mailMu);
  if (cr) caps_landed(&c, csure, ccfg);
  if (sr) speed_landed(sok, mb);
  if (F.on) { double el = app_now() - F.t0;
    if (!F.said3 && el >= 3) { F.said3 = 1; say(); }
    if (F.on && !F.t12 && el >= 12.1) { F.t12 = 1; decide(0); }
    if (F.on && !F.t30 && el >= 30.1) { F.t30 = 1; decide(1); } }
}
#endif
