#define _GNU_SOURCE 1
/*
 * falsojni.c — implementacao dos JNIEnv/JavaVM falsos do FF V.
 * Cobre so o surface de boot do UnityPlayer + Play Asset Delivery + prefs.
 * jclass/jmethodID/jfieldID = strdup(nome); Call*Method casa pelo nome.
 * jstring = struct { char *utf } (NewStringUTF/GetStringUTFChars reais).
 * Metodos-File (getFilesDir/getAbsolutePath) devolvem jstring do caminho.
 */
#include "falsojni.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

// ---- estado -------------------------------------------------------------
static char g_pkg[128];
static char g_gamedir[512];
static char g_userdata[512];

// vtables como arrays de ponteiros (indices da JNINativeInterface do Android).
static void *env_vt[300];
static void *env_vtp = env_vt;
static void *g_env = &env_vtp;

static void *vm_vt[16];
static void *vm_vtp = vm_vt;
static void *g_vm = &vm_vtp;

// objeto sentinela p/ "this"/Activity/Context.
static int g_activity_obj = 0xAC;

/* bitter.jnibridge.JNIBridge.newInterfaceProxy guarda um ponteiro nativo em
 * seu InvocationHandler. Sem JVM, preservamos exatamente esse handle e o
 * devolvemos ao native JNIBridge.invoke quando Runnable.run for executado. */
#define PROXY_MAGIC 0x4a4e4950u /* "JNIP" */
typedef enum fake_proxy_kind {
    PROXY_JNI_BRIDGE,
    PROXY_REFLECTION_HELPER,
} fake_proxy_kind;

typedef struct fake_proxy {
    uint32_t magic;
    fake_proxy_kind kind;
    jlong handle;
} fake_proxy;

typedef struct ui_task {
    fake_proxy *proxy;
    uint64_t due_ms;
} ui_task;

static pthread_t g_ui_thread;
static pthread_mutex_t g_ui_lock = PTHREAD_MUTEX_INITIALIZER;
static ui_task g_ui_queue[256];
static unsigned g_ui_head, g_ui_tail;

/* Objetos minimos do Looper Android usado pelo UnityChoreographer. Eles sao
 * distintos porque o fluxo depende de Message.sendToTarget ser entregue ao
 * Handler.Callback correto; uma sentinela unica para toda a JVM perde essa
 * relacao e deixa a Unity bloqueada antes do primeiro frame. */
typedef enum fake_java_type {
    FJ_HANDLER_THREAD,
    FJ_HANDLER,
    FJ_MESSAGE,
    FJ_CHOREOGRAPHER,
    FJ_BOXED_LONG,
    FJ_LVL_SERVICE_BINDER,
    FJ_INPUT_MANAGER,
    FJ_INPUT_DEVICE,
    FJ_MOTION_RANGE,
    FJ_MOTION_RANGE_LIST,
    FJ_KEY_EVENT,
    FJ_MOTION_EVENT,
} fake_java_type;

typedef struct fake_java_object {
    fake_java_type type;
    jint what;
} fake_java_object;

static fake_java_object g_handler_thread_obj = { FJ_HANDLER_THREAD, 0 };
static fake_java_object g_handler_obj = { FJ_HANDLER, 0 };
static fake_java_object g_message_obj = { FJ_MESSAGE, 0 };
static fake_java_object g_choreographer_obj = { FJ_CHOREOGRAPHER, 0 };
static fake_java_object g_boxed_long_obj = { FJ_BOXED_LONG, 0 };

/* com.unity3d.plugin.lvl.ServiceBinder e' instanciado pelo AndroidJavaObject
 * via ReflectionHelper. Ele precisa ter identidade propria: usar a sentinela
 * generica da Activity fazia GetObjectClass/FromReflectedMethod perderem a
 * classe e, portanto, ate o nome do metodo create(). */
typedef struct fake_lvl_service_binder {
    fake_java_type type;
    jint response_code;
    jobject signed_data;
    jobject signature;
    fake_proxy *done;
    jint nonce;
} fake_lvl_service_binder;

static fake_lvl_service_binder g_lvl_service_binder = {
    FJ_LVL_SERVICE_BINDER, 0, NULL, NULL, NULL, 0
};

/* JNI Call*MethodA recebe um vetor de unions de 8 bytes. Tratar o ponteiro
 * como va_list lia alternadamente o valor e a metade alta zero, deslocando
 * todos os argumentos da ReflectionHelper. */
typedef union fake_jvalue {
    jboolean z;
    jbyte b;
    uint16_t c;
    int16_t s;
    jint i;
    jlong j;
    jfloat f;
    jdouble d;
    jobject l;
} fake_jvalue;

static int g_handlemessage_args;
static int g_doframe_args;
static fake_proxy *g_choreographer_proxy;
static fake_proxy *g_handler_callback;
static _Thread_local int g_next_proxy_is_choreographer;
static jlong g_frame_time_nanos;

static pthread_mutex_t g_handler_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_handler_cond = PTHREAD_COND_INITIALIZER;
static pthread_t g_handler_thread;
static int g_handler_started;
static int g_handler_message_pending;
static int g_handler_frame_pending;
static uint64_t g_handler_frame_due_ms;

/* Uma JVM pode devolver referencias JNI diferentes para a mesma Class, mas
 * IsSameObject reconhece que elas representam o mesmo objeto; jmethodID, por
 * sua vez, e estavel. O bridge de proxies da Unity depende dessas identidades
 * para escolher a interface/metodo do callback. */
typedef struct interned_id {
    char *key;
    char *name;
    char *sig;
} interned_id;

static interned_id g_classes[256];
static interned_id g_methods[512];
static unsigned g_nclasses, g_nmethods;

static void *intern_id(interned_id *ids, unsigned *count, unsigned capacity,
                       const char *key, const char *name, const char *sig) {
    for (unsigned i = 0; i < *count; i++)
        if (!strcmp(ids[i].key, key)) return ids[i].name;
    if (*count >= capacity) return strdup(name);
    ids[*count].key = strdup(key);
    ids[*count].name = strdup(name);
    ids[*count].sig = sig ? strdup(sig) : NULL;
    return ids[(*count)++].name;
}

static jclass intern_class(const char *name) {
    if (!name) name = "?";
    return (jclass)intern_id(g_classes, &g_nclasses,
                            sizeof g_classes / sizeof g_classes[0],
                            name, name, NULL);
}

static jmethodID intern_method(jclass klass, const char *name,
                               const char *sig) {
    char key[768];
    if (!name) name = "?";
    snprintf(key, sizeof key, "%s|%s|%s", klass ? (const char *)klass : "?",
             name, sig ? sig : "?");
    return (jmethodID)intern_id(g_methods, &g_nmethods,
                               sizeof g_methods / sizeof g_methods[0],
                               key, name, sig);
}

static const char *method_name(jmethodID method) {
    for (unsigned i = 0; i < g_nmethods; i++)
        if ((jmethodID)g_methods[i].name == method) return g_methods[i].name;
    return method ? (const char *)method : NULL;
}

static const char *method_signature(jmethodID method) {
    for (unsigned i = 0; i < g_nmethods; i++)
        if ((jmethodID)g_methods[i].name == method) return g_methods[i].sig;
    return NULL;
}

static uint64_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static jstring make_jstring(const char *s);

static int is_proxy(jobject o) {
    return o && ((const fake_proxy *)o)->magic == PROXY_MAGIC;
}

static void invoke_proxy_method(fake_proxy *p, const char *class_name,
                                const char *method_name_, const char *signature,
                                jarray args) {
    if (!p || p->magic != PROXY_MAGIC || !p->handle) return;
    const char *native_name = p->kind == PROXY_REFLECTION_HELPER
                            ? "nativeProxyInvoke" : "invoke";
    uintptr_t invoke = jni_find_native(native_name);
    if (!invoke) {
        fprintf(stderr, "[jni-ui] %s ainda nao registrado\n", native_name);
        return;
    }
    int trace_callback = getenv("FF5_JNI_TRACE") &&
                         (strcmp(method_name_, "doFrame") ||
                          getenv("FF5_JNI_TRACE_FRAMES"));
    if (p->kind == PROXY_REFLECTION_HELPER) {
        if (trace_callback)
            fprintf(stderr,
                    "[jni-ui] nativeProxyInvoke %s handle=0x%llx\n",
                    method_name_, (unsigned long long)p->handle);
        ((jobject (*)(void *, jclass, jlong, jstring, jarray))invoke)(
            g_env, NULL, p->handle, make_jstring(method_name_), args);
    } else {
        jclass callback_class = intern_class(class_name);
        jmethodID callback_method =
            intern_method(callback_class, method_name_, signature);
        if (trace_callback)
            fprintf(stderr,
                    "[jni-ui] invoke %s.%s handle=0x%llx class=%p method=%p\n",
                    class_name, method_name_, (unsigned long long)p->handle,
                    callback_class, callback_method);
        ((jobject (*)(void *, jclass, jlong, jclass, jobject, jarray))invoke)(
            g_env, NULL, p->handle, callback_class,
            (jobject)callback_method, args);
    }
    if (trace_callback)
        fprintf(stderr, "[jni-ui] %s.%s retornou handle=0x%llx\n",
                class_name, method_name_, (unsigned long long)p->handle);
}

static void invoke_proxy(fake_proxy *p) {
    invoke_proxy_method(p, "java/lang/Runnable", "run", "()V", NULL);
}

static void *handler_thread_main(void *unused) {
    (void)unused;
    pthread_setname_np(pthread_self(), "UnityChoreogr");
    for (;;) {
        int deliver_message = 0;
        int deliver_frame = 0;
        uint64_t frame_due = 0;

        pthread_mutex_lock(&g_handler_lock);
        while (!g_handler_message_pending && !g_handler_frame_pending)
            pthread_cond_wait(&g_handler_cond, &g_handler_lock);
        if (g_handler_message_pending) {
            g_handler_message_pending = 0;
            deliver_message = 1;
        } else {
            g_handler_frame_pending = 0;
            deliver_frame = 1;
            frame_due = g_handler_frame_due_ms;
        }
        pthread_mutex_unlock(&g_handler_lock);

        if (deliver_message) {
            fake_proxy *p = g_handler_callback ? g_handler_callback
                                               : g_choreographer_proxy;
            invoke_proxy_method(p, "android/os/Handler$Callback", "handleMessage",
                                "(Landroid/os/Message;)Z",
                                (jarray)&g_handlemessage_args);
        }
        if (deliver_frame) {
            uint64_t now = monotonic_ms();
            if (frame_due > now) usleep((useconds_t)((frame_due - now) * 1000u));
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            g_frame_time_nanos = (jlong)ts.tv_sec * 1000000000LL + ts.tv_nsec;
            invoke_proxy_method(g_choreographer_proxy,
                                "android/view/Choreographer$FrameCallback",
                                "doFrame", "(J)V", (jarray)&g_doframe_args);
        }
    }
    return NULL;
}

