# nxgl

`nxgl` is the common, static SDL2/GLES window-and-context layer for new
NextOS/PortMaster compatibility loaders. Its real baseline is **Mali-450 with
OpenGL ES 2.0**. It does not require Mesa, desktop OpenGL, or GLES 3.x.

The library solves the part that is genuinely common between games:

- preserve the video backend inherited from PortMaster/the firmware;
- consider video usable only after a real window, GLES context, delivered
  config, and drawable all succeed;
- after that real failure, remove an inherited video hint and let SDL
  autodetect once; never choose a replacement `SDL_VIDEODRIVER` by name;
- select a native window size from SDL first, then passed DRM/fbdev facts;
- try the exact GLES/RGBA/depth/stencil ladder declared by the engine adapter;
- reject a desktop-GL context before the game can render black GLSL-ES output;
- record the backend, vendor, renderer, GL/GLSL versions, real drawable, and
  delivered config;
- leave presentation and every engine lifecycle call under explicit ownership.

`nxgl` is not an EGL emulation layer and does not invent Android lifecycle
steps. A loader may put its existing fake-EGL or engine adapter above it.

## Evidence used

The sanitized source of truth is
`references/m13-video-evidence-v1.json`. It separates approved working ports
from historical facts, narrow quirk provenance and future pilots:

| Reference | Classification | Rule retained by nxgl |
|---|---|---|
| Horizon Chase | approved positive, multi-stack | Select ownership from the backend actually opened; keep KMSDRM/Wayland present inside SDL ownership; use real GL strings and drawable. |
| Castle of Illusion | approved positive, multi-device | Keep global NPOT wrap rewriting off; inspect sampler/wrap/atlas first for a geometrically correct black silhouette; use the real drawable. |
| Sonic 4 EP2 AArch64 | approved positive, release-scoped | Request an ES profile, reject measured desktop GL, recreate window/context per config candidate, and wait for a positive drawable. |
| Bully2 | historical/delisted narrow fact only | Cross-check same-stack EGL symbol resolution; it is not a current positive whole-port reference and contributes no copied implementation. |
| LEGO Star Wars TFA | alpha-one quirk provenance only | Name the Amlogic default-backbuffer alpha-zero symptom; the workaround remains exact, adapter-owned and opt-in. |
| Chrono Trigger | negative/designed-only pilot | Supplies no positive runtime or physical evidence to M13. It becomes evidence only after a future framework migration and authorized device gate. |

The physical rows above are imported, hash-pinned release history; **M13 did
not run a device or guest**. Port-specific fixes did **not** become global
behavior. In particular, nxgl never enables an FBO clear, forces texture wrap,
alters a shader, changes an engine render scale, or skips lifecycle work.

## Runtime contract

The normal startup order is:

```text
PortMaster launcher
  -> nxcompat preflight/environment plan
  -> SDL video negotiation
  -> nxgl real-output negotiation
       window -> GLES context -> delivered config -> real drawable
  -> engine/Android adapter lifecycle
```

Call the negotiation before the engine creates any SDL window or render
thread. The inherited-hint recovery restarts the SDL video subsystem; it is
not a hot-recovery API for a running game.

The API-v1 entry points remain source/ABI compatible. New integrations use
API v2 (`NXGL_API_CURRENT_VERSION`): the caller declares one stack owner and
nxgl freezes that provider's callbacks, handles and userdata through
`nxgl_close_v2()`. Provider callback code and userdata must outlive every
open/present/rollback/close callback. Report handle copies are observations,
not ownership transfers, and become stale at close.

`nxgl_open()`/`nxgl_open_v2()` can initialize `SDL_INIT_VIDEO` if necessary,
but they never assign a replacement backend name to
`SDL_VIDEODRIVER`/`SDL_VIDEO_DRIVER`,
sets `MESA_GLES_VERSION_OVERRIDE`, selects a card number, or fixes a
resolution. If nxcompat already initialized SDL, nxgl still
performs the real window/context/drawable gate; `SDL_InitSubSystem()` alone is
not treated as proof that video works.

All mutating API-v1 and API-v2 entry points share a non-blocking nxgl arbiter.
Open, make-current and present return byte-atomic `NXGL_ERROR_BUSY` before
touching outputs or graphics state; the legacy void close is a no-op while
busy. API-v1 accessors reject API-v2 contexts so they cannot bypass the frozen
stack. The adapter must still serialize the preceding nxcompat transaction
with nxgl startup; nxgl does not reach into nxcompat's private arbiter.

Resolution precedence is fixed and capability-based:

1. `SDL_GetDesktopDisplayMode()`;
2. `SDL_GetCurrentDisplayMode()`;
3. `SDL_GetDisplayBounds()`;
4. DRM dimensions passed by the preflight probe;
5. fbdev dimensions passed by the preflight probe.

