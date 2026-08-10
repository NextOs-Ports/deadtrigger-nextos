#ifndef PORT_WINDOW_TITLE
#define PORT_WINDOW_TITLE "Final Fantasy V"
#endif
/*
 * egl_shim.c -- EGL wrapper backed by SDL2 (OpenGL ES 2.0, Mali-450 fbdev)
 *
 * Each fake EGL context maps to a real SDL GL context. A bootstrap context is
 * kept as the share root so all contexts can share resources. On Mali-450
 * (Utgard, ES2-only) we announce ES2-only configs so Unity picks the ES2
 * backend, and we drive the REAL EGL objects the SDL2-mali driver created so
 * eglMakeCurrent can hand a context across threads ("Bully"-style).
 *
 * Callbacks into main (ff5_frame_end_present / ff5_gl_override) are declared
 * weak: egl_shim links standalone, main.c overrides them.
 */

#include <SDL2/SDL.h>
#include <GLES2/gl2.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "egl_shim.h"
#include "util.h"

#define DEFAULT_SCREEN_WIDTH 1280
#define DEFAULT_SCREEN_HEIGHT 720

/* main-provided callbacks (weak so this TU links on its own) */
__attribute__((weak)) void ff5_frame_end_present(void);
__attribute__((weak)) void *ff5_gl_override(const char *procname);

typedef struct {
  SDL_GLContext sdl_context;
  EGLBoolean is_pbuffer;
  int id;
  unsigned long owner_tid; /* thread that owns the SDL context (SDL-mali binds it to the creator) */
  void *real_ctx;          /* shared REAL EGL context (1 per Unity eglCreateContext) */
} _egl_context;

static SDL_Window *egl_window = NULL;
static SDL_GLContext egl_share_root = NULL;
static pthread_mutex_t egl_context_create_mutex = PTHREAD_MUTEX_INITIALIZER;

/* === BULLY APPROACH: use the REAL EGL objects the SDL2-mali created + the REAL
   eglMakeCurrent (via dlsym of libEGL). The SDL_GL_MakeCurrent wrapper binds the
   context to the creator thread -> EGL_BAD_ACCESS when Unity renders on another
   thread. The REAL eglMakeCurrent allows a cross-thread handoff (the other thread
   just releases). We capture dpy/surf/ctx after SDL creates them. === */
#include <dlfcn.h>
#define EGL_DRAW_ATTR 0x3059
static void *(*r_getCurDisplay)(void);
static void *(*r_getCurSurface)(int);
static void *(*r_getCurContext)(void);
static unsigned (*r_makeCurrent)(void*,void*,void*,void*);
static unsigned (*r_swapBuffers)(void*,void*);
static int (*r_getError)(void);
static void *(*r_createContext)(void*,void*,void*,const int*);
static unsigned (*r_chooseConfig)(void*,const int*,void*,int,int*);
static unsigned (*r_getConfigAttrib)(void*,void*,int,int*);
static void *(*r_createPbuffer)(void*,void*,const int*);
static void *g_real_dpy=NULL, *g_real_surf=NULL, *g_real_ctx=NULL, *g_real_cfg=NULL, *g_real_pbuf=NULL;
static int g_use_real_egl=0;
static unsigned long g_creator_tid=0; /* thread that creates the contexts (setup) -> uses PBUFFER;
                                         the RENDER thread (another) uses the WINDOW surface. */
static int frame_count = 0;
static int next_context_id = 1;
static int cached_width = 0;
static int cached_height = 0;
static int g_movie_blocks_present = 0;

static _Thread_local _egl_context *current_context = NULL;
static _Thread_local _egl_context *last_context = NULL;
static _Thread_local int has_real_gl = 0;

static int read_env_int(const char *name, int fallback, int min_value, int max_value) {
  const char *value = getenv(name);
  char *end = NULL;
  long parsed;
  if (!value || !value[0]) return fallback;
  parsed = strtol(value, &end, 10);
  if (!end || *end) return fallback;
  if (parsed < min_value) parsed = min_value;
  if (parsed > max_value) parsed = max_value;
  return (int)parsed;
}

static int screen_width(void) {
  if (!cached_width) cached_width = read_env_int("FF5_WIDTH", DEFAULT_SCREEN_WIDTH, 320, 1920);
  return cached_width;
}

static int screen_height(void) {
  if (!cached_height) cached_height = read_env_int("FF5_HEIGHT", DEFAULT_SCREEN_HEIGHT, 240, 1080);
  return cached_height;
}

