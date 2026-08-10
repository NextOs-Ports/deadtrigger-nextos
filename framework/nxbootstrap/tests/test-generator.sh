#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Generator contract: ONE small visible PortMaster launcher (Limbo shape)
# plus nxport.json. No run.sh layer, no bash runtime library, nothing mute.
set -euo pipefail

PROJECT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
# shellcheck source=private-pid-namespace.sh
source "$PROJECT_ROOT/tests/private-pid-namespace.sh"
nxbootstrap_require_private_pid_namespace || exit $?
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/nxbootstrap-generator.XXXXXX")
OUTPUT=$TEST_ROOT/output

cleanup() {
  case $TEST_ROOT in
    "${TMPDIR:-/tmp}"/nxbootstrap-generator.*) rm -rf -- "$TEST_ROOT" ;;
  esac
}
trap cleanup EXIT INT TERM

fail() {
  printf 'nxbootstrap generator test failed: %s\n' "$*" >&2
  exit 1
}

VERSION=$(cat "$PROJECT_ROOT/VERSION")

# ---------------------------------------------------------------- aarch64 example
python3 "$PROJECT_ROOT/tools/generate-port.py" \
  "$PROJECT_ROOT/examples/nxport.example.json" --output "$OUTPUT" >/dev/null

launcher="$OUTPUT/Meu Port.sh"
manifest="$OUTPUT/meu-port/nxport.json"

[[ -f $launcher && -x $launcher ]] || fail 'visible launcher missing or not executable'
[[ -f $manifest ]] || fail 'nxport.json missing'
if find "$OUTPUT" -name 'run.sh' | grep -q .; then
  fail 'generator emitted a forbidden run.sh layer'
fi
[[ ! -e $OUTPUT/meu-port/nxbootstrap.sh ]] ||
  fail 'generator still emits the retired bootstrap library'
[[ ! -e $OUTPUT/meu-port/nxdeployment.json ]] ||
  fail 'generator still emits the retired deployment receipt'

bash -n "$launcher" || fail 'visible launcher does not parse'
IFS= read -r shebang < "$launcher"
[[ $shebang == '#!/bin/bash' ]] || fail 'launcher shebang is not /bin/bash'

grep -Fq '# PORTMASTER: meu-port, Meu Port.sh' "$launcher" ||
  fail 'launcher lacks the PORTMASTER header'

# The launcher must stay small — that is the whole point (Limbo model).
# 360 covers the golden-port guarantees, fail-closed BYO/payload gates, a
# replacement-stable lock and bounded termination without a second bash library.
launcher_lines=$(wc -l < "$launcher")
(( launcher_lines <= 360 )) || fail "visible launcher grew to $launcher_lines lines"

# Canonical PortMaster lines, in dependency order.
for needle in \
  'source "$controlfolder/control.txt"' \
  'mod_${NXBOOTSTRAP_MOD_NAME}.txt' \
  'get_controls' \
  'GAMEDIR="/$directory/ports/meu-port"' \
  'export NXCOMPAT_PORT_ID=meu-port' \
  'export NXCOMPAT_GAME_DIR="$GAMEDIR"' \
  'export NXCOMPAT_REQUIRED_CAPABILITIES=' \
  'export NXCOMPAT_ENABLED_QUIRKS=' \
  'export NXCOMPAT_RUNTIME_REPORT=log-and-logo' \
  'exec > "$GAMEDIR/log.txt" 2>&1' \
  '[ -n "$sdl_controllerconfig" ] && export SDL_GAMECONTROLLERCONFIG' \
  'pm_platform_helper' \
  'flock -n 9' \
  'NXBOOTSTRAP_LOCK_FILE=' \
  'NXBOOTSTRAP_LOCK_PATH_ID' \
  'NXBOOTSTRAP_SHUTDOWN_TICKS=10' \
  'builtin kill -KILL "$game_pid"' \
  'nxbootstrap_finish' \
  'NXBOOTSTRAP_EXECUTABLE=$(readlink -f' \
  'NXBOOTSTRAP_REQUIRED_FILES=' \
  'required file is missing or unsafe' \
  "trap '' INT TERM HUP" \
  'printf '\''\033c'\''' \
  'pm_finish' \
  'meu-port-universal'