static void start_handler_thread(void) {
    pthread_mutex_lock(&g_handler_lock);
    if (!g_handler_started) {
        g_handler_started = 1;
        int rc = pthread_create(&g_handler_thread, NULL, handler_thread_main, NULL);
        if (rc == 0) {
            pthread_detach(g_handler_thread);
            if (getenv("FF5_JNI_TRACE"))
                fprintf(stderr, "[jni-ui] HandlerThread/Looper iniciado\n");
        } else {
            g_handler_started = 0;
            fprintf(stderr, "[jni-ui] HandlerThread falhou: %d\n", rc);
        }
    }
    pthread_mutex_unlock(&g_handler_lock);
}

static void enqueue_handler_message(void) {
    start_handler_thread();
    pthread_mutex_lock(&g_handler_lock);
    g_handler_message_pending = 1;
    pthread_cond_signal(&g_handler_cond);
    pthread_mutex_unlock(&g_handler_lock);
}

static void enqueue_choreographer_frame(uint64_t delay_ms) {
    start_handler_thread();
    pthread_mutex_lock(&g_handler_lock);
    g_handler_frame_pending = 1;
    /* Android entrega o callback no proximo vsync, nao dentro da chamada que
     * o postou. Um periodo de 16 ms reproduz o display de 60 Hz do aparelho. */
    g_handler_frame_due_ms = monotonic_ms() + (delay_ms ? delay_ms : 16);
    pthread_cond_signal(&g_handler_cond);
    pthread_mutex_unlock(&g_handler_lock);
}

static void post_ui_task(jobject runnable, uint64_t delay_ms) {
    if (!is_proxy(runnable)) {
        if (getenv("FF5_JNI_TRACE"))
            fprintf(stderr, "[jni-ui] Runnable desconhecido %p\n", runnable);
        return;
    }
    fake_proxy *p = (fake_proxy *)runnable;
    if (pthread_equal(pthread_self(), g_ui_thread) && delay_ms == 0) {
        invoke_proxy(p);
        return;
    }
    pthread_mutex_lock(&g_ui_lock);
    unsigned next = (g_ui_tail + 1) % (sizeof g_ui_queue / sizeof g_ui_queue[0]);
    if (next == g_ui_head) {
        pthread_mutex_unlock(&g_ui_lock);
        fprintf(stderr, "[jni-ui] fila UI cheia\n");
        return;
    }
    g_ui_queue[g_ui_tail].proxy = p;
    g_ui_queue[g_ui_tail].due_ms = monotonic_ms() + delay_ms;
    g_ui_tail = next;
    pthread_mutex_unlock(&g_ui_lock);
}

void jni_pump_ui_tasks(void) {
    for (;;) {
        ui_task task;
        int have = 0;
        pthread_mutex_lock(&g_ui_lock);
        if (g_ui_head != g_ui_tail &&
            g_ui_queue[g_ui_head].due_ms <= monotonic_ms()) {
            task = g_ui_queue[g_ui_head];
            g_ui_head = (g_ui_head + 1) %
                        (sizeof g_ui_queue / sizeof g_ui_queue[0]);
            have = 1;
        }
        pthread_mutex_unlock(&g_ui_lock);
        if (!have) break;
        invoke_proxy(task.proxy);
    }
}

// jstring real (p/ o guest ler os caminhos que devolvemos).
typedef struct { char utf[1]; } jstr_hdr;
static jstring make_jstring(const char *s) {
    if (!s) s = "";
    size_t n = strlen(s);
    char *p = (char *)malloc(n + 1);
    memcpy(p, s, n + 1);
    return (jstring)p;   // ponteiro pra C-string; GetStringUTFChars devolve ele
}

// ---- tabela de nativos registrados via RegisterNatives -------------------
typedef struct { char *name; char *sig; uintptr_t fn; } native_t;
static native_t g_natives[512];
static int g_nnatives;

uintptr_t jni_find_native(const char *name) {
    for (int i = 0; i < g_nnatives; i++)
        if (!strcmp(g_natives[i].name, name)) return g_natives[i].fn;
    return 0;
}
uintptr_t jni_find_native_signature(const char *signature) {
    for (int i = 0; i < g_nnatives; i++)
        if (!strcmp(g_natives[i].sig, signature)) return g_natives[i].fn;
    return 0;
}
void jni_dump_natives(void) {
    fprintf(stderr, "[jni] %d metodos nativos registrados:\n", g_nnatives);
    for (int i = 0; i < g_nnatives; i++)
        fprintf(stderr, "      %s %s -> %p\n", g_natives[i].name,
                g_natives[i].sig, (void *)g_natives[i].fn);
}

// ---- stub generico (todo slot nao implementado) --------------------------
static intptr_t jni_stub(void *env, ...) { return 0; }

// ---- implementacoes ------------------------------------------------------
static jint f_GetVersion(void *env) { return 0x00010006; }

static jclass f_FindClass(void *env, const char *name) {
    if (getenv("FF5_JNI_TRACE"))
        fprintf(stderr, "[jni] FindClass %s\n", name ? name : "(null)");
    if (name && strstr(name, "Choreographer$FrameCallback"))
        g_next_proxy_is_choreographer = 1;
    return intern_class(name);
}
/* Declarados adiante (secao do gamepad); IsInstanceOf precisa deles. */
static int ff5_is_key_event(jobject o);
static int ff5_is_motion_event(jobject o);

static jclass f_GetObjectClass(void *env, jobject o) {
    if (o == (jobject)&g_lvl_service_binder)
        return intern_class("com/unity3d/plugin/lvl/ServiceBinder");
    if (ff5_is_key_event(o))    return intern_class("android/view/KeyEvent");
    if (ff5_is_motion_event(o)) return intern_class("android/view/MotionEvent");
    return intern_class("obj");
}
/* A Unity decide o caminho do evento com IsInstanceOf(ev, KeyEvent/MotionEvent).
 * Responder `true` para tudo fazia um KeyEvent do pad entrar no caminho de
 * toque (MotionEvent.obtain) e o botao nunca chegar ao InputObserver do jogo.
 * Fora dos nossos objetos de input o comportamento permissivo continua: a JVM
 * falsa nao modela hierarquia e um `false` generico quebraria outros fluxos. */
static jboolean f_IsInstanceOf(void *env, jobject o, jclass c) {
    const char *cls = c ? (const char *)c : NULL;
    jboolean r = 1;
    if (cls && (ff5_is_key_event(o) || ff5_is_motion_event(o))) {
        if (!strcmp(cls, "android/view/KeyEvent"))         r = ff5_is_key_event(o);
        else if (!strcmp(cls, "android/view/MotionEvent")) r = ff5_is_motion_event(o);
        else if (!strcmp(cls, "android/view/InputEvent"))  r = 1;
        else r = 0;
    }
    if (getenv("FF5_JNI_TRACE"))
        fprintf(stderr, "[jni] IsInstanceOf obj=%p class=%s (%p) -> %s\n",
                o, cls ? cls : "(null)", c, r ? "true" : "false");
    return r;
}
static jboolean f_IsSameObject(void *env, jobject a, jobject b) { return a == b; }

static jthrowable f_ExceptionOccurred(void *env) { return NULL; }
static void f_ExceptionClear(void *env) {}
static jboolean f_ExceptionCheck(void *env) { return 0; }
static jobject f_ref_id(void *env, jobject o) { return o; }
static void f_ref_void(void *env, jobject o) {}

static jmethodID f_GetMethodID(void *env, jclass c, const char *name, const char *sig) {
    if (getenv("FF5_JNI_TRACE"))
        fprintf(stderr, "[jni] GetMethodID class=%s name=%s sig=%s\n",
                c ? (const char *)c : "(null)", name ? name : "(null)",
                sig ? sig : "(null)");
    return intern_method(c, name, sig);
}
static jfieldID f_GetFieldID(void *env, jclass c, const char *name, const char *sig) {
    if (getenv("FF5_JNI_TRACE"))
        fprintf(stderr, "[jni] GetFieldID class=%s name=%s sig=%s\n",
                c ? (const char *)c : "(null)", name ? name : "(null)",
                sig ? sig : "(null)");
    return (jfieldID)strdup(name ? name : "?");
}
static jfieldID f_FromReflectedField(void *env, jobject o) { return (jfieldID)o; }

/* Quem le os campos do ServiceBinder e' o proprio validador managed do jogo.
 * Logar o endereco de retorno em RVA da libil2cpp identifica esse metodo sem
 * alterar nenhum valor: e' so' observacao para a RE do fluxo de licenca. */
extern uintptr_t ff5_il2cpp_base(void);
static void lvl_log_caller(const char *field, void *ret) {
    const uintptr_t base = ff5_il2cpp_base();
    const uintptr_t r = (uintptr_t)ret;
    if (base && r > base)
        fprintf(stderr, "[jni-lvl] %s lido de libil2cpp+0x%lx\n", field,
                (unsigned long)(r - base));
    else
        fprintf(stderr, "[jni-lvl] %s lido de %p\n", field, ret);
}

