/* SPDX-License-Identifier: GPL-3.0-only */
#include "nxaudio.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__,     \
              #condition);                                                     \
      return -1;                                                               \
    }                                                                          \
  } while (0)

static nxaudio_stream_options stream_options(void) {
  nxaudio_stream_options options;
  memset(&options, 0, sizeof(options));
  options.api_version = NXAUDIO_API_VERSION;
  options.struct_size = sizeof(options);
  options.format.frequency = 32000u;
  options.format.sample_format = NXAUDIO_SAMPLE_S16LE;
  options.format.channels = 2u;
  options.format.period_frames = 4u;
  options.format.latency_us = 25000u;
  options.capacity_frames = 8u;
  return options;
}

static int test_stream(void) {
  nxaudio_stream_options options = stream_options();
  nxaudio_stream *stream = NULL;
  nxaudio_pull_result pull;
  nxaudio_stream_stats stats;
  int16_t source[16];
  int16_t output[16];
  uint32_t written;
  size_t index;

  for (index = 0u; index < sizeof(source) / sizeof(source[0]); ++index)
    source[index] = (int16_t)(index + 1u);
  CHECK(nxaudio_stream_create(&options, &stream) == NXAUDIO_OK);
  CHECK(stream != NULL);
  CHECK(nxaudio_worker_submit(stream, source, 6u, &written) == NXAUDIO_OK);
  CHECK(written == 6u);
  CHECK(nxaudio_stream_start(stream) == NXAUDIO_OK);

  memset(output, 0, sizeof(output));
  CHECK(nxaudio_realtime_pull(stream, output, 4u, &pull) == NXAUDIO_OK);
  CHECK(pull.frames_copied == 4u && pull.silence_frames == 0u);
  CHECK(memcmp(output, source, 4u * 2u * sizeof(int16_t)) == 0);

  memset(output, 0x55, sizeof(output));
  CHECK(nxaudio_realtime_pull(stream, output, 4u, &pull) == NXAUDIO_OK);
  CHECK(pull.frames_copied == 2u && pull.silence_frames == 2u);
  CHECK(pull.reason == NXAUDIO_REASON_MIXER_STARVED);
  CHECK(output[4] == 0 && output[5] == 0 && output[6] == 0 && output[7] == 0);

  CHECK(nxaudio_stream_pause(stream) == NXAUDIO_OK);
  memset(output, 0x55, sizeof(output));
  CHECK(nxaudio_realtime_pull(stream, output, 4u, &pull) == NXAUDIO_OK);
  CHECK(pull.reason == NXAUDIO_REASON_PAUSED && pull.silence_frames == 4u);
  CHECK(output[0] == 0 && output[7] == 0);
  CHECK(nxaudio_stream_resume(stream) == NXAUDIO_OK);
  CHECK(nxaudio_worker_submit(stream, source, 4u, &written) == NXAUDIO_OK);
  CHECK(written == 4u);

  CHECK(nxaudio_stream_mark_device_lost(stream) == NXAUDIO_OK);
  CHECK(nxaudio_realtime_pull(stream, output, 4u, &pull) == NXAUDIO_OK);
  CHECK(pull.reason == NXAUDIO_REASON_DEVICE_LOST);
  CHECK(nxaudio_stream_recover(stream, 0) == NXAUDIO_OK);
  CHECK(nxaudio_stream_resume(stream) == NXAUDIO_OK);
  CHECK(nxaudio_realtime_pull(stream, output, 4u, &pull) == NXAUDIO_OK);
  CHECK(pull.frames_copied == 4u && pull.silence_frames == 0u);

  CHECK(nxaudio_stream_get_stats(stream, &stats) == NXAUDIO_OK);
  CHECK(stats.state == NXAUDIO_STREAM_RUNNING);
  CHECK(stats.format.frequency == 32000u && stats.format.channels == 2u &&
        stats.format.period_frames == 4u && stats.format.latency_us == 25000u);
  CHECK(stats.underrun_frames == 2u && stats.device_loss_count == 1u);
  nxaudio_stream_close(&stream);
  CHECK(stream == NULL);
  nxaudio_stream_close(&stream);

  options.format.frequency = 0u;
  stream = (nxaudio_stream *)(uintptr_t)1u;
  CHECK(nxaudio_stream_create(&options, &stream) == NXAUDIO_INVALID);
  CHECK(stream == NULL);
  return 0;
}

