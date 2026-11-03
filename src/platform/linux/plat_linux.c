/* Linux desktop (Arch / Omarchy): a window, keyboard keys standing in for the remote. */
#include "platform/plat.h"
#include "core/keys.h"
#include "core/applog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <unistd.h>

static char assetDir[1024], dataDir[1024];
static SDL_Window *win; /* for the fullscreen toggle (leaving it restores the tiled/floating window) */
static int wantFs; /* the state asked for: some compositors resize without ever reporting fullscreen back */
void plat_pre_init(const char *appId) { (void)appId;
  const char *a = getenv("SB_ASSETS");
  if (a) snprintf(assetDir, sizeof assetDir, "%s", a);
  else if (access("/usr/share/stremio-origin/assets", R_OK) == 0) snprintf(assetDir, sizeof assetDir, "%s", "/usr/share/stremio-origin/assets");
  else snprintf(assetDir, sizeof assetDir, "%s", "assets");
  const char *xdg = getenv("XDG_DATA_HOME"); const char *h = getenv("HOME");
  if (xdg && xdg[0]) snprintf(dataDir, sizeof dataDir, "%s/StremioOrigin", xdg);
  else if (h) snprintf(dataDir, sizeof dataDir, "%s/.local/share/StremioOrigin", h);
  else snprintf(dataDir, sizeof dataDir, "/tmp/StremioOrigin");
  char cmd[1200]; snprintf(cmd, sizeof cmd, "mkdir -p \"%s\"", dataDir); if (system(cmd)) {}
  applog_start(dataDir);
}
void plat_post_window(SDL_Window *w) { win = w; }
const char *plat_asset_dir(void) { return assetDir; }
const char *plat_data_dir(void) { return dataDir; }
void plat_window_size(int *w, int *h, Uint32 *flags) {
  const char *s = getenv("SB_WINDOW"); int ww = 1280, hh = 720;
  if (s && sscanf(s, "%dx%d", &ww, &hh) == 2 && ww > 100 && hh > 100) {}
  *w = ww; *h = hh; *flags = SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE;
}
int plat_map_key(const SDL_KeyboardEvent *e) {
  switch (e->keysym.sym) {
    case SDLK_LEFT: return K_LEFT; case SDLK_RIGHT: return K_RIGHT; case SDLK_UP: return K_UP; case SDLK_DOWN: return K_DOWN;
    case SDLK_RETURN: case SDLK_KP_ENTER: return K_OK;
    case SDLK_ESCAPE: case SDLK_BACKSPACE: return K_BACK;
    case SDLK_SPACE: return K_PLAYPAUSE;
    case SDLK_F1: return K_RED; case SDLK_F2: return K_GREEN; case SDLK_F3: return K_YELLOW; case SDLK_F4: return K_BLUE;
    case SDLK_COMMA: return K_REW; case SDLK_PERIOD: return K_FF;
    case SDLK_PAGEUP: return K_CH_UP; case SDLK_PAGEDOWN: return K_CH_DOWN;
    default: return K_NONE;
  }
}
const char *plat_device_name(void) { return "Linux"; }
int plat_text_input(void) { return getenv("SB_NO_TEXTINPUT") ? 0 : 1; }
void plat_thread_background(int nice) { (void)nice; }
int plat_mem_total_mb(void) { struct sysinfo si; if (sysinfo(&si)) return 0; return (int)((si.totalram * si.mem_unit) >> 20); }
int plat_lifecycle(const SDL_Event *e) {
  if (e->type == SDL_WINDOWEVENT) { if (e->window.event == SDL_WINDOWEVENT_RESTORED) return 1; if (e->window.event == SDL_WINDOWEVENT_MINIMIZED) return -1; }
  return 0;
}
const char *plat_ui_locale(void) { return NULL; }
char *plat_luna_call(const char *uri, const char *payload, int timeoutMs) { (void)uri; (void)payload; (void)timeoutMs; return NULL; }
unsigned long plat_luna_subscribe(const char *uri, const char *payload, PlatLunaCb cb, void *ud) { (void)uri; (void)payload; (void)cb; (void)ud; return 0; }
void plat_luna_cancel(unsigned long handle) { (void)handle; }
void plat_luna_pump(void) {}
int plat_luna_send(const char *uri, const char *payload) { (void)uri; (void)payload; return 0; }
int plat_panel_lines(void) { return 0; }
int plat_fullscreen(void) {
  if (wantFs) return 1;
  if (!win) return 0; Uint32 f = SDL_GetWindowFlags(win); return (f & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP)) != 0;
}
void plat_set_fullscreen(int on) { wantFs = !!on; if (!win) return; SDL_SetWindowFullscreen(win, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0); }