/*
 * main.c — so-loader do Final Fantasy V (Pixel Remaster), Unity 2019.4 il2cpp,
 * armv7 softfp -> Mali-450 fbdev GLES2. Escrito do zero p/ o FFPR5.
 *
 * Fluxo NATIVO do UnityPlayer (nada de forcar estado):
 *   dlopen libs sistema -> shims -> libRMS/JNI_OnLoad -> libmain/JNI_OnLoad
 *   -> libunity/JNI_OnLoad
 *   -> libil2cpp -> initJni(Context) na thread UI -> cria a janela/surface
 *   -> thread UnityMain: recreate/surfaceChanged/focus/resume -> nativeRender.
 * O launcher apenas reproduz o lifecycle Java do APK; a Unity continua dona dos
 * estados do engine, dos jobs e do swap. Saida = SELECT+START.
 */
#define _GNU_SOURCE 1
#include "so_util.h"
#include "falsojni.h"
#include "egl_shim.h"
#include "imports.h"
#include "bionic_shim.h"
#include "native_pad.h"
#include "etc1_dual.h"

#include <SDL2/SDL.h>
#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <execinfo.h>
#include <link.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>
#include <sys/mman.h>

// ---- shims fornecidos pela camada de infra (subagente) -------------------
extern void bionic_set_redirect(const char *gamedir);
extern void android_shim_set_datadir(const char *dir);
extern void opensles_shim_pump_callbacks(void);   // bombeia os buffer-queue do audio
extern void *softfp_gl_resolve(const char *name);
void *ff5_gl_override(const char *name);

// ---- modulos do jogo -----------------------------------------------------
static so_module *g_m_rms, *g_m_main, *g_m_unity, *g_m_il2cpp, *g_m_sead;
static char g_gamedir[512];
static uintptr_t g_unity_base, g_il2cpp_base;
uintptr_t ff5_unity_base(void) { return g_unity_base; }
uintptr_t ff5_il2cpp_base(void) { return g_il2cpp_base; }

/* Botao SDL -> KeyEvent.KEYCODE_* do Android. O FF5 le os KEYCODE_BUTTON_*
 * pelo InputObserver dele, entao esta e' a tabela que o jogo espera; a posicao
 * fisica vem do SDL_GameController (BTN_SOUTH = A), nao do rotulo do db. */
static int pad_button_keycode(int sdl_button) {
    switch (sdl_button) {
    case SDL_CONTROLLER_BUTTON_A:             return 96;  /* BUTTON_A  */
    case SDL_CONTROLLER_BUTTON_B:             return 97;  /* BUTTON_B  */
    case SDL_CONTROLLER_BUTTON_X:             return 99;  /* BUTTON_X  */
    case SDL_CONTROLLER_BUTTON_Y:             return 100; /* BUTTON_Y  */
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  return 102; /* BUTTON_L1 */
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return 103; /* BUTTON_R1 */
    case SDL_CONTROLLER_BUTTON_LEFTSTICK:     return 106; /* BUTTON_THUMBL */
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK:    return 107; /* BUTTON_THUMBR */
    case SDL_CONTROLLER_BUTTON_START:         return 108; /* BUTTON_START  */
    case SDL_CONTROLLER_BUTTON_BACK:          return 109; /* BUTTON_SELECT */
    case SDL_CONTROLLER_BUTTON_GUIDE:         return 110; /* BUTTON_MODE   */
    case SDL_CONTROLLER_BUTTON_DPAD_UP:       return 19;  /* DPAD_UP    */
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:     return 20;  /* DPAD_DOWN  */
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:     return 21;  /* DPAD_LEFT  */
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:    return 22;  /* DPAD_RIGHT */
    default: return 0;
    }
}

/* Eixo SDL -> MotionEvent.AXIS_* do Android. */
static int pad_axis_index(int sdl_axis) {
    switch (sdl_axis) {
    case SDL_CONTROLLER_AXIS_LEFTX:        return 0;   /* AXIS_X  */
    case SDL_CONTROLLER_AXIS_LEFTY:        return 1;   /* AXIS_Y  */
    case SDL_CONTROLLER_AXIS_RIGHTX:       return 11;  /* AXIS_Z  */
    case SDL_CONTROLLER_AXIS_RIGHTY:       return 14;  /* AXIS_RZ */
    case SDL_CONTROLLER_AXIS_TRIGGERLEFT:  return 17;  /* AXIS_LTRIGGER */
    case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: return 18;  /* AXIS_RTRIGGER */
    default: return -1;
    }
}

/* Tracepoints de uma execucao, ativados apenas por FF5_JOB_TRACE. Eles usam
 * UDF Thumb, restauram a instrucao original no primeiro hit e deixam o fluxo
 * continuar exatamente de onde parou. Assim conseguimos provar quais etapas
 * do JobSystem nativo realmente executaram sem pular waits ou completar jobs. */
typedef struct job_tracepoint {
    uintptr_t offset;
    uint16_t original;
    const char *name;
    volatile int armed;
} job_tracepoint;

static job_tracepoint g_job_tracepoints[] = {
    { 0x16f75a, 0, "JobSystem::Initialize", 0 },
    { 0x16f8de, 0, "JobSystem::CreateQueue call", 0 },
    { 0x1710d6, 0, "CreateJobQueue", 0 },
    { 0x16fe84, 0, "JobQueue constructor", 0 },
    { 0x2541d4, 0, "Thread::Run", 0 },
    { 0x254206, 0, "before pthread_attr_init", 0 },
    { 0x188408, 0, "synchronous UI callback", 0 },
    { 0x1884d2, 0, "WaitForJobGroup loop", 0 },
    { 0x190688, 0, "InitializeEngine result", 0 },
    { 0x190730, 0, "globalgamemanagers result", 0 },
    { 0x190768, 0, "player settings result", 0 },
    { 0x256510, 0, "AlertDialog entry", 0 },
};

typedef struct il2cpp_tracepoint {
    uintptr_t offset;
    uint32_t original;
    const char *name;
    volatile int armed;
    int deferred;
} il2cpp_tracepoint;

/* Entradas ARM do bootstrap IL2CPP. O trace e de um disparo so e restaura a
 * instrucao original antes de repeti-la; portanto observa o fluxo sem chamar,
 * pular ou substituir nenhuma etapa nativa. */
static il2cpp_tracepoint g_il2cpp_tracepoints[] = {
    { 0x640598, 0, "codegen callback", 0 },
    { 0x64ee60, 0, "il2cpp_codegen_register", 0 },
    { 0x68f374, 0, "run codegen callbacks", 0 },
    { 0x64efec, 0, "MetadataCache::Initialize", 0 },
    { 0x64faf0, 0, "metadata usage pair", 0 },
    { 0x640edc, 0, "Assembly::Load", 0 },
    /* Fronteiras managed reais do boot. Mantidas opt-in: observam onde a
     * coroutine/Addressables para sem adiantar nem completar nenhuma etapa. */
    { 0x7e9bb8, 0, "SceneBoot.Start", 0 },
    { 0x7e9c98, 0, "SceneBoot.CreateInstance", 0 },
    { 0x7eb3e0, 0, "SceneBoot.Load.MoveNext", 0 },
    { 0x7eb810, 0, "Boot.AssetPath.MoveNext", 0 },
    { 0x7ebb30, 0, "Boot.LoadScene.MoveNext", 0 },
    { 0xb686d4, 0, "ResourceManager.Initialize", 0 },
    { 0xb68894, 0, "Addressables completed", 0 },
    { 0x9b6cf0, 0, "AssetDelivery.TryInitialize", 0 },
    { 0x9b801c, 0, "AssetPackProvider.Provide", 0 },
    { 0x127cc68, 0, "ContentCatalog.Provide", 0 },
    { 0x127cdf8, 0, "ContentCatalog.Start", 0 },
    { 0x127d9d4, 0, "ContentCatalog.LoadCatalog", 0 },
    { 0x127e0c4, 0, "ContentCatalog.OnLoaded", 0 },
    { 0x127e308, 0, "ContentCatalog.load callback", 0 },
    { 0x1279454, 0, "Initialization.OnCatalog", 0 },
    { 0xe8604c, 0, "Json.FromJson<catalog>", 0 },
    { 0x1ec75d8, 0, "Json.FromJson(type)", 0 },
    { 0x1ec76d8, 0, "Json native invoke", 0 },
    { 0xe860d8, 0, "Json native returned", 0 },
    { 0x18a60c0, 0, "TextDataProvider.Provide", 0 },
    { 0x18a6174, 0, "TextDataProvider.Start", 0 },
    { 0x18a66c4, 0, "TextDataProvider.completed", 0 },
    { 0x18a3828, 0, "JsonAssetProvider.Convert", 0 },
    /* Apos os logos, a cena mantem uma cobertura branca enquanto espera o
     * preload do titulo e o retorno do cloud-save. Observe as fronteiras e os
     * estados reais; nao complete nem substitua nenhuma delas. */
    { 0x7d3694, 0, "SceneTitle.CreateInstance", 0 },
    { 0x7d41a4, 0, "SceneTitle.Update", 0 },
    { 0x7d4138, 0, "SceneTitle.SubSceneWait", 0 },
    { 0x7d42d0, 0, "SceneTitle.CloudSaveFinish", 0 },
    { 0x7d42e0, 0, "SceneTitle.InitCompleted", 0 },
    { 0x7d43f8, 0, "SceneTitle.RequestPreload", 0 },
    { 0x7d4718, 0, "Title.SubSceneWait.MoveNext", 0 },
    { 0x7d49b0, 0, "SceneTitleScreen.Create", 0 },
    { 0x7d56f4, 0, "TitleScreen.LoadResourceWait", 0 },
    { 0x7d5248, 0, "TitleScreen.CreateWindow", 0 },
    { 0x7d6724, 0, "Title.LoadResource.MoveNext", 0 },
    { 0x93b2e4, 0, "Title.ResourceLoader.IsAlmostDone", 0 },
    { 0x93bd44, 0, "Title.ResourceLoader.Preload", 0 },
    { 0x93be10, 0, "Title.ResourceLoader.IsCompleted", 0 },
    { 0x93be18, 0, "Title.ResourceLoader.PreloadFinished", 0 },
    { 0x5dcd84, 0, "Title.Preload.MoveNext", 0 },
    /* O branco ainda pertence ao fim da SceneSplash ate ChangeScene concluir.
     * Pontos internos do Update distinguem cada predicado sem alterar os
     * flags, inclusive a espera da coroutine de fade branco. */
    { 0x7ec7b0, 0, "SceneSplash.Start", 0 },
    { 0x7ec84c, 0, "SceneSplash.CreateInstance", 0 },
    { 0x7ed208, 0, "SceneSplash.Update", 0 },
    { 0x7ed24c, 0, "Splash animation finished", 0 },
    { 0x7ed254, 0, "Splash message ready", 0 },
    { 0x7ed264, 0, "Splash load finished", 0 },
    { 0x7ed290, 0, "Splash scene load complete", 0 },
    { 0x7ed2fc, 0, "Splash fade coroutine start", 0 },
    { 0x7ed43c, 0, "SceneSplash.FadeoutFinished", 0 },
    { 0x7ed9d0, 0, "SceneSplash.AnimationFinished", 0 },
    { 0x7ed9fc, 0, "SceneSplash.GotoTitle", 0 },
    { 0x7ee650, 0, "Splash.GotoSceneTask.MoveNext", 0 },
    { 0x7edbdc, 0, "Splash.BlackFade.MoveNext", 0 },
    { 0x7dc7b4, 0, "AndroidLvl.UpdateAndroidLvl", 0 },
    { 0x7dc860, 0, "AndroidLvl.response dispatch", 0 },
    /* Callback do ServiceBinder (0x7db500). Ele le _arg0/_arg1/_arg2 e decide
     * o veredito em r9, gravado uma unica vez em this->+0x38 (0x7db660).
     * Estes pontos mostram qual predicado reprovou, sem alterar nenhum. */
    { 0x7db554, 0, "LVL cb: le _binder", 0 },
    { 0x7db614, 0, "LVL cb: campos lidos", 0 },
    { 0x7db620, 0, "LVL cb: checa signedData", 0 },
    { 0x7db634, 0, "LVL cb: checa signature", 0 },
    { 0x7db648, 0, "LVL cb: RAMO DE ERRO", 0 },
    { 0x7db66c, 0, "LVL cb: entra na validacao", 0 },
    { 0x7db660, 0, "LVL cb: grava veredito", 0 },
    { 0x7ede8c, 0, "Splash.WhiteFade.MoveNext", 0 },
    { 0x7edf20, 0, "Splash white fade completed", 0 },
    { 0xb90218, 0, "SceneManager.LoadCompleted", 0 },
    { 0xb90330, 0, "SceneManager.ChangeScene", 0 },
    /* Estados reais do async SceneSplash.Load. Cada await grava 0..4 no
     * state-machine; os pontos pending/completed mostram qual dependencia
     * deixou de chamar a continuacao. */
    { 0x7ee8ac, 0, "Splash.Load.MoveNext", 0 },
    { 0x7ee9e8, 0, "Splash.Load initial", 0 },
    { 0x7efac4, 0, "Splash.Load await0 pending", 0 },
    { 0x7eeb88, 0, "Splash.Load await0 completed", 0 },
    /* Apos IsCompleted, UniTask ainda chama GetResult no source virtual.
     * Cercamos a resolucao e o BLX para distinguir init de classe, dispatch e
     * retorno sem fabricar a conclusao da tarefa. */
    { 0x7eebb4, 0, "Splash await0 GetResult prep", 0 },
    { 0x7eec28, 0, "Splash await0 target loaded", 0 },
    { 0x7eec34, 0, "Splash await0 before GetResult", 0 },
    { 0x7eec3c, 0, "Splash await0 after GetResult", 0 },
    { 0x7eec64, 0, "Splash after await0 class init", 0 },
    { 0x7eec70, 0, "Splash before indicator getter", 0 },
    { 0x7eec74, 0, "Splash after indicator getter", 0 },
    { 0x16ba650, 0, "FontManager.GetResult", 0 },
    { 0x130f8f0, 0, "Font core error loaded", 0, 2 },
    { 0x130f914, 0, "Font error cancel check", 0, 2 },
    { 0x130f91c, 0, "Font error exception check", 0, 2 },
    { 0x130f930, 0, "Font ExceptionHolder check", 0, 2 },
    { 0x130f938, 0, "Font ExceptionHolder matched", 0, 2 },
    { 0x130f93c, 0, "Font exception extracted", 0, 2 },
    { 0x130f958, 0, "Font throwing exception", 0, 2 },
    { 0x130f99c, 0, "Font throwing cancellation", 0, 2 },
    { 0x10bd184, 0, "Font before WWW ctor", 0 },
    { 0x10bd358, 0, "Font WWW bytes returned", 0 },
    { 0x10bd5cc, 0, "Font before LoadFromMemory", 0 },
    { 0x10bd5d8, 0, "Font after LoadFromMemory", 0 },
    { 0x7efa78, 0, "Splash.Load await1 pending", 0 },
    { 0x7eef94, 0, "Splash.Load await1 completed", 0 },
    { 0x7ef0ac, 0, "Splash message flag write", 0 },
    { 0x7efa34, 0, "Splash.Load await2 pending", 0 },
    { 0x7ef268, 0, "Splash.Load await2 completed", 0 },
    { 0x7ef970, 0, "Splash.Load await3 pending", 0 },
    { 0x7ef8e0, 0, "Splash.Load await3 completed", 0 },
    { 0x7efd34, 0, "Splash.Load await4 pending", 0 },
    { 0x7efc90, 0, "Splash.Load await4 completed", 0 },
    { 0x7efd2c, 0, "Splash load flag write", 0 },
    { 0x9f76a4, 0, "SystemIndicator.CreateText", 0 },
    /* CreateTextObject tambem e usado antes da splash. Estes pontos so sao
     * armados quando a continuacao de SceneSplash.Load chega ao BL unico
     * abaixo, para que os hits pertençam exatamente a segunda chamada. */
    { 0x7eec84, 0, "Splash before SystemIndicator", 0 },
    { 0x9f76a8, 0, "Indicator entered", 0, 1 },
    { 0x9f7770, 0, "Indicator component ready", 0, 1 },
    { 0x9f783c, 0, "Indicator text created", 0, 1 },
    { 0x9f78f8, 0, "Indicator text configured", 0, 1 },
    { 0x9f7940, 0, "Indicator font assigned", 0, 1 },
    { 0x9f79b0, 0, "Indicator layout built", 0, 1 },
    { 0x9f7af4, 0, "Indicator lists built", 0, 1 },
    { 0x9f7b70, 0, "Indicator returning", 0, 1 },
    { 0x7eec90, 0, "SystemIndicator.CreateText returned", 0 },
    { 0x72a6fc, 0, "MessageManager.Initialize", 0 },
    { 0x7eece4, 0, "MessageManager.Initialize returned", 0 },
    { 0x1869024, 0, "MasterManager.LoadAsset", 0 },
    { 0x7eed54, 0, "MasterManager.LoadAsset returned", 0 },
    { 0x72a898, 0, "MessageManager.LoadAsset", 0 },
    { 0x7eedbc, 0, "MessageManager.LoadAsset returned", 0 },
    { 0x117c4d4, 0, "MiscAssetDesc.Load", 0 },
    { 0x7eee40, 0, "MiscAssetDesc.Load returned", 0 },
    { 0x7eee9c, 0, "Splash before UniTask.WhenAll", 0 },
    { 0x130c474, 0, "UniTask.WhenAll", 0 },
    { 0x7eeec8, 0, "Splash after UniTask.WhenAll", 0 },
};

static void on_crash(int sig, siginfo_t *si, void *uc);

static void ff5_trace_managed_string(const char *label, uintptr_t value) {
    if (!value) {
        fprintf(stderr, "[il2trace] %s=(null)\n", label);
        return;
    }
    const uint32_t length = *(const uint32_t *)(value + 8);
    const uint16_t *chars = (const uint16_t *)(value + 12);
    const uint32_t shown = length < 512u ? length : 512u;
    fprintf(stderr, "[il2trace] %s[%u]=\"", label, length);
    for (uint32_t i = 0; i < shown; i++) {
        const uint16_t ch = chars[i];
        fputc(ch >= 0x20 && ch <= 0x7e ? (int)ch : '?', stderr);
    }
    if (shown != length) fputs("...", stderr);
    fputs("\"\n", stderr);
}