do
  grep -Fq "$needle" "$launcher" || fail "launcher lacks canonical line: $needle"
done
grep -Fq "nxbootstrap $VERSION" "$launcher" ||
  fail 'launcher does not record the generator version'
grep -Eq '@[A-Z0-9_]+@' "$launcher" && fail 'launcher has unresolved tokens' || true
grep -Fq '! -L "$GAMEDIR/port-env.sh"' "$launcher" ||
  fail 'launcher sources a symlinked port-env.sh'
grep -Fq 'PORT_32BIT' "$launcher" &&
  fail 'aarch64 launcher must not carry PORT_32BIT' || true

# argument mode game-dir-and-passthrough must reach the exec line.
grep -Fq '"$BIN" "$GAMEDIR" "$@"' "$launcher" ||
  fail 'launcher lost the game-dir-and-passthrough arguments'
# home_mode=preserve must not force HOME.
grep -Fq 'export HOME="$GAMEDIR"' "$launcher" &&
  fail 'home_mode=preserve still overrides HOME' || true

# ---------------------------------------------------------------- runtime contract
# The two quirk ids are approved registry entries evidenced by Bully and Sonic
# 4 Episode II. This fixture exercises their transport only; it does not touch
# either port or copy any game-specific implementation.
cat > "$TEST_ROOT/runtime-contract.json" <<'JSON'
{
  "schema_version": 2,
  "id": "runtime-port",
  "title": "Runtime Contract",
  "launcher_name": "Runtime Contract.sh",
  "architecture": "aarch64",
  "executable": "runtime-loader",
  "argument_mode": "none",
  "home_mode": "preserve",
  "nxextract": {"mode": "no", "version": "1.2.6"},
  "required_files": ["runtime-loader"],
  "private_library_paths": [],
  "prepare_script": "",
  "required_capabilities": [
    "host.portmaster",
    "graphics.gles2",
    "input.controller-mapping"
  ],
  "enabled_quirks": [
    "engine.utgard-finish-to-flush",
    "engine.stream-read-ahead"
  ],
  "runtime_report": "log"
}
JSON
runtime_output="$TEST_ROOT/runtime-output"
python3 "$PROJECT_ROOT/tools/generate-port.py" \
  "$TEST_ROOT/runtime-contract.json" --output "$runtime_output" >/dev/null
runtime_launcher="$runtime_output/Runtime Contract.sh"
runtime_loader="$runtime_output/runtime-port/runtime-loader"
cat > "$runtime_output/runtime-port/port-env.sh" <<'SH'
export NXCOMPAT_PORT_ID=port-env-overwrite
export NXCOMPAT_GAME_DIR=/port-env-overwrite
export NXCOMPAT_REQUIRED_CAPABILITIES=port-env-overwrite
export NXCOMPAT_ENABLED_QUIRKS=port-env-overwrite
export NXCOMPAT_RUNTIME_REPORT=port-env-overwrite
SH
runtime_xdg="$TEST_ROOT/runtime-xdg"
mkdir -p "$runtime_xdg/PortMaster"
cat > "$runtime_xdg/PortMaster/control.txt" <<'SH'
pm_platform_helper() {
  unset NXCOMPAT_PORT_ID NXCOMPAT_GAME_DIR
  unset NXCOMPAT_REQUIRED_CAPABILITIES NXCOMPAT_ENABLED_QUIRKS
  unset NXCOMPAT_RUNTIME_REPORT
}
SH
cat > "$runtime_loader" <<'SH'
#!/usr/bin/env bash
set -eu
result=${NXBOOTSTRAP_TEST_RUNTIME_CONTRACT:?}
printf '%s' "${NXCOMPAT_PORT_ID:?}" > "$result/port-id"
printf '%s' "${NXCOMPAT_GAME_DIR:?}" > "$result/game-dir"
printf '%s' "${NXCOMPAT_REQUIRED_CAPABILITIES:?}" > "$result/capabilities"
printf '%s' "${NXCOMPAT_ENABLED_QUIRKS:?}" > "$result/quirks"
printf '%s' "${NXCOMPAT_RUNTIME_REPORT:?}" > "$result/report"
SH
chmod 0755 "$runtime_loader"
runtime_result="$TEST_ROOT/runtime-result"
mkdir -p "$runtime_result"
NXBOOTSTRAP_TEST_RUNTIME_CONTRACT="$runtime_result" \
XDG_DATA_HOME="$runtime_xdg" \
  bash "$runtime_launcher" || fail 'runtime contract launcher failed'