static jobject f_GetObjectField(void *env, jobject o, jfieldID f) {
    const char *name = (const char *)f;
    if (o == (jobject)&g_lvl_service_binder && name) {
        if (!strcmp(name, "_arg1")) {
            if (getenv("FF5_JNI_TRACE")) {
                fprintf(stderr, "[jni-lvl] ServiceBinder._arg1 -> %p\n",
                        g_lvl_service_binder.signed_data);
                lvl_log_caller("_arg1", __builtin_return_address(0));
            }
            return g_lvl_service_binder.signed_data;
        }
        if (!strcmp(name, "_arg2")) {
            if (getenv("FF5_JNI_TRACE")) {
                fprintf(stderr, "[jni-lvl] ServiceBinder._arg2 -> %p\n",
                        g_lvl_service_binder.signature);
                lvl_log_caller("_arg2", __builtin_return_address(0));
            }
            return g_lvl_service_binder.signature;
        }
    }
    return NULL;
}
static jint f_GetIntField(void *env, jobject o, jfieldID f) {
    const char *name = (const char *)f;
    if (o == (jobject)&g_lvl_service_binder && name && !strcmp(name, "_arg0")) {
        if (getenv("FF5_JNI_TRACE"))
            fprintf(stderr, "[jni-lvl] ServiceBinder._arg0 -> %d\n",
                    g_lvl_service_binder.response_code);
        return g_lvl_service_binder.response_code;
    }
    if (o == (jobject)&g_message_obj && name && !strcmp(name, "what"))
        return g_message_obj.what;
    return 0;
}
static jfloat f_GetFloatField(void *env, jobject o, jfieldID f) { return 0.0f; }
static jobject f_GetStaticObjectField(void *env, jclass c, jfieldID f) {
    const char *name = (const char *)f;
    if (getenv("FF5_JNI_TRACE"))
        fprintf(stderr, "[jni] GetStaticObjectField class=%s field=%s\n",
                c ? (const char *)c : "(null)", name ? name : "(null)");
    /* Unity consulta android.os.Build antes de inicializar o input. Java
     * garante Strings nao nulas aqui; devolver NULL chega a strcasecmp no
     * nativo e derruba o processo. */
    if (name && !strcmp(name, "MANUFACTURER")) return make_jstring("Amlogic");
    if (name && !strcmp(name, "MODEL"))        return make_jstring("NextOS");
    if (name && (!strcmp(name, "DEVICE") || !strcmp(name, "PRODUCT") ||
                 !strcmp(name, "BRAND") || !strcmp(name, "HARDWARE")))
        return make_jstring("NextOS");
    if (name && !strcmp(name, "currentActivity"))
        return (jobject)&g_activity_obj;
    return NULL;
}

// SDK_INT -> 30 (Android 11). O nome do field vem no jfieldID.
static jint f_GetStaticIntField(void *env, jclass c, jfieldID f) {
    const char *n = (const char *)f;
    if (n && !strcmp(n, "SDK_INT")) return 30;
    return 0;
}

/* ===================== gamepad nativo (input Android) ====================
 * O FF5 tem suporte proprio a controle (GamepadPopupManager / InputObserver),
 * entao o caminho certo e' o mesmo do Android: a Unity (a) enumera os devices
 * por InputManager/InputDevice e (b) recebe cada evento por injectEvent ->
 * nativeInjectEvent(InputEvent). Nada de cursor nem toque sintetico.
 *
 * Aqui publicamos UM gamepad e materializamos KeyEvent/MotionEvent como
 * objetos falsos cujos getters a Unity consulta por JNI. */
#define FF5_PAD_DEVICE_ID   1
#define ASOURCE_KEYBOARD    0x00000101
#define ASOURCE_DPAD        0x00000201
#define ASOURCE_GAMEPAD     0x00000401
#define ASOURCE_JOYSTICK    0x01000010
#define ASOURCE_TOUCHSCREEN 0x00001002
#define FF5_PAD_SOURCES     (ASOURCE_KEYBOARD | ASOURCE_DPAD | \
                             ASOURCE_GAMEPAD  | ASOURCE_JOYSTICK)

/* Eixos que um gamepad Android padrao expoe (MotionEvent.AXIS_*). */
static const jint g_pad_axes[] = { 0, 1, 11, 14, 15, 16, 17, 18 };
#define FF5_PAD_NAXES ((int)(sizeof g_pad_axes / sizeof g_pad_axes[0]))

typedef struct fake_input_device {
    fake_java_type type;
    jint id, sources;
    const char *name, *descriptor;
} fake_input_device;
static fake_input_device g_input_device = {
    FJ_INPUT_DEVICE, FF5_PAD_DEVICE_ID, FF5_PAD_SOURCES,
    "NextOS Gamepad", "nextos:gamepad:0"
};
/* O toque NAO pode reportar o gamepad em InputEvent.getDevice(): a Unity le a
 * fonte do device para escolher o caminho e roteava o toque como eixo de
 * joystick, entao o clique nunca chegava a UI. */
static fake_input_device g_touch_device = {
    FJ_INPUT_DEVICE, 0, ASOURCE_TOUCHSCREEN,
    "NextOS Touchscreen", "nextos:touch:0"
};
static fake_java_object g_input_manager = { FJ_INPUT_MANAGER, 0 };
static fake_java_object g_motion_range_list = { FJ_MOTION_RANGE_LIST, 0 };

typedef struct fake_motion_range { fake_java_type type; jint axis; } fake_motion_range;
static fake_motion_range g_motion_range[FF5_PAD_NAXES];

typedef struct fake_input_event {
    fake_java_type type;
    jint action, keycode, repeat, meta, flags, scancode, unicode;
    jint source, tool_type;
    jlong event_time, down_time;
    jfloat axis[32];
} fake_input_event;
static fake_input_event g_key_event    = { FJ_KEY_EVENT, 0 };
static fake_input_event g_motion_event = { FJ_MOTION_EVENT, 0 };
static jlong g_touch_down_time;

/* A Unity nao consome o evento na hora: ela chama MotionEvent.obtain(ev) para
 * COPIAR e enfileirar, e so' depois le os campos da copia (e a recicla). Sem
 * uma copia de verdade a fila recebia a sentinela generica e o toque sumia.
 * Um anel pequeno basta — a Unity recicla cada copia no mesmo quadro. */
#define FF5_EVENT_POOL 32
static fake_input_event g_event_pool[FF5_EVENT_POOL];
static unsigned g_event_pool_next;

static int in_event_pool(jobject o) {
    const fake_input_event *p = (const fake_input_event *)o;
    return o && p >= g_event_pool && p < g_event_pool + FF5_EVENT_POOL;
}
static jobject event_pool_copy(const fake_input_event *src) {
    fake_input_event *dst = &g_event_pool[g_event_pool_next++ % FF5_EVENT_POOL];
    *dst = *src;
    return (jobject)dst;
}

#define INT_ARRAY_MAGIC 0x4e494146u
typedef struct fake_int_array { uint32_t magic; jint length; jint v[8]; } fake_int_array;
static fake_int_array g_device_ids = { INT_ARRAY_MAGIC, 1, { FF5_PAD_DEVICE_ID } };

static fake_int_array *as_int_array(jarray a) {
    if (!a) return NULL;
    fake_int_array *p = (fake_int_array *)a;
    return p->magic == INT_ARRAY_MAGIC ? p : NULL;
}
static int is_input_event(jobject o) {
    if (!o) return 0;
    if (o == (jobject)&g_key_event || o == (jobject)&g_motion_event) return 1;
    return in_event_pool(o);
}
static int ff5_is_key_event(jobject o) {
    if (!is_input_event(o)) return 0;
    return ((const fake_input_event *)o)->type == FJ_KEY_EVENT;
}
static int ff5_is_motion_event(jobject o) {
    if (!is_input_event(o)) return 0;
    return ((const fake_input_event *)o)->type == FJ_MOTION_EVENT;
}

/* Getters de InputDevice/InputEvent que devolvem inteiro. Devolve 1 quando
 * tratou o nome, para o dispatcher generico nao sobrescrever com 0. */
static int touch_trace(const char *what, const char *name, double v) {
    if (getenv("FF5_TOUCH_TRACE"))
        fprintf(stderr, "[touch] %s.%s = %g\n", what, name ? name : "?", v);
    return 1;
}

static int pad_int_getter(jobject o, const char *name, jint a0, jint *out) {
    if (!name || !o) return 0;
    const fake_java_type t = ((const fake_java_object *)o)->type;

    if (t == FJ_INPUT_DEVICE) {
        const fake_input_device *d = (const fake_input_device *)o;
        if (!strcmp(name, "getId"))              { *out = d->id;      return 1; }
        if (!strcmp(name, "getSources"))         { *out = d->sources; return 1; }
        if (!strcmp(name, "getVendorId"))        { *out = 0x054c;            return 1; }
        if (!strcmp(name, "getProductId"))       { *out = 0x09cc;            return 1; }
        if (!strcmp(name, "getControllerNumber")){ *out = 1;                 return 1; }
        if (!strcmp(name, "getKeyboardType"))    { *out = 1;                 return 1; }
    }
    if (t == FJ_MOTION_RANGE) {
        if (!strcmp(name, "getAxis")) { *out = ((fake_motion_range *)o)->axis; return 1; }
        if (!strcmp(name, "getSource")) { *out = FF5_PAD_SOURCES; return 1; }
    }
    if (t == FJ_MOTION_RANGE_LIST && !strcmp(name, "size")) {
        *out = FF5_PAD_NAXES; return 1;
    }
    if (is_input_event(o)) {
        const fake_input_event *e = (const fake_input_event *)o;
        if (!strcmp(name, "getAction"))       { *out = e->action;   return touch_trace("ev","getAction",*out); }
        if (!strcmp(name, "getActionMasked")) { *out = e->action & 0xff; return touch_trace("ev","getActionMasked",*out); }
        if (!strcmp(name, "getActionIndex"))  { *out = 0;           return 1; }
        if (!strcmp(name, "getKeyCode"))      { *out = e->keycode;  return 1; }
        if (!strcmp(name, "getRepeatCount"))  { *out = e->repeat;   return 1; }
        if (!strcmp(name, "getMetaState"))    { *out = e->meta;     return 1; }
        if (!strcmp(name, "getFlags"))        { *out = e->flags;    return 1; }
        if (!strcmp(name, "getScanCode"))     { *out = e->scancode; return 1; }
        if (!strcmp(name, "getUnicodeChar"))  { *out = e->unicode;  return 1; }
        if (!strcmp(name, "getDeviceId"))
            { *out = e->source == ASOURCE_TOUCHSCREEN ? 0 : FF5_PAD_DEVICE_ID; return 1; }
        if (!strcmp(name, "getSource"))       { *out = e->source;    return 1; }
        if (!strcmp(name, "getPointerCount")) { *out = 1;  return 1; }
        if (!strcmp(name, "getPointerId"))    { *out = 0;  return 1; }
        if (!strcmp(name, "getHistorySize"))  { *out = 0;  return 1; }
        if (!strcmp(name, "getToolType"))     { *out = e->tool_type; return 1; }
        if (!strcmp(name, "getButtonState"))  { *out = 0;  return 1; }
        if (!strcmp(name, "getEdgeFlags"))    { *out = 0;  return 1; }
    }
    if (is_input_event(o) && getenv("FF5_TOUCH_TRACE"))
        fprintf(stderr, "[touch] int getter NAO tratado: %s\n", name);
    (void)a0;
    return 0;
}