static nxaudio_backend_observation observation(const char *backend) {
  nxaudio_backend_observation value;
  memset(&value, 0, sizeof(value));
  value.api_version = NXAUDIO_API_VERSION;
  value.struct_size = sizeof(value);
  snprintf(value.backend, sizeof(value.backend), "%s", backend);
  value.server_reachable = 1;
  value.device_opened = 1;
  value.callback_count = 1u;
  value.produced_frames = 256u;
  return value;
}

static int test_backend_reasons(void) {
  nxaudio_backend_observation value = observation("pulse");
  nxaudio_reason reason = NXAUDIO_REASON_NONE;
  value.inherited_attempt = 1;
  CHECK(nxaudio_classify_backend(&value, &reason) == NXAUDIO_OK);
  CHECK(reason == NXAUDIO_REASON_INHERITED_BACKEND);
  value.inherited_attempt = 0;
  CHECK(nxaudio_classify_backend(&value, &reason) == NXAUDIO_OK);
  CHECK(reason == NXAUDIO_REASON_AUTODETECT_BACKEND);

  value = observation("DuMmY");
  CHECK(nxaudio_classify_backend(&value, &reason) == NXAUDIO_UNSUPPORTED);
  CHECK(reason == NXAUDIO_REASON_FAKE_BACKEND);
  value = observation("DISK");
  CHECK(nxaudio_classify_backend(&value, &reason) == NXAUDIO_UNSUPPORTED);
  CHECK(reason == NXAUDIO_REASON_FAKE_BACKEND);

  value = observation("pulse");
  value.server_reachable = 0;
  value.device_opened = 0;
  CHECK(nxaudio_classify_backend(&value, &reason) == NXAUDIO_UNSUPPORTED);
  CHECK(reason == NXAUDIO_REASON_SERVER_UNAVAILABLE);
  value.server_reachable = 1;
  CHECK(nxaudio_classify_backend(&value, &reason) == NXAUDIO_UNSUPPORTED);
  CHECK(reason == NXAUDIO_REASON_DEVICE_OPEN_FAILED);
  value.device_opened = 1;
  value.produced_frames = 0u;
  CHECK(nxaudio_classify_backend(&value, &reason) == NXAUDIO_UNSUPPORTED);
  CHECK(reason == NXAUDIO_REASON_MIXER_STARVED);
  value = observation("");
  CHECK(nxaudio_classify_backend(&value, &reason) == NXAUDIO_INVALID);
  return 0;
}

static int test_asoundrc(void) {
  nxaudio_asoundrc_options options;
  nxaudio_environment_plan plan;
  memset(&options, 0, sizeof(options));
  options.api_version = NXAUDIO_API_VERSION;
  options.struct_size = sizeof(options);
  options.home = "/runtime/profile";
  options.isolated_home = 1;
  options.asoundrc_required = 1;
  options.asoundrc_present = 1;
  CHECK(nxaudio_plan_asoundrc(&options, &plan) == NXAUDIO_OK);
  CHECK(plan.expose == 1 && strcmp(plan.name, "ALSA_CONFIG_PATH") == 0);
  CHECK(strcmp(plan.value, "/runtime/profile/.asoundrc") == 0);
  CHECK(plan.reason == NXAUDIO_REASON_ASOUNDRC_EXPOSED);

  options.asoundrc_present = 0;
  CHECK(nxaudio_plan_asoundrc(&options, &plan) == NXAUDIO_UNSUPPORTED);
  CHECK(plan.reason == NXAUDIO_REASON_ASOUNDRC_REQUIRED);
  options.asoundrc_present = 1;
  options.home = "/runtime/../escape";
  CHECK(nxaudio_plan_asoundrc(&options, &plan) == NXAUDIO_UNSUPPORTED);
  options.isolated_home = 0;
  CHECK(nxaudio_plan_asoundrc(&options, &plan) == NXAUDIO_OK);
  CHECK(plan.expose == 0);
  return 0;
}

