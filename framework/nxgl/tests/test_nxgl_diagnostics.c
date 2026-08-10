/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxgl.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition)) {                                                      \
      (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,          \
                    #condition);                                             \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

static nxgl_surface_observation_v2 observation(
    nxgl_surface_event_v2 event) {
  nxgl_surface_observation_v2 value;
  memset(&value, 0, sizeof(value));
  value.api_version = NXGL_API_VERSION_V2;
  value.struct_size = sizeof(value);
  value.event = event;
  value.window_width = 640;
  value.window_height = 480;
  value.drawable_width = 1280;
  value.drawable_height = 720;
  return value;
}

static void test_observations_are_monotonic_and_passive(void) {
  nxgl_surface_state_v2 state;
  nxgl_surface_observation_v2 event;
  nxgl_surface_state_v2 before_recreate;
  nxgl_surface_state_v2_init(&state);
  CHECK(state.api_version == NXGL_API_VERSION_V2);
  CHECK(state.generation == 0u);
  CHECK(state.context_generation == 1u);
  CHECK(state.focused == 1 && state.minimized == 0 && state.context_lost == 0);

  event = observation(NXGL_SURFACE_EVENT_V2_RESIZED);
  CHECK(nxgl_surface_observe_v2(&state, &event) == NXGL_SUCCESS);
  CHECK(state.generation == 1u);
  CHECK(state.window_width == 640 && state.drawable_width == 1280);

  event = observation(NXGL_SURFACE_EVENT_V2_FOCUS_LOST);
  CHECK(nxgl_surface_observe_v2(&state, &event) == NXGL_SUCCESS);
  CHECK(state.generation == 2u && state.focused == 0);

  event = observation(NXGL_SURFACE_EVENT_V2_MINIMIZED);
  CHECK(nxgl_surface_observe_v2(&state, &event) == NXGL_SUCCESS);
  CHECK(state.generation == 3u && state.minimized == 1 && state.focused == 0);

  event = observation(NXGL_SURFACE_EVENT_V2_CONTEXT_LOST);
  CHECK(nxgl_surface_observe_v2(&state, &event) == NXGL_SUCCESS);
  CHECK(state.generation == 4u && state.context_lost == 1);
  CHECK(state.context_generation == 1u);
  memcpy(&before_recreate, &state, sizeof(before_recreate));

  /* Observing loss performs no native recreation.  Only an explicit, later
   * CONTEXT_RECREATED observation advances the context generation. */
  event = observation(NXGL_SURFACE_EVENT_V2_RESTORED);
  CHECK(nxgl_surface_observe_v2(&state, &event) == NXGL_SUCCESS);
  CHECK(state.context_lost == 1 && state.context_generation == 1u);
  CHECK(state.generation == before_recreate.generation + 1u);

  event = observation(NXGL_SURFACE_EVENT_V2_CONTEXT_RECREATED);
  event.drawable_width = 1920;
  event.drawable_height = 1080;
  CHECK(nxgl_surface_observe_v2(&state, &event) == NXGL_SUCCESS);
  CHECK(state.context_lost == 0 && state.context_generation == 2u);
  CHECK(state.generation == 6u);
  CHECK(state.drawable_width == 1920 && state.drawable_height == 1080);
}

static void test_failed_observations_are_atomic(void) {
  nxgl_surface_state_v2 state;
  nxgl_surface_state_v2 before;
  nxgl_surface_observation_v2 event;
  nxgl_surface_state_v2_init(&state);
  memcpy(&before, &state, sizeof(before));
  event = observation(NXGL_SURFACE_EVENT_V2_CONTEXT_RECREATED);
  CHECK(nxgl_surface_observe_v2(&state, &event) ==
        NXGL_ERROR_INVALID_ARGUMENT);
  CHECK(memcmp(&state, &before, sizeof(state)) == 0);

  event = observation(NXGL_SURFACE_EVENT_V2_RESIZED);
  event.drawable_height = 0;
  CHECK(nxgl_surface_observe_v2(&state, &event) ==
        NXGL_ERROR_INVALID_ARGUMENT);
  CHECK(memcmp(&state, &before, sizeof(state)) == 0);

  state.focused = 2;
  memcpy(&before, &state, sizeof(before));
  event = observation(NXGL_SURFACE_EVENT_V2_FOCUS_LOST);
  CHECK(nxgl_surface_observe_v2(&state, &event) ==
        NXGL_ERROR_INVALID_ARGUMENT);
  CHECK(memcmp(&state, &before, sizeof(state)) == 0);

  nxgl_surface_state_v2_init(&state);
  state.context_generation = 3u;
  memcpy(&before, &state, sizeof(before));
  CHECK(nxgl_surface_observe_v2(&state, &event) ==
        NXGL_ERROR_INVALID_ARGUMENT);
  CHECK(memcmp(&state, &before, sizeof(state)) == 0);

  nxgl_surface_state_v2_init(&state);
  state.window_width = 640;
  memcpy(&before, &state, sizeof(before));
  CHECK(nxgl_surface_observe_v2(&state, &event) ==
        NXGL_ERROR_INVALID_ARGUMENT);
  CHECK(memcmp(&state, &before, sizeof(state)) == 0);

  nxgl_surface_state_v2_init(&state);
  state.generation = UINT64_MAX;
  memcpy(&before, &state, sizeof(before));
  event = observation(NXGL_SURFACE_EVENT_V2_FOCUS_LOST);
  CHECK(nxgl_surface_observe_v2(&state, &event) ==
        NXGL_ERROR_INVALID_ARGUMENT);
  CHECK(memcmp(&state, &before, sizeof(state)) == 0);

  nxgl_surface_state_v2_init(&state);
  event = observation(NXGL_SURFACE_EVENT_V2_CONTEXT_LOST);
  CHECK(nxgl_surface_observe_v2(&state, &event) == NXGL_SUCCESS);
  state.context_generation = UINT64_MAX;
  memcpy(&before, &state, sizeof(before));
  event = observation(NXGL_SURFACE_EVENT_V2_CONTEXT_RECREATED);
  CHECK(nxgl_surface_observe_v2(&state, &event) ==
        NXGL_ERROR_INVALID_ARGUMENT);
  CHECK(memcmp(&state, &before, sizeof(state)) == 0);

  nxgl_surface_state_v2_init(&state);
  memcpy(&before, &state, sizeof(before));
  event = observation(NXGL_SURFACE_EVENT_V2_RESIZED);
  event.drawable_width = NXGL_SURFACE_DIMENSION_MAX + 1;
  CHECK(nxgl_surface_observe_v2(&state, &event) ==
        NXGL_ERROR_INVALID_ARGUMENT);
  CHECK(memcmp(&state, &before, sizeof(state)) == 0);
}

static nxgl_silhouette_observation_v2 black_silhouette(void) {
  nxgl_silhouette_observation_v2 value;
  memset(&value, 0, sizeof(value));
  value.api_version = NXGL_API_VERSION_V2;
  value.struct_size = sizeof(value);
  value.pixels_are_black = 1;
  value.silhouette_is_intact = 1;
  return value;
}

static void test_silhouette_prioritizes_sampler_wrap_atlas(void) {
  nxgl_silhouette_observation_v2 value = black_silhouette();
  nxgl_silhouette_diagnosis_v2 diagnosis =
      NXGL_SILHOUETTE_V2_AUDIT_RENDER_PIPELINE;
  /* Even before evidence collection, a perfect black silhouette starts with
   * sampler/wrap/atlas rather than shader or lighting speculation. */
  CHECK(nxgl_classify_black_silhouette_v2(&value, &diagnosis) ==
        NXGL_SUCCESS);
  CHECK(diagnosis == NXGL_SILHOUETTE_V2_AUDIT_SAMPLER_WRAP_ATLAS);

  value.uses_texture_atlas = 1;
  value.repeating_or_mirrored_uv = 1;
  value.forced_clamp_to_edge = 1;
  value.sampler_override_active = 1;
  CHECK(nxgl_classify_black_silhouette_v2(&value, &diagnosis) ==
        NXGL_SUCCESS);
  CHECK(diagnosis == NXGL_SILHOUETTE_V2_AUDIT_SAMPLER_WRAP_ATLAS);

  value.silhouette_is_intact = 0;
  CHECK(nxgl_classify_black_silhouette_v2(&value, &diagnosis) ==
        NXGL_SUCCESS);
  CHECK(diagnosis == NXGL_SILHOUETTE_V2_AUDIT_RENDER_PIPELINE);

  value.pixels_are_black = 0;
  CHECK(nxgl_classify_black_silhouette_v2(&value, &diagnosis) ==
        NXGL_SUCCESS);
  CHECK(diagnosis == NXGL_SILHOUETTE_V2_NOT_APPLICABLE);
}

static void test_silhouette_validation_does_not_overwrite_result(void) {
  nxgl_silhouette_observation_v2 value = black_silhouette();
  nxgl_silhouette_diagnosis_v2 diagnosis =
      NXGL_SILHOUETTE_V2_AUDIT_RENDER_PIPELINE;
  value.forced_clamp_to_edge = 2;
  CHECK(nxgl_classify_black_silhouette_v2(&value, &diagnosis) ==
        NXGL_ERROR_INVALID_ARGUMENT);
  CHECK(diagnosis == NXGL_SILHOUETTE_V2_AUDIT_RENDER_PIPELINE);
}

int main(void) {
  test_observations_are_monotonic_and_passive();
  test_failed_observations_are_atomic();
  test_silhouette_prioritizes_sampler_wrap_atlas();
  test_silhouette_validation_does_not_overwrite_result();
  if (failures) {
    (void)fprintf(stderr, "%d nxgl diagnostic test(s) failed\n", failures);
    return 1;
  }
  (void)fprintf(stdout, "nxgl pure lifecycle/diagnostic tests passed\n");
  return 0;
}
