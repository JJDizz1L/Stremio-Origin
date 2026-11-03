/* LG webOS: the compositor needs the app's identity in the environment before SDL starts, the
   Wayland driver, and the hint that hands the Back key to the app (otherwise the system takes
   it). Remote keys arrive as scancodes defined by the TV's SDL. */
#include "platform/plat.h"
#include "core/keys.h"
#include "core/applog.h"
#include "cJSON.h"
#include <SDL2/SDL_webOS.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>

static char assetDir[1024], dataDir[1024], devName[160];
/* the folder takes a new file (a mount can be read-only, a folder another user's) */
static int writable_dir(const char *d) {
  char p[1100]; snprintf(p, sizeof p, "%s", d);
  for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; } mkdir(p, 0755);
  struct stat st; if (stat(d, &st) || !S_ISDIR(st.st_mode)) return 0;
  snprintf(p, sizeof p, "%s/.w%d", d, (int)getpid()); int fd = open(p, O_CREAT | O_WRONLY | O_TRUNC, 0600); if (fd < 0) return 0;
  close(fd); unlink(p); return 1;
}
void plat_pre_init(const char *appId) {
  { char lp[256]; snprintf(lp, sizeof lp, "/tmp/%s.log", appId); FILE *f = freopen(lp, "w", stderr); /* the app's log, readable over ssh */
    if (!f) { snprintf(lp, sizeof lp, "/tmp/%s.%d.log", appId, (int)getuid()); f = freopen(lp, "w", stderr); } if (f) fchmod(fileno(f), 0600); /* /tmp is shared by every app: the log is ours alone */ /* /tmp is shared and sticky: a log left by another user (a root run) cannot be taken over */
    if (f) setvbuf(stderr, NULL, _IOLBF, 0); }
  setenv("APPID", appId, 0); setenv("LS2_APPID", appId, 0);
  setenv("SDL_VIDEODRIVER", "wayland", 0);
  if (!getenv("XDG_RUNTIME_DIR")) setenv("XDG_RUNTIME_DIR", "/tmp/xdg", 1);
  SDL_SetHint(SDL_HINT_WEBOS_ACCESS_POLICY_KEYS_BACK, "true");
  SDL_SetHint(SDL_HINT_WEBOS_ACCESS_POLICY_KEYS_EXIT, "true");
  char *base = SDL_GetBasePath(); /* from /proc/self/exe: inside the jail, the jail's own path to the app */
  snprintf(assetDir, sizeof assetDir, "%sassets", base ? base : "./");
  /* the app's writable folder, kept across updates where the install allows it. The first that takes a file:
     - the one a rooted TV has always used (the app runs as root there): its data stays;
     - developer mode (ares-install, jailed as a plain user): LG's jail mounts /media/developer read-write and
       makes it the home, but the folders under apps/ belong to root;
     - the Homebrew Channel's jail mounts /media/internal read-write instead;
     - beside the binary (writable from webOS 4 on, emptied by every update);
     - /tmp, RAM: the settings and the sign-in last until the TV restarts */
  char cands[5][1024]; int nc = 0;
  snprintf(cands[nc++], 1024, "/media/developer/apps/usr/palm/data/%s", appId);
  snprintf(cands[nc++], 1024, "/media/developer/.%s", appId);
  snprintf(cands[nc++], 1024, "/media/internal/.%s", appId);
  if (base) snprintf(cands[nc++], 1024, "%sdata", base);
  snprintf(cands[nc++], 1024, "/tmp/%s", appId);
  if (base) SDL_free(base);
  dataDir[0] = 0; char skipped[600] = ""; size_t sk = 0;
  for (int i = 0; i < nc && !dataDir[0]; i++) {
    if (writable_dir(cands[i])) snprintf(dataDir, sizeof dataDir, "%s", cands[i]);
    else if (sk < sizeof skipped) sk += snprintf(skipped + sk, sizeof skipped - sk, " %s (%s)", cands[i], strerror(errno)); }
  if (!dataDir[0]) snprintf(dataDir, sizeof dataDir, "/tmp/%s", appId);
  fprintf(stderr, "[store] data in %s (uid %d)%s%s\n", dataDir, (int)getuid(), skipped[0] ? "; not usable:" : "", skipped);
  if (!strncmp(dataDir, "/tmp/", 5)) fprintf(stderr, "[store] warning: /tmp is RAM, the settings and the sign-in are lost when the TV restarts\n");
  if (geteuid() == 0) umask(0); /* started as root (a development run by hand): what it writes stays writable for the app as the TV runs it */
  applog_start(dataDir); /* /tmp/<appid>.log keeps every line as before; the kept log (for Share logs) gets them scrubbed */
  /* the device, for logs */
  FILE *f = fopen("/var/run/nyx/os_info.json", "r"); char buf[8192] = ""; if (f) { size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f); }
  const char *r = strstr(buf, "\"webos_release\""); char rel[32] = "?";
  if (r) { r = strchr(r + 15, '"'); if (r) { r++; size_t k = 0; while (*r && *r != '"' && k + 1 < sizeof rel) rel[k++] = *r++; rel[k] = 0; } }
  snprintf(devName, sizeof devName, "LG webOS %s", rel);
}
void plat_post_window(SDL_Window *w) { (void)w; }
const char *plat_asset_dir(void) { return assetDir; }
const char *plat_data_dir(void) { return dataDir; }
void plat_window_size(int *w, int *h, Uint32 *flags) { *w = 1920; *h = 1080; *flags = SDL_WINDOW_OPENGL; /* measured on a 43LK5900 (webOS 4.4): with SDL_WINDOW_FULLSCREEN the compositor hands back a 1280x720 surface; without it, 1920x1080 */
  const char *v = getenv("SB_WIN"); if (v) { int a, b, f; if (sscanf(v, "%dx%d:%d", &a, &b, &f) == 3) { *w = a; *h = b; *flags = SDL_WINDOW_OPENGL | (f & 1 ? SDL_WINDOW_FULLSCREEN : 0) | (f & 2 ? SDL_WINDOW_ALLOW_HIGHDPI : 0) | (f & 4 ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0); } } /* SB_WIN=3840x2160:2 to try other surface requests */ }
