/* ===== FF5 — controle NATIVO pelos dois choke-points de input do jogo =====
 *
 * O gameplay usa `InputListener`/Unity Input classico. O shell (titulo, popups
 * e uGUI) usa `System.Input.InputSystemManager`, wrapper do NOVO Unity Input
 * System. O backend Android dos dois caminhos nao recebe o pad no so-loader,
 * embora o SDL leia o controle corretamente.
 *
 * Substituimos somente os getters desses wrappers e respondemos direto do SDL.
 * O fluxo de telas, estados e callbacks continua sendo o nativo do jogo.
 * `InputSystemManager.Update` continua original; um detour na entrada apenas
 * fixa uma amostra logica por tick para as bordas Down/Up sobreviverem a varias
 * consultas no mesmo quadro.
 *
 * RVAs (dump Il2CppDumper do libil2cpp deste APK, metadata v24.4):
 *   InputListener.GetKey     0xB1EB70   (self, Key, type, ignoreBlocking, mi)
 *   InputListener.GetKeyDown 0xB1EFBC   (self, Key, type, canMouse, ignoreBlk, mi)
 *   InputListener.GetKeyUp   0xB1F41C   (self, Key, type, ignoreBlocking, mi)
 *
 * Diagnostico: FF5_NATLOG=1 loga os (Key,type) que o jogo consulta (histograma
 * revela o que cada tela poll a). Padrao CRAVADO ON; FF5_NO_NATPAD desliga.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <SDL2/SDL.h>
#include <GLES2/gl2.h>

#include "egl_shim.h"
#include "native_pad.h"
#include "pad_ordinal_fix.h"

extern uintptr_t ff5_il2cpp_base(void);

/* ---- RVAs dos corpos a substituir ---- */
#define RVA_GETKEY      0xB1EB70UL
#define RVA_GETKEYDOWN  0xB1EFBCUL
#define RVA_GETKEYUP    0xB1F41CUL
#define RVA_ANYKEY      0xB1F868UL   /* InputListener.AnyKey(type) */
#define RVA_ANYKEYDOWN  0xB1F9DCUL   /* InputListener.AnyKeyDown() — "TAP TO START" */
#define RVA_ANYKEYUP    0xB1FB54UL   /* InputListener.AnyKeyUp() */

/* System.Input.InputSystemManager — caminho real do shell/titulo. */
#define RVA_SYSINPUT_UPDATE   0x81FA6CUL
#define RVA_SYSINPUT_ANYKEY   0x8205B4UL
#define RVA_SYSINPUT_GETKEY   0x820650UL
#define RVA_SYSINPUT_KEYDOWN  0x820800UL
#define RVA_SYSINPUT_KEYUP    0x820960UL

/* UnityEngine.Input legado. O build Touch consulta estes wrappers managed em
 * vez de consumir o MotionEvent Android entregue ao nativeInjectEvent. */
#define RVA_UNITY_GETTOUCH           0x1EBB994UL
#define RVA_UNITY_GETTOUCH_INJECTED  0x1EBBA14UL
#define RVA_UNITY_ANYKEY             0x1EBBB5CUL
#define RVA_UNITY_TOUCHCOUNT         0x1EBBFB0UL
#define RVA_UNITY_TOUCHSUPPORTED     0x1EBBFF8UL

/* Selecao de frontend: informa ao jogo que o P1 SDL e' um gamepad Xbox. */
#define RVA_INPUTUTIL_GAMEPADTYPE    0x81D478UL
#define RVA_INPUTUTIL_CONNECTED      0x822EC8UL

/* Os bundles compartilhados do Pixel Remaster trazem previews 1080p dos seis
 * jogos e o Unity sobe todos eles para a GPU. O pacote do port substitui
 * somente os previews de FF1/2/3/4/6 por pixels transparentes, preservando os
 * assets do FF5. Addressables passa o CRC do bundle original ao loader; para
 * esses dois CRCs conhecidos, zero significa "carregue sem comparar CRC".
 * Todo o restante do catalogo continua verificado normalmente. */
#define RVA_ASSETBUNDLE_OPTIONS_GET_CRC 0x18A00ACUL
#define CRC_COMMON_ASSETS_ORIGINAL      0xB67B560EUL
#define CRC_TOUCH_COMMON_ORIGINAL       0xABEC30C7UL

/* UnityEngine.AssetBundle.LoadFromFile* (UnityEngine.AssetBundleModule).
 * O Addressables deste build le m_Crc diretamente, portanto o getter acima e'
 * mantido como defesa mas o filtro efetivo precisa ficar na fronteira nativa
 * que recebe path/crc/offset. Ha overloads sync/async que resolvem o icall de
 * forma independente, por isso cobrimos todos os que aceitam CRC. */
#define RVA_AB_LOAD_ASYNC_INTERNAL 0x1EC3B0CUL
#define RVA_AB_LOAD_ASYNC_CRC      0x1EC3BD0UL
#define RVA_AB_LOAD_ASYNC_FULL     0x1EC3C30UL
#define RVA_AB_LOAD_SYNC_INTERNAL  0x1EC3C98UL
#define RVA_AB_LOAD_SYNC_CRC       0x1EC3D00UL
#define RVA_AB_LOAD_SYNC_FULL      0x1EC3D60UL

/* Last.Management.SystemConfig.Initialize consulta este metodo antes de
 * escolher/carregar os singletons Touch/KeyInput. No Android o valor salvo
 * nasce como Touch; aceitar o popup somente depois deixa o bundle Touch
 * gigante residente ate o reload e esgota a RAM do aparelho de 1 GB.
 *
 * NextOS e uma plataforma de gamepad: devolver false aqui nao pula cena nem
 * chama entry point fora de ordem. Apenas entrega a mesma configuracao que o
 * callback nativo "Switch to gamepad" persistiria, mas no ponto em que o jogo
 * originalmente a le, antes de decidir quais assets carregar. */
#define RVA_SYSTEMCONFIG_GET_TOUCH_STATE   0xB39194UL
#define RVA_SYSTEMCONFIG_GET_TOUCH_ENABLED 0xB3983CUL
#define RVA_INPUTSETTING_SET_SINGLETONS     0x9287BCUL
#define RVA_INPUTSETTING_SET_TOUCH          0x928858UL

/* O APK contem dois titulos. Este aparelho carrega Last.UI.Touch. O callback
 * abaixo e' exatamente o gerado para UpdateNone: toque -> Select (estado 3). */
#define RVA_TOUCH_TITLE_UPDATE       0xEC0E30UL
#define RVA_TOUCH_TITLE_START        0xEC5F54UL
#define OFF_TITLE_NEXT_STATE         0x10
#define OFF_TITLE_VIEW               0x2C
#define OFF_TITLE_SCREEN_BUTTON      0x34
/* GamepadPopupManager: o popup "Switch to gamepad controls?" e' UI do NOVO Input
 * System (nao passa pelo InputListener), entao nem pad nem toque injetado o
 * aceitam. Detouramos o Update (roda todo frame na thread do Unity) e, quando o
 * popup esta aberto e o nosso A e' pressionado, chamamos o handler do "Yes"
 * (b__38_0) direto — mesma acao do botao. Dai o jogo troca pra gamepad e passa a
 * ler o InputListener (que hookamos). */
#define RVA_POPUP_UPDATE    0xB0E044UL   /* GamepadPopupManager.Update() (self=r0) */
#define RVA_POPUP_CLOSE     0xB11650UL   /* closeOpenedPopupWindow(): fecha/restaura o estado */
#define OFF_POPUP_AUTO_DETECT 0x2C       /* mEnableAutomaticGamepadDetection */
#define OFF_POPUP_OPENED    0x30         /* campo bool isPopupWindowOpened */

/* ---- KeyValue.InputDeviceType ---- */
enum { IDT_KEYBOARD = 0, IDT_GAMEPAD = 1, IDT_MOUSE = 2, IDT_NONE = 3 };

