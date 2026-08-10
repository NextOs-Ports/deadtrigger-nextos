/*
 * opensl.c -- OpenSL ES to SDL2 audio bridge
 *
 * Translates OpenSL ES BufferQueue audio players into SDL2 audio output.
 * Lock-free SPSC ring buffers between the game thread and the SDL audio thread.
 *
 * imports.c must map the guest's OpenSL DATA imports to the address of our
 * interface-id variables, e.g. {"SL_IID_ENGINE", &sl_IID_ENGINE},
 * {"SL_IID_PLAY", &sl_IID_PLAY}, {"SL_IID_ANDROIDSIMPLEBUFFERQUEUE",
 * &sl_IID_ANDROIDSIMPLEBUFFERQUEUE}, and slCreateEngine -> slCreateEngine_shim.
 */

#include <SDL2/SDL.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "opensl.h"
#include "so_util.h"
#include "util.h"

#define MAX_PLAYERS 16
/* OpenSL entrega PCM em blocos curtos e repoe cada bloco pelo callback. Um
 * ring de 4 MiB por slot deixava ate 64 MiB de paginas antigas residentes
 * depois de musicas/efeitos da abertura. 1 MiB ainda comporta centenas de ms
 * de audio PCM e limita o conjunto inteiro a 16 MiB. */
#define RING_BUFFER_SIZE (1 * 1024 * 1024)
#define RING_BUFFER_MASK (RING_BUFFER_SIZE - 1)
#define SDL_AUDIO_SAMPLES 4096

/* Interface ID storage */
static const int id_engine_tag = 1;
static const int id_play_tag = 2;
static const int id_volume_tag = 3;
static const int id_bufferqueue_tag = 4;
static const int id_effectsend_tag = 5;
static const int id_enginecap_tag = 6;
static const int id_envreverb_tag = 7;
static const int id_androidbq_tag = 8;

const SLInterfaceID sl_IID_ENGINE = &id_engine_tag;
const SLInterfaceID sl_IID_PLAY = &id_play_tag;
const SLInterfaceID sl_IID_VOLUME = &id_volume_tag;
const SLInterfaceID sl_IID_BUFFERQUEUE = &id_bufferqueue_tag;
const SLInterfaceID sl_IID_EFFECTSEND = &id_effectsend_tag;
const SLInterfaceID sl_IID_ENGINECAPABILITIES = &id_enginecap_tag;
const SLInterfaceID sl_IID_ENVIRONMENTALREVERB = &id_envreverb_tag;
const SLInterfaceID sl_IID_ANDROIDSIMPLEBUFFERQUEUE = &id_androidbq_tag;

/* OpenSL ES data structures */
typedef struct {
  SLuint32 locatorType;
  SLuint32 numBuffers;
} SLDataLocator_BufferQueue;

typedef struct {
  SLuint32 formatType;
  SLuint32 numChannels;
  SLuint32 samplesPerSec;
  SLuint32 bitsPerSample;
  SLuint32 containerSize;
  SLuint32 channelMask;
  SLuint32 endianness;
} SLDataFormat_PCM;

typedef struct {
  void *pLocator;
  void *pFormat;
} SLDataSource;

typedef struct {
  void *pLocator;
  void *pFormat;
} SLDataSink;

/* Per-player state */
typedef void (*slBufferQueueCallback)(void *caller, void *pContext);

typedef struct {
  uint8_t ring[RING_BUFFER_SIZE];
  volatile uint32_t ring_head;
  volatile uint32_t ring_tail;

  uint32_t queued_sizes[64];
  volatile uint32_t queued_head_index;
  volatile uint32_t queued_tail_index;
  volatile uint32_t queued_count;
  volatile uint32_t queued_front_offset;
  uint32_t queue_capacity;

  slBufferQueueCallback callback;
  void *callback_context;

  uint32_t enqueued_since_cb;
  uint32_t last_enqueue_size;

  void (*play_callback)(void *caller, void *pContext, SLuint32 event);
  void *play_callback_context;
  SLuint32 play_event_mask;

  uint32_t enqueue_counter;
  int ever_enqueued;
  int headatend_fired;
  int decoder_done;
  volatile uint32_t underrun_count;
  volatile uint32_t fadeout_count;
  volatile uint32_t frames_played;  /* total output frames mixed, for fade-in */

  volatile SLuint32 play_state;
  float volume;
  int active;
  uint64_t played_bytes;

  SLuint32 num_channels;
  SLuint32 sample_rate;
  SLuint32 bits_per_sample;

  void *obj_vtable[8];
  void *obj_ptr;
  void *play_vtable[8];
  void *play_ptr;
  void *volume_vtable[8];
  void *volume_ptr;
  void *bq_vtable[8];
  void *bq_ptr;
  void *effectsend_vtable[8];
  void *effectsend_ptr;
} AudioPlayer;

