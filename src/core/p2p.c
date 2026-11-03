#include "platform/plat.h"
#include "platform/mem.h"
#include "core/p2p.h"
#include "core/sbengine.h"
#include "core/store.h"
#include "core/i18n.h"
#include "core/usb.h"
#include <SDL2/SDL.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#ifdef __linux__
#include <sys/vfs.h>
#endif

/* the large public trackers that are still alive, on top of the addon's own */
static const char *PUBLIC_TRACKERS[] = {
  "udp://tracker.opentrackr.org:1337/announce", "udp://open.demonii.com:1337/announce", "udp://open.stealth.si:80/announce",
  "udp://tracker.torrent.eu.org:451/announce", "udp://exodus.desync.com:6969/announce", "udp://explodie.org:6969/announce",
  "udp://tracker.dler.org:6969/announce", "udp://opentracker.io:6969/announce", "udp://tracker.tiny-vps.com:6969/announce",
  "udp://tracker.theoks.net:6969/announce", "https://tracker.tamersunion.org:443/announce", "udp://tracker1.bt.moack.co.kr:80/announce" };

/* The buffer is a window of the film around what plays (the engine frees what is far behind), kept where the TV
   has room: a USB drive the user turned on for it, else the app's flash partition, else /tmp (RAM on a TV), else
   /dev/shm. Its size follows the film's bitrate and what is free (on a USB drive, the whole film), and shrinks while
   it plays when the room runs low. It never refuses to play for storage: on a TV with almost nothing free it
   streams from the smallest window the engine takes. When the USB drive goes (or is switched off) the window moves
   to the next place while the film plays: the engine downloads again there, the stream keeps its address. */
#define MB 1048576ull
#define W_FLOOR 24      /* MB: the engine's smallest window */
#define W_GOOD  64      /* MB: below this a location is "short" and the next one is tried */
#define START_SLOW 7.0  /* s before a slow start explains itself */
#define STALL_SLOW 3.0  /* s of buffering mid-film before it explains itself */
#define W_USB_MAX 8192  /* MB: a USB drive holds the whole film up to this */
#define SEG_MB 16       /* MB: segment files on drives that cannot free bytes inside a file (FAT32, exFAT) */

static const sbe_api *E; static int loadTried;
enum { ENG_OFF, ENG_STARTING, ENG_ON, ENG_FAILED, ENG_STOPPING };
static int engState; /* written by the start/stop threads: atomics below */
#define ENG() __atomic_load_n(&engState, __ATOMIC_ACQUIRE)
#define ENG_SET(v) __atomic_store_n(&engState, (v), __ATOMIC_RELEASE)
static int cleaning; /* the start-up sweep of left-overs is running */
static char engErr[128];
static char stateDir[1024], cacheDir[1024];
static double idleSince; /* seconds: the engine stops some minutes after the last torrent went */

/* what the TV has, read when the engine starts, and what was chosen from it */
enum { LOC_FLASH, LOC_RAM, LOC_DISK, LOC_USB, LOC_CACHE };
typedef struct {
  int memTotal, memFree, cpus, tier;           /* MB, MB, cores, 0 small 1 medium 2 large */
  int loc; char root[1024];                    /* where the window lives */
  long budget, reserve;                        /* MB the window may use there at start; MB kept free there */
  long window, target, wmax;                   /* MB: the window now, the one the bitrate asks for, the ceiling */
  int lowStorage;                              /* flash was short (memory used) or the window is below what the film wants */
  int flashShort;
  long seg; char usbMnt[512], usbFs[24];       /* MB per segment file (0: holes); the USB drive in use */
} Prof;
static Prof PF;
static const char *loc_name(int l) { return l == LOC_FLASH ? "flash" : l == LOC_RAM ? "memory" : l == LOC_USB ? "usb" : l == LOC_CACHE ? "cache" : "disk"; }
/* a move of the window to another place, worked out on a thread and applied by p2p_pump */
static int relocating; static volatile int relocReady; static Prof relocTo; static double relocAt, relocLook; static unsigned relocSerial;

static void rm_tree(const char *dir);
/* the kept local cache ("Cache torrents locally"): the last torrent's payload, held on disk after
   the player closes and reused when the same torrent plays again. Evicted when a different title
   plays (any torrent or direct stream), when the setting goes off, and on exit. */
static char keptIh[41], keptRoot[1024];
static long cache_mb(void) { long m = atol(store_get("p2pCacheMB", "0")); return m == 512 || m == 1024 || m == 2048 ? m : 0; } /* the setting's size, else off */
static void cache_root(char *o, size_t n) {
  const char *e = getenv("SB_P2P_CACHE"); if (e && e[0]) { snprintf(o, n, "%s/engine", e); return; }
  const char *xdg = getenv("XDG_CACHE_HOME"); char base[1024];
  if (xdg && xdg[0]) snprintf(base, sizeof base, "%s", xdg);
  else { const char *h = getenv("HOME"); if (h && h[0]) snprintf(base, sizeof base, "%s/.cache", h); else snprintf(base, sizeof base, "%s/cache", store_data_dir()); }
  snprintf(o, n, "%s/StremioOrigin/engine", base);
}
static void mkpath(char *dir) { /* mkdir -p for usable_dir's single-level parent step */
  for (char *p = dir + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(dir, 0755); *p = '/'; }
  mkdir(dir, 0755);
}
static void evict_kept(void) { /* the kept cache goes now (a different title plays, the setting went off) */
  if (!keptRoot[0]) { keptIh[0] = 0; return; }
  rm_tree(keptRoot); rmdir(keptRoot); keptRoot[0] = 0; keptIh[0] = 0;
  fprintf(stderr, "[p2p] local cache evicted\n");
}
static void kept_invalidate(const char *ih) { /* that torrent's payload was deleted by the engine: no cache of it left */
  if (ih && ih[0] && !strcmp(keptIh, ih)) { keptRoot[0] = 0; keptIh[0] = 0; }
}
/* the one torrent being resolved / played */
static struct { int on; char ih[41], magnet[4096]; int want; uint32_t idx; uint64_t size; double t0; P2pCb cb; void *ud; char status[256]; int stage; } R;
/* the active one, for stats, pacing and the reason line */
static struct { char ih[41]; uint64_t size, lastBytes; double lastT, rate, dur, bps; int calm, gentle, playedOnce; double startT0, stallT0, lastWatch, lastReason, reasonLogT, lastReport; int reasonKind; char reason[200]; } A;