static int pad_float_getter(jobject o, const char *name, jint a0, jfloat *out) {
    if (!name || !o) return 0;
    const fake_java_type t = ((const fake_java_object *)o)->type;
    if (t == FJ_MOTION_RANGE) {
        const jint axis = ((fake_motion_range *)o)->axis;
        /* Gatilhos vao de 0..1; os demais eixos de -1..1. */
        const int trigger = (axis == 17 || axis == 18 || axis == 22 || axis == 23);
        if (!strcmp(name, "getMin"))   { *out = trigger ? 0.0f : -1.0f; return 1; }
        if (!strcmp(name, "getMax"))   { *out = 1.0f;                   return 1; }
        if (!strcmp(name, "getRange")) { *out = trigger ? 1.0f : 2.0f;  return 1; }
        if (!strcmp(name, "getFlat"))  { *out = 0.0f;                   return 1; }
        if (!strcmp(name, "getFuzz"))  { *out = 0.0f;                   return 1; }
        if (!strcmp(name, "getResolution")) { *out = 0.0f;              return 1; }
    }
    if (is_input_event(o)) {
        const fake_input_event *e = (const fake_input_event *)o;
        if (!strcmp(name, "getAxisValue"))
            { *out = (a0 >= 0 && a0 < 32) ? e->axis[a0] : 0.0f; return 1; }
        if (!strcmp(name, "getX")) { *out = e->axis[0]; return touch_trace("ev","getX",*out); }
        if (!strcmp(name, "getY")) { *out = e->axis[1]; return touch_trace("ev","getY",*out); }
        if (!strcmp(name, "getPressure") || !strcmp(name, "getSize"))
            { *out = 1.0f; return 1; }
        if (!strcmp(name, "getTouchMajor") || !strcmp(name, "getTouchMinor"))
            { *out = 8.0f; return 1; }
        if (getenv("FF5_TOUCH_TRACE"))
            fprintf(stderr, "[touch] float getter NAO tratado: %s\n", name);
    }
    return 0;
}

static int pad_long_getter(jobject o, const char *name, jlong *out) {
    if (!name || !is_input_event(o)) return 0;
    const fake_input_event *e = (const fake_input_event *)o;
    if (!strcmp(name, "getEventTime")) { *out = e->event_time; return 1; }
    if (!strcmp(name, "getDownTime"))  { *out = e->down_time;  return 1; }
    return 0;
}

/* Objetos devolvidos por metodos de referencia (int[] de ids, o device, os
 * MotionRange). Devolve 1 quando o nome pertence ao subsistema de input. */
static int pad_object_getter(jobject thiz, const char *name, jint a0,
                             jobject a0obj, jobject *out) {
    if (!name) return 0;
    if (!strcmp(name, "getInputDeviceIds") || !strcmp(name, "getDeviceIds")) {
        /* Diagnostico: sem pad enumerado o jogo nao abre o popup de troca de
         * controles, o que permite testar o caminho de toque isolado. */
        g_device_ids.length = getenv("FF5_NO_PAD_ENUM") ? 0 : 1;
        *out = (jobject)&g_device_ids; return 1;
    }
    /* InputEvent.getDevice() nao leva argumento; InputManager.getInputDevice(id) leva. */
    if (!strcmp(name, "getDevice") && is_input_event(thiz)) {
        *out = ((const fake_input_event *)thiz)->source == ASOURCE_TOUCHSCREEN
             ? (jobject)&g_touch_device : (jobject)&g_input_device;
        return 1;
    }
    /* MotionEvent/KeyEvent.obtain(src): a copia que entra na fila da Unity. */
    if (!strcmp(name, "obtain")) {
        const fake_input_event *src = NULL;
        if (is_input_event(thiz)) src = (const fake_input_event *)thiz;
        else if (is_input_event(a0obj)) src = (const fake_input_event *)a0obj;
        if (src) { *out = event_pool_copy(src); return 1; }
    }
    if (!strcmp(name, "getInputDevice") || !strcmp(name, "getDevice")) {
        *out = (a0 == FF5_PAD_DEVICE_ID) ? (jobject)&g_input_device : NULL;
        return 1;
    }
    if (thiz && ((const fake_java_object *)thiz)->type == FJ_INPUT_DEVICE) {
        const fake_input_device *d = (const fake_input_device *)thiz;
        if (!strcmp(name, "getName"))       { *out = make_jstring(d->name); return 1; }
        if (!strcmp(name, "getDescriptor")) { *out = make_jstring(d->descriptor); return 1; }
        if (!strcmp(name, "getMotionRanges")) { *out = (jobject)&g_motion_range_list; return 1; }
        if (!strcmp(name, "getMotionRange")) {
            for (int i = 0; i < FF5_PAD_NAXES; i++)
                if (g_pad_axes[i] == a0) { *out = (jobject)&g_motion_range[i]; return 1; }
            *out = NULL; return 1;
        }
    }
    if (thiz && ((const fake_java_object *)thiz)->type == FJ_MOTION_RANGE_LIST &&
        !strcmp(name, "get")) {
        *out = (a0 >= 0 && a0 < FF5_PAD_NAXES) ? (jobject)&g_motion_range[a0] : NULL;
        return 1;
    }
    return 0;
}

static void pad_init_objects(void) {
    for (int i = 0; i < FF5_PAD_NAXES; i++) {
        g_motion_range[i].type = FJ_MOTION_RANGE;
        g_motion_range[i].axis = g_pad_axes[i];
    }
}

/* --- API usada pelo loop de input do loader ---------------------------- */
jobject jni_pad_key_event(int action, int keycode, int repeat) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const jlong now = (jlong)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    g_key_event.type = FJ_KEY_EVENT;
    g_key_event.action = action;
    g_key_event.keycode = keycode;
    g_key_event.repeat = repeat;
    g_key_event.meta = 0;
    g_key_event.flags = 0;
    g_key_event.scancode = 0;
    g_key_event.unicode = 0;
    g_key_event.source = FF5_PAD_SOURCES;
    g_key_event.tool_type = 0;
    g_key_event.event_time = now;
    if (action == 0 && repeat == 0) g_key_event.down_time = now;
    return (jobject)&g_key_event;
}

/* Toque na tela. Necessario UMA vez: o dialogo "Gamepad has been connected /
 * Switch to gamepad controls?" aparece enquanto o jogo ainda esta em modo
 * TOQUE, entao os botoes dele nao respondem ao pad — no Android o usuario
 * toca "Yes" e so' depois o jogo passa a ler o controle. Reproduzimos esse
 * toque unico; a partir dai a navegacao e' 100% pelo pad. */
jobject jni_touch_event(int action, float x, float y) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const jlong now = (jlong)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    memset(&g_motion_event, 0, sizeof g_motion_event);
    g_motion_event.type = FJ_MOTION_EVENT;
    g_motion_event.action = action;              /* 0=DOWN 1=UP 2=MOVE */
    g_motion_event.source = ASOURCE_TOUCHSCREEN;
    g_motion_event.tool_type = 1;                /* TOOL_TYPE_FINGER */
    g_motion_event.axis[0] = x;
    g_motion_event.axis[1] = y;
    g_motion_event.event_time = now;
    if (action == 0) g_touch_down_time = now;
    g_motion_event.down_time = g_touch_down_time;
    return (jobject)&g_motion_event;
}

jobject jni_pad_motion_event(const float *axes, int naxes) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const jlong now = (jlong)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    g_motion_event.type = FJ_MOTION_EVENT;
    g_motion_event.action = 2;               /* ACTION_MOVE */
    g_motion_event.source = FF5_PAD_SOURCES;
    g_motion_event.tool_type = 0;
    g_motion_event.event_time = now;
    g_motion_event.down_time = now;
    for (int i = 0; i < 32; i++)
        g_motion_event.axis[i] = (i < naxes && axes) ? axes[i] : 0.0f;
    return (jobject)&g_motion_event;
}