int plat_map_key(const SDL_KeyboardEvent *e) {
  switch (e->keysym.scancode) {
    case SDL_WEBOS_SCANCODE_BACK: return K_BACK;
    case SDL_WEBOS_SCANCODE_RED: return K_RED; case SDL_WEBOS_SCANCODE_GREEN: return K_GREEN;
    case SDL_WEBOS_SCANCODE_YELLOW: return K_YELLOW; case SDL_WEBOS_SCANCODE_BLUE: return K_BLUE;
    case SDL_WEBOS_SCANCODE_CH_UP: return K_CH_UP; case SDL_WEBOS_SCANCODE_CH_DOWN: return K_CH_DOWN;
    case SDL_WEBOS_SCANCODE_CURSOR_SHOW: return K_CURSOR_SHOW; case SDL_WEBOS_SCANCODE_CURSOR_HIDE: return K_CURSOR_HIDE;
    case SDL_WEBOS_SCANCODE_EXIT: return K_EXIT;
    /* media keys come through SDL's Linux table: Play (web 415, KEY_PLAY) and the single play/pause key (10252,
       KEY_PLAYPAUSE) both arrive as AUDIOPLAY and cannot be told apart, so it toggles (Play on a paused film plays,
       as 415 does); Pause (19, KEY_PAUSE) is PAUSE */
    case SDL_SCANCODE_AUDIOPLAY: return K_PLAYPAUSE;
    case SDL_SCANCODE_PAUSE: return K_PAUSE;
    case SDL_SCANCODE_AUDIOSTOP: case SDL_SCANCODE_STOP: return K_STOP;
    case SDL_SCANCODE_AUDIOREWIND: return K_REW;
    case SDL_SCANCODE_AUDIOFASTFORWARD: return K_FF;
    default: break;
  }
  switch (e->keysym.sym) {
    case SDLK_LEFT: return K_LEFT; case SDLK_RIGHT: return K_RIGHT; case SDLK_UP: return K_UP; case SDLK_DOWN: return K_DOWN;
    case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SELECT: return K_OK;
    case SDLK_AC_BACK: case SDLK_ESCAPE: return K_BACK;
    case SDLK_AUDIOPLAY: return K_PLAYPAUSE; case SDLK_AUDIOSTOP: return K_STOP;
    case SDLK_AUDIOREWIND: return K_REW; case SDLK_AUDIOFASTFORWARD: return K_FF;
    case SDLK_PAUSE: return K_PAUSE;
    default: break;
  }
  return K_NONE;
}
const char *plat_device_name(void) { return devName; }