static double now_s(void) { return SDL_GetTicks() / 1000.0; }
static double idle_limit(void) { const char *e = getenv("SB_P2PIDLE"); return e ? atof(e) : 300; } /* SB_P2PIDLE=5: development, stop cycles */
static void lib_path(char *p, size_t cap) { char *base = SDL_GetBasePath();
#ifdef __APPLE__
  snprintf(p, cap, "%slibsbengine.dylib", base ? base : "");
#else
  snprintf(p, cap, "%slibsbengine.so", base ? base : "");
#endif
  SDL_free(base); }
static int load(void) {
  if (E) return 1; if (loadTried) return 0; loadTried = 1;
  char p[1200]; lib_path(p, sizeof p);
  void *h = dlopen(p, RTLD_NOW | RTLD_LOCAL);
  if (!h) { fprintf(stderr, "[p2p] no engine library: %s\n", dlerror()); return 0; }
  const sbe_api *(*get)(void) = (const sbe_api *(*)(void))dlsym(h, "sbe_get");
  if (!get || !get() || get()->api() != SBE_API_VERSION) { fprintf(stderr, "[p2p] engine library does not match\n"); return 0; }
  E = get(); fprintf(stderr, "[p2p] engine %s\n", E->version());
  return 1;
}
int p2p_available(void) { if (E) return 1; char p[1200]; lib_path(p, sizeof p); return access(p, R_OK) == 0; }

static void rm_tree(const char *dir) {
  DIR *d = opendir(dir); if (!d) return; struct dirent *e; char p[1400];
  while ((e = readdir(d))) { if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
    struct stat st; if (lstat(p, &st)) continue; if (S_ISDIR(st.st_mode)) { rm_tree(p); rmdir(p); } else unlink(p); }
  closedir(d);
}
static long free_mb(const char *dir) { struct statvfs v; if (statvfs(dir, &v)) return -1; return (long)((unsigned long long)v.f_bavail * v.f_frsize / MB); }

/* the candidate folders (SB_P2P_FLASH / SB_P2P_TMP point them elsewhere for tests: a tiny disk image, a full folder) */
static void flash_root(char *o, size_t n) { const char *e = getenv("SB_P2P_FLASH"); snprintf(o, n, "%s/engine", e ? e : store_data_dir()); }
#ifndef SB_APP_ID
#define SB_APP_ID "tv.stremioorigin.app"
#endif
static void usb_root(char *o, size_t n, const char *mnt) { snprintf(o, n, "%s/%s-engine", mnt, SB_APP_ID); }
/* /tmp and /dev/shm are shared by every app on the TV: a folder per app, and per user when one of that name belongs
   to someone else (a second install, a left-over of a run as root) */
static void tmp_root(char *o, size_t n, int shm) {
  const char *e = getenv("SB_P2P_TMP"), *base = e && !shm ? e : shm ? "/dev/shm" : "/tmp";
  snprintf(o, n, "%s/%s-engine", base, SB_APP_ID);
  struct stat st; if (!lstat(o, &st) && (!S_ISDIR(st.st_mode) || st.st_uid != geteuid())) snprintf(o, n, "%s/%s-engine-%d", base, SB_APP_ID, (int)geteuid());
}

static int is_tmpfs(const char *dir) {
#ifdef __linux__
  struct statfs f; return !statfs(dir, &f) && (unsigned long)f.f_type == 0x01021994UL;
#else
  (void)dir; return 1; /* the Mac stands in for a TV: its /tmp plays the part of the TV's RAM-backed /tmp (sized from SB_MEMFREE) */
#endif
}
/* the folder can be written and can free bytes inside a file (the window needs hole punching: FAT cannot) */
static int usable_dir(const char *dir) {
  char parent[1024]; snprintf(parent, sizeof parent, "%s", dir); char *sl = strrchr(parent, '/'); if (sl && sl != parent) *sl = 0;
  mkdir(parent, 0755); mkdir(dir, 0755);
  char p[1100]; snprintf(p, sizeof p, "%s/.probe", dir);
  int fd = open(p, O_CREAT | O_TRUNC | O_RDWR, 0600); if (fd < 0) return 0;
  char buf[4096]; memset(buf, 7, sizeof buf); int ok = 1; for (int i = 0; i < 16 && ok; i++) ok = write(fd, buf, sizeof buf) == (ssize_t)sizeof buf;
#ifdef __linux__
  if (ok && fallocate(fd, 0x01 | 0x02 /* KEEP_SIZE | PUNCH_HOLE */, 16 * 1024, 32 * 1024)) ok = 0;
#elif defined(__APPLE__)
  if (ok) { struct fpunchhole h; memset(&h, 0, sizeof h); h.fp_offset = 16 * 1024; h.fp_length = 32 * 1024; if (fcntl(fd, F_PUNCHHOLE, &h)) ok = 0; }
#endif
  close(fd); unlink(p);
  return ok;
}

/* MB the window may use in a folder: flash keeps room for the picture cache to grow and for the system, RAM keeps
   room for the video pipeline and the engine's own memory */
static long keep_override(void) { const char *e = getenv("SB_P2P_KEEPMB"); return e ? atol(e) : -1; } /* development: the MB kept free, for tests on small disk images */
static long ram_reserve(const Prof *P) { if (keep_override() >= 0) return keep_override();
  long r = P->memTotal * 12 / 100; if (r < 64) r = 64; if (r > 160) r = 160; return r + (P->tier == 0 ? 24 : 32); }
static long flash_reserve(void) { if (keep_override() >= 0) return keep_override();
  long cap = cache_cap_bytes() / (long)MB, kept = cache_kept_bytes(); kept = kept < 0 ? cap / 2 : kept / (long)MB;
  return 24 + (cap > kept ? cap - kept : 0);
}
static long ram_free_now(const char *root) { long m = plat_mem_free_mb(); long t = free_mb(root); if (t >= 0 && (m < 0 || t < m)) m = t; return m; }