// ---- dispatchers de Call*Method (casam pelo nome) ------------------------
// Devolvem jstring (caminho/pacote) ou objeto sentinela conforme o metodo.
static jobject dispatch_object(void *env, jobject thiz, jmethodID m, va_list ap) {
    const char *name = method_name(m);
    if (!name) return NULL;
    if (getenv("FF5_JNI_TRACE"))
        fprintf(stderr, "[jni] CallObjectMethod obj=%p method=%s\n", thiz, name);
    {   /* Enumeracao de gamepad: os getters de referencia do InputDevice. */
        jobject pad = NULL;
        va_list probe; va_copy(probe, ap);
        jint a0 = va_arg(probe, jint);
        va_end(probe);
        va_list probe2; va_copy(probe2, ap);
        jobject a0obj = va_arg(probe2, jobject);
        va_end(probe2);
        if (pad_object_getter(thiz, name, a0, a0obj, &pad)) return pad;
    }
    if (!strcmp(name, "getPackageName")) return make_jstring(g_pkg);
    if (!strcmp(name, "getInstallerPackageName")) return make_jstring("com.android.vending");
    if (!strcmp(name, "getAssetPackPath")) {
        char p[600]; snprintf(p, sizeof p, "%s/assetpack", g_gamedir);
        return make_jstring(p);
    }
    if (!strcmp(name, "getFilesDir") || !strcmp(name, "getDataDir") ||
        !strcmp(name, "getExternalFilesDir") || !strcmp(name, "getCacheDir") ||
        !strcmp(name, "getExternalCacheDir"))
        return make_jstring(g_userdata);
    // File: getAbsolutePath/getPath/getCanonicalPath => identidade (o thiz ja
    // e um jstring do caminho que devolvemos acima).
    if (!strcmp(name, "getAbsolutePath") || !strcmp(name, "getPath") ||
        !strcmp(name, "getCanonicalPath") || !strcmp(name, "toString"))
        return thiz;
    if (!strcmp(name, "findLibrary")) {
        const char *lib = (const char *)va_arg(ap, void *);
        char p[600]; snprintf(p, sizeof p, "%s/lib%s.so", g_gamedir, lib ? lib : "?");
        return make_jstring(p);
    }
    /* AndroidJNIHelper resolve membros por meio dos metodos Java de
     * ReflectionHelper e converte o objeto retornado com FromReflected*. A JVM
     * falsa deve devolver o ID real, nao uma sentinela generica. */
    if (!strcmp(name, "getMethodID")) {
        jclass klass = va_arg(ap, jclass);
        const char *method = (const char *)va_arg(ap, jobject);
        const char *sig = (const char *)va_arg(ap, jobject);
        (void)va_arg(ap, jint); /* isStatic */
        if (getenv("FF5_JNI_TRACE"))
            fprintf(stderr, "[jni-reflect] method class=%s name=%s sig=%s\n",
                    klass ? (const char *)klass : "(null)",
                    method ? method : "(null)", sig ? sig : "(null)");
        return (jobject)intern_method(klass, method, sig);
    }
    if (!strcmp(name, "getConstructorID")) {
        jclass klass = va_arg(ap, jclass);
        const char *sig = (const char *)va_arg(ap, jobject);
        if (getenv("FF5_JNI_TRACE"))
            fprintf(stderr, "[jni-reflect] ctor class=%s sig=%s\n",
                    klass ? (const char *)klass : "(null)",
                    sig ? sig : "(null)");
        return (jobject)intern_method(klass, "<init>", sig);
    }
    if (!strcmp(name, "getFieldID")) {
        jclass klass = va_arg(ap, jclass);
        const char *field = (const char *)va_arg(ap, jobject);
        const char *sig = (const char *)va_arg(ap, jobject);
        (void)va_arg(ap, jint); /* isStatic */
        if (getenv("FF5_JNI_TRACE"))
            fprintf(stderr, "[jni-reflect] field class=%s name=%s sig=%s\n",
                    klass ? (const char *)klass : "(null)",
                    field ? field : "(null)", sig ? sig : "(null)");
        return (jobject)strdup(field ? field : "?");
    }
    if (!strcmp(name, "newInterfaceProxy") || !strcmp(name, "newProxyInstance")) {
        jlong handle = va_arg(ap, jlong);
        (void)va_arg(ap, jobject); /* Class[]; o handle ja identifica o proxy. */
        fake_proxy *p = (fake_proxy *)calloc(1, sizeof *p);
        p->magic = PROXY_MAGIC;
        p->kind = !strcmp(name, "newProxyInstance")
                ? PROXY_REFLECTION_HELPER : PROXY_JNI_BRIDGE;
        p->handle = handle;
        if (g_next_proxy_is_choreographer) {
            /* A Unity cria um unico proxy que implementa Handler.Callback e
             * Choreographer.FrameCallback. Guarde as duas funcoes do objeto. */
            g_choreographer_proxy = p;
            g_handler_callback = p;
            g_next_proxy_is_choreographer = 0;
        }
        if (getenv("FF5_JNI_TRACE"))
            fprintf(stderr, "[jni-ui] newInterfaceProxy handle=0x%llx -> %p\n",
                    (unsigned long long)handle, (void *)p);
        return (jobject)p;
    }
    if (!strcmp(name, "obtainMessage")) {
        g_message_obj.what = va_arg(ap, jint);
        if (getenv("FF5_JNI_TRACE"))
            fprintf(stderr, "[jni-ui] Handler.obtainMessage(%d)\n",
                    g_message_obj.what);
        return (jobject)&g_message_obj;
    }
    if (!strcmp(name, "getInstance"))
        return (jobject)&g_choreographer_obj;
    if (!strcmp(name, "getLooper"))
        return (jobject)&g_handler_thread_obj;
    if (!strcmp(name, "setTitle") || !strcmp(name, "setMessage")) {
        jobject text = va_arg(ap, jobject);
        if (getenv("FF5_JNI_TRACE"))
            fprintf(stderr, "[jni-ui] AlertDialog.%s: %s\n", name,
                    text ? (const char *)text : "(null)");
        return thiz;
    }
    if (!strcmp(name, "getProperty")) {
        const char *k = (const char *)va_arg(ap, void *);
        // AudioManager props: valores validos p/ o mixer FMOD nao travar.
        if (k && strstr(k, "PerBuffer")) return make_jstring("256");
        if (k && strstr(k, "ampleRate")) return make_jstring("48000");
        return make_jstring("");
    }
    if (!strcmp(name, "getString")) return make_jstring("");  // SharedPreferences
    /* Context.getSystemService("input") tem que devolver o InputManager, ou a
     * Unity nunca chega em getInputDeviceIds e o pad fica invisivel. */
    if (!strcmp(name, "getSystemService")) {
        const char *svc = (const char *)va_arg(ap, void *);
        if (svc && !strcmp(svc, "input")) return (jobject)&g_input_manager;
        return (jobject)&g_activity_obj;
    }
    // getAssets / edit / etc: objeto sentinela nao-nulo.
    return (jobject)&g_activity_obj;
}

static jobject dispatch_object_a(void *env, jobject thiz, jmethodID m,
                                 const fake_jvalue *args) {
    const char *name = method_name(m);
    if (!name) return NULL;
    if (getenv("FF5_JNI_TRACE"))
        fprintf(stderr, "[jni] CallObjectMethodA obj=%p method=%s\n", thiz, name);

    {   /* Enumeracao de gamepad (mesmo contrato da variante va_list). */
        jobject pad = NULL;
        if (pad_object_getter(thiz, name, args ? args[0].i : 0,
                              args ? args[0].l : NULL, &pad)) return pad;
    }
    if (!strcmp(name, "getMethodID")) {
        jclass klass = args ? (jclass)args[0].l : NULL;
        const char *method = args ? (const char *)args[1].l : NULL;
        const char *sig = args ? (const char *)args[2].l : NULL;
        if (getenv("FF5_JNI_TRACE"))
            fprintf(stderr, "[jni-reflect] methodA class=%s name=%s sig=%s static=%u\n",
                    klass ? (const char *)klass : "(null)",
                    method ? method : "(null)", sig ? sig : "(null)",
                    args ? (unsigned)args[3].z : 0u);
        return (jobject)intern_method(klass, method, sig);
    }
    if (!strcmp(name, "getConstructorID")) {
        jclass klass = args ? (jclass)args[0].l : NULL;
        const char *sig = args ? (const char *)args[1].l : NULL;
        if (getenv("FF5_JNI_TRACE"))
            fprintf(stderr, "[jni-reflect] ctorA class=%s sig=%s\n",
                    klass ? (const char *)klass : "(null)", sig ? sig : "(null)");
        return (jobject)intern_method(klass, "<init>", sig);
    }
    if (!strcmp(name, "getFieldID")) {
        jclass klass = args ? (jclass)args[0].l : NULL;
        const char *field = args ? (const char *)args[1].l : NULL;
        const char *sig = args ? (const char *)args[2].l : NULL;
        if (getenv("FF5_JNI_TRACE"))
            fprintf(stderr, "[jni-reflect] fieldA class=%s name=%s sig=%s static=%u\n",
                    klass ? (const char *)klass : "(null)",
                    field ? field : "(null)", sig ? sig : "(null)",
                    args ? (unsigned)args[3].z : 0u);
        return (jobject)strdup(field ? field : "?");
    }
    if (!strcmp(name, "newInterfaceProxy") || !strcmp(name, "newProxyInstance")) {
        jlong handle = args ? args[0].j : 0;
        fake_proxy *p = (fake_proxy *)calloc(1, sizeof *p);
        p->magic = PROXY_MAGIC;
        p->kind = !strcmp(name, "newProxyInstance")
                ? PROXY_REFLECTION_HELPER : PROXY_JNI_BRIDGE;
        p->handle = handle;
        if (g_next_proxy_is_choreographer) {
            g_choreographer_proxy = p;
            g_handler_callback = p;
            g_next_proxy_is_choreographer = 0;
        }
        if (getenv("FF5_JNI_TRACE"))
            fprintf(stderr, "[jni-ui] newProxyInstanceA handle=0x%llx -> %p\n",
                    (unsigned long long)handle, (void *)p);
        return (jobject)p;
    }

    /* AndroidJNI usa predominantemente as variantes A. Preserve aqui a mesma
     * semantica dos dispatchers varargs para caminhos e objetos do boot. */
    if (!strcmp(name, "getPackageName")) return make_jstring(g_pkg);
    if (!strcmp(name, "getInstallerPackageName"))
        return make_jstring("com.android.vending");
    if (!strcmp(name, "getAssetPackPath")) {
        char path[600];
        snprintf(path, sizeof path, "%s/assetpack", g_gamedir);
        return make_jstring(path);
    }
    if (!strcmp(name, "getFilesDir") || !strcmp(name, "getDataDir") ||
        !strcmp(name, "getExternalFilesDir") || !strcmp(name, "getCacheDir") ||
        !strcmp(name, "getExternalCacheDir"))
        return make_jstring(g_userdata);
    if (!strcmp(name, "getAbsolutePath") || !strcmp(name, "getPath") ||
        !strcmp(name, "getCanonicalPath") || !strcmp(name, "toString"))
        return thiz;
    if (!strcmp(name, "findLibrary")) {
        const char *library = args ? (const char *)args[0].l : NULL;
        char path[600];
        snprintf(path, sizeof path, "%s/lib%s.so", g_gamedir,
                 library ? library : "?");
        return make_jstring(path);
    }
    if (!strcmp(name, "obtainMessage")) {
        g_message_obj.what = args ? args[0].i : 0;
        return (jobject)&g_message_obj;
    }
    if (!strcmp(name, "getInstance"))
        return (jobject)&g_choreographer_obj;
    if (!strcmp(name, "getLooper"))
        return (jobject)&g_handler_thread_obj;
    if (!strcmp(name, "setTitle") || !strcmp(name, "setMessage"))
        return thiz;
    if (!strcmp(name, "getProperty")) {
        const char *property = args ? (const char *)args[0].l : NULL;
        if (property && strstr(property, "PerBuffer")) return make_jstring("256");
        if (property && strstr(property, "ampleRate")) return make_jstring("48000");
        return make_jstring("");
    }
    if (!strcmp(name, "getString")) return make_jstring("");
    if (!strcmp(name, "getClass")) return f_GetObjectClass(env, thiz);
    if (!strcmp(name, "getName"))
        return make_jstring(thiz ? (const char *)thiz : "");
    return (jobject)&g_activity_obj;
}