static AudioPlayer g_players[MAX_PLAYERS];
static pthread_mutex_t g_players_lock = PTHREAD_MUTEX_INITIALIZER;
static SDL_AudioDeviceID g_audio_dev = 0;
static int g_audio_initialized = 0;

static void queue_reset(AudioPlayer *p) {
  memset(p->queued_sizes, 0, sizeof(p->queued_sizes));
  p->queued_head_index = 0;
  p->queued_tail_index = 0;
  p->queued_count = 0;
  p->queued_front_offset = 0;
  p->played_bytes = 0;
}

static void queue_push(AudioPlayer *p, uint32_t size) {
  if (size == 0) return;

  if (p->queued_count >= (sizeof(p->queued_sizes) / sizeof(p->queued_sizes[0]))) {
    uint32_t tail = (p->queued_tail_index - 1) %
                    (sizeof(p->queued_sizes) / sizeof(p->queued_sizes[0]));
    p->queued_sizes[tail] += size;
    return;
  }

  uint32_t tail = p->queued_tail_index %
                  (sizeof(p->queued_sizes) / sizeof(p->queued_sizes[0]));
  p->queued_sizes[tail] = size;
  p->queued_tail_index++;
  p->queued_count++;
}

static void queue_consume(AudioPlayer *p, uint32_t bytes) {
  while (bytes > 0 && p->queued_count > 0) {
    uint32_t head = p->queued_head_index %
                    (sizeof(p->queued_sizes) / sizeof(p->queued_sizes[0]));
    uint32_t queued = p->queued_sizes[head];
    uint32_t remaining = queued - p->queued_front_offset;

    if (bytes < remaining) {
      p->queued_front_offset += bytes;
      return;
    }

    bytes -= remaining;
    p->queued_sizes[head] = 0;
    p->queued_head_index++;
    p->queued_count--;
    p->queued_front_offset = 0;
  }
}

/* Ring buffer helpers */
static inline uint32_t ring_readable(const AudioPlayer *p) {
  return p->ring_head - p->ring_tail;
}

static inline uint32_t ring_writable(const AudioPlayer *p) {
  return RING_BUFFER_SIZE - (p->ring_head - p->ring_tail);
}

static uint32_t ring_write(AudioPlayer *p, const void *data, uint32_t len) {
  uint32_t space = ring_writable(p);
  if (len > space) len = space;
  if (len == 0) return 0;

  const uint8_t *src = (const uint8_t *)data;
  uint32_t head = p->ring_head & RING_BUFFER_MASK;
  uint32_t first = RING_BUFFER_SIZE - head;
  if (first > len) first = len;
  memcpy(p->ring + head, src, first);
  if (len > first) memcpy(p->ring, src + first, len - first);

  __sync_synchronize();
  p->ring_head += len;
  return len;
}

static uint32_t ring_read(AudioPlayer *p, void *data, uint32_t len) {
  uint32_t avail = ring_readable(p);
  if (len > avail) len = avail;
  if (len == 0) return 0;

  uint8_t *dst = (uint8_t *)data;
  uint32_t tail = p->ring_tail & RING_BUFFER_MASK;
  uint32_t first = RING_BUFFER_SIZE - tail;
  if (first > len) first = len;
  memcpy(dst, p->ring + tail, first);
  if (len > first) memcpy(dst + first, p->ring, len - first);

  __sync_synchronize();
  p->ring_tail += len;
  return len;
}

/* SDL2 audio callback */
#define SDL_OUTPUT_RATE 44100
#define TMP_BUF_SAMPLES (SDL_AUDIO_SAMPLES * 2)

