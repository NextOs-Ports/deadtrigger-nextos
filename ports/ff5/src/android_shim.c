/*
 * android_shim.c -- minimal fake Android NDK for Linux (ARM32)
 *
 * AAsset / AAssetManager   -> real files under a configurable data dir.
 * ANativeWindow_*         -> fake handle backed by the SDL/EGL window size.
 * ALooper_* (minimal)     -> fake loopers; pollAll pumps the audio callbacks.
 * ASensor* (stubs)        -> safe "no sensors" values.
 * __android_log_*         -> stderr.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "android_shim.h"
#include "egl_shim.h"
#include "opensl.h"
#include "util.h"

/* ---------------- data dir + path resolution ---------------- */

static char g_datadir[1024] = ".";

void android_shim_set_datadir(const char *dir) {
  if (dir && dir[0]) {
    snprintf(g_datadir, sizeof g_datadir, "%s", dir);
    /* strip trailing slash */
    size_t n = strlen(g_datadir);
    while (n > 1 && g_datadir[n - 1] == '/') g_datadir[--n] = 0;
  } else {
    strcpy(g_datadir, ".");
  }
  debugPrintf("android_shim: datadir = %s\n", g_datadir);
}

/* Resolve an asset name to a real path. Tries, in order:
   <datadir>/<name>, <datadir>/assets/<name>, then the raw <name>. */
static FILE *asset_fopen(const char *name, long *out_len) {
  if (!name) return NULL;
  char path[2048];
  FILE *fp = NULL;
  const char *n = name;
  while (*n == '/') n++;                 /* asset names are relative */

  snprintf(path, sizeof path, "%s/%s", g_datadir, n);
  fp = fopen(path, "rb");
  if (!fp) {
    snprintf(path, sizeof path, "%s/assets/%s", g_datadir, n);
    fp = fopen(path, "rb");
  }
  if (!fp)
    fp = fopen(name, "rb");             /* last resort: as-given */

  if (fp && out_len) {
    fseek(fp, 0, SEEK_END);
    *out_len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
  }
  debugPrintf("android_shim: AAssetManager_open('%s') -> %s\n", name, fp ? "ok" : "MISS");
  return fp;
}

/* ---------------- AAsset ---------------- */

struct AAsset {
  FILE *fp;
  long length;
  void *buffer;   /* lazy full-file buffer for AAsset_getBuffer */
};

AAsset *AAssetManager_open(AAssetManager *mgr, const char *filename, int mode) {
  (void)mgr; (void)mode;
  long len = 0;
  FILE *fp = asset_fopen(filename, &len);
  if (!fp) return NULL;
  AAsset *a = (AAsset *)calloc(1, sizeof(AAsset));
  if (!a) { fclose(fp); return NULL; }
  a->fp = fp;
  a->length = len;
  return a;
}

int AAsset_read(AAsset *asset, void *buf, size_t count) {
  if (!asset || !asset->fp) return -1;
  size_t r = fread(buf, 1, count, asset->fp);
  return (int)r;
}

long AAsset_seek(AAsset *asset, long offset, int whence) {
  if (!asset || !asset->fp) return -1;
  if (fseek(asset->fp, offset, whence) != 0) return -1;
  return ftell(asset->fp);
}

long AAsset_getLength(AAsset *asset) {
  return asset ? asset->length : 0;
}

const void *AAsset_getBuffer(AAsset *asset) {
  if (!asset || !asset->fp) return NULL;
  if (!asset->buffer && asset->length > 0) {
    asset->buffer = malloc((size_t)asset->length);
    if (asset->buffer) {
      long cur = ftell(asset->fp);
      fseek(asset->fp, 0, SEEK_SET);
      if (fread(asset->buffer, 1, (size_t)asset->length, asset->fp) != (size_t)asset->length) {
        free(asset->buffer); asset->buffer = NULL;
      }
      fseek(asset->fp, cur, SEEK_SET);
    }
  }
  return asset->buffer;
}

void AAsset_close(AAsset *asset) {
  if (!asset) return;
  if (asset->fp) fclose(asset->fp);
  if (asset->buffer) free(asset->buffer);
  free(asset);
}

AAssetManager *AAssetManager_fromJava(void *env, void *assetManager) {
  (void)env; (void)assetManager;
  static int fake_mgr;
  return (AAssetManager *)&fake_mgr;
}

/* ---------------- ANativeWindow ---------------- */

static int g_fake_window;

ANativeWindow *ANativeWindow_fromSurface(void *env, void *surface) {
  (void)env; (void)surface;
  return (ANativeWindow *)&g_fake_window;
}

void ANativeWindow_acquire(ANativeWindow *window) { (void)window; }
void ANativeWindow_release(ANativeWindow *window) { (void)window; }

