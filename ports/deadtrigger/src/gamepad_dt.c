/* SDL controller state -> Dead Trigger's own PlayerControlsGamepad semantics. */
#define _GNU_SOURCE

#include "dt.h"
#include "gamepad_dt.h"

#include <errno.h>
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

enum dt_game_input {
    DT_INPUT_FIRE = 0,
    DT_INPUT_RELOAD = 1,
    DT_INPUT_PAUSE = 2,
    DT_INPUT_PREV_WEAPON = 3,
    DT_INPUT_NEXT_WEAPON = 4,
    DT_INPUT_AIM = 5,
    DT_INPUT_ITEM1 = 6,
    DT_INPUT_ITEM2 = 7,
    DT_INPUT_ITEM3 = 8,
    DT_INPUT_ITEM4 = 9,
    DT_INPUT_ACTION = 10,
    DT_INPUT_MOVE_RIGHT = 11,
    DT_INPUT_MOVE_UP = 12,
    DT_INPUT_VIEW_RIGHT = 13,
    DT_INPUT_VIEW_UP = 14,
    DT_INPUT_COUNT = 15
};

typedef void *(*il2cpp_domain_get_fn)(void);
typedef const void **(*il2cpp_domain_get_assemblies_fn)(void *, size_t *);
typedef void *(*il2cpp_assembly_get_image_fn)(const void *);
typedef void *(*il2cpp_class_from_name_fn)(void *, const char *, const char *);
typedef const void *(*il2cpp_class_get_method_from_name_fn)(
    void *, const char *, int);

static _Atomic uint32_t g_left_x_bits;
static _Atomic uint32_t g_left_y_bits;
static _Atomic uint32_t g_right_x_bits;
static _Atomic uint32_t g_right_y_bits;
static _Atomic uint32_t g_level;
static _Atomic uint32_t g_pending_down;
static _Atomic uint32_t g_pending_up;

/*
 * Edge delivery is latched: PlayerControlsGamepad.Update is skipped by the
 * game while the HUD is hidden, a fade runs or InUseMode is active, so an
 * edge exposed for exactly one frame can be consumed by nobody. A latched
 * bit only clears on the frame after the game actually read it.
 */
static uint32_t g_frame_level;
static uint32_t g_down_latch;
static uint32_t g_up_latch;
static uint32_t g_down_read;
static uint32_t g_up_read;
static uint32_t g_previous_level;
static int g_fire_down_delivered;
static float g_frame_left_x;
static float g_frame_left_y;
static float g_frame_right_x;
static float g_frame_right_y;

/*
 * The game computes view delta as GetGpadAxis * ViewSensitivity in DEGREES
 * PER FRAME, clamped to 360°*dt. ViewSensitivity = 2x the touch-sensitivity
 * option, which is large enough that any axis scaling still hit the 360°/s
 * clamp — that is why scaling the axis alone changed nothing on the TV. The
 * sensitivity getters are hooked instead, so the camera speed is ours:
 * max speed = view_sens degrees per frame (~25 fps on this device).
 */
static float g_look_scale = 1.0f;
static float g_look_expo = 0.55f;
static float g_view_sens_x = 6.0f;
static float g_view_sens_y = 4.0f;
static int g_fire_sticky_up = 1;
static int g_padlog;

static int g_install_state;
static unsigned g_install_attempts;
static atomic_ullong g_gameplay_axis_millis;

static uint64_t monotonic_milliseconds(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
        return 0;
    return (uint64_t)value.tv_sec * 1000u +
           (uint64_t)value.tv_nsec / 1000000u;
}