static void sdl_audio_callback(void *userdata, Uint8 *stream, int len) {
  (void)userdata;
  memset(stream, 0, len);

  int16_t *out = (int16_t *)stream;
  int out_samples = len / (int)sizeof(int16_t);

  static float mix_buf[SDL_AUDIO_SAMPLES * 2];
  if (out_samples > SDL_AUDIO_SAMPLES * 2) out_samples = SDL_AUDIO_SAMPLES * 2;
  memset(mix_buf, 0, out_samples * sizeof(float));

  uint32_t out_frames = out_samples / 2;

  int16_t tmp[TMP_BUF_SAMPLES];

  for (int i = 0; i < MAX_PLAYERS; i++) {
    AudioPlayer *p = &g_players[i];
    if (!p->active || p->play_state != SL_PLAYSTATE_PLAYING) continue;

    uint32_t src_rate = p->sample_rate;
    if (src_rate == 0) src_rate = SDL_OUTPUT_RATE;
    uint32_t src_channels = p->num_channels;
    if (src_channels == 0) src_channels = 2;
    uint32_t frame_size = src_channels * sizeof(int16_t);
    float vol = p->volume;
    if (vol < 0.0f || vol > 2.0f || vol != vol /* NaN */) {
      vol = 0.0f;
    }
    if (src_channels == 1) {
      vol *= 0.35f;
    } else {
      vol *= 0.8f;
    }

    uint32_t src_frames_needed;
    if (src_rate == SDL_OUTPUT_RATE) {
      src_frames_needed = out_frames;
    } else {
      src_frames_needed = (uint32_t)((uint64_t)out_frames * src_rate / SDL_OUTPUT_RATE) + 2;
    }

    uint32_t src_bytes_want = src_frames_needed * frame_size;
    if (src_bytes_want > sizeof(tmp)) src_bytes_want = sizeof(tmp);
    src_bytes_want = (src_bytes_want / frame_size) * frame_size;

    uint32_t got = ring_read(p, tmp, src_bytes_want);
    got = (got / frame_size) * frame_size;
    uint32_t src_frames_got = got / frame_size;
    if (src_frames_got == 0) continue;
    queue_consume(p, got);
    p->played_bytes += got;

    int underrun = (got < src_bytes_want);
    uint32_t fade_out_len = 64;
    uint32_t fade_start = 0;
    if (underrun) {
      p->underrun_count++;
      if (src_frames_got > fade_out_len) {
        fade_start = src_frames_got - fade_out_len;
      } else {
        fade_start = 0;
        fade_out_len = src_frames_got;
      }
      p->fadeout_count++;
    }

    uint32_t fadein_remaining = (p->frames_played < 32) ? (32 - p->frames_played) : 0;

    if (src_rate == SDL_OUTPUT_RATE && src_channels == 2) {
      uint32_t n = src_frames_got;
      if (n > out_frames) n = out_frames;
      for (uint32_t f = 0; f < n; f++) {
        float env = 1.0f;
        if (f < fadein_remaining) {
          env = (float)(p->frames_played + f) / 32.0f;
        }
        if (underrun && f >= fade_start && fade_out_len > 0) {
          float fo = 1.0f - (float)(f - fade_start) / (float)fade_out_len;
          if (fo < env) env = fo;
        }
        if (env < 0.0f) env = 0.0f;
        float v = vol * env;
        mix_buf[f * 2]     += (float)tmp[f * 2]     * v;
        mix_buf[f * 2 + 1] += (float)tmp[f * 2 + 1] * v;
      }
      p->frames_played += n;
    } else {
      uint32_t step = (uint32_t)((uint64_t)src_rate * 65536 / SDL_OUTPUT_RATE);
      uint32_t pos = 0;
      uint32_t fade_start_out = underrun ?
        (uint32_t)((uint64_t)fade_start * SDL_OUTPUT_RATE / src_rate) : out_frames + 1;
      uint32_t fade_out_len_out = (uint32_t)((uint64_t)fade_out_len * SDL_OUTPUT_RATE / src_rate);
      if (fade_out_len_out == 0) fade_out_len_out = 1;
      uint32_t mixed = 0;
      for (uint32_t f = 0; f < out_frames; f++) {
        uint32_t idx = pos >> 16;
        uint32_t frac = pos & 0xFFFF;
        if (idx >= src_frames_got) break;

        float l0, r0, l1, r1;
        if (src_channels == 1) {
          l0 = (float)tmp[idx]; r0 = l0;
          l1 = (idx + 1 < src_frames_got) ? (float)tmp[idx + 1] : l0; r1 = l1;
        } else {
          l0 = (float)tmp[idx * 2]; r0 = (float)tmp[idx * 2 + 1];
          if (idx + 1 < src_frames_got) { l1 = (float)tmp[(idx + 1) * 2]; r1 = (float)tmp[(idx + 1) * 2 + 1]; }
          else { l1 = l0; r1 = r0; }
        }

        float frac_f = (float)frac / 65536.0f;
        float left  = l0 + (l1 - l0) * frac_f;
        float right = r0 + (r1 - r0) * frac_f;
        float env = 1.0f;
        if (f < fadein_remaining) {
          env = (float)(p->frames_played + f) / 32.0f;
        }
        if (f >= fade_start_out && fade_out_len_out > 0) {
          float fo = 1.0f - (float)(f - fade_start_out) / (float)fade_out_len_out;
          if (fo < env) env = fo;
        }
        if (env < 0.0f) env = 0.0f;
        float v = vol * env;
        mix_buf[f * 2]     += left * v;
        mix_buf[f * 2 + 1] += right * v;
        pos += step;
        mixed++;
      }
      p->frames_played += mixed;
    }
  }

  /* Soft-clip limiter - smooth, no discontinuities */
  const float master_gain = 0.30f;
  const float threshold = 28000.0f;
  const float knee = 4000.0f;

  for (int s = 0; s < out_samples; s++) {
    float x = mix_buf[s] * master_gain;
    float ax = fabsf(x);
    if (ax > threshold) {
      float over = ax - threshold;
      float compressed = threshold + knee * (over / (over + knee));
      x = (x > 0) ? compressed : -compressed;
    }
    if (x > 32767.0f) x = 32767.0f;
    if (x < -32768.0f) x = -32768.0f;
    out[s] = (int16_t)x;
  }
}

