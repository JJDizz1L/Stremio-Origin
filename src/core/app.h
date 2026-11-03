/* The app: which screen is up, how screens change, the keys, the shared top bar, the backdrop,
   the accent colour, toasts. Screens are modules that register an enter/leave/key/tick/draw set. */
#ifndef SB_APP_H
#define SB_APP_H
#include <stdint.h>
#include "core/anim.h"
#include "core/stremio.h"

typedef enum { SCR_LOGIN = 0, SCR_HOME, SCR_DISC, SCR_LIB, SCR_DETAIL, SCR_SIM, SCR_SEARCH, SCR_SET, SCR_N } Scr;
typedef struct {
  void (*enter)(const void *params);
  void (*leave)(void);
  int  (*key)(int k);                 /* 1 = handled (for Back: the screen stepped back inside itself) */
  void (*tick)(float dt);
  void (*draw)(void);
  const Item *(*focused)(void);       /* the card under the highlight (hold OK opens its menu), or NULL */
  void (*open)(const Item *it);       /* a short OK on that card (NULL = the default: resume or the title page) */
  void (*text)(const char *utf8);     /* typed text (the sign-in fields) */
  int  (*origin)(float *x, float *y); /* the centre of the focused element: the next screen grows out of it, Back shrinks toward it */
  void (*out)(float k);              /* Back: the page drawn once more over the one it returns to, its entrance played backwards (k 0..1) */
} Screen;

/* parameters for the title page */
typedef struct { Item it; int openStreams; } DetailParams;

extern Scr SCR;
extern Screen *screens[SCR_N];
extern Screen *app_overlay;
void app_overlay_keep(Screen *s);     /* an overlay that draws something of its own while closed (an action's chip): drawn and ticked from now on */           /* a sheet or menu over the screen (streams, the card menu): keys go there first */
void app_init(void);
void app_register(Scr s, Screen *sc);
void app_go(Scr s, const void *params, int noStack);
void app_back(void);
void app_key(int k, int down, int repeat);
void app_text(const char *utf8);
void app_tick(float dt);
void app_draw(void);
void app_draw_under(void);            /* the page under the sheets (for their glass): backdrop, screen, top bar */
float app_screen_alpha(void);         /* the entrance fade of the current screen (0..1) */
int  app_is_board(Scr s);
void app_open_item(const Item *it);   /* resume where it was left, or the title page */
extern int (*app_resume_hook)(const Item *it);   /* the player: 1 when it took the item */
extern void (*app_card_menu_hook)(const Item *it); /* the card menu */
extern void (*app_play_hook)(const Stream *s, const char *type, const char *id, const char *vid, int fromStart); /* the player */
void app_play(const Stream *s, const char *type, const char *id, const char *vid, int fromStart);
void app_play_from_start(const Item *it);
void app_open_external(const char *url, const char *name);  /* an addon's outside link: a stremio:///detail page here, else a code for the phone */
void app_refresh_boards(void);        /* the library changed: the boards rebuild */
extern int app_splash_done;
void app_reveal_start(void);                        /* first launch: the page rises in once the splash goes */
float app_reveal_x(float delay, float dur);         /* 0..1 of that entrance for a block with this delay (1 when none runs) */
int  app_revealing(void);

/* frame */
float app_dt(void);
double app_now(void);                 /* seconds, monotonic */
unsigned app_frame(void);
void app_touch(void);                 /* a key arrived: idle timers restart */
unsigned app_bg_gen(void);
int app_bg_pending(void);   /* a new backdrop is asked for and on its way */
void app_glow_kick(void);              /* the phone sent words: Search's glows surge and spring back */             /* bumps each time a new backdrop takes over */
float app_idle(void);
float app_trailer_idle(void);           /* seconds since a key other than OK (the background trailer's own idleness) */                 /* seconds since the last key */
int  app_rtl(void);
void app_occluder(float x, float y, float w, float h); /* an opaque panel holding still: the backdrop is not drawn under it (next frame) */
void app_occluder_drop(void);  /* it moves now: the backdrop under it from this frame on (no hole for the frame it takes) */
void app_rtl_hold(int on);  /* app_rtl() answers 0 while on: a page drawn left to right for a mirrored picture */
int  app_mirror_key(int k);           /* Left<->Right in RTL */