static void on_job_trace(int sig, siginfo_t *si, void *opaque) {
    ucontext_t *u = (ucontext_t *)opaque;
    uintptr_t pc = u ? (uintptr_t)u->uc_mcontext.arm_pc : 0;
    for (unsigned i = 0; i < sizeof g_job_tracepoints / sizeof g_job_tracepoints[0]; i++) {
        job_tracepoint *t = &g_job_tracepoints[i];
        uintptr_t site = g_unity_base + t->offset;
        if (t->armed && (pc == site || pc == site + 2)) {
            *(volatile uint16_t *)site = t->original;
            __builtin___clear_cache((char *)site, (char *)site + 2);
            t->armed = 0;
            fprintf(stderr,
                    "[jobtrace] %-25s +0x%lx r0=%08lx r1=%08lx r2=%08lx r3=%08lx r4=%08lx lr=%08lx\n",
                    t->name, (unsigned long)t->offset,
                    u->uc_mcontext.arm_r0, u->uc_mcontext.arm_r1,
                    u->uc_mcontext.arm_r2, u->uc_mcontext.arm_r3,
                    u->uc_mcontext.arm_r4, u->uc_mcontext.arm_lr);
            fflush(stderr);
            /* O kernel pode entregar o PC no BKPT ou logo depois dele. Em
             * ambos os casos voltamos ao halfword restaurado uma unica vez. */
            u->uc_mcontext.arm_pc = site;
            return;
        }
    }
    for (unsigned i = 0; i < sizeof g_il2cpp_tracepoints / sizeof g_il2cpp_tracepoints[0]; i++) {
        il2cpp_tracepoint *t = &g_il2cpp_tracepoints[i];
        uintptr_t site = g_il2cpp_base + t->offset;
        if (t->armed && pc == site) {
            *(volatile uint32_t *)site = t->original;
            __builtin___clear_cache((char *)site, (char *)site + 4);
            t->armed = 0;
            fprintf(stderr,
                    "[il2trace] %-25s +0x%lx tid=%lx r0=%08lx r1=%08lx r2=%08lx r3=%08lx lr=%08lx\n",
                    t->name, (unsigned long)t->offset,
                    (unsigned long)pthread_self(),
                    u->uc_mcontext.arm_r0, u->uc_mcontext.arm_r1,
                    u->uc_mcontext.arm_r2, u->uc_mcontext.arm_r3,
                    u->uc_mcontext.arm_lr);
            if (t->offset == 0x7eec84) {
                unsigned armed = 0;
                for (unsigned j = 0;
                     j < sizeof g_il2cpp_tracepoints / sizeof g_il2cpp_tracepoints[0];
                     j++) {
                    il2cpp_tracepoint *next = &g_il2cpp_tracepoints[j];
                    if (next->deferred != 1 || next->armed) continue;
                    uintptr_t next_site = g_il2cpp_base + next->offset;
                    *(volatile uint32_t *)next_site = 0xe7f000f0u;
                    __builtin___clear_cache((char *)next_site,
                                            (char *)next_site + 4);
                    next->armed = 1;
                    armed++;
                }
                fprintf(stderr,
                        "[il2trace] %u pontos internos do SystemIndicator armados\n",
                        armed);
            }
            if (t->offset == 0x16ba650) {
                unsigned armed = 0;
                for (unsigned j = 0;
                     j < sizeof g_il2cpp_tracepoints / sizeof g_il2cpp_tracepoints[0];
                     j++) {
                    il2cpp_tracepoint *next = &g_il2cpp_tracepoints[j];
                    if (next->deferred != 2 || next->armed) continue;
                    uintptr_t next_site = g_il2cpp_base + next->offset;
                    *(volatile uint32_t *)next_site = 0xe7f000f0u;
                    __builtin___clear_cache((char *)next_site,
                                            (char *)next_site + 4);
                    next->armed = 1;
                    armed++;
                }
                fprintf(stderr,
                        "[il2trace] %u pontos do FontManager.GetResult armados\n",
                        armed);
            }
            if (t->offset == 0x130f8f0) {
                const uintptr_t core = u->uc_mcontext.arm_r4;
                fprintf(stderr,
                        "[il2trace] font core=%p result=%p error=%p version=%u "
                        "unhandled=%u completed=%u continuation=%p state=%p\n",
                        (void *)core,
                        core ? (void *)(uintptr_t)*(const uint32_t *)core : NULL,
                        core ? (void *)(uintptr_t)*(const uint32_t *)(core + 4) : NULL,
                        core ? *(const uint16_t *)(core + 8) : 0,
                        core ? *(const uint8_t *)(core + 10) : 0,
                        core ? *(const uint32_t *)(core + 12) : 0,
                        core ? (void *)(uintptr_t)*(const uint32_t *)(core + 16) : NULL,
                        core ? (void *)(uintptr_t)*(const uint32_t *)(core + 20) : NULL);
            }
            if (t->offset == 0x10bd5cc) {
                const uintptr_t machine = u->uc_mcontext.arm_r7;
                const uintptr_t data = machine
                    ? *(const uint32_t *)(machine + 0x18) : 0;
                const uint32_t length = data
                    ? *(const uint32_t *)(data + 0x0c) : 0;
                const uint8_t *bytes = data
                    ? (const uint8_t *)(data + 0x10) : NULL;
                fprintf(stderr,
                        "[il2trace] font machine=%p decrypted=%p len=%u "
                        "head=%02x%02x%02x%02x%02x%02x%02x%02x\n",
                        (void *)machine, (void *)data, length,
                        length > 0 ? bytes[0] : 0, length > 1 ? bytes[1] : 0,
                        length > 2 ? bytes[2] : 0, length > 3 ? bytes[3] : 0,
                        length > 4 ? bytes[4] : 0, length > 5 ? bytes[5] : 0,
                        length > 6 ? bytes[6] : 0, length > 7 ? bytes[7] : 0);
            }
            if (t->offset == 0x10bd184) {
                ff5_trace_managed_string("font WWW url",
                                         u->uc_mcontext.arm_r1);
            }
            if (t->offset == 0x10bd358) {
                const uintptr_t data = u->uc_mcontext.arm_r0;
                const uint32_t length = data
                    ? *(const uint32_t *)(data + 0x0c) : 0;
                fprintf(stderr,
                        "[il2trace] font WWW=%p uwr=%p bytes=%p len=%u\n",
                        (void *)u->uc_mcontext.arm_r4,
                        u->uc_mcontext.arm_r4
                            ? (void *)(uintptr_t)*(const uint32_t *)(
                                  u->uc_mcontext.arm_r4 + 8)
                            : NULL,
                        (void *)data, length);
            }
            if (t->offset == 0x130f93c && u->uc_mcontext.arm_r0 > 0x10000u) {
                const uintptr_t edi = u->uc_mcontext.arm_r0;
                const uintptr_t exception = *(const uint32_t *)(edi + 8);
                fprintf(stderr, "[il2trace] font EDI=%p exception=%p\n",
                        (void *)edi, (void *)exception);
                if (exception > 0x10000u) {
                    ff5_trace_managed_string(
                        "font exception class",
                        *(const uint32_t *)(exception + 8));
                    ff5_trace_managed_string(
                        "font exception message",
                        *(const uint32_t *)(exception + 12));
                    const uintptr_t inner = *(const uint32_t *)(exception + 20);
                    if (inner > 0x10000u)
                        ff5_trace_managed_string(
                            "font inner message",
                            *(const uint32_t *)(inner + 12));
                }
            }
            if (t->offset == 0x64faf0) {
                const uint32_t *metadata = (const uint32_t *)u->uc_mcontext.arm_r3;
                const uint32_t *pair = (const uint32_t *)(u->uc_mcontext.arm_r0 - 4);
                const uint32_t *usage_list = (const uint32_t *)u->uc_mcontext.arm_r7;
                fprintf(stderr,
                        "[il2trace] metadata=%p magic=%08x version=%u usagePairs=0x%x usageLists=0x%x pair=%p(+0x%lx) list=%p(+0x%lx) r4=%lu r8=%lu\n",
                        (void *)metadata, metadata[0], metadata[1],
                        metadata[0xe8 / 4], metadata[0xf0 / 4], (void *)pair,
                        (unsigned long)((uintptr_t)pair - (uintptr_t)metadata),
                        (void *)usage_list,
                        (unsigned long)((uintptr_t)usage_list - (uintptr_t)metadata),
                        u->uc_mcontext.arm_r4, u->uc_mcontext.arm_r8);
                if ((uintptr_t)usage_list >= (uintptr_t)metadata &&
                    (uintptr_t)usage_list + 8 <= (uintptr_t)metadata + 9849672u)
                    fprintf(stderr, "[il2trace] usageList start=%u count=%u\n",
                            usage_list[0], usage_list[1]);
            }
            if (t->offset == 0x127e308) {
                const uintptr_t op = u->uc_mcontext.arm_r1;
                if (op) {
                    const uintptr_t result = *(const uint32_t *)(op + 8);
                    const uint32_t refs = *(const uint32_t *)(op + 12);
                    const uint32_t status = *(const uint32_t *)(op + 16);
                    const uintptr_t error = *(const uint32_t *)(op + 20);
                    fprintf(stderr,
                            "[il2trace] catalog op=%p result=%p refs=%u status=%u error=%p\n",
                            (void *)op, (void *)result, refs, status, (void *)error);
                    if (error) {
                        ff5_trace_managed_string("catalog exception class",
                                                 *(const uint32_t *)(error + 8));
                        ff5_trace_managed_string("catalog exception message",
                                                 *(const uint32_t *)(error + 12));
                        const uintptr_t inner = *(const uint32_t *)(error + 20);
                        if (inner)
                            ff5_trace_managed_string("catalog inner message",
                                                     *(const uint32_t *)(inner + 12));
                    }
                }
            }
            if ((t->offset == 0x7d3694 || t->offset == 0x7d41a4 ||
                 t->offset == 0x7d42d0 || t->offset == 0x7d42e0) &&
                u->uc_mcontext.arm_r0) {
                const uintptr_t scene = u->uc_mcontext.arm_r0;
                fprintf(stderr,
                        "[il2trace] title scene=%p cloud=%u init=0x%08x arg1=%08lx\n",
                        (void *)scene, *(const uint8_t *)(scene + 0x1c),
                        *(const uint32_t *)(scene + 0x20),
                        u->uc_mcontext.arm_r1);
            }
            if ((t->offset == 0x7d4718 || t->offset == 0x7d6724) &&
                u->uc_mcontext.arm_r0) {
                const uintptr_t coroutine = u->uc_mcontext.arm_r0;
                fprintf(stderr,
                        "[il2trace] title coroutine=%p state=%d current=%p owner=%p\n",
                        (void *)coroutine, *(const int32_t *)(coroutine + 8),
                        (void *)(uintptr_t)*(const uint32_t *)(coroutine + 12),
                        t->offset == 0x7d6724
                            ? (void *)(uintptr_t)*(const uint32_t *)(coroutine + 16)
                            : NULL);
            }
            if ((t->offset == 0x93b2e4 || t->offset == 0x93bd44 ||
                 t->offset == 0x93be10 || t->offset == 0x93be18) &&
                u->uc_mcontext.arm_r0) {
                const uintptr_t loader = u->uc_mcontext.arm_r0;
                fprintf(stderr,
                        "[il2trace] title loader=%p monitor=%p finished=%u resident=%p effect=%p\n",
                        (void *)loader,
                        (void *)(uintptr_t)*(const uint32_t *)(loader + 8),
                        *(const uint8_t *)(loader + 12),
                        (void *)(uintptr_t)*(const uint32_t *)(loader + 16),
                        (void *)(uintptr_t)*(const uint32_t *)(loader + 20));
            }
            if ((t->offset == 0x7ec7b0 || t->offset == 0x7ec84c ||
                 t->offset == 0x7ed208 || t->offset == 0x7ed43c ||
                 t->offset == 0x7ed9d0) &&
                u->uc_mcontext.arm_r0 > 0x10000u) {
                const uintptr_t splash = u->uc_mcontext.arm_r0;
                fprintf(stderr,
                        "[il2trace] splash=%p content=%p load=%u anim=%u done=%u "
                        "indicator=%u progress=%p android=%u screen=%p message=%u\n",
                        (void *)splash,
                        (void *)(uintptr_t)*(const uint32_t *)(splash + 0x20),
                        *(const uint8_t *)(splash + 0x24),
                        *(const uint8_t *)(splash + 0x25),
                        *(const uint8_t *)(splash + 0x26),
                        *(const uint8_t *)(splash + 0x27),
                        (void *)(uintptr_t)*(const uint32_t *)(splash + 0x28),
                        *(const uint8_t *)(splash + 0x2c),
                        (void *)(uintptr_t)*(const uint32_t *)(splash + 0x30),
                        *(const uint8_t *)(splash + 0x34));
            }
            if (t->offset == 0x7ed24c || t->offset == 0x7ed254 ||
                t->offset == 0x7ed264 || t->offset == 0x7ed290 ||
                t->offset == 0x7ed2fc) {
                const uintptr_t splash = u->uc_mcontext.arm_r4;
                if (splash)
                    fprintf(stderr,
                            "[il2trace] splash boundary=%p flags=%u,%u,%u,%u android=%u message=%u\n",
                            (void *)splash,
                            *(const uint8_t *)(splash + 0x24),
                            *(const uint8_t *)(splash + 0x25),
                            *(const uint8_t *)(splash + 0x26),
                            *(const uint8_t *)(splash + 0x27),
                            *(const uint8_t *)(splash + 0x2c),
                            *(const uint8_t *)(splash + 0x34));
            }
            if ((t->offset == 0x7ee650 || t->offset == 0x7edbdc ||
                 t->offset == 0x7ede8c) && u->uc_mcontext.arm_r0) {
                const uintptr_t coroutine = u->uc_mcontext.arm_r0;
                fprintf(stderr,
                        "[il2trace] splash coroutine=%p state=%d current=%p owner=%p\n",
                        (void *)coroutine, *(const int32_t *)(coroutine + 8),
                        (void *)(uintptr_t)*(const uint32_t *)(coroutine + 12),
                        (void *)(uintptr_t)*(const uint32_t *)(coroutine + 16));
            }
            /* No callback do LVL o que interessa esta' em r5/r7/r9 e no
             * responseCode salvo em [fp-32]; o dump generico so' cobre r0-r3. */
            if (t->offset == 0x7db554 || t->offset == 0x7db614 ||
                t->offset == 0x7db620 || t->offset == 0x7db634 ||
                t->offset == 0x7db648 || t->offset == 0x7db66c ||
                t->offset == 0x7db660) {
                const uintptr_t fp = u->uc_mcontext.arm_fp;
                fprintf(stderr,
                        "[il2trace] lvl r5(signature)=%08lx r7(signedData)=%08lx "
                        "r9(veredito)=%d sl(this)=%08lx respCode=%d\n",
                        u->uc_mcontext.arm_r5, u->uc_mcontext.arm_r7,
                        (int)u->uc_mcontext.arm_r9, u->uc_mcontext.arm_r10,
                        fp ? *(const int32_t *)(fp - 32) : -999);
            }
            if (t->offset == 0x7dc7b4 || t->offset == 0x7dc860) {
                const uintptr_t lvl = t->offset == 0x7dc7b4
                                    ? u->uc_mcontext.arm_r0
                                    : u->uc_mcontext.arm_r4;
                if (lvl > 0x10000u)
                    fprintf(stderr,
                            "[il2trace] AndroidLvl=%p running=%u response=%d "
                            "received=%u popup=%u binder=%p\n",
                            (void *)lvl, *(const uint8_t *)(lvl + 0x28),
                            *(const int32_t *)(lvl + 0x38),
                            *(const uint8_t *)(lvl + 0x44),
                            *(const uint8_t *)(lvl + 0x90),
                            (void *)(uintptr_t)*(const uint32_t *)(lvl + 0x34));
            }
            if (t->offset == 0x7ee8ac && u->uc_mcontext.arm_r0) {
                const uintptr_t machine = u->uc_mcontext.arm_r0;
                fprintf(stderr,
                        "[il2trace] splash async=%p state=%d owner=%p exception=%p\n",
                        (void *)machine, *(const int32_t *)machine,
                        (void *)(uintptr_t)*(const uint32_t *)(machine + 12),
                        (void *)(uintptr_t)*(const uint32_t *)(machine + 8));
            }
            if (t->offset == 0x7ee9e8 || t->offset == 0x7efac4 ||
                t->offset == 0x7eeb88 || t->offset == 0x7efa78 ||
                t->offset == 0x7eef94 || t->offset == 0x7ef0ac ||
                t->offset == 0x7efa34 || t->offset == 0x7ef268 ||
                t->offset == 0x7ef970 || t->offset == 0x7ef8e0 ||
                t->offset == 0x7efd34 || t->offset == 0x7efc90 ||
                t->offset == 0x7efd2c) {
                const uintptr_t machine = u->uc_mcontext.arm_r10;
                const uintptr_t owner = u->uc_mcontext.arm_r8;
                fprintf(stderr,
                        "[il2trace] splash async-stage machine=%p state=%d owner=%p flags=%u,%u,%u,%u,%u\n",
                        (void *)machine,
                        machine ? *(const int32_t *)machine : -999,
                        (void *)owner,
                        owner ? *(const uint8_t *)(owner + 0x24) : 0,
                        owner ? *(const uint8_t *)(owner + 0x25) : 0,
                        owner ? *(const uint8_t *)(owner + 0x26) : 0,
                        owner ? *(const uint8_t *)(owner + 0x2c) : 0,
                        owner ? *(const uint8_t *)(owner + 0x34) : 0);
            }
            fflush(stderr);
            u->uc_mcontext.arm_pc = site;
            return;
        }
    }
    on_crash(sig, si, opaque);
}

static void install_job_tracepoints(void) {
    if (!getenv("FF5_JOB_TRACE") || !g_unity_base) return;
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) page = 4096;
    for (unsigned i = 0; i < sizeof g_job_tracepoints / sizeof g_job_tracepoints[0]; i++) {
        job_tracepoint *t = &g_job_tracepoints[i];
        uintptr_t site = g_unity_base + t->offset;
        uintptr_t base = site & ~((uintptr_t)page - 1);
        if (mprotect((void *)base, (size_t)page, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
            fprintf(stderr, "[jobtrace] mprotect falhou em +0x%lx\n",
                    (unsigned long)t->offset);
            continue;
        }
        t->original = *(uint16_t *)site;
        *(volatile uint16_t *)site = 0xde00; /* udf #0, Thumb */
        __builtin___clear_cache((char *)site, (char *)site + 2);
        t->armed = 1;
    }
    /* O Mali-450 usa um kernel ARM antigo onde invalidar somente dois bytes
     * pode deixar a linha anterior no I-cache. O flush integral do modulo e o
     * mesmo caminho ja validado pelos patches do so-loader. */
    so_use(g_m_unity);
    so_flush_caches();
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_job_trace;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGILL, &sa, NULL);
    fprintf(stderr, "[jobtrace] %zu pontos armados\n",
            sizeof g_job_tracepoints / sizeof g_job_tracepoints[0]);
}

static void install_il2cpp_tracepoints(void) {
    if (!getenv("FF5_IL2CPP_TRACE") || !g_il2cpp_base || !g_m_il2cpp) return;
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) page = 4096;
    for (unsigned i = 0; i < sizeof g_il2cpp_tracepoints / sizeof g_il2cpp_tracepoints[0]; i++) {
        il2cpp_tracepoint *t = &g_il2cpp_tracepoints[i];
        uintptr_t site = g_il2cpp_base + t->offset;
        uintptr_t page_base = site & ~((uintptr_t)page - 1);
        if (mprotect((void *)page_base, (size_t)page,
                     PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
            fprintf(stderr, "[il2trace] mprotect falhou em +0x%lx\n",
                    (unsigned long)t->offset);
            continue;
        }
        t->original = *(uint32_t *)site;
        if (t->deferred) continue;
        *(volatile uint32_t *)site = 0xe7f000f0u; /* UDF #0, ARM */
        __builtin___clear_cache((char *)site, (char *)site + 4);
        t->armed = 1;
    }
    so_use(g_m_il2cpp);
    so_flush_caches();
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_job_trace;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGILL, &sa, NULL);
    fprintf(stderr, "[il2trace] %zu pontos armados\n",
            sizeof g_il2cpp_tracepoints / sizeof g_il2cpp_tracepoints[0]);
}

// ---- resolucao cross-modulo (libunity dlopen/dlsym libil2cpp) -------------
// libunity resolve o il2cpp em runtime via dlopen/dlsym; interceptamos e
// devolvemos os simbolos dos modulos que carregamos com so_load.
#define H_UNITY  ((void *)0x100)
#define H_IL2CPP ((void *)0x101)
#define H_MAIN   ((void *)0x102)
#define H_SEAD   ((void *)0x103)
#define H_RMS    ((void *)0x104)

static so_module *load_module(const char *soname, size_t heap);
static int apply_unpacked_il2cpp_resources_patch(void);
static int restore_il2cpp_debug_log_guard(void);
static void *(*real_il2cpp_init)(const char *domain_name);
static void (*ff5_il2cpp_gc_collect)(int max_generation);
static size_t (*ff5_il2cpp_gc_get_used_size)(void);
static size_t (*ff5_il2cpp_gc_get_heap_size)(void);

/* O ELF do IL2CPP e carregado manualmente porque o linker glibc nao aceita o
 * binario Bionic. Por isso o Boehm embutido nao descobre o PT_LOAD gravavel e
 * inicia com n_root_sets == 0. Os caches nativos de System.Type guardam
 * Il2CppObject*, mas sem uma raiz estatica o coletor recicla esses RuntimeType.
 *
 * Estes RVAs pertencem a este libil2cpp.so exato (Unity 2019.4/FF5): os dois
 * contadores de mark_rts.c e GC_arrays._static_roots. O registro e feito logo
 * depois do il2cpp_init real, no mesmo ponto em que o runtime Android ja criou
 * o GC e antes de devolver o controle ao Unity. O guard do PT_LOAD impede que
 * um binario diferente receba uma estrutura interna incompativel. */
#define FF5_GC_N_ROOT_SETS_RVA 0x2690594u
#define FF5_GC_ROOT_SIZE_RVA   0x2690598u
#define FF5_GC_STATIC_ROOTS_RVA 0x26e4bb8u
#define FF5_IL2CPP_DATA_RVA    0x23b2660u
#define FF5_IL2CPP_DATA_SIZE   0x389be4u

struct ff5_gc_root {
    uintptr_t start;
    uintptr_t end;
    uintptr_t next;
    uint32_t temporary;
};