int32_t ANativeWindow_getWidth(ANativeWindow *window) {
  (void)window;
  return (int32_t)egl_shim_width();
}

int32_t ANativeWindow_getHeight(ANativeWindow *window) {
  (void)window;
  return (int32_t)egl_shim_height();
}

int32_t ANativeWindow_getFormat(ANativeWindow *window) {
  (void)window;
  return 1; /* WINDOW_FORMAT_RGBA_8888 */
}

int32_t ANativeWindow_setBuffersGeometry(ANativeWindow *window, int32_t w,
                                         int32_t h, int32_t format) {
  (void)window; (void)w; (void)h; (void)format;
  return 0;
}

/* ---------------- ALooper (minimal) ---------------- */

static int g_fake_looper;

ALooper *ALooper_forThread(void) { return (ALooper *)&g_fake_looper; }
ALooper *ALooper_prepare(int opts) { (void)opts; return (ALooper *)&g_fake_looper; }
void ALooper_acquire(ALooper *looper) { (void)looper; }
void ALooper_release(ALooper *looper) { (void)looper; }
void ALooper_wake(ALooper *looper) { (void)looper; }

int ALooper_pollAll(int timeoutMillis, int *outFd, int *outEvents, void **outData) {
  (void)outFd; (void)outEvents; (void)outData;
  /* Keep audio flowing even if the guest parks in a looper poll. */
  opensles_shim_pump_callbacks();
  if (timeoutMillis > 0) {
    int t = timeoutMillis > 5 ? 5 : timeoutMillis;
    usleep((useconds_t)t * 1000);
  }
  return -3; /* ALOOPER_POLL_TIMEOUT */
}

/* ---------------- ASensor* (stubs: no sensors) ---------------- */

ASensorManager *ASensorManager_getInstance(void) {
  static int fake_mgr;
  return (ASensorManager *)&fake_mgr;
}

ASensor const *ASensorManager_getDefaultSensor(ASensorManager *mgr, int type) {
  (void)mgr; (void)type;
  return NULL;
}

int ASensorManager_getSensorList(ASensorManager *mgr, ASensor const *const **list) {
  (void)mgr;
  if (list) *list = NULL;
  return 0;
}

ASensorEventQueue *ASensorManager_createEventQueue(ASensorManager *mgr,
                                                   ALooper *looper, int ident,
                                                   void *callback, void *data) {
  (void)mgr; (void)looper; (void)ident; (void)callback; (void)data;
  static int fake_queue;
  return (ASensorEventQueue *)&fake_queue;
}

int ASensorManager_destroyEventQueue(ASensorManager *mgr, ASensorEventQueue *q) {
  (void)mgr; (void)q; return 0;
}

int ASensorEventQueue_enableSensor(ASensorEventQueue *q, ASensor const *sensor) {
  (void)q; (void)sensor; return 0;
}
int ASensorEventQueue_disableSensor(ASensorEventQueue *q, ASensor const *sensor) {
  (void)q; (void)sensor; return 0;
}
int ASensorEventQueue_setEventRate(ASensorEventQueue *q, ASensor const *sensor, int32_t usec) {
  (void)q; (void)sensor; (void)usec; return 0;
}
int ASensorEventQueue_hasEvents(ASensorEventQueue *q) { (void)q; return 0; }
int ASensorEventQueue_getEvents(ASensorEventQueue *q, void *events, size_t count) {
  (void)q; (void)events; (void)count; return 0;
}

int ASensor_getType(ASensor const *sensor) { (void)sensor; return 0; }
const char *ASensor_getName(ASensor const *sensor) { (void)sensor; return "none"; }
const char *ASensor_getVendor(ASensor const *sensor) { (void)sensor; return "NextOS"; }
ANDROID_SOFTFP float ASensor_getResolution(ASensor const *sensor) { (void)sensor; return 0.0f; }
int ASensor_getMinDelay(ASensor const *sensor) { (void)sensor; return 0; }

/* ---------------- Android logging (-> stderr) ---------------- */

int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap) {
  (void)prio;
  fprintf(stderr, "[log:%s] ", tag ? tag : "?");
  int r = vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  return r;
}

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  int r = __android_log_vprint(prio, tag, fmt, ap);
  va_end(ap);
  return r;
}

int __android_log_write(int prio, const char *tag, const char *text) {
  (void)prio;
  fprintf(stderr, "[log:%s] %s\n", tag ? tag : "?", text ? text : "");
  return 0;
}

int __android_log_buf_print(int bufID, int prio, const char *tag, const char *fmt, ...) {
  (void)bufID;
  va_list ap; va_start(ap, fmt);
  int r = __android_log_vprint(prio, tag, fmt, ap);
  va_end(ap);
  return r;
}

int __android_log_buf_write(int bufID, int prio, const char *tag, const char *text) {
  (void)bufID;
  return __android_log_write(prio, tag, text);
}
