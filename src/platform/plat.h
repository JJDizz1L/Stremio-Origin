/* What differs per device, behind one small interface. The core never includes a platform
   header: webOS, the Mac preview, and later Android TV and Tizen each implement this file. */
#ifndef SB_PLAT_H
#define SB_PLAT_H
#include <SDL2/SDL.h>

/* before SDL_Init: environment, hints */
void plat_pre_init(const char *appId);
/* after the window exists */
void plat_post_window(SDL_Window *w);
/* where the bundled assets are (fonts, i18n) and where to keep data (settings, caches) */
const char *plat_asset_dir(void);
const char *plat_data_dir(void);
/* the window size to ask for; fullscreen on TVs */
void plat_window_size(int *w, int *h, Uint32 *flags);
/* maps an SDL key event to the app's key codes (sb_keys.h); 0 = not a key the app uses */
int plat_map_key(const SDL_KeyboardEvent *e);
/* 1 when the platform types text for us (a keyboard on the Mac); 0 = the app shows its own keyboard */
int plat_text_input(void);
/* the device's name for logs ("LG webOS 4.4.2 OLED55C9") */
const char *plat_device_name(void);
/* the device's RAM in MB (0 when unknown): picks the picture budget and the drawing size */
int plat_mem_total_mb(void);
void plat_thread_background(int nice);  /* the calling thread yields to the drawing thread (and threads it starts inherit it) */
/* an SDL event as the app's life sees it: 1 = back in front (a relaunch, a return from another app),
   -1 = sent to the back, 0 = neither */
int plat_lifecycle(const SDL_Event *e);
/* the TV's own UI language ("fr-FR"), asked once; NULL when the platform does not say */
const char *plat_ui_locale(void);
/* a one-reply call to a system service, waited for at most timeoutMs; the reply's payload (malloc'd) or NULL.
   webOS: luna ("luna://com.webos.settingsservice/getSystemSettings"); elsewhere always NULL */
char *plat_luna_call(const char *uri, const char *payload, int timeoutMs);
/* a call that answers more than once ({"subscribe":true}): each reply's payload reaches cb on the main thread,
   from plat_luna_pump() (call it every frame while one runs). A handle, or 0 when the call could not be made
   (always 0 off webOS) */
typedef void (*PlatLunaCb)(const char *payload, void *ud);
unsigned long plat_luna_subscribe(const char *uri, const char *payload, PlatLunaCb cb, void *ud);
void plat_luna_cancel(unsigned long handle);
void plat_luna_pump(void);
/* a call whose answer nobody waits for (a refusal is logged); 0 when it could not be sent (always 0 off webOS) */
int plat_luna_send(const char *uri, const char *payload);
/* the panel's lines (2160 on a 4K TV) from the platform itself, without a system service; 0 when unknown */
int plat_panel_lines(void);
/* desktop fullscreen: 1 while the window fills the screen (toggle back restores the tiled/floating window) */
int plat_fullscreen(void);
void plat_set_fullscreen(int on);
#endif