static int ff5_register_il2cpp_gc_root(void) {
    uintptr_t data_start = 0;
    uintptr_t data_end = 0;

    for (int i = 0; i < g_so_nmods; i++) {
        const struct so_phdr_mod *m = &g_so_mods[i];
        if (m->base != g_il2cpp_base || !strstr(m->name, "libil2cpp"))
            continue;
        for (int j = 0; j < m->phnum; j++) {
            const Elf32_Phdr *ph = &m->ph[j];
            if (ph->p_type != PT_LOAD || !(ph->p_flags & PF_W)) continue;
            if (ph->p_vaddr != FF5_IL2CPP_DATA_RVA ||
                ph->p_memsz != FF5_IL2CPP_DATA_SIZE) {
                fprintf(stderr,
                        "[ff5] GC root: PT_LOAD inesperado rva=0x%lx size=0x%lx\n",
                        (unsigned long)ph->p_vaddr,
                        (unsigned long)ph->p_memsz);
                return -1;
            }
            data_start = m->base + ph->p_vaddr;
            data_end = data_start + ph->p_memsz;
        }
    }
    if (!data_start || data_end <= data_start) {
        fprintf(stderr, "[ff5] GC root: PT_LOAD gravavel do IL2CPP ausente\n");
        return -1;
    }

    volatile uint32_t *nroots = (volatile uint32_t *)(g_il2cpp_base +
                                      FF5_GC_N_ROOT_SETS_RVA);
    volatile uintptr_t *root_size = (volatile uintptr_t *)(g_il2cpp_base +
                                      FF5_GC_ROOT_SIZE_RVA);
    volatile struct ff5_gc_root *roots =
        (volatile struct ff5_gc_root *)(g_il2cpp_base +
                                       FF5_GC_STATIC_ROOTS_RVA);

    if (*nroots != 0 || *root_size != 0 || roots[0].start != 0 ||
        roots[0].end != 0 || roots[0].next != 0 || roots[0].temporary != 0) {
        fprintf(stderr,
                "[ff5] GC root: estado inicial inesperado n=%u size=%lu entry=%p..%p\n",
                *nroots, (unsigned long)*root_size,
                (void *)roots[0].start, (void *)roots[0].end);
        return -1;
    }

    roots[0].start = data_start;
    roots[0].end = data_end;
    roots[0].next = 0;
    roots[0].temporary = 0;
    *root_size = data_end - data_start;
    __sync_synchronize();
    *nroots = 1;
    fprintf(stderr, "[ff5] GC root: IL2CPP RW registrado %p..%p (%lu bytes)\n",
            (void *)data_start, (void *)data_end,
            (unsigned long)(data_end - data_start));
    return 0;
}

/* Preserve a sonda FF5_GC_DISABLE, mas somente depois do registro normal. */
static void *ff5_il2cpp_init_gc_bridge(const char *domain_name) {
    void *domain = real_il2cpp_init ? real_il2cpp_init(domain_name) : NULL;
    if (domain && ff5_register_il2cpp_gc_root() != 0)
        fprintf(stderr, "[ff5] GC root: registro falhou\n");
    if (domain) {
        so_use(g_m_il2cpp);
        ff5_il2cpp_gc_collect =
            (void (*)(int))so_find_addr_safe("il2cpp_gc_collect");
        ff5_il2cpp_gc_get_used_size =
            (size_t (*)(void))so_find_addr_safe("il2cpp_gc_get_used_size");
        ff5_il2cpp_gc_get_heap_size =
            (size_t (*)(void))so_find_addr_safe("il2cpp_gc_get_heap_size");
    }
    if (domain && getenv("FF5_GC_DISABLE")) {
        so_use(g_m_il2cpp);
        void (*gc_disable)(void) =
            (void (*)(void))so_find_addr_safe("il2cpp_gc_disable");
        if (gc_disable) {
            gc_disable();
            fprintf(stderr, "[ff5] GC probe: coleta desabilitada apos il2cpp_init\n");
        }
    }
    return domain;
}

/* No Android, Application.streamingAssetsPath aponta para
 * jar:file://<base.apk>!/assets. Aqui o APK ja foi materializado em g_gamedir;
 * deixar o getter montar "jar:file://!/assets" faz o Addressables abrir o JSON
 * por um caminho remendado, mas concluir o ContentCatalogData como NULL. A
 * receita aprovada do Terraria e interceptar o getter, nao a coroutine: todas
 * as etapas do Addressables continuam na ordem original, agora sobre um path
 * POSIX real. */
static void *(*ff5_il2cpp_string_new)(const char *);

static void *ff5_streaming_assets_path(void) {
    if (!ff5_il2cpp_string_new) return NULL;
    char path[sizeof g_gamedir + 8];
    const char *value = g_gamedir;
    /* LoadFromFile aceita um path POSIX, mas o FontLoader do FF5 usa WWW: com
     * um path cru o WWW o trata como URL sem esquema, cai no curl ("Could not
     * resolve host: localhost") e devolve 0 bytes -> "Value cannot be null." e
     * a splash fica branca para sempre. Com file:// o bundle carrega
     * (UnityFS, 25600 bytes) e o catalogo continua funcionando, entao este e'
     * o default; FF5_STREAMING_POSIX_PATH volta ao path cru para diagnostico. */
    if (!getenv("FF5_STREAMING_POSIX_PATH")) {
        snprintf(path, sizeof path, "file://%s", g_gamedir);
        value = path;
    }
    /* Nunca retenha um Il2CppString* somente numa global do loader: a area de
     * dados do executavel nao e uma GC root do Boehm embutido e o objeto pode
     * ser coletado/reutilizado. O getter Android cria um resultado managed e
     * o entrega ao chamador; reproduza essa propriedade a cada chamada. */
    void *result = ff5_il2cpp_string_new(value);
    static unsigned calls;
    if (calls++ < 8)
        fprintf(stderr, "[ff5] streamingAssetsPath[%u] -> %s (%p)\n",
                calls, value, result);
    return result;
}

/* --- veredito do LVL ------------------------------------------------------
 * O callback do ServiceBinder (libil2cpp+0x7db500) calcula o veredito em r9 e
 * o grava UMA unica vez em AndroidLvl+0x38 (0x7db660). O Update despacha por
 * uma jump-table de 4 entradas indexada por (response+1):
 *   -1 = aguardando, 0 = LICENSED, 1 = NOT_LICENSED, 2 = LICENSED_OLD_KEY.
 * Qualquer outro valor cai fora da tabela e vira o dialogo de erro 1031.
 *
 * O valor -2 e' o DEFAULT do callback: e' o que sobra quando a verificacao da
 * assinatura RSA nao conclui. Offline, sem com.android.vending, ela nunca
 * conclui — a assinatura so' pode ser produzida com a chave privada da Square
 * Enix. O caminho de sucesso real (0x7dc2c4, veredito = int.Parse dos dados
 * verificados) fica INTACTO: trocamos apenas o default -2 por 0 (LICENSED),
 * ou seja, na ausencia de servico de licenca o jogo assume licenciado em vez
 * de assumir erro. Tres sitios materializam esse default com `mvn r9,#1`. */
static const uintptr_t g_lvl_default_sites[] = { 0x7db548, 0x7db7dc, 0x7dc30c };
#define LVL_MVN_R9_1  0xe3e09001u   /* mvn r9, #1  -> r9 = -2 */
#define LVL_MOV_R9_0  0xe3a09000u   /* mov r9, #0  -> r9 = 0 (LICENSED) */

static int install_lvl_licensed_default(void) {
    if (getenv("FF5_LVL_KEEP_ERROR")) {
        fprintf(stderr, "[ff5] LVL: default de erro preservado (FF5_LVL_KEEP_ERROR)\n");
        return 0;
    }
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) page = 4096;
    unsigned patched = 0;
    for (unsigned i = 0; i < sizeof g_lvl_default_sites / sizeof g_lvl_default_sites[0]; i++) {
        uintptr_t site = g_il2cpp_base + g_lvl_default_sites[i];
        if (*(volatile uint32_t *)site != LVL_MVN_R9_1) {
            fprintf(stderr, "[ff5] LVL: +0x%lx nao e' `mvn r9,#1` (0x%08x); nao mexo\n",
                    (unsigned long)g_lvl_default_sites[i], *(volatile uint32_t *)site);
            continue;
        }
        uintptr_t base = site & ~((uintptr_t)page - 1);
        if (mprotect((void *)base, (size_t)page * 2,
                     PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
            fprintf(stderr, "[ff5] LVL: mprotect falhou em +0x%lx\n",
                    (unsigned long)g_lvl_default_sites[i]);
            continue;
        }
        *(volatile uint32_t *)site = LVL_MOV_R9_0;
        __builtin___clear_cache((char *)site, (char *)site + 4);
        patched++;
    }
    fprintf(stderr, "[ff5] LVL: default do veredito = LICENSED (%u/%u sitios)\n",
            patched, (unsigned)(sizeof g_lvl_default_sites / sizeof g_lvl_default_sites[0]));
    return patched ? 0 : -1;
}

static int install_streaming_assets_path_bridge(void) {
    const uintptr_t rva = 0x194403c;
    uintptr_t site = g_il2cpp_base + rva;
    if (*(uint32_t *)site != 0xe59f0034u ||
        *(uint32_t *)(site + 4) != 0xe79f0000u) {
        fprintf(stderr, "[ff5] get_streamingAssetsPath inesperado; bridge recusada\n");
        return -1;
    }
    so_use(g_m_il2cpp);
    ff5_il2cpp_string_new = (void *)so_find_addr_safe("il2cpp_string_new");
    if (!ff5_il2cpp_string_new) {
        fprintf(stderr, "[ff5] il2cpp_string_new ausente; bridge recusada\n");
        return -1;
    }
    so_make_text_writable();
    hook_arm(site, (uintptr_t)ff5_streaming_assets_path);
    so_flush_caches();
    so_make_text_executable();
    fprintf(stderr, "[ff5] bridge nativa de streamingAssetsPath instalada\n");
    return 0;
}

static void *my_dlopen(const char *name, int flag) {
    if (getenv("FF5_JNI_TRACE"))
        fprintf(stderr, "[dl] dlopen(%s, 0x%x)\n", name ? name : "NULL", flag);
    if (!name) return H_MAIN;
    if (strstr(name, "il2cpp")) {
        /* O Unity Android abre o backend IL2CPP sob demanda durante initJni.
         * Materialize o ELF nesse mesmo dlopen, nao antes do lifecycle. */
        if (!g_m_il2cpp) {
            fprintf(stderr, "[ff5] dlopen(%s): carregando backend IL2CPP\n", name);
            g_m_il2cpp = load_module("libil2cpp.so", 96u * 1024 * 1024);
            if (g_m_il2cpp && restore_il2cpp_debug_log_guard() != 0) {
                fprintf(stderr,
                        "[ff5] integridade de UnityEngine.Debug.Log nao pode ser restaurada\n");
                return NULL;
            }
            if (g_m_il2cpp && install_streaming_assets_path_bridge() != 0)
                return NULL;
            if (g_m_il2cpp) install_lvl_licensed_default();
            if (g_m_il2cpp && getenv("FF5_IL2CPP_TRACE")) {
                uintptr_t root = *(uint32_t *)(g_il2cpp_base + 0x2690c50);
                uintptr_t metadata = *(uint32_t *)(g_il2cpp_base + 0x2681c94);
                fprintf(stderr,
                        "[il2trace] apos init_array: callbacks=%p metadataRegistration=%p expected=%p\n",
                        (void *)root, (void *)metadata,
                        (void *)(g_il2cpp_base + 0x250ef80));
            }
            if (g_m_il2cpp) install_il2cpp_tracepoints();
            /* Janela exclusivamente diagnostica para anexar o gdb depois que
             * os RVAs do IL2CPP existem e antes de devolver o dlopen a Unity.
             * Nao altera a ordem nem chama entry points fora do lifecycle. */
            if (g_m_il2cpp && getenv("FF5_GDB_WAIT_MS")) {
                int wait_ms = atoi(getenv("FF5_GDB_WAIT_MS"));
                if (wait_ms < 0) wait_ms = 0;
                if (wait_ms > 60000) wait_ms = 60000;
                fprintf(stderr, "[ff5] GDB wait %d ms; il2cpp=%p\n",
                        wait_ms, (void *)g_il2cpp_base);
                fflush(stderr);
                usleep((useconds_t)wait_ms * 1000u);
            }
        }
        return g_m_il2cpp ? H_IL2CPP : NULL;
    }
    if (strstr(name, "unity")) {
        /* Chamado por com.unity3d.player.NativeLoader.load. O mapeamento e
         * manual por incompatibilidade Bionic/glibc, mas o instante e o entry
         * point continuam sendo os do loader Java original. */
        if (!g_m_unity) {
            fprintf(stderr, "[ff5] dlopen(%s): carregando engine Unity\n", name);
            g_m_unity = load_module("libunity.so", 48u * 1024 * 1024);
        }
        return g_m_unity ? H_UNITY : NULL;
    }
    if (strstr(name, "main"))   return H_MAIN;
    if (strstr(name, "RMS"))    return H_RMS;
    if (strstr(name, "sead")) {
        /* DllImport("sead") do IL2CPP equivale ao dlopen preguiçoso feito
         * pelo linker Android. Preserve esse momento do fluxo e carregue o
         * plugin real somente na primeira resolucao P/Invoke. */
        if (!g_m_sead) {
            fprintf(stderr, "[ff5] dlopen(%s): carregando plugin SEAD nativo\n", name);
            g_m_sead = load_module("libsead.so", 4u * 1024 * 1024);
        }
        return g_m_sead ? H_SEAD : NULL;
    }
    return dlopen(name, flag);   // libs reais do sistema
}
static void *my_dlsym(void *h, const char *name) {
    uintptr_t a = 0;
    if (h == H_IL2CPP && g_m_il2cpp) { so_use(g_m_il2cpp); a = so_find_addr_safe(name); }
    else if (h == H_UNITY && g_m_unity) { so_use(g_m_unity); a = so_find_addr_safe(name); }
    else if (h == H_MAIN && g_m_main) { so_use(g_m_main); a = so_find_addr_safe(name); }
    else if (h == H_SEAD && g_m_sead) { so_use(g_m_sead); a = so_find_addr_safe(name); }
    else if (h == H_RMS && g_m_rms) { so_use(g_m_rms); a = so_find_addr_safe(name); }
    if (getenv("FF5_JNI_TRACE"))
        fprintf(stderr, "[dl] dlsym(%p, %s) -> %p\n", h,
                name ? name : "NULL", (void *)a);
    if (a && h == H_IL2CPP && name && !strcmp(name, "il2cpp_init")) {
        real_il2cpp_init = (void *(*)(const char *))a;
        return (void *)ff5_il2cpp_init_gc_bridge;
    }
    if (a) return (void *)a;
    if (h == H_UNITY || h == H_IL2CPP || h == H_MAIN || h == H_SEAD ||
        h == H_RMS)
        return NULL;
    /* Esta Unity carrega o dispatch GLES com dlopen+dlsym, nao somente com
     * eglGetProcAddress. Roteie a mesma fronteira para trace e thunks ABI. */
    void *gl = ff5_gl_override(name);
    if (gl) return gl;
    return dlsym(h, name);
}
static int my_dlclose(void *h) { return 0; }
static char *my_dlerror(void) { return NULL; }
static int my_dladdr(void *addr, void *info) { return 0; }

// dl_iterate_phdr custom: INTERPOE o da libc (exe -rdynamic, carrega 1o). O
// unwinder C++ da libgcc acha o .ARM.exidx de cada lib via dl_iterate_phdr;
// nossos modulos (libunity/libil2cpp) sao mapeados a mao -> invisiveis ao da
// libc -> excecao C++ nao acha o landing pad -> crash. Reportamos os modulos
// do dynamic linker (real, RTLD_NEXT) + os NOSSOS (g_so_mods).
int dl_iterate_phdr(int (*cb)(struct dl_phdr_info *, size_t, void *), void *data) {
    static int (*real)(int (*)(struct dl_phdr_info *, size_t, void *), void *);
    if (!real) real = (void *)dlsym(RTLD_NEXT, "dl_iterate_phdr");
    int r = real ? real(cb, data) : 0;
    if (r) return r;
    for (int i = 0; i < g_so_nmods; i++) {
        struct dl_phdr_info info;
        memset(&info, 0, sizeof info);
        info.dlpi_addr = (ElfW(Addr))g_so_mods[i].base;
        info.dlpi_name = g_so_mods[i].name;
        info.dlpi_phdr = (const ElfW(Phdr) *)g_so_mods[i].ph;
        info.dlpi_phnum = (ElfW(Half))g_so_mods[i].phnum;
        r = cb(&info, sizeof info, data);
        if (r) return r;
    }
    return r;
}

/* ARM EHABI nao depende apenas de dl_iterate_phdr: o unwinder consulta
 * __gnu_Unwind_Find_exidx diretamente. Os .so mapeados por so_load nao estao
 * na lista do dynamic linker, entao devolvemos o PT_ARM_EXIDX do modulo que
 * contem o PC; frames das bibliotecas do sistema continuam no resolvedor real. */
#ifndef PT_ARM_EXIDX
#define PT_ARM_EXIDX 0x70000001
#endif
static void *ff5_find_exidx(void *pc_, int *count) {
    uintptr_t pc = (uintptr_t)pc_ & ~(uintptr_t)1;
    for (int i = 0; i < g_so_nmods; i++) {
        struct so_phdr_mod *m = &g_so_mods[i];
        int owns_pc = 0;
        const Elf32_Phdr *exidx = NULL;
        for (int j = 0; j < m->phnum; j++) {
            const Elf32_Phdr *ph = &m->ph[j];
            if (ph->p_type == PT_LOAD &&
                pc >= m->base + ph->p_vaddr &&
                pc < m->base + ph->p_vaddr + ph->p_memsz)
                owns_pc = 1;
            if (ph->p_type == PT_ARM_EXIDX) exidx = ph;
        }
        if (owns_pc && exidx) {
            if (count) *count = (int)(exidx->p_memsz / 8);
            void *table = (void *)(m->base + exidx->p_vaddr);
            if (getenv("FF5_EXIDX_TRACE")) {
                static unsigned logs;
                if (__atomic_fetch_add(&logs, 1, __ATOMIC_RELAXED) < 80)
                    fprintf(stderr, "[exidx] pc=%p %s+0x%lx -> %p (%u entries)\n",
                            pc_, m->name, (unsigned long)(pc - m->base), table,
                            (unsigned)(exidx->p_memsz / 8));
            }
            return table;
        }
    }
    static void *(*real_find)(void *, int *);
    if (!real_find)
        real_find = (void *(*)(void *, int *))dlsym(RTLD_NEXT, "__gnu_Unwind_Find_exidx");
    if (real_find) return real_find(pc_, count);
    if (count) *count = 0;
    return NULL;
}

// ---- overrides diversos que o engine importa -----------------------------
static const char *my_glGetString(unsigned int name) {
    // Unity chama glGetString(GL_VERSION/GL_EXTENSIONS) fora do contexto as
    // vezes; delega ao GLESv2 real quando ja ha contexto.
    static const char *(*real)(unsigned int) = NULL;
    if (!real) real = (const char *(*)(unsigned int))dlsym(RTLD_DEFAULT, "glGetString");
    const char *r = real ? real(name) : NULL;
    /* Dupla camada de alpha exige passar por glShaderSource; com
     * GL_OES_get_program_binary a Unity revive binarios Mali cacheados e o
     * patch de shader nunca roda. Esconde as extensoes de binario. */
    if (r && name == 0x1F03 /* GL_EXTENSIONS */ && ff5_dual_enabled()) {
        static char *filtered;
        if (!filtered) {
            filtered = strdup(r);
            if (filtered) {
                static const char *drop[] = {"GL_OES_get_program_binary",
                                             "GL_ARM_mali_program_binary"};
                for (unsigned i = 0; i < 2; i++) {
                    char *hit = strstr(filtered, drop[i]);
                    if (!hit) continue;
                    size_t n = strlen(drop[i]);
                    if (hit[n] == ' ') n++;
                    memmove(hit, hit + n, strlen(hit + n) + 1);
                }
                fprintf(stderr, "[dual] extensoes de program binary escondidas "
                                "(shader sempre por fonte)\n");
            }
        }
        if (filtered) return filtered;
    }
    if (r) return r;
    if (name == 0x1F02) return "OpenGL ES 2.0";     // GL_VERSION
    if (name == 0x1F00) return "NextOS";            // GL_VENDOR
    if (name == 0x1F01) return "Mali-450";          // GL_RENDERER
    return "";
}

/* Unity resolve a maior parte do GLES por eglGetProcAddress. Estes wrappers
 * nao alteram a chamada: servem para registrar os parametros exatos que
 * chegam ao Mali quando FF5_GL_TRACE=1. As APIs de upload usam apenas
 * inteiros/ponteiros, portanto a ABI base AAPCS e a hard-float coincidem. */
