/*
 * falsojni.h — JNIEnv/JavaVM falsos para o so-loader do FF V.
 * Escrito do zero p/ a superficie REAL que o libunity/il2cpp do FFPR5 chama
 * (UnityPlayer padrao + Play Asset Delivery + SharedPreferences). Sem premissas
 * de outros jogos: a tabela de metodos cresce por demanda.
 *
 * Modelo: jclass/jmethodID/jfieldID = ponteiro pra string internada (nome).
 * Os dispatchers de Call*Method casam pelo nome do metodo.
 */
#ifndef FF5_FALSOJNI_H
#define FF5_FALSOJNI_H

#include <stdint.h>
#include <stdarg.h>

typedef void *jobject;
typedef void *jclass;
typedef void *jstring;
typedef void *jarray;
typedef void *jthrowable;
typedef void *jmethodID;
typedef void *jfieldID;
typedef int32_t jint;
typedef int8_t jbyte;
typedef uint8_t jboolean;
typedef int64_t jlong;
typedef float jfloat;
typedef double jdouble;

// Inicializa vm+env falsos. Passar o nome do pacote (FFPR5) e o gamedir
// (usado nos dispatchers de path: getFilesDir/getAssetPackPath/save).
void jni_init(const char *package, const char *gamedir);
void *jni_get_vm(void);
void *jni_get_env(void);
// Objeto "Activity"/"this" que passamos aos metodos nativos do UnityPlayer.
void *jni_get_activity(void);
// Executa na thread UI os Runnable postados por Activity.runOnUiThread/Handler.
void jni_pump_ui_tasks(void);

// Depois do JNI_OnLoad: acha um metodo nativo registrado via RegisterNatives.
uintptr_t jni_find_native(const char *name);
uintptr_t jni_find_native_signature(const char *signature);
jint jni_get_array_length(jarray array);
void jni_dump_natives(void);

/* Gamepad nativo: monta o KeyEvent/MotionEvent que a Unity le por JNI dentro
 * de nativeInjectEvent. `action` = 0 (ACTION_DOWN) ou 1 (ACTION_UP);
 * `keycode` = KeyEvent.KEYCODE_* do Android. Os eixos seguem MotionEvent
 * .AXIS_* (indice = numero do eixo). Os objetos sao estaticos: a Unity le os
 * campos durante a chamada e nao guarda a referencia. */
jobject jni_pad_key_event(int action, int keycode, int repeat);
jobject jni_pad_motion_event(const float *axes, int naxes);
/* Toque de tela (SOURCE_TOUCHSCREEN). action: 0=DOWN 1=UP 2=MOVE. */
jobject jni_touch_event(int action, float x, float y);

#endif