static void ensure_audio_initialized(void) {
  if (g_audio_initialized) return;

  SDL_AudioSpec want, have;
  memset(&want, 0, sizeof(want));
  want.freq = 44100;
  want.format = AUDIO_S16SYS;
  want.channels = 2;
  want.samples = SDL_AUDIO_SAMPLES;
  want.callback = sdl_audio_callback;
  want.userdata = NULL;

  g_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
  if (g_audio_dev == 0) {
    debugPrintf("opensl: SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
    g_audio_initialized = 1;
    return;
  }

  debugPrintf("opensl: SDL audio opened: %dHz %dch %d samples\n",
              have.freq, have.channels, have.samples);
  SDL_PauseAudioDevice(g_audio_dev, 0);
  g_audio_initialized = 1;
}

static void player_reset_meta(AudioPlayer *p) {
  p->ring_head = 0;
  p->ring_tail = 0;
  memset(p->queued_sizes, 0, sizeof(p->queued_sizes));
  p->queued_head_index = 0;
  p->queued_tail_index = 0;
  p->queued_count = 0;
  p->queued_front_offset = 0;
  p->queue_capacity = 0;
  p->callback = NULL;
  p->callback_context = NULL;
  p->enqueued_since_cb = 0;
  p->last_enqueue_size = 0;
  p->play_callback = NULL;
  p->play_callback_context = NULL;
  p->play_event_mask = 0;
  p->enqueue_counter = 0;
  p->ever_enqueued = 0;
  p->headatend_fired = 0;
  p->decoder_done = 0;
  p->underrun_count = 0;
  p->fadeout_count = 0;
  p->frames_played = 0;
  p->play_state = SL_PLAYSTATE_STOPPED;
  p->volume = 1.0f;
  p->active = 1;
  p->played_bytes = 0;
  p->num_channels = 0;
  p->sample_rate = 0;
  p->bits_per_sample = 0;
}

static AudioPlayer *alloc_player(void) {
  pthread_mutex_lock(&g_players_lock);
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (!g_players[i].active) {
      AudioPlayer *p = &g_players[i];
      player_reset_meta(p);
      pthread_mutex_unlock(&g_players_lock);
      return p;
    }
  }
  for (int i = 0; i < MAX_PLAYERS; i++) {
    AudioPlayer *p = &g_players[i];
    if (p->play_state == SL_PLAYSTATE_STOPPED &&
        ring_readable(p) == 0 &&
        p->queued_count == 0) {
      player_reset_meta(p);
      pthread_mutex_unlock(&g_players_lock);
      return p;
    }
  }
  for (int i = 0; i < MAX_PLAYERS; i++) {
    AudioPlayer *p = &g_players[i];
    if (p->play_state == SL_PLAYSTATE_STOPPED) {
      if (g_audio_dev) SDL_LockAudioDevice(g_audio_dev);
      player_reset_meta(p);
      if (g_audio_dev) SDL_UnlockAudioDevice(g_audio_dev);
      pthread_mutex_unlock(&g_players_lock);
      return p;
    }
  }
  {
    int oldest = -1;
    uint64_t most_played = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
      AudioPlayer *p = &g_players[i];
      if (p->played_bytes >= most_played) {
        most_played = p->played_bytes;
        oldest = i;
      }
    }
    if (oldest >= 0) {
      AudioPlayer *p = &g_players[oldest];
      if (g_audio_dev) SDL_LockAudioDevice(g_audio_dev);
      p->play_state = SL_PLAYSTATE_STOPPED;
      player_reset_meta(p);
      if (g_audio_dev) SDL_UnlockAudioDevice(g_audio_dev);
      pthread_mutex_unlock(&g_players_lock);
      return p;
    }
  }
  pthread_mutex_unlock(&g_players_lock);
  debugPrintf("opensl: FATAL: no player slots at all!\n");
  return NULL;
}