static unsigned g_gl_upload_seq;
static uint64_t g_gl_tex_alloc_bytes;
static unsigned g_gl_tex_alloc_count;
static uint64_t g_gl_tex_live_bytes;
static unsigned g_gl_tex_live_count;
#define FF5_GL_TRACK_IDS 65536u
static uint32_t g_gl_tex_live[FF5_GL_TRACK_IDS];
static void (*real_glTexImage2D)(unsigned, int, int, int, int, int,
                                unsigned, unsigned, const void *);
static void (*real_glTexSubImage2D)(unsigned, int, int, int, int, int,
                                   unsigned, unsigned, const void *);
static void (*real_glCompressedTexImage2D)(unsigned, int, unsigned, int, int,
                                          int, int, const void *);
static void (*real_glCompressedTexSubImage2D)(unsigned, int, int, int, int,
                                             int, unsigned, int, const void *);
static void (*real_glPixelStorei)(unsigned, int);
static void (*real_glDeleteTextures_trace)(int, const unsigned *);
static void (*real_glGetIntegerv_trace)(unsigned, int *);
static void (*real_glBindFramebuffer_trace)(unsigned, unsigned);
static void (*real_glDrawArrays_trace)(unsigned, int, int);
static void (*real_glDrawElements_trace)(unsigned, int, unsigned, const void *);
static void (*real_glClear_trace)(unsigned);
static void (*real_glReadPixels_trace)(int, int, int, int, unsigned, unsigned, void *);
static void (*real_glShaderSource_trace)(unsigned, int, const char **, const int *);
static void (*real_glCompileShader_trace)(unsigned);
static void (*real_glLinkProgram_trace)(unsigned);
static unsigned g_gl_shader_source_count;

/* O compilador GLES2 do Mali-450 exige que a precisao de um uniform seja
 * identica nos dois estagios. Alguns variants de Sprite/Default desta Unity
 * declaram _Color/_RendererColor com precisao implicita ou diferente; o link
 * falha e o draw dos personagens simplesmente desaparece. Harmonizamos apenas
 * essas declaracoes para mediump, preservando o valor/tint do material. */
static int glsl_identifier_in_line(const char *line, size_t len, const char *id) {
    size_t ilen = strlen(id);
    for (size_t i = 0; i + ilen <= len; i++) {
        if (memcmp(line + i, id, ilen) != 0) continue;
        unsigned char before = i ? (unsigned char)line[i - 1] : ' ';
        unsigned char after = i + ilen < len ? (unsigned char)line[i + ilen] : ' ';
        if (!(isalnum(before) || before == '_') &&
            !(isalnum(after) || after == '_')) return 1;
    }
    return 0;
}

static int glsl_target_uniform_line(const char *line, size_t len) {
    return memmem(line, len, "uniform", 7) &&
           (glsl_identifier_in_line(line, len, "_Color") ||
            glsl_identifier_in_line(line, len, "_RendererColor"));
}

static char *glsl_fix_color_precision(const char *src, size_t len,
                                      size_t *out_len, int *changed) {
    char *out = malloc(len * 2 + 64);
    if (!out) return NULL;
    size_t r = 0, w = 0;
    *changed = 0;
    while (r < len) {
        size_t end = r;
        while (end < len && src[end] != '\n') end++;
        size_t line_len = end - r;
        if (glsl_target_uniform_line(src + r, line_len)) {
            const char *line = src + r;
            const char *u = memmem(line, line_len, "uniform", 7);
            size_t prefix = u ? (size_t)(u - line) + 7 : 0;
            memcpy(out + w, line, prefix); w += prefix;
            size_t p = prefix;
            while (p < line_len && (line[p] == ' ' || line[p] == '\t')) p++;
            if (p + 4 <= line_len && !memcmp(line + p, "lowp", 4)) p += 4;
            else if (p + 5 <= line_len && !memcmp(line + p, "highp", 5)) p += 5;
            else if (p + 7 <= line_len && !memcmp(line + p, "mediump", 7)) p += 7;
            while (p < line_len && (line[p] == ' ' || line[p] == '\t')) p++;
            memcpy(out + w, " mediump ", 9); w += 9;
            memcpy(out + w, line + p, line_len - p); w += line_len - p;
            *changed = 1;
        } else {
            memcpy(out + w, src + r, line_len); w += line_len;
        }
        if (end < len) out[w++] = src[end++];
        r = end;
    }
    out[w] = 0;
    *out_len = w;
    return out;
}

/* Variante ETC1 split-alpha do Sprite/Default: em alguns pacotes o atlas ja
 * chega RGBA, mas o material ainda seleciona _AlphaTex. Se esse sampler e'
 * dummy, mix(..., _AlphaTex.r, _EnableExternalAlpha) zera a silhueta inteira.
 * Mantemos este reparo separado/diagnosticavel ate a captura no Mali provar o
 * variant. Ele altera somente o seletor de alpha; cor/tint continuam nativos. */
static char *glsl_disable_external_alpha(const char *src, size_t len,
                                         size_t *out_len, int *changed) {
    static const char token[] = "_EnableExternalAlpha";
    const size_t token_len = sizeof token - 1;
    char *out = malloc(len + 1);
    if (!out) return NULL;
    size_t r = 0, w = 0;
    *changed = 0;
    while (r < len) {
        size_t end = r;
        while (end < len && src[end] != '\n') end++;
        size_t line_len = end - r;
        if (memmem(src + r, line_len, "uniform", 7) &&
            glsl_identifier_in_line(src + r, line_len, token)) {
            *changed = 1; /* remove a declaracao que ficaria sem uso */
        } else {
            size_t p = 0;
            while (p < line_len) {
                if (p + token_len <= line_len &&
                    !memcmp(src + r + p, token, token_len)) {
                    unsigned char before = p ? (unsigned char)src[r + p - 1] : ' ';
                    unsigned char after = p + token_len < line_len
                        ? (unsigned char)src[r + p + token_len] : ' ';
                    if (!(isalnum(before) || before == '_') &&
                        !(isalnum(after) || after == '_')) {
                        memcpy(out + w, "0.0", 3); w += 3; p += token_len;
                        *changed = 1;
                        continue;
                    }
                }
                out[w++] = src[r + p++];
            }
            if (end < len) out[w++] = '\n';
        }
        if (end < len) end++;
        r = end;
    }
    out[w] = 0;
    *out_len = w;
    return out;
}

static void trace_glShaderSource(unsigned shader, int count,
                                 const char **strings, const int *lengths) {
    if (!real_glShaderSource_trace)
        real_glShaderSource_trace = dlsym(RTLD_DEFAULT, "glShaderSource");
    if (!real_glShaderSource_trace || !strings || count <= 0) return;

    size_t total = 0;
    for (int i = 0; i < count && strings[i]; i++) {
        size_t n = lengths && lengths[i] >= 0 ? (size_t)lengths[i] : strlen(strings[i]);
        if (n > 65536 || total > 65536 - n) { total = 0; break; }
        total += n;
    }
    if (!total || getenv("FF5_NO_COLOR_PRECISION_FIX")) {
        real_glShaderSource_trace(shader, count, strings, lengths);
        return;
    }
    char *joined = malloc(total + 1);
    if (!joined) {
        real_glShaderSource_trace(shader, count, strings, lengths);
        return;
    }
    size_t pos = 0;
    for (int i = 0; i < count && strings[i]; i++) {
        size_t n = lengths && lengths[i] >= 0 ? (size_t)lengths[i] : strlen(strings[i]);
        memcpy(joined + pos, strings[i], n); pos += n;
    }
    joined[pos] = 0;
    unsigned seq = ++g_gl_shader_source_count;
    int has_extalpha = memmem(joined, pos, "_EnableExternalAlpha", 20) != NULL;
    int has_alphatex = memmem(joined, pos, "_AlphaTex", 9) != NULL;
    int has_color = memmem(joined, pos, "_Color", 6) != NULL;
    int has_renderer_color = memmem(joined, pos, "_RendererColor", 14) != NULL;
    if (seq == 1)
        fprintf(stderr, "[shader] glShaderSource intercept ativo\n");
    if (getenv("FF5_SHADER_DIAG") &&
        (has_extalpha || has_alphatex || has_color || has_renderer_color)) {
        static unsigned diag_count;
        if (diag_count++ < 80)
            fprintf(stderr, "[shader-src] #%u shader=%u len=%zu extalpha=%d alphatex=%d color=%d rendererColor=%d\n",
                    seq, shader, pos, has_extalpha, has_alphatex, has_color,
                    has_renderer_color);
    }

    size_t color_len = 0; int color_changed = 0;
    char *color_fixed = glsl_fix_color_precision(joined, pos, &color_len,
                                                  &color_changed);
    const char *stage = color_changed && color_fixed ? color_fixed : joined;
    size_t stage_len = color_changed && color_fixed ? color_len : pos;

    size_t alpha_len = 0; int alpha_changed = 0;
    char *alpha_fixed = NULL;
    if (getenv("FF5_EXTALPHA_FIX") && has_extalpha)
        alpha_fixed = glsl_disable_external_alpha(stage, stage_len, &alpha_len,
                                                   &alpha_changed);

    size_t dual_len = 0;
    char *dual_fixed = NULL;
    if (ff5_dual_enabled()) {
        const char *in = alpha_changed && alpha_fixed ? alpha_fixed : stage;
        size_t in_len = alpha_changed && alpha_fixed ? alpha_len : stage_len;
        dual_fixed = ff5_dual_patch_shader(in, in_len, &dual_len);
        if (dual_fixed) {
            static unsigned dual_logged;
            if (dual_logged++ < 8)
                fprintf(stderr, "[shader-fix] shader=%u: _MainTex -> gemea de alpha ETC1\n",
                        shader);
        }
    }

    const char *result = dual_fixed ? dual_fixed
                        : alpha_changed && alpha_fixed ? alpha_fixed : stage;
    size_t result_len = dual_fixed ? dual_len
                        : alpha_changed && alpha_fixed ? alpha_len : stage_len;
    if (color_changed) {
        fprintf(stderr, "[shader-fix] shader=%u: precisao de _Color/_RendererColor -> mediump\n",
                shader);
    }
    if (alpha_changed)
        fprintf(stderr, "[shader-fix] shader=%u: ExternalAlpha -> alpha RGBA interno\n",
                shader);
    if (color_changed || alpha_changed || dual_fixed) {
        int one_len = (int)result_len;
        real_glShaderSource_trace(shader, 1, &result, &one_len);
    } else {
        real_glShaderSource_trace(shader, count, strings, lengths);
    }
    free(dual_fixed);
    free(alpha_fixed);
    free(color_fixed);
    free(joined);
}

static void trace_glCompileShader(unsigned shader) {
    if (!real_glCompileShader_trace)
        real_glCompileShader_trace = dlsym(RTLD_DEFAULT, "glCompileShader");
    if (!real_glCompileShader_trace) return;
    real_glCompileShader_trace(shader);
    int ok = 1;
    void (*getiv)(unsigned, unsigned, int *) = dlsym(RTLD_DEFAULT, "glGetShaderiv");
    if (getiv) getiv(shader, 0x8B81 /* GL_COMPILE_STATUS */, &ok);
    if (!ok) {
        char log[1024] = {0};
        void (*getlog)(unsigned, int, int *, char *) =
            dlsym(RTLD_DEFAULT, "glGetShaderInfoLog");
        if (getlog) getlog(shader, sizeof log - 1, NULL, log);
        fprintf(stderr, "[shader] compile falhou shader=%u: %s\n", shader, log);
    }
}

static void trace_glLinkProgram(unsigned program) {
    if (!real_glLinkProgram_trace)
        real_glLinkProgram_trace = dlsym(RTLD_DEFAULT, "glLinkProgram");
    if (!real_glLinkProgram_trace) return;
    real_glLinkProgram_trace(program);
    int ok = 1;
    void (*getiv)(unsigned, unsigned, int *) = dlsym(RTLD_DEFAULT, "glGetProgramiv");
    if (getiv) getiv(program, 0x8B82 /* GL_LINK_STATUS */, &ok);
    if (!ok) {
        char log[1024] = {0};
        void (*getlog)(unsigned, int, int *, char *) =
            dlsym(RTLD_DEFAULT, "glGetProgramInfoLog");
        if (getlog) getlog(program, sizeof log - 1, NULL, log);
        fprintf(stderr, "[shader] link falhou program=%u: %s\n", program, log);
    }
    ff5_dual_on_link_program(program);
}

/* A render thread desta Unity conserva o contexto/window EGL current durante
 * toda a vida. Para achar a fronteira nativa de quadro sem adivinhar, acompanhe
 * somente o dispatch GLES que a propria Unity resolveu. O trace e opt-in e
 * agrega os draws entre mudancas de FBO para nao produzir um log por draw. */
static _Thread_local unsigned g_gl_bound_fbo_trace;
static _Thread_local unsigned g_gl_fbo_draws_trace;
static _Thread_local unsigned g_gl_fbo_clears_trace;
static _Thread_local unsigned g_gl_fbo_binds_trace;
static _Thread_local unsigned g_gl_presents_trace;

static void trace_fbo0_pixels(unsigned frame) {
    if (!getenv("FF5_FBO_PIXELS") ||
        (frame != 1 && frame != 120 && frame != 600))
        return;
    int width = egl_shim_width();
    int height = egl_shim_height();
    size_t count = (size_t)width * (size_t)height;
    if (width <= 0 || height <= 0 || count > SIZE_MAX / 4) return;
    unsigned char *rgba = malloc(count * 4);
    if (!rgba) return;
    if (!real_glReadPixels_trace)
        real_glReadPixels_trace = dlsym(RTLD_DEFAULT, "glReadPixels");
    if (!real_glReadPixels_trace) {
        free(rgba);
        return;
    }
    real_glReadPixels_trace(0, 0, width, height, 0x1908 /* GL_RGBA */,
                            0x1401 /* GL_UNSIGNED_BYTE */, rgba);
    unsigned minv[4] = {255, 255, 255, 255};
    unsigned maxv[4] = {0, 0, 0, 0};
    size_t rgb_nonzero = 0, alpha_zero = 0, alpha_nonopaque = 0;
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < count; ++i) {
        const unsigned char *p = rgba + i * 4;
        if (p[0] || p[1] || p[2]) ++rgb_nonzero;
        if (!p[3]) ++alpha_zero;
        if (p[3] != 255) ++alpha_nonopaque;
        for (unsigned c = 0; c < 4; ++c) {
            if (p[c] < minv[c]) minv[c] = p[c];
            if (p[c] > maxv[c]) maxv[c] = p[c];
            hash = (hash ^ p[c]) * 16777619u;
        }
    }
    fprintf(stderr,
            "[fbpixels:%u] %dx%d rgb_nonzero=%zu/%zu alpha_zero=%zu nonopaque=%zu "
            "min=%u,%u,%u,%u max=%u,%u,%u,%u hash=%08x tid=%lx\n",
            frame, width, height, rgb_nonzero, count, alpha_zero,
            alpha_nonopaque, minv[0], minv[1], minv[2], minv[3], maxv[0],
            maxv[1], maxv[2], maxv[3], hash, (unsigned long)pthread_self());
    fflush(stderr);
    free(rgba);
}

static void gl_errlog(const char *what, unsigned seq, int width, int height,
                      unsigned fmt);
static int gl_errlog_enabled(void);
static unsigned (*real_glGetError_trace)(void);
static int gl_upload_trace_enabled(void) { return getenv("FF5_GL_TRACE") != NULL; }
static int gl_upload_stats_enabled(void) { return getenv("FF5_GL_STATS") != NULL; }
static unsigned gl_upload_id(void) {
    return __atomic_add_fetch(&g_gl_upload_seq, 1, __ATOMIC_RELAXED);
}
static size_t gl_uncompressed_bytes(int width, int height,
                                    unsigned format, unsigned type) {
    if (width <= 0 || height <= 0) return 0;
    size_t bytes_per_pixel;
    if (type == 0x8363 /* GL_UNSIGNED_SHORT_5_6_5 */ ||
        type == 0x8033 /* GL_UNSIGNED_SHORT_4_4_4_4 */ ||
        type == 0x8034 /* GL_UNSIGNED_SHORT_5_5_5_1 */) {
        bytes_per_pixel = 2;
    } else if (type == 0x1401 /* GL_UNSIGNED_BYTE */) {
        switch (format) {
        case 0x1906: /* GL_ALPHA */
        case 0x1909: /* GL_LUMINANCE */ bytes_per_pixel = 1; break;
        case 0x190A: /* GL_LUMINANCE_ALPHA */ bytes_per_pixel = 2; break;
        case 0x1907: /* GL_RGB */ bytes_per_pixel = 3; break;
        default: bytes_per_pixel = 4; break; /* RGBA/BGRA */
        }
    } else {
        bytes_per_pixel = 4; /* estimativa conservadora p/ tipos raros */
    }
    return (size_t)width * (size_t)height * bytes_per_pixel;
}
static void gl_record_texture_allocation(const char *kind, unsigned sequence,
                                         unsigned target,
                                         int level, int width, int height,
                                         unsigned format, size_t bytes) {
    if (!gl_upload_stats_enabled() || level != 0 || !bytes) return;
    g_gl_tex_alloc_bytes += bytes;
    unsigned count = ++g_gl_tex_alloc_count;

    unsigned binding_enum = 0;
    if (target == 0x0DE1 /* GL_TEXTURE_2D */)
        binding_enum = 0x8069 /* GL_TEXTURE_BINDING_2D */;
    else if (target == 0x8513 /* GL_TEXTURE_CUBE_MAP */ ||
             (target >= 0x8515 && target <= 0x851A))
        binding_enum = 0x8514 /* GL_TEXTURE_BINDING_CUBE_MAP */;
    int binding = 0;
    if (binding_enum) {
        if (!real_glGetIntegerv_trace)
            real_glGetIntegerv_trace = dlsym(RTLD_DEFAULT, "glGetIntegerv");
        if (real_glGetIntegerv_trace)
            real_glGetIntegerv_trace(binding_enum, &binding);
    }
    if (binding > 0 && (unsigned)binding < FF5_GL_TRACK_IDS) {
        uint32_t old = g_gl_tex_live[binding];
        if (!old) ++g_gl_tex_live_count;
        else g_gl_tex_live_bytes -= old;
        uint32_t tracked = bytes > UINT32_MAX ? UINT32_MAX : (uint32_t)bytes;
        g_gl_tex_live[binding] = tracked;
        g_gl_tex_live_bytes += tracked;
    }
    if (bytes >= 1024u * 1024u || count % 128 == 0) {
        fprintf(stderr,
                "[glstats:%u] %s %dx%d fmt=%x alloc=%.2f MiB total=%.2f MiB "
                "live=%.2f MiB/%u count=%u\n",
                sequence, kind, width, height, format,
                (double)bytes / (1024.0 * 1024.0),
                (double)g_gl_tex_alloc_bytes / (1024.0 * 1024.0),
                (double)g_gl_tex_live_bytes / (1024.0 * 1024.0),
                g_gl_tex_live_count, count);
    }
}
static int gl_unpack_alignment(void) {
    int value = -1;
    if (!real_glGetIntegerv_trace)
        real_glGetIntegerv_trace = dlsym(RTLD_DEFAULT, "glGetIntegerv");
    if (real_glGetIntegerv_trace) real_glGetIntegerv_trace(0x0CF5, &value);
    return value;
}
static void trace_glTexImage2D(unsigned target, int level, int internalformat,
                              int width, int height, int border,
                              unsigned format, unsigned type, const void *pixels) {
    unsigned n = gl_upload_id();
    gl_record_texture_allocation("TexImage2D", n, target, level, width, height, format,
                                 gl_uncompressed_bytes(width, height, format, type));
    if (gl_upload_trace_enabled()) {
        fprintf(stderr, "[gltrace:%u] TexImage2D tgt=%x lvl=%d ifmt=%x %dx%d border=%d fmt=%x type=%x px=%p unpack=%d tid=%lx\n",
                n, target, level, internalformat, width, height, border, format, type, pixels,
                gl_unpack_alignment(), (unsigned long)pthread_self());
        fflush(stderr);
    }
    if (!real_glTexImage2D) real_glTexImage2D = dlsym(RTLD_DEFAULT, "glTexImage2D");
    unsigned pending = 0;
    if (gl_errlog_enabled()) {
        if (!real_glGetError_trace)
            real_glGetError_trace = dlsym(RTLD_DEFAULT, "glGetError");
        // Drena erro herdado de chamadas nao rastreadas; sem isso o upload
        // seguinte levaria a culpa de um erro alheio.
        if (real_glGetError_trace)
            while ((pending = real_glGetError_trace()) != 0)
                fprintf(stderr, "[glerr:%u] (pendente de chamada anterior) 0x%x\n",
                        n, pending);
    }
    if (real_glTexImage2D)
        real_glTexImage2D(target, level, internalformat, width, height, border,
                          format, type, pixels);
    if (gl_errlog_enabled()) {
        unsigned err = real_glGetError_trace ? real_glGetError_trace() : 0;
        if (err)
            fprintf(stderr,
                    "[glerr:%u] TexImage2D tgt=%x lvl=%d ifmt=%x %dx%d border=%d "
                    "fmt=%x type=%x px=%p -> GL error 0x%x\n",
                    n, target, level, internalformat, width, height, border,
                    format, type, pixels, err);
    }
}
static void trace_glTexSubImage2D(unsigned target, int level, int xoffset,
                                 int yoffset, int width, int height,
                                 unsigned format, unsigned type, const void *pixels) {
    unsigned n = gl_upload_id();
    if (gl_upload_trace_enabled()) {
        fprintf(stderr, "[gltrace:%u] TexSubImage2D tgt=%x lvl=%d xy=%d,%d %dx%d fmt=%x type=%x px=%p\n",
                n, target, level, xoffset, yoffset, width, height, format, type, pixels);
        fflush(stderr);
    }
    if (!real_glTexSubImage2D) real_glTexSubImage2D = dlsym(RTLD_DEFAULT, "glTexSubImage2D");
    if (real_glTexSubImage2D)
        real_glTexSubImage2D(target, level, xoffset, yoffset, width, height,
                             format, type, pixels);
    gl_errlog("TexSubImage2D", n, width, height, format);
}
static void trace_glCompressedTexImage2D(unsigned target, int level,
                                        unsigned internalformat, int width,
                                        int height, int border, int image_size,
                                        const void *data) {
    unsigned n = gl_upload_id();
    gl_record_texture_allocation("CompressedTexImage2D", n, target, level, width,
                                 height, internalformat,
                                 image_size > 0 ? (size_t)image_size : 0);
    if (gl_upload_trace_enabled()) {
        fprintf(stderr, "[gltrace:%u] CompressedTexImage2D tgt=%x lvl=%d ifmt=%x %dx%d border=%d size=%d px=%p\n",
                n, target, level, internalformat, width, height, border, image_size, data);
        fflush(stderr);
    }
    if (!real_glCompressedTexImage2D)
        real_glCompressedTexImage2D = dlsym(RTLD_DEFAULT, "glCompressedTexImage2D");
    if (real_glCompressedTexImage2D)
        real_glCompressedTexImage2D(target, level, internalformat, width, height,
                                    border, image_size, data);
    gl_errlog("CompressedTexImage2D", n, width, height, internalformat);
    ff5_dual_on_compressed_upload(target, level, internalformat, width, height,
                                  image_size, data);
}
static void trace_glCompressedTexSubImage2D(unsigned target, int level,
                                           int xoffset, int yoffset, int width,
                                           int height, unsigned format,
                                           int image_size, const void *data) {
    unsigned n = gl_upload_id();
    if (gl_upload_trace_enabled()) {
        fprintf(stderr, "[gltrace:%u] CompressedTexSubImage2D tgt=%x lvl=%d xy=%d,%d %dx%d fmt=%x size=%d px=%p\n",
                n, target, level, xoffset, yoffset, width, height, format, image_size, data);
        fflush(stderr);
    }
    if (!real_glCompressedTexSubImage2D)
        real_glCompressedTexSubImage2D = dlsym(RTLD_DEFAULT, "glCompressedTexSubImage2D");
    if (real_glCompressedTexSubImage2D)
        real_glCompressedTexSubImage2D(target, level, xoffset, yoffset, width,
                                       height, format, image_size, data);
}
static void trace_glPixelStorei(unsigned pname, int param) {
    if (gl_upload_trace_enabled()) {
        fprintf(stderr, "[gltrace] PixelStorei pname=%x param=%d tid=%lx\n",
                pname, param, (unsigned long)pthread_self());
        fflush(stderr);
    }
    if (!real_glPixelStorei) real_glPixelStorei = dlsym(RTLD_DEFAULT, "glPixelStorei");
    if (real_glPixelStorei) real_glPixelStorei(pname, param);
}