int egl_shim_width(void) { return screen_width(); }
int egl_shim_height(void) { return screen_height(); }

static int real_current_is_window_surface(void) {
  if (!g_use_real_egl || !r_getCurSurface || !g_real_surf)
    return 0;
  return r_getCurSurface(EGL_DRAW_ATTR) == g_real_surf;
}

SDL_Window *egl_shim_get_window(void) { return egl_window; }

void egl_shim_movie_begin(void) {
  if (!__atomic_exchange_n(&g_movie_blocks_present, 1, __ATOMIC_ACQ_REL))
    debugPrintf("egl_shim: movie owns framebuffer; GL present blocked\n");
}

void egl_shim_movie_end(void) {
  if (__atomic_exchange_n(&g_movie_blocks_present, 0, __ATOMIC_ACQ_REL))
    debugPrintf("egl_shim: movie released framebuffer; GL present restored\n");
}

int egl_shim_movie_active(void) {
  return __atomic_load_n(&g_movie_blocks_present, __ATOMIC_ACQUIRE) != 0;
}

void egl_shim_create_window(void) {
  /* Portable resolution: if FF5_WIDTH/HEIGHT are not pinned, use the NATIVE
     display resolution (SDL_GetDesktopDisplayMode) -> 480p/720p/1080p with no
     hardcode. setenv BEFORE any screen_width() so egl_shim and main.c agree. */
  if (!getenv("FF5_WIDTH") || !getenv("FF5_HEIGHT")) {
    SDL_DisplayMode dm;
    if (SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0) {
      char b[16];
      snprintf(b, sizeof b, "%d", dm.w); setenv("FF5_WIDTH", b, 1);
      snprintf(b, sizeof b, "%d", dm.h); setenv("FF5_HEIGHT", b, 1);
      debugPrintf("egl_shim: [AUTO] native display %dx%d -> FF5_WIDTH/HEIGHT\n", dm.w, dm.h);
    }
  }
  int width = screen_width();
  int height = screen_height();
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

  egl_window = SDL_CreateWindow(
      PORT_WINDOW_TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
      width, height,
      SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN);
  if (!egl_window) {
    debugPrintf("egl_shim: SDL_CreateWindow FAILED: %s\n", SDL_GetError());
    return;
  }
  debugPrintf("egl_shim: Window created %dx%d\n", width, height);

  egl_share_root = SDL_GL_CreateContext(egl_window);
  if (!egl_share_root) {
    debugPrintf("egl_shim: SDL_GL_CreateContext FAILED: %s\n", SDL_GetError());
    return;
  }
  debugPrintf("egl_shim: GL share-root context created\n");

  /* capture the REAL EGL objects the SDL2-mali created (current = share_root now) */
  r_getCurDisplay=dlsym(RTLD_DEFAULT,"eglGetCurrentDisplay");
  r_getCurSurface=dlsym(RTLD_DEFAULT,"eglGetCurrentSurface");
  r_getCurContext=dlsym(RTLD_DEFAULT,"eglGetCurrentContext");
  r_makeCurrent  =dlsym(RTLD_DEFAULT,"eglMakeCurrent");
  r_swapBuffers  =dlsym(RTLD_DEFAULT,"eglSwapBuffers");
  r_getError     =dlsym(RTLD_DEFAULT,"eglGetError");
  r_createContext=dlsym(RTLD_DEFAULT,"eglCreateContext");
  r_chooseConfig =dlsym(RTLD_DEFAULT,"eglChooseConfig");
  r_getConfigAttrib=dlsym(RTLD_DEFAULT,"eglGetConfigAttrib");
  unsigned (*r_querySurface)(void*,void*,int,int*)=dlsym(RTLD_DEFAULT,"eglQuerySurface");
  if(r_getCurDisplay&&r_getCurSurface&&r_getCurContext&&r_makeCurrent&&r_createContext&&r_chooseConfig&&r_querySurface){
    g_real_dpy=r_getCurDisplay(); g_real_surf=r_getCurSurface(EGL_DRAW_ATTR); g_real_ctx=r_getCurContext();
    /* DIAG: real mali-fbdev surface size (decisive for the zoom/fullscreen bug) */
    { int rw=-1,rh=-1; r_querySurface(g_real_dpy,g_real_surf,0x3057/*WIDTH*/,&rw);
      r_querySurface(g_real_dpy,g_real_surf,0x3056/*HEIGHT*/,&rh);
      int dw=0,dh=0; SDL_GL_GetDrawableSize(egl_window,&dw,&dh);
      int mts=0,mrb=0,mvp[2]={0,0};
      void (*r_getiv)(unsigned,int*)=dlsym(RTLD_DEFAULT,"glGetIntegerv");
      if(r_getiv){ r_getiv(0x0D33,&mts); r_getiv(0x84E8,&mrb); r_getiv(0x0D3A,mvp); }
      debugPrintf("egl_shim: [DIAG] REAL surface=%dx%d  SDL drawable=%dx%d  requested=%dx%d  GL_MAX_TEX=%d MAX_RB=%d MAX_VP=%dx%d\n",
                  rw,rh,dw,dh,width,height,mts,mrb,mvp[0],mvp[1]); }
    /* EXACT config of the SDL surface (via CONFIG_ID) -> shared contexts match the
       surface (else eglMakeCurrent = EGL_BAD_MATCH 0x3009). */
    int cfgid=0, n=0; r_querySurface(g_real_dpy, g_real_surf, 0x3028 /*EGL_CONFIG_ID*/, &cfgid);
    int cfgattr[]={0x3028 /*EGL_CONFIG_ID*/, cfgid, 0x3038 /*EGL_NONE*/};
    r_chooseConfig(g_real_dpy, cfgattr, &g_real_cfg, 1, &n);
    debugPrintf("egl_shim: surface CONFIG_ID=%d cfg=%p n=%d\n", cfgid, g_real_cfg, n);
    /* real PBUFFER for the setup thread (so it doesn't hold the window surface) */
    r_createPbuffer=dlsym(RTLD_DEFAULT,"eglCreatePbufferSurface");
    if(r_createPbuffer && g_real_cfg){ int pb[]={0x3057/*WIDTH*/,16, 0x3056/*HEIGHT*/,16, 0x3038};
      g_real_pbuf=r_createPbuffer(g_real_dpy, g_real_cfg, pb);
      debugPrintf("egl_shim: pbuffer=%p (err=0x%x)\n", g_real_pbuf, r_getError?r_getError():0); }
    if(g_real_dpy&&g_real_surf&&g_real_ctx&&g_real_cfg&&n>0){ g_use_real_egl=1;
      debugPrintf("egl_shim: REAL EGL dpy=%p surf=%p ctx=%p cfg=%p (Bully-style, 1 ctx/thread)\n",
                  g_real_dpy,g_real_surf,g_real_ctx,g_real_cfg); }
    /* EGL_BUFFER_PRESERVED: on Mali fbdev double-buffer (page0/page1), the back
       buffer is DISCARDED on swap -> Unity's composite-draw into a recycled buffer
       does not survive (tile does not resolve) -> black after the menu. Preserving
       the back buffer on swap makes the composite reach the scanout. */
    if(!getenv("FF5_NO_PRESERVE")){
      unsigned (*r_surfAttrib)(void*,void*,int,int)=dlsym(RTLD_DEFAULT,"eglSurfaceAttrib");
      if(r_surfAttrib){
        unsigned ok=r_surfAttrib(g_real_dpy,g_real_surf,0x3093 /*EGL_SWAP_BEHAVIOR*/,0x3094 /*EGL_BUFFER_PRESERVED*/);
        debugPrintf("egl_shim: eglSurfaceAttrib(SWAP_BEHAVIOR=PRESERVED) -> %u err=0x%x\n",ok,r_getError?r_getError():0);
      }
    }
  }
  if(!g_use_real_egl) debugPrintf("egl_shim: REAL EGL unavailable -> SDL fallback\n");

  SDL_GL_MakeCurrent(egl_window, NULL); /* release -> any thread can do a REAL eglMakeCurrent */
  debugPrintf("egl_shim: Context released, ready for game\n");
}

