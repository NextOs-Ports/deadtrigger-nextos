# nxandroid

`nxandroid` 0.1.0 is a small C99 contract layer for Android-native port
adapters. It validates and executes an adapter-declared lifecycle profile and
resolves imports through an explicit ABI catalog. It is not a Java runtime, a
JNI implementation, a loader, an EGL backend or an engine abstraction.

The adapter remains the sole owner of `JavaVM`, `JNIEnv`, Activity objects,
registered native methods, surfaces, graphics, input, save calls and engine
entry points. The core sees only ordered phase records and two generic
callbacks. Consequently there is no universal fake JNI behavior to inherit in
a new port.

## Lifecycle contract

An `nxandroid_profile` contains all modules used by that lifecycle plus the
exact ordered `nxandroid_step` list selected by the adapter. The validator does
not reorder or synthesize a phase. Its common safety constraints are:

- every declared module runs `MODULE_INITIALIZED` once and, when declared,
  `MODULE_JNI` once after its own initializer and before `ACTIVITY_CREATE`;
- `ACTIVITY_CREATE` precedes `GRAPHICS_REQUEST` and guest surface phases;
- `GRAPHICS_REQUEST` precedes both `GL_READY` and `SURFACE_UP` for a surface
  generation. Approved adapters may place host `GL_READY` before the guest
  surface callbacks (Bully-like) or after them (Unity-like);
- resume/pause and focus-gain/loss are independent adapter-ordered epoch pairs.
  Their matching events must use the same non-zero `cycle_id`; they do not
  pretend that Android always delivers them at one fixed surface point;
- `ENTRY` requires an active guest surface by default. A source-proven adapter
  whose native flow creates GL, enters the engine and creates objects before its
  guest Surface callback may opt into
  `NXANDROID_PROFILE_ALLOW_ENTRY_BEFORE_SURFACE_CALLBACK`. That exception still
  requires `GRAPHICS_REQUEST` and `GL_READY` for the same pending generation;
  both `ENTRY` and `OBJECTS_READY` must complete before `SURFACE_UP`, while
  `INPUT_ENABLE` and the blocking `RUN_LOOP` remain forbidden until that Surface
  is active. The opt-in is rejected when a profile does not actually use it or
  attempts to mix it with the default Surface-first relation;
- `OBJECTS_READY` belongs to that one entry and its generation. A driven profile
  then enters one synchronous, blocking `RUN_LOOP`; only after it returns may
  input be disabled and shutdown continue. Recreated surfaces and repeated
  resume/focus cycles do not rerun entry or object creation;
- the closing path contains pause and save in that state order. When a profile
  declared `FOCUS_GAIN`, its matching `FOCUS_LOSS` is mandatory before pause;
  adapters without a proven focus callback do not invent one. Normal return
  additionally requires `NATIVE_SHUTDOWN` immediately before `TERMINAL`. An
  adapter-owned terminal may follow save without a native shutdown only with
  `NXANDROID_PROFILE_ALLOW_ADAPTER_TERMINAL` and a specific terminal contract
  ID.

Normal return also requires `SURFACE_DOWN` before native shutdown, so the core
does not assume that an opaque shutdown callback released graphics ownership.
The narrow adapter-terminal alternative may intentionally end with the surface
still active when its adapter contract proves why native teardown is unsafe.

`TERMINAL` is still only an adapter callback carrying a policy and contract ID.
The core never calls `exit`, `_exit`, raises a signal or launches a process.

The profile is copied into each context. There is no process-global state, so
sequential and independent contexts do not share module, surface, callback or
import state. Lazy modules loaded after Activity are deliberately not inferred
in version 0.1.0; a future contract must add an explicit phase/flag rather than
silently weakening the declared-module gate.

Some approved runtimes expose one blocking owner such as KOTOR's `SDL_main`:
that call bundles entry, window/GL, objects, input, loop and terminal internally.
It must not be threaded or split into fictitious phases just to fit the driven
profile. `RUNTIME_DELEGATED` is the narrow alternative: it must be the final
step, requires `NXANDROID_PROFILE_ALLOW_DELEGATED_RUNTIME` plus a specific
contract ID, and is called synchronously. The core waits for its return and
explicitly makes no claim about lifecycle hidden inside it.

## Callbacks, rollback and reentrancy

`nxandroid_context_step()` executes one declared step and
`nxandroid_context_run()` executes the remainder. An invoke callback is
attempted at most once. A rollback callback is attempted only for a step whose
profile record has a non-empty `rollback_contract_id`, and at most once in
reverse invocation order. No missing rollback record is replaced with guessed
engine teardown.

Rollbackable acquisitions also carry a numeric `rollback_group`. An explicitly
declared forward teardown step may close that group. Once the close callback
succeeds, earlier rollback callbacks in the group are consumed and will never
run again. Thus a later save/terminal failure cannot call resume, focus, input or
surface teardown twice. A failed close does not consume its group.

All same-context API calls, including getters and destroy, are prohibited while
an invoke or rollback callback is active. They return a documented sentinel,
mark a sticky violation and force the outer operation to fail before advancing
the step ledger. This prevents a callback from ignoring `EREENTRANT` and making
the outer run commit anyway. Calls involving another context remain independent.
Each context is driven by one host thread; the callback guard is not a claim of
general concurrent thread safety and deliberately does not hide adapter locking.