/* SLPlayItf methods */
static SLresult play_GetDuration(void *self, SLmillisecond *pMsec) {
  (void)self;
  if (pMsec) *pMsec = SL_TIME_UNKNOWN;
  return SL_RESULT_SUCCESS;
}

static SLresult play_GetPosition(void *self, SLmillisecond *pMsec) {
  void **itf_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].play_ptr == itf_ptr) {
      AudioPlayer *p = &g_players[i];
      uint64_t position = 0;
      uint32_t channels = p->num_channels ? p->num_channels : 2;
      uint32_t sample_rate = p->sample_rate ? p->sample_rate : 44100;
      uint32_t bytes_per_frame = channels * sizeof(int16_t);
      if (bytes_per_frame != 0 && sample_rate != 0) {
        uint64_t frames = p->played_bytes / bytes_per_frame;
        position = (frames * 1000ULL) / sample_rate;
      }
      if (pMsec) *pMsec = (SLmillisecond)position;
      return SL_RESULT_SUCCESS;
    }
  }
  if (pMsec) *pMsec = 0;
  return SL_RESULT_SUCCESS;
}

static SLresult play_SetPlayState(void *self, SLuint32 state) {
  void **itf_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].play_ptr == itf_ptr) {
      AudioPlayer *p = &g_players[i];
      if (g_audio_dev) SDL_LockAudioDevice(g_audio_dev);
      if (state == SL_PLAYSTATE_STOPPED && p->play_state != SL_PLAYSTATE_STOPPED) {
        p->headatend_fired = 0;
        p->decoder_done = 0;
        p->ring_head = 0;
        p->ring_tail = 0;
        queue_reset(p);
      }
      if (state == SL_PLAYSTATE_PLAYING && p->play_state != SL_PLAYSTATE_PLAYING) {
        p->frames_played = 0;
        p->underrun_count = 0;
        p->fadeout_count = 0;
      }
      p->play_state = state;
      if (g_audio_dev) SDL_UnlockAudioDevice(g_audio_dev);
      return SL_RESULT_SUCCESS;
    }
  }
  return SL_RESULT_SUCCESS;
}

static SLresult play_GetPlayState(void *self, SLuint32 *pState) {
  void **itf_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].play_ptr == itf_ptr) {
      if (pState) *pState = g_players[i].play_state;
      return SL_RESULT_SUCCESS;
    }
  }
  if (pState) *pState = SL_PLAYSTATE_STOPPED;
  return SL_RESULT_SUCCESS;
}

static SLresult play_RegisterCallback(void *self, void *callback, void *ctx) {
  void **itf_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].play_ptr == itf_ptr) {
      g_players[i].play_callback = (void (*)(void *, void *, SLuint32))callback;
      g_players[i].play_callback_context = ctx;
      return SL_RESULT_SUCCESS;
    }
  }
  return SL_RESULT_SUCCESS;
}

static SLresult play_SetCallbackEventsMask(void *self, SLuint32 eventFlags) {
  void **itf_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].play_ptr == itf_ptr) {
      g_players[i].play_event_mask = eventFlags;
      return SL_RESULT_SUCCESS;
    }
  }
  return SL_RESULT_SUCCESS;
}

/* SLVolumeItf methods */
static SLresult volume_SetVolumeLevel(void *self, SLmillibel level) {
  float linear;
  if (level <= -9600) linear = 0.0f;
  else linear = powf(10.0f, level / 2000.0f);
  if (linear > 2.0f) linear = 1.0f;

  void **itf_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].volume_ptr == itf_ptr) {
      g_players[i].volume = linear;
      return SL_RESULT_SUCCESS;
    }
  }
  return SL_RESULT_SUCCESS;
}

static SLresult volume_GetVolumeLevel(void *self, SLmillibel *pLevel) {
  (void)self;
  if (pLevel) *pLevel = 0;
  return SL_RESULT_SUCCESS;
}

static SLresult volume_GetMaxVolumeLevel(void *self, SLmillibel *pMaxLevel) {
  (void)self;
  if (pMaxLevel) *pMaxLevel = 0;
  return SL_RESULT_SUCCESS;
}

/* SLBufferQueueItf methods */
static SLresult bq_Enqueue(void *self, const void *pBuffer, SLuint32 size) {
  void **itf_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].bq_ptr == itf_ptr) {
      AudioPlayer *p = &g_players[i];
      uint32_t written = ring_write(p, pBuffer, size);
      queue_push(p, written);
      p->enqueued_since_cb += written;
      if (written > 0) {
        p->last_enqueue_size = written;
        p->enqueue_counter++;
        p->ever_enqueued = 1;
      }
      return SL_RESULT_SUCCESS;
    }
  }
  return SL_RESULT_SUCCESS;
}