static void gamepad_env_init(void) {
    static int initialized;
    if (initialized)
        return;
    initialized = 1;
    const char *setting;
    if ((setting = getenv("DT_LOOK_SCALE")))
        g_look_scale = strtof(setting, NULL);
    if ((setting = getenv("DT_LOOK_EXPO")))
        g_look_expo = strtof(setting, NULL);
    if ((setting = getenv("DT_VIEW_SENS_X")))
        g_view_sens_x = strtof(setting, NULL);
    if ((setting = getenv("DT_VIEW_SENS_Y")))
        g_view_sens_y = strtof(setting, NULL);
    if ((setting = getenv("DT_FIRE_STICKY")))
        g_fire_sticky_up = atoi(setting) != 0;
    g_padlog = getenv("DT_PADLOG") != NULL;
    fprintf(stderr,
            "[gamepad] look_scale=%.2f look_expo=%.2f "
            "view_sens=%.1f/%.1f fire_sticky=%d\n",
            g_look_scale, g_look_expo,
            g_view_sens_x, g_view_sens_y, g_fire_sticky_up);
}

static uint32_t float_bits(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static float bits_float(uint32_t bits) {
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static float clamp_axis(float value) {
    const float dead_zone = 0.16f;
    float magnitude = fabsf(value);
    if (magnitude < dead_zone)
        return 0.0f;
    /* Rescale so motion starts from zero at the dead-zone edge instead of
     * jumping straight to 0.16 — the raw cut made fine aim impossible. */
    magnitude = (magnitude - dead_zone) / (1.0f - dead_zone);
    if (magnitude > 1.0f)
        magnitude = 1.0f;
    return value < 0.0f ? -magnitude : magnitude;
}

void dt_gamepad_publish(float left_x, float left_y,
                        float right_x, float right_y,
                        uint32_t semantic_level, int enabled) {
    if (!enabled) {
        left_x = left_y = right_x = right_y = 0.0f;
        semantic_level = 0;
    }
    semantic_level &= (1u << (DT_INPUT_ACTION + 1)) - 1u;

    atomic_store_explicit(&g_left_x_bits, float_bits(clamp_axis(left_x)),
                          memory_order_relaxed);
    atomic_store_explicit(&g_left_y_bits, float_bits(clamp_axis(left_y)),
                          memory_order_relaxed);
    atomic_store_explicit(&g_right_x_bits, float_bits(clamp_axis(right_x)),
                          memory_order_relaxed);
    atomic_store_explicit(&g_right_y_bits, float_bits(clamp_axis(right_y)),
                          memory_order_relaxed);

    uint32_t old = atomic_exchange_explicit(
        &g_level, semantic_level, memory_order_acq_rel);
    uint32_t pressed = semantic_level & ~old;
    uint32_t released = old & ~semantic_level;
    if (pressed)
        atomic_fetch_or_explicit(&g_pending_down, pressed,
                                 memory_order_release);
    if (released)
        atomic_fetch_or_explicit(&g_pending_up, released,
                                 memory_order_release);
}

void dt_gamepad_frame_begin(void) {
    gamepad_env_init();
    uint32_t level =
        atomic_load_explicit(&g_level, memory_order_acquire);
    g_frame_level = level;
    uint32_t new_down =
        atomic_exchange_explicit(&g_pending_down, 0, memory_order_acq_rel) |
        (level & ~g_previous_level);
    uint32_t new_up =
        atomic_exchange_explicit(&g_pending_up, 0, memory_order_acq_rel) |
        (g_previous_level & ~level);
    g_down_latch = (g_down_latch & ~g_down_read) | new_down;
    g_up_latch = (g_up_latch & ~g_up_read) | new_up;
    g_down_read = 0;
    g_up_read = 0;
    if (g_padlog && (new_down || new_up))
        fprintf(stderr, "[pad] down=%03x up=%03x level=%03x\n",
                new_down, new_up, level);
    g_previous_level = level;

    g_frame_left_x = bits_float(atomic_load_explicit(
        &g_left_x_bits, memory_order_relaxed));
    g_frame_left_y = bits_float(atomic_load_explicit(
        &g_left_y_bits, memory_order_relaxed));
    g_frame_right_x = bits_float(atomic_load_explicit(
        &g_right_x_bits, memory_order_relaxed));
    g_frame_right_y = bits_float(atomic_load_explicit(
        &g_right_y_bits, memory_order_relaxed));
}

/*
 * IL2CPP static methods normally receive E_Input in x0. Keeping the x1
 * fallback also covers older generated wrappers that retained a null
 * synthetic "this" argument; no fixed ABI guess is needed at runtime.
 */
static int semantic_argument(uintptr_t arg0, uintptr_t arg1) {
    if (arg1 < DT_INPUT_COUNT && (arg0 == 0 || arg0 >= DT_INPUT_COUNT))
        return (int)arg1;
    return (int)arg0;
}

static uint8_t hooked_button_down(uintptr_t arg0, uintptr_t arg1,
                                  uintptr_t arg2) {
    (void)arg2;
    int input = semantic_argument(arg0, arg1);
    if (input < 0 || input > DT_INPUT_ACTION)
        return 0;
    uint32_t bit = 1u << input;
    if (!(g_down_latch & bit))
        return 0;
    g_down_read |= bit;
    if (input == DT_INPUT_FIRE)
        g_fire_down_delivered = 1;
    return 1;
}

static uint8_t hooked_button_up(uintptr_t arg0, uintptr_t arg1,
                                uintptr_t arg2) {
    (void)arg2;
    int input = semantic_argument(arg0, arg1);
    if (input < 0 || input > DT_INPUT_ACTION)
        return 0;
    uint32_t bit = 1u << input;
    if (g_up_latch & bit) {
        g_up_read |= bit;
        return 1;
    }
    /*
     * Fire is the only input whose ButtonUp stops something. The stop path
     * is skipped by the game while InUseMode is active, losing the release
     * for good and leaving the weapon firing forever. Re-asserting the
     * release whenever the trigger is idle is safe: the FireUpDelegate stop
     * is an idempotent state write.
     */
    return input == DT_INPUT_FIRE && g_fire_sticky_up &&
           g_fire_down_delivered && !(g_frame_level & bit);
}

/*
 * The game's ViewSensitivity was tuned for touch swipes and is far too fast
 * for a stick held at full deflection. Scale it down and blend in a
 * quadratic response so small deflections aim finely.
 */
static float look_axis(float value) {
    float magnitude = fabsf(value);
    float shaped =
        value * ((1.0f - g_look_expo) + g_look_expo * magnitude);
    return shaped * g_look_scale;
}

static float hooked_get_axis(uintptr_t arg0, uintptr_t arg1,
                             uintptr_t arg2) {
    (void)arg2;
    int input = semantic_argument(arg0, arg1);
    if (input >= DT_INPUT_MOVE_RIGHT && input <= DT_INPUT_VIEW_UP)
        atomic_store_explicit(&g_gameplay_axis_millis,
                              monotonic_milliseconds(),
                              memory_order_release);
    switch (input) {
        case DT_INPUT_MOVE_RIGHT:
            return g_frame_left_x;
        case DT_INPUT_MOVE_UP:
            /* SDL has +Y down; Unity's vertical game axis has +Y up. */
            return -g_frame_left_y;
        case DT_INPUT_VIEW_RIGHT:
            return look_axis(g_frame_right_x);
        case DT_INPUT_VIEW_UP:
            /* The game treats positive ViewUp as look-down; SDL's raw +Y
             * down already matches it (confirmed on the TV — negating this
             * axis inverted the camera). */
            return look_axis(g_frame_right_y);
        default:
            return 0.0f;
    }
}

static float hooked_view_sens_x(void) {
    return g_view_sens_x;
}

static float hooked_view_sens_y(void) {
    return g_view_sens_y;
}

/*
 * GuiMogaPopup is the obsolete "MOGA controller connected" accessory popup.
 * In this asset set, Connection_Layout does not contain the widgets expected
 * by Init(), so its exception aborts MFGuiManager.LateUpdate every frame after
 * a mission. NextOS has no MOGA service and uses the semantic SDL bridge
 * below, therefore only the popup's two large entry points are made inert.
 */
static void hooked_noop(void) {
}

/*
 * IL2CPP folds identical method bodies across the whole binary, so tiny
 * getters/updaters share code with unrelated classes — patching one
 * corrupts all of them (SIGILL seen live when 10 GuiMogaPopup methods were
 * patched). Every body address is recorded and duplicates are refused;
 * only large, uniquely-bodied methods get replaced.
 */
static uintptr_t g_patched_bodies[16];
static int g_patched_body_count;

static int body_already_patched(uintptr_t target) {
    for (int index = 0; index < g_patched_body_count; ++index)
        if (g_patched_bodies[index] == target)
            return 1;
    if (g_patched_body_count <
        (int)(sizeof g_patched_bodies / sizeof g_patched_bodies[0]))
        g_patched_bodies[g_patched_body_count++] = target;
    return 0;
}

static void *find_method(dt_module *il2cpp, const char *class_name,
                         const char *method_name, int parameter_count) {
#define API(type, name)                                                        \
    type name = (type)dt_module_symbol(il2cpp, #name)
    API(il2cpp_domain_get_fn, il2cpp_domain_get);
    API(il2cpp_domain_get_assemblies_fn, il2cpp_domain_get_assemblies);
    API(il2cpp_assembly_get_image_fn, il2cpp_assembly_get_image);
    API(il2cpp_class_from_name_fn, il2cpp_class_from_name);
    API(il2cpp_class_get_method_from_name_fn,
        il2cpp_class_get_method_from_name);
#undef API

    if (!il2cpp_domain_get || !il2cpp_domain_get_assemblies ||
        !il2cpp_assembly_get_image || !il2cpp_class_from_name ||
        !il2cpp_class_get_method_from_name)
        return NULL;

    void *domain = il2cpp_domain_get();
    if (!domain)
        return NULL;
    size_t assembly_count = 0;
    const void **assemblies =
        il2cpp_domain_get_assemblies(domain, &assembly_count);
    if (!assemblies)
        return NULL;

    for (size_t index = 0; index < assembly_count; ++index) {
        void *image = il2cpp_assembly_get_image(assemblies[index]);
        void *klass = image
            ? il2cpp_class_from_name(image, "", class_name) : NULL;
        if (!klass)
            continue;
        return (void *)il2cpp_class_get_method_from_name(
            klass, method_name, parameter_count);
    }
    return NULL;
}

static int replace_method_body(dt_module *il2cpp, const void *method,
                               void *replacement, const char *name) {
#if defined(__aarch64__)
    if (!method || !replacement)
        return 0;
    uintptr_t target = *(const uintptr_t *)method;
    uintptr_t base = (uintptr_t)dt_module_base(il2cpp);
    size_t size = dt_module_size(il2cpp);
    if (target < base || target + 16 > base + size) {
        fprintf(stderr,
                "[gamepad] methodPointer invalido para %s: %p\n",
                name, (void *)target);
        return 0;
    }
    if (body_already_patched(target)) {
        fprintf(stderr,
                "[gamepad] corpo de %s compartilhado (folding) — "
                "patch recusado\n", name);
        return 0;
    }

    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0)
        return 0;
    uintptr_t page =
        target & ~((uintptr_t)page_size - 1u);
    uintptr_t end =
        (target + 16u + (uintptr_t)page_size - 1u) &
        ~((uintptr_t)page_size - 1u);
    size_t span = (size_t)(end - page);
    if (mprotect((void *)page, span,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        fprintf(stderr, "[gamepad] mprotect %s: %s\n",
                name, strerror(errno));
        return 0;
    }

    uint32_t *code = (uint32_t *)target;
    uint32_t old0 = code[0], old1 = code[1];
    code[0] = 0x58000050u; /* ldr x16, #8 */
    code[1] = 0xd61f0200u; /* br x16 */
    memcpy(code + 2, &replacement, sizeof replacement);
    __builtin___clear_cache((char *)target, (char *)target + 16);
    if (mprotect((void *)page, span, PROT_READ | PROT_EXEC) != 0)
        fprintf(stderr, "[gamepad] aviso: restaurar RX em %s: %s\n",
                name, strerror(errno));
    fprintf(stderr,
            "[gamepad] %s @ il2cpp+0x%lx (%08x %08x) -> %p\n",
            name, (unsigned long)(target - base), old0, old1, replacement);
    return 1;
#else
    (void)il2cpp;
    (void)method;
    (void)replacement;
    (void)name;
    return 0;
#endif
}

void dt_gamepad_try_install(void) {
    if (g_install_state)
        return;
    dt_module *il2cpp = dt_module_find("libil2cpp.so");
    if (!il2cpp)
        return;

    const void *button_down =
        find_method(il2cpp, "PlayerControlsGamepad", "ButtonDown", 1);
    const void *button_up =
        find_method(il2cpp, "PlayerControlsGamepad", "ButtonUp", 1);
    const void *get_axis =
        find_method(il2cpp, "PlayerControlsGamepad", "GetGpadAxis", 1);
    if (!button_down || !button_up || !get_axis) {
        if (++g_install_attempts == 1 || g_install_attempts % 600 == 0)
            fprintf(stderr,
                    "[gamepad] aguardando metadata PlayerControlsGamepad "
                    "(tentativa %u)\n", g_install_attempts);
        return;
    }

    uintptr_t down_body = *(const uintptr_t *)button_down;
    uintptr_t up_body = *(const uintptr_t *)button_up;
    uintptr_t axis_body = *(const uintptr_t *)get_axis;
    if (down_body == up_body || down_body == axis_body ||
        up_body == axis_body) {
        fprintf(stderr,
                "[gamepad] corpos IL2CPP compartilhados inesperadamente; "
                "instalacao recusada\n");
        g_install_state = -1;
        return;
    }

    if (!replace_method_body(il2cpp, button_down,
                             (void *)&hooked_button_down, "ButtonDown") ||
        !replace_method_body(il2cpp, button_up,
                             (void *)&hooked_button_up, "ButtonUp") ||
        !replace_method_body(il2cpp, get_axis,
                             (void *)&hooked_get_axis, "GetGpadAxis")) {
        fprintf(stderr, "[gamepad] falha ao instalar ponte semantica\n");
        g_install_state = -1;
        return;
    }
    /*
     * Camera speed: overriding the sensitivity getters detaches the pad
     * camera from the game's touch-sensitivity option (see comment at
     * g_view_sens_x). Non-fatal if a build lacks them.
     */
    const void *sens_x =
        find_method(il2cpp, "PlayerControlsGamepad",
                    "get_ViewSensitivityX", 0);
    const void *sens_y =
        find_method(il2cpp, "PlayerControlsGamepad",
                    "get_ViewSensitivityY", 0);
    if (sens_x && sens_y) {
        if (!replace_method_body(il2cpp, sens_x,
                                 (void *)&hooked_view_sens_x,
                                 "get_ViewSensitivityX") ||
            !replace_method_body(il2cpp, sens_y,
                                 (void *)&hooked_view_sens_y,
                                 "get_ViewSensitivityY"))
            fprintf(stderr,
                    "[gamepad] aviso: sensibilidade da camera segue a "
                    "opcao de toque do jogo\n");
    } else {
        fprintf(stderr,
                "[gamepad] aviso: getters de sensibilidade ausentes na "
                "metadata\n");
    }

    static const struct {
        const char *method;
        int parameters;
    } moga_hooks[] = {
        { "Init", 0 },
        { "Show", 2 },
    };
    int moga_neutered = 0;
    for (size_t entry = 0;
         entry < sizeof moga_hooks / sizeof moga_hooks[0]; ++entry) {
        const void *method =
            find_method(il2cpp, "GuiMogaPopup", moga_hooks[entry].method,
                        moga_hooks[entry].parameters);
        if (method &&
            replace_method_body(il2cpp, method, (void *)&hooked_noop,
                                moga_hooks[entry].method))
            ++moga_neutered;
    }
    fprintf(stderr,
            "[gamepad] GuiMogaPopup neutralizado (%d/2 metodos)\n",
            moga_neutered);

    g_install_state = 1;
    fprintf(stderr,
            "[gamepad] ponte SDL -> PlayerControlsGamepad instalada por "
            "metadata; fluxo Update original preservado\n");
}

int dt_gamepad_is_installed(void) {
    return g_install_state == 1;
}

int dt_gamepad_gameplay_active(void) {
    uint64_t last = atomic_load_explicit(&g_gameplay_axis_millis,
                                         memory_order_acquire);
    uint64_t now = monotonic_milliseconds();
    return last && now >= last && now - last <= 300u;
}