int plat_text_input(void) { return 0; } /* the remote has no keyboard: the app draws one */

#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
void plat_thread_background(int nice) { setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), nice); } /* per thread on Linux */
int plat_mem_total_mb(void) {
  FILE *f = fopen("/proc/meminfo", "r"); if (!f) return 0;
  char line[128]; long kb = 0; while (fgets(line, sizeof line, f)) if (sscanf(line, "MemTotal: %ld kB", &kb) == 1) break;
  fclose(f); return (int)(kb / 1024);
}
/* the TV sends the app to the back for another app, the home launcher or the screen saver, and a relaunch
   (its icon picked while it runs) brings the same process back: the window loses and regains its focus,
   with the app-level events where SDL gives them. Only a return after a leave counts, so the first focus at
   start is not a resume */
int plat_lifecycle(const SDL_Event *e) {
  static int away;
  int in = 0, out = 0;
  if (e->type == SDL_APP_WILLENTERBACKGROUND || e->type == SDL_APP_DIDENTERBACKGROUND) out = 1;
  else if (e->type == SDL_APP_WILLENTERFOREGROUND || e->type == SDL_APP_DIDENTERFOREGROUND) in = 1;
  else if (e->type == SDL_WINDOWEVENT) switch (e->window.event) {
    case SDL_WINDOWEVENT_FOCUS_LOST: case SDL_WINDOWEVENT_HIDDEN: case SDL_WINDOWEVENT_MINIMIZED: out = 1; break;
    case SDL_WINDOWEVENT_FOCUS_GAINED: case SDL_WINDOWEVENT_SHOWN: case SDL_WINDOWEVENT_RESTORED: in = 1; break;
    default: break; }
  if (out) { if (!away) fprintf(stderr, "[life] to the back (%d/%d)\n", e->type, e->type == SDL_WINDOWEVENT ? e->window.event : 0); away = 1; return -1; }
  if (in && away) { away = 0; fprintf(stderr, "[life] back in front (%d/%d)\n", e->type, e->type == SDL_WINDOWEVENT ? e->window.event : 0); return 1; }
  return 0;
}
/* Settings > Language "Auto": the TV's UI language, from its settings service, as the system apps read it */
const char *plat_ui_locale(void) {
  static char loc[24]; static int asked;
  if (asked) return loc[0] ? loc : NULL; asked = 1;
  char *r = plat_luna_call("luna://com.webos.settingsservice/getSystemSettings", "{\"keys\":[\"localeInfo\"]}", 1500);
  cJSON *j = r ? cJSON_Parse(r) : NULL; free(r);
  cJSON *u = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(j, "settings"), "localeInfo"), "locales"), "UI");
  if (cJSON_IsString(u) && u->valuestring) snprintf(loc, sizeof loc, "%s", u->valuestring);
  cJSON_Delete(j);
  if (!loc[0]) { /* refused or silent: what the TV's SDL reports, when it has the call (SDL 2.0.14 on) */
    SDL_Locale *(*pref)(void) = NULL; *(void **)(&pref) = dlsym(RTLD_DEFAULT, "SDL_GetPreferredLocales");
    SDL_Locale *l = pref ? pref() : NULL;
    if (l && l[0].language && strcmp(l[0].language, "C") && strcmp(l[0].language, "POSIX")) snprintf(loc, sizeof loc, "%s%s%s", l[0].language, l[0].country ? "-" : "", l[0].country ? l[0].country : "");
    if (l) SDL_free(l);
    if (loc[0]) fprintf(stderr, "[i18n] TV language from SDL: %s\n", loc); }
  fprintf(stderr, "[i18n] TV language: %s\n", loc[0] ? loc : "(no answer)");
  return loc[0] ? loc : NULL;
}
/* the TV's SDL knows the panel without a system service (a refused systemproperty call leaves 4K sets at 1080p) */
int plat_panel_lines(void) {
  SDL_bool (*res)(int *, int *) = NULL; *(void **)(&res) = dlsym(RTLD_DEFAULT, "SDL_webOSGetPanelResolution");
  int w = 0, h = 0; if (!res || !res(&w, &h)) return 0;
  return h;
}

int plat_fullscreen(void) { return 0; }
void plat_set_fullscreen(int on) { (void)on; }