static SLresult bq_Clear(void *self) {
  void **itf_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].bq_ptr == itf_ptr) {
      AudioPlayer *p = &g_players[i];
      if (g_audio_dev) SDL_LockAudioDevice(g_audio_dev);
      p->ring_head = 0;
      p->ring_tail = 0;
      queue_reset(p);
      p->enqueued_since_cb = 0;
      p->enqueue_counter = 0;
      p->ever_enqueued = 0;
      p->headatend_fired = 0;
      p->decoder_done = 0;
      if (g_audio_dev) SDL_UnlockAudioDevice(g_audio_dev);
      return SL_RESULT_SUCCESS;
    }
  }
  return SL_RESULT_SUCCESS;
}

static SLresult bq_GetState(void *self, void *pState) {
  void **itf_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].bq_ptr == itf_ptr) {
      AudioPlayer *p = &g_players[i];
      if (pState) {
        SLuint32 *state = (SLuint32 *)pState;
        uint32_t play_index = 0;
        uint32_t capacity = p->queue_capacity ? p->queue_capacity : 1;
        if (p->queued_count > 0) {
          play_index = p->queued_head_index % capacity;
        }
        state[0] = p->queued_count;
        state[1] = play_index;
      }
      return SL_RESULT_SUCCESS;
    }
  }
  return SL_RESULT_SUCCESS;
}

static SLresult bq_RegisterCallback(void *self, slBufferQueueCallback callback, void *pContext) {
  void **itf_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].bq_ptr == itf_ptr) {
      AudioPlayer *p = &g_players[i];
      p->callback = callback;
      p->callback_context = pContext;
      return SL_RESULT_SUCCESS;
    }
  }
  return SL_RESULT_SUCCESS;
}

static SLresult bq_GetState_or_RegisterCallback(void *self, void *arg1, void *arg2) {
  uintptr_t maybe_callback = (uintptr_t)arg1;
  uintptr_t text = (uintptr_t)text_base;

  if (text_base && maybe_callback >= text && maybe_callback < text + text_size) {
    return bq_RegisterCallback(self, (slBufferQueueCallback)arg1, arg2);
  }

  return bq_GetState(self, arg1);
}

/* Stub for unused interfaces */
static SLresult stub_success(void) { return SL_RESULT_SUCCESS; }

/* Player object methods */
static SLresult player_Realize(void *self, SLBoolean async) {
  (void)self; (void)async;
  return SL_RESULT_SUCCESS;
}

static SLresult player_GetInterface(void *self, SLInterfaceID iid, void **pInterface) {
  void **obj_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].obj_ptr == obj_ptr) {
      AudioPlayer *p = &g_players[i];
      if (iid == sl_IID_PLAY) {
        *pInterface = &p->play_ptr;
      } else if (iid == sl_IID_VOLUME) {
        *pInterface = &p->volume_ptr;
      } else if (iid == sl_IID_BUFFERQUEUE || iid == sl_IID_ANDROIDSIMPLEBUFFERQUEUE) {
        *pInterface = &p->bq_ptr;
      } else if (iid == sl_IID_EFFECTSEND) {
        *pInterface = &p->effectsend_ptr;
      } else {
        *pInterface = &p->effectsend_ptr;
      }
      return SL_RESULT_SUCCESS;
    }
  }
  return SL_RESULT_SUCCESS;
}

static void player_Destroy(void *self) {
  void **obj_ptr = (void **)self;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (&g_players[i].obj_ptr == obj_ptr) {
      AudioPlayer *p = &g_players[i];
      if (g_audio_dev) SDL_LockAudioDevice(g_audio_dev);
      p->play_state = SL_PLAYSTATE_STOPPED;
      p->active = 0;
      if (g_audio_dev) SDL_UnlockAudioDevice(g_audio_dev);
      return;
    }
  }
}