static nxaudio_adapter_request adapter(nxaudio_stack stack,
                                       const char *contract) {
  nxaudio_adapter_request request;
  memset(&request, 0, sizeof(request));
  request.api_version = NXAUDIO_API_VERSION;
  request.struct_size = sizeof(request);
  request.stack = stack;
  request.contract_id = contract;
  request.guest_uses_stack = 1;
  return request;
}

static int test_adapter_policy(void) {
  nxaudio_adapter_request request;
  nxaudio_reason reason;
  size_t index;
  CHECK(nxaudio_adapter_contract_count() == 7u);
  for (index = 0u; index < nxaudio_adapter_contract_count(); ++index)
    CHECK(nxaudio_adapter_contract_at(index) != NULL);
  CHECK(nxaudio_adapter_contract_at(7u) == NULL);

  request = adapter(NXAUDIO_STACK_OPENSL_ES, "tasm2-opensl-sdl-v1");
  CHECK(nxaudio_adapter_validate(&request, &reason) == NXAUDIO_OK);
  request = adapter(NXAUDIO_STACK_AAUDIO, "generic-aaudio");
  CHECK(nxaudio_adapter_validate(&request, &reason) == NXAUDIO_UNSUPPORTED);
  CHECK(reason == NXAUDIO_REASON_CONTRACT_UNPROVEN);
  request = adapter(NXAUDIO_STACK_OPENAL, "bully2-openal-v1");
  request.bundles_external_provider = 1;
  CHECK(nxaudio_adapter_validate(&request, &reason) == NXAUDIO_UNSUPPORTED);
  CHECK(reason == NXAUDIO_REASON_PROVIDER_INCOMPATIBLE);
  request = adapter(NXAUDIO_STACK_FMOD, "horizon-fmod-sdl-v1");
  CHECK(nxaudio_adapter_validate(&request, &reason) == NXAUDIO_OK);
  request = adapter(NXAUDIO_STACK_FMOD_EX, "castle-fmodex-v1");
  CHECK(nxaudio_adapter_validate(&request, &reason) == NXAUDIO_OK);
  request = adapter(NXAUDIO_STACK_WWISE, "sor4-wwise-openal-glibc230-v1");
  CHECK(nxaudio_adapter_validate(&request, &reason) == NXAUDIO_UNSUPPORTED);
  request.canonical_recipe = 1;
  CHECK(nxaudio_adapter_validate(&request, &reason) == NXAUDIO_OK);
  return 0;
}

static int test_audibility(void) {
  nxaudio_audibility_evidence evidence;
  nxaudio_reason reason;
  memset(&evidence, 0, sizeof(evidence));
  evidence.api_version = NXAUDIO_API_VERSION;
  evidence.struct_size = sizeof(evidence);
  evidence.device_opened = 1;
  evidence.human_audible_confirmed = 1;
  evidence.produced_frames = 6222u;
  evidence.nonzero_samples = 1000u;
  evidence.peak = 8192u;
  evidence.scope = NXAUDIO_EVIDENCE_SYNTHETIC;
  CHECK(nxaudio_verify_audibility(&evidence, &reason) == NXAUDIO_UNSUPPORTED);
  CHECK(reason == NXAUDIO_REASON_AUDIBILITY_UNPROVEN);
  evidence.scope = NXAUDIO_EVIDENCE_IMPORTED_APPROVED_PHYSICAL;
  CHECK(nxaudio_verify_audibility(&evidence, &reason) == NXAUDIO_OK);
  CHECK(reason == NXAUDIO_REASON_AUDIBLE_CONFIRMED);
  evidence.peak = 0u;
  CHECK(nxaudio_verify_audibility(&evidence, &reason) == NXAUDIO_UNSUPPORTED);
  return 0;
}

int main(void) {
  if (test_stream() != 0 || test_backend_reasons() != 0 ||
      test_asoundrc() != 0 || test_adapter_policy() != 0 ||
      test_audibility() != 0)
    return 1;
  puts("nxaudio M14 host contract tests passed");
  puts("guest_code_executed=0 hardware_ran=0 device_access=0 network_access=0");
  return 0;
}