static void trace_glDeleteTextures(int n, const unsigned *textures) {
    uint64_t freed = 0;
    unsigned removed = 0;
    if (gl_upload_stats_enabled() && textures && n > 0) {
        for (int i = 0; i < n; ++i) {
            unsigned id = textures[i];
            if (id >= FF5_GL_TRACK_IDS || !g_gl_tex_live[id]) continue;
            freed += g_gl_tex_live[id];
            g_gl_tex_live_bytes -= g_gl_tex_live[id];
            g_gl_tex_live[id] = 0;
            --g_gl_tex_live_count;
            ++removed;
        }
        if (freed >= 1024u * 1024u || removed >= 64) {
            fprintf(stderr,
                    "[glstats] DeleteTextures n=%d tracked=%u freed=%.2f MiB "
                    "live=%.2f MiB/%u\n",
                    n, removed, (double)freed / (1024.0 * 1024.0),
                    (double)g_gl_tex_live_bytes / (1024.0 * 1024.0),
                    g_gl_tex_live_count);
        }
    }
    if (!real_glDeleteTextures_trace)
        real_glDeleteTextures_trace = dlsym(RTLD_DEFAULT, "glDeleteTextures");
    if (real_glDeleteTextures_trace)
        real_glDeleteTextures_trace(n, textures);
    ff5_dual_on_delete_textures(n, textures);
}

/* rastreio de estado p/ as gemeas de alpha (etc1_dual) */
static void (*real_glBindTexture_dual)(unsigned, unsigned);
static void trace_glBindTexture(unsigned target, unsigned texture) {
    if (!real_glBindTexture_dual)
        real_glBindTexture_dual = dlsym(RTLD_DEFAULT, "glBindTexture");
    if (real_glBindTexture_dual) real_glBindTexture_dual(target, texture);
    ff5_dual_on_bind_texture(target, texture);
}
static void (*real_glActiveTexture_dual)(unsigned);
static void trace_glActiveTexture(unsigned unit) {
    if (!real_glActiveTexture_dual)
        real_glActiveTexture_dual = dlsym(RTLD_DEFAULT, "glActiveTexture");
    if (real_glActiveTexture_dual) real_glActiveTexture_dual(unit);
    ff5_dual_on_active_texture(unit);
}
static void (*real_glUseProgram_dual)(unsigned);
static void trace_glUseProgram(unsigned program) {
    if (!real_glUseProgram_dual)
        real_glUseProgram_dual = dlsym(RTLD_DEFAULT, "glUseProgram");
    if (real_glUseProgram_dual) real_glUseProgram_dual(program);
    ff5_dual_on_use_program(program);
}
static void (*real_glUniform1i_dual)(int, int);
static void trace_glUniform1i(int location, int v0) {
    if (!real_glUniform1i_dual)
        real_glUniform1i_dual = dlsym(RTLD_DEFAULT, "glUniform1i");
    if (real_glUniform1i_dual) real_glUniform1i_dual(location, v0);
    ff5_dual_on_uniform1i(location, v0);
}

/* FF5_GL_ERRLOG: diagnostico de upload/RT rejeitados pelo blob Mali (glGet* =
 * stall; so com env). Cobre a tese dos sprites invisiveis: textura recusada ou
 * FBO incompleto amostra lixo/preto sem nenhum log. */
static int gl_errlog_enabled(void) { return getenv("FF5_GL_ERRLOG") != NULL; }
static void gl_errlog(const char *what, unsigned seq, int width, int height,
                      unsigned fmt) {
    if (!gl_errlog_enabled()) return;
    if (!real_glGetError_trace)
        real_glGetError_trace = dlsym(RTLD_DEFAULT, "glGetError");
    if (!real_glGetError_trace) return;
    unsigned err = real_glGetError_trace();
    if (err)
        fprintf(stderr, "[glerr:%u] %s %dx%d fmt=%x -> GL error 0x%x\n",
                seq, what, width, height, fmt, err);
}
static void (*real_glFramebufferTexture2D_trace)(unsigned, unsigned, unsigned,
                                                 unsigned, int);
static void trace_glFramebufferTexture2D(unsigned target, unsigned attachment,
                                         unsigned textarget, unsigned texture,
                                         int level) {
    if (!real_glFramebufferTexture2D_trace)
        real_glFramebufferTexture2D_trace =
            dlsym(RTLD_DEFAULT, "glFramebufferTexture2D");
    if (real_glFramebufferTexture2D_trace)
        real_glFramebufferTexture2D_trace(target, attachment, textarget,
                                          texture, level);
    if (gl_errlog_enabled()) {
        fprintf(stderr, "[fberr] FramebufferTexture2D att=%x tex=%u lvl=%d\n",
                attachment, texture, level);
        gl_errlog("FramebufferTexture2D", 0, 0, 0, attachment);
    }
}
static void (*real_glRenderbufferStorage_trace)(unsigned, unsigned, int, int);
static void trace_glRenderbufferStorage(unsigned target, unsigned internalformat,
                                        int width, int height) {
    if (!real_glRenderbufferStorage_trace)
        real_glRenderbufferStorage_trace =
            dlsym(RTLD_DEFAULT, "glRenderbufferStorage");
    if (real_glRenderbufferStorage_trace)
        real_glRenderbufferStorage_trace(target, internalformat, width, height);
    if (gl_errlog_enabled()) {
        fprintf(stderr, "[fberr] RenderbufferStorage fmt=%x %dx%d\n",
                internalformat, width, height);
        gl_errlog("RenderbufferStorage", 0, width, height, internalformat);
    }
}
static unsigned (*real_glCheckFramebufferStatus_trace)(unsigned);
static unsigned trace_glCheckFramebufferStatus(unsigned target) {
    if (!real_glCheckFramebufferStatus_trace)
        real_glCheckFramebufferStatus_trace =
            dlsym(RTLD_DEFAULT, "glCheckFramebufferStatus");
    unsigned status = real_glCheckFramebufferStatus_trace
                          ? real_glCheckFramebufferStatus_trace(target)
                          : 0;
    if (gl_errlog_enabled() && status != 0x8CD5 /* GL_FRAMEBUFFER_COMPLETE */)
        fprintf(stderr, "[fberr] CheckFramebufferStatus target=%x -> 0x%x "
                        "(INCOMPLETO) fbo=%u\n",
                target, status, g_gl_bound_fbo_trace);
    return status;
}

static int gl_fbo_trace_enabled(void) { return getenv("FF5_FBO_TRACE") != NULL; }
static void trace_glBindFramebuffer(unsigned target, unsigned framebuffer) {
    unsigned previous = g_gl_bound_fbo_trace;
    unsigned sequence = ++g_gl_fbo_binds_trace;
    if (gl_fbo_trace_enabled() && (sequence <= 600 || previous == 0 || framebuffer == 0)) {
        fprintf(stderr,
                "[fbtrace:%u] bind %u->%u draws=%u clears=%u target=%x tid=%lx\n",
                sequence, previous, framebuffer, g_gl_fbo_draws_trace,
                g_gl_fbo_clears_trace, target, (unsigned long)pthread_self());
        fflush(stderr);
    }
    /* O trace do FF5 mostrou uma fronteira invariavel por quadro:
     *   ... FBO 1 -> FBO 0 (composite final: 1 draw + clear) -> FBO 2 ...
     * A Unity inicia o proximo quadro ao sair do FBO0. Nesse instante todos os
     * draws do composite ja estao acumulados e a window ainda e current nesta
     * mesma gfx thread, portanto este e o ponto nativo seguro para um unico
     * swap. CRAVADO como padrao ON: e o caminho provado na tela (SceneTitle ->
     * logo FINAL FANTASY V no Mali). O que funciona NAO pode depender de env —
     * sem essa apresentacao a tela fica preta. FF5_NO_FBO_PRESENT desliga so
     * para diagnostico. */
    if (!getenv("FF5_NO_FBO_PRESENT") && previous == 0 && framebuffer != 0 &&
        g_gl_fbo_draws_trace != 0) {
        unsigned present = ++g_gl_presents_trace;
        trace_fbo0_pixels(present);
        np_cursor_draw();
        egl_shim_force_present("next-frame-fbo-bind");
    }
    g_gl_bound_fbo_trace = framebuffer;
    g_gl_fbo_draws_trace = 0;
    g_gl_fbo_clears_trace = 0;
    if (!real_glBindFramebuffer_trace)
        real_glBindFramebuffer_trace = dlsym(RTLD_DEFAULT, "glBindFramebuffer");
    if (real_glBindFramebuffer_trace)
        real_glBindFramebuffer_trace(target, framebuffer);
}

static void trace_glDrawArrays(unsigned mode, int first, int count) {
    ++g_gl_fbo_draws_trace;
    ff5_dual_before_draw();
    if (!real_glDrawArrays_trace)
        real_glDrawArrays_trace = dlsym(RTLD_DEFAULT, "glDrawArrays");
    if (real_glDrawArrays_trace) real_glDrawArrays_trace(mode, first, count);
}

static void trace_glDrawElements(unsigned mode, int count, unsigned type,
                                 const void *indices) {
    ++g_gl_fbo_draws_trace;
    ff5_dual_before_draw();
    if (!real_glDrawElements_trace)
        real_glDrawElements_trace = dlsym(RTLD_DEFAULT, "glDrawElements");
    if (real_glDrawElements_trace)
        real_glDrawElements_trace(mode, count, type, indices);
}

static void trace_glClear(unsigned mask) {
    ++g_gl_fbo_clears_trace;
    if (!real_glClear_trace) real_glClear_trace = dlsym(RTLD_DEFAULT, "glClear");
    if (real_glClear_trace) real_glClear_trace(mask);
}

void *ff5_gl_override(const char *name) {
    if (!name) return NULL;
    if (!strcmp(name, "glTexImage2D")) return (void *)trace_glTexImage2D;
    if (!strcmp(name, "glTexSubImage2D")) return (void *)trace_glTexSubImage2D;
    if (!strcmp(name, "glCompressedTexImage2D")) return (void *)trace_glCompressedTexImage2D;
    if (!strcmp(name, "glCompressedTexSubImage2D")) return (void *)trace_glCompressedTexSubImage2D;
    if (!strcmp(name, "glPixelStorei")) return (void *)trace_glPixelStorei;
    if (!strcmp(name, "glDeleteTextures")) return (void *)trace_glDeleteTextures;
    if (!strcmp(name, "glBindFramebuffer") || !strcmp(name, "glBindFramebufferOES"))
        return (void *)trace_glBindFramebuffer;
    if (!strcmp(name, "glFramebufferTexture2D") ||
        !strcmp(name, "glFramebufferTexture2DOES"))
        return (void *)trace_glFramebufferTexture2D;
    if (!strcmp(name, "glRenderbufferStorage") ||
        !strcmp(name, "glRenderbufferStorageOES"))
        return (void *)trace_glRenderbufferStorage;
    if (!strcmp(name, "glCheckFramebufferStatus") ||
        !strcmp(name, "glCheckFramebufferStatusOES"))
        return (void *)trace_glCheckFramebufferStatus;
    if (!strcmp(name, "glDrawArrays")) return (void *)trace_glDrawArrays;
    if (!strcmp(name, "glDrawElements")) return (void *)trace_glDrawElements;
    if (!strcmp(name, "glClear")) return (void *)trace_glClear;
    if (!strcmp(name, "glShaderSource")) return (void *)trace_glShaderSource;
    /* O rastreio de estado so existe para as gemeas de alpha; sem dupla camada
     * a Unity deve falar direto com o driver (todo draw passa por aqui). */
    if (ff5_dual_enabled()) {
        if (!strcmp(name, "glBindTexture")) return (void *)trace_glBindTexture;
        if (!strcmp(name, "glActiveTexture")) return (void *)trace_glActiveTexture;
        if (!strcmp(name, "glUseProgram")) return (void *)trace_glUseProgram;
        if (!strcmp(name, "glUniform1i")) return (void *)trace_glUniform1i;
    }
    if (!strcmp(name, "glCompileShader")) return (void *)trace_glCompileShader;
    if (!strcmp(name, "glLinkProgram")) return (void *)trace_glLinkProgram;
    if (!strcmp(name, "glGetString")) return (void *)my_glGetString;
    return softfp_gl_resolve(name);
}

/* A Unity 2019 entrega o frame final ao soltar a window surface na gfx thread,
 * mas este build nao passa por eglSwapBuffers no import interceptado. Nesse
 * instante a surface real ainda esta current; e o unico ponto seguro para
 * apresentar o quadro completo no Mali fbdev (mesmo fluxo provado no RE4). */
void ff5_frame_end_present(void) {
    egl_shim_force_present("frame-end-release");
}

static int my_system_property_get(const char *key, char *val) {
    if (val) val[0] = 0;
    return 0;
}

// sysconf: o guest e BIONIC (constantes _SC_* != glibc). Traduz as que a Unity
// usa p/ dimensionar heap/threads (bionic: PAGE_SIZE/PAGESIZE=39/40,
// NPROCESSORS_CONF/ONLN=96/97, _SC_PHYS_PAGES/AVPHYS_PAGES=nao-def no bionic ->
// Unity le /proc). Damos valores sanos e logamos o resto.
static long my_sysconf(int name) {
    long r;
    switch (name) {
    case 39: case 40: r = 4096; break;                 // _SC_PAGE_SIZE/PAGESIZE (bionic)
    case 96: case 97: r = ff5_ncpu(); break;           // _SC_NPROCESSORS_CONF/ONLN (bionic) -> 1 (job inline)
    case 0x0058: r = 262144; break;                    // fallback phys pages
    default:
        r = sysconf(name);
        if (getenv("FF5_DBG")) fprintf(stderr, "[ff5] sysconf(%d) = %ld\n", name, r);
        break;
    }
    return r;
}

// O engine instala handler proprio de SIGSEGV/etc e RE-RAISE (esconde o PC do
// crash real). Bloqueamos o registro dos sinais fatais p/ NOSSO handler pegar
// a falha original (pc na libunity). Demais sinais passam.
static int is_fatal_sig(int s) {
    return s == SIGSEGV || s == SIGBUS || s == SIGABRT || s == SIGILL || s == SIGFPE;
}
// struct sigaction do guest = BIONIC armv7 (LP32) = 16 BYTES, layout
// handler-first: { void* sa_handler; unsigned long sa_mask(4B); int sa_flags;
// void* sa_restorer; }. sigset_t bionic classico = 4B (sinais 1..32).
// glibc = ~140B (sa_mask=128B). O memmgr da Unity (libunity+0x2456fc) guarda
// cada oldact num slot de 16B (stride sig*16): passar esse slot direto p/ a
// glibc (ou memset de 80B) ESTOURA os slots vizinhos e corrompe um ponteiro
// de funcao do memmgr -> blx p/ .bss (0xc72e70). Traduzir nos dois sentidos,
// escrevendo EXATAMENTE 16B no oldact do guest. (mesma classe do pthread_sigmask)
struct bionic_sigaction {
    void         *bsa_handler;    // union handler/sigaction (mesmo offset)
    unsigned long bsa_mask;       // 4B no armv7
    int           bsa_flags;
    void         *bsa_restorer;
};
static int my_sigaction(int sig, const void *act_, void *old_) {
    const struct bionic_sigaction *act = act_;
    struct bionic_sigaction *old = old_;
    if (getenv("FF5_DBG"))
        fprintf(stderr, "[ff5] sigaction(sig=%d)%s\n", sig, is_fatal_sig(sig) ? " BLOQUEADO" : "");
    if (is_fatal_sig(sig)) {
        // mantem NOSSO crash handler; devolve oldact benigno (SIG_DFL) em
        // formato BIONIC, EXATAMENTE 16B (nunca mais -> estouraria os vizinhos).
        if (old) { old->bsa_handler = NULL; old->bsa_mask = 0;
                   old->bsa_flags = 0; old->bsa_restorer = NULL; }
        return 0;   // finge sucesso
    }
    struct sigaction ga, go, *pga = NULL, *pgo = NULL;
    if (act) {
        memset(&ga, 0, sizeof ga);
        ga.sa_flags = act->bsa_flags & ~0x04000000; // tira SA_RESTORER: glibc poe o dela
        if (act->bsa_flags & SA_SIGINFO)
            ga.sa_sigaction = (void (*)(int, siginfo_t *, void *))act->bsa_handler;
        else
            ga.sa_handler = (void (*)(int))act->bsa_handler;
        sigemptyset(&ga.sa_mask);
        for (int s = 1; s <= 32; s++)
            if (act->bsa_mask & (1UL << (s - 1))) sigaddset(&ga.sa_mask, s);
        pga = &ga;
    }
    if (old) { memset(&go, 0, sizeof go); pgo = &go; }
    int r = sigaction(sig, pga, pgo);
    if (old) {
        old->bsa_handler = (go.sa_flags & SA_SIGINFO) ? (void *)go.sa_sigaction : (void *)go.sa_handler;
        unsigned long m = 0;
        for (int s = 1; s <= 32; s++) if (sigismember(&go.sa_mask, s)) m |= (1UL << (s - 1));
        old->bsa_mask = m;
        old->bsa_flags = go.sa_flags;
        old->bsa_restorer = NULL;
    }
    return r;
}
/* libil2cpp usa sigsuspend no handshake stop-the-world do Boehm GC. A mascara
 * recebida aqui e sigset_t do Bionic armv7 (4 bytes); entrega-la diretamente a
 * glibc faria sigsuspend ler 128 bytes da stack e esperar com uma mascara
 * aleatoria. Converta a mascara, preservando o bloqueio real ate o sinal de
 * restart do GC — nao substitua nem encurte o protocolo nativo. */
