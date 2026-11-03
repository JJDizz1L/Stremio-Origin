/* Video playback behind one small interface: the TV's own media pipeline on webOS (the picture
   is drawn on the video plane under the app's transparent window), a stub on the Mac preview,
   and later the Android TV and Tizen players. The core never sees a platform call. */
#ifndef SB_VIDEO_H
#define SB_VIDEO_H
typedef enum { VID_IDLE = 0, VID_LOADING, VID_PLAYING, VID_PAUSED, VID_BUFFERING, VID_ENDED, VID_ERROR } VidState;
typedef struct { char lang[16], name[64], codec[16]; int idx, forced, channels; } VidTrack;   /* codec: as the platform names it ("dts", "ac3", "eac3"...), "" unknown */
int    video_init(const char *appId);
void   video_load(const char *url);          /* starts loading and playing */
/* a stream that can't seek by itself (one continuous local conversion): each seek loads "<base>?t=<seconds>" again,
   times count from that moment, the length is durSec */
void   video_load_timed(const char *base, double durSec);
void   video_unload(void);                   /* releases the decoder now */
void   video_play(void);
void   video_pause(void);
void   video_seek(double sec);
double video_time(void);
double video_duration(void);                 /* 0 while unknown */
double video_buffered_end(void);             /* seconds buffered ahead (absolute position) */
VidState video_state(void);
const char *video_error(void);
int    video_live(void);                     /* 1 once a first frame has been shown */
int    video_has_media(void);            /* the TV has taken the stream (its media handle exists) */
void   video_set_volume(int pct, int easeMs);  /* the stream's volume, 0-100, reached over easeMs */
void   video_set_hwdec(const char *mode); /* decoding backend ("auto", "no", "nvdec", ...): takes effect on the next title played */
int    video_settling(void);                 /* 1 while a seek is on its way (asked, not yet done): the picture is not where it will be */
int    video_width(void);
int    video_height(void);
double video_fps(void);          /* the picture's frame rate (23.976...), 0 while unknown */
int    video_hdr(char *out, int cap);  /* the HDR type the TV reports for the stream playing ("DolbyVision", "HDR10", "none"...); 0 while unknown */
int    video_audio_tracks(VidTrack *out, int max);
int    video_audio_current(void);
void   video_select_audio(int idx);
int    video_text_tracks(VidTrack *out, int max);  /* subtitle tracks inside the file (the platform renders them) */
void   video_select_text(int idx);                 /* -1 = off */
/* where the picture goes, in layout units; it is kept inside the screen (a TV's video plane goes black
   for a rectangle that leaves it) */
void   video_set_window(float x, float y, float w, float h);
/* zoom: a part of the decoded frame (pixels of video_width x video_height) shown in a rectangle of the
   screen (layout units). This is how Fill and Zoom work on a TV, never a window larger than the screen.
   0 when the platform cannot crop (then the picture stays whole) */
/* 1 when the plane can be put in part of the screen (a smaller window, or a crop to a smaller box); 0 when only the
   whole screen is safe: webOS 3/4 read a partial rectangle in the TV's own display space (1280x720 on a 2018 set,
   whatever the app's surface), one outside it shows sound over a black screen */
int    video_partial_plane(void);
int    video_set_crop(int sx, int sy, int sw, int sh, float dx, float dy, float dw, float dh);
void   video_set_surface(int w, int h);      /* the drawable's size: the plane's rectangle is read in its pixels */
int    video_audio_unsupported(void);        /* 1 when the TV plays the picture but not this audio track */
void   video_pump(void);                     /* delivers the platform's events; call per frame */
void   video_render(void);                   /* draws the current frame into the GL framebuffer (desktop GL backends) */
#endif
