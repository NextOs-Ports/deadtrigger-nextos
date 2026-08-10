# Changelog

## 1.2.6

- Case-insensitive ZIP path collisions are enforced over the members a recipe
  actually selects, not over the whole archive. Real Android APKs routinely
  carry obfuscated resources that differ only in case (`res/9N.9.png` vs
  `res/9n.9.png`); refusing the archive rejected legitimate builds whose
  colliding members are never extracted. The extracted tree is still
  guaranteed writable on exFAT/FAT cards, because the check now covers exactly
  what gets written.

All notable NXExtract changes are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## 1.2.5

- Introduce transaction journal format 2. Every backup/install rename now has
  a durable write-ahead intent, the rename fsyncs both parent directories, and
  the confirmed state is fsynced afterward. Fourteen simulated power-loss
  boundaries prove that recovery either restores the complete previous payload
  or retains the complete newly published payload.
- Make the marker a strict publication record: exact engine/recipe/ABI/commit
  identity, plan fingerprint, transaction ID, item schema and a metadata seal
  of every committed object. Recovery performs full payload validation before
  trusting even a matching marker and rolls back corrupted publication.
- Reject recipe parents, workspace objects, logs, locks, journals, stages,
  caches and hook checkpoints that are symlinked or hard-linked. Recipe
  templates and reserved path overlaps now fail before log/workspace effects.
- Reject ZIP traversal, special/encrypted members, file/directory conflicts and
  Unicode/case-fold path collisions globally, including members a recipe would
  not otherwise select. Published trees also reject symlinks, hardlinks and
  non-portable collisions.
- Seal cache-space accounting to valid cached inner APKs only; stale or extra
  cache files cannot hide the actual missing-space preflight.
- Add exact whole-bundle pinning for new/touched ports. Engine, runner, runtime
  helper and optional UI are compared by SHA-256; 1.2.2 remains only the
  documented historical audit floor.
- Audit every ELF found in the release tree. The AArch64 UI must be PIE with
  RELRO/NOW, non-executable stack, no RPATH/RUNPATH, exact AArch64 interpreter,
  only `libc`/`libdl` dependencies and GLIBC no newer than 2.30. The current
  unchanged UI requires GLIBC 2.17.
- `run-extractor.sh` now invokes the runtime helper explicitly through `bash`,
  so a PortMaster ZIP installed on FAT/exFAT remains usable when executable
  mode bits are lost. The helper still owns the process-scoped library and SDL
  isolation before the launcher re-enters itself.
- Once the validated installation marker is published, cleanup of the
  transaction backup, stage, source cache or journal can no longer turn the
  install into a failure. Refused cleanup keeps the journal as a retry signal;
  the next run recognizes the marker's transaction and safely finishes it.
- Expand the release suite to 55 synthetic Python tests plus isolated runtime,
  full-bundle pin and hostile-ELF gate regressions. No APK, OBB, ZIP or
  proprietary game data is included in the source release.

## 1.2.4

- New `input.packages`: a recipe can declare which Android package it accepts.
  Two games from the same studio share the engine, the asset names and the
  layout, so content rules alone let the wrong one install silently; the refusal
  now names both the package that arrived and the one the port expects.
- New source kind `container`: copies the selected APK itself as a single file.
  Ports whose engine reads its resources from the package at run time (Cocos2d-x
  with minizip, for example) could only be served by extracting the entire asset
  tree and repacking it, which needs twice the card space during install and
  leaves both copies committed. `container` picks the base APK of the set by
  default, or a named `split`, and never considers the bundle that wraps it.

## 1.2.3

- Zip-format OBB files (Aspyr, Gameloft, Rockstar and others publish OBBs that
  are plain ZIP archives) are now also kept as loose-file candidates during
  discovery. Before this, a loose `.obb` that happened to be a ZIP was
  classified only as a companion archive, so `file`/`entry_or_file` rules that
  select the OBB itself could never match it — the installer searched inside
  the OBB instead of taking it as the payload. `entry` rules that look inside
  such OBBs keep working unchanged. First hit: KOTOR (build 53), whose two
  OBBs are ZIPs and are consumed whole by the game's own ObbFile layer.