static int my_sigsuspend(const void *mask_) {
    if (!mask_) {
        errno = EINVAL;
        return -1;
    }
    uint32_t bionic_mask = *(const uint32_t *)mask_;
    sigset_t host_mask;
    sigemptyset(&host_mask);
    for (int s = 1; s <= 32; ++s)
        if (bionic_mask & (1U << (s - 1))) sigaddset(&host_mask, s);
    return sigsuspend(&host_mask);
}
static void *my_signal(int sig, void *h) {
    if (is_fatal_sig(sig)) return NULL;
    return (void *)signal(sig, (void (*)(int))h);
}
static void my_exit(int code) { fprintf(stderr, "[ff5] guest exit(%d)\n", code); _exit(code); }
static void my_abort(void) { fprintf(stderr, "[ff5] guest abort()\n"); _exit(1); }

// mmap instrumentado: loga tamanho e falhas (Unity reserva heap via mmap; num
// processo 32-bit o espaco de enderecos pode faltar).
static void *my_mmap(void *addr, size_t len, int prot, int flags, int fd, long off) {
    static void *(*real)(void *, size_t, int, int, int, long);
    if (!real) real = (void *)dlsym(RTLD_NEXT, "mmap");
    void *r = real(addr, len, prot, flags, fd, off);
    if (r == (void *)-1 || len >= 8u * 1024 * 1024)
        fprintf(stderr, "[ff5] mmap(len=%zu KB, prot=%d, flags=0x%x) = %p%s\n",
                len / 1024, prot, flags, r, r == (void *)-1 ? " FALHOU" : "");
    return r;
}
static void *my_malloc_big(size_t n) {
    static void *(*real)(size_t);
    if (!real) real = (void *)dlsym(RTLD_NEXT, "malloc");
    void *r = real(n);
    if (!r && n) fprintf(stderr, "[ff5] malloc(%zu KB) = NULL\n", n / 1024);
    else if (r && n >= 1024u * 1024u && getenv("FF5_ALLOC_TRACE")) {
        uintptr_t caller = (uintptr_t)__builtin_return_address(0);
        const char *module = "host";
        unsigned long offset = (unsigned long)caller;
        if (g_unity_base && caller >= g_unity_base &&
            caller < g_unity_base + 0x1400000u) {
            module = "libunity"; offset = (unsigned long)(caller - g_unity_base);
        } else if (g_il2cpp_base && caller >= g_il2cpp_base &&
                   caller < g_il2cpp_base + 0x4000000u) {
            module = "libil2cpp"; offset = (unsigned long)(caller - g_il2cpp_base);
        }
        fprintf(stderr, "[alloc] malloc %.2f MiB -> %p caller=%s+0x%lx\n",
                (double)n / (1024.0 * 1024.0), r, module, offset);
    }
    return r;
}

// INTERPOSICAO GLOBAL (exe -rdynamic): definir raise/pthread_kill/abort captura
// QUALQUER caller (guest + glibc-interno via PLT). Aqui o backtrace roda em
// contexto NORMAL (nao signal) -> com dl_iterate_phdr desenrola a libunity.
static void attrib(const char *tag, void *a) {
    uintptr_t v = (uintptr_t)a;
    if (g_unity_base && v >= g_unity_base && v < g_unity_base + 0x1400000)
        fprintf(stderr, "   %s libunity+0x%lx\n", tag, (unsigned long)(v - g_unity_base));
    else if (g_il2cpp_base && v >= g_il2cpp_base && v < g_il2cpp_base + 0x4000000)
        fprintf(stderr, "   %s libil2cpp+0x%lx\n", tag, (unsigned long)(v - g_il2cpp_base));
    else {
        Dl_info di;
        if (dladdr(a, &di) && di.dli_fname)
            fprintf(stderr, "   %s %p (%s+0x%lx %s)\n", tag, a, di.dli_fname,
                    (unsigned long)(v - (uintptr_t)di.dli_fbase),
                    di.dli_sname ? di.dli_sname : "?");
        else fprintf(stderr, "   %s %p\n", tag, a);
    }
}
static void dump_bt(const char *who, int sig, void *r0, void *r1, void *r2) {
    fprintf(stderr, "\n[ff5] %s(sig=%d) callers:\n", who, sig);
    attrib("ret0", r0); attrib("ret1", r1); attrib("ret2", r2);
    fflush(stderr);
}
// Pass-through: Unity/GC usa pthread_kill p/ suspender threads; NAO podemos
// _exit. Logamos so sinais FATAIS (crash real) e repassamos ao real; sinais
// de gestao (SIGXCPU/PWR/etc) passam silenciosos.
static int fatal_s(int s){ return s==4||s==6||s==7||s==8||s==11; }
int raise(int sig) {
    static int (*real)(int); if (!real) real = dlsym(RTLD_NEXT, "raise");
    if (fatal_s(sig)) dump_bt("raise", sig, __builtin_return_address(0), __builtin_return_address(1), __builtin_return_address(2));
    return real ? real(sig) : 0;
}
int pthread_kill(unsigned long t, int sig) {
    static int (*real)(unsigned long, int); if (!real) real = dlsym(RTLD_NEXT, "pthread_kill");
    if (fatal_s(sig)) dump_bt("pthread_kill", sig, __builtin_return_address(0), __builtin_return_address(1), __builtin_return_address(2));
    return real ? real(t, sig) : 0;
}
void abort(void) {
    static void (*real)(void); if (!real) real = dlsym(RTLD_NEXT, "abort");
    dump_bt("abort", 6, __builtin_return_address(0), __builtin_return_address(1), __builtin_return_address(2));
    if (real) real();
    _exit(134);
}

// ---- crash: PC + endereco faltoso, relativo aos modulos -------------------
extern void *text_virtbase;   // base do modulo ATUAL no so_util
static void on_crash(int sig, siginfo_t *si, void *uc) {
    ucontext_t *u = (ucontext_t *)uc;
    uintptr_t pc = u ? (uintptr_t)u->uc_mcontext.arm_pc : 0;
    uintptr_t lr = u ? (uintptr_t)u->uc_mcontext.arm_lr : 0;
    void *fault = si ? si->si_addr : 0;
    fprintf(stderr, "\n[ff5] SINAL %d  fault=%p  pc=0x%lx  lr=0x%lx\n",
            sig, fault, (unsigned long)pc, (unsigned long)lr);
    if (u) {
        mcontext_t *m = &u->uc_mcontext;
        fprintf(stderr, "      r0=%08lx r1=%08lx r2=%08lx r3=%08lx\n",
                m->arm_r0, m->arm_r1, m->arm_r2, m->arm_r3);
        fprintf(stderr, "      r4=%08lx r5=%08lx r6=%08lx r7=%08lx\n",
                m->arm_r4, m->arm_r5, m->arm_r6, m->arm_r7);
        fprintf(stderr, "      r8=%08lx r9=%08lx r10=%08lx fp=%08lx ip=%08lx sp=%08lx\n",
                m->arm_r8, m->arm_r9, m->arm_r10, m->arm_fp, m->arm_ip, m->arm_sp);
        // varre a pilha por retornos VALIDOS de codigo (libunity .text: <0xc6a600,
        // thumb=impar). Exclui .bss (zerado) que poluia antes.
        uintptr_t *st = (uintptr_t *)m->arm_sp;
        fprintf(stderr, "      cadeia de callers (libunity .text):\n");
        int found = 0;
        for (int k = 0; k < 800 && found < 16; k++) {
            uintptr_t v = st[k];
            if (g_unity_base && (v & 1) && v > g_unity_base + 0x1000 &&
                v < g_unity_base + 0xc6a600) {
                fprintf(stderr, "        [sp+%d] libunity+0x%lx\n", k * 4,
                        (unsigned long)(v - 1 - g_unity_base));
                found++;
            } else if (g_il2cpp_base && (v & 1) && v > g_il2cpp_base + 0x1000 &&
                       v < g_il2cpp_base + 0x3000000) {
                fprintf(stderr, "        [sp+%d] libil2cpp+0x%lx\n", k * 4,
                        (unsigned long)(v - 1 - g_il2cpp_base));
                found++;
            }
        }
    }
    int pc_guest = 0, lr_guest = 0;
    if (g_unity_base && pc >= g_unity_base && pc < g_unity_base + 0x1400000) {
        fprintf(stderr, "      pc = libunity+0x%lx\n", (unsigned long)(pc - g_unity_base));
        pc_guest = 1;
    }
    if (g_il2cpp_base && pc >= g_il2cpp_base && pc < g_il2cpp_base + 0x4000000) {
        fprintf(stderr, "      pc = libil2cpp+0x%lx\n", (unsigned long)(pc - g_il2cpp_base));
        pc_guest = 1;
    }
    if (g_unity_base && lr >= g_unity_base && lr < g_unity_base + 0x1400000) {
        fprintf(stderr, "      lr = libunity+0x%lx\n", (unsigned long)(lr - g_unity_base));
        lr_guest = 1;
    }
    if (g_il2cpp_base && lr >= g_il2cpp_base && lr < g_il2cpp_base + 0x4000000) {
        fprintf(stderr, "      lr = libil2cpp+0x%lx\n", (unsigned long)(lr - g_il2cpp_base));
        lr_guest = 1;
    }
    if (!pc_guest) attrib("pc-host", (void *)pc);
    if (!lr_guest) attrib("lr-host", (void *)lr);
    fflush(stderr);
    _exit(128 + sig);
}
static void install_crash_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL); sigaction(SIGFPE, &sa, NULL);
    if (getenv("FF5_JOB_TRACE") || getenv("FF5_IL2CPP_TRACE")) {
        sa.sa_sigaction = on_job_trace;
        sigaction(SIGILL, &sa, NULL);
        sigaction(SIGTRAP, &sa, NULL);
    } else {
        sigaction(SIGILL, &sa, NULL);
        sigaction(SIGTRAP, &sa, NULL);
    }
}

// ---- registro dos overrides EGL/GL/dl no resolvedor ----------------------
static void register_overrides(void) {
    // EGL -> egl_shim (forca config ES2 no Mali)
    ff5_add_override("eglGetDisplay",            (void *)egl_shim_GetDisplay);
    ff5_add_override("eglInitialize",            (void *)egl_shim_Initialize);
    ff5_add_override("eglTerminate",             (void *)egl_shim_Terminate);
    ff5_add_override("eglChooseConfig",          (void *)egl_shim_ChooseConfig);
    ff5_add_override("eglCreateWindowSurface",   (void *)egl_shim_CreateWindowSurface);
    ff5_add_override("eglCreatePbufferSurface",  (void *)egl_shim_CreatePbufferSurface);
    ff5_add_override("eglCreateContext",         (void *)egl_shim_CreateContext);
    ff5_add_override("eglMakeCurrent",           (void *)egl_shim_MakeCurrent);
    ff5_add_override("eglSwapBuffers",           (void *)egl_shim_SwapBuffers);
    ff5_add_override("eglDestroySurface",        (void *)egl_shim_DestroySurface);
    ff5_add_override("eglDestroyContext",        (void *)egl_shim_DestroyContext);
    ff5_add_override("eglQuerySurface",          (void *)egl_shim_QuerySurface);
    ff5_add_override("eglGetConfigAttrib",       (void *)egl_shim_GetConfigAttrib);
    ff5_add_override("eglGetError",              (void *)egl_shim_GetError);
    ff5_add_override("eglGetProcAddress",        (void *)egl_shim_GetProcAddress);
    ff5_add_override("eglQueryString",           (void *)egl_shim_QueryString);
    ff5_add_override("eglSwapInterval",          (void *)egl_shim_SwapInterval);
    ff5_add_override("eglGetCurrentContext",     (void *)egl_shim_GetCurrentContext);
    ff5_add_override("eglGetCurrentSurface",     (void *)egl_shim_GetCurrentSurface);
    ff5_add_override("__gnu_Unwind_Find_exidx",  (void *)ff5_find_exidx);
    // dl* -> cross-modulo
    ff5_add_override("dlopen",  (void *)my_dlopen);
    ff5_add_override("dlsym",   (void *)my_dlsym);
    ff5_add_override("dlclose", (void *)my_dlclose);
    ff5_add_override("dlerror", (void *)my_dlerror);
    ff5_add_override("dladdr",  (void *)my_dladdr);
    // diversos
    ff5_add_override("glGetString",           (void *)my_glGetString);
    ff5_add_override("glTexImage2D",          (void *)trace_glTexImage2D);
    ff5_add_override("glTexSubImage2D",       (void *)trace_glTexSubImage2D);
    ff5_add_override("glCompressedTexImage2D", (void *)trace_glCompressedTexImage2D);
    ff5_add_override("glCompressedTexSubImage2D", (void *)trace_glCompressedTexSubImage2D);
    ff5_add_override("glDeleteTextures",       (void *)trace_glDeleteTextures);
    ff5_add_override("glFramebufferTexture2D", (void *)trace_glFramebufferTexture2D);
    ff5_add_override("glRenderbufferStorage",  (void *)trace_glRenderbufferStorage);
    ff5_add_override("glCheckFramebufferStatus", (void *)trace_glCheckFramebufferStatus);
    ff5_add_override("glShaderSource",        (void *)trace_glShaderSource);
    if (ff5_dual_enabled()) {
        ff5_add_override("glBindTexture",      (void *)trace_glBindTexture);
        ff5_add_override("glActiveTexture",    (void *)trace_glActiveTexture);
        ff5_add_override("glUseProgram",       (void *)trace_glUseProgram);
        ff5_add_override("glUniform1i",        (void *)trace_glUniform1i);
    }
    ff5_add_override("glCompileShader",       (void *)trace_glCompileShader);
    ff5_add_override("glLinkProgram",          (void *)trace_glLinkProgram);
    ff5_add_override("__system_property_get", (void *)my_system_property_get);
    ff5_add_override("mmap",  (void *)my_mmap);
    ff5_add_override("mmap64", (void *)my_mmap);
    ff5_add_override("malloc", (void *)my_malloc_big);
    ff5_add_override("sysconf", (void *)my_sysconf);
    ff5_add_override("sigaction", (void *)my_sigaction);
    ff5_add_override("__sigaction", (void *)my_sigaction);
    ff5_add_override("__libc_sigaction", (void *)my_sigaction);
    ff5_add_override("sigsuspend", (void *)my_sigsuspend);
    ff5_add_override("signal",    (void *)my_signal);
    ff5_add_override("bsd_signal", (void *)my_signal);
    ff5_add_override("sysv_signal", (void *)my_signal);
    ff5_add_override("exit",  (void *)my_exit);
    ff5_add_override("_exit", (void *)my_exit);
    ff5_add_override("abort", (void *)my_abort);
}

// ---- carrega um modulo do jogo -------------------------------------------
static so_module *load_module(const char *soname, size_t heap) {
    char path[600];
    snprintf(path, sizeof path, "%s/%s", g_gamedir, soname);
    void *base = mmap(NULL, heap, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) { fprintf(stderr, "[ff5] mmap %s falhou\n", soname); return NULL; }
    if (so_load(path, base, heap) != 0) { fprintf(stderr, "[ff5] so_load %s falhou\n", soname); return NULL; }
    if (strstr(soname, "il2cpp")) g_il2cpp_base = (uintptr_t)text_virtbase;
    else if (strstr(soname, "unity")) g_unity_base = (uintptr_t)text_virtbase;
    so_record_phdr(soname);   // p/ o dl_iterate_phdr custom (unwind de excecoes C++)
    fprintf(stderr, "[ff5] %s: base=%p load ok, relocate...\n", soname, text_virtbase);
    so_relocate();
    fprintf(stderr, "[ff5] %s: relocate ok, resolve...\n", soname);
    so_resolve(NULL, 0, 1);          // fallback = ff5_resolve (pthread/sem/etc)
    fprintf(stderr, "[ff5] %s: resolve ok, finalize...\n", soname);
    so_finalize();
    so_flush_caches();
    fprintf(stderr, "[ff5] %s: finalize ok, init_array...\n", soname);
    so_execute_init_array();
    /* so_load le o ELF inteiro para uma imagem temporaria. Relocacoes,
     * imports, phdr e init_array ja foram consumidos; dynsym/dynstr vivem no
     * PT_LOAD. Num aparelho de 1 GB, liberar aqui evita manter mais ~55 MB de
     * copias de libil2cpp/libunity durante todo o jogo. */
    so_free_temp();
    fprintf(stderr, "[ff5] %s carregado (%zu KB heap)\n", soname, heap / 1024);
    return so_save();
}

/* O libil2cpp distribuido no APK traz um hook ARM de endereco fixo no corpo
 * de UnityEngine.Debug.Log(object):
 *
 *     ldr pc, [pc, #-4]
 *     .word 0x7f7f0000
 *
 * Nem libRMS, libmain, libunity, libsead nem o proprio libil2cpp mapeiam esse
 * endereco. A sequencia Android completa (incluindo C0009/m5, JNI_OnLoad da
 * RMS e NativeLoader.load) tambem o deixa ausente, logo o primeiro Log feito
 * por InputObserver.OnApplicationPause salta para memoria inexistente.
 *
 * Os metodos Debug vizinhos gerados pela mesma versao do IL2CPP comprovam as
 * duas instrucoes originais: o guard de inicializacao estatica ldrb/cmp. A
 * restauracao abaixo e deliberadamente estreita: somente aceita os dois words
 * exatos do hook conhecido. Assim preservamos todo o corpo de Debug.Log e sua
 * inicializacao normal, sem pular o metodo nem inventar um retorno.
 */
static int restore_il2cpp_debug_log_guard(void) {
    enum { DEBUG_LOG_GUARD_OFFSET = 0x1952d90 };
    static const uint32_t hook[2] = { 0xe51ff004u, 0x7f7f0000u };
    static const uint32_t original[2] = { 0xe5d40000u, 0xe3500000u };

    if (!g_il2cpp_base || !g_m_il2cpp) return -1;
    uint32_t *site = (uint32_t *)(g_il2cpp_base + DEBUG_LOG_GUARD_OFFSET);
    if (site[0] == original[0] && site[1] == original[1]) return 0;
    if (site[0] != hook[0] || site[1] != hook[1]) {
        fprintf(stderr,
                "[ff5] libil2cpp+0x%x inesperado: %08x %08x; reparo recusado\n",
                DEBUG_LOG_GUARD_OFFSET, site[0], site[1]);
        return -1;
    }

    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) page = 4096;
    uintptr_t page_base = (uintptr_t)site & ~((uintptr_t)page - 1);
    if (mprotect((void *)page_base, (size_t)page,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        perror("[ff5] mprotect Debug.Log");
        return -1;
    }
    site[0] = original[0];
    site[1] = original[1];
    __builtin___clear_cache((char *)site, (char *)site + sizeof original);
    if (mprotect((void *)page_base, (size_t)page, PROT_READ | PROT_EXEC) != 0) {
        perror("[ff5] mprotect RX Debug.Log");
        return -1;
    }
    so_use(g_m_il2cpp);
    so_flush_caches();
    fprintf(stderr,
            "[ff5] UnityEngine.Debug.Log: guard estatico original restaurado\n");
    return 0;
}

/* No APK real o Unity copia os recursos IL2CPP do ZIP para o diretorio de
 * dados e testa o retorno da copia em libunity+0x246944. No port esses mesmos
 * arquivos ja chegam desempacotados em bin/Data; a copia ainda e executada,
 * mas a origem APK nao existe e o retorno falso abre o AlertDialog
 * "Not enough storage...". Isso parecia um deadlock em WaitForJobGroup porque
 * o dialogo e sincronizado com a UI.
 *
 * Tal como a receita validada no Terraria, aceitamos o retorno da etapa de
 * extracao somente depois de comprovar os dois recursos IL2CPP no layout
 * final. Nao pula a chamada nem a inicializacao do runtime: transforma apenas
 * o branch de erro posterior (beq.n) em nop.
 */