/* --- Mutex hooks (called from imports.c pthread wrappers) --- */

void egl_shim_on_mutex_post_lock(void *mutex_id) {
  (void)mutex_id;
}

void egl_shim_on_mutex_pre_unlock(void *mutex_id) {
  (void)mutex_id;
}

int egl_shim_ensure_current(void) {
  if (has_real_gl)
    return 1;
  _egl_context *ctx = current_context ? current_context : last_context;
  if (!egl_window || !ctx || !ctx->sdl_context)
    return 0;

  int ret = SDL_GL_MakeCurrent(egl_window, ctx->sdl_context);
  if (ret == 0) {
    has_real_gl = 1;
    current_context = ctx;
    debugPrintf("egl_shim: restored current context [tid=%lx] [ctx_id=%d]\n",
                (unsigned long)pthread_self(), ctx->id);
    return 1;
  }

  debugPrintf("egl_shim: failed to restore current context [tid=%lx] [ctx_id=%d]: %s\n",
              (unsigned long)pthread_self(), ctx->id, SDL_GetError());
  return 0;
}

/* --- EGL API --- */

EGLDisplay egl_shim_GetDisplay(EGLNativeDisplayType display_id) {
  (void)display_id;
  debugPrintf("egl_shim: eglGetDisplay() [tid=%lx]\n",(unsigned long)pthread_self());
  return (EGLDisplay)strdup("display");
}