/* the window a film wants: about 40 s in front at its bitrate, the same again for the trims' slack, plus head, tail
   and a short look-behind (see the engine's window split) */
static long want_window(double bps) { if (bps <= 0) bps = 1 * MB; long w = (long)(2.2 * bps * 40 / MB) + 32; return w; }
static long want_for(const Prof *P, double bps, uint64_t size) { /* a USB drive or the local cache: the whole film (seeks back and forth stay on it) */
  if (P->loc == LOC_USB || P->loc == LOC_CACHE) { long w = size ? (long)(size / MB) + 64 : P->wmax; return w > P->wmax ? P->wmax : w; } return want_window(bps); }
static long clamp_in(const Prof *P, long want, long room) {
  long w = want; if (w > P->wmax) w = P->wmax; if (w > room) w = room; if (w < W_FLOOR) w = W_FLOOR; return w;
}
static long clamp_window(long want, long room) { return clamp_in(&PF, want, room); }

/* runs on a thread (the engine's start, a move): probes write to each place */
static void choose_profile(Prof *P) {
  memset(P, 0, sizeof *P);
  P->memTotal = plat_mem_total_mb(); if (P->memTotal <= 0) P->memTotal = 8192; /* the Mac: plenty, unless SB_MEMMB says otherwise */
  P->memFree = plat_mem_free_mb(); if (P->memFree < 0) P->memFree = P->memTotal / 4;
  long nc = sysconf(_SC_NPROCESSORS_ONLN); P->cpus = nc > 0 ? (int)nc : 1;
  P->tier = P->memTotal < 1024 || P->memFree < 160 ? 0 : P->memTotal < 2048 || P->memFree < 400 ? 1 : 2;
  P->wmax = P->tier == 0 ? 256 : P->tier == 1 ? 384 : 512;
  /* a USB drive switched on for it comes first: 70 % of its free room, the whole film up to 8 GB */
  UsbDrive d; char ur[1024];
  if (usb_cache_on(&d)) { usb_root(ur, sizeof ur, d.mnt); mkdir(ur, 0755); struct stat st;
    long uf = free_mb(d.mnt), bud = uf * 7 / 10; if (bud > W_USB_MAX) bud = W_USB_MAX;
    if (!stat(ur, &st) && S_ISDIR(st.st_mode) && !access(ur, W_OK) && bud >= W_GOOD) {
      P->loc = LOC_USB; snprintf(P->root, sizeof P->root, "%s", ur); P->budget = bud; P->reserve = uf - bud; P->wmax = W_USB_MAX;
      P->seg = d.punch ? 0 : SEG_MB; snprintf(P->usbMnt, sizeof P->usbMnt, "%s", d.mnt); snprintf(P->usbFs, sizeof P->usbFs, "%s", d.fs);
      P->target = want_for(P, 0, 0); P->window = clamp_in(P, P->target, P->budget);
      fprintf(stderr, "[p2p] window in usb %s (%s%s), %ld MB free, %ld MB now, up to %ld MB\n", P->root, d.fs, P->seg ? ", 16 MB segment files" : "", uf, P->window, P->budget);
      return; }
    fprintf(stderr, "[p2p] usb %s not usable now (%ld MB free)\n", d.mnt, uf); }
  /* the local cache ("Cache torrents locally"): up to the setting's MB on disk, kept between plays */
  { long mb = cache_mb();
    if (mb > 0) { char cr[1024]; cache_root(cr, sizeof cr); mkpath(cr);
      if (usable_dir(cr)) { long cf = free_mb(cr), res = 64, bud = cf < 0 ? -1 : cf - res; if (bud > mb) bud = mb;
        if (bud >= W_GOOD) {
          P->loc = LOC_CACHE; snprintf(P->root, sizeof P->root, "%s", cr); P->budget = bud; P->reserve = cf - bud; P->wmax = mb;
          P->seg = 0; /* usable_dir already proved hole punching: the torrent's own files */
          P->target = want_for(P, 0, 0); P->window = clamp_in(P, P->target, P->budget);
          fprintf(stderr, "[p2p] window in cache %s, %ld MB free, %ld MB now, up to %ld MB\n", P->root, cf, P->window, bud);
          return; }
        fprintf(stderr, "[p2p] local cache %s not usable now (%ld MB free)\n", cr, cf); } } }
  /* the candidates */
  char fr[1024], tr[1024], sr[1024]; flash_root(fr, sizeof fr); tmp_root(tr, sizeof tr, 0); tmp_root(sr, sizeof sr, 1);
  int fOk = usable_dir(fr); long fFree = fOk ? free_mb(fr) : -1, fRes = flash_reserve(), fBud = fFree < 0 ? -1 : fFree - fRes;
  const char *ram = NULL; int rKind = LOC_RAM; long rFree = -1, rRes = 0, rBud = -1;
  if (usable_dir(tr)) ram = tr; else if (usable_dir(sr)) ram = sr;
  if (ram) { if (is_tmpfs(ram)) { rFree = ram_free_now(ram); rRes = ram_reserve(P); } else { rKind = LOC_DISK; rFree = free_mb(ram); rRes = keep_override() >= 0 ? keep_override() : 32; } rBud = rFree < 0 ? -1 : rFree - rRes; }
  int pick; /* 0 flash, 1 ram */
  if (fOk && fBud >= W_GOOD) pick = 0;
  else if (ram && rBud >= W_GOOD) pick = 1;
  else if (fOk && ram) pick = rBud > fBud ? 1 : (rBud == fBud ? (rFree > fFree) : 0);
  else pick = fOk ? 0 : 1;
  if (pick == 1 && !ram) pick = 0; /* nothing usable at all: the flash folder, the engine says what fails */
  P->flashShort = !fOk || fBud < W_GOOD;
  if (pick == 0) { P->loc = LOC_FLASH; snprintf(P->root, sizeof P->root, "%s", fr); P->budget = fBud; P->reserve = fRes; }
  else { P->loc = rKind; snprintf(P->root, sizeof P->root, "%s", ram); P->budget = rBud; P->reserve = rRes; }
  if (pick == 0 && ram) rmdir(ram); else if (pick == 1) rmdir(fr); /* the probe's empty folder where the window is not */
  if (P->budget < W_FLOOR) P->budget = W_FLOOR; /* never "not possible": the smallest window, wherever there is most room */
  P->target = want_window(0); P->window = clamp_in(P, P->target, P->budget);
  P->lowStorage = P->flashShort || P->window < P->target;
  char fs[96], rs[96];
  if (fOk) snprintf(fs, sizeof fs, "%ld MB free, keeps %ld -> %ld MB", fFree, fRes, fBud); else snprintf(fs, sizeof fs, "not usable");
  if (ram) snprintf(rs, sizeof rs, "%s %ld MB free, keeps %ld -> %ld MB", rKind == LOC_RAM ? "tmpfs" : "disk", rFree, rRes, rBud); else snprintf(rs, sizeof rs, "not usable");
  fprintf(stderr, "[p2p] profile: RAM %d MB (free %d), %d CPU(s), %s | flash %s | memory %s | window in %s (%s), %ld MB now, up to %ld MB%s\n",
    P->memTotal, P->memFree, P->cpus, P->tier == 0 ? "small" : P->tier == 1 ? "medium" : "large", fs, rs, loc_name(P->loc), P->root,
    P->window, P->budget < P->wmax ? P->budget : P->wmax, P->lowStorage ? " (low storage)" : "");
}