## 1.2.2

- Discard staged data that fails whole-set validation instead of keeping it for
  the next resume. Each extracted item is accepted on its own, so a stage that
  only fails as a *set* used to be resumed and re-rejected forever: the field
  log showed `resuming 1.2 GiB of already validated staged data` followed by the
  same validation error on every retry, with no way out short of deleting the
  workspace by hand. The stage and `state.json` are now removed when whole-set
  validation raises, so the next run extracts from scratch. Found in the field
  on Hitman GO 1.1.1 and carried as a local patch there; this promotes it
  upstream so ports vendor a clean tree.

## 1.2.1

- Never fail an install because the scratch source cache could not be deleted.
  The cache is now dropped after the payload is committed and after every
  source archive is closed, and a removal that still fails is logged and left
  for the next run instead of aborting. FUSE-backed shares (exFAT on Knulli and
  Batocera, NFS, SMB) keep a hidden placeholder for files unlinked while open,
  so `source-cache/bundle-*` answered `[Errno 39] Directory not empty` and a
  fully installed game was reported as a failed data setup.
- Add a regression test covering an install whose source-cache removal fails.

## 1.2.0

- Reconcile the embedded copy with the expected 1.1.2 matched-but-rejected
  candidate diagnostic and add its synthetic regression test.
- Add a generic process-scoped runtime helper that resolves native UI
  dependencies from firmware paths before inherited compatibility paths,
  removes library directories inside the game tree and preserves SDL backend
  inheritance or autodetection.
- Test the runtime boundary directly and through `run-extractor.sh` in the
  release gate.
- Add an explicit `NXEXTRACT_SDL_AUTODETECT=1` child-only recovery path for
  proven-invalid inherited SDL video/audio overrides. The default remains
  unchanged and no backend is selected by the helper.

## 1.1.2

- Make the per-launch marker check skip the full tree walk: committed trees
  are re-checked only through their anchor `required_paths`, while install,
  update, verification and adoption retain full validation.
- Add `install --force-source` for transactional upgrades from a newer source
  package while preserving the current payload until validation/publication.
- Report matched-but-rejected candidates in required-payload plan errors.
  When files match a payload's source pattern but every one fails validation
  (size, sha256, crc32 or ELF machine), the error now says so and names one of
  the rejected candidates instead of claiming the payload was not found.
- Add a synthetic regression test for the rejected-candidate diagnostic.

## 1.1.1

- Log the exact full-validation rejection for every attempted ABI when existing
  game data cannot be adopted. Validation remains strict; the additional
  diagnostic identifies the incomplete or mismatched path without requiring a
  source-package scan to fail first.
- Add a synthetic regression test for the existing-data rejection diagnostic.

## 1.1.0

- Licensed the standalone project under MIT.
- Made the UI compatibility build the default release path.
- Added an AArch64 release gate that rejects GLIBC requirements above 2.30.
- Added `elf_machine: "{abi}"` for ABI-neutral recipes and an ARMv7 fallback
  regression test.
- Added a public `nxextract --version` command and engine version in new
  installation markers.
- Added complete English documentation and standalone architecture, recipe,
  contribution, security and device-compatibility guides.
- Added sanitized real-device screenshots using only the synthetic fixture.
- Added public CI, issue forms, pull-request guidance, funding/community links
  and standalone release notes.
- Validated the Python 3.7 core, GLIBC 2.17 UI and KMSDRM flow on ArkOS.

## 1.0.0

- Initial content-driven APK/APKM/APKS/XAPK extractor.
- Loose split grouping by Android package and automatic ABI selection.
- Resumable staging, bake hooks, full validation and journaled publication.
- Crash recovery, rollback, fast markers and legal-source preservation.
- Dynamic SDL2 first-run UI for fbdev/Mali, KMSDRM and Wayland-class systems.