runtime_game_dir=$(cd "$runtime_output/runtime-port" && pwd -P)
[[ $(<"$runtime_result/port-id") == runtime-port ]] ||
  fail 'child did not receive NXCOMPAT_PORT_ID'
[[ $(<"$runtime_result/game-dir") == "$runtime_game_dir" ]] ||
  fail 'child did not receive the physical NXCOMPAT_GAME_DIR'
printf '%s' $'host.portmaster\ngraphics.gles2\ninput.controller-mapping' \
  > "$TEST_ROOT/expected-capabilities"
cmp -s "$TEST_ROOT/expected-capabilities" "$runtime_result/capabilities" ||
  fail 'child did not receive required_capabilities exactly'
printf '%s' \
  $'engine.utgard-finish-to-flush\nengine.stream-read-ahead' \
  > "$TEST_ROOT/expected-quirks"
cmp -s "$TEST_ROOT/expected-quirks" "$runtime_result/quirks" ||
  fail 'child did not receive enabled_quirks exactly'
[[ $(<"$runtime_result/report") == log ]] ||
  fail 'child did not receive runtime_report'

# ---------------------------------------------------------------- BYO/payload guards
# A mode=yes port must have the complete NXExtract integration. Files created
# by that phase are accepted; any missing/unsafe required file blocks the
# loader. The odd filename proves the manifest list remains literal.
cat > "$TEST_ROOT/byo-guard.json" <<'JSON'
{
  "schema_version": 2,
  "id": "byo-guard",
  "title": "BYO Guard",
  "launcher_name": "BYO Guard.sh",
  "architecture": "aarch64",
  "executable": "byo-loader",
  "argument_mode": "none",
  "home_mode": "preserve",
  "nxextract": {"mode": "yes", "version": "1.2.6"},
  "required_files": [
    "byo-loader",
    "data/main.obb",
    "lib/libGame.so",
    "lib/libfox.so",
    "data/owner $literal * 'quote'.bin"
  ],
  "private_library_paths": [],
  "prepare_script": "",
  "required_capabilities": [],
  "enabled_quirks": [],
  "runtime_report": "log"
}
JSON
byo_output="$TEST_ROOT/byo-output"
python3 "$PROJECT_ROOT/tools/generate-port.py" \
  "$TEST_ROOT/byo-guard.json" --output "$byo_output" >/dev/null
byo_launcher="$byo_output/BYO Guard.sh"
byo_game="$byo_output/byo-guard"
byo_result="$TEST_ROOT/byo-result"
byo_xdg="$TEST_ROOT/byo-xdg"
mkdir -p "$byo_game/nxextract" "$byo_result" "$byo_xdg"
cat > "$byo_game/byo-loader" <<'SH'
#!/usr/bin/env bash
set -eu
printf 'launch\n' >> "${NXBOOTSTRAP_TEST_BYO_RESULT:?}/launches"
SH
cat > "$byo_game/extractor.json" <<'JSON'
{"schema": 1}
JSON
cat > "$byo_game/nxextract/nxextract.py" <<'PY'
# Non-empty core fixture; the runner below isolates launcher behavior.
PY
cat > "$byo_game/nxextract/nxextract-runtime-env.sh" <<'SH'
#!/usr/bin/env bash
exec "$@"
SH
cat > "$byo_game/nxextract/run-extractor.sh" <<'SH'
#!/usr/bin/env bash
set -eu
printf 'extract\n' >> "${NXBOOTSTRAP_TEST_BYO_RESULT:?}/extractions"
if [ "${NXBOOTSTRAP_TEST_SKIP_INSTALL:-0}" != 1 ]; then
  mkdir -p "$NXEXTRACT_GAME_DIR/data" "$NXEXTRACT_GAME_DIR/lib"
  printf 'obb\n' > "$NXEXTRACT_GAME_DIR/data/main.obb"
  printf 'game\n' > "$NXEXTRACT_GAME_DIR/lib/libGame.so"
  printf 'fox\n' > "$NXEXTRACT_GAME_DIR/lib/libfox.so"
  printf 'literal\n' > "$NXEXTRACT_GAME_DIR/data/owner \$literal * 'quote'.bin"