/* libtorrent for the device's memory: fewer connections and peers, small socket and send buffers, a short disk queue */
static void apply_memory_settings(void) {
  static const struct { const char *n; int v[3]; } S[] = {
    { "connections_limit",            {   50,    80,   120 } },
    { "connection_speed",             {   15,    25,    35 } },
    { "max_peerlist_size",            {  400,  1000,  2000 } },
    { "max_paused_peerlist_size",     {   50,   200,   500 } },
    { "num_want",                     {   50,   100,   200 } },
    { "send_buffer_watermark",        { 196608, 393216, 524288 } },
    { "send_buffer_low_watermark",    { 32768, 65536, 10240 * 10 } },
    { "send_buffer_watermark_factor", {   50,    50,    50 } },
    { "recv_socket_buffer_size",      { 131072, 262144, 0 } },   /* 0: the system's own */
    { "send_socket_buffer_size",      { 65536, 131072, 0 } },
    { "max_queued_disk_bytes",        { 524288, 1048576, 1048576 } },
    { "max_allowed_in_request_queue", {   64,   250,   500 } },
    { "unchoke_slots_limit",          {    4,     6,     8 } },
    { "dht_max_peers",                {  300,  1000,  5000 } },
    { "dht_max_torrents",             {  200,   600,  2000 } },
    { "dht_max_dht_items",            {  100,   300,   700 } },
    { "file_pool_size",               {    8,    16,    40 } },
    { "aio_threads",                  {    1,     2,     4 } },
    { "hashing_threads",              {    1,     1,     2 } },
    { "stop_tracker_timeout",         {    1,     2,     2 } },   /* s: stopping (idle, exit) does not wait long for trackers */
  };
  char bad[300] = ""; int nb = 0;
  for (unsigned i = 0; i < sizeof S / sizeof *S; i++) { int v = S[i].v[PF.tier];
    if (!strcmp(S[i].n, "connection_speed") && PF.cpus <= 2 && v > 15) v = 15; /* a slow CPU: fewer handshakes a second */
    if (E->setting(S[i].n, v)) nb += snprintf(bad + nb, sizeof bad - nb > 0 ? sizeof bad - nb : 0, " %s", S[i].n); }
  if (bad[0]) fprintf(stderr, "[p2p] settings not taken:%s\n", bad);
}
static uint64_t mem_cache_bytes(void) { return (PF.tier == 0 ? 4 : PF.tier == 1 ? 8 : 16) * MB; }
static uint64_t upload_bps(void) { return (PF.tier == 0 ? 256 : PF.tier == 1 ? 512 : 1024) * 1024ull; }

static void *engine_start(void *a) { plat_thread_background(5); (void)a; /* libtorrent's threads start from here and keep the lower priority */
  choose_profile(&PF);
  snprintf(cacheDir, sizeof cacheDir, "%s", PF.root);
  snprintf(stateDir, sizeof stateDir, "%s/cache/engine-state", store_data_dir());
  int reuse = keptIh[0] && R.ih[0] && !strcmp(R.ih, keptIh) && !strcmp(cacheDir, keptRoot); /* the same torrent again: its files are still there */
  if (reuse) { fprintf(stderr, "[p2p] reusing local cache for %s\n", R.ih); mkdir(cacheDir, 0755); }
  else rm_tree(cacheDir); mkdir(cacheDir, 0755); { char c[1100]; snprintf(c, sizeof c, "%s/cache", store_data_dir()); mkdir(c, 0755); } mkdir(stateDir, 0755); /* the DHT table stays between runs */
  if (!reuse && keptRoot[0]) evict_kept(); /* anything kept is gone now (wiped above, or another place) */
  int r = E->start(stateDir, cacheDir, (uint64_t)PF.window * MB, upload_bps(), mem_cache_bytes(), (uint64_t)PF.seg * MB);
  if (r) { snprintf(engErr, sizeof engErr, "%s", E->error(NULL)); ENG_SET(ENG_FAILED); return NULL; }
  apply_memory_settings();
  fprintf(stderr, "[p2p] engine on: window %ld MB in %s, piece cache %llu MB, upload %llu KB/s, tier %d\n", PF.window, loc_name(PF.loc),
    (unsigned long long)(mem_cache_bytes() / MB), (unsigned long long)(upload_bps() / 1024), PF.tier);
  ENG_SET(ENG_ON);
  return NULL;
}
static void *engine_stop(void *a) { (void)a; E->stop(); rm_tree(cacheDir); rmdir(cacheDir); ENG_SET(ENG_OFF); return NULL; }
/* the engine parked with a kept cache: stopped, but the payload stays on disk for the next play */
static void *engine_park(void *a) { (void)a; E->stop(); ENG_SET(ENG_OFF); fprintf(stderr, "[p2p] engine parked, local cache kept in %s\n", cacheDir); return NULL; }
/* left-overs of a run that did not end well (a crash, the TV switched off): in RAM they would hold memory until the
   TV restarts. Swept once at start, on a thread */