Callback code and `ops.userdata` are borrowed from the adapter. They must remain
alive and callable until context destruction completes, including all forward
invocations and rollback attempts. The context never assumes ownership of them.

On a callback failure, the context is fail-stopped and cannot retry later
steps. Every explicitly declared rollback is still attempted even if an earlier
rollback fails. `nxandroid_context_destroy()` is result-bearing so a reentrant
destroy cannot become a silent use-after-free.

## Explicit import policy

`nxandroid_import_catalog` owns a sorted snapshot of provider descriptors, not
the provider code or data. Every provider and request names all of these
fields:

- symbol name and Bionic architecture;
- Bionic, JNI or NDK domain;
- function or data kind;
- optional or critical classification;
- semantic `contract_id`;
- implementation or explicit stub.

An explicit stub also requires bounded human-readable semantics and the import
request must opt into stubs. There is no lookup callback, `dlsym`, host-name
fallback or generic zero stub. A missing strong import fails. A missing critical
weak import also fails. Only an import declared both non-critical and weak may
bind to zero.

Catalog creation uses a deterministic merge sort and resolution uses binary
search. Public counts and strings are bounded by the constants in
`include/nxandroid.h`; module duplicate validation is intentionally bounded to
at most 64 declarations. Binding output is transactional: an error leaves the
entire caller array untouched.

Binding `contract_id` and `stub_semantics` strings are catalog-owned. Provider
addresses are borrowed: their code or data must outlive every binding consumer
and the catalog. The required destruction order is consumers first, catalog
second, provider code/data last; destroying the catalog does not unload or free
a provider.

The catalog describes compatibility decisions; it does not implement Bionic
layouts, JNI tables or NDK objects. Those stay in ABI- and adapter-specific
providers.

## M16 approved-adapter ledger

`references/m16-adapter-contract-v1.json` is the read-only evidence ledger for
the five approved references: Bully 2, Sonic 4 Episode II, Horizon Chase,
KOTOR and `asm2_127`. It records file/line evidence, provenance, reusable
mechanisms, adapter-owned data, known limits and suggested tests for M16-001
through M16-020. It deliberately excludes WIP sources and never promotes
offsets, callbacks, JNI bindings, lifecycle, save or shutdown behavior into a
universal default.

The pure host gate is:

```sh
python3 -B framework/nxandroid/tests/test_m16_adapter_contract.py
```

It validates the exact adapter scope, evidence paths, line bounds, provenance,
specificity counters and fail-closed defaults. The state is
`closed_for_framework`: M16-013 and M16-014 combine the prior acceptance of the
five finalized ports with additional sanitized technical observations, while
M16-020 closes the contract gate.

`references/m16-runtime-receipt-v1.json` states the evidence boundary explicitly.
The approved ports already carry the human save/load and gameplay acceptance;
the agent does not claim to have replayed campaigns or reached every checkpoint.
The additional observations cover native returns, save boundaries and clean
shutdown where available. None of that promotes an offset, callback, JNI map,
lifecycle sequence, save schema or terminal action to a universal default. A new
adapter still needs its own acceptance before release.

## Build and deterministic host gate

```sh
cmake -S framework/nxandroid -B /tmp/nxandroid-build \
  -DNXANDROID_ENABLE_SANITIZERS=ON
cmake --build /tmp/nxandroid-build --parallel
ctest --test-dir /tmp/nxandroid-build --output-on-failure
```

`tests/test_nxandroid.c` covers normal execution, invalid order, every callback
failure point, sticky invoke/rollback reentrancy, explicit rollback, 1,000 fresh
contexts, surface and resume cycles, both approved GL/surface orderings, the
source-proven Sonic-style pre-Surface entry opt-in and its fail-closed cases,
blocking run-loop order, a KOTOR-like delegated owner, terminal opt-in, catalog
mismatch, critical weak rejection and transactional resolution. It uses only
host-owned mocks: no guest ELF, initializer,
`JNI_OnLoad`, device, network or signal is used.

## Isolated external-signal gate

`tests/test_signal.c` is deliberately excluded from CTest and must never be
invoked directly. Its only supported entry point is:

```sh
bash framework/nxandroid/tests/run-signal-isolated.sh
```

The runner reuses nxbootstrap's sealed user/PID/mount namespace guard and
watchdog and has no host fallback. The fixture opens a Linux pidfd for each
direct child before releasing it; if pidfd authority is unavailable it exits
77 inside the namespace without substituting a raw PID signal. `SIGTERM` is
sent only through the target child's pidfd. A separately identified sibling is
kept alive throughout both cases and receives no signal.

The active case blocks in the declared `RUN_LOOP`; its async-signal-safe handler
does only a `sig_atomic_t` update and a nonblocking self-pipe write. The same
forward lifecycle then performs input disable, focus loss, pause, save, surface
down, native shutdown and terminal exactly once. The early case receives the
signal before entry and proves an explicitly declared rollback without starting
guest code. Both GCC and Clang builds are compiled and run in the sealed suite;
the output includes source/binary hashes plus explicit zero guest initializer,
`JNI_OnLoad`, device, network and hardware claims.