EGLBoolean egl_shim_Initialize(EGLDisplay dpy, EGLint *major, EGLint *minor) {
  (void)dpy;
  if (major) *major = 1;
  if (minor) *minor = 4;
  debugPrintf("egl_shim: eglInitialize() -> 1.4\n");
  return EGL_TRUE;
}

EGLBoolean egl_shim_Terminate(EGLDisplay dpy) {
  (void)dpy;
  debugPrintf("egl_shim: eglTerminate()\n");
  if (egl_share_root) {
    SDL_GL_DeleteContext(egl_share_root);
    egl_share_root = NULL;
  }
  if (egl_window) {
    SDL_DestroyWindow(egl_window);
    egl_window = NULL;
  }
  return EGL_TRUE;
}

EGLBoolean egl_shim_ChooseConfig(EGLDisplay dpy, const EGLint *attrib_list,
                                  EGLConfig *configs, EGLint config_size,
                                  EGLint *num_config) {
  (void)dpy; (void)attrib_list;
  /* Mesma regra do Terraria aprovado: a Unity deve enxergar a config que o
     SDL/Mali realmente criou. Atributos inventados aqui mudam o formato que a
     engine usa para montar os buffers, embora o driver continue com outro. */
  if (configs && config_size > 0)
    configs[0] = g_real_cfg ? (EGLConfig)g_real_cfg
                            : (EGLConfig)(uintptr_t)0xC0F00;
  if (num_config) *num_config = 1;
  return EGL_TRUE;
}

EGLSurface egl_shim_CreateWindowSurface(EGLDisplay dpy, EGLConfig config,
                                         EGLNativeWindowType win,
                                         const EGLint *attrib_list) {
  (void)dpy; (void)config; (void)win; (void)attrib_list;
  EGLSurface s = (EGLSurface)strdup("window");
  debugPrintf("egl_shim: eglCreateWindowSurface() -> %p\n", s);
  return s;
}

EGLSurface egl_shim_CreatePbufferSurface(EGLDisplay dpy, EGLConfig config,
                                          const EGLint *attrib_list) {
  (void)dpy; (void)config; (void)attrib_list;
  EGLSurface s = (EGLSurface)strdup("pbuffer");
  debugPrintf("egl_shim: eglCreatePbufferSurface() -> %p\n", s);
  return s;
}

EGLContext egl_shim_CreateContext(EGLDisplay dpy, EGLConfig config,
                                  EGLContext share_context,
                                  const EGLint *attrib_list) {
  (void)dpy; (void)config; (void)share_context; (void)attrib_list;
  _egl_context *c = (_egl_context *)calloc(1, sizeof(_egl_context));
  if (!c)
    return EGL_NO_CONTEXT;
  /* Do NOT create the SDL/EGL context here: Unity creates contexts on the MAIN
     thread but RENDERS on a worker -> a context bound to main -> eglMakeCurrent
     on the worker = EGL_BAD_ACCESS. We defer creation to the 1st MakeCurrent, on
     the render thread itself (sdl_context=NULL). */
  c->sdl_context = NULL;
  c->id = next_context_id++;
  if (!g_creator_tid) g_creator_tid = (unsigned long)pthread_self(); /* 1st CreateContext = setup thread */
  if (g_use_real_egl && r_createContext) {
    int ctxattr[] = {0x3098 /*EGL_CONTEXT_CLIENT_VERSION*/, 2, 0x3038 /*EGL_NONE*/};
    c->real_ctx = r_createContext(g_real_dpy, g_real_cfg, g_real_ctx /*share*/, ctxattr);
    debugPrintf("egl_shim: eglCreateContext -> ctx_id=%d real=%p [tid=%lx]\n",
                c->id, c->real_ctx, (unsigned long)pthread_self());
    if (!c->real_ctx) c->real_ctx = g_real_ctx; /* fallback: use the share-root */
  }
  return (EGLContext)c;
}