static void *sweep(void *a) { (void)a; plat_thread_background(15);
  char p[1024]; flash_root(p, sizeof p); rm_tree(p); tmp_root(p, sizeof p, 0); rm_tree(p); rmdir(p); tmp_root(p, sizeof p, 1); rm_tree(p); rmdir(p);
  cache_root(p, sizeof p); rm_tree(p); rmdir(p); /* a kept cache from a run that never exited cleanly */
  for (int i = 0; i < 2; i++) { struct stat st; const char *old = i ? "/dev/shm/sb-engine" : "/tmp/sb-engine"; /* the shared name earlier versions used, when it is ours */
    if (!lstat(old, &st) && S_ISDIR(st.st_mode) && st.st_uid == geteuid()) { rm_tree(old); rmdir(old); } }
  __atomic_store_n(&cleaning, 0, __ATOMIC_RELEASE); return NULL; }
/* ---- the window changes place while the engine runs: the USB drive went or was switched off (at once, even mid-film:
   the engine downloads again in the next place and the stream keeps its address), or one was switched on (between
   films only). The choice probes the places, so it is made on a thread; p2p_pump applies it. ---- */
static int engRun, relocRun;
static void *reloc_thread(void *a) { (void)a; plat_thread_background(10); choose_profile(&relocTo); __atomic_store_n(&relocReady, 1, __ATOMIC_RELEASE); return NULL; }
static void *rm_later(void *a) { plat_thread_background(15); char *d = a; SDL_Delay(5000); /* writes queued for the old place end first */
  if (strcmp(d, cacheDir)) { rm_tree(d); rmdir(d); } free(d); return NULL; }
static void watch_usb(double t) {
  if (!E || ENG() != ENG_ON) return;
  if (relocating) {
    if (!__atomic_load_n(&relocReady, __ATOMIC_ACQUIRE)) return;
    relocating = 0; relocReady = 0;
    if (relocRun != engRun || !strcmp(relocTo.root, PF.root)) return; /* worked out for an engine that stopped since, or no change */
    Prof n = relocTo; n.target = want_for(&n, A.bps, A.size); n.window = clamp_in(&n, n.target, n.budget); n.lowStorage = n.flashShort || n.window < n.target;
    if (E->relocate(n.root, (uint64_t)n.seg * MB)) { fprintf(stderr, "[p2p] window could not move to %s\n", n.root); return; }
    char *old = strdup(PF.root); int was = PF.loc; long ww = PF.window;
    PF = n; snprintf(cacheDir, sizeof cacheDir, "%s", PF.root); E->window((uint64_t)PF.window * MB); A.lastWatch = 0; relocAt = now_s();
    fprintf(stderr, "[p2p] window moved from %s to %s (%s), %ld -> %ld MB%s\n", loc_name(was), loc_name(PF.loc), PF.root, ww, PF.window, A.ih[0] ? ", while playing" : "");
    pthread_t th; if (old && !pthread_create(&th, NULL, rm_later, old)) pthread_detach(th); else free(old);
    return; }
  unsigned s = usb_serial(); if (s == relocSerial && t < relocLook) return; relocSerial = s; relocLook = t + 1;
  UsbDrive d; int want = usb_cache_on(&d);
  int leave = PF.loc == LOC_USB && (!want || strcmp(d.mnt, PF.usbMnt));
  int enter = PF.loc != LOC_USB && want && !A.ih[0] && (!R.on || R.stage == 0);
  if (!leave && !enter) return;
  relocating = 1; relocReady = 0; relocRun = engRun;
  pthread_t th; if (pthread_create(&th, NULL, reloc_thread, NULL)) relocating = 0; else pthread_detach(th);
}
int p2p_moved_recently(void) { return relocAt > 0 && now_s() - relocAt < 30; }

/* 0 started / starting / will start, -1 cannot */
static int ensure_engine(void) {
  int s = ENG(); if (s == ENG_ON || s == ENG_STARTING || s == ENG_STOPPING) return 0; /* after a stop, p2p_pump starts it again */
  if (__atomic_load_n(&cleaning, __ATOMIC_ACQUIRE)) return 0;
  if (!load()) return -1;
  ENG_SET(ENG_STARTING); engErr[0] = 0; engRun++;
  pthread_t th; if (pthread_create(&th, NULL, engine_start, NULL)) { ENG_SET(ENG_FAILED); return -1; }
  pthread_detach(th);
  return 0;
}

