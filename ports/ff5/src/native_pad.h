#ifndef FF5_NATIVE_PAD_H
#define FF5_NATIVE_PAD_H

#include <SDL2/SDL.h>

/* Instala os hooks IL2CPP e atualiza o estado SDL do pad. */
void np_frame(void);

/* Congela nivel/bordas/toque uma vez por nativeRender, na UnityMain. */
void np_game_tick(void);

/* Hotplug: chamados pelo loop SDL depois de receber o evento. */
void np_controller_added(int device_index);
void np_controller_removed(SDL_JoystickID instance_id);
int np_is_player1(SDL_JoystickID instance_id);

/* SELECT+START e cursor touch fallback (analogico direito + R3). */
int np_exit_combo(void);
int np_cursor_state(int *x, int *y, int *pressed, unsigned *move_sequence);

/* Overlay GLES2. Deve ser chamado na gfx thread, com FBO 0 current, antes do swap. */
void np_cursor_draw(void);

#endif