EGLBoolean egl_shim_MakeCurrent(EGLDisplay dpy, EGLSurface draw,
                                 EGLSurface read, EGLContext ctx) {
  (void)dpy; (void)read;

  _egl_context *context = (_egl_context *)ctx;
  static _Thread_local int mc_count = 0;
  int mc = ++mc_count;

  /* === BULLY-STYLE: REAL eglMakeCurrent (cross-thread OK). bind: (dpy,surf,surf,ctx); unbind: 0s. */
  if (g_use_real_egl) {
    unsigned r;
    if (context == NULL || draw == NULL) {
      /* FRAME-END: the render thread is RELEASING the window -> end of its frame,
         FBO0 (window) holds Unity's FINAL frame. Present NOW while the window is
         current -> capture the last-rendered scene. */
      if (real_current_is_window_surface()) {
        if (ff5_frame_end_present) ff5_frame_end_present();
      }
      r = r_makeCurrent(g_real_dpy, NULL, NULL, NULL);
      current_context = NULL; has_real_gl = 0;
    } else {
      current_context = context; last_context = context;
      unsigned long me=(unsigned long)pthread_self();
      /* O Unity mantem setup e render concorrentes. Como o EGL Utgard recusa o
         mesmo contexto current em duas threads (EGL_BAD_ACCESS), materialize
         um contexto real compartilhado por thread para representar o unico
         handle do guest. Estados nao compartilhados que o guest ja configurou
         no setup precisam ser reproduzidos quando esse contexto nasce. */
      static _Thread_local void *tl_ctx=NULL;
      static _Thread_local int tl_ctx_needs_base_state=0;
      if (!tl_ctx) {
        int ctxattr[]={0x3098,2,0x3038};
        tl_ctx = r_createContext(g_real_dpy, g_real_cfg, g_real_ctx, ctxattr);
        if(!tl_ctx) tl_ctx = (context->real_ctx?context->real_ctx:g_real_ctx);
        tl_ctx_needs_base_state = 1;
      }
      void *surf = (me==g_creator_tid && g_real_pbuf) ? g_real_pbuf : g_real_surf;
      r = r_makeCurrent(g_real_dpy, surf, surf, tl_ctx);
      if (r == 0 && surf!=g_real_surf) { r = r_makeCurrent(g_real_dpy, g_real_surf, g_real_surf, tl_ctx); surf=g_real_surf; }
      has_real_gl = (r != 0);
      if (r && tl_ctx_needs_base_state) {
        /* Unity definiu ambos como 1 no contexto de setup. Sem espelhar isso,
           RGB8 de largura impar usa stride 4 no contexto sintetico e o Mali
           le alem do buffer (219x1125 foi o primeiro caso real). */
        glPixelStorei(0x0D05 /*GL_PACK_ALIGNMENT*/, 1);
        glPixelStorei(0x0CF5 /*GL_UNPACK_ALIGNMENT*/, 1);
        tl_ctx_needs_base_state = 0;
      }
      static _Thread_local int lg=0;
      if (lg < 10) { debugPrintf("egl_shim: REAL MakeCurrent %s [tid=%lx] %s tl_ctx=%p err=0x%x\n",
            r?"OK":"FAIL", me, surf==g_real_pbuf?"PBUF":"WIN", tl_ctx, r?0:(r_getError?r_getError():0)); lg++; }
    }
    return EGL_TRUE;
  }

  /* === UNBIND (SDL fallback) === */
  if (context == NULL || draw == NULL) {
    current_context = NULL;
    if (egl_window) {
      SDL_GL_MakeCurrent(egl_window, NULL);
    }
    has_real_gl = 0;
    return EGL_TRUE;
  }

  int is_window = (((char *)draw)[0] == 'w');
  context->is_pbuffer = is_window ? EGL_FALSE : EGL_TRUE;
  current_context = context;
  last_context = context;

  if (!egl_window)
    return EGL_TRUE;

  /* SDL-mali binds the SDL context to its creator thread. Unity creates the
     context on MAIN and RENDERS on a gfx thread -> we must MIGRATE: if the
     context does not exist OR belongs to another thread, (re)create it on this
     thread. The sustained render thread ends up owning it. */
  unsigned long me = (unsigned long)pthread_self();
  if (!context->sdl_context || context->owner_tid != me) {
    pthread_mutex_lock(&egl_context_create_mutex);
    if (context->sdl_context) { SDL_GL_DeleteContext(context->sdl_context); context->sdl_context = NULL; }
    SDL_GL_MakeCurrent(egl_window, NULL);
    context->sdl_context = SDL_GL_CreateContext(egl_window);
    context->owner_tid = me;
    pthread_mutex_unlock(&egl_context_create_mutex);
    debugPrintf("egl_shim: (re)created SDL context for ctx_id=%d [tid=%lx] -> %p\n",
                context->id, me, context->sdl_context);
    if (!context->sdl_context) { debugPrintf("egl_shim: SDL_GL_CreateContext FAILED: %s\n", SDL_GetError()); return EGL_TRUE; }
  }

  int ret = SDL_GL_MakeCurrent(egl_window, context->sdl_context);
  if (ret == 0) {
    has_real_gl = 1;
    static _Thread_local int acq_log = 0;
    if (acq_log < 20 || mc % 500 == 0) {
      acq_log++;
    }
  } else {
    has_real_gl = 0;
    debugPrintf("egl_shim: MakeCurrent #%d %s [tid=%lx] SDL FAILED [ctx_id=%d]: %s\n",
                mc, is_window ? "WINDOW" : "PBUFFER",
                (unsigned long)pthread_self(), context->id, SDL_GetError());
  }

  return EGL_TRUE;
}