static void fail(const char *msg) {
  P2pCb cb = R.cb; void *ud = R.ud; R.on = 0; R.status[0] = 0;
  if (cb) cb(ud, NULL, msg, 0);
}
static int hexval(int c) { return isdigit(c) ? c - '0' : tolower(c) - 'a' + 10; }
void p2p_resolve(const char *url, P2pCb cb, void *ud) {
  memset(&R, 0, sizeof R); R.cb = cb; R.ud = ud; R.t0 = now_s();
  memset(&A, 0, sizeof A); A.startT0 = R.t0;
  /* p2p://<ih>/<idx>?tr=<encoded>&tr=... */
  const char *p = url + 6; int n = 0; while (n < 40 && isxdigit((unsigned char)p[n])) { R.ih[n] = (char)tolower((unsigned char)p[n]); n++; }
  R.ih[n] = 0; if (n != 40) { R.on = 1; fail(T("Couldn’t start the torrent engine")); return; }
  if (keptIh[0] && strcmp(keptIh, R.ih)) evict_kept(); /* another torrent now: the kept cache makes room */
  p += 40; R.want = -1; if (*p == '/') { p++; if (isdigit((unsigned char)*p)) R.want = atoi(p); }
  int m = snprintf(R.magnet, sizeof R.magnet, "magnet:?xt=urn:btih:%s", R.ih);
  const char *q = strchr(url, '?');
  while (q && (q = strstr(q, "tr=")) && m < (int)sizeof R.magnet - 600) { /* the addon's own trackers first (already encoded) */
    q += 3; const char *e = strchr(q, '&'); int l = e ? (int)(e - q) : (int)strlen(q);
    char dec[512]; int k = 0; for (int i = 0; i < l && k < 511; i++) { if (q[i] == '%' && i + 2 < l) { dec[k++] = (char)(hexval(q[i + 1]) * 16 + hexval(q[i + 2])); i += 2; } else dec[k++] = q[i]; } dec[k] = 0;
    if (!strncmp(dec, "udp://", 6) || !strncmp(dec, "http://", 7) || !strncmp(dec, "https://", 8) || !strncmp(dec, "ws", 2)) m += snprintf(R.magnet + m, sizeof R.magnet - m, "&tr=%.*s", l, q);
    q = e; }
  for (unsigned i = 0; i < sizeof PUBLIC_TRACKERS / sizeof *PUBLIC_TRACKERS && m < (int)sizeof R.magnet - 200; i++) { /* then the public ones, encoded */
    m += snprintf(R.magnet + m, sizeof R.magnet - m, "&tr="); for (const char *s = PUBLIC_TRACKERS[i]; *s && m < (int)sizeof R.magnet - 4; s++) {
      if (isalnum((unsigned char)*s) || *s == '.' || *s == '-' || *s == '_') R.magnet[m++] = *s; else m += snprintf(R.magnet + m, sizeof R.magnet - m, "%%%02X", (unsigned char)*s); }
    R.magnet[m] = 0; }
  R.on = 1; R.stage = 0; R.status[0] = 0;
  if (ensure_engine()) fail(T("Couldn’t start the torrent engine"));
}
void p2p_cancel(void) { if (!R.on) return; R.on = 0; R.status[0] = 0; R.cb = NULL; if (E && ENG() == ENG_ON) E->remove(R.ih); kept_invalidate(R.ih); idleSince = now_s(); }
/* a direct (non-torrent) title starts: the kept torrent cache makes room for it */
void p2p_note_direct_play(void) { evict_kept(); }
const char *p2p_status(void) { return R.on ? R.status : ""; }

static int is_video(const char *n) { const char *d = strrchr(n, '.'); if (!d) return 0; static const char *ext[] = { ".mp4", ".mkv", ".avi", ".ts", ".m4v", ".webm", ".mov" };
  for (unsigned i = 0; i < sizeof ext / sizeof *ext; i++) if (!strcasecmp(d, ext[i])) return 1; return 0; }
/* the requested file when it is a video, else the largest video, else the largest file */
static int pick_file(const sbe_file *f, int n, int want) {
  if (want >= 0 && want < n && is_video(f[want].path)) return want;
  if (want >= 0) { int nv = 0; for (int i = 0; i < n; i++) nv += is_video(f[i].path); if (nv > 1) return -1; } /* a pack without the file asked for: never another episode in its place */
  int best = -1; uint64_t mx = 0; for (int i = 0; i < n; i++) if (is_video(f[i].path) && f[i].size > mx) { mx = f[i].size; best = i; }
  if (best >= 0) return best;
  best = 0; mx = 0; for (int i = 0; i < n; i++) if (f[i].size > mx) { mx = f[i].size; best = i; }
  return best;
}
static void fmt_rate(double b, char *o, size_t n) { char v[16];
  if (b >= 1e6) { snprintf(v, sizeof v, "%.1f", b / 1e6); snprintf(o, n, "%s", T1("{n} MB/s", "n", v)); }
  else { snprintf(v, sizeof v, "%d", (int)(b / 1e3 + 0.5)); snprintf(o, n, "%s", T1("{n} KB/s", "n", v)); } }
static void fmt_mbs(double b, char *o, size_t n) { char v[16]; snprintf(v, sizeof v, "%.1f", b / 1e6); snprintf(o, n, "%s", T1("{n} MB/s", "n", v)); }
int p2p_peer_line(char *out, int cap) {
  if (!E || ENG() != ENG_ON || !A.ih[0]) return 0;
  sbe_stats st; memset(&st, 0, sizeof st); if (E->stats(&st) || !st.peers) return 0;
  char sp[64], pe[16]; fmt_rate(A.rate, sp, sizeof sp); snprintf(pe, sizeof pe, "%u", st.peers);
  snprintf(out, cap, "%s  ·  %s%s", T1("{n} peers", "n", pe), sp, PF.loc == LOC_USB ? "  ·  USB" : PF.loc == LOC_CACHE ? "  ·  cache" : ""); /* the buffer on the USB drive or in the local cache: said after the speed */
  return 1;
}

/* the window for the film's bitrate, within the room the location has now */
static void window_follow(double bps) {
  if (!E || ENG() != ENG_ON || bps <= 0) return;
  if (A.bps > 0 && bps > A.bps * 0.85 && bps < A.bps * 1.15) return;
  A.bps = bps; PF.target = want_for(&PF, bps, A.size);
  A.lastWatch = 0; /* the watcher applies it at its next look */
}
/* every 2 s while a torrent is active: what is free where the window lives; the window shrinks when the room runs low
   (another app, the picture cache, the video pipeline's memory) and grows back toward what the film wants */