/* ---- UnityEngine.InputSystem.Key (subconjunto que o FF5 usa na UI) ---- */
enum {
  K_None = 0, K_Space = 1, K_Enter = 2, K_Tab = 3,
  K_Escape = 60, K_LeftArrow = 61, K_RightArrow = 62, K_UpArrow = 63,
  K_DownArrow = 64, K_Backspace = 65, K_PageDown = 66, K_PageUp = 67,
};

/* ================= estado logico do pad (layout Xbox via SDL) ================= */
enum { PB_A, PB_B, PB_X, PB_Y, PB_LB, PB_RB, PB_LT, PB_RT,
       PB_BACK, PB_START, PB_L3, PB_R3, PB_DU, PB_DD, PB_DL, PB_DR,
       PB_COUNT };
enum { PA_LX, PA_LY, PA_RX, PA_RY, PA_LT, PA_RT, PA_COUNT };

static volatile unsigned char g_b[PB_COUNT];       /* nivel (segurado) */
static unsigned char g_bprev[PB_COUNT];

static volatile uint32_t g_level_mask;
static volatile uint32_t g_pending_down, g_pending_up;
static volatile uint32_t g_tick_level, g_tick_down, g_tick_up;
static uint32_t g_tick_previous;

#define NP_MAX_CONTROLLERS 8
static SDL_GameController *g_gc[NP_MAX_CONTROLLERS];
static SDL_GameController *g_p1_gc;
static SDL_Joystick *g_p1_js;
static int g_p1_slot_claimed;
static int g_opened_initial;

static int g_cursor_enabled = 1;
static float g_cursor_fx, g_cursor_fy;
static volatile int g_cursor_x, g_cursor_y, g_cursor_pressed;
static volatile unsigned g_cursor_move_sequence, g_cursor_visible_until;
static Uint32 g_cursor_last_ms;

/* Diagnostico remoto opt-in: um arquivo efemero injeta um unico botao no
 * mesmo estado logico do P1. Inativo no run.sh/release normal. */
static char g_pad_command_file[512];
static int g_command_pb = -1;
static Uint32 g_command_until;

/* Layout exato de UnityEngine.Touch neste metadata (0x44 bytes). Coordenadas
 * da Unity usam origem embaixo/esquerda; o cursor SDL usa topo/esquerda. */
typedef struct NPUnityTouch {
  int finger_id;
  float position_x, position_y;
  float raw_position_x, raw_position_y;
  float delta_x, delta_y;
  float delta_time;
  int tap_count;
  int phase;
  int type;
  float pressure, maximum_pressure;
  float radius, radius_variance;
  float altitude_angle, azimuth_angle;
} NPUnityTouch;
_Static_assert(sizeof(NPUnityTouch) == 0x44, "UnityEngine.Touch layout");

enum { TOUCH_BEGAN = 0, TOUCH_MOVED = 1, TOUCH_STATIONARY = 2,
       TOUCH_ENDED = 3, TOUCH_CANCELED = 4 };
static NPUnityTouch g_touch_tick;
static volatile int g_touch_count;
static int g_touch_tracking, g_touch_release_pending;
static int g_touch_prev_x, g_touch_prev_y;
static unsigned g_touch_prev_move_sequence;

static int np_log;

static int np_have_p1_controller(void) {
  return g_p1_gc && SDL_GameControllerGetAttached(g_p1_gc);
}

void np_controller_added(int index) {
  if (index < 0) return;
  pad_ordinal_fix_apply(index, "FF5");
  SDL_JoystickID iid = SDL_JoystickGetDeviceInstanceID(index);

  if (g_p1_js && SDL_JoystickInstanceID(g_p1_js) == iid) return;
  for (int i = 0; i < NP_MAX_CONTROLLERS; i++)
    if (g_gc[i] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_gc[i])) == iid)
      return;

  /* FF5 e' single-player: o primeiro no logico e' P1. Os demais ficam
   * identificados/abertos para hotplug, mas jamais sao mesclados ao input. */
  int is_p1 = !g_p1_slot_claimed;
  if (is_p1) g_p1_slot_claimed = 1;

  if (!SDL_IsGameController(index)) {
    if (!is_p1) return;
    g_p1_js = SDL_JoystickOpen(index);
    if (g_p1_js)
      fprintf(stderr, "[natpad] P1 joystick cru js%d: %s (botoes=%d eixos=%d hats=%d)\n",
              index, SDL_JoystickName(g_p1_js), SDL_JoystickNumButtons(g_p1_js),
              SDL_JoystickNumAxes(g_p1_js), SDL_JoystickNumHats(g_p1_js));
    else
      fprintf(stderr, "[natpad] P1 joystick cru js%d falhou: %s\n", index, SDL_GetError());
    return;
  }

  for (int i = 0; i < NP_MAX_CONTROLLERS; i++) {
    if (g_gc[i]) continue;
    g_gc[i] = SDL_GameControllerOpen(index);
    if (g_gc[i]) {
      if (is_p1) g_p1_gc = g_gc[i];
      fprintf(stderr, "[natpad] %s controller js%d aberto: %s (instance=%d)\n",
              is_p1 ? "P1" : "P2+", index, SDL_GameControllerName(g_gc[i]), (int)iid);
    } else {
      fprintf(stderr, "[natpad] controller js%d falhou: %s\n", index, SDL_GetError());
    }
    return;
  }
  fprintf(stderr, "[natpad] limite de %d controles atingido; js%d ignorado\n",
          NP_MAX_CONTROLLERS, index);
}

void np_controller_removed(SDL_JoystickID instance_id) {
  for (int i = 0; i < NP_MAX_CONTROLLERS; i++) {
    if (!g_gc[i]) continue;
    SDL_Joystick *joy = SDL_GameControllerGetJoystick(g_gc[i]);
    if (SDL_JoystickInstanceID(joy) != instance_id) continue;
    fprintf(stderr, "[natpad] controller removido: %s (instance=%d)\n",
            SDL_GameControllerName(g_gc[i]), (int)instance_id);
    if (g_p1_gc == g_gc[i]) {
      g_p1_gc = NULL;
      g_p1_slot_claimed = 0;
    }
    SDL_GameControllerClose(g_gc[i]);
    g_gc[i] = NULL;
  }
  if (g_p1_js && SDL_JoystickInstanceID(g_p1_js) == instance_id) {
    SDL_JoystickClose(g_p1_js);
    g_p1_js = NULL;
    g_p1_slot_claimed = 0;
  }
}

int np_is_player1(SDL_JoystickID instance_id) {
  if (g_p1_gc &&
      SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_p1_gc)) == instance_id)
    return 1;
  return g_p1_js && SDL_JoystickInstanceID(g_p1_js) == instance_id;
}

static void np_open_initial(void) {
  if (g_opened_initial) return;
  g_opened_initial = 1;
  SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER);
  int n = SDL_NumJoysticks();
  for (int i = 0; i < n; i++) np_controller_added(i);
  if (!np_have_p1_controller() && !g_p1_js)
    fprintf(stderr, "[natpad] P1 indisponivel (NumJoysticks=%d)\n", n);
}

/* Fallback cru conserva o layout que este port já usava antes do ordinal fix. */
static const int JB[PB_COUNT] = {
  0, 1, 2, 3, 4, 5, -1, -1, 6, 7, 8, 9, -1, -1, -1, -1
};

static float cursor_axis(float v) {
  const float dz = 0.22f;
  if (v > -dz && v < dz) return 0.0f;
  if (v > 0.0f) return (v - dz) / (1.0f - dz);
  return (v + dz) / (1.0f - dz);
}