EGLBoolean egl_shim_SwapBuffers(EGLDisplay dpy, EGLSurface surface) {
  (void)dpy; (void)surface;
  if (__atomic_load_n(&g_movie_blocks_present, __ATOMIC_ACQUIRE)) return EGL_TRUE;
  if (g_use_real_egl) {
    if (r_swapBuffers && g_real_dpy && g_real_surf && real_current_is_window_surface()) {
      r_swapBuffers(g_real_dpy, g_real_surf);
      int fc = ++frame_count; static int sl=0;
      if (sl < 8) { debugPrintf("egl_shim: REAL SwapBuffers #%d [tid=%lx]\n", fc, (unsigned long)pthread_self()); sl++; }
    } else {
      static _Thread_local int skip_log = 0;
      if (skip_log < 8) {
        debugPrintf("egl_shim: REAL SwapBuffers SKIPPED [tid=%lx] cur=%p want=%p\n",
                    (unsigned long)pthread_self(),
                    r_getCurSurface ? r_getCurSurface(EGL_DRAW_ATTR) : NULL, g_real_surf);
        skip_log++;
      }
    }
    return EGL_TRUE;
  }
  if (!egl_window) return EGL_TRUE;

  if (has_real_gl && current_context && !current_context->is_pbuffer) {
    SDL_GL_SwapWindow(egl_window);
    ++frame_count;
  } else {
    static int noswap_log = 0;
    if (noswap_log < 3) {
      debugPrintf("egl_shim: SwapBuffers SKIPPED (no real GL) [tid=%lx]\n",
                  (unsigned long)pthread_self());
      noswap_log++;
    }
  }
  return EGL_TRUE;
}

void egl_shim_force_present(const char *reason) {
  if (__atomic_load_n(&g_movie_blocks_present, __ATOMIC_ACQUIRE)) return;
  if (g_use_real_egl) {
    if (r_swapBuffers && g_real_dpy && g_real_surf && real_current_is_window_surface()) {
      r_swapBuffers(g_real_dpy, g_real_surf);
      int fc = ++frame_count;
      static _Thread_local int sl=0;
      if (sl < 12) {
        debugPrintf("egl_shim: FORCE SwapBuffers #%d [tid=%lx] %s\n",
                    fc, (unsigned long)pthread_self(), reason ? reason : "?");
        sl++;
      }
    } else if (getenv("FF5_SWAP_ANY") && r_swapBuffers && g_real_dpy && g_real_surf) {
      /* Diagnostico Mali-fbdev: esta Unity mantem a window current na gfx
         worker entre frames. Verifica se o driver permite apresentar essa
         surface pela UnityMain sem rebind nem alteracao do contexto. */
      unsigned ok = r_swapBuffers(g_real_dpy, g_real_surf);
      int err = r_getError ? r_getError() : 0;
      int fc = ok ? ++frame_count : frame_count;
      static _Thread_local int any_log = 0;
      if (any_log < 20) {
        debugPrintf("egl_shim: CROSS-THREAD SwapBuffers %s #%d [tid=%lx] cur=%p err=0x%x\n",
                    ok ? "OK" : "FAIL", fc, (unsigned long)pthread_self(),
                    r_getCurSurface ? r_getCurSurface(EGL_DRAW_ATTR) : NULL, err);
        any_log++;
      }
    } else {
      static _Thread_local int skip_log = 0;
      if (skip_log < 12) {
        debugPrintf("egl_shim: FORCE SwapBuffers SKIPPED [tid=%lx] %s cur=%p want=%p\n",
                    (unsigned long)pthread_self(), reason ? reason : "?",
                    r_getCurSurface ? r_getCurSurface(EGL_DRAW_ATTR) : NULL, g_real_surf);
        skip_log++;
      }
    }
    return;
  }

  if (egl_window && has_real_gl && current_context && !current_context->is_pbuffer) {
    SDL_GL_SwapWindow(egl_window);
    int fc = ++frame_count;
    static _Thread_local int sl=0;
    if (sl < 12) {
      debugPrintf("egl_shim: FORCE SDL swap #%d [tid=%lx] %s\n",
                  fc, (unsigned long)pthread_self(), reason ? reason : "?");
      sl++;
    }
  }
}

