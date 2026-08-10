# nxaudio

`nxaudio` is the small common audio contract for M14. It does not choose a
firmware by name and does not replace an engine mixer. Backend discovery and a
real SDL device-open receipt remain owned by `nxcompat`; engine callbacks,
formats and teardown remain in the exact adapter that proved them.

The common layer provides:

- a lock-free single-producer/single-consumer PCM queue: a worker performs
  guest calls, conversion and heavy mixing before submitting PCM; the realtime
  consumer only copies or zero-fills bytes;
- explicit ready/running/paused/device-lost/closed states, underrun accounting
  and deterministic caller-quiesced close;
- distinct reasons for an absent server, a device-open failure and a mixer that
  opened but stopped producing PCM;
- an isolated-HOME plan that exposes an existing `.asoundrc` through
  `ALSA_CONFIG_PATH`, without inventing or copying one;
- a finite adapter allowlist. OpenSL ES, OpenAL, FMOD, FMOD Ex and Wwise are
  accepted only with a source-proven contract. AAudio has no generic contract
  and therefore remains rejected until an approved guest proves one;
- an audibility gate that rejects synthetic/API-only success. It accepts only
  current physical evidence or source-pinned imported physical evidence with a
  real open, nonzero PCM and human confirmation.

## Backend order and receipts

`nxcompat_sdl2_negotiate_audio_v2()` tries the inherited/default backend first,
rejects `dummy` and `disk`, then permits one normal SDL autodetect attempt. A
receipt is publishable only after a nonzero device ID, a valid obtained spec,
close and verified cleanup. Frequency, sample format, channels, period and
derived latency are copied into the engine adapter's `nxaudio_format`; nxaudio
never silently substitutes them.

An unavailable Pulse/PipeWire server is not called an underrun. Conversely, a
server and device that opened but whose mixer callbacks produce no frames is
reported as `mixer-starved`. Every fallback keeps the stable reason in the
receipt/log; raw backend errors are diagnostic only.

## Adapter boundaries

| Stack | Approved contract | Boundary |
|---|---|---|
| SDL2 | `nxcompat-sdl2-audio-v2` | common real-open probe |
| OpenSL ES | `tasm2-opensl-sdl-v1`, `castle-opensl-sdl-v1` | exact engine queue/callback |
| AAudio | none | fail closed until a guest proves the API/lifecycle |
| OpenAL | `bully2-openal-v1` | use the firmware provider; never bundle an incompatible one |
| FMOD | `horizon-fmod-sdl-v1` | exact Unity/FMOD PCM bridge |
| FMOD Ex | `castle-fmodex-v1` | exact Castle bridge |
| Wwise | `sor4-wwise-openal-glibc230-v1` | canonical `NextOs-Ports/sor4-nextos/port/wwise-native/build-glibc230.sh` recipe only |

The positive cross-check is deliberately limited to approved evidence from
Horizon Chase, TASM2 1.2.7d, Streets of Rage 4 and Castle of Illusion. Historical
WIP notes never satisfy the gate.

## Host gate

```sh
bash framework/nxaudio/tests/run-host.sh
python3 -B framework/nxaudio/tests/test_m14_audio_contract.py
```

Both are host-only. They open no SDL/audio device, execute no guest code and do
not use the network. Audible physical evidence is imported from the approved,
source-pinned port records; it is not fabricated by the host test.
