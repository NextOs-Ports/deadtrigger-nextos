/*
 * android_shim.h -- minimal fake Android NDK for Linux (ARM32)
 *
 * Covers only what the Unity 2019 IL2CPP guest imports on this port:
 * AAsset / AAssetManager (routed to a data dir), ANativeWindow_*, ALooper_*
 * (minimal), ASensor* (safe stubs), __android_log_* (-> stderr).
 * The game runs through UnityPlayer/JNI, not android_native_app_glue, so no
 * AInputEvent / android_app machinery is provided here.
 */

#ifndef __ANDROID_SHIM_H__
#define __ANDROID_SHIM_H__

#include <stdint.h>
#include <stddef.h>

#if defined(__arm__)
#define ANDROID_SOFTFP __attribute__((pcs("aapcs")))
#else
#define ANDROID_SOFTFP
#endif

/* opaque handles */
typedef struct AAsset AAsset;
typedef struct AAssetManager AAssetManager;
typedef struct AAssetDir AAssetDir;
typedef struct ANativeWindow ANativeWindow;
typedef struct ALooper ALooper;
typedef struct ASensorManager ASensorManager;
typedef struct ASensorEventQueue ASensorEventQueue;
typedef struct ASensor ASensor;

/* ---- Asset manager (routes to the data dir set below) ---- */
#define AASSET_MODE_UNKNOWN   0
#define AASSET_MODE_RANDOM    1
#define AASSET_MODE_STREAMING 2
#define AASSET_MODE_BUFFER    3

AAsset *AAssetManager_open(AAssetManager *mgr, const char *filename, int mode);
int AAsset_read(AAsset *asset, void *buf, size_t count);
long AAsset_seek(AAsset *asset, long offset, int whence);
long AAsset_getLength(AAsset *asset);
void AAsset_close(AAsset *asset);
const void *AAsset_getBuffer(AAsset *asset);
AAssetManager *AAssetManager_fromJava(void *env, void *assetManager);

/* ---- ANativeWindow ---- */
ANativeWindow *ANativeWindow_fromSurface(void *env, void *surface);
void ANativeWindow_acquire(ANativeWindow *window);
void ANativeWindow_release(ANativeWindow *window);
int32_t ANativeWindow_getWidth(ANativeWindow *window);
int32_t ANativeWindow_getHeight(ANativeWindow *window);
int32_t ANativeWindow_getFormat(ANativeWindow *window);
int32_t ANativeWindow_setBuffersGeometry(ANativeWindow *window, int32_t w,
                                         int32_t h, int32_t format);

/* ---- ALooper (minimal) ---- */
ALooper *ALooper_forThread(void);
ALooper *ALooper_prepare(int opts);
void ALooper_acquire(ALooper *looper);
void ALooper_release(ALooper *looper);
int ALooper_pollAll(int timeoutMillis, int *outFd, int *outEvents, void **outData);
void ALooper_wake(ALooper *looper);

/* ---- ASensor* (safe stubs) ---- */
ASensorManager *ASensorManager_getInstance(void);
ASensor const *ASensorManager_getDefaultSensor(ASensorManager *mgr, int type);
int ASensorManager_getSensorList(ASensorManager *mgr, ASensor const *const **list);
ASensorEventQueue *ASensorManager_createEventQueue(ASensorManager *mgr,
                                                   ALooper *looper, int ident,
                                                   void *callback, void *data);
int ASensorManager_destroyEventQueue(ASensorManager *mgr, ASensorEventQueue *q);
int ASensorEventQueue_enableSensor(ASensorEventQueue *q, ASensor const *sensor);
int ASensorEventQueue_disableSensor(ASensorEventQueue *q, ASensor const *sensor);
int ASensorEventQueue_setEventRate(ASensorEventQueue *q, ASensor const *sensor, int32_t usec);
int ASensorEventQueue_hasEvents(ASensorEventQueue *q);
int ASensorEventQueue_getEvents(ASensorEventQueue *q, void *events, size_t count);
int ASensor_getType(ASensor const *sensor);
const char *ASensor_getName(ASensor const *sensor);
const char *ASensor_getVendor(ASensor const *sensor);
ANDROID_SOFTFP float ASensor_getResolution(ASensor const *sensor);
int ASensor_getMinDelay(ASensor const *sensor);

/* ---- Android logging (-> stderr) ---- */
int __android_log_print(int prio, const char *tag, const char *fmt, ...);
int __android_log_vprint(int prio, const char *tag, const char *fmt, __builtin_va_list ap);
int __android_log_write(int prio, const char *tag, const char *text);
int __android_log_buf_print(int bufID, int prio, const char *tag, const char *fmt, ...);
int __android_log_buf_write(int bufID, int prio, const char *tag, const char *text);

/* ---- Shim config ---- */
/* Base directory the AAsset* routines open files under (the port data dir,
   e.g. "/storage/roms/ports/ff5"). NULL/unset -> current directory. */
void android_shim_set_datadir(const char *dir);

#endif