EGLBoolean egl_shim_DestroySurface(EGLDisplay dpy, EGLSurface surface) {
  (void)dpy;
  free(surface);
  return EGL_TRUE;
}

EGLBoolean egl_shim_DestroyContext(EGLDisplay dpy, EGLContext ctx) {
  (void)dpy;
  _egl_context *context = (_egl_context *)ctx;
  if (context) {
    if (context->sdl_context)
      SDL_GL_DeleteContext(context->sdl_context);
    free(context);
  }
  return EGL_TRUE;
}

EGLBoolean egl_shim_QuerySurface(EGLDisplay dpy, EGLSurface surface,
                                  EGLint attribute, EGLint *value) {
  (void)dpy; (void)surface;
  if (attribute == 0x3057 && value) *value = screen_width();
  else if (attribute == 0x3056 && value) *value = screen_height();
  return EGL_TRUE;
}

EGLBoolean egl_shim_GetConfigAttrib(EGLDisplay dpy, EGLConfig config,
                                     EGLint attribute, EGLint *value) {
  (void)dpy; (void)config;
  if (!value) return EGL_TRUE;
  if (g_real_dpy && g_real_cfg && r_getConfigAttrib &&
      r_getConfigAttrib(g_real_dpy, g_real_cfg, attribute, value))
    return EGL_TRUE;
  /* Fallback coerente RGB888/D24/S8 caso o EGL real nao exponha a consulta. */
  switch (attribute) {
  case 0x3020: *value = 24; break;          /* EGL_BUFFER_SIZE */
  case 0x3021: *value = 0; break;           /* EGL_ALPHA_SIZE */
  case 0x3022: *value = 8; break;           /* EGL_BLUE_SIZE */
  case 0x3023: *value = 8; break;           /* EGL_GREEN_SIZE */
  case 0x3024: *value = 8; break;           /* EGL_RED_SIZE */
  case 0x3025: *value = 24; break;          /* EGL_DEPTH_SIZE */
  case 0x3026: *value = 8; break;           /* EGL_STENCIL_SIZE */
  case 0x3027: *value = 0x3038; break;     /* EGL_CONFIG_CAVEAT = EGL_NONE */
  case 0x3028: *value = 1; break;           /* EGL_CONFIG_ID */
  case 0x3031: *value = 0; break;          /* EGL_SAMPLE_BUFFERS */
  case 0x3032: *value = 0; break;          /* EGL_SAMPLES */
  case 0x3033: *value = 0x0005; break;     /* EGL_SURFACE_TYPE WINDOW|PBUFFER */
  case 0x3040: *value = 0x0004; break;     /* EGL_RENDERABLE_TYPE ES2 */
  case 0x3039: *value = 0; break;          /* EGL_BIND_TO_TEXTURE_RGB */
  case 0x303A: *value = 0; break;          /* EGL_BIND_TO_TEXTURE_RGBA */
  case 0x3042: *value = 0x0004; break;     /* EGL_CONFORMANT = ES2 */
  case 0x302E: *value = 0; break;          /* EGL_NATIVE_VISUAL_ID */
  case 0x3034: *value = 0; break;          /* EGL_TRANSPARENT_TYPE = EGL_NONE */
  default: *value = 0; break;
  }
  return EGL_TRUE;
}

EGLint egl_shim_GetError(void) { return EGL_SUCCESS; }