/* --- com.unity3d.plugin.lvl.ServiceBinder ---------------------------------
 * O ILicensingService responde SEMPRE de forma assincrona: bindService()
 * retorna e so' depois o binder entrega (responseCode, signedData, signature)
 * e roda o Runnable. O AndroidLvl do jogo depende dessa ordem — ele guarda o
 * proprio ServiceBinder no campo `_binder` DEPOIS que create() retorna, e o
 * callback comeca lendo esse campo:
 *
 *     r9 = -2 (default); r0 = this->_binder; if (r0 == 0) goto store(-2)
 *
 * Rodar o Runnable dentro do create() (como fazia o ramo "bindService
 * indisponivel") chega com `_binder` ainda nulo, o callback aborta no default
 * -2 e o Update cai fora da jump-table {-1,0,1,2} => dialogo de erro 1031.
 * Portanto: preencher a resposta e POSTAR o Runnable na fila da UI, para que
 * ele rode depois do retorno, exatamente como o servico real. */
static void lvl_arm_licensed_response(jobject runnable, jint nonce) {
    g_lvl_service_binder.nonce = nonce;
    g_lvl_service_binder.response_code = 0;   /* LICENSED */

    /* Formato do LicenseValidator: responseCode|nonce|package|versionCode|
     * userId|timestamp|extra. O jogo exige signedData e signature nao-vazios
     * (dois String.IsNullOrEmpty antes de seguir). */
    char signed_data[512];
    snprintf(signed_data, sizeof signed_data, "%d|%d|%s|%d|%s|%lld|",
             0, (int)nonce, g_pkg, 1, "0000000000000000",
             (long long)time(NULL) * 1000LL);
    g_lvl_service_binder.signed_data = make_jstring(signed_data);

    /* Assinatura RSA-1024/SHA1 em base64: 128 bytes => 171 chars + '='.
     * Convert.FromBase64String precisa de um base64 bem formado. */
    char signature[173];
    memset(signature, 'A', 171);
    signature[171] = '=';
    signature[172] = '\0';
    g_lvl_service_binder.signature = make_jstring(signature);

    if (getenv("FF5_JNI_TRACE"))
        fprintf(stderr, "[jni-lvl] resposta LICENSED armada: %s\n", signed_data);

    /* Atraso pequeno: garante que create() ja retornou e `_binder` esta' setado. */
    const char *ms = getenv("FF5_LVL_DELAY_MS");
    post_ui_task(runnable, ms ? (uint64_t)strtoul(ms, NULL, 10) : 250);
}

static void lvl_handle_create(jobject runnable, jint nonce, const char *tag) {
    if (is_proxy(runnable)) g_lvl_service_binder.done = (fake_proxy *)runnable;
    if (getenv("FF5_JNI_TRACE"))
        fprintf(stderr, "[jni-lvl] ServiceBinder.%s nonce=%d runnable=%p proxy=%d\n",
                tag, nonce, runnable, is_proxy(runnable));
    if (!is_proxy(runnable)) return;
    if (getenv("FF5_LVL_UNBOUND")) {
        /* Reproduz o ramo sem com.android.vending (bindService=false): o Java
         * roda mDone.run() sincronicamente com a resposta ainda no default.
         * Mantido so' para diagnostico — termina no dialogo 1031. */
        if (getenv("FF5_JNI_TRACE"))
            fprintf(stderr, "[jni-lvl] FF5_LVL_UNBOUND -> Runnable.run sincrono\n");
        invoke_proxy((fake_proxy *)runnable);
        return;
    }
    lvl_arm_licensed_response(runnable, nonce);
}

static void dispatch_void(void *env, jobject thiz, jmethodID m, va_list ap) {
    const char *name = method_name(m);
    if (!name) return;
    if (getenv("FF5_JNI_TRACE") &&
        (strcmp(name, "postFrameCallback") ||
         getenv("FF5_JNI_TRACE_FRAMES")))
        fprintf(stderr, "[jni] CallVoidMethod obj=%p method=%s\n", thiz, name);
    if (!strcmp(name, "runOnUiThread")) {
        jobject runnable = va_arg(ap, jobject);
        if (getenv("FF5_JNI_TRACE"))
            fprintf(stderr, "[jni-ui] runOnUiThread runnable=%p proxy=%d\n",
                    runnable, is_proxy(runnable));
        post_ui_task(runnable, 0);
        return;
    }
    if (!strcmp(name, "create") && thiz == (jobject)&g_lvl_service_binder) {
        jint nonce = va_arg(ap, jint);
        jobject runnable = va_arg(ap, jobject);
        lvl_handle_create(runnable, nonce, "create");
        return;
    }
    if (!strcmp(name, "start") && thiz == (jobject)&g_handler_thread_obj) {
        start_handler_thread();
        return;
    }
    if (!strcmp(name, "sendToTarget") && thiz == (jobject)&g_message_obj) {
        if (getenv("FF5_JNI_TRACE"))
            fprintf(stderr, "[jni-ui] Message.sendToTarget what=%d\n",
                    g_message_obj.what);
        enqueue_handler_message();
        return;
    }
    if (!strcmp(name, "postFrameCallback") ||
        !strcmp(name, "postFrameCallbackDelayed")) {
        jobject callback = va_arg(ap, jobject);
        uint64_t delay = !strcmp(name, "postFrameCallbackDelayed")
                       ? (uint64_t)va_arg(ap, jlong) : 0;
        if (is_proxy(callback)) g_choreographer_proxy = (fake_proxy *)callback;
        enqueue_choreographer_frame(delay);
        return;
    }
    if (!strcmp(name, "removeFrameCallback")) {
        pthread_mutex_lock(&g_handler_lock);
        g_handler_frame_pending = 0;
        pthread_mutex_unlock(&g_handler_lock);
        return;
    }
    // Play Asset Delivery: getAssetPackState(name, listener) -> avisa COMPLETED.
    if (!strcmp(name, "getPackState") || !strcmp(name, "getAssetPackState") ||
        !strcmp(name, "registerListener")) {
        // dispara nativeStatusQueryResult(pack, 4=COMPLETED, 0) se existir.
        uintptr_t cb = jni_find_native("nativeStatusQueryResult");
        void *pack = va_arg(ap, void *);
        if (cb) ((void (*)(void *, void *, void *, int, jlong))cb)(env, NULL, pack, 4, 0);
    }
}

static void dispatch_void_a(void *env, jobject thiz, jmethodID m,
                            const fake_jvalue *args) {
    const char *name = method_name(m);
    if (!name) return;
    if (getenv("FF5_JNI_TRACE"))
        fprintf(stderr, "[jni] CallVoidMethodA obj=%p method=%s\n", thiz, name);
    if (!strcmp(name, "create") && thiz == (jobject)&g_lvl_service_binder) {
        jint nonce = args ? args[0].i : 0;
        jobject runnable = args ? args[1].l : NULL;
        lvl_handle_create(runnable, nonce, "createA");
        return;
    }
    if (!strcmp(name, "runOnUiThread")) {
        post_ui_task(args ? args[0].l : NULL, 0);
        return;
    }
}

// Variantes ...MethodV (va_list) — sao as que o C++ do libunity usa.
static jobject f_CallObjectMethodV(void *env, jobject o, jmethodID m, va_list ap) { return dispatch_object(env, o, m, ap); }
static jobject f_CallObjectMethod(void *env, jobject o, jmethodID m, ...) {
    va_list ap; va_start(ap, m); jobject r = dispatch_object(env, o, m, ap); va_end(ap); return r;
}
static jobject f_CallStaticObjectMethodV(void *env, jclass c, jmethodID m, va_list ap) { return dispatch_object(env, NULL, m, ap); }
static jobject f_CallStaticObjectMethod(void *env, jclass c, jmethodID m, ...) {
    va_list ap; va_start(ap, m); jobject r = dispatch_object(env, NULL, m, ap); va_end(ap); return r;
}
static jobject f_CallObjectMethodA(void *env, jobject o, jmethodID m,
                                   const fake_jvalue *args) {
    return dispatch_object_a(env, o, m, args);
}
static jobject f_CallStaticObjectMethodA(void *env, jclass c, jmethodID m,
                                         const fake_jvalue *args) {
    return dispatch_object_a(env, NULL, m, args);
}
static void f_CallVoidMethodV(void *env, jobject o, jmethodID m, va_list ap) { dispatch_void(env, o, m, ap); }
static void f_CallVoidMethod(void *env, jobject o, jmethodID m, ...) {
    va_list ap; va_start(ap, m); dispatch_void(env, o, m, ap); va_end(ap);
}
static void f_CallStaticVoidMethodV(void *env, jclass c, jmethodID m, va_list ap) { dispatch_void(env, NULL, m, ap); }
static void f_CallStaticVoidMethod(void *env, jclass c, jmethodID m, ...) {
    va_list ap; va_start(ap, m); dispatch_void(env, NULL, m, ap); va_end(ap);
}
static void f_CallVoidMethodA(void *env, jobject o, jmethodID m,
                              const fake_jvalue *args) {
    dispatch_void_a(env, o, m, args);
}
static void f_CallStaticVoidMethodA(void *env, jclass c, jmethodID m,
                                    const fake_jvalue *args) {
    dispatch_void_a(env, NULL, m, args);
}
static jint f_CallIntMethodV(void *env, jobject o, jmethodID m, va_list ap) {
    jint out = 0;
    /* getAxisValue/getMotionRange/getPointerId levam um argumento inteiro. */
    va_list probe; va_copy(probe, ap);
    jint a0 = va_arg(probe, jint);
    va_end(probe);
    if (pad_int_getter(o, method_name(m), a0, &out)) return out;
    return 0;
}
static jint f_CallIntMethod(void *env, jobject o, jmethodID m, ...) {
    va_list ap; va_start(ap, m); jint r = f_CallIntMethodV(env, o, m, ap);
    va_end(ap); return r;
}
static jboolean dispatch_boolean(void *env, jobject o, jmethodID m, va_list ap) {
    const char *name = method_name(m);
    if (name && (!strcmp(name, "post") || !strcmp(name, "postDelayed"))) {
        jobject runnable = va_arg(ap, jobject);
        uint64_t delay = !strcmp(name, "postDelayed") ? (uint64_t)va_arg(ap, jlong) : 0;
        post_ui_task(runnable, delay);
        return 1;
    }
    return 0;
}
static jboolean f_CallBooleanMethodV(void *env, jobject o, jmethodID m, va_list ap) {
    return dispatch_boolean(env, o, m, ap);
}
static jboolean f_CallBooleanMethod(void *env, jobject o, jmethodID m, ...) {
    va_list ap; va_start(ap, m); jboolean r = dispatch_boolean(env, o, m, ap); va_end(ap); return r;
}
static jboolean f_CallBooleanMethodA(void *env, jobject o, jmethodID m,
                                     const fake_jvalue *args) {
    const char *name = method_name(m);
    if (name && (!strcmp(name, "post") || !strcmp(name, "postDelayed"))) {
        jobject runnable = args ? args[0].l : NULL;
        uint64_t delay = name && !strcmp(name, "postDelayed") && args
                       ? (uint64_t)args[1].j : 0;
        post_ui_task(runnable, delay);
        return 1;
    }
    return 0;
}
static jlong f_CallLongMethodV(void *env, jobject o, jmethodID m, va_list ap) {
    const char *name = method_name(m);
    if (o == (jobject)&g_boxed_long_obj && name && !strcmp(name, "longValue"))
        return g_frame_time_nanos;
    jlong out = 0;
    if (pad_long_getter(o, name, &out)) return out;
    return 0;
}
static jlong f_CallLongMethod(void *env, jobject o, jmethodID m, ...) {
    va_list ap; va_start(ap, m); jlong r = f_CallLongMethodV(env, o, m, ap);
    va_end(ap); return r;
}
static jlong f_CallLongMethodA(void *env, jobject o, jmethodID m,
                               const fake_jvalue *args) {
    const char *name = method_name(m);
    if (o == (jobject)&g_boxed_long_obj && name && !strcmp(name, "longValue"))
        return g_frame_time_nanos;
    jlong out = 0;
    if (pad_long_getter(o, name, &out)) return out;
    return 0;
}
// getRefreshRate/DisplayMetrics — devolver 60 p/ o timer nao dividir por zero.
// Os eixos do pad e os MotionRange tem precedencia sobre esse default.
static jfloat f_CallFloatMethodV(void *env, jobject o, jmethodID m, va_list ap) {
    jfloat out = 0.0f;
    va_list probe; va_copy(probe, ap);
    jint a0 = va_arg(probe, jint);
    va_end(probe);
    if (pad_float_getter(o, method_name(m), a0, &out)) return out;
    return 60.0f;
}
static jfloat f_CallFloatMethod(void *env, jobject o, jmethodID m, ...) {
    va_list ap; va_start(ap, m); jfloat r = f_CallFloatMethodV(env, o, m, ap);
    va_end(ap); return r;
}
static jfloat f_CallFloatMethodA(void *env, jobject o, jmethodID m,
                                 const fake_jvalue *args) {
    jfloat out = 0.0f;
    if (pad_float_getter(o, method_name(m), args ? args[0].i : 0, &out)) return out;
    return 60.0f;
}
static jint f_CallIntMethodA(void *env, jobject o, jmethodID m,
                             const fake_jvalue *args) {
    jint out = 0;
    if (pad_int_getter(o, method_name(m), args ? args[0].i : 0, &out)) return out;
    return 0;
}
static jint f_CallStaticIntMethodV(void *env, jclass c, jmethodID m, va_list ap) { return 0; }
static jint f_CallStaticIntMethod(void *env, jclass c, jmethodID m, ...) { return 0; }
static jint f_CallStaticIntMethodA(void *env, jclass c, jmethodID m,
                                   const fake_jvalue *args) { return 0; }