static int command_button(const char *name) {
  if (!strcmp(name, "a")) return PB_A;
  if (!strcmp(name, "b")) return PB_B;
  if (!strcmp(name, "x")) return PB_X;
  if (!strcmp(name, "y")) return PB_Y;
  if (!strcmp(name, "lb")) return PB_LB;
  if (!strcmp(name, "rb")) return PB_RB;
  if (!strcmp(name, "lt")) return PB_LT;
  if (!strcmp(name, "rt")) return PB_RT;
  if (!strcmp(name, "select")) return PB_BACK;
  if (!strcmp(name, "start")) return PB_START;
  if (!strcmp(name, "l3")) return PB_L3;
  if (!strcmp(name, "r3")) return PB_R3;
  if (!strcmp(name, "up")) return PB_DU;
  if (!strcmp(name, "down")) return PB_DD;
  if (!strcmp(name, "left")) return PB_DL;
  if (!strcmp(name, "right")) return PB_DR;
  return -1;
}

static void np_apply_pad_command(unsigned char *buttons) {
  if (!g_pad_command_file[0]) return;
  Uint32 now = SDL_GetTicks();
  FILE *fp = fopen(g_pad_command_file, "r");
  if (fp) {
    char line[128], name[32];
    int duration_ms = 140;
    int value2 = 0;
    line[0] = 0;
    fgets(line, sizeof line, fp);
    int fields = sscanf(line, "%31s %d %d", name, &duration_ms, &value2);
    fclose(fp);
    unlink(g_pad_command_file);
    if (fields == 3 && !strcmp(name, "cursor")) {
      int w = egl_shim_width(), h = egl_shim_height();
      int x = duration_ms, y = value2;
      if (x < 0) x = 0; else if (w > 0 && x >= w) x = w - 1;
      if (y < 0) y = 0; else if (h > 0 && y >= h) y = h - 1;
      g_cursor_fx = (float)x;
      g_cursor_fy = (float)y;
      __atomic_store_n(&g_cursor_x, x, __ATOMIC_RELEASE);
      __atomic_store_n(&g_cursor_y, y, __ATOMIC_RELEASE);
      __atomic_add_fetch(&g_cursor_move_sequence, 1, __ATOMIC_ACQ_REL);
      __atomic_store_n(&g_cursor_visible_until, now + 2500, __ATOMIC_RELEASE);
      fprintf(stderr, "[natpad-test] cursor em %d,%d\n", x, y);
      return;
    }
    int pb = fields >= 1 ? command_button(name) : -1;
    if (duration_ms < 40) duration_ms = 40;
    if (duration_ms > 1000) duration_ms = 1000;
    if (pb >= 0) {
      g_command_pb = pb;
      g_command_until = now + (Uint32)duration_ms;
      fprintf(stderr, "[natpad-test] %s por %d ms\n", name, duration_ms);
    }
  }
  if (g_command_pb >= 0 && (Sint32)(g_command_until - now) > 0)
    buttons[g_command_pb] = 1;
  else
    g_command_pb = -1;
}

static void np_cursor_update(float rx, float ry, int r3) {
  int w = egl_shim_width(), h = egl_shim_height();
  Uint32 now = SDL_GetTicks();
  if (g_cursor_fx == 0.0f && g_cursor_fy == 0.0f) {
    g_cursor_fx = w * 0.5f;
    g_cursor_fy = h * 0.5f;
    __atomic_store_n(&g_cursor_x, (int)g_cursor_fx, __ATOMIC_RELEASE);
    __atomic_store_n(&g_cursor_y, (int)g_cursor_fy, __ATOMIC_RELEASE);
  }
  float dt = g_cursor_last_ms ? (now - g_cursor_last_ms) / 1000.0f : 0.0f;
  g_cursor_last_ms = now;
  if (dt > 0.05f) dt = 0.05f;

  float dx = cursor_axis(rx), dy = cursor_axis(ry);
  if (g_cursor_enabled && (dx != 0.0f || dy != 0.0f)) {
    const float speed = 900.0f;
    g_cursor_fx += dx * speed * dt;
    g_cursor_fy += dy * speed * dt;
    if (g_cursor_fx < 0.0f) g_cursor_fx = 0.0f;
    if (g_cursor_fy < 0.0f) g_cursor_fy = 0.0f;
    if (g_cursor_fx > w - 1) g_cursor_fx = (float)(w - 1);
    if (g_cursor_fy > h - 1) g_cursor_fy = (float)(h - 1);
    __atomic_store_n(&g_cursor_x, (int)g_cursor_fx, __ATOMIC_RELEASE);
    __atomic_store_n(&g_cursor_y, (int)g_cursor_fy, __ATOMIC_RELEASE);
    __atomic_add_fetch(&g_cursor_move_sequence, 1, __ATOMIC_ACQ_REL);
    __atomic_store_n(&g_cursor_visible_until, now + 2500, __ATOMIC_RELEASE);
  }
  if (g_cursor_enabled && r3)
    __atomic_store_n(&g_cursor_visible_until, now + 2500, __ATOMIC_RELEASE);
  __atomic_store_n(&g_cursor_pressed, g_cursor_enabled && r3, __ATOMIC_RELEASE);
}