static void setup_player_vtables(AudioPlayer *p) {
  for (int i = 0; i < 8; i++) p->obj_vtable[i] = (void *)stub_success;
  p->obj_vtable[0] = (void *)player_Realize;
  p->obj_vtable[3] = (void *)player_GetInterface;
  p->obj_vtable[6] = (void *)player_Destroy;
  p->obj_ptr = p->obj_vtable;

  for (int i = 0; i < 8; i++) p->play_vtable[i] = (void *)stub_success;
  p->play_vtable[0] = (void *)play_SetPlayState;
  p->play_vtable[1] = (void *)play_GetPlayState;
  p->play_vtable[2] = (void *)play_GetDuration;
  p->play_vtable[3] = (void *)play_GetPosition;
  p->play_vtable[4] = (void *)play_RegisterCallback;
  p->play_vtable[5] = (void *)play_SetCallbackEventsMask;
  p->play_ptr = p->play_vtable;

  for (int i = 0; i < 8; i++) p->volume_vtable[i] = (void *)stub_success;
  p->volume_vtable[0] = (void *)volume_SetVolumeLevel;
  p->volume_vtable[1] = (void *)volume_GetVolumeLevel;
  p->volume_vtable[2] = (void *)volume_GetMaxVolumeLevel;
  p->volume_ptr = p->volume_vtable;

  for (int i = 0; i < 8; i++) p->bq_vtable[i] = (void *)stub_success;
  p->bq_vtable[0] = (void *)bq_Enqueue;
  p->bq_vtable[1] = (void *)bq_Clear;
  p->bq_vtable[2] = (void *)bq_GetState_or_RegisterCallback;
  p->bq_vtable[3] = (void *)bq_RegisterCallback;
  p->bq_ptr = p->bq_vtable;

  for (int i = 0; i < 8; i++) p->effectsend_vtable[i] = (void *)stub_success;
  p->effectsend_ptr = p->effectsend_vtable;
}

/* Output mix */
static void *g_outmix_vtable[8];
static void *g_outmix_ptr;

static SLresult outmix_Realize(void *self, SLBoolean async) { (void)self; (void)async; return SL_RESULT_SUCCESS; }

static SLresult outmix_GetInterface(void *self, SLInterfaceID iid, void **pInterface) {
  (void)self; (void)iid;
  static void *stub_vtable[8];
  static void *stub_ptr;
  static int inited = 0;
  if (!inited) {
    for (int i = 0; i < 8; i++) stub_vtable[i] = (void *)stub_success;
    stub_ptr = stub_vtable;
    inited = 1;
  }
  if (pInterface) *pInterface = &stub_ptr;
  return SL_RESULT_SUCCESS;
}

static void outmix_Destroy(void *self) { (void)self; }

static void init_outmix(void) {
  static int inited = 0;
  if (inited) return;
  inited = 1;
  for (int i = 0; i < 8; i++) g_outmix_vtable[i] = (void *)stub_success;
  g_outmix_vtable[0] = (void *)outmix_Realize;
  g_outmix_vtable[3] = (void *)outmix_GetInterface;
  g_outmix_vtable[6] = (void *)outmix_Destroy;
  g_outmix_ptr = g_outmix_vtable;
}

/* Engine interface */
static SLresult engine_CreateOutputMix(void *self, void **pMix,
                                        SLuint32 numInterfaces,
                                        const SLInterfaceID *pInterfaceIds,
                                        const SLBoolean *pInterfaceRequired) {
  (void)self; (void)numInterfaces; (void)pInterfaceIds; (void)pInterfaceRequired;
  init_outmix();
  if (pMix) *pMix = &g_outmix_ptr;
  return SL_RESULT_SUCCESS;
}

static SLresult engine_CreateAudioPlayer(void *self, void **pPlayer,
                                          void *pAudioSrc, void *pAudioSnk,
                                          SLuint32 numInterfaces,
                                          const SLInterfaceID *pInterfaceIds,
                                          const SLBoolean *pInterfaceRequired) {
  (void)self; (void)pAudioSnk; (void)numInterfaces;
  (void)pInterfaceIds; (void)pInterfaceRequired;

  ensure_audio_initialized();

  AudioPlayer *p = alloc_player();
  if (!p) {
    if (pPlayer) *pPlayer = NULL;
    return SL_RESULT_RESOURCE_ERROR;
  }

  if (pAudioSrc) {
    SLDataSource *src = (SLDataSource *)pAudioSrc;
    if (src->pLocator) {
      SLDataLocator_BufferQueue *loc = (SLDataLocator_BufferQueue *)src->pLocator;
      if (loc->locatorType == SL_DATALOCATOR_BUFFERQUEUE) {
        p->queue_capacity = loc->numBuffers;
      }
    }
    if (src->pFormat) {
      SLDataFormat_PCM *fmt = (SLDataFormat_PCM *)src->pFormat;
      if (fmt->formatType == SL_DATAFORMAT_PCM) {
        p->num_channels = fmt->numChannels;
        p->sample_rate = fmt->samplesPerSec / 1000;
        p->bits_per_sample = fmt->bitsPerSample;
      }
    }
  }

  if (p->sample_rate == 0) {
    p->num_channels = 2;
    p->sample_rate = 44100;
    p->bits_per_sample = 16;
  }

  setup_player_vtables(p);
  if (pPlayer) *pPlayer = &p->obj_ptr;
  return SL_RESULT_SUCCESS;
}

