/* Torrents, inside the app: the engine library is loaded and started the first time a torrent is
   played, keeps a small window of the film where the TV has room (flash, else RAM), serves the file on 127.0.0.1 for the player, and stops
   five minutes after the last film. Everything is called from the main thread. */
#ifndef SB_P2P_H
#define SB_P2P_H
#include <stdint.h>
typedef void (*P2pCb)(void *ud, const char *url, const char *err, uint64_t fileSize); /* url NULL on failure (err is for the user) */
int  p2p_available(void);                       /* the engine library is there */
void p2p_resolve(const char *p2pUrl, P2pCb cb, void *ud); /* p2p://<infohash>/<fileIdx>?tr=... ; one at a time (a new one replaces it) */
void p2p_cancel(void);                          /* Back while it resolves */
const char *p2p_status(void);                   /* the line for the player's art while resolving ("" while it is quick) */
const char *p2p_wait_reason(int waiting);       /* while the player waits (1): "" until it is slow (~7 s at the start, 3 s mid-film), then one short reason; 0 resets */
int  p2p_peer_line(char *out, int cap);         /* "12 peers · 2.4 MB/s" (" · USB" when the buffer is on the USB drive) while a torrent is active; 0 when there is none */
void p2p_pace(int playing, int buffering, double durationSec); /* the player, about once a second */
void p2p_player_closed(void);                   /* torrents and their data go; the engine stays warm a while (a kept local cache stays on disk) */
void p2p_note_direct_play(void);              /* a direct (non-torrent) title starts: a kept local cache is evicted */
int  p2p_moved_recently(void);                  /* the window changed place in the last 30 s (a USB drive went): a stream error then is worth one retry */
void p2p_pump(void);
void p2p_shutdown(void);                        /* at exit: the engine stops (a few seconds at most) and its window files go */                            /* once per frame */
#endif