static void watch_room(double t) {
  if (t - A.lastWatch < 2.0) return; A.lastWatch = t;
  sbe_stats st; memset(&st, 0, sizeof st); if (E->stats(&st)) return;
  long fr; UsbDrive d;
  if (PF.loc == LOC_USB) fr = usb_drive(&d) && !strcmp(d.mnt, PF.usbMnt) ? d.freeMb : -1; /* the watcher's last look: a drive is never asked from the drawing thread */
  else fr = PF.loc == LOC_RAM ? ram_free_now(PF.root) : free_mb(PF.root);
  if (fr < 0) return;
  long resident = (long)(st.resident / MB);
#ifndef __linux__
  if (PF.loc == LOC_RAM) fr -= resident; /* the Mac's stand-in free memory (SB_MEMFREE) does not shrink as the window fills; a TV's does */
#endif
  long room = fr + resident - PF.reserve;
  if (t - A.lastReport >= 30) { A.lastReport = t; /* for the TV's log: is the window holding, and how does the engine fare */
    fprintf(stderr, "[p2p] buffer: %ld MB in %s (window %ld MB, freed %llu MB, refetched %llu pieces), %ld MB free there, %u peers, %.1f MB/s, engine cache %llu MB%s\n", resident, loc_name(PF.loc), PF.window,
      (unsigned long long)(st.freed / MB), (unsigned long long)st.refetched, fr, st.peers, A.rate / 1e6, (unsigned long long)(st.mem_cache_used / MB), st.window_unsupported ? ", STORAGE CANNOT FREE PIECES" : ""); }
  long w = clamp_window(PF.target, room);
  int low = PF.flashShort || w < PF.target;
  if (low != PF.lowStorage) PF.lowStorage = low;
  long step = PF.loc == LOC_USB ? 128 : 8; /* a USB drive keeps 30 % free and frees in 16 MB segments: only changes worth it */
  if (w != PF.window && (w < PF.window - (PF.loc == LOC_USB ? step : 0) || w - PF.window >= step)) { /* shrink at once, grow in steps worth a change */
    fprintf(stderr, "[p2p] window %ld -> %ld MB (%s: %ld MB free, %ld MB ours, film wants %ld MB)%s\n", PF.window, w, loc_name(PF.loc), fr, resident, PF.target,
      st.window_unsupported ? " [storage cannot free pieces]" : "");
    PF.window = w; E->window((uint64_t)w * MB); }
}

/* full speed at the start, after a seek and while the player waits; after 15 s of smooth play, at most
   twice the file's average bitrate (at least 1 MB/s) so the rest of the home line stays free */
void p2p_pace(int playing, int buffering, double dur) {
  if (!E || ENG() != ENG_ON || !A.ih[0]) return;
  if (dur > 60 && A.size > 0 && dur != A.dur) { A.dur = dur; window_follow((double)A.size / dur); }
  if (playing && !buffering) { A.calm++; A.playedOnce = 1; A.stallT0 = 0; } else A.calm = 0;
  int gentle = A.calm >= 15 && A.size > 0 && dur > 60;
  if (gentle == A.gentle) return;
  A.gentle = gentle;
  double cap = 2.0 * (double)A.size / dur; if (cap < 1048576) cap = 1048576;
  E->limits(gentle ? (uint64_t)cap : 0, upload_bps());
}

/* One short line on why it is slow, only once it is: "Finding peers…", "Getting the file list…", "Slow source: 0.6
   MB/s for a 4 MB/s video", "Few peers (3) · 0.4 MB/s", "Low storage: streaming from memory", "Waiting for the next
   pieces…". It loads on regardless. */
static int reason_now(char *o, size_t n) { /* the kind of reason (for the log), the line in o */
  sbe_stats st; memset(&st, 0, sizeof st); if (!E || ENG() != ENG_ON || E->stats(&st)) { snprintf(o, n, "%s", T("Finding peers…")); return 1; }
  double secs = now_s() - A.startT0; char r[64], v[64], pe[16];
  if (R.on && R.stage < 2) { /* no file list yet */
    char s[16]; snprintf(s, sizeof s, "%d", (int)secs);
    if (secs >= 30) snprintf(o, n, "%s", T1("Still looking for peers… {s}s · this torrent may have few seeders", "s", s)); /* peers that never send the list are no better than none */
    else snprintf(o, n, "%s", st.peers ? T("Getting the file list…") : T("Finding peers…"));
    return secs >= 30 ? 2 : st.peers ? 3 : 1; }
  if (!st.peers) { snprintf(o, n, "%s", T("Finding peers…")); return 1; }
  double need = A.size > 0 && A.dur > 60 ? (double)A.size / A.dur : 0;
  if (need > 0 && A.rate < need * 0.9) { fmt_mbs(A.rate, r, sizeof r); fmt_mbs(need, v, sizeof v); snprintf(o, n, "%s", T2("Slow source: {r} for a {v} video", "r", r, "v", v)); return 4; }
  if (st.peers < 5) { fmt_rate(A.rate, r, sizeof r); snprintf(pe, sizeof pe, "%u", st.peers); snprintf(o, n, "%s", T2("Few peers ({n}) · {r}", "n", pe, "r", r)); return 5; }
  if (PF.lowStorage) { snprintf(o, n, "%s", PF.loc == LOC_RAM ? T("Low storage: streaming from memory") : T("Low storage: small buffer")); return 6; }
  snprintf(o, n, "%s", T("Waiting for the next pieces…"));
  return 7;
}
const char *p2p_wait_reason(int waiting) {
  double t = now_s();
  if (!waiting) { A.stallT0 = 0; A.reason[0] = 0; A.reasonKind = 0; return ""; }
  double since, slow;
  if (!A.playedOnce) { since = A.startT0 > 0 ? A.startT0 : t; slow = START_SLOW; }
  else { if (A.stallT0 <= 0) A.stallT0 = t; since = A.stallT0; slow = STALL_SLOW; }
  if (t - since < slow) { A.reason[0] = 0; A.reasonKind = 0; return ""; }
  if (!A.reason[0] || t - A.lastReason >= 1.0) { A.lastReason = t; int k = reason_now(A.reason, sizeof A.reason);
    if (k != A.reasonKind || t - A.reasonLogT >= 15) { A.reasonKind = k; A.reasonLogT = t; fprintf(stderr, "[p2p] slow (%.0f s, %s): %s\n", t - since, A.playedOnce ? "stalled" : "start", A.reason); } }
  return A.reason;
}