fi
SH
chmod 0755 "$byo_game/byo-loader" \
  "$byo_game/nxextract/run-extractor.sh" \
  "$byo_game/nxextract/nxextract-runtime-env.sh"
bash -n "$byo_launcher" || fail 'mode=yes BYO launcher does not parse'
NXBOOTSTRAP_TEST_BYO_RESULT="$byo_result" XDG_DATA_HOME="$byo_xdg" \
  bash "$byo_launcher" || fail 'complete mode=yes BYO launch failed'
[[ $(wc -l < "$byo_result/extractions") -eq 1 &&
   $(wc -l < "$byo_result/launches") -eq 1 ]] ||
  fail 'complete mode=yes path did not extract once then launch once'

mv "$byo_game/data/main.obb" "$TEST_ROOT/main.obb.saved"
if NXBOOTSTRAP_TEST_BYO_RESULT="$byo_result" \
   NXBOOTSTRAP_TEST_SKIP_INSTALL=1 XDG_DATA_HOME="$byo_xdg" \
   bash "$byo_launcher"; then
  fail 'mode=yes launched with a missing required OBB'
fi
[[ $(wc -l < "$byo_result/launches") -eq 1 ]] ||
  fail 'loader ran after the required-file gate failed'
mv "$TEST_ROOT/main.obb.saved" "$byo_game/data/main.obb"

mv "$byo_game/extractor.json" "$TEST_ROOT/extractor.json.saved"
if NXBOOTSTRAP_TEST_BYO_RESULT="$byo_result" XDG_DATA_HOME="$byo_xdg" \
   bash "$byo_launcher"; then
  fail 'mode=yes skipped a missing extractor recipe'
fi
mv "$TEST_ROOT/extractor.json.saved" "$byo_game/extractor.json"
mv "$byo_game/nxextract/run-extractor.sh" "$TEST_ROOT/runner.saved"
if NXBOOTSTRAP_TEST_BYO_RESULT="$byo_result" XDG_DATA_HOME="$byo_xdg" \
   bash "$byo_launcher"; then
  fail 'mode=yes skipped a missing extractor runner'
fi
mv "$TEST_ROOT/runner.saved" "$byo_game/nxextract/run-extractor.sh"
[[ $(wc -l < "$byo_result/launches") -eq 1 ]] ||
  fail 'loader ran with an incomplete mode=yes integration'

mv "$byo_game/lib/libfox.so" "$TEST_ROOT/libfox.saved"
ln -s "$TEST_ROOT/libfox.saved" "$byo_game/lib/libfox.so"
if NXBOOTSTRAP_TEST_BYO_RESULT="$byo_result" \
   NXBOOTSTRAP_TEST_SKIP_INSTALL=1 XDG_DATA_HOME="$byo_xdg" \
   bash "$byo_launcher"; then
  fail 'required-file gate accepted a final symlink'
fi
mv "$byo_game/lib/libfox.so" "$TEST_ROOT/rejected-libfox-link"
mv "$TEST_ROOT/libfox.saved" "$byo_game/lib/libfox.so"

mv "$byo_game/data" "$TEST_ROOT/data-outside-port"
ln -s "$TEST_ROOT/data-outside-port" "$byo_game/data"
if NXBOOTSTRAP_TEST_BYO_RESULT="$byo_result" \
   NXBOOTSTRAP_TEST_SKIP_INSTALL=1 XDG_DATA_HOME="$byo_xdg" \
   bash "$byo_launcher"; then
  fail 'required-file gate accepted an escaping parent symlink'