/* Engine object */
static void *g_engine_obj_vtable[8];
static void *g_engine_obj_ptr;
static void *g_engine_itf_vtable[16];
static void *g_engine_itf_ptr;

static SLresult engine_obj_Realize(void *self, SLBoolean async) { (void)self; (void)async; return SL_RESULT_SUCCESS; }

static SLresult engine_obj_GetInterface(void *self, SLInterfaceID iid, void **pInterface) {
  (void)self;
  if (iid == sl_IID_ENGINE) {
    if (pInterface) *pInterface = &g_engine_itf_ptr;
  } else {
    static void *stub_vtable[8];
    static void *stub_ptr;
    static int inited = 0;
    if (!inited) {
      for (int i = 0; i < 8; i++) stub_vtable[i] = (void *)stub_success;
      stub_ptr = stub_vtable;
      inited = 1;
    }
    if (pInterface) *pInterface = &stub_ptr;
  }
  return SL_RESULT_SUCCESS;
}

static void engine_obj_Destroy(void *self) {
  (void)self;
  if (g_audio_dev) {
    SDL_CloseAudioDevice(g_audio_dev);
    g_audio_dev = 0;
    g_audio_initialized = 0;
  }
}

static void init_engine(void) {
  static int inited = 0;
  if (inited) return;
  inited = 1;

  for (int i = 0; i < 8; i++) g_engine_obj_vtable[i] = (void *)stub_success;
  g_engine_obj_vtable[0] = (void *)engine_obj_Realize;
  g_engine_obj_vtable[3] = (void *)engine_obj_GetInterface;
  g_engine_obj_vtable[6] = (void *)engine_obj_Destroy;
  g_engine_obj_ptr = g_engine_obj_vtable;

  for (int i = 0; i < 16; i++) g_engine_itf_vtable[i] = (void *)stub_success;
  g_engine_itf_vtable[2] = (void *)engine_CreateAudioPlayer;
  g_engine_itf_vtable[7] = (void *)engine_CreateOutputMix;
  g_engine_itf_ptr = g_engine_itf_vtable;
}

/* Public API */
void opensles_shim_pump_callbacks(void) {
  for (int i = 0; i < MAX_PLAYERS; i++) {
    AudioPlayer *p = &g_players[i];
    if (!p->active || p->play_state != SL_PLAYSTATE_PLAYING) continue;

    uint32_t readable = ring_readable(p);
    uint32_t callback_threshold = p->last_enqueue_size;
    if (callback_threshold == 0 || callback_threshold > (RING_BUFFER_SIZE / 2)) {
      callback_threshold = RING_BUFFER_SIZE / 4;
    }
    uint32_t refill_threshold = callback_threshold * 2;
    if (refill_threshold > RING_BUFFER_SIZE / 2) refill_threshold = RING_BUFFER_SIZE / 2;

    int max_calls = 4;
    while (p->callback && readable <= refill_threshold && max_calls > 0) {
      uint32_t counter_before = p->enqueue_counter;
      p->callback(&p->bq_ptr, p->callback_context);

      if (p->ever_enqueued && !p->decoder_done &&
          p->enqueue_counter == counter_before) {
        p->decoder_done = 1;
        break;
      }
      readable = ring_readable(p);
      max_calls--;
    }

    if (!p->callback && p->ever_enqueued && !p->decoder_done &&
        p->queued_count == 0 && readable == 0) {
      p->decoder_done = 1;
    }

    if (p->decoder_done && !p->headatend_fired) {
      if (ring_readable(p) == 0) {
        p->headatend_fired = 1;
        if (p->play_callback && (p->play_event_mask & SL_PLAYEVENT_HEADATEND)) {
          p->play_callback(&p->play_ptr, p->play_callback_context, SL_PLAYEVENT_HEADATEND);
        }
        if (g_audio_dev) SDL_LockAudioDevice(g_audio_dev);
        p->play_state = SL_PLAYSTATE_STOPPED;
        queue_reset(p);
        if (g_audio_dev) SDL_UnlockAudioDevice(g_audio_dev);
      }
    }
  }
}

SLresult slCreateEngine_shim(void **pEngine, SLuint32 numOptions,
                              const void *pEngineOptions,
                              SLuint32 numInterfaces,
                              const SLInterfaceID *pInterfaceIds,
                              const SLBoolean *pInterfaceRequired) {
  (void)numOptions; (void)pEngineOptions; (void)numInterfaces;
  (void)pInterfaceIds; (void)pInterfaceRequired;

  init_engine();
  if (pEngine) *pEngine = &g_engine_obj_ptr;
  return SL_RESULT_SUCCESS;
}