There is no built-in `1280x720` fallback. After context creation,
`SDL_GL_GetDrawableSize()` is authoritative and must become positive. The
default 300 ms window pumps asynchronous compositor configure events; if the
drawable differs from the requested window, the last real value wins.

## Declaring an engine ladder

The caller declares what the engine actually supports. This GLES2 example
keeps RGBA8888 mandatory and relaxes only depth/stencil:

```c
#include "nxgl.h"

static const nxgl_config_candidate configs[] = {
    {2, 0, 8, 8, 8, 8, 24, 8, 1},
    {2, 0, 8, 8, 8, 8, 16, 0, 1},
    {2, 0, 8, 8, 8, 8,  0, 0, 1},
};

nxgl_engine_requirements requirements;
nxgl_open_options options;
nxgl_context *graphics = NULL;
nxgl_report report;

nxgl_engine_requirements_init(&requirements);
requirements.minimum_alpha_bits = 8;
requirements.minimum_depth_bits = 0;

nxgl_open_options_init(&options);
options.window_title = "My Port";
options.requirements = &requirements;
options.candidates = configs;
options.candidate_count = sizeof(configs) / sizeof(configs[0]);

if (nxgl_open(&options, &graphics, &report) != NXGL_SUCCESS) {
    /* Show the error through the port's normal startup/error path. */
}
```

The default request floor is GLES2 with RGB888 and double buffering. GLES3 is
never inserted into the request ladder automatically. A real GLES3 context
returned by the driver for an ES2 request is accepted because it still exposes
the GLES2 API; an engine that requires an exact maximum can declare one. If an
engine is proven to need an explicit GLES3 retry, its adapter adds that
candidate after the GLES2 candidates. Nothing in nxgl asks Mesa for “3.2”.

Every candidate sets all RGB, alpha, depth, stencil, double-buffer, profile,
version, and no-MSAA attributes before creating its own window and context.
API v2 then requires a current same-stack EGL display/context/surface, finds
the delivered `EGLConfig` by its real config ID, checks ES2/window capability,
and compares the positive EGL surface size with the drawable. The delivered
values must satisfy the engine requirements.

## Backend retry and desktop-GL rejection

The first full attempt uses the environment exactly as inherited. Each failed
candidate releases its own window/context before the next candidate. Only
after the complete inherited path fails, and only when nxgl initialized and
therefore owns SDL video, may the default policy:

1. remove an inherited `SDL_VIDEODRIVER`/`SDL_VIDEO_DRIVER`;
2. restart SDL video without naming another backend;
3. repeat the real-output gate once.

If SDL returns a desktop context, nxgl destroys it. The default policy may
retry that same candidate once with `SDL_HINT_OPENGL_ES_DRIVER=1`; on an
already selected X11 backend it also requires the EGL rather than GLX hint.
Both the environment and SDL hints are snapshotted dynamically, restored
exactly (absent, empty or full value), and verified before success is
published. There is no third attempt. This chooses the GLES API required by
the engine; it does not choose a firmware backend or GL version. The bounded
attempt journal and status callback record fallback actions and stable reasons
without retaining provider error text, paths, IPs or tokens.

If SDL video was already caller-owned, nxgl never quits or restarts it for
autodetection; failure is returned with the inherited environment intact.

## Status/logo callback and nxcompat

The API-v1 `nxgl_status_callback` receives bounded lines for resolution, each
attempted config, fallback actions, the selected profile, and errors. API v2
keeps per-attempt detail in its bounded, finite-reason journal and sends only
the terminal selected/error notification through the callback. A port may
route that notification to stderr and to text over its existing startup logo;
nxgl does not create a competing splash screen. Callback code and userdata are
borrowed by a successful context and must remain alive until its final close;
callbacks run under the nxgl arbiter and must return promptly.

For API v2, the terminal selected/error notification runs inside the
transaction and arbiter, before either output is published. It must return
promptly. An nxgl open/close/present call re-entered from that notification
returns byte-atomic BUSY; after the callback returns, nxgl publishes the
context/report (or cleanup handle) and releases the arbiter.

With `-DNXGL_WITH_NXCOMPAT=ON`, `libnxgl-nxcompat.a` adds two adapters:

- `nxgl_nxcompat_resolution_sources()` passes the already-probed DRM/fbdev
  dimensions to nxgl;
- `nxgl_nxcompat_capture_report()` converts a free-form `nxgl_report` into the
  legacy diagnostic profile and emits the shared graphics status line. This
  call is diagnostic-only and **never** satisfies a capability requirement;
- `nxgl_nxcompat_publish_context()` accepts an opaque, already-open
  `nxgl_context`, verifies that its SDL window/context are current, requeries
  GL strings, the delivered SDL config and positive drawable, then resolves
  the current EGL display/context/config through `SDL_GL_GetProcAddress()` and
  transactionally publishes the typed receipt to `nxcompat_registry`.