fi
mv "$byo_game/data" "$TEST_ROOT/rejected-data-link"
mv "$TEST_ROOT/data-outside-port" "$byo_game/data"
[[ $(wc -l < "$byo_result/launches") -eq 1 ]] ||
  fail 'loader ran after an unsafe required path'

# auto is optional only when recipe and both supported runners are absent.
cat > "$TEST_ROOT/auto-guard.json" <<'JSON'
{
  "schema_version": 2,
  "id": "auto-guard",
  "title": "Auto Guard",
  "launcher_name": "Auto Guard.sh",
  "architecture": "aarch64",
  "executable": "auto-loader",
  "argument_mode": "none",
  "home_mode": "preserve",
  "nxextract": {"mode": "auto", "version": "1.2.6"},
  "required_files": ["auto-loader"],
  "private_library_paths": [],
  "prepare_script": "",
  "required_capabilities": [],
  "enabled_quirks": [],
  "runtime_report": "log"
}
JSON
auto_output="$TEST_ROOT/auto-output"
python3 "$PROJECT_ROOT/tools/generate-port.py" \
  "$TEST_ROOT/auto-guard.json" --output "$auto_output" >/dev/null
auto_launcher="$auto_output/Auto Guard.sh"
auto_game="$auto_output/auto-guard"
cat > "$auto_game/auto-loader" <<'SH'
#!/usr/bin/env bash
printf 'launch\n' >> "${NXBOOTSTRAP_TEST_BYO_RESULT:?}/auto-launches"
SH
chmod 0755 "$auto_game/auto-loader"
NXBOOTSTRAP_TEST_BYO_RESULT="$byo_result" XDG_DATA_HOME="$byo_xdg" \
  bash "$auto_launcher" || fail 'empty mode=auto did not skip NXExtract'
printf '{}\n' > "$auto_game/extractor.json"
if NXBOOTSTRAP_TEST_BYO_RESULT="$byo_result" XDG_DATA_HOME="$byo_xdg" \
   bash "$auto_launcher"; then
  fail 'partial mode=auto integration did not fail closed'
fi
mv "$auto_game/extractor.json" "$TEST_ROOT/auto-recipe.saved"
mkdir -p "$auto_game/nxextract"
printf '# partial core\n' > "$auto_game/nxextract/nxextract.py"
if NXBOOTSTRAP_TEST_BYO_RESULT="$byo_result" XDG_DATA_HOME="$byo_xdg" \
   bash "$auto_launcher"; then
  fail 'mode=auto ignored an isolated NXExtract core'
fi
[[ $(wc -l < "$byo_result/auto-launches") -eq 1 ]] ||
  fail 'partial mode=auto integration reached the loader'

# ---------------------------------------------------------------- armv7 example
python3 "$PROJECT_ROOT/tools/generate-port.py" \
  "$PROJECT_ROOT/examples/nxport-armv7.example.json" \
  --output "$OUTPUT" --force >/dev/null
armv7_launcher="$OUTPUT/Meu Port ARMHF.sh"
[[ -f $armv7_launcher ]] || fail 'armv7 launcher was not generated'
grep -Fq 'PORT_32BIT="Y"' "$armv7_launcher" ||
  fail 'armv7 launcher lacks the literal PORT_32BIT marker'
grep -Fq 'arm-linux-gnueabihf' "$armv7_launcher" ||
  fail 'armv7 launcher lacks the 32-bit library path block'
grep -Fq 'PIPEWIRE_MODULE_DIR' "$armv7_launcher" ||
  fail 'armv7 launcher lacks the 32-bit audio module block'
bash -n "$armv7_launcher" || fail 'armv7 launcher does not parse'

# ---------------------------------------------------------------- determinism
python3 "$PROJECT_ROOT/tools/generate-port.py" \
  "$PROJECT_ROOT/examples/nxport.example.json" --output "$TEST_ROOT/det-a" >/dev/null
python3 "$PROJECT_ROOT/tools/generate-port.py" \
  "$PROJECT_ROOT/examples/nxport.example.json" --output "$TEST_ROOT/det-b" >/dev/null