/* the edge: a push that way and back, over the element's own place */
typedef struct { Timer1 t; float dx, dy; } Nudge;
void  nudge_start(Nudge *n, int k);
void  nudge_tick(Nudge *n, float dt);
float nudge_x(const Nudge *n);
float nudge_y(const Nudge *n);

/* hold OK: 0..1 of the way to the card menu (the line that runs around the card) */
float app_hold_progress(void);
int  app_key_held(void);               /* the key being handled repeats a held one (screens glide instead of popping) */
int  app_ok_down(void);                /* OK is held down now (a sheet's own hold reads it) */
void  app_clock_draw(float right, float y, float alpha); /* the pages' clock, its right edge at x (the player's too) */
void  app_clock_top(void); /* the bar's clock over the start screen and the guide (main, last) */
#ifdef SB_LINUX
void app_fsbtn_geom(float right, float y, float *x, float *yy); /* the button's 76x76 circle beside the clock pill */
int  app_fsbtn_hit(float right, float y, float x, float yy);    /* 1 when the pointer is on it */
void app_fsbtn_draw(float right, float y);                   /* the button beside the clock, drawn with it */
void app_toggle_fullscreen(void);                            /* fullscreen, or back to tiled/floating */
#endif
float app_hold_k(double since);        /* 0..1 of a hold begun then, on the same clock as the card menu's line */

/* ---- the top bar ---- */
#define NAV_SET 0       /* the bar reads Settings, Search, the three tabs (as the page) */
#define NAV_SEARCH 1
#define NAV_DISC 2
#define NAV_HOME 3
#define NAV_LIB 4
typedef struct { int idx, active; int want; double swAt; Nudge nudge; Tween px, pw, pill; float pillShown; Tween cornerF[2], cornerS[2], dotK[2]; Tween setRot; float chipX[5], chipW[5]; int laidOut; } TopNav;
extern TopNav topNav;
void topnav_focus(void);
void topnav_draw(void);
void topnav_tick(float dt);
void topnav_switch(int idx);

/* ---- the backdrop ---- */
void bg_set(const char *url, int delayMs, const char *alt);   /* crossfades to the picture; alt = the poster when there is no backdrop */
void bg_clear(void);
void bg_draw(void);
void bg_tick(float dt);
void bg_set_mode(int detail);                                  /* the shading: a title page shows more of the cover */
const char *bg_current(void);
void app_ambient_guide(int mode);                             /* the guide takes Settings' two glows for its page: 1 up, 2 fading out, 0 gone */
unsigned int app_ambient_glass_tex(void); /* those glows, small, for glass over them (upside down, as targets are), 0 none */
void app_ambient_draw(void);                                   /* ...and draws them over its base, once a frame */
unsigned bg_glass_tex(float *alpha);                          /* the shown backdrop as drawn (upside down), 0 when none: the glass of the sheets */

/* ---- accent ---- */
extern uint32_t ACC, ACC2, ACC3, ACC_D, ACC_FG;
extern const char *ACCENT_KEYS[18];
void accent_apply(void);               /* from the "accent" setting */
void accent_scope(const char *titleId, const char *poster, const char *logo); /* dynamic modes: this title's colour (NULL = the default) */
void accent_tint_fetch(const char *id, const char *poster, const char *logo);  /* ahead of time, for the focused card */
int  accent_dynamic(void);
int  accent_tint_of(const char *id, const char *poster, const char *logo, uint32_t out[4]); /* a title's own accent: 1 ready, 0 being worked out */
uint32_t acc_a(float a);               /* the accent with an alpha */

/* ---- the Magic Remote pointer ---- */
void app_pointer_move(float x, float y);          /* layout units */
void app_pointer_button(int down, float x, float y);
void app_pointer_wheel(float dx, float dy);
void app_pointer_system(int show);                /* the TV showed / hid its cursor */

/* ---- toast / notices ---- */
void toast(const char *msg, int ms, const char *key);
void exit_ask(void);
void app_quit(void);
int  app_quitting(void);

/* the page colour */
#define PAGE RGB(12, 11, 17)
#endif