void p2p_shutdown(void) {
  usb_shutdown();
  evict_kept(); /* wipe on exit: the local cache never survives a restart */
  if (!E) return;
  for (int i = 0; i < 300 && (ENG() == ENG_STARTING || ENG() == ENG_STOPPING); i++) SDL_Delay(10); /* a start or stop under way ends first (3 s at most) */
  if (ENG() == ENG_ON) { Uint32 t0 = SDL_GetTicks(); E->remove_all(); /* no torrent state left behind */ E->stop(); rm_tree(cacheDir); rmdir(cacheDir); ENG_SET(ENG_OFF); fprintf(stderr, "[p2p] engine stopped at exit in %u ms\n", SDL_GetTicks() - t0); }
}
void p2p_player_closed(void) {
  if (R.on) p2p_cancel();
  if (E && ENG() == ENG_ON && cache_mb() > 0 && A.ih[0] && PF.loc == LOC_CACHE) {
    /* kept for the next play of this torrent: the engine stops, the payload stays */
    snprintf(keptIh, sizeof keptIh, "%s", A.ih); snprintf(keptRoot, sizeof keptRoot, "%s", cacheDir);
    memset(&A, 0, sizeof A); idleSince = 0; ENG_SET(ENG_STOPPING);
    pthread_t th; if (!pthread_create(&th, NULL, engine_park, NULL)) { pthread_detach(th); return; }
    ENG_SET(ENG_ON); E->remove_all(); evict_kept(); idleSince = now_s(); /* the park never started: fall back to deleting */
    return;
  }
  if (E && ENG() == ENG_ON) E->remove_all(); /* the engine deletes each torrent's payload with it */
  evict_kept(); /* the setting is off (or nothing cached there): nothing stays behind */
  memset(&A, 0, sizeof A); idleSince = now_s();
}
void p2p_pump(void) {
  usb_pump();
  static int swept; if (!swept) { swept = 1; __atomic_store_n(&cleaning, 1, __ATOMIC_RELEASE); pthread_t th; if (!pthread_create(&th, NULL, sweep, NULL)) pthread_detach(th); else cleaning = 0; }
  double t = now_s(); int es = ENG();
  if (E && es == ENG_ON && (A.ih[0] || R.on)) { /* the measured rate: bytes received between looks, smoothed */
    if (t - A.lastT >= 1.0) { sbe_stats st; if (!E->stats(&st)) { if (A.lastT > 0 && st.downloaded >= A.lastBytes) { double r = (double)(st.downloaded - A.lastBytes) / (t - A.lastT); A.rate = A.rate * 0.6 + r * 0.4; } A.lastBytes = st.downloaded; } A.lastT = t; }
    if (A.ih[0]) watch_room(t); }
  watch_usb(t);
  if (!R.on) { if (E && es == ENG_ON && !A.ih[0] && idleSince > 0 && t - idleSince > idle_limit()) { ENG_SET(ENG_STOPPING); pthread_t th; if (!pthread_create(&th, NULL, engine_stop, NULL)) pthread_detach(th); else ENG_SET(ENG_ON); idleSince = 0; fprintf(stderr, "[p2p] engine stopped after %.0f idle seconds\n", idle_limit()); } return; }
  if (es == ENG_OFF) { if (ensure_engine()) { fail(T("Couldn’t start the torrent engine")); return; } }
  if (es == ENG_FAILED) { fprintf(stderr, "[p2p] engine failed: %s\n", engErr); ENG_SET(ENG_OFF); fail(T("Couldn’t start the torrent engine")); return; }
  if (es != ENG_ON) { snprintf(R.status, sizeof R.status, "%s", p2p_wait_reason(1)); return; }
  if (R.stage == 0 && relocating) return; /* a USB drive just switched on: the torrent starts there */
  if (R.stage == 0) { if (E->open(R.magnet, R.ih)) { fail(T("Couldn’t start the torrent engine")); return; } R.stage = 1; }
  snprintf(R.status, sizeof R.status, "%s", p2p_wait_reason(1)); /* the art's line: nothing while it is quick, then why it is slow */
  if (R.stage == 1) {
    static sbe_file files[512]; int n = E->files(R.ih, files, 512);
    if (n == -2) { fprintf(stderr, "[p2p] %s: %s\n", R.ih, E->error(R.ih)); E->remove(R.ih); kept_invalidate(R.ih); fail(T("No peers found for this torrent")); return; }
    if (n < 0) return; /* no file list yet: it keeps looking, the line says so */
    if (n > 512) n = 512;
    int idx = pick_file(files, n, R.want);
    if (idx < 0) { fprintf(stderr, "[p2p] %s: file #%d is not a video of this torrent's %d file(s)\n", R.ih, R.want, n); E->remove(R.ih); kept_invalidate(R.ih); fail(T("The file isn’t in this torrent pack")); return; }
    R.idx = (uint32_t)idx; R.size = files[idx].size; R.stage = 2;
    snprintf(A.ih, sizeof A.ih, "%s", R.ih); A.size = R.size; A.lastWatch = 0;
    { double est = (double)R.size / 5400.0; if (est < 0.4 * MB) est = 0.4 * MB; if (est > 6.0 * MB) est = 6.0 * MB; window_follow(est); } /* a guess from the size (a 90-minute film) until the player knows the length */
    fprintf(stderr, "[p2p] %s: %d file(s), playing #%d (%llu MB) after %.1f s\n", R.ih, n, idx, (unsigned long long)(R.size >> 20), t - R.t0);
  }
  if (R.stage == 2) {
    char url[600]; int r = E->stream(R.ih, R.idx, url, sizeof url);
    if (r < 0) { fprintf(stderr, "[p2p] stream failed: %s\n", E->error(R.ih)); fail(T("Couldn’t start the torrent engine")); return; }
    if (r == 0) return;
    snprintf(A.ih, sizeof A.ih, "%s", R.ih); A.size = R.size; A.calm = 0; A.gentle = 0; E->limits(0, upload_bps());
    P2pCb cb = R.cb; void *ud = R.ud; uint64_t size = R.size; R.on = 0; R.status[0] = 0;
    fprintf(stderr, "[p2p] stream ready after %.1f s: %s\n", t - R.t0, url);
    if (cb) cb(ud, url, NULL, size);
  }
}