diff -r "$TEST_ROOT/det-a" "$TEST_ROOT/det-b" >/dev/null ||
  fail 'two clean generations from the same manifest differ'

# ---------------------------------------------------------------- overwrite guard
if python3 "$PROJECT_ROOT/tools/generate-port.py" \
  "$PROJECT_ROOT/examples/nxport.example.json" --output "$OUTPUT" \
  >/dev/null 2>&1; then
  fail 'generator overwrote existing output without --force'
fi
python3 "$PROJECT_ROOT/tools/generate-port.py" \
  "$PROJECT_ROOT/examples/nxport.example.json" --output "$OUTPUT" --force \
  >/dev/null || fail '--force regeneration failed'

# ---------------------------------------------------------------- validation
reject() {
  local description=$1 payload=$2
  printf '%s' "$payload" > "$TEST_ROOT/bad.json"
  if python3 "$PROJECT_ROOT/tools/generate-port.py" "$TEST_ROOT/bad.json" \
    --output "$TEST_ROOT/bad-out" --force >/dev/null 2>&1; then
    fail "generator accepted $description"
  fi
}

reject 'an absolute executable path' '{
  "schema_version": 2, "id": "x", "title": "X", "launcher_name": "X.sh",
  "architecture": "aarch64", "executable": "/bin/sh",
  "argument_mode": "none", "home_mode": "port",
  "nxextract": {"mode": "no", "version": "1.2.6"},
  "required_files": [], "private_library_paths": [], "prepare_script": "",
  "required_capabilities": [], "enabled_quirks": [],
  "runtime_report": "log"}'
reject 'an unknown architecture' '{
  "schema_version": 2, "id": "x", "title": "X", "launcher_name": "X.sh",
  "architecture": "riscv", "executable": "x",
  "argument_mode": "none", "home_mode": "port",
  "nxextract": {"mode": "no", "version": "1.2.6"},
  "required_files": [], "private_library_paths": [], "prepare_script": "",
  "required_capabilities": [], "enabled_quirks": [],
  "runtime_report": "log"}'
reject 'a personal path in the title' '{
  "schema_version": 2, "id": "x", "title": "/home/felipe/X",
  "launcher_name": "X.sh",
  "architecture": "aarch64", "executable": "x",
  "argument_mode": "none", "home_mode": "port",
  "nxextract": {"mode": "no", "version": "1.2.6"},
  "required_files": [], "private_library_paths": [], "prepare_script": "",
  "required_capabilities": [], "enabled_quirks": [],
  "runtime_report": "log"}'
reject 'a device-named capability' '{
  "schema_version": 2, "id": "x", "title": "X", "launcher_name": "X.sh",
  "architecture": "aarch64", "executable": "x",
  "argument_mode": "none", "home_mode": "port",
  "nxextract": {"mode": "no", "version": "1.2.6"},
  "required_files": [], "private_library_paths": [], "prepare_script": "",
  "required_capabilities": ["host.device.rg35xx"], "enabled_quirks": [],
  "runtime_report": "log"}'

# Legacy v1 manifests still upgrade.
printf '%s' '{
  "schema_version": 1, "id": "legado", "title": "Legado",
  "launcher_name": "Legado.sh", "architecture": "aarch64",
  "executable": "legado-bin", "argument_mode": "none",
  "home_mode": "port", "nxextract": "no",
  "required_files": ["legado-bin"]}' > "$TEST_ROOT/legacy.json"
python3 "$PROJECT_ROOT/tools/generate-port.py" "$TEST_ROOT/legacy.json" \
  --output "$TEST_ROOT/legacy-out" >/dev/null ||
  fail 'legacy schema v1 manifest no longer upgrades'
[[ -f "$TEST_ROOT/legacy-out/Legado.sh" ]] ||
  fail 'legacy upgrade did not produce the launcher'
grep -Fq 'NXExtract: disabled' "$TEST_ROOT/legacy-out/Legado.sh" ||
  fail 'nxextract.mode=no did not disable the extractor block'

printf 'nxbootstrap generator test passed\n'