static void np_poll(void) {
  np_open_initial();
  unsigned char nb[PB_COUNT]; float na[PA_COUNT];
  memset(nb, 0, sizeof nb); memset(na, 0, sizeof na);
  if (np_have_p1_controller()) {
    SDL_GameControllerUpdate();
    SDL_GameController *gc = g_p1_gc;
#define ORBTN(slot, button) nb[slot] |= SDL_GameControllerGetButton(gc, button)
    ORBTN(PB_A, SDL_CONTROLLER_BUTTON_A);
    ORBTN(PB_B, SDL_CONTROLLER_BUTTON_B);
    ORBTN(PB_X, SDL_CONTROLLER_BUTTON_X);
    ORBTN(PB_Y, SDL_CONTROLLER_BUTTON_Y);
    ORBTN(PB_LB, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    ORBTN(PB_RB, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    ORBTN(PB_BACK, SDL_CONTROLLER_BUTTON_BACK);
    ORBTN(PB_START, SDL_CONTROLLER_BUTTON_START);
    ORBTN(PB_L3, SDL_CONTROLLER_BUTTON_LEFTSTICK);
    ORBTN(PB_R3, SDL_CONTROLLER_BUTTON_RIGHTSTICK);
    ORBTN(PB_DU, SDL_CONTROLLER_BUTTON_DPAD_UP);
    ORBTN(PB_DD, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    ORBTN(PB_DL, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    ORBTN(PB_DR, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
#undef ORBTN
    na[PA_LX]=SDL_GameControllerGetAxis(gc,SDL_CONTROLLER_AXIS_LEFTX)/32767.0f;
    na[PA_LY]=SDL_GameControllerGetAxis(gc,SDL_CONTROLLER_AXIS_LEFTY)/32767.0f;
    na[PA_RX]=SDL_GameControllerGetAxis(gc,SDL_CONTROLLER_AXIS_RIGHTX)/32767.0f;
    na[PA_RY]=SDL_GameControllerGetAxis(gc,SDL_CONTROLLER_AXIS_RIGHTY)/32767.0f;
    short lt=SDL_GameControllerGetAxis(gc,SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    short rt=SDL_GameControllerGetAxis(gc,SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    na[PA_LT]=lt>0?lt/32767.0f:0;
    na[PA_RT]=rt>0?rt/32767.0f:0;
  } else if (g_p1_js) {
    SDL_JoystickUpdate();
    int nbtn = SDL_JoystickNumButtons(g_p1_js);
    for (int i = 0; i < PB_COUNT; i++)
      if (JB[i] >= 0 && JB[i] < nbtn) nb[i] = SDL_JoystickGetButton(g_p1_js, JB[i]);
    if (SDL_JoystickNumAxes(g_p1_js) >= 2) {
      na[PA_LX] = SDL_JoystickGetAxis(g_p1_js, 0) / 32767.0f;
      na[PA_LY] = SDL_JoystickGetAxis(g_p1_js, 1) / 32767.0f;
    }
    if (SDL_JoystickNumAxes(g_p1_js) >= 4) {
      na[PA_RX] = SDL_JoystickGetAxis(g_p1_js, 2) / 32767.0f;
      na[PA_RY] = SDL_JoystickGetAxis(g_p1_js, 3) / 32767.0f;
    }
    if (SDL_JoystickNumHats(g_p1_js) > 0) {
      Uint8 h = SDL_JoystickGetHat(g_p1_js, 0);
      if (h & SDL_HAT_UP) nb[PB_DU] = 1;
      if (h & SDL_HAT_DOWN) nb[PB_DD] = 1;
      if (h & SDL_HAT_LEFT) nb[PB_DL] = 1;
      if (h & SDL_HAT_RIGHT) nb[PB_DR] = 1;
    }
  }
  np_apply_pad_command(nb);
  if (na[PA_LT] > 0.5f) nb[PB_LT] = 1;
  if (na[PA_RT] > 0.5f) nb[PB_RT] = 1;
  /* d-pad tambem via stick esquerdo (deadzone 0.5) p/ navegacao de menu */
  if (na[PA_LY] < -0.5f) nb[PB_DU] = 1;
  if (na[PA_LY] >  0.5f) nb[PB_DD] = 1;
  if (na[PA_LX] < -0.5f) nb[PB_DL] = 1;
  if (na[PA_LX] >  0.5f) nb[PB_DR] = 1;

  uint32_t level = 0, down = 0, up = 0;
  for (int i = 0; i < PB_COUNT; i++) {
    if (nb[i]) level |= 1u << i;
    if (nb[i] && !g_bprev[i]) down |= 1u << i;
    if (!nb[i] && g_bprev[i]) up |= 1u << i;
    g_b[i] = nb[i];
    g_bprev[i] = nb[i];
  }
  __atomic_store_n(&g_level_mask, level, __ATOMIC_RELEASE);
  if (down) __atomic_fetch_or(&g_pending_down, down, __ATOMIC_ACQ_REL);
  if (up) __atomic_fetch_or(&g_pending_up, up, __ATOMIC_ACQ_REL);
  np_cursor_update(na[PA_RX], na[PA_RY], nb[PB_R3]);
}

static void np_begin_game_tick(void) {
  uint32_t level = __atomic_load_n(&g_level_mask, __ATOMIC_ACQUIRE);
  uint32_t down = __atomic_exchange_n(&g_pending_down, 0, __ATOMIC_ACQ_REL);
  uint32_t up = __atomic_exchange_n(&g_pending_up, 0, __ATOMIC_ACQ_REL);
  down |= level & ~g_tick_previous;
  up |= g_tick_previous & ~level;
  g_tick_previous = level;
  __atomic_store_n(&g_tick_level, level, __ATOMIC_RELEASE);
  __atomic_store_n(&g_tick_down, down, __ATOMIC_RELEASE);
  __atomic_store_n(&g_tick_up, up, __ATOMIC_RELEASE);

  int cx = __atomic_load_n(&g_cursor_x, __ATOMIC_ACQUIRE);
  int cy = __atomic_load_n(&g_cursor_y, __ATOMIC_ACQUIRE);
  unsigned move_sequence = __atomic_load_n(&g_cursor_move_sequence, __ATOMIC_ACQUIRE);
  int r3 = (level & (1u << PB_R3)) != 0;
  int r3_down = (down & (1u << PB_R3)) != 0;
  int r3_up = (up & (1u << PB_R3)) != 0;
  int phase = -1;

  if (r3_down || (r3 && !g_touch_tracking)) {
    phase = TOUCH_BEGAN;
    g_touch_tracking = 1;
    if (r3_up && !r3) g_touch_release_pending = 1;
  } else if (g_touch_release_pending && !r3) {
    phase = TOUCH_ENDED;
    g_touch_release_pending = 0;
    g_touch_tracking = 0;
  } else if (r3) {
    phase = move_sequence != g_touch_prev_move_sequence
          ? TOUCH_MOVED : TOUCH_STATIONARY;
    g_touch_tracking = 1;
  } else if (r3_up || g_touch_tracking) {
    phase = TOUCH_ENDED;
    g_touch_tracking = 0;
  }

  if (phase >= 0) {
    int screen_h = egl_shim_height();
    int unity_y = screen_h > 0 ? screen_h - 1 - cy : cy;
    int prev_unity_y = screen_h > 0 ? screen_h - 1 - g_touch_prev_y : g_touch_prev_y;
    memset(&g_touch_tick, 0, sizeof g_touch_tick);
    g_touch_tick.finger_id = 0;
    g_touch_tick.position_x = g_touch_tick.raw_position_x = (float)cx;
    g_touch_tick.position_y = g_touch_tick.raw_position_y = (float)unity_y;
    if (phase == TOUCH_MOVED) {
      g_touch_tick.delta_x = (float)(cx - g_touch_prev_x);
      g_touch_tick.delta_y = (float)(unity_y - prev_unity_y);
    }
    g_touch_tick.delta_time = 1.0f / 60.0f;
    g_touch_tick.tap_count = 1;
    g_touch_tick.phase = phase;
    g_touch_tick.type = 0; /* TouchType.Direct */
    g_touch_tick.pressure = 1.0f;
    g_touch_tick.maximum_pressure = 1.0f;
    __atomic_store_n(&g_touch_count, 1, __ATOMIC_RELEASE);
  } else {
    __atomic_store_n(&g_touch_count, 0, __ATOMIC_RELEASE);
  }
  g_touch_prev_x = cx;
  g_touch_prev_y = cy;
  g_touch_prev_move_sequence = move_sequence;
}

static int mask_has(uint32_t mask, int pb) {
  return pb >= 0 && pb < PB_COUNT && (mask & (1u << pb)) != 0;
}

/* ============ mapa do InputListener.Key -> pad ============
 * O argumento nao e' UnityEngine.KeyCode: e' um objeto System.Input.Key. O
 * unico campo de instancia e' InputValue.Name (Il2CppString* em +0x08), criado
 * do enum GameKey. Comparar esse nome preserva qualquer endereco de heap/ASLR. */
static int il2cpp_key_name_is(const void *key, const char *ascii) {
  if (!key || !ascii) return 0;
  const void *name = *(void * const *)((const char *)key + 0x08);
  if (!name) return 0;
  int length = *(const int *)((const char *)name + 0x08);
  size_t wanted = strlen(ascii);
  if (length < 0 || (size_t)length != wanted) return 0;
  const uint16_t *chars = (const uint16_t *)((const char *)name + 0x0C);
  for (size_t i = 0; i < wanted; i++)
    if (chars[i] != (uint16_t)(unsigned char)ascii[i]) return 0;
  return 1;
}

static int key_pb(const void *key) {
  if (il2cpp_key_name_is(key, "Action"))      return PB_A;
  if (il2cpp_key_name_is(key, "Cancel"))      return PB_B;
  if (il2cpp_key_name_is(key, "Shortcut"))    return PB_X;
  if (il2cpp_key_name_is(key, "Menu"))        return PB_Y;
  if (il2cpp_key_name_is(key, "Up"))          return PB_DU;
  if (il2cpp_key_name_is(key, "Down"))        return PB_DD;
  if (il2cpp_key_name_is(key, "Left"))        return PB_DL;
  if (il2cpp_key_name_is(key, "Right"))       return PB_DR;
  if (il2cpp_key_name_is(key, "SwitchLeft"))  return PB_LB;
  if (il2cpp_key_name_is(key, "SwitchRight")) return PB_RB;
  if (il2cpp_key_name_is(key, "PageUp"))      return PB_LT;
  if (il2cpp_key_name_is(key, "PageDown"))    return PB_RT;
  if (il2cpp_key_name_is(key, "Start"))       return PB_START;
  if (il2cpp_key_name_is(key, "Select"))      return PB_BACK;
  if (il2cpp_key_name_is(key, "StickL"))      return PB_L3;
  if (il2cpp_key_name_is(key, "StickR"))
    return g_cursor_enabled ? -1 : PB_R3;
  return -1;
}

/* ============ mapa EXATO do PlayerInput serializado -> pad ============
 * InputActionType: Enter=0, Cancel=1, Shortcut=2, Menu=3, direcoes=4..7,
 * SwitchLeft/Right=8/9, PageUp/Down=10/11, Start=12, StickL/R=13/14.
 * Bindings reais: south/east/west/north, shoulders, triggers, start e sticks. */
static int action_pb(int action) {
  switch (action) {
    case 0: return PB_A;       /* Enter: buttonSouth */
    case 1: return PB_B;       /* Cancel: buttonEast */
    case 2: return PB_X;       /* Shortcut: buttonWest */
    case 3: return PB_Y;       /* Menu: buttonNorth */
    case 4: return PB_DU;
    case 5: return PB_DD;
    case 6: return PB_DL;
    case 7: return PB_DR;
    case 8: return PB_LB;
    case 9: return PB_RB;
    case 10: return PB_LT;
    case 11: return PB_RT;
    case 12: return PB_START;
    case 13: return PB_L3;
    case 14: return g_cursor_enabled ? -1 : PB_R3;
    default: return -1;
  }
}

static uint32_t native_action_mask(uint32_t mask) {
  /* SELECT é a action Option, que o enum deste manager não publica. R3 pertence
   * ao cursor enquanto o fallback está ligado; não pode também acionar StickR. */
  mask &= ~(1u << PB_BACK);
  if (g_cursor_enabled) mask &= ~(1u << PB_R3);
  return mask;
}

/* histograma de consultas p/ diagnostico */
static unsigned g_hist_key[256], g_hist_type[4];
static unsigned g_hist_action[16];
static void log_query(const char *fn, int key, int type) {
  if (!np_log) return;
  if (key >= 0 && key < 256) g_hist_key[key]++;
  if (type >= 0 && type < 4) g_hist_type[type]++;
  static int fc;
  if ((fc++ % 240) == 0) {
    char line[512]; int p = 0;
    p += snprintf(line+p, sizeof line-p, "[natlog] type kbd=%u pad=%u mouse=%u none=%u | keys:",
                  g_hist_type[0], g_hist_type[1], g_hist_type[2], g_hist_type[3]);
    for (int i = 0; i < 256 && p < (int)sizeof line - 16; i++)
      if (g_hist_key[i]) p += snprintf(line+p, sizeof line-p, " %d:%u", i, g_hist_key[i]);
    fprintf(stderr, "%s\n", line); fsync(2);
  }
}

static void log_action(const char *fn, int action) {
  if (!np_log) return;
  if (action >= 0 && action < 16) g_hist_action[action]++;
  static unsigned n;
  if ((n++ % 240) != 0) return;
  fprintf(stderr, "[natlog-new] %s action=%d |", fn, action);
  for (int i = 0; i < 16; i++)
    if (g_hist_action[i]) fprintf(stderr, " %d:%u", i, g_hist_action[i]);
  fputc('\n', stderr);
  fsync(2);
}

/* ================= corpos substituidos ================= */
static int my_GetKey(void *self, void *key, int type, int ign, void *mi) {
  (void)self; (void)type; (void)ign; (void)mi;
  log_query("GetKey", (int)(uintptr_t)key, type);
  int pb = key_pb(key);
  return mask_has(__atomic_load_n(&g_tick_level, __ATOMIC_ACQUIRE), pb);
}
static int my_GetKeyDown(void *self, void *key, int type, int canMouse, int ign, void *mi) {
  (void)self; (void)type; (void)canMouse; (void)ign; (void)mi;
  log_query("GetKeyDown", (int)(uintptr_t)key, type);
  int pb = key_pb(key);
  uint32_t edge = __atomic_load_n(&g_tick_down, __ATOMIC_ACQUIRE);
  return mask_has(edge, pb);
}
static int my_GetKeyUp(void *self, void *key, int type, int ign, void *mi) {
  (void)self; (void)type; (void)ign; (void)mi;
  log_query("GetKeyUp", (int)(uintptr_t)key, type);
  int pb = key_pb(key);
  uint32_t edge = __atomic_load_n(&g_tick_up, __ATOMIC_ACQUIRE);
  return mask_has(edge, pb);
}
/* "TAP TO START" e telas de "aperte qualquer botao" usam estes. */
static int any_level(void) {
  return native_action_mask(__atomic_load_n(&g_tick_level, __ATOMIC_ACQUIRE)) != 0;
}
static int my_AnyKey(void *self, int type, void *mi) {
  (void)self; (void)type; (void)mi;
  if (np_log) log_query("AnyKey", -1, type);
  return any_level();
}
static int my_AnyKeyDown(void *self, void *mi) {
  (void)self; (void)mi;
  if (np_log) log_query("AnyKeyDown", -2, 0);
  return native_action_mask(__atomic_load_n(&g_tick_down, __ATOMIC_ACQUIRE)) != 0;
}
static int my_AnyKeyUp(void *self, void *mi) {
  (void)self; (void)mi;
  return native_action_mask(__atomic_load_n(&g_tick_up, __ATOMIC_ACQUIRE)) != 0;
}

/* Novo Unity Input System: estes são os getters que o próprio FF5 expõe para
 * titulo, popup, menu, mapa e batalha. Não tocamos em InputAction internamente. */
static int my_SysAnyKey(void *self, void *mi) {
  (void)self; (void)mi;
  log_action("AnyKey", -1);
  return any_level();
}
static int my_SysGetKey(void *self, int action, void *mi) {
  (void)self; (void)mi;
  log_action("GetKey", action);
  return mask_has(__atomic_load_n(&g_tick_level, __ATOMIC_ACQUIRE), action_pb(action));
}
static int my_SysGetKeyDown(void *self, int action, void *mi) {
  (void)self; (void)mi;
  log_action("GetKeyDown", action);
  return mask_has(__atomic_load_n(&g_tick_down, __ATOMIC_ACQUIRE), action_pb(action));
}
static int my_SysGetKeyUp(void *self, int action, void *mi) {
  (void)self; (void)mi;
  log_action("GetKeyUp", action);
  return mask_has(__atomic_load_n(&g_tick_up, __ATOMIC_ACQUIRE), action_pb(action));
}

/* ================= UnityEngine.Input: ponte Touch real ================= */
static int my_UnityAnyKey(void *mi) {
  (void)mi;
  return any_level();
}

static int my_UnityTouchCount(void *mi) {
  (void)mi;
  return __atomic_load_n(&g_touch_count, __ATOMIC_ACQUIRE);
}

static int my_UnityTouchSupported(void *mi) {
  (void)mi;
  return 1;
}

/* GetTouch tem retorno de struct oculto em r0; index chega em r1. */
static void my_UnityGetTouch(void *out_touch, int index, void *mi) {
  (void)mi;
  if (!out_touch) return;
  if (index == 0 && __atomic_load_n(&g_touch_count, __ATOMIC_ACQUIRE))
    memcpy(out_touch, &g_touch_tick, sizeof g_touch_tick);
  else
    memset(out_touch, 0, sizeof g_touch_tick);
}

/* Variante injected: index em r0 e ponteiro de saida em r1. */
static void my_UnityGetTouchInjected(int index, void *out_touch, void *mi) {
  (void)mi;
  if (!out_touch) return;
  if (index == 0 && __atomic_load_n(&g_touch_count, __ATOMIC_ACQUIRE))
    memcpy(out_touch, &g_touch_tick, sizeof g_touch_tick);
  else
    memset(out_touch, 0, sizeof g_touch_tick);
}

static int my_InputUtilConnected(void *mi) {
  (void)mi;
  return np_have_p1_controller() || g_p1_js != NULL;
}

static int my_InputUtilGamePadType(void *mi) {
  (void)mi;
  return (np_have_p1_controller() || g_p1_js != NULL) ? 1 : 0; /* Xbox / Non */
}

static int my_SystemConfigGetTouchState(void *self, void *mi) {
  (void)self; (void)mi;
  return 0; /* KeyInput/gamepad desde a decisao nativa de boot. */
}

static int my_SystemConfigGetTouchEnabled(void *self, void *mi) {
  (void)self; (void)mi;
  return 0;
}

static uint32_t my_AssetBundleRequestOptionsGetCrc(void *self, void *mi) {
  (void)mi;
  if (!self) return 0;
  const uint32_t crc = *(const uint32_t *)((const char *)self + 0x0C);
  if (crc == CRC_COMMON_ASSETS_ORIGINAL ||
      crc == CRC_TOUCH_COMMON_ORIGINAL) {
    static uint32_t logged;
    const uint32_t bit = crc == CRC_COMMON_ASSETS_ORIGINAL ? 1u : 2u;
    if (!(logged & bit)) {
      logged |= bit;
      fprintf(stderr, "[assets] CRC %08x neutralizado para bundle compartilhado otimizado\n",
              crc);
      fsync(2);
    }
    return 0;
  }
  return crc;
}

static int np_managed_string_ends_with_ascii(const void *managed,
                                              const char *suffix) {
  if (!managed || !suffix) return 0;
  const int length = *(const int *)((const char *)managed + 0x08);
  const size_t suffix_len = strlen(suffix);
  if (length < 0 || length > 4096 || (size_t)length < suffix_len) return 0;
  const uint16_t *chars = (const uint16_t *)((const char *)managed + 0x0C);
  const size_t start = (size_t)length - suffix_len;
  for (size_t i = 0; i < suffix_len; ++i)
    if (chars[start + i] != (uint16_t)(unsigned char)suffix[i]) return 0;
  return 1;
}

static int np_optimized_bundle_path(const void *path, unsigned *index) {
  static const char *const names[] = {
    "common_assets_all_360cea9c8d736317e4488e4bffd56a83.bundle",
    "touch_common_assets_all_c6e76e3930c2929e0de3ff8f7d391e82.bundle",
    "effects_textures_assets_all_71942a984f294663665345d34b02140f.bundle",
    "world1_assets_all_1bbde808c7599c02833e2edffbd42e39.bundle",
    "key_icon_gamepadmode_assets_all_141b4e9a8ae3408a640e52dd1786e047.bundle",
    "key_menu_assets_all_eb107c80718c6e8778f174329e355eb2.bundle",
    "chara_field_assets_all_c95cc44373b5b86084a12b7e4ff340c3.bundle",
    "hit_sprite_assets_all_d764f7f6b8deb9c78b71f1afd33374ef.bundle",
  };
  for (unsigned i = 0; i < sizeof names / sizeof names[0]; ++i) {
    if (np_managed_string_ends_with_ascii(path, names[i])) {
      if (index) *index = i;
      return 1;
    }
  }
  return 0;
}

static uint32_t np_optimized_bundle_crc(uint32_t crc, const void *path,
                                         const char *entry) {
  unsigned index = UINT32_MAX;
  const int known_crc = crc == CRC_COMMON_ASSETS_ORIGINAL ||
                        crc == CRC_TOUCH_COMMON_ORIGINAL;
  if (!known_crc && !np_optimized_bundle_path(path, &index)) return crc;
  if (index == UINT32_MAX)
    index = crc == CRC_COMMON_ASSETS_ORIGINAL ? 0u : 1u;
  static uint32_t logged;
  const uint32_t bit = index < 32 ? 1u << index : 0;
  if (!bit || !(logged & bit)) {
    logged |= bit;
    fprintf(stderr, "[assets] %s: CRC %08x -> 0 (bundle otimizado #%u)\n",
            entry, crc, index);
    fsync(2);
  }
  return 0;
}

typedef void *(*np_ab_crc_fn)(void *path, uint32_t crc);
typedef void *(*np_ab_full_fn)(void *path, uint32_t crc, uint64_t offset);
static np_ab_crc_fn g_ab_load_async_crc_orig;
static np_ab_crc_fn g_ab_load_sync_crc_orig;
static np_ab_full_fn g_ab_load_async_internal_orig;
static np_ab_full_fn g_ab_load_async_full_orig;
static np_ab_full_fn g_ab_load_sync_internal_orig;
static np_ab_full_fn g_ab_load_sync_full_orig;

static void *my_ABLoadAsyncInternal(void *path, uint32_t crc, uint64_t offset) {
  return g_ab_load_async_internal_orig(path,
      np_optimized_bundle_crc(crc, path, "LoadFromFileAsync_Internal"), offset);
}
static void *my_ABLoadAsyncCrc(void *path, uint32_t crc) {
  return g_ab_load_async_crc_orig(path,
      np_optimized_bundle_crc(crc, path, "LoadFromFileAsync(crc)"));
}
static void *my_ABLoadAsyncFull(void *path, uint32_t crc, uint64_t offset) {
  return g_ab_load_async_full_orig(path,
      np_optimized_bundle_crc(crc, path, "LoadFromFileAsync(full)"), offset);
}
static void *my_ABLoadSyncInternal(void *path, uint32_t crc, uint64_t offset) {
  return g_ab_load_sync_internal_orig(path,
      np_optimized_bundle_crc(crc, path, "LoadFromFile_Internal"), offset);
}
static void *my_ABLoadSyncCrc(void *path, uint32_t crc) {
  return g_ab_load_sync_crc_orig(path,
      np_optimized_bundle_crc(crc, path, "LoadFromFile(crc)"));
}
static void *my_ABLoadSyncFull(void *path, uint32_t crc, uint64_t offset) {
  return g_ab_load_sync_full_orig(path,
      np_optimized_bundle_crc(crc, path, "LoadFromFile(full)"), offset);
}

/* ================= detour do GamepadPopupManager.Update ================= */
static uintptr_t g_base;
static void (*g_popup_update_orig)(void *self, void *mi);   /* gateway p/ o Update original */
static void (*g_sysinput_update_orig)(void *self, void *mi);
static void (*g_touch_title_update_orig)(void *self, void *mi);
static void (*g_set_input_singletons_orig)(void *touch, void *key, void *mi);
static void (*g_set_touch_mode_orig)(int is_touch, void *mi);
static int g_popup_closed;
static int g_title_a_latched;

/* Toda mudanca de modo continua atravessando InputSettingUtility, incluindo
 * SetTouchGamepadState, selecao dos singletons e limpeza de recursos. Apenas
 * normalizamos o argumento para KeyInput antes de o metodo nativo executa-lo. */
static void my_InputSettingSetTouch(int is_touch, void *mi) {
  static int logged;
  if (!logged || is_touch) {
    fprintf(stderr, "[natpad] InputSettingUtility.SetTouchGamepadEnabled(%d) -> KeyInput\n",
            is_touch);
    fsync(2);
    logged = 1;
  }
  if (g_set_touch_mode_orig) g_set_touch_mode_orig(0, mi);
}

/* O bootstrap entrega aqui, juntos, os singletons Touch e KeyInput. No APK
 * Android o prefab Touch nasce ativo e so e' trocado depois pelo popup, o que
 * recarrega toda a cena. Ativamos KeyInput pelo proprio metodo nativo assim
 * que ambos os objetos existem — antes da primeira cena/titulo e sem reload. */
static void my_InputSettingSetSingletons(void *touch, void *key, void *mi) {
  if (g_set_input_singletons_orig)
    g_set_input_singletons_orig(touch, key, mi);
  fprintf(stderr, "[natpad] bootstrap de input registrado -> ativando KeyInput nativo\n");
  fsync(2);
  ((void (*)(int, void *))(g_base + RVA_INPUTSETTING_SET_TOUCH))(0, NULL);
}

/* O modo deste port e' fixamente KeyInput. O novo Input System do Android nao
 * recebe o pad SDL e, sozinho, conclui que o gamepad foi desconectado: abre um
 * popup oferecendo Touch e recarrega a cena. Desligamos apenas essa deteccao
 * automatica; P1/hotplug continuam no SDL e todo input do jogo segue nativo.
 * Se uma janela ja tiver sido criada no mesmo frame, fechamos pelo metodo do
 * proprio manager, que restaura pause/timeScale sem reload. */
static void my_PopupUpdate(void *self, void *mi) {
  if (self)
    *(volatile unsigned char *)((char *)self + OFF_POPUP_AUTO_DETECT) = 0;
  if (g_popup_update_orig) g_popup_update_orig(self, mi);
  if (self) {
    unsigned char open = *(volatile unsigned char *)((char *)self + OFF_POPUP_OPENED);
    if (open && !g_popup_closed) {
      fprintf(stderr, "[natpad] popup automatico Touch/gamepad fechado (modo KeyInput fixo)\n");
      fsync(2);
      ((void (*)(void *, void *))(g_base + RVA_POPUP_CLOSE))(self, NULL);
      g_popup_closed = 1;
    } else if (!open) {
      g_popup_closed = 0;
    }
  }
}

static void my_SysInputUpdate(void *self, void *mi) {
  if (np_log) {
    static int logged;
    if (!logged) {
      logged = 1;
      fprintf(stderr, "[natpad] InputSystemManager.Update ativo; tick/touch sincronizados\n");
      fsync(2);
    }
  }
  if (g_sysinput_update_orig) g_sysinput_update_orig(self, mi);
}

/* A tela Touch nao consulta AnyKey. A liga o mesmo callback que o screenButton
 * liga no estado None; o proprio callback toca o som e pede o estado Select.
 * Nao escrevemos stateMachine nem pulamos nenhuma etapa do fluxo do titulo. */
static void my_TouchTitleUpdate(void *self, void *mi) {
  if (g_touch_title_update_orig) g_touch_title_update_orig(self, mi);

  int a = g_b[PB_A] != 0;
  if (!a) {
    g_title_a_latched = 0;
    return;
  }
  if (g_title_a_latched || !self) return;
  g_title_a_latched = 1;

  int next_state = *(volatile int *)((char *)self + OFF_TITLE_NEXT_STATE);
  void *view = *(void **)((char *)self + OFF_TITLE_VIEW);
  void *screen_button = *(void **)((char *)self + OFF_TITLE_SCREEN_BUTTON);
  if (next_state != 0 || !view || !screen_button) return;

  fprintf(stderr, "[natpad] titulo Touch + A -> callback nativo TAP TO START\n");
  fsync(2);
  ((void (*)(void *, void *))(g_base + RVA_TOUCH_TITLE_START))(self, NULL);
}
static int np_body_replace(unsigned long rva, void *fn) {
  /* Os corpos do libil2cpp deste APK sao ARM (prologo e92d.. push). Veneer ARM
   * absoluto: LDR PC,[PC,#-4] + endereco. fn tambem e' ARM (loader -marm). */
  long pg = sysconf(_SC_PAGESIZE);
  uint32_t *c = (uint32_t *)(g_base + rva);
  void *pa = (void *)((uintptr_t)c & ~((uintptr_t)pg - 1));
  if (mprotect(pa, pg * 2, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) return 0;
  c[0] = 0xE51FF004;                                   /* LDR PC, [PC, #-4] */
  c[1] = (uint32_t)(uintptr_t)fn;                      /* alvo (ARM, bit0=0) */
  mprotect(pa, pg * 2, PROT_READ | PROT_EXEC);
  __builtin___clear_cache((char *)pa, (char *)pa + 8);
  return 1;
}

/* Detoura um Update ARM: gateway replica push+add-fp (PC-independentes) e
 * salta para Update+8; a entrada recebe o veneer absoluto do hook. */
static int np_install_update_detour(unsigned long rva, void *hook,
                                    void (**original)(void *, void *),
                                    const char *tag) {
  uint32_t *u = (uint32_t *)(g_base + rva);
  /* prologo esperado: push {..,lr}; add fp,sp,#imm  (validar antes de mexer) */
  if ((u[0] & 0x0FFF0000) != 0x092D0000 || (u[1] & 0x0FFFF000) != 0x028DB000) {
    fprintf(stderr, "[natpad] prologo %s inesperado: %08x %08x — detour abortado\n",
            tag, u[0], u[1]); fsync(2);
    return 0;
  }
  void *gw = mmap(NULL, 32, PROT_READ | PROT_WRITE | PROT_EXEC,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (gw == MAP_FAILED) return 0;
  uint32_t *g = (uint32_t *)gw;
  g[0] = u[0];                         /* push {..} original */
  g[1] = u[1];                         /* add fp,sp,#imm original */
  g[2] = 0xE51FF004;                   /* LDR PC,[PC,#-4] */
  g[3] = (uint32_t)(uintptr_t)(g_base + rva + 8);  /* -> Update+8 */
  __builtin___clear_cache((char *)gw, (char *)gw + 16);
  *original = (void (*)(void *, void *))gw;

  long pgsz = sysconf(_SC_PAGESIZE);
  void *pa = (void *)((uintptr_t)u & ~((uintptr_t)pgsz - 1));
  if (mprotect(pa, pgsz * 2, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) return 0;
  u[0] = 0xE51FF004;                   /* LDR PC,[PC,#-4] (substitui push) */
  u[1] = (uint32_t)(uintptr_t)hook;     /* alvo (substitui add fp) */
  mprotect(pa, pgsz * 2, PROT_READ | PROT_EXEC);
  __builtin___clear_cache((char *)pa, (char *)pa + 8);
  return 1;
}

static int g_installed;
void np_frame(void) {
  if (getenv("FF5_NO_NATPAD")) return;
  static int configured;
  if (!configured) {
    const char *cursor = getenv("FF5_CURSOR");
    const char *command_file = getenv("FF5_PAD_FILE");
    g_cursor_enabled = cursor ? atoi(cursor) != 0 : 1;
    if (command_file)
      snprintf(g_pad_command_file, sizeof g_pad_command_file, "%s", command_file);
    configured = 1;
    fprintf(stderr, "[natpad] cursor analogico direito + R3: %s\n",
            g_cursor_enabled ? "ON" : "OFF");
    if (g_pad_command_file[0])
      fprintf(stderr, "[natpad-test] comandos em %s\n", g_pad_command_file);
  }
  g_base = ff5_il2cpp_base();
  if (!g_base) return;
  np_log = getenv("FF5_NATLOG") ? 1 : 0;
  if (!g_installed) {
    if (!np_body_replace(RVA_GETKEY, (void *)my_GetKey)) return;
    np_body_replace(RVA_GETKEYDOWN, (void *)my_GetKeyDown);
    np_body_replace(RVA_GETKEYUP, (void *)my_GetKeyUp);
    np_body_replace(RVA_ANYKEY, (void *)my_AnyKey);
    np_body_replace(RVA_ANYKEYDOWN, (void *)my_AnyKeyDown);
    np_body_replace(RVA_ANYKEYUP, (void *)my_AnyKeyUp);
    np_body_replace(RVA_SYSINPUT_ANYKEY, (void *)my_SysAnyKey);
    np_body_replace(RVA_SYSINPUT_GETKEY, (void *)my_SysGetKey);
    np_body_replace(RVA_SYSINPUT_KEYDOWN, (void *)my_SysGetKeyDown);
    np_body_replace(RVA_SYSINPUT_KEYUP, (void *)my_SysGetKeyUp);
    np_body_replace(RVA_UNITY_GETTOUCH, (void *)my_UnityGetTouch);
    np_body_replace(RVA_UNITY_GETTOUCH_INJECTED, (void *)my_UnityGetTouchInjected);
    np_body_replace(RVA_UNITY_ANYKEY, (void *)my_UnityAnyKey);
    np_body_replace(RVA_UNITY_TOUCHCOUNT, (void *)my_UnityTouchCount);
    np_body_replace(RVA_UNITY_TOUCHSUPPORTED, (void *)my_UnityTouchSupported);
    np_body_replace(RVA_INPUTUTIL_GAMEPADTYPE, (void *)my_InputUtilGamePadType);
    np_body_replace(RVA_INPUTUTIL_CONNECTED, (void *)my_InputUtilConnected);
    np_body_replace(RVA_SYSTEMCONFIG_GET_TOUCH_STATE,
                    (void *)my_SystemConfigGetTouchState);
    np_body_replace(RVA_SYSTEMCONFIG_GET_TOUCH_ENABLED,
                    (void *)my_SystemConfigGetTouchEnabled);
    np_body_replace(RVA_ASSETBUNDLE_OPTIONS_GET_CRC,
                    (void *)my_AssetBundleRequestOptionsGetCrc);
    np_install_update_detour(RVA_AB_LOAD_ASYNC_INTERNAL,
                             (void *)my_ABLoadAsyncInternal,
                             (void (**)(void *, void *))&g_ab_load_async_internal_orig,
                             "AssetBundle.LoadFromFileAsync_Internal");
    np_install_update_detour(RVA_AB_LOAD_ASYNC_CRC,
                             (void *)my_ABLoadAsyncCrc,
                             (void (**)(void *, void *))&g_ab_load_async_crc_orig,
                             "AssetBundle.LoadFromFileAsync(crc)");
    np_install_update_detour(RVA_AB_LOAD_ASYNC_FULL,
                             (void *)my_ABLoadAsyncFull,
                             (void (**)(void *, void *))&g_ab_load_async_full_orig,
                             "AssetBundle.LoadFromFileAsync(full)");
    np_install_update_detour(RVA_AB_LOAD_SYNC_INTERNAL,
                             (void *)my_ABLoadSyncInternal,
                             (void (**)(void *, void *))&g_ab_load_sync_internal_orig,
                             "AssetBundle.LoadFromFile_Internal");
    np_install_update_detour(RVA_AB_LOAD_SYNC_CRC,
                             (void *)my_ABLoadSyncCrc,
                             (void (**)(void *, void *))&g_ab_load_sync_crc_orig,
                             "AssetBundle.LoadFromFile(crc)");
    np_install_update_detour(RVA_AB_LOAD_SYNC_FULL,
                             (void *)my_ABLoadSyncFull,
                             (void (**)(void *, void *))&g_ab_load_sync_full_orig,
                             "AssetBundle.LoadFromFile(full)");
    np_install_update_detour(RVA_SYSINPUT_UPDATE, (void *)my_SysInputUpdate,
                             &g_sysinput_update_orig, "InputSystemManager.Update");
    np_install_update_detour(RVA_INPUTSETTING_SET_TOUCH,
                             (void *)my_InputSettingSetTouch,
                             (void (**)(void *, void *))&g_set_touch_mode_orig,
                             "InputSettingUtility.SetTouchGamepadEnabled");
    np_install_update_detour(RVA_INPUTSETTING_SET_SINGLETONS,
                             (void *)my_InputSettingSetSingletons,
                             (void (**)(void *, void *))&g_set_input_singletons_orig,
                             "InputSettingUtility.SetInputSingletons");
    np_install_update_detour(RVA_POPUP_UPDATE, (void *)my_PopupUpdate,
                             &g_popup_update_orig, "GamepadPopupManager.Update");
    np_install_update_detour(RVA_TOUCH_TITLE_UPDATE, (void *)my_TouchTitleUpdate,
                             &g_touch_title_update_orig, "Touch.TitleWindowController.Update");
    g_installed = 1;
    fprintf(stderr, "[natpad] hooks instalados (KeyInput desde boot + classic/new input + Touch/cursor)\n");
    fsync(2);
  }
  np_poll();
}

void np_game_tick(void) {
  np_begin_game_tick();
}

int np_exit_combo(void) {
  uint32_t level = __atomic_load_n(&g_level_mask, __ATOMIC_ACQUIRE);
  return mask_has(level, PB_BACK) && mask_has(level, PB_START);
}

int np_cursor_state(int *x, int *y, int *pressed, unsigned *move_sequence) {
  if (!g_cursor_enabled) return 0;
  if (x) *x = __atomic_load_n(&g_cursor_x, __ATOMIC_ACQUIRE);
  if (y) *y = __atomic_load_n(&g_cursor_y, __ATOMIC_ACQUIRE);
  if (pressed) *pressed = __atomic_load_n(&g_cursor_pressed, __ATOMIC_ACQUIRE);
  if (move_sequence)
    *move_sequence = __atomic_load_n(&g_cursor_move_sequence, __ATOMIC_ACQUIRE);
  return 1;
}

static void cursor_rect(int x, int y, int w, int h, int screen_w, int screen_h) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > screen_w) w = screen_w - x;
  if (y + h > screen_h) h = screen_h - y;
  if (w <= 0 || h <= 0) return;
  glScissor(x, screen_h - y - h, w, h);
  glClear(GL_COLOR_BUFFER_BIT);
}

void np_cursor_draw(void) {
  if (!g_cursor_enabled) return;
  Uint32 now = SDL_GetTicks();
  Uint32 until = __atomic_load_n(&g_cursor_visible_until, __ATOMIC_ACQUIRE);
  if ((Sint32)(until - now) <= 0) return;

  int sw = egl_shim_width(), sh = egl_shim_height();
  int x = __atomic_load_n(&g_cursor_x, __ATOMIC_ACQUIRE);
  int y = __atomic_load_n(&g_cursor_y, __ATOMIC_ACQUIRE);
  int pressed = __atomic_load_n(&g_cursor_pressed, __ATOMIC_ACQUIRE);
  GLint old_box[4];
  GLfloat old_clear[4];
  GLboolean old_mask[4];
  GLboolean old_scissor = glIsEnabled(GL_SCISSOR_TEST);
  glGetIntegerv(GL_SCISSOR_BOX, old_box);
  glGetFloatv(GL_COLOR_CLEAR_VALUE, old_clear);
  glGetBooleanv(GL_COLOR_WRITEMASK, old_mask);

  glEnable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

  /* Seta preenchida sem shader/VBO: scissor+clear nao perturba o programa nem
   * os atributos da Unity. Contorno grosso e ciano saturado sobrevivem tanto
   * ao fundo branco das transicoes quanto aos mapas escuros. */
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  for (int row = 0; row < 27; row++)
    cursor_rect(x, y + row, 4 + row / 2, 1, sw, sh);
  cursor_rect(x + 9, y + 18, 9, 17, sw, sh);

  if (pressed) glClearColor(1.0f, 0.55f, 0.05f, 1.0f);
  else glClearColor(0.0f, 0.88f, 1.0f, 1.0f);
  for (int row = 2; row < 24; row++) {
    int width = 1 + row / 2;
    cursor_rect(x + 2, y + row, width, 1, sw, sh);
  }
  cursor_rect(x + 11, y + 20, 5, 13, sw, sh);

  glClearColor(old_clear[0], old_clear[1], old_clear[2], old_clear[3]);
  glColorMask(old_mask[0], old_mask[1], old_mask[2], old_mask[3]);
  glScissor(old_box[0], old_box[1], old_box[2], old_box[3]);
  if (!old_scissor) glDisable(GL_SCISSOR_TEST);
}