static jboolean f_CallStaticBooleanMethodV(void *env, jclass c, jmethodID m, va_list ap) { return 0; }
static jboolean f_CallStaticBooleanMethod(void *env, jclass c, jmethodID m, ...) { return 0; }
static jboolean f_CallStaticBooleanMethodA(void *env, jclass c, jmethodID m,
                                           const fake_jvalue *args) { return 0; }

static jobject f_NewObjectV(void *env, jclass c, jmethodID m, va_list ap) {
    const char *class_name = c ? (const char *)c : NULL;
    const char *sig = method_signature(m);
    if (getenv("FF5_JNI_TRACE"))
        fprintf(stderr, "[jni] NewObjectV class=%s method=%s\n",
                class_name ? class_name : "(null)",
                method_name(m) ? method_name(m) : "(null)");
    if (class_name && !strcmp(class_name, "android/os/HandlerThread"))
        return (jobject)&g_handler_thread_obj;
    if (class_name && !strcmp(class_name, "android/os/Handler")) {
        if (sig && strstr(sig, "Handler$Callback")) {
            (void)va_arg(ap, jobject); /* Looper */
            jobject callback = va_arg(ap, jobject);
            if (is_proxy(callback)) g_handler_callback = (fake_proxy *)callback;
        }
        return (jobject)&g_handler_obj;
    }
    if (class_name && !strcmp(class_name, "android/os/Message"))
        return (jobject)&g_message_obj;
    if (class_name && !strcmp(class_name, "com/unity3d/plugin/lvl/ServiceBinder")) {
        g_lvl_service_binder.response_code = 0;
        g_lvl_service_binder.signed_data = NULL;
        g_lvl_service_binder.signature = NULL;
        g_lvl_service_binder.done = NULL;
        return (jobject)&g_lvl_service_binder;
    }
    return (jobject)&g_activity_obj;
}
static jobject f_NewObject(void *env, jclass c, jmethodID m, ...) {
    va_list ap; va_start(ap, m); jobject r = f_NewObjectV(env, c, m, ap);
    va_end(ap); return r;
}
static jobject f_NewObjectA(void *env, jclass c, jmethodID m,
                            const fake_jvalue *args) {
    const char *class_name = c ? (const char *)c : NULL;
    const char *sig = method_signature(m);
    if (getenv("FF5_JNI_TRACE"))
        fprintf(stderr, "[jni] NewObjectA class=%s method=%s\n",
                class_name ? class_name : "(null)",
                method_name(m) ? method_name(m) : "(null)");
    if (class_name && !strcmp(class_name, "android/os/HandlerThread"))
        return (jobject)&g_handler_thread_obj;
    if (class_name && !strcmp(class_name, "android/os/Handler")) {
        if (sig && strstr(sig, "Handler$Callback") && args &&
            is_proxy(args[1].l))
            g_handler_callback = (fake_proxy *)args[1].l;
        return (jobject)&g_handler_obj;
    }
    if (class_name && !strcmp(class_name, "android/os/Message"))
        return (jobject)&g_message_obj;
    if (class_name && !strcmp(class_name,
                              "com/unity3d/plugin/lvl/ServiceBinder")) {
        g_lvl_service_binder.response_code = 0;
        g_lvl_service_binder.signed_data = NULL;
        g_lvl_service_binder.signature = NULL;
        g_lvl_service_binder.done = NULL;
        return (jobject)&g_lvl_service_binder;
    }
    return (jobject)&g_activity_obj;
}

// ---- strings -------------------------------------------------------------
static jstring f_NewString(void *env, const uint16_t *chars, jint length) {
    if (!chars || length <= 0) return make_jstring("");
    /* Os nomes/assinaturas JNI e caminhos usados no boot sao ASCII. Ainda
     * codificamos todo BMP basico para nao devolver NULL quando
     * AndroidJNI.NewString (slot 163), em vez de NewStringUTF, e' usado. */
    size_t capacity = (size_t)length * 3u + 1u;
    char *utf8 = (char *)malloc(capacity);
    size_t out = 0;
    for (jint i = 0; i < length; i++) {
        uint16_t ch = chars[i];
        if (ch < 0x80u) {
            utf8[out++] = (char)ch;
        } else if (ch < 0x800u) {
            utf8[out++] = (char)(0xc0u | (ch >> 6));
            utf8[out++] = (char)(0x80u | (ch & 0x3fu));
        } else {
            utf8[out++] = (char)(0xe0u | (ch >> 12));
            utf8[out++] = (char)(0x80u | ((ch >> 6) & 0x3fu));
            utf8[out++] = (char)(0x80u | (ch & 0x3fu));
        }
    }
    utf8[out] = '\0';
    return (jstring)utf8;
}
static jint f_GetStringLength(void *env, jstring s) {
    return s ? (jint)strlen((const char *)s) : 0;
}
static const uint16_t *f_GetStringChars(void *env, jstring s, jboolean *iscopy) {
    const unsigned char *utf8 = (const unsigned char *)(s ? s : "");
    size_t n = strlen((const char *)utf8);
    uint16_t *chars = (uint16_t *)calloc(n + 1u, sizeof *chars);
    size_t in = 0, out = 0;
    while (in < n) {
        unsigned ch = utf8[in++];
        if ((ch & 0xe0u) == 0xc0u && in < n)
            ch = ((ch & 0x1fu) << 6) | (utf8[in++] & 0x3fu);
        else if ((ch & 0xf0u) == 0xe0u && in + 1u < n) {
            unsigned continuation1 = utf8[in++] & 0x3fu;
            unsigned continuation2 = utf8[in++] & 0x3fu;
            ch = ((ch & 0x0fu) << 12) |
                 (continuation1 << 6) | continuation2;
        }
        chars[out++] = (uint16_t)ch;
    }
    if (iscopy) *iscopy = 1;
    return chars;
}
static void f_ReleaseStringChars(void *env, jstring s, const uint16_t *chars) {
    free((void *)chars);
}
static jstring f_NewStringUTF(void *env, const char *s) { return make_jstring(s); }
static jint f_GetStringUTFLength(void *env, jstring s) { return s ? (jint)strlen((char *)s) : 0; }
static const char *f_GetStringUTFChars(void *env, jstring s, jboolean *iscopy) {
    if (iscopy) *iscopy = 0;
    return s ? (const char *)s : "";
}
static void f_ReleaseStringUTFChars(void *env, jstring s, const char *c) {}

// ---- arrays (bridge byte[] do AssetManager/RMS) --------------------------
#define BYTE_ARRAY_MAGIC 0x42594152u /* "BYAR" */
typedef struct fake_byte_array {
    uint32_t magic;
    jint length;
    jbyte bytes[];
} fake_byte_array;