static int apply_unpacked_il2cpp_resources_patch(void) {
    char metadata[768], resources[768];
    snprintf(metadata, sizeof metadata,
             "%s/bin/Data/Managed/Metadata/global-metadata.dat", g_gamedir);
    snprintf(resources, sizeof resources,
             "%s/bin/Data/Managed/Resources/mscorlib.dll-resources.dat", g_gamedir);
    if (access(metadata, R_OK) != 0 || access(resources, R_OK) != 0) {
        fprintf(stderr, "[ff5] recursos IL2CPP desempacotados ausentes; patch de layout recusado\n");
        return -1;
    }

    uintptr_t p = g_unity_base + 0x246944;
    if (*(uint16_t *)p != 0xd04c) {
        fprintf(stderr, "[ff5] libunity+0x246944 inesperado: 0x%04x\n", *(uint16_t *)p);
        return -1;
    }
    long page = sysconf(_SC_PAGESIZE);
    void *base = (void *)(p & ~((uintptr_t)page - 1));
    if (mprotect(base, (size_t)page, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
        return -1;
    *(uint16_t *)p = 0xbf00; /* nop: recursos ja estao no destino final */
    __builtin___clear_cache((char *)base, (char *)base + page);
    mprotect(base, (size_t)page, PROT_READ | PROT_EXEC);
    fprintf(stderr, "[ff5] layout IL2CPP desempacotado validado; extracao APK pode reutilizar bin/Data\n");
    return 0;
}

// ---- JobWorkerCount=0: job-system INLINE ---------------------------------
// Unity 2019 agenda jobs (ex.: UnloadUnusedAssets no 1o nativeRender) pros
// worker threads; no so-loader os workers NAO executam -> a main trava em
// WaitForJobGroup (libunity+0x1884d2, cond_wait, contador de conclusao=0).
// Reportar 1 CPU NAO basta neste build; setando JobsUtility.JobWorkerCount=0
// via il2cpp o job-system roda INLINE na propria thread. (receita Terraria
// TER_JOBWORKERS0). O dominio il2cpp so fica pronto DENTRO do 1o nativeRender,
// entao rodamos numa thread auxiliar que espera o dominio e ataca+invoca.
static void *(*il2_dom_get)(void);
static void *(*il2_dom_asms)(void *, size_t *);
static void *(*il2_asm_img)(void *);
static void *(*il2_cls_from_name)(void *, const char *, const char *);
static void *(*il2_cls_method)(void *, const char *, int);
static void *(*il2_cls_field)(void *, const char *);
static void *(*il2_rt_invoke)(void *, void *, void **, void **);
static void  (*il2_cls_init)(void *);
static void  (*il2_field_static_get)(void *, void *);
static void *(*il2_thread_attach)(void *);
static int jobs0_resolve(void) {
    so_use(g_m_il2cpp);
    il2_dom_get       = (void *)so_find_addr_safe("il2cpp_domain_get");
    il2_dom_asms      = (void *)so_find_addr_safe("il2cpp_domain_get_assemblies");
    il2_asm_img       = (void *)so_find_addr_safe("il2cpp_assembly_get_image");
    il2_cls_from_name = (void *)so_find_addr_safe("il2cpp_class_from_name");
    il2_cls_method    = (void *)so_find_addr_safe("il2cpp_class_get_method_from_name");
    il2_cls_field     = (void *)so_find_addr_safe("il2cpp_class_get_field_from_name");
    il2_rt_invoke     = (void *)so_find_addr_safe("il2cpp_runtime_invoke");
    il2_cls_init      = (void *)so_find_addr_safe("il2cpp_runtime_class_init");
    il2_field_static_get = (void *)so_find_addr_safe("il2cpp_field_static_get_value");
    il2_thread_attach = (void *)so_find_addr_safe("il2cpp_thread_attach");
    return (il2_dom_get && il2_dom_asms && il2_asm_img && il2_cls_from_name &&
            il2_cls_method && il2_rt_invoke) ? 0 : -1;
}
static int jobs0_apply(void) {
    void *domain = il2_dom_get(); if (!domain) return -1;
    size_t na = 0; void **asms = il2_dom_asms(domain, &na); if (!asms || !na) return -1;
    for (size_t i = 0; i < na; i++) {
        void *img = il2_asm_img(asms[i]); if (!img) continue;
        void *cls = il2_cls_from_name(img, "Unity.Jobs.LowLevel.Unsafe", "JobsUtility");
        if (!cls) continue;
        int zero = 0; void *params[1] = { &zero }; int any = 0;
        const char *set[] = { "set_JobWorkerCount", "SetJobQueueMaximumActiveThreadCount",
                              "SetJobQueueMaximumWarpThreadCount" };
        for (unsigned s = 0; s < 3; s++) {
            void *m = il2_cls_method(cls, set[s], 1); if (!m) continue;
            void *exc = NULL; il2_rt_invoke(m, NULL, params, &exc);
            fprintf(stderr, "[jobs0] %s(0) exc=%p\n", set[s], exc); any = 1;
        }
        if (any) return 0;
    }
    return -1;
}
static void *jobs0_thread(void *arg) {
    (void)arg;
    if (jobs0_resolve() != 0) { fprintf(stderr, "[jobs0] il2cpp API ausente\n"); return NULL; }
    // 1) espera o dominio existir (il2_dom_get e' seguro; a init do runtime
    //    acontece dentro do 1o nativeRender).
    void *d = NULL;
    for (int i = 0; i < 6000 && !(d = il2_dom_get()); i++) usleep(2000);
    if (!d) { fprintf(stderr, "[jobs0] dominio nao apareceu\n"); return NULL; }
    // 2) settle: NAO tocar em il2cpp enquanto o runtime constroi metadata
    //    (dom_get_assemblies/class_from_name em image meio-carregada = crash).
    //    Ao fim do delay o main ja travou no WaitForJobGroup -> metadata estavel.
    const char *dl = getenv("FF5_JOBS0_DELAY");
    int ms = (dl && *dl) ? atoi(dl) : 6000;
    fprintf(stderr, "[jobs0] dominio pronto; settle %dms antes de aplicar\n", ms);
    usleep((useconds_t)ms * 1000);
    if (il2_thread_attach) il2_thread_attach(d);
    if (jobs0_apply() == 0) fprintf(stderr, "[jobs0] JobWorkerCount=0 aplicado\n");
    else fprintf(stderr, "[jobs0] JobsUtility nao encontrada\n");
    return NULL;
}

/* Diagnostico de pressao entre cenas. Resources.UnloadUnusedAssets e' a API
 * publica que o proprio Unity usa para soltar assets sem referencia; invocamos
 * pelo metadata IL2CPP na UnityMain, preservando a fila/lifecycle nativos.
 * O comando por arquivo deixa a primeira validacao inteiramente opt-in. */
static void *g_resources_unload_unused;
static void ff5_memory_report(const char *stage) {
    long rss_kb = -1, swap_kb = -1, avail_kb = -1, swapfree_kb = -1;
    FILE *f = fopen("/proc/self/status", "r");
    if (f) {
        char key[64], line[256];
        while (fgets(line, sizeof line, f)) {
            long value;
            if (sscanf(line, "%63s %ld", key, &value) != 2) continue;
            if (!strcmp(key, "VmRSS:")) rss_kb = value;
            else if (!strcmp(key, "VmSwap:")) swap_kb = value;
        }
        fclose(f);
    }
    f = fopen("/proc/meminfo", "r");
    if (f) {
        char key[64], line[256];
        while (fgets(line, sizeof line, f)) {
            long value;
            if (sscanf(line, "%63s %ld", key, &value) != 2) continue;
            if (!strcmp(key, "MemAvailable:")) avail_kb = value;
            else if (!strcmp(key, "SwapFree:")) swapfree_kb = value;
        }
        fclose(f);
    }
    fprintf(stderr,
            "[memory:%s] rss=%.1f MiB swap=%.1f MiB avail=%.1f MiB "
            "swapfree=%.1f MiB gl-live=%.1f MiB/%u\n",
            stage, rss_kb / 1024.0, swap_kb / 1024.0, avail_kb / 1024.0,
            swapfree_kb / 1024.0, g_gl_tex_live_bytes / 1048576.0,
            g_gl_tex_live_count);
}

static int ff5_resolve_unload_unused(void) {
    if (g_resources_unload_unused) return 0;
    if ((!il2_dom_get || !il2_dom_asms || !il2_asm_img ||
         !il2_cls_from_name || !il2_cls_method || !il2_rt_invoke) &&
        jobs0_resolve() != 0)
        return -1;
    void *domain = il2_dom_get();
    if (!domain) return -1;
    size_t count = 0;
    void **assemblies = il2_dom_asms(domain, &count);
    if (!assemblies || !count) return -1;
    for (size_t i = 0; i < count; ++i) {
        void *image = il2_asm_img(assemblies[i]);
        if (!image) continue;
        void *klass = il2_cls_from_name(image, "UnityEngine", "Resources");
        if (!klass) continue;
        g_resources_unload_unused = il2_cls_method(klass,
                                                   "UnloadUnusedAssets", 0);
        if (g_resources_unload_unused) return 0;
    }
    return -1;
}

/* Khepri mantem um cache global Dictionary<string, LoadedAssetBundle>. No
 * Android com bastante RAM isso reduz I/O, mas neste APK o bootstrap chega a
 * abrir centenas de bundles antes do primeiro mapa. Unload(false) solta o
 * container do bundle sem destruir Texture/Sprite/AudioClip ja instanciados;
 * limpar o Dictionary faz um uso futuro recarregar o bundle normalmente.
 *
 * O layout abaixo vem do il2cpp.h gerado para este APK (metadata 24.4, LP32).
 * A operacao permanece opt-in durante a validacao: escrever "report" ou
 * "purge" em FF5_BUNDLECACHE_FILE. Ela roda na UnityMain entre dois frames. */
#define RVA_ASSETBUNDLE_UNLOAD 0x1EC466CUL
typedef struct ff5_managed_object {
    void *klass;
    void *monitor;
} ff5_managed_object;
typedef struct ff5_bundle_dict {
    ff5_managed_object obj;
    void *buckets;
    void *entries;
    int32_t count;
    int32_t version;
    int32_t free_list;
    int32_t free_count;
    void *comparer;
    void *keys;
    void *values;
    void *sync_root;
} ff5_bundle_dict;
typedef struct ff5_managed_array {
    ff5_managed_object obj;
    void *bounds;
    uint32_t max_length;
    unsigned char data[];
} ff5_managed_array;
typedef struct ff5_bundle_entry {
    int32_t hash_code;
    int32_t next;
    void *key;
    void *bundle_name;
    void *hash;
    uint32_t crc;
    void *asset_bundle;
} ff5_bundle_entry;
_Static_assert(sizeof(ff5_bundle_entry) == 28,
               "FF5 Dictionary<string,LoadedAssetBundle>.Entry LP32");

static void *g_bundle_cache_class;
static void *g_bundle_cache_field;
static int ff5_resolve_bundle_cache(void) {
    if (g_bundle_cache_field) return 0;
    if ((!il2_cls_field || !il2_field_static_get) && jobs0_resolve() != 0)
        return -1;
    void *domain = il2_dom_get();
    if (!domain) return -1;
    size_t count = 0;
    void **assemblies = il2_dom_asms(domain, &count);
    if (!assemblies) return -1;
    for (size_t i = 0; i < count; ++i) {
        void *image = il2_asm_img(assemblies[i]);
        if (!image) continue;
        void *klass = il2_cls_from_name(
            image, "Khepri.AssetDelivery.AssetBundles", "AssetBundleCache");
        if (!klass) continue;
        void *field = il2_cls_field(klass, "assetBundles");
        if (!field) continue;
        if (il2_cls_init) il2_cls_init(klass);
        g_bundle_cache_class = klass;
        g_bundle_cache_field = field;
        return 0;
    }
    return -1;
}

static void ff5_string_ascii(void *managed_string, char *out, size_t out_size) {
    if (!out_size) return;
    out[0] = 0;
    if (!managed_string) return;
    int32_t length = *(int32_t *)((unsigned char *)managed_string + 8);
    const uint16_t *chars = (const uint16_t *)((unsigned char *)managed_string + 12);
    if (length < 0 || length > 4096) return;
    size_t n = (size_t)length;
    if (n >= out_size) n = out_size - 1;
    for (size_t i = 0; i < n; ++i)
        out[i] = chars[i] >= 32 && chars[i] < 127 ? (char)chars[i] : '?';
    out[n] = 0;
}

static int ff5_bundle_cache_command(int purge) {
    if (ff5_resolve_bundle_cache() != 0) {
        fprintf(stderr, "[bundle-cache] classe/campo nao resolvidos\n");
        return -1;
    }
    ff5_bundle_dict *dict = NULL;
    il2_field_static_get(g_bundle_cache_field, &dict);
    if (!dict) {
        fprintf(stderr, "[bundle-cache] Dictionary ainda nulo\n");
        return -1;
    }
    ff5_managed_array *array = (ff5_managed_array *)dict->entries;
    uint32_t capacity = array ? array->max_length : 0;
    int32_t slots = dict->count;
    if (slots < 0 || slots > 65536 || (uint32_t)slots > capacity) {
        fprintf(stderr,
                "[bundle-cache] layout recusado count=%d free=%d capacity=%u\n",
                slots, dict->free_count, capacity);
        return -1;
    }

    int live = 0, touch = 0, key = 0, containers = 0;
    ff5_bundle_entry *entries = array ? (ff5_bundle_entry *)array->data : NULL;
    for (int32_t i = 0; i < slots; ++i) {
        ff5_bundle_entry *entry = &entries[i];
        if (entry->hash_code < 0) continue;
        ++live;
        char name[160];
        ff5_string_ascii(entry->bundle_name ? entry->bundle_name : entry->key,
                         name, sizeof name);
        if (!strncmp(name, "touch_", 6)) ++touch;
        else if (!strncmp(name, "key_", 4)) ++key;
        if (entry->asset_bundle) ++containers;
        if (!purge && live <= 48)
            fprintf(stderr,
                    "[bundle-cache] %03d bundle=%p crc=%08x %s\n",
                    live, entry->asset_bundle, entry->crc,
                    name[0] ? name : "(sem nome)");
    }
    fprintf(stderr,
            "[bundle-cache] %s live=%d touch=%d key=%d containers=%d "
            "slots=%d free=%d capacity=%u\n",
            purge ? "purge-before" : "report", live, touch, key, containers,
            slots, dict->free_count, capacity);
    if (!purge) return 0;

    typedef void (*assetbundle_unload_fn)(void *, int, void *);
    assetbundle_unload_fn unload = (assetbundle_unload_fn)
        (ff5_il2cpp_base() + RVA_ASSETBUNDLE_UNLOAD);
    int unloaded = 0;
    for (int32_t i = 0; i < slots; ++i) {
        ff5_bundle_entry *entry = &entries[i];
        if (entry->hash_code < 0 || !entry->asset_bundle) continue;
        unload(entry->asset_bundle, 0, NULL); /* false: preserve loaded objects */
        ++unloaded;
    }

    void *dict_class = dict->obj.klass;
    void *clear = dict_class ? il2_cls_method(dict_class, "Clear", 0) : NULL;
    void *exception = NULL;
    if (clear) il2_rt_invoke(clear, dict, NULL, &exception);
    fprintf(stderr,
            "[bundle-cache] Unload(false)=%d; Dictionary.Clear method=%p exception=%p\n",
            unloaded, clear, exception);
    if (!clear || exception) return -1;

    if (ff5_resolve_unload_unused() == 0) {
        void *operation = il2_rt_invoke(g_resources_unload_unused, NULL, NULL,
                                        &exception);
        fprintf(stderr,
                "[bundle-cache] Resources.UnloadUnusedAssets operation=%p exception=%p\n",
                operation, exception);
    }
    return exception ? -1 : 0;
}

static void ff5_poll_bundle_cache_command(long frame) {
    const char *path = getenv("FF5_BUNDLECACHE_FILE");
    if (!path || !*path || frame % 10 != 0) return;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char command[32] = "report";
    if (fscanf(f, "%31s", command) != 1) strcpy(command, "report");
    fclose(f);
    if (unlink(path) != 0) return;
    const int purge = !strcmp(command, "purge");
    ff5_memory_report(purge ? "bundle-purge-before" : "bundle-report");
    ff5_bundle_cache_command(purge);
    ff5_memory_report(purge ? "bundle-purge-issued" : "bundle-report-done");
}

static void ff5_poll_unload_command(long frame) {
    const char *path = getenv("FF5_UNLOAD_FILE");
    if (!path || !*path || frame % 10 != 0 || access(path, F_OK) != 0) return;
    if (unlink(path) != 0) return;
    ff5_memory_report("unload-before");
    if (ff5_resolve_unload_unused() != 0) {
        fprintf(stderr, "[assets] Resources.UnloadUnusedAssets nao resolvida\n");
        return;
    }
    void *exception = NULL;
    void *operation = il2_rt_invoke(g_resources_unload_unused, NULL, NULL,
                                    &exception);
    fprintf(stderr,
            "[assets] Resources.UnloadUnusedAssets solicitada frame=%ld "
            "operation=%p exception=%p\n",
            frame, operation, exception);
    ff5_memory_report("unload-issued");
}

// ---- lifecycle Java do UnityPlayer 2019 ----------------------------------
// UnityPlayer.<init> chama initJni(Context) na UI thread. Depois a SurfaceView
// posta recreate/surfaceChanged e Activity.onWindowFocusChanged libera
// focus->resume->render na thread Java "UnityMain". Rodar tudo numa thread so
// perde exatamente essa fronteira de TLS/Looper que inicializa o engine.
typedef struct unity_lifecycle {
    uintptr_t render, recreate, surfchg, resume, focus, pause, done;
    void *env, *thiz;
    volatile int stop;
    volatile int finished;
    volatile long frame;
} unity_lifecycle;

static int lifecycle_stopping(unity_lifecycle *lc) {
    return __atomic_load_n(&lc->stop, __ATOMIC_ACQUIRE);
}

static void lifecycle_stop(unity_lifecycle *lc) {
    __atomic_store_n(&lc->stop, 1, __ATOMIC_RELEASE);
}

static void *unity_main_thread(void *opaque) {
    unity_lifecycle *lc = opaque;
    static uintptr_t surface_token = 0x5f;

    pthread_setname_np(pthread_self(), "UnityMain");
    install_crash_handler();
    fprintf(stderr, "[ff5] UnityMain iniciou (separada da UI thread)\n");

    // SurfaceHolder.Callback: surfaceCreated, depois surfaceChanged + evento.
    if (lc->recreate) {
        fprintf(stderr, "[ff5] >> UnityMain: surfaceCreated/recreate\n");
        ((void (*)(void *, void *, int, void *))lc->recreate)(lc->env, lc->thiz, 0, &surface_token);
        if (!getenv("FF5_ONE_SURFACE_EVENT")) {
            fprintf(stderr, "[ff5] >> UnityMain: surfaceChanged/recreate\n");
            ((void (*)(void *, void *, int, void *))lc->recreate)(lc->env, lc->thiz, 0, &surface_token);
        }
    }
    if (lc->surfchg) {
        fprintf(stderr, "[ff5] >> UnityMain: nativeSendSurfaceChangedEvent\n");
        ((void (*)(void *, void *))lc->surfchg)(lc->env, lc->thiz);
    }

    // windowFocusChanged(true) posta foco antes de checkResumePlayer enfileirar
    // nativeResume. So depois o handler habilita os frames nativeRender.
    if (lc->focus) {
        fprintf(stderr, "[ff5] >> UnityMain: nativeFocusChanged(true)\n");
        ((void (*)(void *, void *, int))lc->focus)(lc->env, lc->thiz, 1);
    }
    if (lc->resume) {
        fprintf(stderr, "[ff5] >> UnityMain: nativeResume\n");
        ((void (*)(void *, void *))lc->resume)(lc->env, lc->thiz);
    }

    fprintf(stderr, "[ff5] >> UnityMain: render loop\n");
    unsigned long gc_interval = 0;
    const char *gc_interval_env = getenv("FF5_GC_INTERVAL");
    if (gc_interval_env) gc_interval = strtoul(gc_interval_env, NULL, 10);
    if (gc_interval && gc_interval < 60) gc_interval = 60;
    while (!lifecycle_stopping(lc)) {
        /* Um snapshot por quadro, antes de a Unity consultar Input/Touch. Isso
         * funciona tanto no frontend KeyInput quanto no frontend Touch. */
        np_game_tick();
        int keep_running = ((int (*)(void *, void *))lc->render)(lc->env, lc->thiz);
        if (!keep_running) {
            fprintf(stderr, "[ff5] nativeRender pediu encerramento\n");
            lifecycle_stop(lc);
            break;
        }
        opensles_shim_pump_callbacks();
        // Diagnostico apenas. No fluxo normal eglSwapBuffers da Unity apresenta.
        if (getenv("FF5_FORCE_PRESENT")) egl_shim_force_present("FF5_FORCE_PRESENT");
        lc->frame++;
        ff5_poll_unload_command(lc->frame);
        ff5_poll_bundle_cache_command(lc->frame);
        if ((getenv("FF5_GC_STATS") || gc_interval) &&
            lc->frame % 300 == 0 && ff5_il2cpp_gc_get_used_size &&
            ff5_il2cpp_gc_get_heap_size) {
            size_t used_before = ff5_il2cpp_gc_get_used_size();
            size_t heap_before = ff5_il2cpp_gc_get_heap_size();
            if (gc_interval && lc->frame % (long)gc_interval == 0 &&
                ff5_il2cpp_gc_collect)
                ff5_il2cpp_gc_collect(0);
            size_t used_after = ff5_il2cpp_gc_get_used_size();
            size_t heap_after = ff5_il2cpp_gc_get_heap_size();
            fprintf(stderr,
                    "[gc] frame=%ld used=%.2f->%.2f MiB heap=%.2f->%.2f MiB%s\n",
                    lc->frame, (double)used_before / 1048576.0,
                    (double)used_after / 1048576.0,
                    (double)heap_before / 1048576.0,
                    (double)heap_after / 1048576.0,
                    gc_interval && lc->frame % (long)gc_interval == 0
                        ? " collected" : "");
        }
        if (lc->frame % 300 == 0)
            fprintf(stderr, "[ff5] frame %ld presents=%u ticks=%u\n",
                    lc->frame,
                    __atomic_load_n(&g_gl_presents_trace, __ATOMIC_RELAXED),
                    SDL_GetTicks());
    }

    // UnityPlayer.destroy(): perde foco, pausa na UnityMain e chama nativeDone.
    if (lc->focus)
        ((void (*)(void *, void *, int))lc->focus)(lc->env, lc->thiz, 0);
    if (lc->pause)
        ((int (*)(void *, void *))lc->pause)(lc->env, lc->thiz);
    if (lc->done)
        ((int (*)(void *, void *))lc->done)(lc->env, lc->thiz);

    __atomic_store_n(&lc->finished, 1, __ATOMIC_RELEASE);
    fprintf(stderr, "[ff5] UnityMain encerrou no frame %ld\n", lc->frame);
    return NULL;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    install_crash_handler();

    const char *gd = getenv("FF5_GAMEDIR");
    char resolved_gamedir[PATH_MAX];
    if (realpath(gd ? gd : ".", resolved_gamedir))
        snprintf(g_gamedir, sizeof g_gamedir, "%s", resolved_gamedir);
    else
        snprintf(g_gamedir, sizeof g_gamedir, "%s", gd ? gd : ".");
    fprintf(stderr, "[ff5] gamedir=%s\n", g_gamedir);
    ff5_dual_init(g_gamedir);   /* gemeas de alpha ETC1 (FF5_DUAL_ALPHA=1) */

    // libs do sistema visiveis ao dlsym(RTLD_DEFAULT) do engine.
    dlopen("libz.so", RTLD_GLOBAL | RTLD_NOW);
    dlopen("libGLESv2.so", RTLD_GLOBAL | RTLD_NOW);
    dlopen("libEGL.so", RTLD_GLOBAL | RTLD_NOW);

    // SDL: video+audio+gamecontroller do SISTEMA (nunca forcar driver).
    // ⚠️ NAO deixar o SDL instalar handler de sinal: o SDL (sdl2-compat->SDL3)
    // pega a SIGSEGV real de um ctor e RE-RAISE, mascarando o pc verdadeiro.
    setenv("SDL_NO_SIGNAL_HANDLERS", "1", 1);
    SDL_SetHint("SDL_NO_SIGNAL_HANDLERS", "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "[ff5] SDL_Init: %s\n", SDL_GetError()); return 1;
    }
    install_crash_handler();   // re-instala o NOSSO por cima de qualquer um do SDL

    // shims prontos ANTES de carregar as libs (ctors chamam de volta).
    ff5_imports_init();
    register_overrides();
    bionic_set_redirect(g_gamedir);
    android_shim_set_datadir(g_gamedir);
    jni_init("com.square_enix.android_googleplay.FFPR5", g_gamedir);

    /* 1) A primeira referencia Java a defpackage.C0009 executa o inicializador
     * estatico da classe: System.loadLibrary("RMS"). Isso antecede o primeiro
     * C0009.m4 usado pelo UnityPlayer e, portanto, tambem o carregamento de
     * libmain. O APK protege e prepara parte do runtime neste JNI_OnLoad; deixa-lo
     * de fora abandona o fluxo Android antes mesmo do UnityPlayer.<init>. */
    g_m_rms = load_module("libRMS.so", 2u * 1024 * 1024);
    if (!g_m_rms) return 1;
    so_use(g_m_rms);
    uintptr_t rms_onload = so_find_addr_safe("JNI_OnLoad");
    if (!rms_onload) {
        fprintf(stderr, "[ff5] libRMS sem JNI_OnLoad?!\n");
        return 1;
    }
    int rms_ver = ((int (*)(void *, void *))rms_onload)(jni_get_vm(), NULL);
    fprintf(stderr, "[ff5] JNI_OnLoad(libRMS) = 0x%x\n", rms_ver);
    /* O restante do inicializador estatico de C0009 chama o unico native com
     * assinatura (I)[B para materializar sua tabela de strings. Mesmo sem uma
     * JVM Java executando o loop que consome o byte[], esta chamada faz parte
     * da sequencia anterior ao UnityPlayer e deve ocorrer no mesmo ponto. */
    uintptr_t rms_strings = jni_find_native_signature("(I)[B");
    if (!rms_strings) {
        fprintf(stderr, "[ff5] native (I)[B da libRMS nao registrado\n");
        return 1;
    }
    jarray rms_table = ((jarray (*)(void *, void *, jint))rms_strings)(
        jni_get_env(), NULL, 0);
    jint rms_table_size = jni_get_array_length(rms_table);
    if (!rms_table || rms_table_size <= 0) {
        fprintf(stderr, "[ff5] inicializacao C0009/m5 falhou (array=%p len=%d)\n",
                rms_table, rms_table_size);
        return 1;
    }
    fprintf(stderr, "[ff5] C0009/m5: tabela Java materializada (%d bytes)\n",
            rms_table_size);

    // 2) System.loadLibrary("main") -> libmain.JNI_OnLoad registra NativeLoader.
    // O so-loader faz o mapeamento manual, mas preserva a mesma entrada JNI.
    g_m_main = load_module("libmain.so", 1u * 1024 * 1024);
    if (!g_m_main) return 1;
    so_use(g_m_main);
    uintptr_t main_onload = so_find_addr_safe("JNI_OnLoad");
    if (main_onload) {
        int ver = ((int (*)(void *, void *))main_onload)(jni_get_vm(), NULL);
        fprintf(stderr, "[ff5] JNI_OnLoad(libmain) = 0x%x\n", ver);
    } else {
        fprintf(stderr, "[ff5] libmain sem JNI_OnLoad?!\n");
        return 1;
    }

    /* 3) UnityPlayer.loadNative chama o metodo registrado por libmain. Ele
     * constroi <nativeLibraryDir>/libunity.so, faz dlopen/dlsym JNI_OnLoad e
     * chama o entry point usando a JavaVM. my_dlopen acima apenas materializa
     * o ELF Android quando essa chamada nativa realmente acontece. */
    uintptr_t native_loader_load = jni_find_native("load");
    if (!native_loader_load) {
        fprintf(stderr, "[ff5] NativeLoader.load nao registrado\n");
        return 1;
    }
    int unity_loaded = ((int (*)(void *, void *, jstring))native_loader_load)(
        jni_get_env(), NULL, (jstring)g_gamedir);
    fprintf(stderr, "[ff5] NativeLoader.load(%s) = %d\n", g_gamedir, unity_loaded);
    if (!unity_loaded || !g_m_unity) return 1;
    if (apply_unpacked_il2cpp_resources_patch() != 0) return 1;
    install_job_tracepoints();

    // 4) natives registrados pelo JNI_OnLoad chamado dentro de NativeLoader.
    jni_dump_natives();

    void *env = jni_get_env();
    void *thiz = jni_get_activity();

    // 5) UnityPlayer.<init>: initJni(Context) ainda na UI/main thread e ANTES
    // de CreateGlView/surfaceCreated. A assinatura JNI tem exatamente 3 args.
    uintptr_t initjni = jni_find_native("initJni");
    if (!initjni) {
        fprintf(stderr, "[ff5] initJni nao registrado — abortando\n");
        return 1;
    }
    fprintf(stderr, "[ff5] >> UI: initJni(Context)\n");
    ((void (*)(void *, void *, void *))initjni)(env, thiz, thiz);
    fprintf(stderr, "[ff5] << UI: initJni OK\n");

    // 7) CreateGlView/SurfaceView: SDL cria a janela hospedeira; o contexto
    // bootstrap e liberado, e a Unity cria os contextos dela no recreate.
    fprintf(stderr, "[ff5] >> egl_shim_create_window\n");
    egl_shim_create_window();
    fprintf(stderr, "[ff5] << egl_shim_create_window OK\n");

    // 8) natives que pertencem a thread Java UnityMain.
    uintptr_t render = jni_find_native("nativeRender");
    uintptr_t recreate = jni_find_native("nativeRecreateGfxState");
    uintptr_t surfchg = jni_find_native("nativeSendSurfaceChangedEvent");
    uintptr_t resume = jni_find_native("nativeResume");
    uintptr_t focus = jni_find_native("nativeFocusChanged");
    uintptr_t pause = jni_find_native("nativePause");
    uintptr_t done = jni_find_native("nativeDone");
    uintptr_t inject = jni_find_native("nativeInjectEvent");
    fprintf(stderr, "[ff5] natives: render=%p recreate=%p surfchg=%p resume=%p focus=%p\n",
            (void *)render, (void *)recreate, (void *)surfchg,
            (void *)resume, (void *)focus);
    (void)inject;

    if (!render) { fprintf(stderr, "[ff5] nativeRender nao registrado — abortando\n"); return 1; }

    // --- muro atual: WaitForJobGroup (libunity+0x188480) trava no 1o
    // nativeRender. O job-system roda "inline" (threaded flag=0) mas o inline
    // NAO incrementa o contador de conclusao [r4+0x24] -> cond_wait eterno.
    // (diagnostico via Terraria TER_FORCETHREADED). Duas vias experimentais:
    //  FF5_JOBS0=1     : JobsUtility.JobWorkerCount=0 via il2cpp (thread aux;
    //                    fragil: corrida com a init do runtime).
    //  FF5_SKIPJOBWAIT=1: patch nativo — cbnz do topo do loop (0x1884b6) vira
    //                    branch incondicional p/ o "done" (sai sem esperar).
    if (getenv("FF5_JOBS0")) {
        pthread_t jt;
        if (pthread_create(&jt, NULL, jobs0_thread, NULL) == 0) pthread_detach(jt);
    }
    if (getenv("FF5_SKIPJOBWAIT") && g_unity_base) {
        uintptr_t p = g_unity_base + 0x1884b6;         // cbnz r6, 0x1884e6
        long pg = sysconf(_SC_PAGESIZE);
        void *pb = (void *)(p & ~((uintptr_t)pg - 1));
        if (mprotect(pb, pg * 2, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
            *(uint16_t *)p = 0xE016;                    // b.n 0x1884e6 (Thumb)
            mprotect(pb, pg * 2, PROT_READ | PROT_EXEC);
            __builtin___clear_cache((char *)pb, (char *)pb + pg * 2);
            fprintf(stderr, "[skipjobwait] libunity+0x1884b6 cbnz->b (pula WaitForJobGroup)\n");
        }
    }
    unity_lifecycle lc;
    memset(&lc, 0, sizeof lc);
    lc.render = render; lc.recreate = recreate; lc.surfchg = surfchg;
    lc.resume = resume; lc.focus = focus; lc.pause = pause; lc.done = done;
    lc.env = env; lc.thiz = thiz;

    /* Instale os hooks ANTES de iniciar UnityMain. SystemConfig.Initialize
     * roda nos primeiros nativeRender e decide ali entre os bundles Touch e
     * KeyInput; instalar pelo loop SDL depois de criar a thread deixava uma
     * corrida que podia carregar ~300 MB de assets Touch antes da troca. */
    np_frame();

    pthread_t unity_thread;
    if (pthread_create(&unity_thread, NULL, unity_main_thread, &lc) != 0) {
        fprintf(stderr, "[ff5] falhou criar a thread UnityMain\n");
        return 1;
    }

    fprintf(stderr, "[ff5] input: %d joystick(s), inject=%p\n",
            SDL_NumJoysticks(), (void *)inject);
    char g_tap_file[512];
    g_tap_file[0] = 0;
    if (getenv("FF5_TAP_FILE"))
        snprintf(g_tap_file, sizeof g_tap_file, "%s", getenv("FF5_TAP_FILE"));
    float axes[32];
    memset(axes, 0, sizeof axes);
    int cursor_touch_down = 0;
    unsigned cursor_move_seen = 0;
    int exit_combo_seen = 0;
    while (!lifecycle_stopping(&lc) &&
           !__atomic_load_n(&lc.finished, __ATOMIC_ACQUIRE)) {
        /* Activity.runOnUiThread/Handler pertencem a esta thread, como no
         * UnityPlayer Java. Os callbacks nativos sinalizam as esperas deles. */
        jni_pump_ui_tasks();
        /* Controle NATIVO: instala hooks do InputListener e amostra o pad SDL
         * (mesmo thread do SDL_PollEvent). O jogo le o pad pelo proprio codigo. */
        np_frame();
        SDL_Event e;
        int axes_dirty = 0;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) lifecycle_stop(&lc);
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) lifecycle_stop(&lc);
            if (e.type == SDL_CONTROLLERDEVICEADDED)
                np_controller_added(e.cdevice.which);
            if (e.type == SDL_CONTROLLERDEVICEREMOVED)
                np_controller_removed(e.cdevice.which);
            /* Caminho nativo do Android: cada botao vira um KeyEvent entregue
             * ao nativeInjectEvent, exatamente como o UnityPlayer Java faz. */
            if (inject && (e.type == SDL_CONTROLLERBUTTONDOWN ||
                           e.type == SDL_CONTROLLERBUTTONUP) &&
                np_is_player1(e.cbutton.which)) {
                int kc = pad_button_keycode(e.cbutton.button);
                /* Com cursor ativo, R3 pertence exclusivamente ao clique. */
                if (e.cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSTICK &&
                    np_cursor_state(NULL, NULL, NULL, NULL))
                    kc = 0;
                if (kc) {
                    const int action = (e.type == SDL_CONTROLLERBUTTONDOWN) ? 0 : 1;
                    jobject ev = jni_pad_key_event(action, kc, 0);
                    const int consumed =
                        ((int (*)(void *, void *, void *))inject)(env, thiz, ev);
                    if (getenv("FF5_PAD_TRACE"))
                        fprintf(stderr, "[pad] botao %d -> keycode %d action %d consumido=%d\n",
                                e.cbutton.button, kc, action, consumed);
                }
            }
            if (inject && e.type == SDL_CONTROLLERAXISMOTION &&
                np_is_player1(e.caxis.which)) {
                int ax = pad_axis_index(e.caxis.axis);
                if (np_cursor_state(NULL, NULL, NULL, NULL) &&
                    (e.caxis.axis == SDL_CONTROLLER_AXIS_RIGHTX ||
                     e.caxis.axis == SDL_CONTROLLER_AXIS_RIGHTY))
                    ax = -1;
                if (ax >= 0) {
                    const int trigger = (ax == 17 || ax == 18);
                    axes[ax] = trigger ? (float)e.caxis.value / 32767.0f
                                       : (float)e.caxis.value / 32768.0f;
                    if (axes[ax] < -1.0f) axes[ax] = -1.0f;
                    if (axes[ax] > 1.0f) axes[ax] = 1.0f;
                    axes_dirty = 1;
                }
            }
        }
        if (inject && axes_dirty) {
            jobject ev = jni_pad_motion_event(axes, 32);
            ((void (*)(void *, void *, void *))inject)(env, thiz, ev);
        }
        if (np_exit_combo()) {
            if (!exit_combo_seen) {
                fprintf(stderr, "[ff5] SELECT+START -> saindo\n");
                lifecycle_stop(&lc);
            }
            exit_combo_seen = 1;
        } else {
            exit_combo_seen = 0;
        }

        /* Cursor fallback aprovado nos FFs: analógico direito move a seta e
         * R3 mantém um toque real no ponto. A continua confirmação nativa. */
        if (inject) {
            int cx = 0, cy = 0, pressed = 0;
            unsigned move_sequence = 0;
            if (np_cursor_state(&cx, &cy, &pressed, &move_sequence)) {
                if (pressed && !cursor_touch_down) {
                    cursor_touch_down = 1;
                    cursor_move_seen = move_sequence;
                    ((int (*)(void *, void *, void *))inject)(
                        env, thiz, jni_touch_event(0, (float)cx, (float)cy));
                } else if (pressed && move_sequence != cursor_move_seen) {
                    cursor_move_seen = move_sequence;
                    ((int (*)(void *, void *, void *))inject)(
                        env, thiz, jni_touch_event(2, (float)cx, (float)cy));
                } else if (!pressed && cursor_touch_down) {
                    cursor_touch_down = 0;
                    ((int (*)(void *, void *, void *))inject)(
                        env, thiz, jni_touch_event(1, (float)cx, (float)cy));
                }
            }
        }
        /* Ponte de toque (diagnostico / confirmacao unica do pad): escrever
         * "x y" em $FF5_TAP_FILE dispara um toque nessas coordenadas. E' o
         * mesmo toque que o usuario daria no Android para aceitar o dialogo
         * "Switch to gamepad controls?", que aparece antes de o jogo comecar
         * a ler o controle. Nao e' cursor nem entra no caminho de gameplay. */
        if (inject && g_tap_file[0]) {
            FILE *tf = fopen(g_tap_file, "r");
            if (tf) {
                float tx = 0, ty = 0;
                char verb[32];
                long pos = ftell(tf);
                if (fscanf(tf, "%31s", verb) == 1 && !strcmp(verb, "key")) {
                    int kc = 0;
                    if (fscanf(tf, "%d", &kc) == 1) {
                        fclose(tf);
                        remove(g_tap_file);
                        fprintf(stderr, "[ff5] keycode %d DOWN consumido=%d\n", kc,
                            ((int (*)(void *, void *, void *))inject)(env, thiz,
                                jni_pad_key_event(0, kc, 0)));
                        SDL_Delay(80);
                        ((int (*)(void *, void *, void *))inject)(env, thiz,
                            jni_pad_key_event(1, kc, 0));
                        continue;
                    }
                }
                fseek(tf, pos, SEEK_SET);
                if (fscanf(tf, "%f %f", &tx, &ty) == 2) {
                    fclose(tf);
                    remove(g_tap_file);
                    fprintf(stderr, "[ff5] toque em (%.0f, %.0f)\n", tx, ty);
                    /* O toque precisa DURAR varios quadros: a Unity drena a
                     * fila uma vez por frame e, com DOWN e UP no mesmo frame,
                     * a uGUI ve um unico toque ja em Ended e o PointerDown
                     * nunca acontece — o botao nao registra o clique. */
                    fprintf(stderr, "[ff5] toque DOWN consumido=%d\n",
                        ((int (*)(void *, void *, void *))inject)(env, thiz,
                            jni_touch_event(0, tx, ty)));
                    for (int i = 0; i < 8; i++) {
                        SDL_Delay(40);
                        ((void (*)(void *, void *, void *))inject)(env, thiz,
                            jni_touch_event(2, tx, ty));   /* ACTION_MOVE */
                    }
                    SDL_Delay(40);
                    ((void (*)(void *, void *, void *))inject)(env, thiz,
                        jni_touch_event(1, tx, ty));
                } else {
                    fclose(tf);
                    remove(g_tap_file);
                }
            }
        }
        SDL_Delay(4);
    }

    lifecycle_stop(&lc);
    pthread_join(unity_thread, NULL);
    fprintf(stderr, "[ff5] saindo (frame %ld)\n", lc.frame);
    SDL_Quit();
    return 0;
}