static void *egl_shim_lookup_egl_proc(const char *procname) {
  if (!procname || procname[0] != 'e' || procname[1] != 'g' || procname[2] != 'l') return NULL;
  if (!strcmp(procname, "eglGetDisplay")) return (void *)egl_shim_GetDisplay;
  if (!strcmp(procname, "eglInitialize")) return (void *)egl_shim_Initialize;
  if (!strcmp(procname, "eglTerminate")) return (void *)egl_shim_Terminate;
  if (!strcmp(procname, "eglChooseConfig")) return (void *)egl_shim_ChooseConfig;
  if (!strcmp(procname, "eglGetConfigAttrib")) return (void *)egl_shim_GetConfigAttrib;
  if (!strcmp(procname, "eglCreateWindowSurface")) return (void *)egl_shim_CreateWindowSurface;
  if (!strcmp(procname, "eglCreatePbufferSurface")) return (void *)egl_shim_CreatePbufferSurface;
  if (!strcmp(procname, "eglDestroySurface")) return (void *)egl_shim_DestroySurface;
  if (!strcmp(procname, "eglCreateContext")) return (void *)egl_shim_CreateContext;
  if (!strcmp(procname, "eglDestroyContext")) return (void *)egl_shim_DestroyContext;
  if (!strcmp(procname, "eglMakeCurrent")) return (void *)egl_shim_MakeCurrent;
  if (!strcmp(procname, "eglSwapBuffers")) return (void *)egl_shim_SwapBuffers;
  if (!strcmp(procname, "eglSwapInterval")) return (void *)egl_shim_SwapInterval;
  if (!strcmp(procname, "eglGetCurrentContext")) return (void *)egl_shim_GetCurrentContext;
  if (!strcmp(procname, "eglGetCurrentSurface")) return (void *)egl_shim_GetCurrentSurface;
  if (!strcmp(procname, "eglGetError")) return (void *)egl_shim_GetError;
  if (!strcmp(procname, "eglBindAPI")) return (void *)egl_shim_BindAPI;
  if (!strcmp(procname, "eglQueryString")) return (void *)egl_shim_QueryString;
  if (!strcmp(procname, "eglQuerySurface")) return (void *)egl_shim_QuerySurface;
  return NULL;
}

void *egl_shim_GetProcAddress(const char *procname) {
  void *ptr = egl_shim_lookup_egl_proc(procname);
  if (ptr) {
    debugPrintf("egl_shim: eglGetProcAddress(%s) -> shim\n", procname);
    return ptr;
  }

  if (ff5_gl_override) {
    ptr = ff5_gl_override(procname);
    if (ptr) {
      debugPrintf("egl_shim: eglGetProcAddress(%s) -> gl-override\n", procname);
      return ptr;
    }
  }

  ptr = SDL_GL_GetProcAddress(procname);
  if (ptr) return ptr;

  size_t len = strlen(procname);
  if (len > 3 && strcmp(procname + len - 3, "OES") == 0) {
    char stripped[256];
    if (len - 3 < sizeof(stripped)) {
      memcpy(stripped, procname, len - 3);
      stripped[len - 3] = '\0';
      ptr = SDL_GL_GetProcAddress(stripped);
      if (ptr) return ptr;
    }
  }

  debugPrintf("egl_shim: eglGetProcAddress(%s) -> NOT FOUND\n", procname);
  return NULL;
}

EGLBoolean egl_shim_BindAPI(unsigned int api) {
  (void)api;
  return EGL_TRUE;
}

const char *egl_shim_QueryString(EGLDisplay dpy, EGLint name) {
  (void)dpy;
  switch (name) {
  case 0x3053: return "NextOS";      /* EGL_VENDOR */
  case 0x3054: return "1.4 NextOS";  /* EGL_VERSION */
  case 0x3055: return "";            /* EGL_EXTENSIONS */
  case 0x308D: return "OpenGL_ES";   /* EGL_CLIENT_APIS */
  default: return "";
  }
}

EGLBoolean egl_shim_SwapInterval(EGLDisplay dpy, EGLint interval) {
  (void)dpy;
  SDL_GL_SetSwapInterval(interval);
  return EGL_TRUE;
}

EGLContext egl_shim_GetCurrentContext(void) {
  return (EGLContext)current_context;
}

EGLSurface egl_shim_GetCurrentSurface(EGLint readdraw) {
  (void)readdraw;
  return (EGLSurface)"window";
}

EGLBoolean egl_shim_SurfaceAttrib(EGLDisplay dpy, EGLSurface s, EGLint a,
                                  EGLint v) {
  (void)dpy; (void)s; (void)a; (void)v;
  return EGL_TRUE;
}