The strong bridge does not call `nxgl_open()`, create or destroy resources,
swap buffers, alter GL state, or select a backend. A missing EGL symbol creates
an honest partial receipt: window/GLES/drawable evidence may remain valid while
`graphics.egl` and `graphics.egl-config` stay absent/lost. Required-capability
evaluation, rather than renderer-name inference, decides whether that is fatal
for the port. Receipt generations must increase for a given registry; stale or
malformed publications leave the previous state byte-identical.

The adapter links to nxcompat without changing nxcompat itself.

## Presentation ownership

The default `nxgl_present_policy` is `NXGL_PRESENT_ENGINE_OWNED`, a strict
no-op. Linking the library cannot add a second swap or alter the framebuffer.
An engine adapter must explicitly select one of:

- `NXGL_PRESENT_SDL` for `SDL_GL_SwapWindow()`;
- `NXGL_PRESENT_ADAPTER` with a callback for raw EGL, a firmware blitter, or
  another proven path.

Two optional flags are also adapter decisions:

- `NXGL_PRESENT_FINISH_BEFORE_SWAP` calls `glFinish()`;
- `NXGL_PRESENT_FORCE_BACKBUFFER_ALPHA_ONE` performs an A-only clear while
  preserving scissor, color mask, and clear color; it refuses to run while an
  off-screen FBO is bound.

Neither flag is inferred from a device name, GPU name, or SDL backend. API v2
accepts alpha-one only with the exact named Amlogic quirk and an explicit
observed-alpha-zero reason, on FBO 0 with a real alpha channel. It snapshots
and verifies the framebuffer binding, and restores plus verifies scissor
enablement, all four color-mask bits and clear color before the provider
performs one present. `glFinish`
remains a separate explicit flag and is never global. A raw-EGL provider must
resolve and validate the same stack that created its context; SDL ownership
uses the SDL provider's present callback.

## Surface lifecycle and dimensions

API v2 exposes passive observations for focus, minimize/restore, resize and
context loss/recreation. These calls update a caller-owned state machine and
monotonic generations; they do not recreate a native surface, invent Android
lifecycle events or call the engine. Malformed booleans, partial dimensions,
out-of-range values and generation overflow fail without changing the state.

`nxgl_calculate_surface_metrics_v2()` keeps display, drawable, viewport and
render-target dimensions distinct and reports their measured ratios. It never
assumes that requested window size equals scanout, drawable or internal render
scale. The pure silhouette classifier only prioritizes a sampler/wrap/atlas
audit when black pixels preserve a correct geometric silhouette; it never
clears an FBO, swaps a shader or converts a texture.

## Texture policy

nxgl does not hook texture calls. NPOT workarounds and forced
`CLAMP_TO_EDGE` are therefore **off by construction**. If one game proves that
it needs a sampler workaround, that belongs in its engine adapter and must not
become a global default. This preserves repeated/mirrored atlas UVs on
Mali-450 and other GLES2 drivers.

## Build and tests

```sh
cmake -S framework/nxgl -B /tmp/nxgl-build \
  -DNXGL_BUILD_TESTS=ON \
  -DNXGL_BUILD_NATIVE_TESTS=OFF \
  -DNXGL_WITH_NXCOMPAT=ON \
  -DNXGL_ENABLE_SANITIZERS=ON
cmake --build /tmp/nxgl-build --parallel
ctest --test-dir /tmp/nxgl-build --output-on-failure
```

This command is the hermetic M12 receipt gate. With
`NXGL_BUILD_NATIVE_TESTS=OFF`, it builds
only the nxcompat bridge test, whose SDL/GL/EGL symbols and opaque context are
entirely test-owned. It never calls `nxgl_open()` and does not require a host
GPU, window system, EGL driver, device, or session mutation.

M13 has a separate canonical runner:

```sh
bash framework/nxgl/tests/run-m13-host.sh
```

It compiles the API-v2 open, present, lifecycle, metrics and diagnostic
fixtures with GCC and Clang sanitizers. Forbidden-symbol and dynamic-dependency
barriers run before the fixtures, so the automatic gate cannot initialize SDL
video, create a window/context, call EGL/GLES or touch a GPU/session/device.
Its result deliberately records `physical_device_evidence=0`; imported release
history is not a new hardware run.

`NXGL_BUILD_NATIVE_TESTS=ON` enables the separate native-linked policy suite
for intentional/manual host validation. It is not part of the hermetic M12
receipt gate and must not be confused with physical device proof. Both paths
compile as C99 with strict warnings; the M12 bridge path is the safe default.

The output is a static library intended to be compiled into each loader.
Public packages still need to audit the **final loader and every bundled Linux
ELF** for the project-wide `GLIBC <= 2.30` gate; a host test build is not a
release artifact.