static fake_byte_array *as_byte_array(jarray a) {
    if (!a || (uintptr_t)a < 0x10000) return NULL;
    fake_byte_array *b = (fake_byte_array *)a;
    return b->magic == BYTE_ARRAY_MAGIC ? b : NULL;
}
static jint f_GetArrayLength(void *env, jarray a) {
    if (a == (jarray)&g_handlemessage_args || a == (jarray)&g_doframe_args)
        return 1;
    fake_int_array *ia = as_int_array(a);
    if (ia) return ia->length;
    fake_byte_array *b = as_byte_array(a);
    return b ? b->length : 0;
}
static jint *f_GetIntArrayElements(void *env, jarray a, jboolean *c) {
    if (c) *c = 0;
    fake_int_array *ia = as_int_array(a);
    return ia ? ia->v : NULL;
}
static void f_GetIntArrayRegion(void *env, jarray a, jint start, jint len, jint *buf) {
    fake_int_array *ia = as_int_array(a);
    if (!ia || !buf || start < 0 || len < 0 || start + len > ia->length) return;
    memcpy(buf, ia->v + start, (size_t)len * sizeof(jint));
}
static jobject f_GetObjectArrayElement(void *env, jarray a, jint i) {
    if (i != 0) return NULL;
    if (a == (jarray)&g_handlemessage_args) return (jobject)&g_message_obj;
    if (a == (jarray)&g_doframe_args) return (jobject)&g_boxed_long_obj;
    return NULL;
}
static jarray f_NewByteArray(void *env, jint n) {
    if (n < 0) return NULL;
    fake_byte_array *b = calloc(1, sizeof *b + (size_t)(n > 0 ? n : 1));
    if (!b) return NULL;
    b->magic = BYTE_ARRAY_MAGIC;
    b->length = n;
    return (jarray)b;
}
static jbyte *f_GetByteArrayElements(void *env, jarray a, jboolean *c) {
    if (c) *c = 0;
    fake_byte_array *b = as_byte_array(a);
    return b ? b->bytes : NULL;
}
static void f_ReleaseArrayElements(void *env, jarray a, void *e, jint mode) {}
static void f_GetByteArrayRegion(void *env, jarray a, jint s, jint l, void *buf) {
    fake_byte_array *b = as_byte_array(a);
    if (b && buf && s >= 0 && l >= 0 && s <= b->length && l <= b->length - s)
        memcpy(buf, b->bytes + s, (size_t)l);
}
static void f_SetByteArrayRegion(void *env, jarray a, jint s, jint l, const void *buf) {
    fake_byte_array *b = as_byte_array(a);
    if (b && buf && s >= 0 && l >= 0 && s <= b->length && l <= b->length - s)
        memcpy(b->bytes + s, buf, (size_t)l);
}

jint jni_get_array_length(jarray array) { return f_GetArrayLength(g_env, array); }

// ---- NIO -----------------------------------------------------------------
static void *f_GetDirectBufferAddress(void *env, jobject buf) { return buf; }
static jlong f_GetDirectBufferCapacity(void *env, jobject buf) { return 0; }

// ---- registro / vm -------------------------------------------------------
typedef struct { const char *name; const char *sig; void *fn; } JNINativeMethod;
static jint f_RegisterNatives(void *env, jclass c, const JNINativeMethod *m, jint n) {
    for (jint i = 0; i < n && g_nnatives < 512; i++) {
        if (getenv("FF5_JNI_TRACE"))
            fprintf(stderr, "[jni]   native %s %s -> %p\n", m[i].name,
                    m[i].sig ? m[i].sig : "", m[i].fn);
        g_natives[g_nnatives].name = strdup(m[i].name);
        g_natives[g_nnatives].sig = strdup(m[i].sig ? m[i].sig : "");
        g_natives[g_nnatives].fn = (uintptr_t)m[i].fn;
        g_nnatives++;
    }
    fprintf(stderr, "[jni] RegisterNatives: +%d (total %d)\n", n, g_nnatives);
    return 0;
}
static jint f_GetJavaVM(void *env, void **vm) { *vm = g_vm; return 0; }

// JavaVM vtable
static jint vm_GetEnv(void *vm, void **penv, jint ver) { *penv = g_env; return 0; }
static jint vm_AttachCurrentThread(void *vm, void **penv, void *args) { *penv = g_env; return 0; }
static jint vm_DetachCurrentThread(void *vm) { return 0; }
static jint vm_DestroyJavaVM(void *vm) { return 0; }

// ---- montagem das vtables ------------------------------------------------
void jni_init(const char *package, const char *gamedir) {
    g_ui_thread = pthread_self();
    pad_init_objects();
    snprintf(g_pkg, sizeof g_pkg, "%s", package ? package : "com.square_enix.android_googleplay.FFPR5");
    snprintf(g_gamedir, sizeof g_gamedir, "%s", gamedir ? gamedir : ".");
    snprintf(g_userdata, sizeof g_userdata, "%s/userdata", g_gamedir);

    for (int i = 0; i < 300; i++) env_vt[i] = (void *)jni_stub;

    env_vt[4]   = (void *)f_GetVersion;
    env_vt[6]   = (void *)f_FindClass;
    env_vt[7]   = (void *)f_ref_id;    // FromReflectedMethod
    env_vt[8]   = (void *)f_FromReflectedField;
    env_vt[15]  = (void *)f_ExceptionOccurred;
    env_vt[17]  = (void *)f_ExceptionClear;
    env_vt[21]  = (void *)f_ref_id;    // NewGlobalRef
    env_vt[22]  = (void *)f_ref_void;  // DeleteGlobalRef
    env_vt[23]  = (void *)f_ref_void;  // DeleteLocalRef
    env_vt[24]  = (void *)f_IsSameObject;
    env_vt[25]  = (void *)f_ref_id;    // NewLocalRef
    env_vt[28]  = (void *)f_NewObject;
    env_vt[29]  = (void *)f_NewObjectV;
    env_vt[30]  = (void *)f_NewObjectA;
    env_vt[31]  = (void *)f_GetObjectClass;
    env_vt[32]  = (void *)f_IsInstanceOf;
    env_vt[33]  = (void *)f_GetMethodID;
    env_vt[34]  = (void *)f_CallObjectMethod;
    env_vt[35]  = (void *)f_CallObjectMethodV;
    env_vt[36]  = (void *)f_CallObjectMethodA;
    env_vt[37]  = (void *)f_CallBooleanMethod;
    env_vt[38]  = (void *)f_CallBooleanMethodV;
    env_vt[39]  = (void *)f_CallBooleanMethodA;
    env_vt[49]  = (void *)f_CallIntMethod;
    env_vt[50]  = (void *)f_CallIntMethodV;
    env_vt[51]  = (void *)f_CallIntMethodA;
    env_vt[52]  = (void *)f_CallLongMethod;
    env_vt[53]  = (void *)f_CallLongMethodV;
    env_vt[54]  = (void *)f_CallLongMethodA;
    env_vt[55]  = (void *)f_CallFloatMethod;
    env_vt[56]  = (void *)f_CallFloatMethodV;
    env_vt[57]  = (void *)f_CallFloatMethodA;
    env_vt[61]  = (void *)f_CallVoidMethod;
    env_vt[62]  = (void *)f_CallVoidMethodV;
    env_vt[63]  = (void *)f_CallVoidMethodA;
    env_vt[94]  = (void *)f_GetFieldID;
    env_vt[95]  = (void *)f_GetObjectField;
    env_vt[100] = (void *)f_GetIntField;
    env_vt[102] = (void *)f_GetFloatField;
    env_vt[113] = (void *)f_GetMethodID;      // GetStaticMethodID (mesmo modelo)
    env_vt[114] = (void *)f_CallStaticObjectMethod;
    env_vt[115] = (void *)f_CallStaticObjectMethodV;
    env_vt[116] = (void *)f_CallStaticObjectMethodA;
    env_vt[117] = (void *)f_CallStaticBooleanMethod;
    env_vt[118] = (void *)f_CallStaticBooleanMethodV;
    env_vt[119] = (void *)f_CallStaticBooleanMethodA;
    env_vt[129] = (void *)f_CallStaticIntMethod;
    env_vt[130] = (void *)f_CallStaticIntMethodV;
    env_vt[131] = (void *)f_CallStaticIntMethodA;
    env_vt[141] = (void *)f_CallStaticVoidMethod;
    env_vt[142] = (void *)f_CallStaticVoidMethodV;
    env_vt[143] = (void *)f_CallStaticVoidMethodA;
    env_vt[144] = (void *)f_GetFieldID;       // GetStaticFieldID
    env_vt[145] = (void *)f_GetStaticObjectField;
    env_vt[150] = (void *)f_GetStaticIntField;
    env_vt[163] = (void *)f_NewString;
    env_vt[164] = (void *)f_GetStringLength;
    env_vt[165] = (void *)f_GetStringChars;
    env_vt[166] = (void *)f_ReleaseStringChars;
    env_vt[167] = (void *)f_NewStringUTF;
    env_vt[168] = (void *)f_GetStringUTFLength;
    env_vt[169] = (void *)f_GetStringUTFChars;
    env_vt[170] = (void *)f_ReleaseStringUTFChars;
    env_vt[171] = (void *)f_GetArrayLength;
    env_vt[173] = (void *)f_GetObjectArrayElement;
    env_vt[176] = (void *)f_NewByteArray;
    env_vt[184] = (void *)f_GetByteArrayElements;
    env_vt[187] = (void *)f_GetIntArrayElements;
    env_vt[195] = (void *)f_ReleaseArrayElements;   /* ReleaseIntArrayElements */
    env_vt[203] = (void *)f_GetIntArrayRegion;
    env_vt[192] = (void *)f_ReleaseArrayElements;
    env_vt[200] = (void *)f_GetByteArrayRegion;
    env_vt[208] = (void *)f_SetByteArrayRegion;
    env_vt[205] = (void *)f_ExceptionCheck;
    env_vt[215] = (void *)f_RegisterNatives;
    env_vt[219] = (void *)f_GetJavaVM;
    env_vt[230] = (void *)f_GetDirectBufferAddress;
    env_vt[231] = (void *)f_GetDirectBufferCapacity;

    for (int i = 0; i < 16; i++) vm_vt[i] = (void *)jni_stub;
    vm_vt[3] = (void *)vm_DestroyJavaVM;
    vm_vt[4] = (void *)vm_AttachCurrentThread;
    vm_vt[5] = (void *)vm_DetachCurrentThread;
    vm_vt[6] = (void *)vm_GetEnv;
    vm_vt[7] = (void *)vm_AttachCurrentThread;  // AsDaemon
}

void *jni_get_vm(void) { return g_vm; }
void *jni_get_env(void) { return g_env; }
void *jni_get_activity(void) { return (void *)&g_activity_obj; }
